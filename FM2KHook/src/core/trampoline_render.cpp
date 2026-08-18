// trampoline_render.cpp -- render snapshot/restore (RenderFrameWithSnapshot + EbDiag_Dump) + the render-side carve-out constants/buffers. Split from main_loop_trampoline.cpp.
#include "main_loop_trampoline.h"
#include "globals.h"
#include "../hooks/hooks.h"
#include "../hooks/wndproc_subclass.h"
#include "../hooks/background_mode.h"  // FM2K_TEST_BACKGROUND deferred demotion marker
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

// ============================================================================
// RENDER SNAPSHOT/RESTORE
// ============================================================================
// Render mutates sim-authoritative memory (object animation counters, RNG via
// ProcessShakeEffect / ProcessColorInterpolation, input-tracking fields).
// If render runs at wall-clock cadence it leaks extra mutations into the save
// state. We snapshot the affected regions, run render, then restore.
// Identical to what Hook_RenderGame used to do; moved here so the trampoline
// owns the render step end-to-end.
#include "../netplay/savestate.h"  // WaveCAddrs

uint64_t g_render_game_only_ns = 0;  // #63 diag: original_render_game() time, set by RenderFrameWithSnapshot
static uint8_t s_render_saved_afterimage[WaveCAddrs::AFTERIMAGE_POOL_SZ];
static uint8_t s_render_saved_input_tracking[0xA0];

// SHAKE_EFFECTS block (40 B at 0x447DA9..0x447DD1) must be carved out of
// the render-side afterimage snapshot so ProcessShakeEffect's per-render
// timer decrement (a1[3]--) reaches sim memory. The dev-scripted
// Duration in each character's KGT drives how long shake lasts; we need
// that countdown to propagate. Without this carve, the snapshot reverts
// the decrement every render, sim state is permanently pinned at
// timer == duration, and any character whose script re-SETs the opcode
// each frame rumbles forever.
constexpr uintptr_t SHAKE_EFFECTS_ADDR  = 0x447DA9;  // FM2K-only global (FM95 equivalent: RE-4); all consumers gated on kIsFM2K
constexpr size_t    SHAKE_EFFECTS_SZ    = 40;
constexpr size_t    SHAKE_OFFSET_IN_AI  = SHAKE_EFFECTS_ADDR - WaveCAddrs::AFTERIMAGE_POOL;  // 0x479

// EFFECT_SYS1 (palette flash 1 struct) sits inside the afterimage_pool slice
// at 0x447D7D, 42 B, immediately BEFORE the shake block. Render-side carve-
// out must skip this region too -- ProcessColorInterpolation writes
// per-frame interpolation values into g_object_data_ptr from inputs read out
// of EFFECT_SYS1, and the [EB] handler in update_game writes the struct's
// timer/duration. If the render-side restore stomps these every render, the
// palette flash either snaps back to its pre-render state (visible flicker)
// or never reflects the current sim's progress.
constexpr uintptr_t PFLASH1_ADDR        = 0x447D7D;  // FM2K-only global (FM95 equivalent: RE-4); all consumers gated on kIsFM2K
constexpr size_t    PFLASH1_SZ          = 42;
constexpr size_t    PFLASH1_OFFSET_IN_AI = PFLASH1_ADDR - WaveCAddrs::AFTERIMAGE_POOL;
static_assert(PFLASH1_OFFSET_IN_AI + PFLASH1_SZ <= SHAKE_OFFSET_IN_AI,
              "EFFECT_SYS1 must end before shake block");

// [EB] diagnostic -- see header for full doc. Defined here so both the
// trampoline path and Hook_RenderGame share state (frame counter, log fp,
// post-shake window). Only one of those two paths fires per frame depending
// on FM2K_BYPASS_TRAMPOLINE: trampoline mode → RenderFrameWithSnapshot calls
// us; bypass mode → Hook_RenderGame (the MinHook detour) calls us.
void EbDiag_Dump(const char* tag) {
    // [FM2K-DIAG] Every address below (palette-flash structs, SHAKE block,
    // g_screen_x/y) is an FM2K global; FM95 equivalents are unmapped --
    // RE-4 (palette flash / shake) + RE-5 (camera/scroll) in
    // docs/FM95_Support_Status.md. Whole dump is FM2K-only.
    if constexpr (!FM2K::kIsFM2K) { (void)tag; return; }
    static const bool s_eb_diag = []() {
        const char* v = std::getenv("FM2K_EB_DIAG");
        return v && v[0] == '1';
    }();
    if (!s_eb_diag) return;

    static FILE* s_eb_diag_fp = nullptr;
    static uint32_t s_eb_frame = 0;
    static int s_eb_post_window = 0;

    const uint32_t s1_mode  = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 0);
    const uint32_t s1_off   = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 4);
    const uint32_t s1_amp   = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 8);
    const uint32_t s1_timer = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 12);
    const uint32_t s1_dur   = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 16);
    const uint32_t s2_off   = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 4 + 20);
    const uint32_t s2_timer = *(uint32_t*)(SHAKE_EFFECTS_ADDR + 12 + 20);
    // Palette flash state -- same [EB] opcode handler, separate timers.
    // p1: g_effect_id_1 @ 0x447D7D; timer (a1[5]) @ +0x14 (g_timer_countdown2 0x447D91)
    // p2: g_effect_id_2 @ 0x4456D0; timer (a1[5]) @ +0x14 (g_timer_countdown1 0x4456E4)
    // dur (a1[10]) @ +0x28
    const uint32_t p1_mode  = *(uint32_t*)0x447D7D;  // g_effect_id_1 (FM2K, RE-4)
    const uint32_t p1_timer = *(uint32_t*)0x447D91;  // g_timer_countdown2 (FM2K, RE-4)
    const uint32_t p1_dur   = *(uint32_t*)0x447DA5;  // 0x447D7D + 0x28
    const uint32_t p2_mode  = *(uint32_t*)0x4456D0;  // g_effect_id_2 (FM2K, RE-4)
    const uint32_t p2_timer = *(uint32_t*)0x4456E4;  // g_timer_countdown1 (FM2K, RE-4)
    const uint32_t p2_dur   = *(uint32_t*)0x4456F8;  // 0x4456D0 + 0x28
    // Global RNG seed -- ProcessColorInterpolation mode 3 calls game_rand()
    // each render. Comparing rng across vanilla (FM2K_BYPASS_TRAMPOLINE=1)
    // vs our trampoline path tells us whether render-time RNG matches
    // vanilla. If rng differs, palette colors will visibly differ even
    // when timer/duration are identical.
    const uint32_t rng_seed = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    const bool active = (s1_timer != 0 || s2_timer != 0
                         || p1_timer != 0 || p2_timer != 0);

    // Bump the per-render-boundary frame counter on PRE-SAVE only so all four
    // tags in a single render cycle share the same `f=` value.
    const bool is_pre_save = (tag[0] == 'P' && tag[1] == 'R' && tag[2] == 'E'
                              && tag[4] == 'S');
    if (is_pre_save) ++s_eb_frame;

    if (active) s_eb_post_window = 60;  // ~0.6s of post-shake camera tracking
    else if (s_eb_post_window > 0 && is_pre_save) --s_eb_post_window;
    else if (s_eb_post_window == 0 && !active) return;

    // g_screen_x/y at 0x447F2C/30 -- sprite_rendering_engine reads these for
    // every sprite (stage AND characters). If they drift after shake ends,
    // that's the stage-offset bug.
    const int32_t scr_x = *(int32_t*)0x447F2C;  // g_screen_x (FM2K, RE-5)
    const int32_t scr_y = *(int32_t*)0x447F30;  // g_screen_y (FM2K, RE-5)
    if (!s_eb_diag_fp) {
        char base[64];
        std::snprintf(base, sizeof(base),
                      "FM2K_eb_diag_pid%lu.log",
                      (unsigned long)GetCurrentProcessId());
        char path[MAX_PATH];
        if (!Fm2k_BuildLogPath(path, sizeof(path), base)) {
            std::snprintf(path, sizeof(path), "%s", base);
        }
        s_eb_diag_fp = std::fopen(path, "w");
        if (s_eb_diag_fp) {
            std::fprintf(s_eb_diag_fp,
                "# tag, frame, shake_s1{mode,off,amp,tmr,dur}, shake_s2{off,tmr}, "
                "palette_p1{m,tmr,dur}, palette_p2{m,tmr,dur}, rng, scr_x, scr_y\n");
            std::fflush(s_eb_diag_fp);
        }
    }
    if (s_eb_diag_fp) {
        std::fprintf(s_eb_diag_fp,
            "%-12s f=%u s1{m=%u o=%d amp=%u tmr=%u dur=%u} s2{o=%d tmr=%u} "
            "p1{m=%u tmr=%u dur=%u} p2{m=%u tmr=%u dur=%u} rng=%08x scr=(%d,%d)%s\n",
            tag, s_eb_frame,
            s1_mode, (int32_t)s1_off, s1_amp, s1_timer, s1_dur,
            (int32_t)s2_off, s2_timer,
            p1_mode, p1_timer, p1_dur,
            p2_mode, p2_timer, p2_dur,
            rng_seed, scr_x, scr_y,
            active ? "" : " [post-shake]");
        // Flush only on POST-RESTORE so a crash mid-frame leaves the log
        // readable. One fflush per frame instead of four.
        if (tag[0] == 'P' && tag[1] == 'O' && tag[5] == 'R' && tag[6] == 'E') {
            std::fflush(s_eb_diag_fp);
        }
    }
}

void RenderFrameWithSnapshot() {
    if (!original_render_game) return;

    // FPS counter + title-bar stats (hotkey probe too). Hook_RenderGame used
    // to do this; in trampoline mode render goes through here instead.
    Hook_RenderDiagnostics_Tick();

    // Per-frame state dump during battle-transition windows. Cheap (single
    // counter check per frame); only emits log lines for ~3 sec around
    // every CSS↔battle boundary.
    Hook_BattleDiag_TickIfActive();

    // Render isolates sim state when we're driving the simulation
    // deterministically -- either as a player under GekkoNet (host) or as
    // a spectator replaying confirmed inputs. Without protection, render's
    // mutations to RNG / object pool / afterimage / input tracking leak into
    // sim memory, and the spectator's evolution diverges from the host's
    // (which IS protected). One frame is enough to desync RNG.
    const bool protect = Netplay_IsActive() || SpectatorNode_IsPlayingBack();
    EbDiag_Dump("PRE-SAVE");
    // Snapshot regions are FM2K carve-outs (afterimage pool / palette flash /
    // shake / input-tracking block). FM95 has no afterimage subsystem
    // (WaveCAddrs 0-stubs; palette-flash/shake pending RE-4) and the FM95
    // input-tracking snapshot is covered by savestate_fm95 regions; port
    // point: RE input ring layout. Both the save block here and the restore
    // block below are gated on kIsFM2K so FM95 never touches these addresses.
    if (protect) { if constexpr (FM2K::kIsFM2K) {
        // RNG is intentionally NOT save+restored across render anymore.
        // Render-side game_rand() calls (ProcessColorInterpolation mode 3,
        // ProcessShakeEffect mode 4, sprite effects) need to propagate to
        // sim memory or palette flash visuals freeze on a single static
        // interpolation factor instead of the animated random gradient
        // vanilla shows. Both peers run render once per wall-clock frame
        // and consume identical RNG amounts, so they stay in lockstep
        // without explicit RNG restore. Verified via offline/bypass diff --
        // offline (no protection) matches vanilla; online (with protection)
        // showed RNG frozen at the pre-render value across 20+ frames.
        //
        // Object pool is also intentionally NOT saved/restored. The render
        // path writes per-object color override values via
        // ProcessColorInterpolation (g_object_data_ptr + 68/72/76/80) AND
        // various sprite/animation timers. Reverting those after render
        // makes palette flash mode 1 (Tyrogue fade-to-black) visually
        // "undone" -- sim-side timer keeps decrementing, but the persistent
        // color state needed across frames gets wiped. Both peers run the
        // same renders, so object pool drift stays symmetric.
        //
        // "Both peers run the same renders" is a PREMISE, and it was violated
        // during character select until 2026-08-17: `protect` above is
        // Netplay_IsActive() || SpectatorNode_IsPlayingBack(), and
        // Netplay_IsActive() is BATTLE-only, so on the two player planes every
        // CSS render ran with the afterimage pool and the 0x447EE0
        // input-tracking block unprotected -- while the pre-rendezvous
        // free-run gave one peer 19-34 more CSS renders than the other
        // (css_child_asymmetry.md hazard 1). The pre-rendezvous CSS park
        // (netplay_css.cpp) restores the premise for that window by
        // construction: a parked tick returns before RenderFrameWithSnapshot,
        // so both peers enter CSS lockstep having run the SAME number of CSS
        // renders (zero), and inside the lockstep window RunCssTick is 1:1
        // sim:render (a stalled tick skips both). The unprotected-render
        // exposure itself is unchanged in code and still applies to the NATIVE
        // (title/menu) phase, which no barrier makes frame-equal.
        // Afterimage save: three slices skipping both EFFECT_SYS1 (palette
        // flash 1) and SHAKE_EFFECTS so render-side state evolution for
        // each propagates back into sim memory:
        //   [0                     .. PFLASH1_OFFSET)   -- save (head)
        //   [PFLASH1_OFFSET        .. PFLASH1_END)      -- SKIP (palette flash 1)
        //   [PFLASH1_END           .. SHAKE_OFFSET)     -- save (gap, currently 0 B)
        //   [SHAKE_OFFSET          .. SHAKE_END)        -- SKIP (shake block)
        //   [SHAKE_END             .. POOL_END)         -- save (tail)
        constexpr size_t PFLASH1_END_IN_AI = PFLASH1_OFFSET_IN_AI + PFLASH1_SZ;
        constexpr size_t SHAKE_END_IN_AI   = SHAKE_OFFSET_IN_AI + SHAKE_EFFECTS_SZ;
        memcpy(s_render_saved_afterimage,
               (void*)WaveCAddrs::AFTERIMAGE_POOL,
               PFLASH1_OFFSET_IN_AI);
        memcpy(s_render_saved_afterimage + PFLASH1_END_IN_AI,
               (void*)(WaveCAddrs::AFTERIMAGE_POOL + PFLASH1_END_IN_AI),
               SHAKE_OFFSET_IN_AI - PFLASH1_END_IN_AI);
        memcpy(s_render_saved_afterimage + SHAKE_END_IN_AI,
               (void*)(WaveCAddrs::AFTERIMAGE_POOL + SHAKE_END_IN_AI),
               WaveCAddrs::AFTERIMAGE_POOL_SZ - SHAKE_END_IN_AI);
        memcpy(s_render_saved_input_tracking, (void*)0x447EE0,                     0xA0);
    } }

    EbDiag_Dump("PRE-RENDER");
    // Render RNG isolation: re-seed the render stream from the gameplay seed,
    // then route render's game_rand draws to it (see globals.h / Hook_GameRand).
    // Render never advances the gameplay seed -> rollback/cross-peer
    // determinism; render colors stay deterministic per confirmed frame.
    g_render_rng_seed = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    g_in_render_rng = true;
    // #63 diag: time original_render_game alone vs the wrapper (diagnostics).
    extern uint64_t g_render_game_only_ns;
    const uint64_t _rg0 = SDL_GetPerformanceCounter();
    original_render_game();
    // #63 TEST: FM2K_RENDER_STALL_US busy-waits to SIMULATE a heavy-stage
    // render (e.g. Robot Heroes Aubeclisse ~13ms) on a light game, so the
    // netplay/offline frame-skip catch-up can be exercised + determinism-
    // verified in the loopback harness (which runs light WonderfulWorld).
    {
        static const long s_stall_us = []{ const char* v = std::getenv("FM2K_RENDER_STALL_US");
                                           return (v && v[0]) ? std::atol(v) : 0L; }();
        if (s_stall_us > 0) {
            const uint64_t freq = SDL_GetPerformanceFrequency();
            const uint64_t end = SDL_GetPerformanceCounter() + (uint64_t)s_stall_us * freq / 1000000ull;
            while (SDL_GetPerformanceCounter() < end) { /* busy-wait */ }
        }
    }
    g_render_game_only_ns = SDL_GetPerformanceCounter() - _rg0;
    g_in_render_rng = false;

    // FM2K_TEST_BACKGROUND: THIS is the observable marker the deferred minimize
    // waits on. A returned original_render_game() means the engine completed a
    // full frame through DirectDraw -- i.e. cnc-ddraw's renderer context exists
    // and has presented. Minimizing before that point can make D3D9 device
    // creation fail against a zero-size client area and drop cnc-ddraw into its
    // software-blit fallback ("-WARNING- Using slow software rendering" in the
    // title bar), which distorts frame pacing. No-op except on the one tick that
    // performs the demotion, and entirely absent unless the flag is set.
    FM2KBackground::OnFrameRendered();

    // Render sub-profiler report (FM2K_RENDER_PROFILE=1). Decomposes the heavy-
    // stage render cost into blit (per-sprite pixel push, timed in
    // Hook_BlitSpriteWithBlendMode) vs residual = render_game-total - blit
    // (the full-screen blur case -10 + RLEDecompress + per-sprite LUT rebuild +
    // ddraw tail). blit calls / g_object_count reveals afterimage-trail
    // multiplication; the blend-mode mix shows which blit modes (additive/alpha
    // are ~5x copy) to SIMD first. Display-only -- no sim/determinism impact.
    {
        static const bool s_rp_on = []{ const char* v = std::getenv("FM2K_RENDER_PROFILE");
                                        return v && v[0] == '1'; }();
        if (s_rp_on) {
            extern volatile uint32_t g_rp_blit_calls;
            extern volatile uint64_t g_rp_blit_ns;
            extern volatile uint64_t g_rp_blit_area;
            extern volatile uint32_t g_rp_blit_mode[5];
            const uint64_t freq = SDL_GetPerformanceFrequency();
            static uint32_t s_n = 0;
            static uint64_t s_rg_ns = 0;
            static uint64_t s_last_calls = 0, s_last_ns = 0, s_last_area = 0;
            static uint64_t s_last_mode[5] = {0, 0, 0, 0, 0};
            s_rg_ns += g_render_game_only_ns;
            if (++s_n >= 300) {
                auto ms = [freq](uint64_t t){ return (double)t * 1000.0 / (double)freq; };
                const uint64_t d_calls = (uint64_t)g_rp_blit_calls - s_last_calls;
                const uint64_t d_ns    = g_rp_blit_ns - s_last_ns;
                const uint64_t d_area  = g_rp_blit_area - s_last_area;
                uint64_t d_mode[5];
                for (int i = 0; i < 5; ++i) d_mode[i] = (uint64_t)g_rp_blit_mode[i] - s_last_mode[i];
                // g_object_count -- FM2K global; FM95 equivalent unmapped (RE-5), report 0 there.
                const uint32_t objs   = FM2K::kIsFM2K ? *(volatile uint32_t*)0x4246FC : 0u;
                const double rg_ms    = ms(s_rg_ns) / s_n;
                const double blit_ms  = ms(d_ns) / s_n;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[RENDER-PROF] n=%u objs=%u render_game=%.2fms/f | blit %.0f calls/f "
                    "area=%.0fkpx/f time=%.2fms/f | residual(blur+rle+lut+tail)=%.2fms/f | "
                    "blend copy=%llu half=%llu add=%llu sub=%llu alpha=%llu (calls/300)",
                    s_n, objs, rg_ms,
                    (double)d_calls / s_n, (double)d_area / s_n / 1000.0, blit_ms,
                    rg_ms - blit_ms,
                    (unsigned long long)d_mode[0], (unsigned long long)d_mode[1],
                    (unsigned long long)d_mode[2], (unsigned long long)d_mode[3],
                    (unsigned long long)d_mode[4]);
                s_last_calls = g_rp_blit_calls;
                s_last_ns    = g_rp_blit_ns;
                s_last_area  = g_rp_blit_area;
                for (int i = 0; i < 5; ++i) s_last_mode[i] = g_rp_blit_mode[i];
                s_rg_ns = 0;
                s_n = 0;
            }
        }
    }

    EbDiag_Dump("POST-RENDER");

    // FM2K-only restore -- mirrors the gated save block above (FM95: RE-4 /
    // savestate_fm95-covered input tracking).
    if (protect) { if constexpr (FM2K::kIsFM2K) {
        // (RNG restore removed -- see PRE-RENDER comment above.)
        // (Object pool restore removed too -- see PRE-RENDER comment.
        // Tyrogue's mode-1 fade-to-black depends on the last per-frame
        // ProcessColorInterpolation write to g_object_data_ptr+68/72/76/80
        // PERSISTING into the next frame's object_pool. When the timer
        // hits 0, ProcessColorInterpolation skips the write -- and the
        // sprite render reads the persisted last-frame value (mostly
        // black) so the screen stays black. With object_pool restore,
        // that persistence is wiped each frame and the visual snaps back
        // to the default palette.)
        // Afterimage restore: mirror of the 3-slice split save.
        // EFFECT_SYS1 + shake_effects regions in live memory keep whatever
        // ProcessColorInterpolation / ProcessShakeEffect just wrote.
        constexpr size_t PFLASH1_END_IN_AI = PFLASH1_OFFSET_IN_AI + PFLASH1_SZ;
        constexpr size_t SHAKE_END_IN_AI   = SHAKE_OFFSET_IN_AI + SHAKE_EFFECTS_SZ;
        memcpy((void*)WaveCAddrs::AFTERIMAGE_POOL,
               s_render_saved_afterimage,
               PFLASH1_OFFSET_IN_AI);
        memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + PFLASH1_END_IN_AI),
               s_render_saved_afterimage + PFLASH1_END_IN_AI,
               SHAKE_OFFSET_IN_AI - PFLASH1_END_IN_AI);
        memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + SHAKE_END_IN_AI),
               s_render_saved_afterimage + SHAKE_END_IN_AI,
               WaveCAddrs::AFTERIMAGE_POOL_SZ - SHAKE_END_IN_AI);
        memcpy((void*)0x447EE0,                    s_render_saved_input_tracking, 0xA0);
    } }
    EbDiag_Dump("POST-RESTORE");

    // (PostRenderRng back-patch REMOVED.) It existed because render-side
    // game_rand calls advanced the shared gameplay seed, so the saved slot
    // had to be patched to post-render rng for forward/replay to agree. With
    // render RNG now isolated to g_render_rng_seed (see Hook_GameRand), render
    // no longer touches the gameplay seed at all -- the saved seed is already
    // the pure sim rng, identical across peers. No patch needed, and the
    // patch was what made cross-peer rng diverge under real rollback.

    SharedMem_Update();
}

