// Bounded deep-join snapshot transport (Wave 4). Both ends of one mechanism:
//
//   HOST   -- push the freshly cached battle snapshot, at every battle entry,
//             to the subscribers that were admitted through the bounded
//             deep-join path and have not been corrected yet. Nobody else.
//   VIEWER -- a bounded deep joiner HOLDS at battle entry until that snapshot
//             applies, supervises the hold with a cheap re-request, and
//             escalates to a full re-JOIN. It never releases into the
//             from-scratch battle path.
//
// WHY A SNAPSHOT AND NOT A PAYLOAD. Wave 3 proved the bounded anchor's latency
// win (2.0-2.9 s to live, ~91 % of events skipped) and Wave 3.1 proved it
// desyncs: at an identical logical frame -- same rng, hp, timer and BOTH script
// ids -- the deep joiner's characters sit at their spawn X while a continuing
// spectator's have moved. The enumeration that followed found FIVE independent
// families of state that match N's SIMULATION leaves behind, that battle-entry
// init does not reset, and that the event stream does not carry: object-pool
// free-slot occupancy (g_object_pool @ 0x4701E0; the match-complete branch at
// 0x409825 demotes nothing, so every battle object stays type 4), object list
// topology (0x430240 heads/tails + the 0x4CFA20 node pool + g_current_object_ptr),
// the 162 KB afterimage pool @ 0x447930 (allocated first-free, and its
// camera_manager consumer draws 3 game_rand per frame -- a SIM-path RNG
// consumer), per-object script-VM cursors into a blob CSS frees and reallocates,
// and the never-reset system-variable array @ 0x4456B0. Enumerating those onto
// the wire is the whack-a-mole that was explicitly ruled out; the battle
// savestate covers all five BY CONSTRUCTION, which is why the transport is the
// snapshot itself.
//
// Coverage was VERIFIED before this was written, not assumed:
//   * afterimage pool -- SaveStateData::afterimage_pool, savestate.h:115, over
//     WaveCAddrs::AFTERIMAGE_POOL 0x447930 .. 0x46F6C0 (savestate.h:202-203);
//     saved whole at savestate_fm2k_save.cpp:463, restored at
//     savestate_fm2k_load.cpp:524-535 with three documented render-side
//     carve-outs (palette-flash-1 42 B, shake 40 B, g_last_frame_time 4 B, all
//     inside the first 0x4A8 bytes of preamble) plus the spec-only forced
//     stage-sound carve-out at 0x44795C.
//   * system variables A-P -- SaveStateData::effect_sys2, savestate.h:95, over
//     EffectAddrs::EFFECT_SYS2 0x4456B0 + 88 (savestate_internal.h:33-34);
//     savestate_fm2k_load.cpp:447-457 names the layout explicitly and RESTORES
//     offsets 0x00..0x20, i.e. exactly the 32-byte 0x4456B0..0x4456CF array.
//
// GATED OFF: everything here is downstream of DeepJoinEnabled()
// (FM2K_SPEC_DEEP_JOIN=1) on the host, and of the SPEC_ACK_DEEP_JOIN grant bit
// on the viewer. With the gate off the host never marks a sub eligible, so no
// push is ever armed and no viewer is ever told to hold.
//
// ---------------------------------------------------------------------------
// TELEMETRY CONTRACT -- the viewer-side ladder's outcomes, one line each.
// ---------------------------------------------------------------------------
// The rollout plan (docs/dev/spectate_deep_join_rollout.md, Phase 1.3 and
// Phase 3) counts these out of uploaded logs, so each terminal outcome emits
// EXACTLY ONE line per occurrence and the substrings below are the stable
// contract. Do NOT reword them without updating the analysers that grep them:
//
//   applied           "[SPEC-DEEPJOIN] snapshot APPLIED at anchor="
//                     the deep join completed; viewer is bit-exact from here.
//   re-requested      "[SPEC-DEEPJOIN] re-requesting the battle snapshot"
//                     one per SPEC_SNAPSHOT_REQ (progress-gated, ~1.5s apart).
//   escalated         "[SPEC-DEEPJOIN] hold budget"
//                     one per budget exhaustion -> loud full re-JOIN.
//   held-battle-ended "[SPEC-DEEPJOIN] OUTCOME=held-battle-ended"
//                     ONE per hold episode, on the first 1Hz scan that sees the
//                     held battle's MATCH_END queued behind us. The prose
//                     "the battle we are holding for has ENDED" line keeps
//                     repeating at 1Hz after it -- that one is the live
//                     diagnostic, this one is the countable event.
//   refused-stale     "[SPEC-DEEPJOIN] OUTCOME=refused-stale"
//                     one per discarded blob, with reason=backward (a re-ship
//                     for a frame we are already past) or reason=missed-battle
//                     (a push for a battle that started after our hold armed).
//                     Both used to be SILENT -- the only trace was the generic
//                     "discarding re-join snapshot" line, which cannot tell a
//                     deep-join refusal from any other re-join discard.
//
// The char-mismatch refusal keeps its own existing "refusing snapshot at
// anchor=" line (the 0x40CD47 AV guard); it is loud already and the analysers
// count it separately as `charguard`.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state
#include "control_channel.h"
#include "netplay.h"
#include "netplay_state.h"
#include "../core/globals.h"

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
using namespace specnode;

namespace {

// One SPEC_SNAPSHOT_REQ, sent `repeats` times. The control channel is plain
// unreliable UDP, so a DONE (which stops the host pushing) is repeated the same
// way SESSION_END is; a WANT is not, because the 1.5 s cadence already retries.
void SendSnapshotReq(uint8_t flags, uint32_t anchor, int repeats) {
    if (g_state.upstream_addr.sin_port == 0) return;
    CtrlPacket req = {};
    req.header.type = CtrlMsg::SPEC_SNAPSHOT_REQ;
    req.data.spec_snapshot_req.anchor_frame = anchor;
    req.data.spec_snapshot_req.match_index  = g_state.pb_snapshot_inbox.meta.match_index;
    req.data.spec_snapshot_req.flags        = flags;
    for (int i = 0; i < repeats; ++i) {
        ControlChannel_SendTo(req, g_state.upstream_addr);
    }
}

// Does the queue we are holding behind already contain the MATCH_END of the
// battle we are holding FOR? Diagnostic only -- see the call site.
bool QueueHasMatchEnd() {
    for (const auto& ev : g_state.pb_queue) {
        if (ev.type == SessionEventType::MATCH_END) return true;
    }
    return false;
}

// One-shot latch for the held-battle-ended OUTCOME line. The prose diagnostic
// it sits next to is deliberately 1Hz-repeating (it is the "this is still
// broken" heartbeat), which makes it useless for COUNTING the corner across a
// soak -- one wedged viewer would contribute hundreds of hits. Cleared when a
// hold arms and when one completes, so the count is "hold episodes that saw
// their battle end", which is exactly the number Phase 1.3 wants.
bool g_held_battle_ended_reported = false;

}  // namespace

namespace specnode {

bool DeepJoinEnabled() {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char* e = std::getenv("FM2K_SPEC_DEEP_JOIN");
        s_enabled = (e && e[0] == '1') ? 1 : 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SPEC-DEEPJOIN] bounded deep join %s (FM2K_SPEC_DEEP_JOIN)",
            s_enabled ? "ENABLED -- bounded backfill from the current "
                        "char-select + mandatory battle-entry snapshot"
                      : "disabled (default; from-frame-0 backfill)");
    }
    return s_enabled == 1;
}

uint32_t ConsumedInputPos() {
    size_t queued = 0;
    for (const auto& ev : g_state.pb_queue) {
        if (ev.type == SessionEventType::INPUT) ++queued;
    }
    return (g_state.next_expected_frame >= (uint32_t)queued)
               ? (uint32_t)(g_state.next_expected_frame - (uint32_t)queued)
               : 0u;
}

// ---------------------------------------------------------------------------
// HOST
// ---------------------------------------------------------------------------

void DeepJoinOnBind(Subscriber& sub) {
    if (!sub.deep_join_eligible) return;
    // Nothing has been pushed to this sub yet on the deep-join path.
    sub.deep_join_push_anchor = 0xFFFFFFFFu;
    // Arm ONLY when the host is already inside the battle this viewer is about
    // to walk into. The eligibility decision was taken at pin time, when the
    // host was NOT in battle; the bind runs a few ticks later and the host can
    // have crossed in between (StashSnapshot then fired while this sub was
    // still unbound, so its arm pass missed it). If the host is still between
    // matches, current_snapshot holds the PREVIOUS battle's blob -- exactly the
    // stale-blob hazard Wave 3 closed by keying use_snapshot on the grant -- so
    // we arm nothing and wait for the next StashSnapshot.
    const bool host_in_battle =
        Netplay_GetSessionKind() == NetplaySessionKind::BATTLE;
    sub.deep_join_pending = host_in_battle && g_state.current_snapshot.valid;
    char buf[48] = {}; FormatAddr(sub.addr, buf, sizeof(buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SPEC-DEEPJOIN] %s bound on the bounded path -- battle-entry snapshot "
        "push %s (host_in_battle=%d, cached snapshot valid=%d match=%u "
        "anchor=%u)",
        buf, sub.deep_join_pending ? "ARMED now" : "deferred to the next "
                                                   "battle entry",
        (int)host_in_battle, (int)g_state.current_snapshot.valid,
        g_state.current_snapshot.match_index,
        g_state.current_snapshot.input_frame);
}

void DeepJoinArmSubscribers() {
    size_t armed = 0;
    for (auto& sub : g_state.subscribers) {
        if (!sub.deep_join_eligible) continue;
        // RE-ARMS EVERY BATTLE ENTRY while the viewer is still uncorrected.
        // That is the rematch property: a joiner whose first battle ended
        // before its snapshot arrived is served the NEXT battle's blob with no
        // new handshake, and a viewer that joined between matches 2 and 3 is
        // served battle 3's the moment StashSnapshot refreshes the cache.
        sub.deep_join_pending = true;
        ++armed;
    }
    if (armed == 0) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SPEC-DEEPJOIN] battle entry -- armed %zu bounded deep joiner(s) for "
        "match=%u anchor INPUT-frame=%u (wire %zu bytes)",
        armed, g_state.current_snapshot.match_index,
        g_state.current_snapshot.input_frame,
        g_state.current_snapshot.wire_blob.size());
}

void DeepJoinPushTick(uint64_t now) {
    const auto& cache = g_state.current_snapshot;
    if (!cache.valid) return;
    // Only ever push the CURRENT battle's blob. The arm is set by StashSnapshot
    // (in battle, cache already refreshed) and drained on a following
    // maintenance tick, so this is normally true; if the host somehow left
    // battle in between, hold the arm rather than shipping a blob that has
    // become the PREVIOUS match's -- dropping a between-matches viewer into the
    // last match's state is a worse desync than the one this wave fixes.
    if (Netplay_GetSessionKind() != NetplaySessionKind::BATTLE) return;
    for (auto& sub : g_state.subscribers) {
        if (!sub.deep_join_pending || !sub.deep_join_eligible) continue;
        // Unbound subs keep the arm: the bind path ships their payload and
        // DeepJoinOnBind decides fresh. Pushing to an unbound sub drops the
        // bytes on the floor (no TCP socket / no RC endpoint paired yet).
        if (!sub.tcp_bound) continue;
        if (sub.deep_join_push_anchor == cache.input_frame) {
            // Already pushed THIS battle's blob. A repeat arm (e.g. a second
            // StashSnapshot for the same cache) must not re-ship it -- the
            // viewer's supervised re-request is the retry path, and it is
            // rate-limited host-side.
            sub.deep_join_pending = false;
            continue;
        }
        char buf[48] = {}; FormatAddr(sub.addr, buf, sizeof(buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SPEC-DEEPJOIN] pushing battle snapshot to %s (match=%u, anchor "
            "INPUT-frame=%u, wire %zu bytes) -- this viewer skipped prior "
            "matches' simulation and is holding at battle entry for it",
            buf, cache.match_index, cache.input_frame,
            cache.wire_blob.size());
        SendSnapshotTo(sub.addr);
        sub.deep_join_push_anchor   = cache.input_frame;
        sub.deep_join_pending       = false;
        // Share the existing re-ship bookkeeping so a rebind inside
        // SPECTATOR_SNAPSHOT_RESHIP_MS does not immediately blast the same
        // blob again on top of the chunks still in flight for this push.
        sub.last_snapshot_ship_ms   = now;
        sub.last_snapshot_match_idx = cache.match_index;
        sub.snapshot_reship_skips   = 0;
        // ONE push per maintenance tick, the same pacing the bind loop uses,
        // so N deep joiners at one battle entry spread over N ticks instead of
        // stacking into a single game frame.
        return;
    }
}

void SpectatorNode_HandleSnapshotReq_Impl(const sockaddr_in& from,
                                          uint32_t anchor_frame,
                                          uint32_t match_index,
                                          uint8_t  flags) {
    const uint64_t now = GetTickCount64();
    for (auto& sub : g_state.subscribers) {
        if (!AddrEqual(sub.addr, from)) continue;
        sub.last_seen_ms = now;
        char buf[48] = {}; FormatAddr(sub.addr, buf, sizeof(buf));
        if (flags & SPEC_SNAPREQ_DONE) {
            if (sub.deep_join_eligible) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SPEC-DEEPJOIN] %s reports the snapshot APPLIED at "
                    "anchor=%u (match=%u) -- clearing eligibility; it is a "
                    "bit-exact continuing viewer from here and gets no further "
                    "pushes", buf, anchor_frame, match_index);
            }
            sub.deep_join_eligible = false;
            sub.deep_join_pending  = false;
            return;
        }
        if (!sub.deep_join_eligible) {
            // Not a deep joiner (or already corrected). Shipping a snapshot to
            // a viewer that is bit-exact by simulation is pure risk, so refuse
            // rather than be helpful.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-DEEPJOIN] ignoring SPEC_SNAPSHOT_REQ from %s -- not a "
                "bounded deep joiner (anchor=%u)", buf, anchor_frame);
            return;
        }
        if (now - sub.deep_join_last_reply_ms < SPECTATOR_DEEPJOIN_REPLY_FLOOR_MS) {
            return;
        }
        sub.deep_join_last_reply_ms = now;
        const auto& cache = g_state.current_snapshot;
        if (cache.valid && cache.input_frame == anchor_frame) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-DEEPJOIN] re-shipping snapshot to %s on request "
                "(anchor=%u match=%u wire %zu bytes) -- chunk offsets are a "
                "pure function of the cached blob, so this RESUMES the "
                "viewer's partial transfer rather than restarting it",
                buf, cache.input_frame, cache.match_index,
                cache.wire_blob.size());
            SendSnapshotTo(sub.addr);
            sub.deep_join_push_anchor   = cache.input_frame;
            sub.deep_join_pending       = false;
            sub.last_snapshot_ship_ms   = now;
            sub.last_snapshot_match_idx = cache.match_index;
            sub.snapshot_reship_skips   = 0;
        } else {
            // We do not hold the battle they are waiting on: either we have not
            // entered it yet, or we have moved on. Shipping what we DO hold
            // would be the stale blob, which is the exact hazard Wave 3 closed
            // by keying use_snapshot on the grant -- so ship NOTHING, and do
            // NOT arm the push either (the pacer cannot tell a fresh cache from
            // a stale one, it would ship this same blob next tick). The per-
            // battle arm in DeepJoinArmSubscribers is the recovery: it fires
            // from StashSnapshot with the cache already refreshed, so it can
            // only ever offer the CURRENT battle.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-DEEPJOIN] %s asked for anchor=%u but our cache is "
                "%s(valid=%d anchor=%u match=%u) -- NOT shipping a stale blob; "
                "the next battle entry's arm serves them",
                buf, anchor_frame, cache.valid ? "" : "empty ",
                (int)cache.valid, cache.input_frame, cache.match_index);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// VIEWER
// ---------------------------------------------------------------------------

bool DeepJoinWalkingToAnchor() {
    return g_state.spec_deep_join && !g_state.pb_deep_join_await;
}

DeepJoinVerdict DeepJoinSnapshotVerdict(const State::SnapshotInbox& inbox) {
    const uint32_t consumed = ConsumedInputPos();

    // THE RULE. anchor == consumed is true exactly when our sim sits at the
    // snapshot's logical frame having consumed nothing past it. It refuses
    // backward rewinds (anchor < consumed -- the 2026-06-11 "battle restarted
    // mid-stream") outright, and turns forward anchors into a WAIT or a REFUSE
    // depending on whether we can still reach them, so the 0x40CD47
    // forward-jump AV is unreachable either way. It is STRICTLY NARROWER than
    // the !pb_started proxy it stands in for on this path, never wider.
    if (inbox.anchor_frame < consumed) {
        // Was SILENT. The caller discards the inbox right after this, so the
        // only trace of a backward re-ship was the generic "discarding re-join
        // snapshot" line, which every other join shape also produces -- a soak
        // could not tell "the host re-shipped an old blob at me" from an
        // ordinary re-join discard. One line per discarded blob.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SPEC-DEEPJOIN] OUTCOME=refused-stale reason=backward anchor=%u "
            "consumed=%u match=%u hold=%d -- this blob is for a frame we are "
            "already past; applying it would rewind the sim under a live-edge "
            "queue. Discarding; the per-battle re-arm serves us the current one",
            inbox.anchor_frame, consumed, inbox.meta.match_index,
            (int)g_state.pb_deep_join_await);
        return DeepJoinVerdict::REFUSE;
    }
    if (inbox.anchor_frame > consumed) {
        // Ahead of us. If we are still mirroring the anchored char-select, this
        // is simply the snapshot for the battle we are walking INTO: the host
        // pushes at ITS battle entry, which is normally a second or two before
        // our engine finishes the pin walk. Keep it and re-check next tick --
        // consuming the queued CSS INPUTs is what closes the gap.
        //
        // If we are ALREADY held at a battle entry, a forward anchor means the
        // host has moved on to a battle we missed entirely. It can never become
        // applicable, so refuse it rather than wedge the inbox on it.
        if (!g_state.pb_deep_join_await) return DeepJoinVerdict::WAIT;
        // Was SILENT, and this is the interesting half of the pair: it is the
        // exact fingerprint of the held-battle-ended corner (our hold is on
        // battle N, the blob is battle N+1's). Counting it is how Phase 1.3
        // decides whether the self-healing variant is needed at all.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SPEC-DEEPJOIN] OUTCOME=refused-stale reason=missed-battle "
            "anchor=%u consumed=%u match=%u -- the host has entered a battle we "
            "never reached; our sim is frozen at an earlier battle's frame 0 and "
            "can never consume forward to this anchor. Discarding; the hold "
            "stays armed and the escalation ladder keeps working",
            inbox.anchor_frame, consumed, inbox.meta.match_index);
        return DeepJoinVerdict::REFUSE;
    }

    // anchor == consumed. The phase gate downstream would hold this anyway;
    // report WAIT so the caller keeps the inbox instead of running the char
    // check while the engine is still mid-pin-walk (chars are not locked yet at
    // that point and a spurious REFUSE would throw away a good snapshot).
    const uint32_t game_mode  = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    const uint32_t captured   = inbox.meta.captured_game_mode;
    const uint32_t apply_gate = (captured == 0u) ? 3000u : captured;
    if (game_mode < apply_gate) return DeepJoinVerdict::WAIT;

    // STALENESS GATE (Wave 4.1). pb_p1_char / pb_p2_char are written when a
    // MATCH_START DRAINS, and before the first one drains they are still 0 --
    // which is a real character index, not a sentinel. Comparing against them
    // early failed in BOTH directions in the Wave 4 round:
    //   * stale-vs-real: engine 10/13 against announced 0/0 -> spurious REFUSE,
    //     which dropped a perfectly good inbox and cost 1.8-3.3 s of hold plus
    //     an extra wire blob before it self-healed (2 of 6 experiment runs);
    //   * stale-vs-stale: engine 0/0 against announced 0/0 -> PASSED, and the
    //     snapshot applied over a from-scratch battle with zero CRC agreement
    //     (seed 75). The guard "matched" only because both sides were unset.
    //
    // pb_awaiting_match_end is exactly "a MATCH_START has drained and its
    // MATCH_END has not", so requiring it makes pb_p{1,2}_char the announced
    // chars of SOME battle. Combined with the anchor equality already proven
    // above, it makes them the announced chars of THIS battle: consumed ==
    // anchor means the queue head is the first INPUT the host appended after
    // MATCH_START(N), so everything up to and including MATCH_START(N) has
    // drained and MATCH_START(N+1) sits behind battle N's inputs, undrained.
    // That is what makes a REMATCH deep join compare against battle N+1's
    // chars rather than the previous match's.
    //
    // WAIT, not REFUSE: the inbox is kept and re-checked next tick, so the
    // apply lands the instant MATCH_START drains instead of costing a re-ship.
    // Deadlock-free -- this branch is only reachable while
    // !pb_deep_join_await (DeepJoinShouldHold refuses to arm the hold without
    // pb_awaiting_match_end), which is exactly when DeepJoinWalkingToAnchor
    // exempts the parked snapshot from freezing the sim.
    if (!g_state.pb_awaiting_match_end) {
        static uint64_t s_stale_log_ms = 0;
        const uint64_t now_ms = GetTickCount64();
        if (now_ms - s_stale_log_ms >= 1000) {
            s_stale_log_ms = now_ms;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-DEEPJOIN] snapshot at anchor=%u is at our cursor but "
                "this battle's MATCH_START has not drained yet -- WAITING "
                "rather than comparing chars against stale values (a stale "
                "0/0-vs-0/0 comparison is how a snapshot once applied over a "
                "from-scratch battle)", inbox.anchor_frame);
        }
        return DeepJoinVerdict::WAIT;
    }

    // The AV guard, asserted rather than trusted, and now against the CURRENT
    // battle's announced chars. An apply that overwrites the char slots while
    // the locally loaded .player files are a different pair of characters makes
    // the sprite renderer read mismatched image descriptors and AV at 0x40CD47.
    // The local engine reached battle through the MATCH_START pin, so these
    // SHOULD agree -- refuse loudly if they do not rather than load. Wave 4
    // proved this is a real detector, not a nuisance: it fired on 6/7 runs and
    // was RIGHT every time (char-0 .player files really were loaded, because of
    // the CssAutoConfirm hijack Wave 4.1 removes).
    if constexpr (!FM2K::kIsFM95) {
        const uint32_t p1 = *(const uint32_t*)FM2K::ADDR_P1_SELECTED_CHAR;
        const uint32_t p2 = *(const uint32_t*)FM2K::ADDR_P2_SELECTED_CHAR;
        if (p1 != (uint32_t)g_state.pb_p1_char ||
            p2 != (uint32_t)g_state.pb_p2_char) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-DEEPJOIN] refusing snapshot at anchor=%u -- engine has "
                "chars p1=%u p2=%u but THIS battle's drained MATCH_START "
                "announced p1=%u p2=%u. Applying over mismatched .player files "
                "is the 0x40CD47 sprite-renderer AV; holding for a matching "
                "snapshot instead",
                inbox.anchor_frame, p1, p2,
                (unsigned)g_state.pb_p1_char, (unsigned)g_state.pb_p2_char);
            return DeepJoinVerdict::REFUSE;
        }
    }
    return DeepJoinVerdict::APPLY;
}

bool DeepJoinShouldHold() {
    if (!g_state.spec_deep_join) {
        g_state.pb_deep_join_await = false;
        return false;
    }
    // Belt: the per-battle applied flag. DeepJoinOnSnapshotApplied clears
    // spec_deep_join above, so reaching here with it set means the apply came
    // from somewhere else entirely -- do not hold on top of it.
    if (g_state.pb_snapshot_applied_once) {
        g_state.pb_deep_join_await = false;
        return false;
    }
    const uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (mode < 3000u || mode >= 4000u) {
        // Still walking title / char-select. The bounded joiner mirrors that
        // live exactly as it does today -- the hold is a BATTLE-entry hold, not
        // a join hold, and CSS costs nothing to reproduce by simulation.
        g_state.pb_deep_join_await = false;
        return false;
    }
    // The queue head must actually BE this battle's first INPUT, or "consumed"
    // is not the host's snapshot anchor and holding would wedge the drain.
    // pb_awaiting_match_end is set at MATCH_START apply and cleared at
    // MATCH_END apply, so it is exactly "a MATCH_START has drained and its
    // battle is the thing in front of us"; pb_battle_align_pending is the
    // sibling hold that parks those battle INPUTs while the engine finishes the
    // pin walk, and it always clears on the same tick the engine crosses 3000.
    // In the pathological ordering where the engine reaches battle BEFORE
    // MATCH_START drains, we deliberately do NOT hold: freezing there would
    // strand MATCH_START behind a queue nothing can drain.
    if (!g_state.pb_awaiting_match_end || g_state.pb_battle_align_pending) {
        g_state.pb_deep_join_await = false;
        return false;
    }
    if (!g_state.pb_deep_join_await) {
        g_state.pb_deep_join_await         = true;
        g_state.pb_deep_join_anchor        = ConsumedInputPos();
        g_state.pb_deep_join_hold_since_ms = GetTickCount64();
        g_state.pb_deep_join_last_req_ms   = g_state.pb_deep_join_hold_since_ms;
        g_state.pb_deep_join_last_bytes    = 0;
        g_state.pb_deep_join_reqs          = 0;
        g_state.pb_deep_join_scan_ms       = g_state.pb_deep_join_hold_since_ms;
        g_held_battle_ended_reported       = false;  // per-episode telemetry
        const auto& inbox = g_state.pb_snapshot_inbox;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SPEC-DEEPJOIN] HOLDING at battle entry -- consumed "
            "INPUT-frame=%u, q=%zu, snapshot inbox active=%d %zu/%u B "
            "pending_apply=%d. This viewer skipped prior matches' simulation, "
            "so entering from scratch here is the Wave 3.1 desync (spawn-X "
            "characters with matching rng/hp/scripts). The sim does not "
            "advance until the host's battle savestate applies",
            g_state.pb_deep_join_anchor, g_state.pb_queue.size(),
            (int)inbox.active, inbox.bytes_received,
            (unsigned)inbox.meta.total_bytes, (int)inbox.pending_apply);
    }
    return true;
}

void DeepJoinHoldTick(uint64_t now) {
    if (!g_state.pb_deep_join_await) return;
    auto& inbox = g_state.pb_snapshot_inbox;

    // Diagnosis, 1 Hz: the host FINISHED the battle we are holding for. Our sim
    // is frozen at that battle's frame 0, so the queued battle INPUTs -- and
    // the MATCH_END behind them -- can never drain, and the next battle's
    // snapshot will carry an anchor ahead of our consumed position, which the
    // applicability rule correctly refuses. Nothing here can repair that: the
    // local engine is inside a battle it would have to SIMULATE to leave, and
    // simulating it is exactly the desync this hold exists to prevent. Make it
    // loud so validation can see the corner; the escalation ladder below keeps
    // trying, and a re-JOIN landing while the host is still in this battle does
    // recover it.
    //
    // ABOVE THE PROGRESS GATE, deliberately (Wave 4.1). It used to sit below,
    // where the `bytes_received changed -> return` fast path suppressed it on
    // most ticks: under loss a ~35 KB blob is arriving on nearly every tick, so
    // the detector for the one unrecoverable corner was structurally silenced
    // exactly while that corner was being approached, and a validation round
    // reporting "0 has ENDED" could not distinguish "did not happen" from
    // "cannot fire". The scan is its own 1 Hz throttle and touches no
    // supervision state, so hoisting it changes nothing else.
    if (now - g_state.pb_deep_join_scan_ms >= 1000) {
        g_state.pb_deep_join_scan_ms = now;
        if (QueueHasMatchEnd()) {
            // COUNTABLE EVENT, one per hold episode -- see the telemetry
            // contract at the top of this file. Emitted BEFORE the prose line
            // below so a log reader meets the summary first; the prose line's
            // 1Hz cadence (and its exact wording, which the analysers grep as
            // `ended`) is untouched.
            if (!g_held_battle_ended_reported) {
                g_held_battle_ended_reported = true;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "[SPEC-DEEPJOIN] OUTCOME=held-battle-ended anchor=%u "
                    "held=%llums reqs=%u escalations=%u q=%zu inbox=%zu/%u B",
                    g_state.pb_deep_join_anchor,
                    (unsigned long long)(now - g_state.pb_deep_join_hold_since_ms),
                    g_state.pb_deep_join_reqs, g_state.pb_deep_join_escalations,
                    g_state.pb_queue.size(), inbox.bytes_received,
                    (unsigned)inbox.meta.total_bytes);
            }
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-DEEPJOIN] the battle we are holding for has ENDED on the "
                "host (MATCH_END is queued behind %zu event(s) we cannot drain) "
                "and no snapshot arrived in %llums. This viewer cannot resync "
                "without a restart -- the local engine is inside a battle it "
                "would have to simulate to leave. Escalating anyway",
                g_state.pb_queue.size(),
                (unsigned long long)(now - g_state.pb_deep_join_hold_since_ms));
        }
    }

    // Progress gate: a transfer that is still growing needs no prod. Any new
    // covered byte resets the request interval, so a merely slow snapshot under
    // loss rides its RC retransmits out instead of provoking a duplicate ship.
    if (inbox.active || inbox.pending_apply) {
        if (inbox.bytes_received != g_state.pb_deep_join_last_bytes) {
            g_state.pb_deep_join_last_bytes  = inbox.bytes_received;
            g_state.pb_deep_join_last_req_ms = now;
            return;
        }
    }

    if (now - g_state.pb_deep_join_last_req_ms < SPECTATOR_DEEPJOIN_REQ_INTERVAL_MS) {
        return;
    }
    g_state.pb_deep_join_last_req_ms = now;

    if (now - g_state.pb_deep_join_hold_since_ms < SPECTATOR_DEEPJOIN_BUDGET_MS) {
        ++g_state.pb_deep_join_reqs;
        SendSnapshotReq(SPEC_SNAPREQ_WANT, g_state.pb_deep_join_anchor, 1);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SPEC-DEEPJOIN] re-requesting the battle snapshot (#%u, anchor=%u, "
            "held %llums, inbox %zu/%u B) -- one datagram, not a re-JOIN",
            g_state.pb_deep_join_reqs, g_state.pb_deep_join_anchor,
            (unsigned long long)(now - g_state.pb_deep_join_hold_since_ms),
            inbox.bytes_received, (unsigned)inbox.meta.total_bytes);
        return;
    }

    // Budget exhausted -> LOUD failure, then a full re-JOIN. Never a silent
    // release into the from-scratch battle path.
    //
    // The re-JOIN is the real repair, not a gesture: it goes out with
    // SPEC_JOIN_RESUME suppressed (pb_deep_join_force_full_rejoin), so the host
    // takes its destructive-reset branch and RE-PINS. With the host inside the
    // battle we are held at, that re-pin yields a BATTLE grant, the bind's
    // use_snapshot fires, and the snapshot arrives through the ordinary
    // mid-battle join path -- whose anchor is exactly our consumed position,
    // so the applicability rule admits it and the hold releases. A
    // resume-flagged JOIN_REQ would instead take the light-resume bind, which
    // explicitly ships NO snapshot: that is why the suppression exists.
    const uint32_t reqs_this_round = g_state.pb_deep_join_reqs;
    ++g_state.pb_deep_join_escalations;
    g_state.pb_deep_join_hold_since_ms = now;   // fresh budget for the next round
    g_state.pb_deep_join_reqs          = 0;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[SPEC-DEEPJOIN] hold budget %llums EXHAUSTED at anchor=%u after %u "
        "request(s) (inbox active=%d %zu/%u B) -- escalation #%u: full re-JOIN "
        "with SPEC_JOIN_RESUME suppressed so the host re-pins and re-ships. "
        "Still holding; this path never falls back to from-scratch",
        (unsigned long long)SPECTATOR_DEEPJOIN_BUDGET_MS,
        g_state.pb_deep_join_anchor, reqs_this_round,
        (int)inbox.active, inbox.bytes_received,
        (unsigned)inbox.meta.total_bytes, g_state.pb_deep_join_escalations);
    if (g_state.root_addr.sin_port != 0 && !g_state.session_ended) {
        g_state.pb_deep_join_force_full_rejoin = true;
        g_state.last_reconnect_attempt_ms      = now;
        SpectatorNode_RequestJoin(g_state.root_addr,
                                  SpecJoinMode::CURRENT_MATCH);
    }
}

void DeepJoinOnSnapshotApplied(uint32_t anchor) {
    if (!g_state.spec_deep_join && !g_state.pb_deep_join_await) return;
    // 0 when the healthy path applied before the hold ever had to arm -- the
    // host pushes at its own battle entry, so the blob is usually already
    // parked (verdict WAIT) by the time our engine crosses into battle.
    const uint64_t held = (g_state.pb_deep_join_hold_since_ms == 0)
        ? 0u
        : (GetTickCount64() - g_state.pb_deep_join_hold_since_ms);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "[SPEC-DEEPJOIN] snapshot APPLIED at anchor=%u after %llums held / %u "
        "request(s) / %u escalation(s) -- deep join COMPLETE. From here this "
        "viewer is bit-exact and continues by simulation exactly like a "
        "spectator crossing a rematch seam",
        anchor, (unsigned long long)held, g_state.pb_deep_join_reqs,
        g_state.pb_deep_join_escalations);
    g_state.pb_deep_join_await = false;
    g_state.spec_deep_join     = false;
    g_held_battle_ended_reported = false;
    // Tell the host to stop pushing. Repeated 3x like SESSION_END because the
    // control channel is unreliable. If all three are lost the host keeps
    // pushing one snapshot per battle entry to a viewer that no longer needs
    // one -- bounded (~one wire blob per battle) and harmless, because
    // pb_deep_join_await is now false so ApplyPendingSnapshot's re-join discard
    // refuses every one of them.
    SendSnapshotReq(SPEC_SNAPREQ_DONE, anchor, 3);
}

}  // namespace specnode

// Public dispatch entry (netplay_control.cpp). Thin forwarder so the
// implementation can live inside `namespace specnode` with its siblings.
void SpectatorNode_HandleSnapshotReq(const sockaddr_in& from,
                                     uint32_t anchor_frame,
                                     uint32_t match_index,
                                     uint8_t  flags) {
    specnode::SpectatorNode_HandleSnapshotReq_Impl(from, anchor_frame,
                                                   match_index, flags);
}
