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
// PACING TELEMETRY. The brake below decides every frame, a hundred times a
// second, and the heartbeat that reports on the match emits every ten
// seconds. Sampling a per-frame decision at a tenth of a hertz is why a
// player's "it stutters" has never been answerable from a log. These
// counters are the whole fix for that: they accumulate per frame and the
// heartbeat drains them, so one line can say "in this window I braked 180
// frames, 41 ms in total, worst single frame 0.5 ms".
namespace {
std::atomic<uint32_t> g_pace_braked{0};      // frames where the brake applied
std::atomic<uint32_t> g_pace_total{0};       // frames paced at all
std::atomic<uint32_t> g_pace_us_sum{0};      // microseconds added, summed
std::atomic<uint32_t> g_pace_us_max{0};      // worst single frame
std::atomic<int32_t>  g_pace_fa_max_milli{0};// worst frames_ahead, in 1/1000
// LAST frame's decision, kept alongside the window counters above rather than
// derived from them. The counters are DRAINED by the [BEAT] heartbeat every
// ten seconds; a per-frame reader that used them would steal the heartbeat's
// window and blank the only pacing telemetry we had. These two are plain
// last-value stores of the same numbers the brake already computed -- one
// extra store per frame, no second computation, nothing duplicated.
std::atomic<uint32_t> g_pace_last_us{0};
std::atomic<int32_t>  g_pace_last_fa_milli{0};
}  // namespace

// Non-destructive peek for the per-frame timing ring (frame_timing.cpp).
uint32_t Pacing_LastAddedUs()     { return g_pace_last_us.load(std::memory_order_relaxed); }
float    Pacing_LastFramesAhead() { return (float)g_pace_last_fa_milli.load(std::memory_order_relaxed) / 1000.0f; }

void Pacing_TakeStats(PacingStats* out) {
    if (!out) return;
    out->braked_frames = g_pace_braked.exchange(0, std::memory_order_relaxed);
    out->total_frames  = g_pace_total.exchange(0, std::memory_order_relaxed);
    out->added_us_sum  = g_pace_us_sum.exchange(0, std::memory_order_relaxed);
    out->added_us_max  = g_pace_us_max.exchange(0, std::memory_order_relaxed);
    out->frames_ahead_max =
        (float)g_pace_fa_max_milli.exchange(0, std::memory_order_relaxed) / 1000.0f;
}

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

    // THE DRIFT BRAKE, as a FRACTION OF THE FRAME PERIOD rather than a
    // count of milliseconds. Gekko emits no pacing of its own (UpdateSession
    // always advances; there is no Wait/Advise event), so slowing the peer
    // that is AHEAD is the only lever that lets the other one catch up.
    //
    // WHY A PERCENTAGE. Upstream's examples all do
    // `delay = frames_ahead > .5f ? period * 1.016 : period`, and that form
    // is frame-rate invariant where a millisecond count is not. The
    // convergence rate in FRAMES is what the percentage fixes: at 1.6% it
    // takes ~62 frames to shed one frame of advantage whether the period is
    // 16.67 ms at 60 Hz or 10 ms at 100 Hz. Expressed in milliseconds
    // instead, the same nudge would be over half again as strong at 100 Hz
    // as upstream intended, which is the mistake this replaces: the old
    // brake was `frames_ahead * 0.5 ms` capped at 3 ms, i.e. up to 30% of a
    // frame, roughly nineteen times upstream's correction.
    //
    // WHY NOT SIMPLY 1.6% FLAT. A documented failure says flat is not
    // enough here: the host held rigid 10 ms while the client trailed 7-8
    // frames permanently, FA pinned near +-8.5. So the fraction RAMPS with
    // how far ahead we are, from upstream's 1.6% up to kBrakeMaxFrac. At
    // 100 Hz the cap is half a millisecond -- 95 FPS rather than 100, which
    // is imperceptible per frame -- and it bleeds an 8-frame gap off in
    // about 1.6 seconds. The old 3 ms cap did it in a quarter of a second
    // and cost 30% of the frame rate to do it, which is the trade nobody
    // measured and players felt.
    // AND A DEADBAND, measured from two field matches at 138-261 ms ping.
    // Braking ONE peer slows BOTH: the other can only advance as fast as
    // inputs arrive, so it inherits the braked rate. In those logs, every
    // window where neither peer was braked ran at exactly 100.0 fps, and
    // every window where either was braked pulled BOTH down to roughly the
    // braked rate -- including windows with literally zero rollbacks, which
    // rules out rollback cost and the prediction wall.
    //
    // That makes a small correction expensive and, below about two frames,
    // useless as well. Frame advantage is (our average lead minus the
    // peer's reported lead) / 2, and the peer's half arrives a half round
    // trip stale, so at these pings BOTH peers routinely read positive at
    // the same time -- six of nine windows in one match. Braking both
    // cannot change the difference between them. It only costs the session
    // frame rate. So do nothing until the reading is big enough to be a
    // real rift rather than the noise floor of the measurement.
    constexpr float kBrakeDeadband = 2.0f;   // frames ahead before we act
    constexpr float kBrakeMinFrac = 0.016f;  // upstream's constant
    constexpr float kBrakeMaxFrac = 0.05f;   // 5% == 95 FPS at a 10 ms period
    constexpr float kBrakeFullAt  = 8.0f;    // frames ahead at which we cap
    uint32_t added_us = 0;
    if (frames_ahead > kBrakeDeadband && target_ms > 0) {
        float frac = kBrakeMinFrac +
                     (kBrakeMaxFrac - kBrakeMinFrac) *
                     ((frames_ahead - kBrakeDeadband) / (kBrakeFullAt - kBrakeDeadband));
        if (frac > kBrakeMaxFrac) frac = kBrakeMaxFrac;
        const uint64_t extra_ticks =
            (uint64_t)((double)target_ticks_count * (double)frac);
        target_ticks_count += extra_ticks;
        added_us = (uint32_t)((extra_ticks * 1000000ull) / freq);
    }

    // Telemetry, taken whether or not the brake fired, so the heartbeat can
    // report a rate rather than a bare count.
    g_pace_last_us.store(added_us, std::memory_order_relaxed);
    g_pace_last_fa_milli.store((int32_t)(frames_ahead * 1000.0f),
                               std::memory_order_relaxed);
    g_pace_total.fetch_add(1, std::memory_order_relaxed);
    if (added_us) {
        g_pace_braked.fetch_add(1, std::memory_order_relaxed);
        g_pace_us_sum.fetch_add(added_us, std::memory_order_relaxed);
        uint32_t prev = g_pace_us_max.load(std::memory_order_relaxed);
        while (added_us > prev &&
               !g_pace_us_max.compare_exchange_weak(prev, added_us,
                                                    std::memory_order_relaxed)) {}
    }
    if (frames_ahead > 0.0f) {
        const int32_t fa_milli = (int32_t)(frames_ahead * 1000.0f);
        int32_t prev_fa = g_pace_fa_max_milli.load(std::memory_order_relaxed);
        while (fa_milli > prev_fa &&
               !g_pace_fa_max_milli.compare_exchange_weak(prev_fa, fa_milli,
                                                          std::memory_order_relaxed)) {}
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

