// Pre-rendezvous character-select PARK -- the kill switch, the per-phase
// census, the parked-while-waiting detector and the one function that decides a
// pre-rendezvous CSS tick. Split out of netplay_css.cpp 2026-08-17 (review
// amendment lane) purely for the repo's 1000-line rule: netplay_css.cpp had 47
// lines of headroom and this lane's amendments needed more. Behaviour is
// unchanged by the move -- the statics were file-local there and are file-local
// here, and the only consumers are Netplay_ProcessCSS's two pre-rendezvous legs
// plus its rendezvous census line, all declared in netplay_internal.h.
#include "netplay.h"
#include "netplay_internal.h"
#include "globals.h"
#include <SDL3/SDL_log.h>
#include <cstdlib>
#include <windows.h>


// =============================================================================
// PRE-RENDEZVOUS CSS PARK (css_child_asymmetry.md, 2026-08-17)
// =============================================================================
//
// THE BUG THIS CLOSES. The character-select window was NOT lockstep from its
// first frame. Netplay_ProcessCSS used to return true -- "let the game run but
// don't drive the session yet" -- at both pre-rendezvous legs, so RunCssTick
// sim'd AND rendered the character-select scene on whichever peer arrived
// first. Measured in 4/4 preserved corpora: the HOST free-ran 0 scene frames
// and the GUEST 19-34, every time, because the host is later to the scene (it
// does the extra listen / session stand-up work) while the guest, already
// there, spins. The rendezvous block below then realigns the RNG seed, two
// action states, the round-timer counter and the input-repeat state/timer --
// but NOT the object pool. The two peers therefore ran identical inputs on
// differently-PHASED scene state: every CSS object's script cursor (+0x30),
// script wait counter (+0x2C) and animation phase sat 19-34 frames apart, and
// none of those fields is in top= / bind= / fp= / [CSS-FP], so every gate term
// compared EQUAL while the pools were not. ~106 lockstep frames later the
// post-confirm effect chain resolved differently (the kind-1 object at slot 51
// survives on the host, terminates on the guest), create_game_object's
// first-free scan landed the battle-entry type-14 root at slot 52 vs 51, and
// top=/bind= were red for the whole match on one permanent slot transposition
// inherited by all 76 children. Sim-silent (nobj=/crc=/CINPUT identical), which
// is why it survived so long.
//
// THE FIX. Park the CSS SIM -- not the outer loop -- from CSS entry until the
// rendezvous completes, so BOTH peers' first lockstep frame is the same scene
// frame and the pool is identical BY CONSTRUCTION: no wire traffic, no host
// authority, no blob.
//
// WHERE THE PARK LANDS, AND WHY THAT IS THE SCENE'S FRAME 0. Measured from the
// corpora (POOLSET seq == sim-tick ordinal, identical on both peers): the CSS
// scene LOAD (pool 10 -> 49 objects) happens inside a NATIVE tick -- the same
// sim frame that writes game_mode = 2000 -- so by the time ClassifyPhase routes
// the NEXT iteration to RunCssTick the scene is already built. Both peers reach
// that tick at the same seq with byte-identical fp=/top=/bind=
// (seq=16 fp=0x9192249E on both, 4/4 corpora). Parking from the FIRST CSS tick
// therefore costs zero scene frames on either peer and needs no "wait for the
// loader" heuristic: the loader has already run, outside this phase.
//
// WHAT KEEPS RUNNING WHILE PARKED (deadlock safety):
//   * ControlChannel_Poll() at the top of Netplay_ProcessCSS -- so BATTLE_READY
//     is received and PING/PONG flows. AT THE !g_remote_css_ready LEG (the live
//     one) the 1.5s/10s connection timeout is ARMED and publishes DISCONNECT if
//     the peer dies mid-park: control_channel.cpp gates that timeout on
//     g_connected, which CheckFullyConnected sets together with
//     g_simple_state = CONNECTED. AT THE FIRST LEG (g_simple_state < CONNECTED)
//     IT IS NOT ARMED -- g_connected is false there by construction. Review
//     amendment (css_rendezvous_review.md G1a): the first leg is UNREACHABLE
//     (RunNativeTick's pre-handshake gate and hooks_update.cpp's SYNC BARRIER
//     both hold the game at the boot frame until CONNECTED, and g_simple_state
//     never regresses below CONNECTED inside a session), so it is belt-and-
//     braces on a dead path, not a measured hole. If anyone ever makes it live,
//     note that it also returns ABOVE the BATTLE_READY send below -- a peer
//     parked there is frozen AND silent, which the periodic parked-waiting warn
//     in CssPreRendezvousTick is there to make visible in one grep.
//   * The BATTLE_READY (re)send + the g_css_frame == 0 respam loop, both of
//     which sit ABOVE the park return.
//   * KeepaliveTimerProc's off-thread ControlChannel poll.
//   * The outer trampoline loop: PumpMessages, the WndProc subclass, pacing.
//   Only the sim and the render are skipped, via RunCssTick's EXISTING early
//   return (trampoline_css.cpp) -- the same code path a lockstep CSS stall has
//   always taken. That matters for the UX objection: today the CSS screen is
//   already frozen for ~1.1s after the rendezvous while gekko's delay window
//   fills (measured host seq=16 -> seq=17 gap: 1.12s), so this park adds ~0ms
//   on the host and ~350ms on the guest to a freeze that already ships.
//
// WHAT IS NOT DELETED, AND WHY IT IS NOT A FALLBACK. The rendezvous below still
// reseeds the RNG and canonicalises the action states / round timer / input
// repeat state. With a zero-length free-run those are no longer covering THIS
// window -- but they never only covered this one: they also cover the PRE-CSS
// (title/menu) window, which is unsynchronised for real player input, and the
// input-repeat pair additionally carries divergent state out of the previous
// battle's rollback re-sims into a rematch CSS. Different window, different
// mechanism, still load-bearing. See css_rendezvous_fix.md section "deletions".
//
// ENGINE SCOPE: FM2K ONLY (review amendment, css_rendezvous_review.md G6, the
// worst finding). The proof section used to claim "identical park semantics" on
// FM95. It is FALSE on FM95's DEFAULT (host-driven) path, and two files falsify
// it: (1) FM95_TRAMPOLINE is opt-in (hooks_update.cpp), so CPW's own WinMain
// drives the frame and Hook_ProcessGameInputs -- a detour on ADDR_PROCESS_INPUTS
// -- keeps firing while Hook_UpdateGameState returns 0, and its CSS branch
// consults Netplay_CanAdvanceCSS(), which returns true while !g_css_synced, so
// g_input_buffer_index and edge detection would advance on the parked peer for
// the whole park -- the exact buf_idx divergence FM95's own connection barrier
// exists to stop; (2) g_fm95_skip_next_render is only set on the trampoline
// branch, so the legacy path would run an UNPROTECTED render on every parked
// frame (RenderFrameWithSnapshot's protect is BATTLE-only), decoupling sim from
// render 1:N and unequally between peers. On FM95 the park would therefore
// convert a phase offset into a buf_idx + render-count offset, which is not a
// fix. FM95 support is documented incomplete, its gate stage is advisory and
// not green, and neither of those two legs can be validated here -- so the park
// is compiled out of FM95 rather than shipped half-closed. FM95 keeps the
// pre-fix free-run and the [CSS-RDV] census reports it honestly (park=0,
// freerun_sim_ticks>0). To enable the park on FM95 later, BOTH legs must land
// first: Netplay_CanAdvanceCSS() must return false while parked, and the legacy
// CSS return must set g_fm95_skip_next_render.
//
// KILL SWITCH: FM2K_CSS_PARK, default ON. A/B scaffolding for the causality
// proof only -- NOT a fallback and NOT a permanent resident. DELETION IS
// SCHEDULED, not wished (review G6): delete FM2K_CSS_PARK, this function, the
// CssPreRendezvousTick branch and the two harness forwarding blocks
// (tools/spec_selftest.py, tools/hub_spectate_e2e.py) in the release AFTER the
// first stable that ships the park. The [CSS-RDV] census and the harness
// advisory block are DETECTORS and stay.
bool CssRendezvousPark_Enabled() {
    if constexpr (!FM2K::kIsFM2K) {
        return false;                       // FM95: see ENGINE SCOPE above
    }
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = std::getenv("FM2K_CSS_PARK");
        if (v == nullptr || v[0] == '\0') {
            s_on = 1;                       // default ON -- this is the fix
        } else if ((v[0] == '0' || v[0] == '1') && v[1] == '\0') {
            s_on = v[0] - '0';              // strict: only "0" and "1" parse
        } else {
            s_on = 1;                       // fail SAFE (fix stays on), loudly
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[CSS-RDV] FM2K_CSS_PARK=\"%s\" is not \"0\" or \"1\" -- "
                "refusing to guess; keeping the pre-rendezvous park ON", v);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[CSS-RDV] contract: park=%d (FM2K_CSS_PARK, default ON; =0 "
            "restores the pre-fix unsynchronised character-select free-run for "
            "A/B only -- it re-opens a real pool divergence)", s_on);
    }
    return s_on == 1;
}

namespace {
    // Per-CSS-phase census, emitted once at the rendezvous. park = ticks the
    // sim was held; freerun = pre-rendezvous ticks that DID sim (must be 0 with
    // the park on -- this is acceptance term P2, and it is a DETECTOR, so it
    // stays after the kill switch goes).
    uint32_t s_css_park_ticks    = 0;
    uint32_t s_css_freerun_ticks = 0;
    bool     s_css_prev_synced   = false;

    // Parked-while-WAITING detector (review amendment 1, css_rendezvous_review
    // .md G1a -- the highest-value item in that review). The rendezvous census
    // above is emitted ONLY when the rendezvous completes, so the ONE new
    // failure mode this change creates -- a peer that parks and never un-parks
    // -- used to emit NOTHING, and the harness then printed the benign "no
    // netplay character-select phase ran". That is a freeze reported as an
    // absence. Every 2 s while parked, say so out loud instead. Once per 2 s,
    // never per frame (hook logging is SYNCHRONOUS). DETECTOR, not scaffolding:
    // it stays when FM2K_CSS_PARK is deleted.
    constexpr uint32_t kParkWarnMs = 2000u;
    uint32_t s_park_start_ms = 0;      // 0 = not currently parked
    uint32_t s_park_last_warn_ms = 0;

}  // namespace

// Decide + account one pre-rendezvous tick. `leg` is 1 for the pre-CONNECTED
// leg and 2 for the !g_remote_css_ready leg, so a stuck peer's log says WHICH
// wait it is stuck in (they have different deadlock properties -- see the
// deadlock-safety block above). Returns what Netplay_ProcessCSS must hand
// back: false = park (RunCssTick skips sim AND render via its existing stall
// return), true = pre-fix free-run.
bool CssPreRendezvousTick(int leg) {
    if (!CssRendezvousPark_Enabled()) {
        ++s_css_freerun_ticks;
        return true;
    }
    ++s_css_park_ticks;
    const uint32_t now = GetTickCount();
    if (s_park_start_ms == 0) {
        s_park_start_ms     = now ? now : 1u;   // 0 is the not-parked marker
        s_park_last_warn_ms = s_park_start_ms;
    } else if (now - s_park_last_warn_ms >= kParkWarnMs) {
        s_park_last_warn_ms = now;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[CSS-RDV] parked %u ms / %u ticks waiting for peer "
            "BATTLE_READY (leg=%d) -- character-select sim is HELD; the "
            "screen is frozen until the peer arrives",
            now - s_park_start_ms, s_css_park_ticks, leg);
    }
    return false;
}

// ---- the two hooks Netplay_ProcessCSS calls into ---------------------------

// Per-CSS-phase census reset. Called at the top of Netplay_ProcessCSS with the
// live g_css_synced: a new phase (battle end / Netplay_Reset cleared the flag)
// restarts the counters and re-arms the parked-while-waiting warn, so the
// rendezvous line describes THIS phase and not the sum of all of them.
void CssPark_OnPhaseEdge(bool css_synced) {
    if (s_css_prev_synced && !css_synced) {
        s_css_park_ticks    = 0;
        s_css_freerun_ticks = 0;
        s_park_start_ms     = 0;
        s_park_last_warn_ms = 0;
    }
    s_css_prev_synced = css_synced;
}

// P2 acceptance term + permanent detector, emitted once per peer per CSS phase
// at the rendezvous. park_ticks = ticks this peer held the CSS sim waiting for
// the rendezvous; freerun_sim_ticks = pre-rendezvous ticks that actually sim'd.
// With the park ON, freerun MUST be 0 on BOTH peers -- that is the
// by-construction claim, stated as a number in each peer's own log rather than
// inferred from [POOLSET].
void CssPark_EmitRendezvousCensus() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[CSS-RDV] rendezvous: park_ticks=%u freerun_sim_ticks=%u park=%d",
        s_css_park_ticks, s_css_freerun_ticks,
        CssRendezvousPark_Enabled() ? 1 : 0);
}
