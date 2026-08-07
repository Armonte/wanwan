// trampoline_pacing.cpp -- frame pacing (SleepToTarget) + offline/frametime profilers. Split from main_loop_trampoline.cpp.
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

// Sleep until at least `target_ms` have elapsed since `start_qpc`. QPC-based
// for sub-ms accuracy: SDL_Delay/Sleep on Windows overshoots by 1-2 ms even
// with timeBeginPeriod(1) active, which caps the frame loop at ~90 FPS on a
// 10 ms target. Strategy: coarse SDL_Delay until within 2 ms of the
// deadline, then busy-wait the remainder using QueryPerformanceCounter.
// Burns a fraction of a millisecond of CPU at the tail of each frame in
// exchange for actual-100-FPS pacing.
void SleepToTarget(uint64_t start_qpc, uint32_t target_ms,
                          float frames_ahead) {
    const uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t target_ticks_count = (freq * target_ms) / 1000;
    // GekkoNet drift correction. Gekko emits no pacing of its own
    // (UpdateSession always advances; there is no Wait/Advise event), so
    // this wall-clock slow-down of the AHEAD peer is the ONLY lever that
    // lets the lagging peer catch up and shrink local_adv. Scale the brake
    // with how far ahead we are so a multi-frame start gap bleeds off in
    // ~25-35 frames instead of ~60, while a steady-state wobble keeps about
    // the old gentle nudge. Cap +3.0ms/frame (>=77fps floor) -- MUST stay
    // under one 10ms frame so we never invert the rift or stall a confirmed
    // advance. Matches the canonical pattern in
    // vendored/GekkoNet/Examples/OnlineSession/OnlineSession.cpp.
    // Without this the host's frame loop holds rigid 10 ms while the
    // client falls 7-8 frames behind permanently -- the symptom of
    // "host says rb=300, client says rb=0, ahead pinned at +-8.5".
    if (frames_ahead > 0.5f) {
        float extra_ms = frames_ahead * 0.5f;
        if (extra_ms > 3.0f) extra_ms = 3.0f;
        target_ticks_count += (uint64_t)((double)freq * (double)extra_ms / 1000.0);
    }
    const uint64_t target_ticks = start_qpc + target_ticks_count;
    for (;;) {
        uint64_t now = SDL_GetPerformanceCounter();
        if (now >= target_ticks) return;
        uint64_t remaining_ticks = target_ticks - now;
        uint64_t remaining_ms = (remaining_ticks * 1000) / freq;
        if (remaining_ms > 2) {
            // Sleep all but the last ~1.5ms worth -- leaves overshoot headroom.
            SDL_Delay((uint32_t)(remaining_ms - 1));
        }
        // Fall through and re-check; the tail gets busy-waited.
    }
}

// Offline-inclusive frame-pacing instrument (#63). The [PERF] and
// BATTLE STATUS lines only fire on the netplay path, so they're blind to
// the offline loop -- exactly the case bug bumbler hit (consistent ~95 FPS
// offline). This samples the outer trampoline loop directly:
//   work  = engine + render time for this frame (measured just before the
//           pacing sleep), and
//   frame = full wall-clock since the previous tick_start (== work + sleep).
// Read it as: if `work` approaches/exceeds 10 ms the box is render/CPU bound
// and SleepToTarget is innocent (it can only pad up to 10 ms, never stretch
// past work). If `work` is small but `frame` > 10 ms, the pacing is
// overshooting. Gated on FM2K_PERF_PROFILE=1 so it's free in normal play.
// Call once per outer iteration, BEFORE SleepToTarget.
void MaybeLogFrametime(uint64_t tick_start) {
    static const bool on = []{
        const char* v = std::getenv("FM2K_PERF_PROFILE");
        return v && v[0] == '1';
    }();
    if (!on) return;
    const uint64_t freq = SDL_GetPerformanceFrequency();
    const uint64_t work_end = SDL_GetPerformanceCounter();
    static uint64_t s_prev_start = 0;
    static uint64_t s_work_sum = 0, s_work_max = 0;
    static uint64_t s_frame_sum = 0, s_frame_max = 0;
    static uint32_t s_n = 0, s_frames = 0;
    static uint32_t s_over_budget = 0;            // frames whose work exceeded 10ms
    const uint64_t budget_ticks = freq / 100;     // 10ms = freq/100
    const uint64_t work = work_end - tick_start;
    s_work_sum += work;
    if (work > s_work_max) s_work_max = work;
    // A frame whose WORK (engine+render+spectator fan-out, measured before the
    // pacing sleep) exceeds the 10ms budget is a real host hiccup -- SleepToTarget
    // can only pad UP to 10ms, never claw back time already spent. Count them so
    // the spectator load test can assert the host never stalls the two players.
    if (work > budget_ticks) {
        ++s_over_budget;
        // Surface a catastrophic single-frame stall (>2x budget) immediately --
        // the 300-frame summary's average would otherwise bury it. Pure QPC
        // reads, no timing perturbation.
        if (work > budget_ticks * 2) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[HICCUP] host frame work=%.1fms (>2x 10ms budget)",
                (double)work * 1000.0 / (double)freq);
        }
    }
    if (s_prev_start) {
        const uint64_t frame = tick_start - s_prev_start;
        s_frame_sum += frame;
        if (frame > s_frame_max) s_frame_max = frame;
        ++s_frames;
    }
    s_prev_start = tick_start;
    if (++s_n >= 300) {
        auto ms = [freq](uint64_t t) { return (double)t * 1000.0 / (double)freq; };
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[FRAMETIME] n=%u work avg=%.2fms max=%.2fms | frame avg=%.2fms "
            "max=%.2fms (target 10.00ms = 100fps) over_budget=%u/%u",
            s_n, ms(s_work_sum) / s_n, ms(s_work_max),
            s_frames ? ms(s_frame_sum) / s_frames : 0.0, ms(s_frame_max),
            s_over_budget, s_n);
        s_work_sum = s_work_max = s_frame_sum = s_frame_max = 0;
        s_n = s_frames = s_over_budget = 0;
    }
}

// Per-section breakdown of the OFFLINE battle tick (#63 follow-up for the
// Robot Heroes object-count slowdown Yamada reported). Splits the frame into
// process-inputs / update / render so an A/B across stages (Grid vs a heavy
// stage) shows WHICH section scales with object count. The hooked hot
// functions (Hook_GameRand, Hook_DispatchScriptSoundCommand) execute INSIDE
// original_update_game / RenderFrameWithSnapshot, so their per-call overhead
// shows up in the `update` / `render` buckets here. Same FM2K_PERF_PROFILE=1
// gate as [FRAMETIME]; logs every 300 frames.
void MaybeLogOfflineSections(uint64_t pgi, uint64_t ug, uint64_t render,
                                    uint32_t render_rand) {
    static const bool on = []{
        const char* v = std::getenv("FM2K_PERF_PROFILE");
        return v && v[0] == '1';
    }();
    if (!on) return;
    const uint64_t freq = SDL_GetPerformanceFrequency();
    extern uint64_t g_render_game_only_ns;
    static uint64_t s_pgi = 0, s_ug = 0, s_render = 0;
    static uint64_t s_ug_max = 0, s_render_max = 0;
    static uint64_t s_rand = 0, s_rand_max = 0, s_render_game_acc = 0;
    static uint32_t s_n = 0;
    s_pgi += pgi; s_ug += ug; s_render += render; s_rand += render_rand;
    s_render_game_acc += g_render_game_only_ns;
    if (ug > s_ug_max) s_ug_max = ug;
    if (render > s_render_max) s_render_max = render;
    if (render_rand > s_rand_max) s_rand_max = render_rand;
    if (++s_n >= 300) {
        auto ms = [freq](uint64_t t){ return (double)t * 1000.0 / (double)freq; };
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[OFFLINE-SECT] n=%u pgi=%.2fms update=%.2fms(max %.2f) "
            "render=%.2fms(max %.2f) render_game_only=%.2fms render_rand=%llu/f(max %llu)",
            s_n, ms(s_pgi) / s_n, ms(s_ug) / s_n, ms(s_ug_max),
            ms(s_render) / s_n, ms(s_render_max),
            ms(s_render_game_acc) / s_n,
            (unsigned long long)(s_rand / s_n), (unsigned long long)s_rand_max);
        s_pgi = s_ug = s_render = 0;
        s_ug_max = s_render_max = 0;
        s_rand = s_rand_max = 0;
        s_render_game_acc = 0;
        s_n = 0;
    }
}

