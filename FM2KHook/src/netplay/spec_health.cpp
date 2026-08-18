// Spectator per-tick drivers: viewer-side TickHealth (heartbeat/failover/
// reconnect + relay-inbound drain) and host-side TickHostMaintenance (NAT punch,
// TCP bind, subscriber sweep). Extracted VERBATIM from spectator_node.cpp.
// Public API (decls in spectator_node.h); calls specnode helpers via using.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
#include "spec_pool_sync.h"       // Phase 4c match-start pool resync (hold supervision)
#include "control_channel.h"
#include "netplay.h"
#include "replay.h"
#include "savestate.h"            // SaveState_Save / Peek for snapshot capture
#include "netplay_state.h"
#include "../audio/sound_rollback.h"  // Op apply: SOUND_INIT
#include "../hooks/css_autoconfirm.h" // Replay-mode CSS lock-and-confirm
#include "../hooks/per_game_patches.h" // PerGamePatches_SetRuntimeBtbOverrides
#include "../ui/shared_mem.h"         // C10: SharedMem_PublishMatchSession / RoundResult
#include "gekkonet.h"
#include <SDL3/SDL_timer.h>       // SDL_GetPerformanceCounter/Frequency (hi-res backfill timing)

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <array>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
using namespace specnode;

void SpectatorNode_TickHealth() {
    const uint64_t now = (uint64_t)GetTickCount64();

    // ---- Spec hub-relay inbound drain (Phase 3) -------------------------
    // When the launcher has WS binary frames forwarded from the hub, it
    // writes them into the inbound shared-mem ring. Each Slot's payload
    // is a SpecDataHeader-prefixed wire frame -- byte-identical to what the
    // direct RC path delivers. Feed straight into HandleSpecData.
    //
    // Bound work-per-tick so a snapshot burst doesn't monopolize the
    // hook tick. ~32 slots = up to 512 KB per tick which covers most
    // snapshots in 2 ticks. Drain continues next tick if there's more.
    if (g_state.spec_relay_in) {
        constexpr int kMaxPerTick = 32;
        for (int i = 0; i < kMaxPerTick; ++i) {
            const fm2k::spec_relay::Slot* slot =
                fm2k::spec_relay::PeekFront(g_state.spec_relay_in);
            if (!slot) break;
            // The sockaddr_in second arg is a debug breadcrumb; zero works.
            // rc_channel defaults to 0 = single ordered stream, which is
            // exactly what the relay is.
            sockaddr_in zero_from{};
            zero_from.sin_family = AF_INET;
            SpectatorNode_HandleSpecData(
                slot->payload, slot->payload_len, zero_from);
            fm2k::spec_relay::PopFront(g_state.spec_relay_in);
        }
    }

    // ---- Subscriber-side: heartbeat + silence failover ------------------
    if (g_state.subscribed_upstream) {
        if (now - g_state.last_heartbeat_send_ms >= SPECTATOR_HEARTBEAT_INTERVAL_MS) {
            CtrlPacket hb = {};
            hb.header.type = CtrlMsg::SPEC_HEARTBEAT;
            ControlChannel_SendTo(hb, g_state.upstream_addr);
            g_state.last_heartbeat_send_ms = now;
        }

        // Surgical gap-fill pull. The live stream is flowing but our
        // admission cursor (next_expected_frame) has stalled behind
        // buffered future batches (pb_reorder) whose lowest start_frame is
        // AHEAD of the cursor -- a gap the drain loop can never bridge on
        // its own. This is the mid-match snapshot-join gap: snapshot+
        // backfill ride TCP, live rides RC, and under loss the RC endpoint
        // comes up some frames after the bind, so [backfill-end..RC-live-
        // start) arrives over neither channel. Pull exactly that range over
        // the reliable conn via SPEC_JOIN_RESUME. This fires in <1s, well
        // before the 15s silence-failover that used to tear the whole
        // connection down and loop on a full re-snapshot under sustained
        // loss. Idempotent: the host re-ships [cursor..live) and the
        // viewer's positional dedup drops the RC overlap.
        // Two failure shapes, both healed by the same reliable pull:
        //   VARIANT 1 -- buffered future batches sit AHEAD of a stalled
        //     cursor (the backfill-end..RC-live-start gap: some live
        //     batches did arrive, keyed past next_expected, but the bridge
        //     never did). pb_reorder is non-empty.
        //   VARIANT 2 -- the live stream delivered NOTHING at all (pb_reorder
        //     EMPTY, sim starved, cursor frozen). Nothing is buffered to
        //     signal the gap, so variant-1 detection is blind.
        // Firing when actually caught-up is a host-side no-op
        // (SendSessionBackfillFromFrame ships nothing at-or-after the live
        // edge), so an over-eager pull costs one ACK + empty backfill.
        const bool gap_ahead = !g_state.pb_reorder.empty() &&
            g_state.pb_reorder.begin()->first > g_state.next_expected_frame;
        bool starved_battle = false;
        bool starved_any    = false;   // same predicate, WITHOUT the battle gate
        // "We are mid-stream", NOT "we are inside the one battle a snapshot
        // landed in". This term used to read pb_snapshot_applied_once ALONE,
        // which made variant 2 unreachable for most spectators and for ALL of
        // them after their first match boundary: that flag is cleared at every
        // battle EXIT (SpectatorNode_ClearSnapshotAppliedForNextBattle, from
        // the battle->non-battle edge in trampoline_spectator.cpp) because it
        // also suppresses the battle-entry DoInitialSync. A from-frame-0 /
        // CSS-joining viewer never satisfied it at all. Measured: across all
        // seven runs of the 2026-08-07 starve corpus the mid-battle viewers
        // issued ZERO gap-fill pulls -- one sat at q=0 for five seconds and
        // then killed itself -- while viewers still inside their snapshot's
        // battle issued seven or eight. pb_started ("we have popped a real
        // INPUT this session") is durable and covers every join shape; the
        // snapshot flag stays as an OR for a viewer that starves before its
        // first pop.
        const bool mid_stream =
            g_state.pb_started || g_state.pb_snapshot_applied_once;
        if (!gap_ahead && g_state.playing_back && mid_stream &&
            SpectatorNode_PendingFrameCount() == 0) {
            // Gate on battle mode: a starved queue at a match boundary /
            // CSS is NORMAL and must not pull (the pull's JOIN_ACK carries
            // the host's CURRENT kind -- a between-matches kind would abort
            // the viewer's BTB). Battle is the RANGE [3000,4000), which is
            // what every other battle test in this codebase uses
            // (DeepJoinShouldHold, the SpecSim trace gate); the old `== 3000`
            // equality would have missed any battle sub-mode.
            const uint32_t gm = *(const uint32_t*)FM2K::ADDR_GAME_MODE;
            starved_battle = (gm >= 3000u && gm < 4000u);
            starved_any    = true;
        }
        // Measurement only (see spectator_node_internal.h): the ladder above
        // does not exist outside battle, so record how long that costs us.
        SpecCssWindowStarveTick(now, starved_any, starved_battle);
        if (g_state.have_frame_baseline && (gap_ahead || starved_battle)) {
            if (g_state.next_expected_frame != g_state.gap_fill_stall_frame) {
                // Cursor moved (or first observation) -- (re)start the timer,
                // and the pull budget with it: a pull that MOVED the cursor is
                // a pull that worked, and only a run of pulls that changed
                // nothing may escalate.
                g_state.gap_fill_stall_frame       = g_state.next_expected_frame;
                g_state.gap_fill_stall_since_ms    = now;
                g_state.gap_fill_pulls_no_progress = 0;
            } else if (now - g_state.gap_fill_stall_since_ms >=
                           SPECTATOR_GAP_FILL_STALL_MS &&
                       now - g_state.last_gap_fill_send_ms >=
                           SPECTATOR_GAP_FILL_THROTTLE_MS) {
                g_state.last_gap_fill_send_ms = now;
                // Pull, or -- once pulling has provably stopped helping --
                // escalate to the resume-suppressed re-JOIN + RC endpoint
                // reset. Owns its own logging, so the line always names what
                // actually happened. See spec_join_viewer.cpp.
                SpecGapFillPullOrEscalate(now, gap_ahead);
            }
        } else {
            g_state.gap_fill_stall_frame       = 0xFFFFFFFFu;  // no gap / not battle
            g_state.gap_fill_pulls_no_progress = 0;
        }

        // Subscribed, live batches ARE arriving, and NOTHING has anchored the
        // frame baseline. spec_recv's can_base gate refuses a live
        // RC_CHAN_SPEC batch as an anchor on purpose (anchoring the cursor
        // without the snapshot's state would turn a visible freeze into a
        // silent desync), so only the bulk backfill or an applied snapshot can
        // set it -- and if both were lost the viewer buffers live data into
        // pb_reorder forever. That state used to emit ZERO log output while
        // [SPEC-Q] showed q=0 total=0: the exact silent shape of the reported
        // freeze. Make it loud at 1Hz so a log says which half of the
        // handshake went missing. Diagnostic only -- the no-admit re-JOIN
        // below owns the recovery. The 1500ms persistence grace matters: a
        // HEALTHY join legitimately buffers a few live batches in the window
        // between the RC endpoint coming up and the bulk backfill landing, and
        // accusing it of a lost backfill would make the warning worthless.
        {
            static uint64_t s_no_base_since_ms = 0;
            static uint64_t s_no_base_log_ms   = 0;
            const bool no_base = !g_state.have_frame_baseline &&
                                 !g_state.pb_reorder.empty();
            if (!no_base) {
                s_no_base_since_ms = 0;
            } else {
                if (s_no_base_since_ms == 0) s_no_base_since_ms = now;
                if (now - s_no_base_since_ms >= 1500 &&
                    now - s_no_base_log_ms   >= 1000) {
                    s_no_base_log_ms = now;
                    const auto& inbox = g_state.pb_snapshot_inbox;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: NO frame baseline for %llums -- %zu "
                        "live batch(es) buffered from "
                        "INPUT-frame=%u and nothing to anchor them (bulk "
                        "backfill lost; snapshot inbox active=%d %zu/%u B, "
                        "pending_apply=%d). Waiting on the no-admit re-JOIN "
                        "to force a host re-ship",
                        (unsigned long long)(now - s_no_base_since_ms),
                        g_state.pb_reorder.size(),
                        g_state.pb_reorder.begin()->first,
                        (int)inbox.active, inbox.bytes_received,
                        (unsigned)inbox.meta.total_bytes,
                        (int)inbox.pending_apply);
                }
            }
        }

        // GRANT-vs-STREAM consistency backstop. We took the /F battle-boot
        // path because a JOIN_ACK granted BATTLE, which means and only means
        // "a snapshot is coming". If the stream then turns up (baseline set)
        // and no snapshot ever does -- inbox never touched, none applied --
        // the ACK we obeyed did not describe the stream the host assigned us.
        // That is the CSS-grant-vs-BATTLE-ACK mismatch: the viewer
        // replays a from-frame-0 CSS stream as battle input from uninitialised
        // state, which is a deterministic desync rather than a visible fault.
        // Refuse to keep simulating on a contradiction: log loudly and re-JOIN
        // for a grant that matches.
        //
        // CONTAINMENT, NOT REPAIR. Once PopFrameInputs has consumed a real
        // INPUT, pb_started latches and ApplyPendingSnapshot discards any
        // snapshot the re-JOIN wins, so the sim cannot be corrected in place.
        // The host-side pinned grant is what actually prevents the bad boot;
        // this exists so a future variant of the class is loud and recoverable
        // instead of silent.
        if (g_state.spec_boot_expects_snapshot) {
            const auto& inbox = g_state.pb_snapshot_inbox;
            const bool snapshot_seen = g_state.pb_snapshot_applied_once ||
                                       inbox.active || inbox.pending_apply ||
                                       inbox.bytes_received > 0;
            static uint64_t s_grant_since_ms = 0;
            if (snapshot_seen) {
                g_state.spec_boot_expects_snapshot = false;  // promise kept
                s_grant_since_ms = 0;
            } else if (!g_state.have_frame_baseline) {
                s_grant_since_ms = 0;   // stream not flowing yet -- not a verdict
            } else {
                if (s_grant_since_ms == 0) s_grant_since_ms = now;
                // Grace: the bind ships the snapshot BEFORE the backfill, but
                // the blob rides an UNORDERED channel, so under loss a chunk
                // can legitimately trail the baseline by a little.
                if (now - s_grant_since_ms >= 2000) {
                    char gbuf[48] = {}; FormatAddr(g_state.root_addr, gbuf, sizeof(gbuf));
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: JOIN_ACK/stream MISMATCH -- granted "
                        "BATTLE (we /F-booted for a snapshot join) but %llums "
                        "after the stream anchored at INPUT-frame=%u NO "
                        "snapshot has arrived. The host pinned us a "
                        "from-frame-0 mirror; continuing would replay CSS "
                        "input as battle input. Re-JOIN %s for a matching "
                        "grant (pb_started=%d -- if 1 the local sim is already "
                        "past repair and the viewer should be restarted)",
                        (unsigned long long)(now - s_grant_since_ms),
                        g_state.next_expected_frame, gbuf,
                        (int)g_state.pb_started);
                    g_state.spec_boot_expects_snapshot = false;  // one shot
                    s_grant_since_ms = 0;
                    if (g_state.root_addr.sin_port != 0 && !g_state.session_ended) {
                        g_state.last_reconnect_attempt_ms = now;
                        SpectatorNode_RequestJoin(g_state.root_addr);
                    }
                }
            }
        }

        // Bounded deep-join hold supervision (Wave 4). No-op unless this viewer
        // is actually holding at battle entry for a snapshot. Progress-gated
        // re-request on SPECTATOR_DEEPJOIN_REQ_INTERVAL_MS, then a loud
        // escalation to a resume-suppressed re-JOIN at the budget -- never a
        // silent release into the from-scratch battle path.
        DeepJoinHoldTick(now);

        // Match-start pool resync hold supervision (Phase 4c). No-op unless
        // this viewer is holding at a match boundary for the host's
        // battle-entry snapshot. One nudge per interval, no escalation: the
        // hold is bounded and releases into ordinary playback on its own.
        PoolSync_HoldTick(now);
    }

    // Reconnect path: not subscribed, but we have a root we can fall
    // back to. Throttle with EXPONENTIAL BACKOFF so a genuinely-gone host
    // isn't stormed with a fixed-rate JOIN_REQ every 500ms for the whole
    // host-gone window (observed ~16 retries to a dead host before the
    // watchdog fired). A real blip recovers on the first quick retry --
    // recent admit resets the counter, so backoff only grows while the
    // host stays unreachable.
    if (SpectatorNode_MsSinceLastAdmit() > 0 &&
        SpectatorNode_MsSinceLastAdmit() < 1000) {
        g_state.reconnect_fail_count = 0;       // live again -> reset backoff
    }
    const uint32_t recon_base = (uint32_t)SPECTATOR_RECONNECT_BACKOFF_MS;
    const uint32_t recon_shift =
        g_state.reconnect_fail_count > 4 ? 4u : g_state.reconnect_fail_count;
    uint32_t recon_interval = recon_base << recon_shift;
    if (recon_interval > SPECTATOR_RECONNECT_MAX_BACKOFF_MS)
        recon_interval = (uint32_t)SPECTATOR_RECONNECT_MAX_BACKOFF_MS;
    // task #70: once we've been subscribed but never admitted (the handshake
    // burst was lost), REPEAT re-JOINs must be SLOWER than RC's 7s message
    // retirement -- the standard 2-4s reconnect backoff resets the host's bind
    // state and RESTARTS the snapshot+backfill burst before RC can
    // finish retransmitting it, so the two fight and nothing ever completes.
    // Give each re-ship a full retransmit window. (Pre-subscribe retries stay
    // fast to establish the connection quickly.)
    //
    // The FIRST attempt is exempt: 8s of a black screen is most of the 30s
    // connect deadline and is what a real viewer actually experiences, and the
    // dominant loss shape (the ACK vanished / the stash was dropped) needs no
    // retransmit window at all -- it needs the host to re-ship, which only a
    // re-JOIN past the 3s destructive-reset floor can provoke. The host-side
    // snapshot re-ship floor keeps that fast retry to a backfill-only burst,
    // so it cannot congest an in-flight snapshot the way a full 1MB restart
    // would. Counted, not timed, so exactly one fast attempt is ever made.
    static uint32_t s_noadmit_rejoin_count = 0;
    const uint64_t noadmit_interval = (s_noadmit_rejoin_count == 0)
        ? SPECTATOR_NOADMIT_FIRST_REJOIN_MS
        : SPECTATOR_NOADMIT_REJOIN_MS;
    const bool noadmit_shape =
        g_state.ever_subscribed && !SpectatorNode_HasEverAdmitted();
    if (noadmit_shape && recon_interval < noadmit_interval) {
        recon_interval = (uint32_t)noadmit_interval;
    }
    if (!g_state.session_ended &&            // host said SESSION_END: stop, no storm
        !g_state.subscribed_upstream &&
        g_state.root_addr.sin_port != 0 &&
        now - g_state.last_reconnect_attempt_ms >= recon_interval)
    {
        char buf[48] = {}; FormatAddr(g_state.root_addr, buf, sizeof(buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: reconnecting to root %s (attempt %u, "
                    "next backoff %ums)", buf,
                    g_state.reconnect_fail_count + 1, recon_interval);
        g_state.last_reconnect_attempt_ms = now;
        ++g_state.reconnect_fail_count;
        // Consume the one fast no-admit attempt here too: this branch and the
        // watchdog below are the same recovery seen from either side of the
        // subscribed_upstream latch (a re-JOIN clears it, so whichever one
        // fires first, the next one must already be on the slow cadence).
        if (noadmit_shape) ++s_noadmit_rejoin_count;
        SpectatorNode_RequestJoin(g_state.root_addr);
    }

    // RC full-transport handshake watchdog (task #70): subscribed but NEVER
    // admitted. The reconnect gate above is off (subscribed_upstream latched at
    // JOIN_ACK), but if the host's one-shot snapshot+backfill burst
    // was lost beyond RC's 7s retirement, nothing else re-JOINs -- gap-fill
    // needs have_frame_baseline, which is not set yet.
    // Without this the viewer sat subscribed-but-never-admitted (total=0,
    // spec_max_frame=0) until the 30s process-exit under heavy loss. Re-JOIN so
    // the host resets bind state (HandleJoinReq's destructive-reset branch) and
    // re-ships the burst. Reuses last_reconnect_attempt_ms (the first JOIN runs
    // through the loop above, so it is stamped). The interval clears the host's
    // 3s reset-suppression on the first attempt and RC's 7s retirement on every
    // one after it -- see SPECTATOR_NOADMIT_FIRST_REJOIN_MS.
    if (!g_state.session_ended &&
        g_state.subscribed_upstream &&
        !SpectatorNode_HasEverAdmitted() &&
        g_state.root_addr.sin_port != 0 &&
        now - g_state.last_reconnect_attempt_ms >= noadmit_interval)
    {
        char buf[48] = {}; FormatAddr(g_state.root_addr, buf, sizeof(buf));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: subscribed but NO frame admitted in %llums "
                    "(handshake burst lost under loss; q=%zu reorder=%zu "
                    "baseline=%d snap_bytes=%zu) -- no-admit re-JOIN #%u to %s "
                    "to force the host to re-ship the backfill "
                    "(+snapshot if past its re-ship floor)",
                    (unsigned long long)noadmit_interval,
                    g_state.pb_queue.size(), g_state.pb_reorder.size(),
                    (int)g_state.have_frame_baseline,
                    g_state.pb_snapshot_inbox.bytes_received,
                    s_noadmit_rejoin_count + 1, buf);
        g_state.last_reconnect_attempt_ms = now;
        ++s_noadmit_rejoin_count;
        SpectatorNode_RequestJoin(g_state.root_addr);
    }

    SpectatorNode_TickHostMaintenance();
}

void SpectatorNode_TickHostMaintenance() {
    const uint64_t now = (uint64_t)GetTickCount64();

    // ---- Spectator-incoming NAT punch poll --------------------------------
    // The launcher's hub-event handler (on_spectator_punch_target) writes
    // an external UDP addr into shared mem when the hub forwards a
    // spectator_incoming WS event. Poll spectator_punch_seq for changes;
    // each bump is a new spectator that needs us to fire an outbound
    // packet to open our NAT mapping for them. Without this their first
    // SPEC_JOIN_REQ gets dropped at our NAT and they sit on
    // "Connecting..." through every reconnect cycle.
    //
    // We send a small burst of SPEC_HEARTBEAT packets -- harmless on the
    // spectator side (they're not subscribed yet, packets get logged +
    // dropped) but enough to traverse our NAT and create the inbound
    // hole. The spectator's existing 2-second reconnect will then
    // succeed on its next attempt.
    {
        FM2KSharedMemData* shm = GetSharedMemory();
        static uint32_t s_last_punch_seq = 0;
        if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC &&
            shm->spectator_punch_seq != s_last_punch_seq) {
            s_last_punch_seq = shm->spectator_punch_seq;
            const uint32_t ip_be = shm->spectator_punch_ip_be;
            const uint16_t port  = shm->spectator_punch_port;
            if (ip_be != 0 && port != 0) {
                char ip_str[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &ip_be, ip_str, sizeof(ip_str));
                // Phase 2c: stash the spec_user_id from this punch event so
                // HandleJoinReq's new-subscriber branch can assign it onto
                // the Subscriber when the matching JOIN_REQ from this
                // (ip:port) arrives. Empty string when hub didn't include
                // user_id (older hub); harmless -- relay-mode SendTo will
                // just skip subs with no user_id.
                char user_id_buf[33] = {};  // shm has 32; +1 for safety NUL
                std::memcpy(user_id_buf, shm->spectator_punch_user_id,
                            sizeof(shm->spectator_punch_user_id));
                user_id_buf[32] = '\0';
                if (user_id_buf[0]) {
                    char addr_key[64];
                    std::snprintf(addr_key, sizeof(addr_key), "%s:%u",
                                  ip_str, (unsigned)port);
                    g_state.pending_spec_user_ids[addr_key] = user_id_buf;
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: hub-coordinated NAT punch toward "
                    "spectator %s:%u user_id=%s (seq=%u)",
                    ip_str, (unsigned)port,
                    user_id_buf[0] ? user_id_buf : "(none)",
                    (unsigned)s_last_punch_seq);

                sockaddr_in target{};
                target.sin_family      = AF_INET;
                target.sin_addr.s_addr = ip_be;
                target.sin_port        = htons(port);
                CtrlPacket hb{};
                hb.header.type = CtrlMsg::SPEC_HEARTBEAT;
                // 5-pack burst to ride out single-packet UDP loss; total
                // ~250 B at typical Ctrl size, negligible cost.
                for (int i = 0; i < 5; ++i) {
                    ControlChannel_SendTo(hb, target);
                }

            }
        }
    }

    // ---- Upstream-side: bind subscribers and ship their opener ----
    // On the direct path the reliable channel is already up when the
    // JOIN_REQ arrives (it is the same UDP socket), so a subscriber binds
    // immediately and the snapshot+backfill ship on the next pass.
    //
    // Relay mode (Phase 2c): the launcher's WS-to-hub data path is already
    // up at JOIN_REQ time too, but sends are addressed by spec_user_id, so
    // that field has to land before the sub can bind.
    for (auto& sub : g_state.subscribers) {
        if (sub.bound) continue;
        // Never frame-0-backfill a CURRENT_MATCH viewer: defer the bind
        // until a snapshot exists (next StashSnapshot = next battle
        // entry). The legacy fallback replayed the host's title/CSS
        // inputs into a /F-booted battle (join-during-CSS = total state
        // garbage, exposed by the CSS-dwell harness 2026-06-11), and
        // binding at the battle-entry tick raced StashSnapshot by ~50ms.
        // The viewer meanwhile holds at title until the battle-entry
        // JOIN_ACK re-broadcast seeds its BTB chars.
        // Keyed on the PINNED grant, never on the live kind: the old test
        // parked a between-matches joiner here the moment the host crossed
        // into battle -- waiting for a snapshot it is never going to be sent.
        // pinned_ack_kind == BATTLE is exactly "this sub was granted a
        // snapshot join", decided atomically at JOIN time.
        if (sub.pinned_ack_kind == SPEC_ACK_KIND_BATTLE &&
            !g_state.current_snapshot.valid) {
            // Battle just started but StashSnapshot hasn't run yet (the
            // 51ms bind-vs-stash race) -- wait a tick for the snapshot.
            // Pre-battle joins do NOT defer: they get the from-frame-0
            // stream and follow the host's CSS live (tournament flow:
            // spectators connect while the players sit at CSS, then
            // watch the lock-ins happen).
            static uint64_t s_last_defer_log_ms = 0;
            if (now - s_last_defer_log_ms > 2000) {
                s_last_defer_log_ms = now;
                char dbuf[48] = {}; FormatAddr(sub.addr, dbuf, sizeof(dbuf));
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: deferring CURRENT_MATCH bind for %s "
                    "until StashSnapshot (battle-entry race)", dbuf);
            }
            continue;
        }
        bool just_bound = false;
        if (g_state.spec_transport_relay) {
            // Need spec_user_id to address relay sends. If still empty
            // (loopback race -- punch dict hadn't populated when first
            // JOIN_REQ arrived), wait for the existing-sub re-JOIN path
            // to backfill it. Next bind-loop tick succeeds.
            if (!sub.spec_user_id.empty()) {
                sub.bound = true;
                just_bound = true;
            }
        } else {
            // Direct path: snapshot+backfill AND live all ride the reliable
            // channel to the sub's UDP addr, which is known from the JOIN_REQ
            // itself. There is nothing to wait for -- no dial, no accept.
            sub.bound = true;
            just_bound = true;
        }
        if (just_bound) {
            // Time the whole synchronous backfill: it runs holding g_poll_mutex,
            // so the host main loop's ControlChannel_Poll blocks for exactly this
            // long when a spectator binds. This is the Phase 3 hiccup measurement.
            const uint64_t bf_start = SDL_GetPerformanceCounter();
            char buf[48] = {}; FormatAddr(sub.addr, buf, sizeof(buf));

            // C5 backfill ordering fence:
            //   1. Send the chosen backfill payload (EVENT_BATCH chunks
            //      and/or SNAPSHOT_BEGIN/CHUNK/END). Refreshes
            //      sub.last_seen_ms post-completion so the
            //      SUBSCRIBER_EXPIRY_MS sweep can't reap mid-backfill.
            //   2. MarkBackfillComplete flips the TCP-layer fence so
            //      future BroadcastToAll calls finally include this sub.
            // Until step 2 fires, BroadcastToAll skips this sub -- any
            // live FlushBatch firing in this gap is silently elided and
            // the sub catches up via the backfill instead.

            // Phase 3 branch: CURRENT_MATCH-mode sub WITH a valid cached
            // snapshot → ship snapshot + tail events from snapshot's
            // anchor frame. Otherwise (a CSS grant, OR no snapshot yet
            // because this is the first match before its StashSnapshot
            // ran) fall back to legacy from-frame-0 backfill.
            // Light re-join: a mid-stream viewer declared its resume
            // position -- ship NOTHING but the gap. No snapshot (it
            // would be discarded viewer-side anyway), no from-anchor
            // re-delivery; one round trip and the stream is whole.
            const bool resume_bind = sub.resume_frame > 0;
            // The snapshot is served ONLY to a sub whose grant was BATTLE.
            // current_snapshot still holds the PREVIOUS battle's blob between
            // matches, so anything looser would drop a between-matches viewer
            // into the last match's battle state. pinned_ack_kind is the
            // grant, and BATTLE means and only means "a snapshot is coming".
            const bool use_snapshot = !resume_bind &&
                sub.pinned_ack_kind == SPEC_ACK_KIND_BATTLE &&
                g_state.current_snapshot.valid;
            // A sub that was NOT granted a snapshot join is a
            // between-matches joiner. It mirrors the current char-select and
            // enters the next battle by simulating MATCH_START, exactly like
            // a continuing spectator at a rematch seam -- so it only needs
            // the stream from the current CSS_ENTERED, not from frame 0.
            // The history matters for anyone touching this, so it stays
            // recorded:
            //
            // Wave 3.1 gated this OFF. The bounded anchor's latency win was
            // proven (2.0-2.9s boot-to-play, 90.6-92.9% of events skipped, 7/7)
            // but the deep joiner then DESYNCED: at an identical logical frame
            // -- same rng, hp, timer and script ids -- its character positions
            // sat at spawn while a continuing spectator's had moved. Skipping
            // match 1's simulation leaves engine state that battle-entry init
            // does not reset and that the event stream does not carry, and
            // an op-count fix-up for the skipped prefix cannot supply state
            // that prefix's simulation would have produced.
            //
            // Wave 4 closed that desync by shipping the battle savestate to a
            // held deep joiner at the host's next battle entry, and moved the
            // decision to sub.deep_join_eligible -- pinned at JOIN time from
            // the same read as the grant kind, so the SPEC_ACK_DEEP_JOIN bit
            // the viewer booted on and the payload shipped here are the same
            // decision rather than two derivations against state that moved in
            // between.
            //
            // The bar that let the default flip, and the bar for keeping it:
            // full-state fencepost identity (CHECKSUM, NOT the subset GATE --
            // the subset gate passed all 7 of the Wave 3.1 failing runs), which
            // run_all_tests stage 2d asserts on every gate run.
            const bool use_recent_anchor = sub.deep_join_eligible &&
                !resume_bind && !use_snapshot;
            // Re-ship floor: this sub already got THIS match's snapshot very
            // recently, so this rebind is a re-JOIN retry, not a fresh join.
            // Ship the tail only. use_snapshot itself is NOT cleared -- the
            // the backfill anchor must stay keyed to the snapshot the viewer
            // is (still) assembling. Never twice in a
            // row: see Subscriber::snapshot_reship_skips.
            const bool snapshot_throttled = use_snapshot &&
                sub.last_snapshot_ship_ms != 0 &&
                sub.snapshot_reship_skips == 0 &&
                sub.last_snapshot_match_idx == g_state.current_snapshot.match_index &&
                now - sub.last_snapshot_ship_ms < SPECTATOR_SNAPSHOT_RESHIP_MS;

            // NO op-count baseline is shipped. The OP_BASELINE packet existed
            // to seed the viewer's ops_seen and to re-arm the deleted 0xCE
            // accelerator's admission epoch, and it was gated on that
            // accelerator's capability bit -- which defaults OFF, so in every
            // shipped configuration the packet was never sent. What survives
            // of its job is redundant: EVENT_BATCH2 carries each batch's
            // ABSOLUTE op base, so the viewer's dedupe watermark self-seeds
            // from the first op it accepts and needs no per-bind priming.
            // Deleted rather than switched on, because switching it on would
            // be a behaviour change smuggled into a deletion.

            const char* xport = g_state.spec_transport_relay ? "RELAY" : "RC";
            // Host settings (rounds-to-win, timer, SOCD, etc) go to EVERY
            // joiner regardless of bind flavor -- the push lived only in
            // the snapshot branch, so natural-boot viewers ran
            // engine defaults (wrong round settings, 2026-06-11).
            {
                extern void Netplay_SendHostConfigToSpec(const sockaddr_in& to);
                Netplay_SendHostConfigToSpec(sub.addr);
            }
            if (resume_bind) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: %s bound for %s -- LIGHT RESUME "
                    "(gap backfill from INPUT-frame=%u, no snapshot)",
                    xport, buf, sub.resume_frame);
                SendSessionBackfillFromFrame(sub.addr, sub.resume_frame);
            } else if (use_snapshot) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: %s bound for %s%s%s -- CURRENT_MATCH "
                    "(%s match=%u + tail from INPUT-frame=%u)",
                    xport, buf,
                    g_state.spec_transport_relay ? " user_id=" : "",
                    g_state.spec_transport_relay ? sub.spec_user_id.c_str() : "",
                    snapshot_throttled ? "snapshot SUPPRESSED for"
                                       : "snapshot",
                    g_state.current_snapshot.match_index,
                    g_state.current_snapshot.input_frame);
                // Push current HOST_CONFIG over the UDP ctrl channel
                // BEFORE the snapshot. Live broadcasts only fire at
                // match-start moments (Netplay_StartBattle) -- a spec
                // joining mid-match would otherwise run on whatever
                // stale settings the engine spawned with (wrong stage,
                // default SOCD, etc) until the next round-end.
                extern void Netplay_SendHostConfigToSpec(const sockaddr_in& to);
                Netplay_SendHostConfigToSpec(sub.addr);
                if (snapshot_throttled) {
                    sub.snapshot_reship_skips = 1;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: snapshot re-ship to %s SUPPRESSED "
                        "(shipped %llums ago, floor %llums) -- tail backfill "
                        "only. A re-JOIN this soon is a retry for the LOST "
                        "backfill, and re-blasting the blob competes with the "
                        "chunks still in flight for it. The NEXT rebind ships "
                        "it regardless",
                        buf,
                        (unsigned long long)(now - sub.last_snapshot_ship_ms),
                        (unsigned long long)SPECTATOR_SNAPSHOT_RESHIP_MS);
                } else {
                    SendSnapshotTo(sub.addr);
                    sub.last_snapshot_ship_ms   = now;
                    sub.last_snapshot_match_idx = g_state.current_snapshot.match_index;
                    sub.snapshot_reship_skips   = 0;
                }
                SendSessionBackfillFromFrame(sub.addr,
                    g_state.current_snapshot.input_frame);
            } else if (use_recent_anchor) {
                // Ask first, then announce: HaveBoundedAnchor() is the same
                // predicate the sender uses, so a fresh-session joiner (no
                // prior match yet) no longer logs a BETWEEN-MATCHES claim it
                // retracts on the very next line.
                if (HaveBoundedAnchor()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: %s bound for %s -- BETWEEN-MATCHES "
                        "(bounded backfill from the current char-select, no "
                        "snapshot; mirrors this CSS and enters the next battle "
                        "with the stream)", xport, buf);
                } else {
                    // Since Wave 4 this is UNREACHABLE, and deliberately kept:
                    // eligibility already required HaveBoundedAnchor() at pin
                    // time, and both terms behind it (have_prior_match,
                    // have_css_anchor) are monotonic false -> true with no
                    // trimming, so an anchor that existed at the pin still
                    // exists at the bind. If it ever fires, the grant told a
                    // viewer to hold for a snapshot while shipping it a
                    // from-frame-0 stream -- log it loudly rather than let it
                    // be a silent freeze.
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: %s bound for %s -- deep-join grant but "
                        "NO bounded anchor at bind time (should be impossible: "
                        "anchors are monotonic) -- full from-frame-0 backfill",
                        xport, buf);
                }
                SendSessionBackfillFromRecentAnchor(sub.addr);
                // Wave 4: this viewer is skipping prior matches' SIMULATION, so
                // it owes a battle savestate before it may enter a battle.
                // Arm/defer the push (never ships the PREVIOUS battle's cached
                // blob -- see DeepJoinOnBind).
                DeepJoinOnBind(sub);
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: %s bound for %s%s%s -- session-start "
                    "backfill (no prior match to bound against yet)",
                    xport, buf,
                    g_state.spec_transport_relay ? " user_id=" : "",
                    g_state.spec_transport_relay ? sub.spec_user_id.c_str() : "");
                SendSessionBackfillFromStart(sub.addr);
            }

            sub.last_seen_ms = now;            // post-backfill liveness anchor
            const double bf_ms = (double)(SDL_GetPerformanceCounter() - bf_start)
                                 * 1000.0 / (double)SDL_GetPerformanceFrequency();
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-BACKFILL] join backfill for %s took %.1fms (events=%zu) -- "
                "host main loop blocked this whole time", buf, bf_ms,
                g_state.session_events.size());
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: backfill complete for %s -- live broadcasts engaged",
                        buf);
            // One bind + backfill per maintenance tick. Several spectators can
            // be ready at the SAME instant -- classically a tournament's worth
            // waiting at CSS that all bind on the battle-entry tick when
            // StashSnapshot first validates the snapshot. Each backfill is only
            // ~1-3ms (the 1MB state zero-RLEs to ~32KB), but N of them in a
            // single game frame would stack. Spreading them one-per-frame keeps
            // the host's per-frame spectator cost bounded no matter how many
            // join at once -- the rest bind on the next tick (a few frames'
            // delay to start watching, imperceptible).
            break;
        }
    }

    // ---- Upstream-side: bounded deep-join snapshot push ------------------
    // Drains the arm set StashSnapshot refreshes at every battle entry, ONE sub
    // per maintenance tick -- the same pacing (and the same reason) as the bind
    // loop above. Only subs flagged deep_join_eligible are ever in that set:
    // a continuing viewer and a from-frame-0 viewer are bit-exact by simulation
    // and a pushed snapshot would be pure risk for them.
    DeepJoinPushTick(now);

    // ---- Upstream-side: expire silent subscribers -----------------------
    for (auto it = g_state.subscribers.begin(); it != g_state.subscribers.end(); ) {
        if (now - it->last_seen_ms >= SPECTATOR_SUBSCRIBER_EXPIRY_MS) {
            char buf[48] = {}; FormatAddr(it->addr, buf, sizeof(buf));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: subscriber %s silent -- expiring + sending LEAVE",
                        buf);
            // Notify so they fail over fast instead of waiting their own timer.
            CtrlPacket leave = {};
            leave.header.type = CtrlMsg::SPEC_LEAVE;
            ControlChannel_SendTo(leave, it->addr);
            it = g_state.subscribers.erase(it);
        } else {
            ++it;
        }
    }

}
