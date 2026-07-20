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

    // Stress mode + offline both run CSS natively — no peer, no lockstep.
    const bool skip_netplay = g_offline_mode || g_stress_mode;

    // No connection barrier here. Same reasoning as RunNativeTick:
    // freezing the render path while we wait for handshake leaves the
    // user staring at an unresponsive CSS window for several seconds —
    // they tab out, see "not responding", and force-close. The CSS
    // state machine is fine to tick unsynchronized in this window:
    // Netplay_ProcessCSS already handles the !g_remote_css_ready case
    // by running the game locally without driving GekkoNet, and on the
    // first synced frame it reseeds the RNG so peers re-converge. We
    // still pump ControlChannel/HELLO here so handshake makes
    // progress; if the MM-timer-driven Poll didn't already finish it,
    // this completes it.
    if (!skip_netplay && !Netplay_IsConnected()) {
        ControlChannel_Poll();
        static uint32_t last_poll = 0;
        uint32_t now = GetTickCount();
        if (now - last_poll > 500) {
            ControlChannel_SendHello((uint8_t)g_player_index,
                                     fm2k::game_hash::Compute());
            last_poll = now;
        }
        // Fall through — let the CSS tick.
    }

    // CSS lockstep + stall: Netplay_ProcessCSS returns false while we're
    // waiting on a remote input for the current CSS frame.
    if (!skip_netplay) {
        if (!Netplay_ProcessCSS()) {
            return;
        }
    }

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

    // Advance virtual_time to match the spectator's per-pop bump cadence.
    // Hook_timeGetTime returns g_virtual_time_ms whenever a session is
    // active OR a spectator is subscribed; if host doesn't bump per CSS
    // tick, host's timeGetTime stays at zero while spectator's accelerates
    // by 10/pop, and any FM2K CSS code that reads timeGetTime (animation
    // timers, fade counters) sees different deltas → desync.
    extern uint32_t g_virtual_time_ms;
    g_virtual_time_ms += 10;

    RenderFrameWithSnapshot();
}

