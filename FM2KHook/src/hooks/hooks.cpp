// Simplified hooks - detect battle mode transitions, delegate to netplay
// Sync barrier: block game until both clients connected (CCCaster-style)
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
// Value 0x1F80 is the x86 default but we pin it explicitly to ensure both
// peers use the same value regardless of prior state.
static inline void SetMXCSR(unsigned int v) {
    __asm__ volatile("ldmxcsr %0" : : "m"(v));
}

static inline void PinFPUControlWord() {
    unsigned int cur = 0;
    // x87: 53-bit precision, round-to-nearest-even, all exceptions masked.
    _controlfp_s(&cur, _PC_53 | _RC_NEAR | _MCW_EM,
                       _MCW_PC | _MCW_RC | _MCW_EM);
    // SSE: also pin MXCSR. FM2K's hit-detection and physics likely emit SSE
    // float ops under -mfpmath or vectorizer, and MXCSR rounding mode is
    // independent of the x87 control word. Both peers must agree.
    // #63 TEST: FM2K_FTZ_TEST=1 enables FZ(0x8000)+DAZ(0x0040) to check whether
    // denormal stalls in the game's render math are the Robot Heroes slowdown.
    static const unsigned int s_mxcsr = []{
        const char* v = std::getenv("FM2K_FTZ_TEST");
        return (v && v[0] == '1') ? 0x9FC0u : 0x1F80u;
    }();
    SetMXCSR(s_mxcsr);
}

// Deterministic timeGetTime: during an active GekkoNet battle session the
// return value is derived from the authoritative advance count, NOT wall
// clock. main_game_loop writes timeGetTime() into g_last_frame_time @
// 0x447DD4 every iteration, which lives inside our saved "afterimage_pool"
// region. If forward-sim wrote wall-clock T1 and replay-sim wrote T2 at
// the same frame, the saved afterimage_pool diverges by that timestamp
// byte -- this is the exact "REPLAY DIFF AfterimagePool +0x4A4" signature
// we caught at f=9 in the stress test.
//
// Virtual clock is advanced by 10 ms EACH TIME an AdvanceEvent completes
// (see netplay.cpp). Within a single main_game_loop iteration the game
// polls timeGetTime() multiple times -- we return the same value on every
// call until the next advance. Forward-sim and replay-sim both consume
// the same advance sequence, so both produce identical virtual timestamps
// at the same logical frame.
//
// Outside an active session we pass through -- menus/CSS rely on real wall
// clock for music/animation pacing, and determinism doesn't matter there.
extern bool Netplay_IsActive();
using timeGetTime_t = DWORD(WINAPI*)();
static timeGetTime_t original_timeGetTime = nullptr;

uint32_t g_virtual_time_ms = 0;  // bumped by 10 per AdvanceEvent in netplay.cpp

static DWORD WINAPI Hook_timeGetTime() {
    // Host: virtual clock during an active GekkoNet session so the per-peer
    // simulation evolves on a deterministic 10 ms/frame schedule.
    // Spectator: same -- must return virtual clock once playback is driving
    // the sim, otherwise game code that consumes timeGetTime (animations,
    // particle pacing, etc.) sees wall-clock time and diverges from the
    // host's recorded execution every single frame. RunSpectatorTick is
    // responsible for bumping g_virtual_time_ms each successful advance.
    if constexpr (FM2K::kIsFM2K) {
        if (Netplay_IsActive() || SpectatorNode_IsPlayingBack()) {
            return g_virtual_time_ms;
        }
    } else {
        // FM95: two regimes.
        //  - Loop OWNED (g_fm95_loop_owned): TrampolineMainLoop replaced CPW's
        //    WinMain loop, so there is no host pacing to protect -- virtualize
        //    exactly like FM2K (deterministic 10 ms/frame during any session).
        //  - Host-driven fallback: CPW's WinMain still runs its own timeGetTime
        //    pacing loop and calls our update hook from inside it. Virtualizing
        //    those pacing reads warps the loop (unsigned now-last arithmetic ->
        //    max-speed catchup or stall), so only hand back the virtual clock
        //    while a sim tick is actually running (g_fm95_in_sim_tick).
        const bool virtualize = g_fm95_loop_owned ? true : g_fm95_in_sim_tick;
        if (virtualize &&
            (Netplay_IsActive() || SpectatorNode_IsPlayingBack())) {
            return g_virtual_time_ms;
        }
    }
    return original_timeGetTime ? original_timeGetTime() : 0;
}

// ============================================================================
// CreateFile share-mode override
// ============================================================================
// FM2K opens character files (`.player`, etc.) with dwShareMode=0 -- exclusive.
// When two instances launch from the same folder, the second hits
// ERROR_SHARING_VIOLATION ("Player Open error[…]"). Force-OR the shared-read
// flags so multiple readers can coexist. Writes are still serialized by
// the OS -- we only widen sharing, never narrow it.
//
// Hooked at the kernel32 entry points; both A and W variants because old



// hand-off so CSS behavior is unchanged.
BOOL __cdecl Hook_RunGameLoop() {
    // Set VS player mode once -- FM2K-only: 0x470058 is the FM2K char-select
    // mode flag, not anything meaningful on FM95.
    if constexpr (FM2K::kIsFM2K) {
        static bool vs_mode_set = false;
        if (!vs_mode_set) {
            uint8_t* char_select_mode = (uint8_t*)0x470058;
            DWORD old_protect;
            if (VirtualProtect(char_select_mode, 1, PAGE_READWRITE, &old_protect)) {
                *char_select_mode = 1;  // VS player mode
                VirtualProtect(char_select_mode, 1, old_protect, &old_protect);
                vs_mode_set = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Set VS player mode");
            }
        }
    }

    // Diagnostic: FM2K_BYPASS_TRAMPOLINE=1 falls through to vanilla
    // main_game_loop. All other hooks (input, update, render, RNG) still
    // fire as detours, so we can isolate the trampoline as a cause vs the
    // individual hooks. Use only for offline tests -- netplay/spectator
    // require the trampoline's phase dispatcher to drive Save/Load/Advance.
    static const char* env_bypass = std::getenv("FM2K_BYPASS_TRAMPOLINE");
    static bool bypass = (env_bypass && std::strcmp(env_bypass, "1") == 0);
    if (bypass) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: FM2K_BYPASS_TRAMPOLINE=1 -- calling vanilla "
                    "main_game_loop. Trampoline phase dispatcher "
                    "DISABLED. Netplay/spectator will not work.");
        return original_run_game_loop ? original_run_game_loop() : TRUE;
    }

    return TrampolineMainLoop();
}


// Hook: ProcessGameInputs
// During battle: get synced inputs from GekkoNet and write to game memory
int __cdecl Hook_ProcessGameInputs() {
    // Re-pin the FPU control word on every game tick. DirectDraw's
    // SetCooperativeLevel is called without DDSCL_FPUPRESERVE, so DD is
    // allowed to mutate x87 precision at fullscreen toggle / driver callback
    // time. Without this line, two peers can run at different float
    // precision and float-heavy code (movement vectors, hit-rect math)
    // diverges on the first substantial physics tick -- which matches the
    // "desync starts when you move" signature exactly.
    PinFPUControlWord();

    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    // Stress-test mode: block game's own process_game_inputs during battle -
    // GekkoNet drives sim via AdvanceEvent (which calls original_process_game_inputs
    // internally). Outside battle, pass through normally.
    if (g_stress_mode) {
        if (IsBattleMode(game_mode) && Netplay_IsActive()) {
            return 0;
        }
        return original_process_game_inputs ? original_process_game_inputs() : 0;
    }

    // Battle mode with GekkoNet - block during sync, override inputs when active
    if (IsBattleMode(game_mode) && !g_offline_mode && Netplay_IsConnected()) {
        // Block ProcessGameInputs during battle sync barrier and GekkoNet handshake
        // Same reason as CSS: prevents buf_idx advance and edge detection desync
        if (!Netplay_IsActive() || !Netplay_IsSessionReady()) {
            return 0;
        }

        // GekkoNet active: ProcessBattleInputPhase handles process_game_inputs
        // inside each AdvanceEvent. Don't call original here - it would double-tick.
        // Just log periodically.
        static uint32_t log_count = 0;
        if (log_count < 10 || log_count % 200 == 0) {
            uint32_t p1_stored = *(uint32_t*)FM2K::ADDR_P1_INPUT;
            uint32_t p2_stored = *(uint32_t*)FM2K::ADDR_P2_INPUT;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[PROCESS_INPUTS] Synced: P1=0x%03X P2=0x%03X (buf_idx=%u)",
                p1_stored, p2_stored, *(uint32_t*)0x447EE0);
        }
        log_count++;

        return 0;  // Skip - GekkoNet drives input processing
    }

    // CSS mode - block ProcessGameInputs during stalls!
    // Game loop calls ProcessGameInputs BEFORE UpdateGameState.
    // If we let it run during stalls, it advances g_input_buffer_index
    // and runs edge detection out of sync between clients.
    if (IsCSSMode(game_mode) && Netplay_IsConnected()) {
        Netplay_PollCSS();  // Receive pending data

        if (!Netplay_CanAdvanceCSS()) {
            // STALL: Don't call original - prevents buffer index advance
            // and edge detection from consuming inputs during stall
            return 0;
        }

        // Not stalling - let original run (it calls GetPlayerInput which
        // returns synced CSS input through our hook)
        return original_process_game_inputs ? original_process_game_inputs() : 0;
    }

    // Connection barrier - block while waiting for connection
    // Prevents buf_idx divergence before game even starts
    if (!g_offline_mode && !Netplay_IsConnected()) {
        return 0;
    }

    // Offline or connected non-CSS/non-battle: use original
    return original_process_game_inputs ? original_process_game_inputs() : 0;
}

// ============================================================================
// HOOK SETUP
// ============================================================================

bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Initializing MinHook...");

    PinFPUControlWord();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: Pinned x87 FPU control word to _PC_53 | _RC_NEAR");

    // The locale spoof (InstallLocaleSpoof) already initializes MinHook on
    // FM95 builds and on any FM2K build with FM2K_JP_LOCALE=1, so accept
    // MH_ERROR_ALREADY_INITIALIZED as a no-op success here.
    {
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Hooks: MH_Initialize failed: %d", (int)s);
            return false;
        }
    }

    if (!InstallInputHooks()) return false;  // hooks_getinput.cpp

    if (!InstallUpdateHook()) return false;  // hooks_update.cpp
    if (!InstallRenderHooks()) return false;  // hooks_render.cpp

    // Hook RunGameLoop -- FM2K only. On FM95, ADDR_RUN_GAME_LOOP IS WinMain
    // (the frame loop is inlined into WinMain); detouring it intercepts the
    // process entry point BEFORE init runs, so the trampoline takes over
    // with an uninitialized window/game state and CPW silently dies.
    // Until the trampoline is taught to coexist with FM95's WinMain-driven
    // loop, leave the natural WinMain alone -- the per-frame hooks
    // (Hook_UpdateGameState / Hook_RenderGame / Hook_ProcessGameInputs)
    // still fire from inside FM95's loop and that's enough to drive a
    // basic boot.
    if constexpr (FM2K::kIsFM2K) {
        if (MH_CreateHook((void*)FM2K::ADDR_RUN_GAME_LOOP, (void*)Hook_RunGameLoop,
                          (void**)&original_run_game_loop) != MH_OK ||
            MH_QueueEnableHook((void*)FM2K::ADDR_RUN_GAME_LOOP) != MH_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook RunGameLoop");
            return false;
        }
    } else {
#if defined(ENGINE_FM95)
        // CPW's frame loop is inlined into WinMain, so there is no function
        // entry to detour. Own it with an inline patch at the loop head
        // (0x40AD67) -- TrampolineMainLoop then drives pacing + gekko exactly
        // like FM2K. If the patch fails we leave g_fm95_loop_owned false and
        // fall back to the host-driven Hook_UpdateGameState path.
        if (Fm95InstallOwningLoop()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: FM95 owns the loop via inline patch @0x40AD67");
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: FM95 loop-patch FAILED -- falling back to host-driven tick");
        }
#endif
    }

    InstallVfsHooks();  // hooks_vfs.cpp (non-fatal hooks)

    if (!InstallRngHook()) return false;  // hooks_rng.cpp

    // Hook ProcessGameInputs
    if (MH_CreateHook((void*)FM2K::ADDR_PROCESS_INPUTS, (void*)Hook_ProcessGameInputs,
                      (void**)&original_process_game_inputs) != MH_OK ||
        MH_QueueEnableHook((void*)FM2K::ADDR_PROCESS_INPUTS) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook ProcessGameInputs");
        return false;
    }


    // C3.5 -- vs_round_function detour. Emits ROUND_START / ROUND_END
    // SessionEvents at the round-state-machine substate edges (host only;
    // FM95 builds compile to a no-op). Best-effort install: if the hook
    // fails we keep going so the rest of the engine works (round events
    // are diagnostic, not load-bearing).
    if (!RoundEvents_Install()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: RoundEvents_Install failed -- round events will be missing "
            "from session_events / .fm2krep round_offsets");
    }

    // CSS auto-lock-and-confirm -- installs idle, only activates when
    // SpectatorNode's MATCH_START apply path arms it for offline replay
    // playback (FM2K_REPLAY_FILE set). Live-spectator paths walk CSS via the
    // host's full input stream and don't need this. FM95 builds compile to a
    // no-op (its CSS state machine is structured differently -- separate
    // hand-off).
    if (!CssAutoConfirm_Install()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: CssAutoConfirm_Install failed -- offline replay will fall "
            "back to natural CSS traversal (likely picks wrong chars)");
    }
    // Test-harness CSS auto-confirm: arm the same auto-confirm path used
    // by .fm2krep replay, but driven by env var so a 2-instance loopback
    // netplay test can advance through CSS without keyboard input. Both
    // peers must see the SAME values for gekko CSS sync to land on the
    // same chars/stage. Format: FM2K_TEST_AUTO_CSS=p1char,p1color,p2char,p2color,stage
    // (decimal bytes). Default chars/colors/stage = 0 (= first option).
    // Test-harness FM2K_TEST_AUTO_CSS now ONLY enables the gekko input
    // pulse in Netplay_ProcessCSSInputPhase -- no direct CssAutoConfirm
    // memory pinning. CssAutoConfirm was designed for single-instance
    // offline replay; in 2-peer netplay it produced asymmetric CSS-state
    // transitions (P1 reached battle, P2 didn't) because gekko's
    // confirmed-input stream and CssAutoConfirm's direct-memory writes
    // race differently per peer. With pulse-only, both peers' engines
    // see the same gekko-delivered confirm rising edges and transition
    // CSS→battle through normal engine code, in lockstep.
    {
        const char* env = std::getenv("FM2K_TEST_AUTO_CSS");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: FM2K_TEST_AUTO_CSS env='%s' (pulse-only mode)",
            env ? env : "(null)");
    }

    // Per-game damage multiplier -- only installs the hook if
    // FM2K_DAMAGE_MULT_PCT is set and != 100, so default users don't pay
    // the trampoline cost on every damage event. FM95 build is a no-op.
    if constexpr (FM2K::kIsFM2K) {
        if (!PerGamePatches_InstallDamageMultiplierHook()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: damage multiplier hook install failed -- damage "
                "scaling won't apply this session");
        }
        // Option-A KOF retention: code-cave patch on the engine's
        // CSMK_PLAYER HP-init instruction so the engine never overwrites
        // the winner's slot with max_hp. Only installs when
        // FM2K_TEAM_KOF_RETENTION is enabled.
        if (!PerGamePatches_InstallKofHpInitPatch()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: KOF HP-init patch install failed -- retention "
                "will fall back to vanilla (HP resets between rounds)");
        }
        // /F boot-to-battle prime: MinHook on the slot-0 boot dispatcher
        // (InitializeGameFromCommandLine @ 0x409a60). At entry, restamps
        // the kgt name into g_iniFile_nameOverride -- counters hit_judge_-
        // set_function's earlier empty-default stomp. Only installs when
        // FM2K_BOOT_TO_BATTLE=1 (launcher dev checkbox).
        if (!PerGamePatches_InstallBootToBattleHook()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: boot-to-battle hook install failed -- /F path "
                "will fail with 'GameSystem Open error[]'");
        }
        // Story-init AI hijack: MidHook char_state_machine's
        // g_game_mode_flag dispatch read so battle-phase calls see
        // flag=0 (1P arcade) -- drives stage-script CPU AI init for
        // P2 in our hijacked 1P→VS-CSS modes. Idempotent; only
        // installs if option_mode_selector or one of the mode flags
        // was on at startup (PerGamePatches_ApplyRuntime ran first
        // in DllMain, so atomics are already populated).
        if (!PerGamePatches_InstallStoryInitHijack()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hooks: story-init hijack install failed -- VS CPU / "
                "Training will leave P2 as a non-AI standing dummy");
        }
    }

    // Hook timeGetTime (winmm.dll) -- make the game's frame-skip pacing
    // deterministic across peers. See comment on Hook_timeGetTime for the
    // rationale. Resolve the real address dynamically so the hook works
    // regardless of IAT layout.
    HMODULE winmm = GetModuleHandleA("winmm.dll");
    if (!winmm) winmm = LoadLibraryA("winmm.dll");
    if (winmm) {
        void* real_timeGetTime = (void*)GetProcAddress(winmm, "timeGetTime");
        if (real_timeGetTime) {
            if (MH_CreateHook(real_timeGetTime, (void*)Hook_timeGetTime,
                              (void**)&original_timeGetTime) != MH_OK ||
                MH_QueueEnableHook(real_timeGetTime) != MH_OK) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Hooks: Failed to hook timeGetTime");
                return false;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: timeGetTime hooked for deterministic frame pacing");
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: GetProcAddress(timeGetTime) failed");
        }
    }


    // CSS fast-sound: defer character DirectSound buffer creation until a sound
    // first plays (FM2K_FPK_CSS_FASTSOUND=1), killing the ~150ms per-hover dip
    // from rebuilding ~80 sound buffers on every CSS cursor move. Queues its
    // hooks here; applied in the batch below. See css_fastsound.cpp.
    CssFastSound_Install();

    // Single thread-freeze for every hook queued during this function and by
    // InstallLocaleSpoof/RoundEvents_Install/CssAutoConfirm_Install. One call
    // beats ~12 individual MH_EnableHook(target) freezes (~80–120ms each).
    MH_STATUS apply = MH_ApplyQueued();
    if (apply != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Hooks: MH_ApplyQueued failed: %d", (int)apply);
        return false;
    }

    // CSM dispatch-loop diagnostic. Off by default; FM2K_CSM_DIAG=1 installs
    // a SafetyHook MidHook at 0x412564 that dumps obj state per call. Used
    // by the replay-self-test bisect to find the char_dynamic field that
    // differs between host and replay at the script-divergence frame.
    extern void Hook_InstallCsmDiag();
    Hook_InstallCsmDiag();

    // Camera-operand diagnostic (task #34). FM2K_CAM_DIAG=1 installs a
    // MidHook at camera_manager's battle path logging the camera formula's
    // exact inputs per tick (record-vs-replay diff localizes the drift).
    extern void Hook_InstallCamDiag();
    Hook_InstallCamDiag();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: All hooks installed successfully");
    return true;
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Shutdown complete");
}





