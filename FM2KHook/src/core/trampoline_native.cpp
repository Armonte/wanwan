// trampoline_native.cpp -- RunNativeTick (menu/intro/results phase). Split from main_loop_trampoline.cpp.
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

// Native: menu / intro / results. No netplay state, run sim at wall-clock
// cadence. We don't replicate main_game_loop's frame-skip math -- a simple
// one-tick-per-10ms gets the same perceived behavior without any of the
// prologue writes that broke determinism in battle.
void RunNativeTick() {
    Hook_CheckGameModeTransition_Public();

    const bool skip_netplay = g_offline_mode || g_stress_mode;

    // Pre-handshake gate: hold the game at the boot frame until both peers
    // have completed HELLO/HELLO_ACK. Without this, each peer free-runs
    // through title at its own wall-clock pace; their per-frame RNG/timer
    // states diverge by tens-to-hundreds of frames before CSS-rendezvous
    // reseeds RNG, and CSS state itself evolves locally on each side
    // (cursor moves, animation counters, etc.) before lockstep takes
    // over -- visible as cursor-position desync the moment CSS-SYNC fires.
    //
    // Cost: the peer whose window comes up first sits frozen on title for
    // up to ~MAX_HANDSHAKE_HOLD_MS while the slower peer's process spawns
    // and binds its socket. Hard cap so a missing peer doesn't hang the
    // game forever -- after the cap we fall through to the legacy free-run
    // (matches pre-C* behavior, lets users at least reach offline UI).
    constexpr uint32_t MAX_HANDSHAKE_HOLD_MS = 10000;
    static uint32_t s_first_native_tick_ms = 0;
    if (!skip_netplay && !Netplay_IsConnected()) {
        ControlChannel_Poll();
        const uint32_t now_ms = GetTickCount();
        if (s_first_native_tick_ms == 0) s_first_native_tick_ms = now_ms;
        static uint32_t last_poll = 0;
        if (now_ms - last_poll > 500) {
            ControlChannel_SendHello((uint8_t)g_player_index,
                                     fm2k::game_hash::Compute());
            last_poll = now_ms;
        }
        const bool give_up = (now_ms - s_first_native_tick_ms) >= MAX_HANDSHAKE_HOLD_MS;
        if (!give_up) {
            // Hold: render the current snapshot (so the game's window
            // doesn't go black/freeze visibly), pump messages, do nothing
            // else. No PGI, no UG, no game_mode transition. Both peers
            // sit here until CheckFullyConnected fires on both sides.
            RenderFrameWithSnapshot();
            return;
        }
        // Timed out -- peer never showed up. Bleed-through to CSS lets
        // the user "control" a non-match (no peer ever joins, but the
        // game advances anyway), which is worse than a clean failure.
        // Publish DISCONNECT -- the launcher's PollMatchOutcome path
        // sees that, kills the game, shows a toast, and returns the
        // user to the lobby. We just stay frozen on title until the
        // launcher tears the process down.
        static bool s_logged_timeout = false;
        if (!s_logged_timeout) {
            s_logged_timeout = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Trampoline: handshake hold timed out after %u ms -- "
                        "publishing DISCONNECT (launcher will close us)",
                        MAX_HANDSHAKE_HOLD_MS);
            SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_DISCONNECT);
        }
        // Hold the snapshot until the launcher's outcome poll fires
        // (≤500 ms) and tears the process down. No fall-through --
        // we don't want CSS / inputs reaching the user.
        RenderFrameWithSnapshot();
        return;
    }

    if (!skip_netplay) {
        Netplay_ProcessMenu();
    }

    if (original_process_game_inputs) original_process_game_inputs();
    if (original_update_game)         original_update_game();
    ++g_sim_step_count;   // sim-fps: one logic tick
    ParityRecorder::Capture();  // post-update snapshot for parity .pty

    // Same virtual-time alignment as RunCssTick -- keep timeGetTime cadence
    // 1:1 with the spectator's per-pop bump on title/menu frames so any
    // FM2K menu animation code that reads timeGetTime sees identical deltas
    // on both sides.
    extern uint32_t g_virtual_time_ms;
    g_virtual_time_ms += 10;

    RenderFrameWithSnapshot();
}

