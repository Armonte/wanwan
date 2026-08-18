// trampoline_css.cpp -- RunCssTick (CSS lockstep phase). Split from main_loop_trampoline.cpp.
#include "main_loop_trampoline.h"
#include "globals.h"
#include "../hooks/hooks.h"
#include "../hooks/wndproc_subclass.h"
#include "../netplay/netplay.h"
#include "../netplay/control_channel.h"
#include "../netplay/game_hash.h"
#include "../netplay/spectator_node.h"
#include "../ui/shared_mem.h"
#include "../parity/parity_recorder.h"
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include "trampoline_internal.h"

// CSS: lockstep via control channel. Same sim functions as native, plus a
// stall check that waits for the remote peer's input before advancing.
// When stalling we only pump messages and poll the control channel; we do
// NOT run the sim (so the peers stay frame-aligned).
void RunCssTick() {
    Hook_CheckGameModeTransition_Public();

    // Stress mode + offline both run CSS natively -- no peer, no lockstep.
    const bool skip_netplay = g_offline_mode || g_stress_mode;

    // No connection barrier here, but NOT because the CSS scene may tick
    // unsynchronized -- it may not, and the comment that used to live here
    // saying it was "fine" was the bug (css_child_asymmetry.md): whichever
    // peer reached character select first free-ran the scene 19-34 frames
    // ahead of the other, and the rendezvous realigns the RNG and four
    // scalars but NOT the object pool, so the two sims ran identical inputs
    // on differently-phased scene state and permanently transposed two pool
    // slots. Netplay_ProcessCSS now PARKS the sim at both pre-rendezvous
    // legs (it returns false, which the stall check below already handles by
    // skipping sim and render). We keep pumping ControlChannel/HELLO here so
    // the handshake still makes progress while parked; if the MM-timer-driven
    // Poll didn't already finish it, this completes it. The user-facing
    // freeze this was written to avoid is unchanged in kind and barely
    // changed in length: the post-rendezvous gekko delay window already
    // holds the same screen for ~1.1s before the first AdvanceEvent.
    if (!skip_netplay && !Netplay_IsConnected()) {
        ControlChannel_Poll();
        static uint32_t last_poll = 0;
        uint32_t now = GetTickCount();
        if (now - last_poll > 500) {
            ControlChannel_SendHello((uint8_t)g_player_index,
                                     fm2k::game_hash::Compute());
            last_poll = now;
        }
        // Fall through -- Netplay_ProcessCSS below decides. Pre-park this
        // fell through to a free-running CSS tick; it now falls through to
        // the park, which is the point.
    }

    // CSS lockstep + stall: Netplay_ProcessCSS returns false while we're
    // waiting on a remote input for the current CSS frame. Under #66 CSS
    // rollback it instead runs the sim PER AdvanceEvent (re-sim) internally
    // and returns true once at least one confirmed advance landed this tick.
    if (!skip_netplay) {
        if (!Netplay_ProcessCSS()) {
            return;
        }
    }

    // When CSS rollback is active the sim/parity/[CSS-FP] already ran inside
    // Netplay_ProcessCSS (per AdvanceEvent). Only the lockstep + offline/stress
    // paths run the single native tick here.
    const bool css_rb_active = (!skip_netplay && g_css_rollback);
    if (!css_rb_active) {
        // Run the native CSS tick.
        if (original_process_game_inputs) original_process_game_inputs();
        if (original_update_game)         original_update_game();
        ++g_sim_step_count;   // sim-fps: one logic tick
        ParityRecorder::Capture();  // post-update snapshot for parity .pty

        // [CSS-FP] parity emit (#66 Phase 1): dense per-CSS-frame state so the
        // harness can align host/guest/spectator CSS streams by confirmed input
        // and assert bit-exact cursors/selection. Only in real netplay CSS
        // (offline/stress have no peer to diverge from). Pairs with the
        // spectator's emit in SpectatorSimOneFrame.
        if (!skip_netplay) {
            Netplay_EmitCssFpHost();
        }
    }

    // Advance virtual_time to match the spectator's per-pop bump cadence, so
    // the counter the SPECTATOR reads through Hook_timeGetTime advances at the
    // same rate the host produced frames.
    //
    // CORRECTION 2026-08-17 (css_child_asymmetry.md hazard 2), because the
    // sentence that used to sit here was wrong and is a live trap for anyone
    // adding CSS behaviour keyed on the engine clock: Hook_timeGetTime
    // virtualizes only when Netplay_IsActive() || SpectatorNode_IsPlayingBack()
    // (hooks.cpp), and Netplay_IsActive() is `g_simple_state == BATTLE &&
    // g_session != nullptr` (netplay.cpp) -- i.e. BATTLE-ONLY. So during CSS
    // this bump is consumed by the SPECTATOR plane alone; on the two PLAYER
    // planes timeGetTime returns wall clock during character select and this
    // counter is inert. That is currently harmless rather than lucky: the
    // binary's entire import table holds exactly one clock (timeGetTime, IAT
    // 0x41c22c) and its only xrefs are main_game_loop (which the trampoline
    // replaces outright), main_window_proc and sub_417770 -- none in the
    // object, script or spawn path -- so no clock value can reach a sim
    // decision. Filed, not widened: making CSS virtualize on the player planes
    // is a behaviour change to a phase this campaign has only just made
    // lockstep, and it buys nothing measurable today.
    extern uint32_t g_virtual_time_ms;
    g_virtual_time_ms += 10;

    RenderFrameWithSnapshot();
}

