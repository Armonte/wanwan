// Spectator per-tick drivers: viewer-side TickHealth (heartbeat/failover/
// reconnect + relay-inbound drain) and host-side TickHostMaintenance (NAT punch,
// TCP bind, subscriber sweep). Extracted VERBATIM from spectator_node.cpp.
// Public API (decls in spectator_node.h); calls specnode helpers via using.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
#include "spectator_tcp.h"        // TCP transport for INPUT_BATCH stream
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

// In-flight TCP punch sockets -- see TickHostMaintenance comment for why
// we defer close. Each entry holds a SOCKET handle and the wall-clock
// deadline (ms) after which it's safe to closesocket(). The sweep at
// the bottom of TickHostMaintenance (which runs every frame) processes
// expired entries. Bounded by spectator-join rate, ~1/sec at most.
struct PendingPunchSock {
    SOCKET   handle;
    uint64_t close_after_ms;
};
std::vector<PendingPunchSock> g_pending_punch_sockets;

void SpectatorNode_TickHealth() {
    const uint64_t now = (uint64_t)GetTickCount64();

    // ---- Spec hub-relay inbound drain (Phase 3) -------------------------
    // When the launcher has WS binary frames forwarded from the hub, it
    // writes them into the inbound shared-mem ring. Each Slot's payload
    // is a SpecDataHeader-prefixed wire frame -- byte-identical to what
    // SpectatorTCP::PollUpstream would have produced from the TCP path
    // it's replacing. Feed straight into HandleSpecData.
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
            // payload bytes are exactly what SpectatorTCP's framer
            // would deliver to HandleSpecData via the TCP path. The
            // sockaddr_in second arg is a debug breadcrumb; zero
            // works (TCP path also passes zero).
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

        // Phase F: silent-TCP-death detector. A connection can die
        // asymmetrically -- the host's side errors out while ours never
        // surfaces anything (observed 2026-06-11: host "subscriber read
        // error" at the match-2 boundary, spec recv just went quiet; the
        // op gate then paused UDP forever and nothing re-joined). The UDP
        // datagrams announce the host's op count: if ops we provably lack
        // (udp_highest_op_seq > ops_seen) stay undeliverable while the
        // TCP stream has been silent for several seconds, the stream is
        // wedged regardless of HOW it died. Re-join is cheap (~2s).
        if (!g_state.spec_transport_relay &&
            g_state.udp_epoch_armed &&
            g_state.udp_highest_op_seq > g_state.ops_seen) {
            const uint64_t tcp_last = SpectatorTCP::LastUpstreamRecvMs();
            if (tcp_last > 0 && now > tcp_last && now - tcp_last >= 4000) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: op-gap stall -- UDP announces op_seq=%u "
                    "but ops_seen=%u and TCP silent %llums; declaring "
                    "upstream TCP dead",
                    g_state.udp_highest_op_seq, g_state.ops_seen,
                    (unsigned long long)(now - tcp_last));
                SpectatorNode_OnUpstreamTcpDead();
            }
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
        //   VARIANT 2 -- a mid-match snapshot applied in an ACTIVE battle
        //     but the RC live stream delivered NOTHING at all (pb_reorder
        //     EMPTY, sim starved, cursor frozen at the anchor). Nothing is
        //     buffered to signal the gap, so variant-1 detection is blind.
        // Firing when actually caught-up is a host-side no-op
        // (SendSessionBackfillFromFrame ships nothing at-or-after the live
        // edge), so an over-eager pull costs one ACK + empty backfill.
        const bool gap_ahead = !g_state.pb_reorder.empty() &&
            g_state.pb_reorder.begin()->first > g_state.next_expected_frame;
        bool starved_battle = false;
        if (!gap_ahead && g_state.playing_back &&
            g_state.pb_snapshot_applied_once &&
            SpectatorNode_PendingFrameCount() == 0) {
            // Gate on battle mode: a starved queue at a match boundary /
            // CSS is NORMAL and must not pull (the pull's JOIN_ACK carries
            // the host's CURRENT kind -- a between-matches kind would abort
            // the viewer's BTB). game_mode 3000 == in battle, where a live
            // stream should always be feeding us.
            const uint32_t gm = *(const uint32_t*)FM2K::ADDR_GAME_MODE;
            starved_battle = (gm == 3000);
        }
        if (g_state.have_frame_baseline && (gap_ahead || starved_battle)) {
            if (g_state.next_expected_frame != g_state.gap_fill_stall_frame) {
                // Cursor moved (or first observation) -- (re)start the timer.
                g_state.gap_fill_stall_frame    = g_state.next_expected_frame;
                g_state.gap_fill_stall_since_ms = now;
            } else if (now - g_state.gap_fill_stall_since_ms >=
                           SPECTATOR_GAP_FILL_STALL_MS &&
                       now - g_state.last_gap_fill_send_ms >=
                           SPECTATOR_GAP_FILL_THROTTLE_MS) {
                g_state.last_gap_fill_send_ms = now;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: gap-fill pull -- cursor stalled at "
                    "INPUT-frame=%u (%s, %zu buffered); requesting reliable "
                    "re-ship",
                    g_state.next_expected_frame,
                    gap_ahead ? "gap ahead" : "live starved",
                    g_state.pb_reorder.size());
                SpectatorNode_RequestGapFill();
            }
        } else {
            g_state.gap_fill_stall_frame = 0xFFFFFFFFu;  // no gap / not battle
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
        // That is the FULL_SESSION-grant-vs-BATTLE-ACK mismatch: the viewer
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
                        SpectatorNode_RequestJoin(g_state.root_addr,
                                                  g_state.last_requested_mode);
                    }
                }
            }
        }

        // Silence-based failover. TCP receive activity is the only
        // liveness signal -- the bulk stream lives entirely there.
        //
        // Suppressed when:
        //   (a) Catchup is active. The catchup loop runs sim+disk-IO at
        //       full speed and may legitimately go quiet for seconds --
        //       especially during CSS replay when each cursor move blocks
        //       on a .player file load. Tearing down here would force a
        //       backfill replay that re-incurs the same cost, never
        //       finishing.
        //   (b) pb_queue still has events to drain. recv_ms only updates
        //       on NEW TCP bytes; if the host already sent us 16k events
        //       of backfill in one burst and we're slowly chewing through
        //       them, recv_ms goes stale even though we're actively
        //       processing data. The previous gate (a alone) wasn't
        //       enough -- once initial catchup ended (s_initial_catchup_done
        //       latched true), reconnect-and-backfill paths bypassed
        //       catchup entirely and the failover here would fire mid-
        //       drain → tear down the connection we're using → reconnect
        //       → re-receive backfill → same cycle. Death loop observed
        //       in P3 logs after a host network blip.
        extern bool g_spectator_catchup;
        const uint64_t recv_ms     = SpectatorTCP::LastUpstreamRecvMs();
        const bool     queue_idle  = SpectatorNode_PendingFrameCount() == 0;
        //   (c) snapshot transfer in progress. Under real loss the 1MB
        //       snapshot trickles at TCP-throughput pace (~30KB/s at 20%
        //       loss / 140ms RTT) and a single retransmit-backoff stall
        //       can exceed 5s. Failing over mid-transfer drops the inbox
        //       and restarts the 1MB from scratch -- at high loss the
        //       join NEVER completes (observed: failover at 311296/1081196
        //       bytes, repeating forever). While the inbox is mid-
        //       transfer, allow a much longer window (30s without ANY
        //       TCP bytes) before declaring the upstream dead.
        const auto& snap_inbox = g_state.pb_snapshot_inbox;
        const bool snapshot_in_flight =
            snap_inbox.active &&
            snap_inbox.bytes_received < snap_inbox.meta.total_bytes;
        const uint64_t silence_budget_ms =
            (snapshot_in_flight || !g_state.live_established)
                ? (uint64_t)30000
                : (uint64_t)SPECTATOR_SILENCE_FAILOVER_MS;
        // UDP is the primary liveness signal now: inputs + ops ride
        // datagrams, so TCP being quiet is NORMAL (retransmit storms
        // under loss, host swap stalls, nothing bulk to send). The old
        // TCP-only condition fired at every battle->CSS return under
        // clumsy -- queue drained (host stalled production), TCP quiet
        // past budget -> the viewer sent SPEC_LEAVE, unsubscribed
        // ITSELF, killed its own UDP fan-out, and span through a full
        // reconnect against a host mid-swap. Fail over only when BOTH
        // transports are silent.
        const bool udp_alive = g_state.last_udp_recv_ms > 0 &&
            now - g_state.last_udp_recv_ms < 2000;
        if (!g_spectator_catchup &&
            queue_idle &&
            !udp_alive &&
            recv_ms > 0 &&
            now - recv_ms >= silence_budget_ms)
        {
            char buf[48] = {}; FormatAddr(g_state.upstream_addr, buf, sizeof(buf));
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: upstream %s silent for %llu ms -- failing over",
                        buf,
                        (unsigned long long)(now - recv_ms));
            CtrlPacket leave = {};
            leave.header.type = CtrlMsg::SPEC_LEAVE;
            ControlChannel_SendTo(leave, g_state.upstream_addr);
            g_state.subscribed_upstream = false;
            g_state.live_established    = false;
            SpectatorTCP::DisconnectUpstream();
        }
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
    const uint32_t recon_base =
        g_state.tcp_rejoin_pending ? 500u : (uint32_t)SPECTATOR_RECONNECT_BACKOFF_MS;
    const uint32_t recon_shift =
        g_state.reconnect_fail_count > 4 ? 4u : g_state.reconnect_fail_count;
    uint32_t recon_interval = recon_base << recon_shift;
    if (recon_interval > SPECTATOR_RECONNECT_MAX_BACKOFF_MS)
        recon_interval = (uint32_t)SPECTATOR_RECONNECT_MAX_BACKOFF_MS;
    // task #70: once we've been subscribed but never admitted (the handshake
    // burst was lost), REPEAT re-JOINs must be SLOWER than RC's 7s message
    // retirement -- the standard 2-4s reconnect backoff resets the host's bind
    // state and RESTARTS the OP_BASELINE+snapshot+backfill burst before RC can
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
        (!g_state.subscribed_upstream || g_state.tcp_rejoin_pending) &&
        g_state.root_addr.sin_port != 0 &&
        // Never fire a new JOIN while an upstream connection exists --
        // the host's existing-sub re-JOIN path drops connections from
        // our addr, so a retry during an in-flight handshake/backfill
        // kills its own transfer ("End of stream" loop, 2026-06-11).
        !SpectatorTCP::IsUpstreamConnected() &&
        now - g_state.last_reconnect_attempt_ms >= recon_interval)
    {
        char buf[48] = {}; FormatAddr(g_state.root_addr, buf, sizeof(buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: reconnecting to root %s (mode=%s, attempt %u, "
                    "next backoff %ums)", buf,
                    g_state.last_requested_mode == SpecJoinMode::CURRENT_MATCH
                        ? "CURRENT_MATCH" : "FULL_SESSION",
                    g_state.reconnect_fail_count + 1, recon_interval);
        g_state.last_reconnect_attempt_ms = now;
        ++g_state.reconnect_fail_count;
        // Consume the one fast no-admit attempt here too: this branch and the
        // watchdog below are the same recovery seen from either side of the
        // subscribed_upstream latch (a re-JOIN clears it, so whichever one
        // fires first, the next one must already be on the slow cadence).
        if (noadmit_shape) ++s_noadmit_rejoin_count;
        SpectatorNode_RequestJoin(g_state.root_addr, g_state.last_requested_mode);
    }

    // RC full-transport handshake watchdog (task #70): subscribed but NEVER
    // admitted. The reconnect gate above is off (subscribed_upstream latched at
    // JOIN_ACK), but if the host's one-shot OP_BASELINE+snapshot+backfill burst
    // was lost beyond RC's 7s retirement, no other path re-JOINs in RC full-
    // transport mode -- the silence-failover + op-gap detectors need a TCP recv
    // stamp that's 0 here, and gap-fill needs have_frame_baseline (not yet set).
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
        !SpectatorTCP::IsUpstreamConnected() &&
        now - g_state.last_reconnect_attempt_ms >= noadmit_interval)
    {
        char buf[48] = {}; FormatAddr(g_state.root_addr, buf, sizeof(buf));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: subscribed but NO frame admitted in %llums "
                    "(handshake burst lost under loss; q=%zu reorder=%zu "
                    "baseline=%d snap_bytes=%zu) -- no-admit re-JOIN #%u to %s "
                    "to force the host to re-ship OP_BASELINE+backfill "
                    "(+snapshot if past its re-ship floor)",
                    (unsigned long long)noadmit_interval,
                    g_state.pb_queue.size(), g_state.pb_reorder.size(),
                    (int)g_state.have_frame_baseline,
                    g_state.pb_snapshot_inbox.bytes_received,
                    s_noadmit_rejoin_count + 1, buf);
        g_state.last_reconnect_attempt_ms = now;
        ++s_noadmit_rejoin_count;
        SpectatorNode_RequestJoin(g_state.root_addr, g_state.last_requested_mode);
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

                // TCP simultaneous-open punch (v0.2.35). UDP heartbeat
                // above only opens our NAT for inbound UDP; the
                // INPUT_BATCH stream rides TCP, which uses an entirely
                // separate NAT mapping. Without a TCP-side punch, spec's
                // TCP SYN to our listener gets dropped at our NAT and
                // they sit on "Connecting..." through every reconnect.
                //
                // Strategy: create a temporary raw TCP socket, set
                // SO_REUSEADDR so we can bind to the same port our
                // listener already holds, bind to that port, mark
                // non-blocking, and call connect() toward the spec's
                // external TCP addr. The connect almost certainly fails
                // (spec hasn't punched their side yet, or even if they
                // have the simultaneous-open negotiation usually doesn't
                // succeed in time) -- that's fine. The point is the SYN
                // we send out registers an outbound flow in our NAT's
                // state table from listener_port -> spec_ext_ip:tcp_port.
                // When spec's connect SYN arrives at listener_port from
                // that exact remote endpoint, our NAT lets it through.
                // Listener accept() picks it up normally.
                //
                // Skip when spec_tcp_port == 0 (older spec client without
                // TCP-port reporting) -- UDP-only path is what they had.
                const uint16_t tcp_port = shm->spectator_punch_tcp_port;
                const uint16_t our_listen = SpectatorTCP::GetListenPort();
                if (tcp_port != 0 && our_listen != 0) {
                    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (s != INVALID_SOCKET) {
                        BOOL reuse = TRUE;
                        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                                     (const char*)&reuse, sizeof(reuse));
                        sockaddr_in local{};
                        local.sin_family      = AF_INET;
                        local.sin_addr.s_addr = INADDR_ANY;
                        local.sin_port        = htons(our_listen);
                        if (::bind(s, (sockaddr*)&local, sizeof(local)) == 0) {
                            // Non-blocking so connect() returns
                            // immediately with WSAEWOULDBLOCK. We let
                            // the SYN actually leave the kernel before
                            // closing -- see g_pending_punch_sockets
                            // below.
                            u_long nb = 1;
                            ::ioctlsocket(s, FIONBIO, &nb);
                            sockaddr_in dst{};
                            dst.sin_family      = AF_INET;
                            dst.sin_addr.s_addr = ip_be;
                            dst.sin_port        = htons(tcp_port);
                            ::connect(s, (sockaddr*)&dst, sizeof(dst));
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "SpectatorNode: TCP punch %s:%u from local "
                                "port %u (listener) -- SYN out, deferred close",
                                ip_str, (unsigned)tcp_port,
                                (unsigned)our_listen);
                            // Defer close. Closing immediately races the
                            // kernel's SYN emission (non-blocking
                            // connect just queues; close + linger=0
                            // would abort the unsent SYN). Stash with
                            // a 2-second cleanup deadline; the bottom
                            // of TickHostMaintenance sweeps expired
                            // entries on every tick. By then the SYN
                            // is long-gone and our NAT mapping is
                            // established for ~30 s.
                            g_pending_punch_sockets.push_back(
                                {s, now + 2000});
                        } else {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "SpectatorNode: TCP punch bind to :%u failed "
                                "(WSA=%d) -- punch skipped",
                                (unsigned)our_listen, WSAGetLastError());
                            ::closesocket(s);
                        }
                    } else {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "SpectatorNode: TCP punch socket() failed "
                            "(WSA=%d)", WSAGetLastError());
                    }
                }
            }
        }
    }

    // ---- Upstream-side: bind newly-arrived TCP clients to subscribers ----
    // Spectator's async TCP dial completes some frames after JOIN_ACK; the
    // accept queue carries a fresh socket waiting to be paired by IP. On
    // first successful pair, ship INITIAL_MATCH + backfill -- those bytes
    // were deferred at JOIN_REQ accept time because the socket didn't
    // exist yet.
    //
    // Relay mode (Phase 2c): there's no TCP listener and no async dial;
    // the launcher's WS-to-hub data path is already up at JOIN_REQ time.
    // So as soon as we have a spec_user_id (for addressing relay sends),
    // mark the sub "bound" immediately so the snapshot+backfill ship.
    // Without this short-circuit, sub.tcp_bound stayed false forever in
    // relay mode and the spec saw zero data after subscribing.
    for (auto& sub : g_state.subscribers) {
        if (sub.tcp_bound) continue;
        // Never frame-0-backfill a CURRENT_MATCH viewer: defer the bind
        // until a snapshot exists (next StashSnapshot = next battle
        // entry). The legacy fallback replayed the host's title/CSS
        // inputs into a /F-booted battle (join-during-CSS = total state
        // garbage, exposed by the CSS-dwell harness 2026-06-11), and
        // binding at the battle-entry tick raced StashSnapshot by ~50ms.
        // The viewer meanwhile holds at title until the battle-entry
        // JOIN_ACK re-broadcast seeds its BTB chars.
        // Keyed on the PINNED grant, not on join_mode + live kind. Since the
        // Design 2 downgrade removal, a between-matches joiner keeps
        // join_mode == CURRENT_MATCH, so the old test would park it here the
        // moment the host crossed into battle -- waiting for a snapshot it is
        // never going to be sent. pinned_ack_kind == BATTLE is exactly "this
        // sub was granted a snapshot join", decided atomically at JOIN time.
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
                sub.tcp_bound = true;
                just_bound = true;
            }
        } else if (SpecRcSnapshotEnabled()) {
            // RC full transport (task #55): snapshot+backfill AND live all
            // ride RC to the sub's UDP addr, known since JOIN_REQ. Same
            // shape as the relay short-circuit above -- there is no TCP
            // accept to wait for (the viewer doesn't even dial in this
            // mode). Without this the sub stayed unbound forever and the
            // viewer's 30s connect watchdog killed the process.
            sub.tcp_bound = true;
            just_bound = true;
        } else if (SpectatorTCP::RegisterAcceptedClient(sub.addr)) {
            sub.tcp_bound = true;
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
            // anchor frame. Otherwise (FULL_SESSION, OR no snapshot yet
            // because this is the first match before its StashSnapshot
            // ran) fall back to legacy from-frame-0 backfill.
            // Light re-join: a mid-stream viewer declared its resume
            // position -- ship NOTHING but the gap. No snapshot (it
            // would be discarded viewer-side anyway), no from-anchor
            // re-delivery; one round trip and the stream is whole.
            const bool resume_bind = sub.resume_frame > 0;
            // The snapshot is served ONLY to a sub whose grant was BATTLE.
            // Before Design 2 that was implied by join_mode (anything joining
            // outside battle got downgraded to FULL_SESSION); now that the
            // downgrade is gone, join_mode stays CURRENT_MATCH between matches
            // and current_snapshot still holds the PREVIOUS battle's blob, so
            // keying on join_mode alone would drop a between-matches viewer
            // into the last match's battle state. pinned_ack_kind is the
            // grant, and BATTLE means and only means "a snapshot is coming".
            const bool use_snapshot = !resume_bind &&
                sub.pinned_ack_kind == SPEC_ACK_KIND_BATTLE &&
                g_state.current_snapshot.valid;
            // Design 2: a CURRENT_MATCH sub that was NOT granted a snapshot
            // join is a between-matches joiner. It mirrors the current
            // char-select and enters the next battle by simulating
            // MATCH_START, exactly like a continuing spectator at a rematch
            // seam -- so it only needs the stream from the current
            // CSS_ENTERED, not from frame 0.
            // GATED OFF BY DEFAULT (Wave 3.1). The bounded anchor's latency
            // win is proven (2.0-2.9s boot-to-play, 90.6-92.9% of events
            // skipped, 7/7) but the deep joiner then DESYNCS: at an identical
            // logical frame -- same rng, hp, timer and script ids -- its
            // character positions sit at spawn while a continuing spectator's
            // have moved. Skipping match 1's simulation leaves engine state
            // that battle-entry init does not reset and that the event stream
            // does not carry, and OP_BASELINE fixes the op COUNT for the
            // skipped prefix but cannot supply state that prefix's simulation
            // would have produced. Until that state is transported, the
            // default must be the proven-correct from-frame-0 path.
            //
            // FM2K_SPEC_DEEP_JOIN=1 re-enables it for measurement. Do NOT
            // default this on again without a full-state fencepost run
            // (CHECKSUM, not the subset GATE -- the subset gate passed all 7
            // failing runs).
            static int s_deep_join = -1;
            if (s_deep_join < 0) {
                const char* e = std::getenv("FM2K_SPEC_DEEP_JOIN");
                s_deep_join = (e && e[0] == '1') ? 1 : 0;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: deep-join bounded anchor %s "
                    "(FM2K_SPEC_DEEP_JOIN)",
                    s_deep_join ? "ENABLED -- known full-state desync, "
                                  "measurement only"
                                : "disabled (default; from-frame-0 backfill)");
            }
            const bool use_recent_anchor = s_deep_join == 1 &&
                !resume_bind && !use_snapshot &&
                sub.join_mode == SpecJoinMode::CURRENT_MATCH;
            // Re-ship floor: this sub already got THIS match's snapshot very
            // recently, so this rebind is a re-JOIN retry, not a fresh join.
            // Ship the tail only. use_snapshot itself is NOT cleared -- the
            // OP_BASELINE first_idx and the backfill anchor must stay keyed to
            // the snapshot the viewer is (still) assembling. Never twice in a
            // row: see Subscriber::snapshot_reship_skips.
            const bool snapshot_throttled = use_snapshot &&
                sub.last_snapshot_ship_ms != 0 &&
                sub.snapshot_reship_skips == 0 &&
                sub.last_snapshot_match_idx == g_state.current_snapshot.match_index &&
                now - sub.last_snapshot_ship_ms < SPECTATOR_SNAPSHOT_RESHIP_MS;

            // Phase F: op-count baseline FIRST on the fresh connection --
            // before snapshot/backfill -- so the viewer's ops_seen starts
            // exact for everything this connection delivers. The
            // connection ships ops from min(backfill_first_idx,
            // live-flush cursor) onward; the baseline counts the ops
            // BELOW that point (which a mid-session joiner never sees).
            // Gated on udp_ok: an old build's TCP framer drops the
            // connection on an unknown SpecDataType.
            if (sub.udp_ok && SpecUdpEnabled() &&
                !g_state.spec_transport_relay) {
                // Must name the SAME first event the backfill below ships,
                // or the viewer's ops_seen starts off by the difference.
                const size_t first_idx = resume_bind
                    ? BackfillFirstIdxForFrame(sub.resume_frame)
                    : use_snapshot
                    ? BackfillFirstIdxForFrame(g_state.current_snapshot.input_frame)
                    : (use_recent_anchor && g_state.have_css_anchor)
                    ? g_state.css_anchor_event_idx
                    : 0;
                const size_t clamp = std::min(g_state.last_flushed_event_idx,
                                              g_state.session_events.size());
                const size_t effective_start = std::min(first_idx, clamp);
                uint32_t baseline = 0;
                for (size_t i = 0; i < effective_start; ++i) {
                    if (g_state.session_events[i].type != SessionEventType::INPUT)
                        ++baseline;
                }
                SendOpBaselineTo(sub.addr, baseline);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: OP_BASELINE=%u sent (first_idx=%zu, "
                    "clamp=%zu, total_ops=%u)",
                    baseline, first_idx, clamp, g_state.total_op_count);
            }

            const char* xport = g_state.spec_transport_relay ? "RELAY" : "TCP";
            // Host settings (rounds-to-win, timer, SOCD, etc) go to EVERY
            // joiner regardless of bind flavor -- the push lived only in
            // the snapshot branch, so natural/FULL_SESSION viewers ran
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
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: %s bound for %s -- CURRENT_MATCH with "
                        "no bounded anchor yet (first char-select of the "
                        "session) -- full from-frame-0 backfill",
                        xport, buf);
                }
                SendSessionBackfillFromRecentAnchor(sub.addr);
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: %s bound for %s%s%s -- %s "
                    "(legacy from-frame-0 backfill)",
                    xport, buf,
                    g_state.spec_transport_relay ? " user_id=" : "",
                    g_state.spec_transport_relay ? sub.spec_user_id.c_str() : "",
                    sub.join_mode == SpecJoinMode::CURRENT_MATCH
                        ? "CURRENT_MATCH requested but no snapshot yet"
                        : "FULL_SESSION");
                SendSessionBackfillTo(sub.addr);
            }

            sub.last_seen_ms = now;            // post-backfill liveness anchor
            SpectatorTCP::MarkBackfillComplete(sub.addr);
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
            SpectatorTCP::DisconnectSubscriber(it->addr);
            it = g_state.subscribers.erase(it);
        } else {
            ++it;
        }
    }

    // ---- Sweep deferred TCP-punch sockets --------------------------------
    // 2 s after each punch the SYN has long since left the kernel and our
    // NAT mapping is established for the typical 30 s+ TCP-NAT TTL. Safe
    // to close. Linger=0 so Windows sends an RST instead of waiting in
    // FIN_WAIT (which we don't need -- the spec's incoming SYN goes to
    // our LISTENER, not this transient connect socket).
    for (auto it = g_pending_punch_sockets.begin();
         it != g_pending_punch_sockets.end(); ) {
        if (now >= it->close_after_ms) {
            struct linger lng = { 1, 0 };
            ::setsockopt(it->handle, SOL_SOCKET, SO_LINGER,
                         (const char*)&lng, sizeof(lng));
            ::closesocket(it->handle);
            it = g_pending_punch_sockets.erase(it);
        } else {
            ++it;
        }
    }
}
