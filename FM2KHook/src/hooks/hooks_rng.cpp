// hooks_rng.cpp -- RNG-call trace + Hook_GameRand (render-RNG isolation).
// Split from hooks.cpp (pure move + per-cluster InstallRngHook).

#include "hooks.h"
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
#include "hooks_internal.h"

// RNG-call trace -- gated on FM2K_RNG_TRACE=1 env var. Each call records
// (call_index, caller_pc, rng_pre, rng_post) as 16-byte little-endian
// records to FM2K_rng_trace_pid<PID>.bin in cwd. Used to diff host vs
// spectator processes and find the first divergent rng call site.
//
// Off-by-default (no env var): trampoline branch is just two predictable
// loads + a never-taken branch, single-digit nanoseconds added to the hook.
namespace {
static FILE*    g_rng_trace_fp        = nullptr;
static uint64_t g_rng_call_index      = 0;
static bool     g_rng_trace_resolved  = false;
static bool     g_rng_trace_enabled   = false;
static uint32_t g_rng_trace_max_calls = 0;  // 0 = unlimited

static void RngTrace_ResolveOnce() {
    if (g_rng_trace_resolved) return;
    g_rng_trace_resolved = true;
    const char* env = std::getenv("FM2K_RNG_TRACE");
    g_rng_trace_enabled = (env && std::strcmp(env, "1") == 0);
    const char* env_max = std::getenv("FM2K_RNG_TRACE_MAX");
    if (env_max) {
        g_rng_trace_max_calls = (uint32_t)std::strtoul(env_max, nullptr, 10);
    } else {
        g_rng_trace_max_calls = 2'000'000;  // ~32 MB cap default
    }
    if (g_rng_trace_enabled) {
        char base[64];
        std::snprintf(base, sizeof(base),
                      "FM2K_rng_trace_pid%lu.bin",
                      (unsigned long)GetCurrentProcessId());
        char path[MAX_PATH];
        if (!Fm2k_BuildLogPath(path, sizeof(path), base)) {
            std::snprintf(path, sizeof(path), "%s", base);
        }
        g_rng_trace_fp = std::fopen(path, "wb");
        if (g_rng_trace_fp) {
            // Larger buffer keeps the per-call overhead amortized.
            std::setvbuf(g_rng_trace_fp, nullptr, _IOFBF, 1 << 20);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "RngTrace: enabled -- writing to %s (max=%u calls)",
                        path, g_rng_trace_max_calls);
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "RngTrace: failed to open %s for write", path);
            g_rng_trace_enabled = false;
        }
    }
}
}

// Public API: flush the rng-trace FILE buffer. Called from netplay.cpp's
// auto-terminate path before TerminateProcess, so the trace records that
// are still in stdio's user-space buffer reach disk. Safe no-op when the
// trace isn't enabled.
void Hook_FlushRngTrace() {
    if (g_rng_trace_fp) {
        std::fflush(g_rng_trace_fp);
    }
}

// Hook: GameRand
// Records the call to the trace file (if enabled) then forwards.
uint32_t __cdecl Hook_GameRand() {
    // Render RNG stream isolation (see globals.h). During render_game, draw
    // from g_render_rng_seed via a seed-swap so render's color/effect
    // randomness NEVER advances the gameplay seed (0x41FB1C). That keeps the
    // gameplay rng cadence-independent -> deterministic across rollback and
    // across peers (the root fix for the palette/afterimage desync + crash).
    // The render seed is re-seeded from the gameplay seed each render, so
    // colors stay deterministic per confirmed frame and identical on both
    // peers. Render rng is intentionally NOT traced (it's a separate stream).
    if (g_in_render_rng) {
        ++g_render_rand_calls;  // #63 diag: count render-side rng draws
        const uint32_t gameplay = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = g_render_rng_seed;
        const uint32_t r = original_game_rand ? original_game_rand() : 0;
        g_render_rng_seed = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = gameplay;  // gameplay seed untouched
        return r;
    }
    // Gameplay-seed draw (render isolation NOT active). Count for the
    // per-frame [FULLFP] gp= tally used by the mid-join spectate desync hunt.
    ++g_gameplay_rand_calls;
    // Classify the caller (return addr) into the 7 known game_rand callers so
    // [FULLFP] fn= shows which function's per-frame draw count diverges.
    {
        const uint32_t ra = (uint32_t)(uintptr_t)__builtin_return_address(0);
        if      (ra >= 0x40af30 && ra < 0x40b467) ++g_gp_rand_by_fn[0]; // camera_manager
        else if (ra >= 0x40c9e0 && ra < 0x40ca7c) ++g_gp_rand_by_fn[1]; // ProcessShakeEffect
        else if (ra >= 0x40ca90 && ra < 0x40cc30) ++g_gp_rand_by_fn[2]; // ProcessColorInterpolation
        else if (ra >= 0x40cc30 && ra < 0x40e3e8) ++g_gp_rand_by_fn[3]; // sprite_rendering_engine
        else if (ra >= 0x40f010 && ra < 0x40f90d) ++g_gp_rand_by_fn[4]; // hit_detection_system
        else if (ra >= 0x411270 && ra < 0x4117f0) ++g_gp_rand_by_fn[5]; // ai_input_processor
        else if (ra >= 0x411bf0 && ra < 0x413bd6) ++g_gp_rand_by_fn[6]; // character_state_machine
        else                                      ++g_gp_rand_by_fn[7]; // other
    }
    RngTrace_ResolveOnce();
    if (!g_rng_trace_enabled) {
        return original_game_rand ? original_game_rand() : 0;
    }
    // Read direct caller (0x004139A8 wrapper). For the caller-OF-caller
    // (which would identify the actual game function), walk the frame
    // pointer chain manually with a guard. __builtin_return_address(1)
    // AVs inside MinHook's trampoline because the trampoline doesn't
    // preserve EBP. Instead we read EBP, dereference for the caller's
    // saved EBP, then read [EBP+4] for the caller-of-caller's return
    // address. Wrapped with IsBadReadPtr to avoid AV on broken chains.
    const uint32_t ret_addr = (uint32_t)(uintptr_t)__builtin_return_address(0);
    uint32_t ret_addr_2 = 0;
    {
        uintptr_t cur_bp = (uintptr_t)__builtin_frame_address(0);
        // Walk one frame up: *(bp) = caller's saved bp; *(bp+4) = caller's return addr
        // Then walk one more: *(caller_bp) = caller-of-caller's saved bp; *(caller_bp+4) = its return
        // We want caller-of-caller's return addr.
        if (cur_bp && !IsBadReadPtr((void*)cur_bp, 8)) {
            uintptr_t caller_bp = *(uintptr_t*)cur_bp;
            if (caller_bp && !IsBadReadPtr((void*)caller_bp, 8)) {
                ret_addr_2 = *(uint32_t*)(caller_bp + 4);
            }
        }
    }
    const uint32_t pre  = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    const uint32_t r    = original_game_rand ? original_game_rand() : 0;
    const uint32_t post = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    if (g_rng_trace_fp &&
        (g_rng_trace_max_calls == 0 || g_rng_call_index < g_rng_trace_max_calls))
    {
        uint32_t rec[6] = {
            (uint32_t)g_rng_call_index, ret_addr, ret_addr_2, pre, post, 0,
        };
        std::fwrite(rec, 1, sizeof(rec), g_rng_trace_fp);
        if ((g_rng_call_index & 0x1FFFu) == 0) std::fflush(g_rng_trace_fp);
    }
    ++g_rng_call_index;
    return r;
}

bool InstallRngHook() {
    if (MH_CreateHook((void*)FM2K::ADDR_GAME_RAND, (void*)Hook_GameRand,
                      (void**)&original_game_rand) != MH_OK ||
        MH_QueueEnableHook((void*)FM2K::ADDR_GAME_RAND) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook GameRand");
        return false;
    }
    return true;
}

