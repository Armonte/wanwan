// frame_timing.cpp -- PER-FRAME TIMING RING. OBSERVE ONLY.
// Nothing here writes game memory and nothing here changes shipping behaviour.
// See frame_timing.h for the question it exists to answer.
#include "frame_timing.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The netplay frame at the moment the row closes. Not an engine address: this
// is the hook's own gekko-frame mirror, so it is engine-agnostic and the file
// builds unchanged for FM95.
extern uint32_t g_netplay_frame;

// The pacer's non-destructive peek at what it did to the LAST frame (defined
// in trampoline_pacing.cpp, next to the counters it already keeps for [BEAT]).
// Declared here rather than in trampoline_internal.h so this TU carries no
// dependency on the trampoline's shared header.
uint32_t Pacing_LastAddedUs();
float    Pacing_LastFramesAhead();

namespace {

// 240k rows x 44 B = ~10.5 MB, allocated lazily at ARM time so a dark run
// costs one cached bool test per hook point and no memory at all. 240k frames
// is 40 minutes at 100 Hz -- longer than any match, and long enough that a
// player who says "it went bad about ten minutes in" is still in the file.
// Overflow is COUNTED and reported in the header, never silently dropped: a
// truncated trace that looked complete would make a clean percentile table
// mean nothing.
constexpr size_t kCapacity = 240000;

// The pacing target the whole instrument is judged against. SleepToTarget is
// called with 10 ms everywhere except FM2K_TURBO (0, offline capture sweeps
// only), so this is the budget in every regime a player is ever in.
constexpr uint32_t kBudgetUs = 10000;

// When a WALL-CLOCK frame counts as late. Deliberately NOT kBudgetUs: the
// pacer's busy-wait tail exits at the first QPC tick past its deadline, so a
// healthy 100 fps run is a microsecond or two over 10 ms on nearly every
// frame, and testing against the budget reports 100% late on a run with
// nothing wrong with it. 10.5 ms is 95 fps -- the point where the loss is
// large enough to be a frame rate rather than a rounding artifact, and inside
// the 85-96 fps band the field reports arrive in.
constexpr uint32_t kLateUs = 10500;

struct Row {
    uint32_t seq;            // monotonic frame counter, the row's identity
    // THIS frame's own wall-clock duration: its start to the NEXT frame's
    // start. Back-patched by the next BeginFrame, because it cannot be known
    // until then. Written this way rather than "time since the previous start"
    // (which needs no patch) because that version attributes a hitch to the
    // row AFTER the one that caused it: a 300 ms load would show as a 300 ms
    // `frame` on a row whose own work was 400 us, and every reader has to hold
    // the off-by-one in their head. Here work + sleep belong to the same row
    // as the frame they add up to. The LAST row has no successor, so its 0 is
    // the absence of a measurement -- the same convention as the first row's
    // under the old scheme, and the percentile pass skips zeros for it.
    uint32_t frame_ticks;
    uint32_t work_ticks;     // this frame start -> just before the pacing sleep
    uint32_t sim_ticks;      // summed across every sim step this frame ran
    uint32_t render_ticks;   // RenderFrameWithSnapshot, whole
    uint32_t sleep_ticks;    // measured across SleepToTarget, not inferred
    uint32_t brake_us;       // what the drift brake ADDED to this frame
    uint32_t bf;             // g_netplay_frame at close
    int32_t  fa_milli;       // frames_ahead * 1000, the value the brake acted on
    uint16_t match;
    uint16_t rb_depth;       // worst rewind this frame, in frames
    uint8_t  phase;          // FtPhase
    uint8_t  sim_steps;      // heavy-stage catch-up runs up to 10
    uint8_t  advanced;       // 0 = STALLED: gekko emitted no Advance
    uint8_t  rb_count;       // real (non-runahead) rollbacks this frame
};

Row*     g_rows    = nullptr;
size_t   g_count   = 0;
size_t   g_dropped = 0;
uint32_t g_seq     = 0;
uint16_t g_match   = 0;

// The row under construction. Not atomic and not guarded: every writer is the
// trampoline thread. The one cross-thread caller is the F7 request below,
// which touches an atomic flag and nothing else.
Row      g_cur          = {};
uint64_t g_frame_start  = 0;   // this frame's QPC anchor, shared with the pacer
uint64_t g_prev_start   = 0;
uint64_t g_work_end     = 0;
bool     g_frame_open   = false;
// Whether the previous frame actually produced a row at g_rows[g_count-1].
// Without it, a frame dropped at the ring cap would have its duration
// back-patched onto whatever unrelated row happens to sit at the end.
bool     g_prev_stored  = false;
bool     g_alloc_failed = false;   // latch: never retry the ring allocation

std::atomic<bool> g_dump_requested{false};

}  // namespace

bool FrameTiming_Enabled() {
    // Strict parse, default OFF. Unrecognised values fail to OFF with an Error
    // line, and exactly one contract line is emitted at the first call: an
    // armed run must say so in its own log, because a percentile table is
    // indistinguishable from silence when the instrument never armed.
    //
    // A THREAD-SAFE magic static, not the `static int s_cached = -1;` idiom the
    // sibling instruments use: this one is also read from the WndProc thread
    // (the F7 gate), and a plain lazy int there would race the trampoline's
    // first call and could emit the contract line twice.
    static const int s_cached = []{
        const char* v = std::getenv("FM2K_FRAME_TIMING");
        int on;
        if (!v || !v[0] || (v[0] == '0' && v[1] == '\0')) {
            on = 0;
        } else if (v[0] == '1' && v[1] == '\0') {
            on = 1;
        } else {
            on = 0;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[FT] FM2K_FRAME_TIMING=\"%s\" is not 0 or 1 -- treating as "
                "OFF. The per-frame ring is NOT recording.", v);
        }
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[FT] per-frame timing ring %s (FM2K_FRAME_TIMING)",
            on == 1 ? "ARMED" : "off");
        return on;
    }();
    return s_cached == 1;
}

void FrameTiming_BeginFrame(uint64_t tick_start_qpc) {
    if (!FrameTiming_Enabled()) return;
    // ONE-SHOT on failure. Without the latch a failed allocation is retried
    // every frame and its Error line is emitted every frame -- and hook
    // logging is a synchronous fputs on this thread, so the instrument would
    // become a far worse stutter than anything it was built to find, in
    // exactly the low-memory conditions where a player is already suffering.
    if (!g_rows) {
        if (g_alloc_failed) return;
        g_rows = (Row*)std::calloc(kCapacity, sizeof(Row));
        if (!g_rows) {
            g_alloc_failed = true;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[FT] out of memory for %zu rows (%zu KB) -- ring permanently "
                "disabled for this process", kCapacity,
                (kCapacity * sizeof(Row)) / 1024);
            return;
        }
    }
    // Close out the PREVIOUS row's wall-clock duration now that we know when
    // the next frame began. See Row::frame_ticks for why the duration belongs
    // to the row that spent it rather than to the one after it.
    if (g_prev_stored && g_count && g_prev_start &&
        tick_start_qpc > g_prev_start) {
        g_rows[g_count - 1].frame_ticks = (uint32_t)(tick_start_qpc - g_prev_start);
    }
    g_prev_start  = tick_start_qpc;
    g_prev_stored = false;

    g_cur = Row{};
    // Default ADVANCED. Only the netplay battle path can stall, and it says so
    // explicitly; defaulting the other way would report every menu frame as a
    // dropped frame and bury the real ones.
    g_cur.advanced = 1;
    g_cur.phase    = (uint8_t)FtPhase::NATIVE;
    g_cur.match    = g_match;
    g_frame_start  = tick_start_qpc;
    g_work_end     = tick_start_qpc;
    g_frame_open   = true;
}

void FrameTiming_NotePhase(FtPhase phase) {
    if (!g_frame_open) return;
    g_cur.phase = (uint8_t)phase;
}

void FrameTiming_NoteSim(uint64_t ticks, int steps) {
    if (!g_frame_open) return;
    g_cur.sim_ticks += (uint32_t)ticks;
    const int total = (int)g_cur.sim_steps + (steps > 0 ? steps : 0);
    g_cur.sim_steps = (uint8_t)(total > 255 ? 255 : total);
}

void FrameTiming_NoteRender(uint64_t ticks) {
    if (!g_frame_open) return;
    g_cur.render_ticks += (uint32_t)ticks;
}

void FrameTiming_NoteStall() {
    if (!g_frame_open) return;
    g_cur.advanced = 0;
}

void FrameTiming_NoteRollback(uint32_t rewind_depth) {
    if (!g_frame_open) return;
    if (g_cur.rb_count < 255) ++g_cur.rb_count;
    if (rewind_depth > g_cur.rb_depth && rewind_depth <= 0xFFFFu) {
        g_cur.rb_depth = (uint16_t)rewind_depth;
    }
}

void FrameTiming_EndWork() {
    if (!g_frame_open) return;
    g_work_end = SDL_GetPerformanceCounter();
    g_cur.work_ticks = (uint32_t)(g_work_end - g_frame_start);
}

void FrameTiming_EndFrame() {
    if (!g_frame_open) return;
    g_frame_open = false;
    const uint64_t now = SDL_GetPerformanceCounter();
    g_cur.sleep_ticks = (uint32_t)(now > g_work_end ? now - g_work_end : 0);
    // frame_ticks is left 0 here and back-patched by the next BeginFrame.
    g_cur.seq = g_seq++;
    g_cur.bf  = g_netplay_frame;
    // The brake this frame actually applied, and the advantage reading it
    // acted on. Read through the pacer's own non-destructive peek so the
    // [BEAT] heartbeat's window counters are left alone -- draining them here
    // would silently blank the heartbeat line that has been the only pacing
    // telemetry we had.
    g_cur.brake_us = Pacing_LastAddedUs();
    g_cur.fa_milli = (int32_t)(Pacing_LastFramesAhead() * 1000.0f);
    if (!g_rows) return;
    if (g_count >= kCapacity) { ++g_dropped; return; }
    g_rows[g_count++] = g_cur;
    g_prev_stored = true;
}

void FrameTiming_NoteMatchBoundary() {
    if (!FrameTiming_Enabled()) return;
    ++g_match;
}

void FrameTiming_RequestDump() {
    // Callable from the WndProc thread. Sets a flag and nothing else; the ring
    // itself is touched only on the trampoline thread, same handoff shape as
    // the F8 runahead toggle.
    g_dump_requested.store(true, std::memory_order_release);
}

void FrameTiming_PollDump(int player_index) {
    if (!g_dump_requested.exchange(false, std::memory_order_acq_rel)) return;
    if (!FrameTiming_Enabled()) return;
    FrameTiming_Dump(player_index, "on-demand F7");
}

// ============================================================================
// PERCENTILE SUMMARY
// ============================================================================

namespace {

struct Pct { uint32_t p50, p99, p100, over; size_t n; };

// nth_element rather than a full sort: three order statistics out of a quarter
// of a million values, on a path that still should not take seconds.
// `over_us` is the threshold the over-count uses; 0 means do not count.
Pct Percentiles(uint32_t* v, size_t n, uint64_t freq, uint32_t over_us) {
    Pct p = {0, 0, 0, 0, n};
    if (!n) return p;
    const uint64_t f = freq ? freq : 1;
    auto to_us = [f](uint32_t t) {
        return (uint32_t)((uint64_t)t * 1000000ull / f);
    };
    const size_t i50 = n / 2;
    size_t i99 = (n * 99) / 100;
    if (i99 >= n) i99 = n - 1;
    std::nth_element(v, v + i50, v + n);
    p.p50 = to_us(v[i50]);
    std::nth_element(v, v + i99, v + n);
    p.p99 = to_us(v[i99]);
    p.p100 = to_us(*std::max_element(v, v + n));
    // Counted from the TICK values before conversion, so the threshold test is
    // exact rather than rounded through microseconds.
    if (over_us) {
        const uint64_t th = (uint64_t)over_us * f / 1000000ull;
        for (size_t i = 0; i < n; ++i) if (v[i] > th) ++p.over;
    }
    return p;
}

void ReportPct(const char* label, uint32_t* scratch, size_t n, uint64_t freq,
               uint32_t over_us, bool with_fps) {
    const Pct p = Percentiles(scratch, n, freq, over_us);
    char tail[128] = "";
    if (over_us) {
        std::snprintf(tail, sizeof(tail), "   over %u.%ums: %u/%zu (%.1f%%)",
                      over_us / 1000, (over_us % 1000) / 100, p.over, n,
                      n ? 100.0 * p.over / (double)n : 0.0);
    }
    if (with_fps) {
        // The achieved rate, spelled out. Field reports arrive as "85-96 fps",
        // and a microsecond table that the reader has to invert in their head
        // to compare against that is a table nobody compares.
        auto fps = [](uint32_t us) { return us ? 1000000.0 / us : 0.0; };
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[FT] %-6s us  p50=%6u  p99=%6u  p100=%7u   "
            "(fps %.1f / %.1f / %.1f)%s",
            label, p.p50, p.p99, p.p100,
            fps(p.p50), fps(p.p99), fps(p.p100), tail);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[FT] %-6s us  p50=%6u  p99=%6u  p100=%7u%s",
            label, p.p50, p.p99, p.p100, tail);
    }
}

}  // namespace

void FrameTiming_Dump(int player_index, const char* reason) {
    const uint64_t dump_t0 = SDL_GetPerformanceCounter();
    const uint64_t freq = SDL_GetPerformanceFrequency();

    // Unconditional one-liner: a dark run reporting 0 rows is the evidence
    // that dark means dark.
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[FT] summary: armed=%s rows=%zu dropped=%zu frames=%u matches=%u (%s)",
        FrameTiming_Enabled() ? "yes" : "no", g_count, g_dropped,
        (unsigned)g_seq, (unsigned)g_match + 1u, reason ? reason : "");
    if (g_count == 0) return;

    // ---- percentiles ------------------------------------------------------
    // One scratch buffer reused per metric: nth_element permutes it, so it has
    // to be refilled anyway, and one allocation beats five.
    uint32_t* scratch = (uint32_t*)std::malloc(g_count * sizeof(uint32_t));
    if (scratch) {
        size_t n = 0;
        auto fill = [&](uint32_t Row::*field, bool skip_zero) {
            n = 0;
            for (size_t i = 0; i < g_count; ++i) {
                const uint32_t v = g_rows[i].*field;
                if (skip_zero && v == 0) continue;
                scratch[n++] = v;
            }
        };
        // frame: the LAST row has no successor to back-patch its duration, so
        // its 0 is the ABSENCE of a measurement, not a zero-length frame.
        // Dropped rather than left to drag the p50 toward zero.
        // WHY `frame` IS JUDGED AT kLateUs AND NOT AT kBudgetUs. SleepToTarget
        // busy-waits to the first QPC tick PAST its deadline, so essentially
        // every paced frame is a microsecond or two over 10 ms by
        // construction. Counting those as misses reports 100% over budget on a
        // perfectly healthy run and says nothing. A frame is LATE when it cost
        // measurable frame rate, which is why the threshold is 10.5 ms
        // (95 fps) -- and the printed label carries the number so the reader is
        // never guessing which test produced the count. `work` keeps the strict
        // 10 ms test: work over the budget is over the budget, full stop.
        fill(&Row::frame_ticks, true);   ReportPct("frame",  scratch, n, freq, kLateUs, true);
        fill(&Row::work_ticks, false);   ReportPct("work",   scratch, n, freq, kBudgetUs, false);
        fill(&Row::sim_ticks, false);    ReportPct("sim",    scratch, n, freq, 0, false);
        fill(&Row::render_ticks, false); ReportPct("render", scratch, n, freq, 0, false);
        fill(&Row::sleep_ticks, false);  ReportPct("sleep",  scratch, n, freq, 0, false);
        std::free(scratch);
    }

    // ---- stalls, runs, rollbacks, brake -----------------------------------
    // A stall is only meaningful where gekko could have emitted an Advance, so
    // it is counted over BATTLE_NET rows alone. RUNS are counted because five
    // consecutive stalls is a 50 ms freeze, and the same stall rate spread
    // evenly is not the same experience at all.
    size_t net_rows = 0, stalls = 0, runs = 0, longest = 0, cur_run = 0;
    size_t zero_sleep = 0, rb_frames = 0, rb_total = 0, braked = 0;
    size_t work_over = 0;
    uint32_t max_depth = 0, brake_worst = 0;
    uint64_t brake_sum = 0;
    int32_t  fa_max = 0;
    const uint64_t budget_ticks = (uint64_t)kBudgetUs * (freq ? freq : 1) / 1000000ull;
    for (size_t i = 0; i < g_count; ++i) {
        const Row& r = g_rows[i];
        if (r.sleep_ticks == 0) ++zero_sleep;
        if (r.work_ticks > budget_ticks) ++work_over;
        if (r.rb_count) { ++rb_frames; rb_total += r.rb_count; }
        if (r.rb_depth > max_depth) max_depth = r.rb_depth;
        if (r.brake_us) {
            ++braked;
            brake_sum += r.brake_us;
            if (r.brake_us > brake_worst) brake_worst = r.brake_us;
        }
        if (r.fa_milli > fa_max) fa_max = r.fa_milli;
        if (r.phase != (uint8_t)FtPhase::BATTLE_NET) { cur_run = 0; continue; }
        ++net_rows;
        if (r.advanced) { cur_run = 0; continue; }
        ++stalls;
        if (cur_run == 0) ++runs;
        ++cur_run;
        if (cur_run > longest) longest = cur_run;
    }
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[FT] STALLS: %zu dropped frames over %zu netplay-battle rows (%.2f%%) "
        "in %zu runs, longest run %zu frames (= %zu ms of frozen game)",
        stalls, net_rows, net_rows ? 100.0 * stalls / (double)net_rows : 0.0,
        runs, longest, longest * (kBudgetUs / 1000));
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[FT] rollbacks: %zu over %zu frames, worst rewind %u | brake: %zu "
        "frames, %.1f ms total, worst %u us | FAmax=%+.1f | sleep==0 on %zu "
        "frames (the pacer had nothing left to pad)",
        rb_total, rb_frames, max_depth, braked, brake_sum / 1000.0,
        brake_worst, fa_max / 1000.0f, zero_sleep);

    // ---- the verdict the whole instrument exists for -----------------------
    // Deliberately the SAME rule tools/frame_timing_report.py applies, so a
    // live F7 read and the offline analysis can never disagree about what the
    // run said. Stated as a claim about which of the two causes THESE ROWS
    // support, never as a diagnosis of a cause they cannot see.
    {
        const double work_over_pct = 100.0 * work_over / (double)g_count;
        const double stall_pct = net_rows ? 100.0 * stalls / (double)net_rows : 0.0;
        const double brake_pct = 100.0 * braked / (double)g_count;
        const char* v;
        if (work_over_pct >= 5.0)
            v = "THE BOX. Sim+render misses the budget on its own, before any "
                "network effect -- the pacer cannot pad what is already spent.";
        else if (stall_pct >= 0.5 || brake_pct >= 20.0)
            v = "THE CONNECTION. The frame's own work fits; the loss is "
                "stalled frames and/or the drift brake.";
        else
            v = "NEITHER. The loop met its budget in this window; if it felt "
                "sluggish, the cause is not in these rows.";
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[FT] VERDICT: %s (work over budget %.1f%%, stalls %.2f%%, braked "
            "%.1f%%)", v, work_over_pct, stall_pct, brake_pct);
    }

    // ---- CSV --------------------------------------------------------------
    char filename[256];
    std::snprintf(filename, sizeof(filename), "FM2K_P%d_frametiming.csv",
                  player_index + 1);
    FILE* f = std::fopen(filename, "w");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[FT] failed to open %s", filename);
        return;
    }
    static char iobuf[1 << 20];
    std::setvbuf(f, iobuf, _IOFBF, sizeof(iobuf));

    std::fprintf(f, "# frametiming player=%d reason=%s rows=%zu dropped=%zu "
                    "frames=%u matches=%u qpc_freq=%llu budget_us=%u\n",
                 player_index + 1, reason ? reason : "", g_count, g_dropped,
                 (unsigned)g_seq, (unsigned)g_match + 1u,
                 (unsigned long long)freq, kBudgetUs);
    std::fprintf(f, "# times are MICROSECONDS. frame = THIS frame's own wall "
                    "clock (its start to the next frame's start); work = frame "
                    "start -> just before the pacing sleep; sleep = measured "
                    "across SleepToTarget. work+sleep == frame, on the SAME "
                    "row. The last row's frame is 0: it has no successor, so "
                    "that is an absent measurement, not a zero-length frame.\n");
    std::fprintf(f, "# phase: bat=netplay battle (the ONLY phase where adv=0 "
                    "means a dropped frame), off=offline battle, str=stress, "
                    "css, nat=native/menu, spec=spectator playback.\n");
    std::fprintf(f, "seq,match,phase,bf,frame_us,work_us,sim_us,render_us,"
                    "sleep_us,sim_steps,adv,rb_count,rb_depth,fa,brake_us\n");
    static const char* kPhase[] = {"bat", "off", "str", "css", "nat", "spec"};
    const uint64_t f_hz = freq ? freq : 1;
    for (size_t i = 0; i < g_count; ++i) {
        const Row& r = g_rows[i];
        auto us = [f_hz](uint32_t t) {
            return (unsigned)((uint64_t)t * 1000000ull / f_hz);
        };
        std::fprintf(f, "%u,%u,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.3f,%u\n",
            r.seq, (unsigned)r.match,
            kPhase[r.phase < 6 ? r.phase : 4], r.bf,
            us(r.frame_ticks), us(r.work_ticks), us(r.sim_ticks),
            us(r.render_ticks), us(r.sleep_ticks),
            (unsigned)r.sim_steps, (unsigned)r.advanced,
            (unsigned)r.rb_count, (unsigned)r.rb_depth,
            r.fa_milli / 1000.0, r.brake_us);
    }
    std::fclose(f);

    // The dump's OWN cost, reported rather than assumed. At match end and at
    // teardown this is free (the process is going away anyway); on the F7 path
    // it is a real hitch charged to a live frame, and an instrument that hides
    // its own perturbation is worse than no instrument.
    const double cost_ms =
        (double)(SDL_GetPerformanceCounter() - dump_t0) * 1000.0 / (double)freq;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[FT] wrote %s (%zu rows) -- this dump itself cost %.1f ms; if it ran "
        "mid-match, the frame around seq=%u is hitched BY THIS WRITE and is "
        "not evidence of anything else",
        filename, g_count, cost_ms, (unsigned)g_seq);
}
