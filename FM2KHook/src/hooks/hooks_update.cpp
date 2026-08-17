// hooks_update.cpp -- Hook_UpdateGameState (the per-frame control point). Split from hooks.cpp.
#include "hooks.h"
#include "hooks_internal.h"
#include "round_events.h"     // C3.5 -- vs_round_function detour install
#include "css_autoconfirm.h"  // CSS lock-and-confirm for offline replay playback
#include "css_fastsound.h"    // FM2K_FPK_CSS_FASTSOUND: lazy DSound buffers (CSS dip fix)
#include "per_game_patches.h" // damage multiplier MinHook + team-size override
#include "render_simd.h"      // FM2K_BLIT_SIMD: blit + case -10 blur reimplementation
#include "globals.h"

#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <list>
#include <thread>
#include <condition_variable>
#include <atomic>
#include "netplay.h"
#include "control_channel.h"
#include "../netplay/game_hash.h"
#include "imgui_overlay.h"
#include "shared_mem.h"
#include "savestate.h"  // CHAR_SLOT_BASE, CHAR_SLOT_SIZE (corrected by Wave C audit)
#include "../core/main_loop_trampoline.h"  // TrampolineMainLoop -- owns the outer loop
#include "../audio/sound_rollback.h"        // Mike Z desired/actual sound layer
#include "../netplay/spectator_node.h"      // spectator playback queue accessors
#include "../ui/input_binder.h"             // FM2KInputBinder::Sample_Win32 + Bindings
#include "../ui/screenshot.h"               // FM2KCapture::SaveScreenshot for the auto-banner pipeline
#include "../ui/fc_hud.h"                   // IsChatInputActive -- gate local input during typing
#include "../vfs/fpk_reader.h"              // FM2K_FPK_VFS: inflate a slim .fpk -> original asset bytes
#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cfloat>   // _controlfp_s, _PC_53, _MCW_PC, _RC_NEAR, _MCW_RC, _MCW_EM
#include <cstdint>
#include <string>

// Pin the x87 FPU control word to a fixed precision + rounding mode on the
// game thread. IDA audit found the binary never calls _controlfp / fldcw and
// DirectDraw's SetCooperativeLevel is invoked without DDSCL_FPUPRESERVE, so
// the default precision is whatever DirectDraw/driver/OS happens to leave.
// That varies across machines and is almost certainly why peer simulations
// diverge on movement (velocity, collision, normalization all use floats).
// Call this before every gameplay tick to override any mid-frame changes.
// MXCSR bit layout (SSE control/status register):
//   bit 15 FZ (flush-to-zero)
//   bits 13-14 RC (round control): 00 nearest, 01 down, 10 up, 11 truncate
//   bits 7-12 exception masks (we set all = masked)
//   bit 6 DAZ (denormals-are-zero)
//   bits 0-5 exception flags (sticky, we clear)
// We want: round-to-nearest-even, all exceptions masked, no FZ/DAZ, flags clear.
#include "hooks_internal.h"

// Hook: UpdateGameState
// Main control point - check transitions, process netplay
int __cdecl Hook_UpdateGameState() {
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    // FM2K_TEST_ROUNDS: force g_default_round (0x430124) on EVERY peer -- host,
    // client, AND spectator -- every frame. The host-only HOST_CONFIG write left
    // the spectator on best-of-3 while the host ran 1-round, so they diverged at
    // the host's round-1 match-end (the spec thought it was round 2). Forcing it
    // here keeps all peers on the same round count. Test harness only; no-op unset.
    // FM2K-only: g_default_round's FM95 equivalent is unmapped (RE-3 in
    // docs/FM95_Support_Status.md) -- the harness override is inert there.
    //
    // MEASURED 2026-08-16 (Phase 6): ON FM2K THIS WHOLE FUNCTION IS DEAD CODE.
    // TrampolineMainLoop owns the loop and every phase calls
    // original_update_game() -- the MinHook TRAMPOLINE -- directly
    // (trampoline_battle.cpp, _css.cpp, _native.cpp, _spectator.cpp), so this
    // detour is never entered. Proof: a per-battle log line placed here emitted
    // ZERO lines on all three planes, and the FM2K_TEST_ROUNDS_HOST_ONLY switch
    // below logs on arm and never logged once with the variable set on host,
    // guest and spectator. Consequence for anyone reading the comment above:
    // the "force on every peer every frame" has NOT been in effect on FM2K
    // since the trampoline took the loop, so it cannot have been masking
    // round-count delivery either -- round count reaches a guest or a spectator
    // through HOST_CONFIG and nothing else, which the Phase 6 settings leg
    // measures directly (guest and spectator both latched rounds=2 from a
    // host-only override). Kept, not deleted: the block is correct where it
    // lives, FM95 still reaches this function through its host-driven path, and
    // deleting a dead safety net is a separate decision from documenting it.
    //
    // FM2K_TEST_ROUNDS_HOST_ONLY=1 (default OFF) SUPPRESSES the every-frame
    // force, leaving only the host-side write in netplay_control.cpp's
    // HostApplyMatchSettingOverrides, so round count follows the same delivery
    // path as round time / game speed / stage. Default-off keeps every existing
    // stage bit-identical; on FM2K it is inert either way (see above).
    if constexpr (FM2K::kIsFM2K) {
        static const bool s_rounds_host_only = []{
            const char* v = std::getenv("FM2K_TEST_ROUNDS_HOST_ONLY");
            const bool on = (v && v[0] == '1' && v[1] == '\0');
            if (on)
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[TEST-ROUNDS] HOST_ONLY armed -- the per-frame "
                    "g_default_round force is OFF on this peer; round count "
                    "must arrive via HOST_CONFIG");
            return on;
        }();
        static const int s_test_rounds = []{
            const char* v = std::getenv("FM2K_TEST_ROUNDS");
            return (v && v[0]) ? std::atoi(v) : 0;
        }();
        if (s_test_rounds > 0 && !s_rounds_host_only)
            *(uint32_t*)0x430124 = (uint32_t)s_test_rounds;
    }

    // (The per-battle three-plane [CFG] settings stamp lives in
    //  round_events.cpp's vs_round_function detour, NOT here: FM2K's
    //  TrampolineMainLoop calls original_update_game directly, so this detour
    //  is not on the per-frame battle path at all. Measured, not assumed -- the
    //  first build that stamped from here produced zero [CFG] lines on every
    //  plane.)

    // FM95 host-driven trampoline activation (opt-in via FM95_TRAMPOLINE=1).
    // FM2K's main_game_loop is replaced wholesale by TrampolineMainLoop,
    // but FM95 keeps its natural WinMain pump. Drive the trampoline tick
    // from inside Hook_UpdateGameState instead. For non-NATIVE phases the
    // tick handles update + render itself (via AdvanceEvent +
    // RenderFrameWithSnapshot); set g_fm95_skip_next_render so
    // Hook_RenderGame suppresses the host's natural render call right
    // after we return. NATIVE phase falls through to the existing logic.
    if constexpr (FM2K::kIsFM95) {
        static const bool s_use_trampoline = []() {
            const char* e = std::getenv("FM95_TRAMPOLINE");
            const bool on = (e && std::strcmp(e, "1") == 0);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "FM95 trampoline gate: env=\"%s\" -> %s",
                        e ? e : "(unset)", on ? "ACTIVE" : "off (host-driven)");
            return on;
        }();
        // Only the LEGACY host-driven path. Once the inline loop-patch owns the
        // loop (g_fm95_loop_owned), TrampolineMainLoop drives every phase via
        // TrampolineFrameTick and calls original_update_game directly, so this
        // hook is no longer the driver -- running the host-driven dispatch here
        // too would double-tick. Kept as a fallback for when the patch fails.
        if (s_use_trampoline && !g_fm95_loop_owned) {
            // Host-driven variant: dispatches BATTLE/CSS/SPECTATOR but NOT
            // NATIVE (the fall-through below runs the single host update for
            // NATIVE -- running RunNativeTick here too would double-tick).
            LoopPhase phase = TrampolineFrameTickHostDriven();
            // Log first-seen-per-phase so the log shows the engine-aware
            // classifier picking up FM95 phase transitions in real time.
            static LoopPhase s_last_logged_phase = LoopPhase::NATIVE;
            static bool      s_phase_logged_once = false;
            if (!s_phase_logged_once || phase != s_last_logged_phase) {
                const char* name =
                    phase == LoopPhase::TRAMPOLINE_BATTLE ? "BATTLE" :
                    phase == LoopPhase::CSS               ? "CSS" :
                    phase == LoopPhase::SPECTATOR_PLAYBACK? "SPECTATOR" :
                                                            "NATIVE";
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "FM95 trampoline tick phase = %s", name);
                s_last_logged_phase = phase;
                s_phase_logged_once = true;
            }
            if (phase != LoopPhase::NATIVE) {
                g_fm95_skip_next_render = true;
                return 0;
            }
            // NATIVE: fall through to existing logic so original_update_game
            // gets called via the normal offline / netplay path below.
        }
    }

    // Offline mode - just pass through
    if (g_offline_mode) {
        // T4 probe: when FM2K_T4_PROBE=1, walk the fighter object pool
        // before each update_game tick using the EXACT same logic as
        // vs_round_function case-200 (type==4, flag@+346==0, HP@slot>0).
        // Log when count<2 with details on which entry failed which
        // condition. This captures what case-200 will see when it runs
        // inside this update_game call, so we can pinpoint why the t4
        // walk false-trips on StudioS games (whereas WW always shows 2).
        static const char* env_t4probe = std::getenv("FM2K_T4_PROBE");
        if (game_mode >= 3000 && game_mode < 4000
            && env_t4probe && std::strcmp(env_t4probe, "1") == 0)
        {
            const uint8_t* pool = (const uint8_t*)0x4701E0;
            constexpr uintptr_t HP_BASE   = 0x4DFC85;
            constexpr uintptr_t HP_STRIDE = 57407;
            int count = 0;
            int t4_seen = 0;
            int fail_flag = 0, fail_hp = 0, fail_slot = 0;
            uint32_t fail_e[4] = {0,0,0,0};
            uint32_t fail_why[4] = {0,0,0,0};  // 1=flag, 2=hp, 3=slot
            int fail_n = 0;
            for (int i = 0; i < 1024; i++) {
                const uint8_t* e = pool + i * 382;
                uint32_t type = *(const uint32_t*)(e + 0);
                if (type != 4) continue;
                t4_seen++;
                uint32_t flag346 = *(const uint32_t*)(e + 346);
                uint32_t slot    = *(const uint32_t*)(e + 342);
                if (flag346 != 0) {
                    fail_flag++;
                    if (fail_n < 4) { fail_e[fail_n]=i; fail_why[fail_n]=1; fail_n++; }
                    continue;
                }
                if (slot >= 8) {
                    fail_slot++;
                    if (fail_n < 4) { fail_e[fail_n]=i; fail_why[fail_n]=3; fail_n++; }
                    continue;
                }
                uint32_t hp = *(const uint32_t*)(HP_BASE + slot * HP_STRIDE);
                if (hp == 0) {
                    fail_hp++;
                    if (fail_n < 4) { fail_e[fail_n]=i; fail_why[fail_n]=2; fail_n++; }
                    continue;
                }
                count++;
            }
            static int s_last_count = -1;
            if (count < 2 && (count != s_last_count || (g_frame_counter % 60) == 0)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[T4-PROBE f=%u] count=%d t4_seen=%d "
                    "fails: flag@346=%d hp=%d slot=%d "
                    "first4=[e=%u why=%u, e=%u why=%u, e=%u why=%u, e=%u why=%u] "
                    "(why: 1=flag@+346!=0, 2=HP[slot]==0, 3=slot>=8)",
                    g_frame_counter, count, t4_seen,
                    fail_flag, fail_hp, fail_slot,
                    fail_e[0], fail_why[0], fail_e[1], fail_why[1],
                    fail_e[2], fail_why[2], fail_e[3], fail_why[3]);
            }
            s_last_count = count;
        }

        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // Spectator mode: the trampoline's RunSpectatorTick owns the sim drive
    // (it pops streamed inputs and calls original_update_game itself). This
    // hook still fires because update_game runs from inside that trampoline
    // call -- but we must not run any of the player-side battle-sync /
    // Netplay_StartBattle / GekkoStressSession paths below. Just bump the
    // frame counter and pass through to the real update_game.
    if (g_spectator_mode) {
        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // STRESS-TEST MODE (FM2K_STRESS_MODE=1) - single-instance determinism check
    // GekkoStressSession artificially rolls back every check_distance frames.
    // No network, no sync barriers. Menu/CSS run pass-through; battle mode
    // starts a GekkoStressSession and drives sim via the Save/Load/Advance
    // event loop (same path as online, minus the network).
    // Any desync fired here = local determinism bug. Pure repro.
    // ========================================================================
    if (g_stress_mode) {
        if (IsBattleMode(game_mode)) {
            if (!Netplay_IsActive()) {
                if (!Netplay_StartStressBattle()) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "Stress: Failed to start GekkoStressSession, falling through");
                    g_frame_counter++;
                    return original_update_game ? original_update_game() : 0;
                }
            }
            if (!Netplay_ProcessBattleInputPhase()) {
                return 0;
            }
            g_frame_counter++;
            return 0;
        }
        // Menu / CSS / results: run game normally
        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // SYNC BARRIER - Block game until both clients are connected
    // CCCaster-style: return 0 to freeze game at menu until connection
    // ========================================================================
    if (!Netplay_IsConnected()) {
        // Keep trying to connect
        static uint32_t last_poll = 0;
        static uint32_t block_count = 0;
        uint32_t now = GetTickCount();

        // Poll control channel to process HELLO/HELLO_ACK
        ControlChannel_Poll();

        // Send HELLO periodically until connected
        if (now - last_poll > 500) {
            ControlChannel_SendHello(static_cast<uint8_t>(g_player_index),
                                     fm2k::game_hash::Compute());
            last_poll = now;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SYNC BARRIER: Blocking game (P%d, mode=%u, blocked %u times)",
                g_player_index + 1, game_mode, block_count);
        }

        block_count++;

        // BLOCK GAME - return 0 to prevent any game state updates
        // This keeps both clients at the same starting point
        return 0;
    }

    // Log when we first pass the barrier
    static bool barrier_passed = false;
    if (!barrier_passed) {
        barrier_passed = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SYNC BARRIER PASSED: P%d connected, game_mode=%u, frame=%u",
            g_player_index + 1, game_mode, g_frame_counter);
    }

    // Check for game mode transitions (CSS <-> Battle)
    CheckGameModeTransition();

    // ========================================================================
    // CSS MODE - Delay-based with stall when remote is behind
    // Game loop calls: ProcessGameInputs -> UpdateGameState -> InputHistory
    // We must block ALL of them during stalls to prevent edge detection desync
    // ========================================================================
    if (IsCSSMode(game_mode)) {
        // ProcessCSS handles everything: poll, stall, capture, send batch.
        // Returns false if stalling (waiting for remote input + resending ours).
        if (!Netplay_ProcessCSS()) {
            return 0;  // Stall - don't update game state
        }

        g_frame_counter++;
        return original_update_game ? original_update_game() : 0;
    }

    // ========================================================================
    // BATTLE MODE - Sync barrier then GekkoNet rollback
    // ========================================================================
    if (IsBattleMode(game_mode)) {
        // ----------------------------------------------------------------
        // BATTLE SYNC BARRIER - Block until both clients enter battle mode
        // This ensures both start GekkoNet at the same frame
        // ----------------------------------------------------------------
        if (g_battle_entry_signaled && !Netplay_IsActive()) {
            // Poll for BATTLE_ENTERING from remote
            Netplay_PollBattleSync();

            if (!Netplay_IsBattleSynced()) {
                // Still waiting for remote - block game
                static uint32_t last_log = 0;
                uint32_t now = GetTickCount();
                if (now - last_log > 500) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "BATTLE SYNC BARRIER: Waiting for remote to enter battle mode...");
                    last_log = now;
                }
                return 0;  // Block game until synced
            }

            // Both clients synced - NOW start GekkoNet
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "BATTLE SYNC BARRIER PASSED: Starting GekkoNet session");
            if (Netplay_StartBattle()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GekkoNet session started for battle");
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to start GekkoNet session!");
            }
        }

        // ----------------------------------------------------------------
        // GekkoNet active - process rollback
        // ----------------------------------------------------------------
        if (Netplay_IsActive()) {
            // Process GekkoNet frame - runs full game ticks inside each AdvanceEvent
            // (process_game_inputs + update_game), matching GekkoNet examples.
            // We do NOT call original_update_game here - it already ran.
            if (!Netplay_ProcessBattleInputPhase()) {
                // No advance event yet - keep polling
                return 0;
            }

            // GekkoNet already ran the tick(s). Just update our frame counter.
            g_frame_counter++;
            return 0;  // Skip game loop's own update - already done
        }
    }

    // ========================================================================
    // OTHER MODES (menu, results, etc.)
    // ========================================================================
    Netplay_ProcessMenu();

    g_frame_counter++;
    return original_update_game ? original_update_game() : 0;
}

bool InstallUpdateHook() {
    // Hook UpdateGameState
    if (MH_CreateHook((void*)FM2K::ADDR_UPDATE_GAME, (void*)Hook_UpdateGameState,
                      (void**)&original_update_game) != MH_OK ||
        MH_QueueEnableHook((void*)FM2K::ADDR_UPDATE_GAME) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook UpdateGameState");
        return false;
    }

    return true;
}

