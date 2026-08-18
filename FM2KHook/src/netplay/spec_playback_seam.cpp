// spec_playback_seam.cpp -- the spectator's MATCH BOUNDARY state machine
// (State::PbBoundary SEAM / PINNING), split VERBATIM out of
// SpectatorNode_PopFrameInputs in spec_playback.cpp, which had reached the
// 1000-line cap and could not take another edit.
//
// Pure move, no behaviour change. The block used to express three outcomes by
// falling out of an `if (pb_boundary != NONE) { ... }` scope, returning true
// after writing the synthetic inputs, or returning false; those are now the
// three SeamStepResult values and the caller re-expresses them identically.
// The two file-static walk counters moved with it because nothing else in the
// tree touches them (they are deliberately NOT State fields for that reason),
// and ApplySessionEvent's MATCH_END re-arm reaches them through
// SeamResetWalkState() instead.
//
// What stayed behind in spec_playback.cpp: ApplySessionEvent (the op-apply
// dispatcher) and the rest of PopFrameInputs -- the snapshot holds, the
// battle-align hold, the deep-join hold, the natural-boot walk and the pop
// itself. What is here is exactly the machinery that runs BETWEEN matches.
#include "spectator_node.h"
#include "spectator_node_internal.h"   // shared State model + g_state
#include "spec_playback_internal.h"    // SeamStepResult + this TU's contract
#include "../hooks/css_autoconfirm.h"  // CssAutoConfirm_SetSeamHold
#include "../core/globals.h"           // FM2K::ADDR_GAME_MODE

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Seam synthetic-walk bookkeeping (shared between ApplySessionEvent's MATCH_END
// case -- via SeamResetWalkState -- and the SEAM branch below, hence file scope
// rather than function statics). Deliberately NOT fields on State: State lives
// in the shared spectator_node_internal.h and nothing outside this TU reads
// these.
//
//  s_seam_walk_pops   -- synthetic confirm edges fed by the "our results screen
//                        overran the stream's CSS" walk since this boundary
//                        opened. Bounds the walk (see SeamStep).
//  s_seam_walk_masked -- one-shot: the walk armed CssAutoConfirm's confirm mask
//                        so its synthetic 0x010 edges cannot lock characters
//                        when the engine reaches CSS.
// Both are re-armed at the single NONE -> SEAM edge (MATCH_END apply).
// ---------------------------------------------------------------------------
static uint32_t s_seam_walk_pops   = 0;
static bool     s_seam_walk_masked = false;

namespace specnode {

void SeamResetWalkState() {
    s_seam_walk_pops   = 0;
    s_seam_walk_masked = false;
}

// Boundary handling. SEAM: fall through to the normal pop -- the
// viewer MIRRORS the host's seam (results presses, CSS cursor
// movements) at the host's own pace; the seam hold masks confirm
// bits + locks so the rematch flow can never advance early, and the
// final picks come from the MATCH_START pin. PINNING: battle INPUTs
// are parked at the head while CssAutoConfirm walks the local CSS to
// the announced picks on synthetic neutral; exact pops resume when
// the local game re-enters battle.
//
// Release is EDGE-triggered: the local mode must first drop below
// 3000 (leave the old match's results screen) before a value >= 3000
// counts as "the new battle". The old level check released instantly
// when MATCH_START arrived while results were still on screen (UDP
// live edge), feeding the new match's inputs into the old screen and
// letting CssAutoConfirm disengage before CSS opened. The latch is
// re-armed per boundary at the MATCH_END (NONE -> SEAM) edge -- left
// sticky it decays back into that exact level trigger from boundary 2 on.
SeamStepResult SeamStep(uint16_t* p1_input, uint16_t* p2_input) {
    if (g_state.pb_boundary != State::PbBoundary::NONE) {
        const uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
        if (mode < 3000u) g_state.pb_boundary_left_battle = true;
        if (g_state.pb_boundary == State::PbBoundary::PINNING) {
            if (g_state.pb_boundary_left_battle && mode >= 3000u) {
                g_state.pb_boundary = State::PbBoundary::NONE;
                CssAutoConfirm_SetSeamHold(false);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: boundary PINNING -> NONE (battle entered, "
                    "resuming exact input pops, q=%zu)", g_state.pb_queue.size());
                // fall through to the normal pop below
            } else {
                // Pin walk in progress: park the new match's inputs and
                // feed neutral (CssAutoConfirm drives cursor + confirm
                // directly; the results screen, if still up, advances on
                // a synthetic confirm edge).
                if (!g_state.subscribed_upstream) return SeamStepResult::HOLD;
                uint16_t synthetic = 0;
                if (mode != 2000u) {
                    static uint32_t s_seam_tick = 0;
                    synthetic = (s_seam_tick++ & 1u) ? 0x010u : 0u;
                }
                g_state.pb_current_p1 = synthetic;
                g_state.pb_current_p2 = synthetic;
                if (p1_input) *p1_input = synthetic;
                if (p2_input) *p2_input = synthetic;
                return SeamStepResult::FEED;
            }
        }
        if (g_state.pb_boundary == State::PbBoundary::SEAM) {
            if (mode >= 3000u && !g_state.pb_css_marker_seen) {
                // Our results screens are still running: replay the
                // host's results inputs 1:1 (battle-end state matched,
                // so the pacing matches). Fall through to the normal pop.
            } else if (mode >= 3000u && g_state.pb_css_marker_seen) {
                // Stream already reached the host's CSS but our results
                // overran by a few frames: park the CSS inputs (they
                // mirror from CSS frame 0) and walk the remaining
                // results on synthetic edges.
                //
                // BOUNDED. 0x010 is the confirm/attack bit and this walk feeds
                // it to BOTH players; unbounded it is the "both players jabbing
                // 5a forever" report. Three guards, each closing one hole:
                //
                //  (a) STARVATION vs SEAM. q == 0 here does not mean "the host
                //      is at CSS and we are behind" -- it means the stream ran
                //      dry, and RunSpectatorTick's boundary bypass deliberately
                //      keeps ticking us at q == 0 so a boundary walk can make
                //      progress, so we kept driving the engine with no stream
                //      left to check against. The genuine case ALWAYS has the
                //      host's parked CSS inputs queued (that is what "the
                //      stream reached CSS" means), so q > 0 keeps the legit
                //      advance and drops only the starved one. Cost of holding
                //      is one arrival gap -- the host produces continuously.
                //
                //  (b) NO CONFIRM MASK. The sibling lean-seam branch engages
                //      CssAutoConfirm's hold for exactly this reason; this one
                //      never did. FM2K's VS rematch CSS auto-advances even on
                //      neutral inputs (previous match's locks persist --
                //      css_autoconfirm.h) and a synthetic confirm actively
                //      drives it, so the walk could lock the OLD characters and
                //      carry the engine into a rematch battle -- after which
                //      mode stays >= 3000, the marker stays seen, and the
                //      branch jabs for the rest of the session. Masked, the
                //      walk cannot lock: at CSS the confirm/color bits are
                //      stripped (directions still pass, cursor still mirrors)
                //      and locking belongs to the MATCH_START pin, which
                //      bypasses the mask. Inert until the engine reaches mode
                //      2000, so arming it changes nothing on the path that
                //      resolves in a few frames. Release is the existing seam
                //      contract (battle-align complete / PINNING -> NONE /
                //      post-CSS countdown), unchanged.
                //
                //  (c) NO TERMINATION. The real exits are the next MATCH_START
                //      draining at the head (SEAM -> PINNING) or the engine
                //      reaching CSS (the lean-seam branch below); if neither
                //      happens the conditions are self-sustaining. Stop driving
                //      after kMaxSeamWalkPops and hold neutral -- a results
                //      screen that has not cleared in ~6s is not a results
                //      screen, and an idle picture beats a sim running on
                //      synthetic input. Re-armed per boundary at MATCH_END.
                if (!g_state.subscribed_upstream) return SeamStepResult::HOLD;
                if (g_state.pb_queue.empty()) return SeamStepResult::HOLD;  // (a)
                if (!s_seam_walk_masked) {                           // (b)
                    s_seam_walk_masked = true;
                    CssAutoConfirm_SetSeamHold(true);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: seam results-overrun walk -- confirm "
                        "mask armed before feeding synthetic edges (q=%zu)",
                        g_state.pb_queue.size());
                }
                constexpr uint32_t kMaxSeamWalkPops = 600;  // ~6s at 100fps
                uint16_t synthetic = 0;
                if (s_seam_walk_pops < kMaxSeamWalkPops) {           // (c)
                    synthetic = (s_seam_walk_pops++ & 1u) ? 0x010u : 0u;
                } else if (s_seam_walk_pops == kMaxSeamWalkPops) {
                    ++s_seam_walk_pops;   // one-shot: log once, then hold
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: seam results-overrun walk hit its %u-pop "
                        "bound with mode=%u still >= 3000 -- stopping synthetic "
                        "confirms and holding neutral (waiting on MATCH_START "
                        "or a real CSS transition, q=%zu)",
                        kMaxSeamWalkPops, mode, g_state.pb_queue.size());
                }
                g_state.pb_current_p1 = synthetic;
                g_state.pb_current_p2 = synthetic;
                if (p1_input) *p1_input = synthetic;
                if (p2_input) *p2_input = synthetic;
                return SeamStepResult::FEED;
            } else if (mode == 2000u && !g_state.pb_css_marker_seen) {
                // Our CSS opened before the stream's CSS_ENTERED: the
                // remaining head INPUTs are the host's results tail --
                // discard them (apply ops; one is the marker) and hold
                // neutral so nothing can advance.
                while (!g_state.pb_queue.empty() &&
                       !g_state.pb_css_marker_seen) {
                    const SessionEvent& head = g_state.pb_queue.front();
                    if (head.type != SessionEventType::INPUT) {
                        ApplySessionEvent(head);
                    }
                    g_state.pb_queue.erase(g_state.pb_queue.begin());
                }
                if (!g_state.subscribed_upstream) return SeamStepResult::HOLD;
                g_state.pb_current_p1 = 0;
                g_state.pb_current_p2 = 0;
                if (p1_input) *p1_input = 0;
                if (p2_input) *p2_input = 0;
                return SeamStepResult::FEED;
            } else {
                // CSS open + marker seen: the mirror starts at the host's
                // CSS frame 0. Engage the confirm mask for the WHOLE seam
                // mirror -- no pop countdown. Directions pass through (the
                // cursor still mirrors) but a replayed confirm edge can
                // never lock chars early: a lagging/catch-up viewer replays
                // the seam with edge artifacts (2026-07-17 forensics:
                // spurious act=1/1 ~60 pops early on pass-through cells
                // 11/13 while the host picked 2/14 -> the game match-inited
                // the WRONG chars, the late MATCH_START pin re-locked the
                // right ones mid-init, and the HP reset never re-ran ->
                // battle 2 started with match-1's final HP, full-state
                // desync from bf=2). All locking belongs to the MATCH_START
                // pin, which bypasses this mask (css_autoconfirm masks only
                // when !g_active); the PINNING->NONE release drops the hold
                // at battle entry. The 2026-06-11 "hold never released"
                // deadlock predates the pin and cannot recur.
                g_state.pb_boundary = State::PbBoundary::NONE;
                CssAutoConfirm_SetSeamHold(true);   // mask only
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: lean seam -> mirror (CSS aligned at "
                    "host frame 0, confirm mask held until MATCH_START pin, "
                    "q=%zu)", g_state.pb_queue.size());
                // fall through to the normal pop
            }
        }
    }
    return SeamStepResult::FALL_THROUGH;
}

}  // namespace specnode
