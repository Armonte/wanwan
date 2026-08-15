// spec_pool_sync.cpp -- MATCH-START POOL RESYNC (campaign Phase 4c).
//
// WHAT THIS FIXES, measured before it was written (seam_p4b_diagnosis.md):
// a spectator's object pool is INDEX-INCOHERENT with the host's. The pool
// topology digest (`top=` on [CHECKSUM]: slot index, type, owner, player slot,
// entity kind, creator link) differed host-vs-spectator on 100% of battle
// frames of EVERY match of EVERY run, on BOTH games, including matches that are
// bit-identical by every other measure -- while two independent PLAYER
// processes agreed on it 100% of the time. On wanwan the divergence was one
// object's creator back-pointer; on vanguard-princess from match 2 on it was a
// whole-pool re-index (the type-14 root object sitting at a different slot,
// swapped with a type-4).
//
// Why that is not cosmetic: create_game_object is a pure first-free ascending
// scan and update_game_state walks slots 0..1023 in INDEX order, so update and
// hit-resolution ORDER is a pure function of slot index. The re-index is an
// ENABLING condition -- most matches survive it, and then one does not (a third
// object's hit landing on a different frame: p1.hp -3 with
// hurt_box_active_count == 0). The early warning is `nobj=`: 0 mismatches
// across 22094 clean paired frames, 530 in the 1230 frames BEFORE the first
// fighter-visible divergence in the match that broke.
//
// THE ROOT CAUSE is structural, not a bug in any one function. A spectator
// receives an authoritative SaveState_Load snapshot ONCE, at join. Everything
// after that is pure input replay onto a pool whose slot assignment was never
// guaranteed to match the host's -- and cannot be, because slot assignment is a
// function of the RESIDUE the previous match, the title walk and the char
// select left in the pool, and the spectator's walk through those is its own.
//
// THE FIX: make the pool authoritative at EVERY match boundary instead of only
// at join. The host already captures a fresh battle-entry snapshot at every
// Netplay_StartBattle (SpectatorNode_StashSnapshot) and already has a per-battle
// arm/push pacer (spec_deep_join.cpp) -- it just refuses to serve anyone except
// an uncorrected bounded deep joiner. This TU widens that audience to every
// bound subscriber and gives the viewer a bounded battle-entry hold so the blob
// has a window in which `anchor == consumed` is still true.
//
// SCOPE -- NO ENGINE WRITE ON A PLAYER PROCESS, BY CONSTRUCTION, NOT BY HOPE.
//   * The VIEWER half is unreachable from a player process: PoolSync_Active()
//     requires g_state.upstream_addr.sin_port != 0, i.e. this process has
//     SUBSCRIBED to an upstream, which only SpectatorNode_RequestJoin (and its
//     HandleJoinAck partner) ever sets, both viewer-only.
//   * The HOST half IS reachable from a player process, and that is the point:
//     PoolSync_HostServes() -- which lives in this TU -- runs on the host
//     player process and changes its behaviour at three spec_deep_join.cpp
//     sites (arm / push / SPEC_SNAPSHOT_REQ). It reads the environment,
//     allocates a std::string on its first call and emits one log line. Do NOT
//     read the older, overbroad wording of this paragraph ("nothing here is
//     reachable from a player process") as licence to add player-side work
//     here: what is actually guaranteed is the next bullet.
//   * The apply itself is SaveState_LoadFromBytes -> SaveState_Load with
//     g_player_index == 2 (is_spec_apply), the same cross-process carve-out
//     path every spectator join has always used. No netplay/rollback code path
//     changes; no engine memory is written on a player process.
//   * The host-side half only widens WHO gets an existing per-battle push. It
//     writes nothing to the host's own engine state.
//
// DETERMINISM (the Phase-2 lesson: no schedule-dependent writes). The apply is
// gated on DeepJoinSnapshotVerdict == APPLY, whose whole content is
// `snapshot anchor == our consumed INPUT position` plus a phase gate, a
// MATCH_START-drained gate and a character-identity gate. All four are pure
// functions of the event stream and of engine mode -- none of them is a
// function of a rollback schedule (spectators never roll back at all). The
// applied bytes are the host's, so the result is the same state whichever tick
// the blob happened to land on: either it lands inside the window and the pool
// becomes the host's, or it does not and the viewer continues exactly as it
// does today. There is no third outcome and no partial apply.
//
// THE +0x17A CLAIM, VERIFIED IN CODE RATHER THAN INHERITED. The spectator apply
// path preserves LIVE heap-shaped DWORDs (>= HEAP_PTR_FLOOR 0x01000000) across
// the object-pool memcpy (savestate_fm2k_load.cpp:340-370). Every field the
// topology digest reads is BELOW that floor and therefore comes from the
// snapshot bytes:
//   * parent_object_ptr @ +0x17A is a KgtRuntimeObject* into the SAME pool, so
//     its value range is 0x4701E0 .. 0x4701E0 + 1023*382 = 0x4CF7A2 -- three
//     bits below the floor for every possible slot.
//   * type/+0, owner/+4, playerSlotId/+0x156, entityKind/+0x15A are small
//     integers.
//   * +0x17A..+0x17D is the last dword of the 382-byte slot, i.e. inside the
//     `dst + OVERRIDE_END .. OBJ_SZ` copy and outside the +68..+84 colour
//     carve-out, so it is copied at all.
// The object LIST topology (0x430240 heads/tails, the 0x4CFA20 node pool,
// g_current_object_ptr) is restored by straight memcpy and holds only static
// .data addresses, which are identical across processes.
//
// FAILURE DIRECTION. Every branch here fails toward TODAY'S BEHAVIOUR: a blob
// that is late, lost, or refused costs one match's pool repair and nothing
// else. The hold is bounded (kHoldBudgetMs) and releases into ordinary
// playback; it can never wedge a viewer the way the deep-join hold deliberately
// can, because a deep joiner without its snapshot is WRONG while a continuing
// viewer without one is merely UNREPAIRED.
//
// TELEMETRY: one line per match boundary at most (arm, apply, or timeout).
// Hook logging is synchronous -- nothing here may ever log per frame.
#include "spec_pool_sync.h"
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state
#include "control_channel.h"
#include "../core/globals.h"

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <windows.h>
#include <cctype>
#include <cstdlib>
#include <string>

namespace specnode {

namespace {

// Battle-entry hold budget. Generous enough for a ~45 KB wire blob under the
// harness's 20% loss profile (the deep-join path measures 0 ms held on a
// healthy link -- the host pushes at ITS battle entry, a second or more before
// a lagging viewer finishes its own CSS walk), short enough that a lost push
// costs a visible stutter rather than a stall.
constexpr uint64_t kHoldBudgetMs   = 2500;
// One nudge per interval while holding. The push is already reliable-channel
// bound; this covers the "the arm fired while we were momentarily unbound" and
// "the whole push was lost" cases without ever becoming a flood.
constexpr uint64_t kReqIntervalMs  = 900;

bool     s_applied_this_match  = false;
bool     s_expired_this_match  = false;
bool     s_any_snapshot_landed = false;   // session latch: a join snapshot applied
bool     s_holding             = false;
uint64_t s_hold_since_ms       = 0;
uint64_t s_hold_last_req_ms    = 0;
uint32_t s_hold_anchor         = 0;
uint32_t s_match_seq           = 0;
uint32_t s_synced              = 0;
uint32_t s_missed              = 0;
// Anchor of the last blob that actually applied, +1 (0 = none yet). Guards the
// duplicate-apply the first 4c run showed: the join snapshot applied at anchor
// 958, a MATCH_START then drained at the SAME input position (nothing had been
// consumed), the hold armed, and the host re-shipped the identical blob 1.2 s
// later for a second, entirely redundant apply. Same anchor means we already
// hold that exact authoritative state, so there is nothing to wait for.
uint32_t s_applied_anchor_p1   = 0;
// PHASE 4e (review A2.1). Latched by a MID-STREAM re-JOIN, cleared by the next
// successful apply. While it is set, the two pre-4c safety behaviours this
// feature exempts -- the placeholder CSS drive arm (spec_recv.cpp) and the
// parked-snapshot freeze (spec_playback.cpp) -- stay in force for this viewer.
// Reason: 4c's suppression is correct for a viewer that reaches battle by its
// OWN mirrored char-select, and every run that validated it was of exactly that
// shape. A viewer whose TCP died and whose stream was re-anchored past the CSS
// has no char-select left to mirror, so suppressing BOTH behaviours would leave
// it with no route to mode >= 3000 at all -- the population spec_recv.cpp's own
// comment names as "entirely untested". This latch keeps that population on the
// pre-4c path for exactly one boundary and then rejoins the 4c behaviour.
bool     s_rejoin_pending      = false;

// One SPEC_SNAPSHOT_REQ WANT. Mirrors spec_deep_join.cpp's SendSnapshotReq --
// duplicated rather than exported because that one is deliberately file-local
// to the deep-join ladder and its DONE variant must stay there.
void SendWant(uint32_t anchor) {
    if (g_state.upstream_addr.sin_port == 0) return;
    CtrlPacket req = {};
    req.header.type = CtrlMsg::SPEC_SNAPSHOT_REQ;
    req.data.spec_snapshot_req.anchor_frame = anchor;
    req.data.spec_snapshot_req.match_index  = g_state.pb_snapshot_inbox.meta.match_index;
    req.data.spec_snapshot_req.flags        = SPEC_SNAPREQ_WANT;
    ControlChannel_SendTo(req, g_state.upstream_addr);
}

}  // namespace

// FM2K_SPEC_POOL_SYNC -- DEFAULT ON, kill-switch semantics and strict parse,
// deliberately identical in shape to DeepJoinEnabled(): the only load-bearing
// direction is OFF (it restores the pre-4c behaviour exactly, which is what
// makes "does this reproduce with the pool sync off?" a real triage question
// and what the before/after topology measurement is taken against). An
// unrecognised value fails SAFE to OFF and says so.
bool PoolSync_Enabled() {
    static int s_enabled = -1;
    if (s_enabled >= 0) return s_enabled == 1;

    const char* raw = std::getenv("FM2K_SPEC_POOL_SYNC");
    std::string v = raw ? raw : "";
    const size_t b = v.find_first_not_of(" \t\r\n");
    const size_t e = v.find_last_not_of(" \t\r\n");
    v = (b == std::string::npos) ? std::string() : v.substr(b, e - b + 1);
    for (char& c : v) c = (char)std::tolower((unsigned char)c);

    const char* why = nullptr;
    if (v.empty()) {
        s_enabled = 1;
        why = "default; FM2K_SPEC_POOL_SYNC unset";
    } else if (v == "0" || v == "false" || v == "off" || v == "no" ||
               v == "disabled") {
        s_enabled = 0;
        why = "FM2K_SPEC_POOL_SYNC kill-switch";
    } else if (v == "1" || v == "true" || v == "on" || v == "yes" ||
               v == "enabled") {
        s_enabled = 1;
        why = "FM2K_SPEC_POOL_SYNC set explicitly";
    } else {
        s_enabled = 0;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[POOLSYNC] match-start pool resync disabled -- "
            "FM2K_SPEC_POOL_SYNC=\"%s\" is not a recognised value (accepted: "
            "1/true/on/yes/enabled, 0/false/off/no/disabled). Failing SAFE to "
            "the pre-4c behaviour; UNSET the variable to get the default (ON)",
            raw ? raw : "");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[POOLSYNC] match-start pool resync %s (%s)",
        s_enabled ? "ENABLED -- the host's battle-entry snapshot is re-applied "
                    "at every match boundary so the object pool stays "
                    "index-coherent with the host"
                  : "disabled -- pool indices diverge from the host's after "
                    "the join snapshot (pre-4c behaviour)",
        why);
    return s_enabled == 1;
}

bool PoolSync_HostServes() { return PoolSync_Enabled(); }

bool PoolSync_Active() {
    if (!PoolSync_Enabled()) return false;
    // Live spectator only. Offline replay (spec_session_file.cpp) never
    // subscribes, so this is also the offline-replay exclusion -- there is no
    // upstream to push a blob and nothing to hold for.
    if (g_state.upstream_addr.sin_port == 0) return false;
    // A bounded deep joiner owns the battle-entry boundary itself (its hold
    // must NOT be bounded -- entering from scratch is wrong for it, merely
    // unrepaired for us). Stand aside until its snapshot lands, at which point
    // spec_deep_join clears and it becomes an ordinary continuing viewer.
    if (g_state.spec_deep_join) return false;
    // Do not race a JOIN snapshot that is still the thing in flight. Two
    // disjoint populations reach playback:
    //   * natural_boot viewers (FULL_SESSION from frame 0, and deep joiners
    //     after their apply) never receive a join snapshot at all, so there is
    //     nothing to race and they are active from their very first match --
    //     which matters, because their match 1 is exactly as index-incoherent
    //     as every later one (wanwan control: 5320/5320 frames);
    //   * CURRENT_MATCH snapshot joiners become active only once a snapshot has
    //     actually applied (s_any_snapshot_landed), i.e. once the join flow is
    //     finished. Before that, the join's own hold/apply machinery is in
    //     charge and must not be exempted from anything.
    return g_state.natural_boot || s_any_snapshot_landed;
}

bool PoolSync_Armed() {
    return PoolSync_Active() && !s_applied_this_match && !s_expired_this_match;
}

// PHASE 4e (review A2.1). The predicate the two pre-4c EXEMPTIONS key on. It is
// PoolSync_Active() narrowed by the re-JOIN latch above, and it is deliberately
// NOT the same call as PoolSync_Armed(): the exemptions are about "can this
// viewer walk its own char-select", not about "is this match's blob still
// outstanding".
bool PoolSync_SuppressLegacyCssFallback() {
    return PoolSync_Active() && !s_rejoin_pending;
}

void PoolSync_OnViewerRejoin() {
    if (!PoolSync_Enabled()) return;
    if (s_rejoin_pending) return;
    s_rejoin_pending = true;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "[POOLSYNC] mid-stream re-JOIN -- restoring the pre-4c placeholder CSS "
        "drive and the parked-snapshot freeze for this viewer until its next "
        "snapshot applies. A re-anchored stream carries no char-select for us "
        "to mirror, so the 4c suppressions would leave no route to game_mode "
        "3000 at all");
}

// PHASE 4e (review A3.2). True while THIS viewer is parked in the bounded
// battle-entry hold. spec_deep_join.cpp's DeepJoinSnapshotVerdict uses it for
// exactly what pb_deep_join_await does for a deep joiner: a forward anchor that
// arrives while we are already held is a battle we can never reach.
bool PoolSync_Holding() { return s_holding; }

// PHASE 4e (review A3.2). The parked blob we were holding for has been
// superseded by a LATER battle's, so the repair we are freezing for no longer
// exists. Release now instead of burning the remaining budget: every millisecond
// past this point is frozen video bought for nothing, and on a lagging viewer
// the freeze is what makes the lag worse.
void PoolSync_OnMissedBattle(uint32_t anchor, uint32_t consumed) {
    if (!s_holding) return;
    s_holding            = false;
    s_expired_this_match = true;
    ++s_missed;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "[POOLSYNC] OUTCOME=missed-battle match #%u hold-anchor=%u consumed=%u "
        "blob-anchor=%u -- the host has moved on to a battle we never reached, "
        "so this hold can never be satisfied. Releasing IMMEDIATELY into "
        "ordinary playback rather than at the %llums budget; totals synced=%u "
        "missed=%u",
        s_match_seq, s_hold_anchor, consumed, anchor,
        (unsigned long long)kHoldBudgetMs, s_synced, s_missed);
}

namespace {
// NOTE (review A3.3): there is deliberately no PoolSync_Shutdown /
// PoolSync_OnSessionEnd, unlike every sibling module. The session-level latches
// (s_any_snapshot_landed, s_applied_anchor_p1, s_rejoin_pending, the counters)
// are correct today only because SpectatorNode_HandleSessionEnd is TERMINAL --
// no in-process session restart exists. If one is ever added, these MUST be
// reset with it; the same class already bit us once (5580a1d:
// SpectatorNode_Shutdown cleared session_events but never the anchor fields).
void ArmForNextMatch() {
    s_applied_this_match = false;
    s_expired_this_match = false;
    s_holding            = false;
    s_hold_since_ms      = 0;
    s_hold_last_req_ms   = 0;
    s_hold_anchor        = 0;
}
}  // namespace

void PoolSync_OnMatchEnd()   { ArmForNextMatch(); }
void PoolSync_OnMatchStart() { ++s_match_seq; ArmForNextMatch(); }

bool PoolSync_ShouldHold() {
    if (!PoolSync_Armed()) { s_holding = false; return false; }

    // Same battle-entry predicate the deep-join hold uses, and for the same
    // reasons (spec_deep_join.cpp DeepJoinShouldHold):
    //   * mode in [3000,4000): the engine has crossed into THIS match's battle,
    //     so the snapshot's phase gate can be satisfied;
    //   * pb_awaiting_match_end: a MATCH_START has drained and its MATCH_END
    //     has not, which is what makes "consumed" the host's snapshot anchor
    //     and what makes the announced characters THIS battle's;
    //   * !pb_battle_align_pending: the sibling hold that parks battle inputs
    //     during the pin walk has released, so the cursor is settled;
    //   * pb_boundary == NONE: the rematch seam state machine has finished
    //     (PINNING -> NONE fires on the same battle crossing).
    // Callers reach here only after SeamStep returned FALL_THROUGH, so the last
    // two are normally already true; they are asserted rather than assumed.
    const uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (mode < 3000u || mode >= 4000u ||
        !g_state.pb_awaiting_match_end ||
        g_state.pb_battle_align_pending ||
        g_state.pb_boundary != State::PbBoundary::NONE) {
        s_holding = false;
        return false;
    }

    const uint64_t now = GetTickCount64();
    // Already authoritative at this exact anchor (see s_applied_anchor_p1).
    const uint32_t consumed_now = ConsumedInputPos();
    if (s_applied_anchor_p1 == consumed_now + 1u) {
        s_applied_this_match = true;
        s_holding            = false;
        return false;
    }
    if (!s_holding) {
        s_holding          = true;
        s_hold_since_ms    = now;
        s_hold_last_req_ms = now;
        s_hold_anchor      = ConsumedInputPos();
        const auto& inbox  = g_state.pb_snapshot_inbox;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[POOLSYNC] holding at battle entry for match #%u -- consumed "
            "INPUT-frame=%u, q=%zu, inbox active=%d %zu/%u B pending_apply=%d. "
            "Applying the host's battle-entry snapshot here is what keeps our "
            "object-pool slot assignment identical to the host's for the whole "
            "match; budget %llums, then we continue unrepaired",
            s_match_seq, s_hold_anchor, g_state.pb_queue.size(),
            (int)inbox.active, inbox.bytes_received,
            (unsigned)inbox.meta.total_bytes, (int)inbox.pending_apply,
            (unsigned long long)kHoldBudgetMs);
        return true;
    }

    if (now - s_hold_since_ms >= kHoldBudgetMs) {
        s_holding            = false;
        s_expired_this_match = true;
        ++s_missed;
        const auto& inbox = g_state.pb_snapshot_inbox;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[POOLSYNC] OUTCOME=timeout match #%u anchor=%u after %llums "
            "(inbox active=%d %zu/%u B pending_apply=%d) -- releasing into "
            "ordinary playback. This match's pool indices may drift from the "
            "host's exactly as they did before the resync existed; totals "
            "synced=%u missed=%u",
            s_match_seq, s_hold_anchor,
            (unsigned long long)(now - s_hold_since_ms),
            (int)inbox.active, inbox.bytes_received,
            (unsigned)inbox.meta.total_bytes, (int)inbox.pending_apply,
            s_synced, s_missed);
        return false;
    }
    return true;
}

void PoolSync_HoldTick(uint64_t now) {
    if (!s_holding) return;
    if (now - s_hold_last_req_ms < kReqIntervalMs) return;
    s_hold_last_req_ms = now;
    SendWant(s_hold_anchor);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[POOLSYNC] re-requesting the battle snapshot (match #%u, anchor=%u, "
        "held %llums) -- one datagram", s_match_seq, s_hold_anchor,
        (unsigned long long)(now - s_hold_since_ms));
}

void PoolSync_OnSnapshotApplied(uint32_t anchor) {
    // Latched for EVERY apply, including the join ones this feature does not
    // own: it is precisely "a snapshot has landed on this viewer", which is the
    // signal that a CURRENT_MATCH join flow has finished.
    s_any_snapshot_landed = true;
    const bool was_ours = s_holding || PoolSync_Armed();
    s_applied_this_match = true;
    s_holding            = false;
    s_rejoin_pending     = false;   // Phase 4e: the re-JOIN is repaired
    s_applied_anchor_p1  = anchor + 1u;
    if (!was_ours || !PoolSync_Enabled()) return;
    ++s_synced;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[POOLSYNC] OUTCOME=applied match #%u anchor=%u after %llums held -- "
        "object pool, char slots and list topology are the host's from this "
        "battle frame on; totals synced=%u missed=%u",
        s_match_seq, anchor,
        (unsigned long long)(s_hold_since_ms
            ? (GetTickCount64() - s_hold_since_ms) : 0u),
        s_synced, s_missed);
}

}  // namespace specnode
