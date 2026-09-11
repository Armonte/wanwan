// Pure delay-sizing math for the input-delay negotiation (#24).
//
// Kept dependency-free (no winsock / SDL / project headers) so the
// exact code that runs in the hook can also be exercised by the
// host-compiled unit test at tests/delay_math_test.cpp.
#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>

namespace fm2k::delay {

// Representative RTT (ms) from a window of raw RTT samples.
//   mode 0 (avg)  -> arithmetic mean of the window.
//   mode 1 (peak) -> p90: sorted, index floor(0.9 * (n-1)). Sizes above
//                    the median but discards the worst ~10%, so a lone
//                    cold-start / scheduler-hiccup spike cannot dominate
//                    the pick (that outlier sensitivity is exactly why
//                    an all-time-max worst-RTT was a bad delay basis).
// Sample order is irrelevant -- both mean and percentile are order-
// independent -- so a circular ring can be passed as-is. Returns 0 when
// the window is empty.
inline uint32_t RttFromWindow(const uint32_t* samples, int n, int mode) {
    if (n <= 0 || samples == nullptr) return 0;
    if (mode != 1) {
        uint64_t sum = 0;
        for (int i = 0; i < n; ++i) sum += samples[i];
        return (uint32_t)(sum / (uint64_t)n);
    }
    std::vector<uint32_t> sorted(samples, samples + n);
    std::sort(sorted.begin(), sorted.end());
    return sorted[(size_t)((n - 1) * 9 / 10)];
}

// Input-delay candidate (frames) from a representative RTT (ms).
// FM2K runs at 100 Hz so 10 ms = 1 frame; delay covers the ONE-WAY
// latency, hence rtt/2. ceil is done via (+9)/10. Clamped to [2, 15].
// Returns -1 when rtt_ms is 0 (no measurement available yet).
// Jitter (ms) over the same RTT window: p90 - p50. Spread, not absolute
// level -- a link that is uniformly slow needs delay for its LATENCY, while a
// link that wobbles needs extra headroom on top so a late packet does not
// force a prediction. Order-independent like RttFromWindow, so the circular
// ring can be passed as-is. Returns 0 for an empty/too-small window.
inline uint32_t JitterFromWindow(const uint32_t* samples, int n) {
    if (n <= 1 || samples == nullptr) return 0;
    std::vector<uint32_t> sorted(samples, samples + n);
    std::sort(sorted.begin(), sorted.end());
    const uint32_t p50 = sorted[(size_t)((n - 1) / 2)];
    const uint32_t p90 = sorted[(size_t)((n - 1) * 9 / 10)];
    return p90 > p50 ? (p90 - p50) : 0u;
}

// Upper bound on the AUTO-sized delay. 24 frames = 240 ms at FM2K's 100 Hz.
//
// WAS 15 (150 ms), WHICH CAPPED AUTO PLAY AT ~300 ms RTT.
//
// Measured 2026-09-10 on a 300 ms / 20 ms-jitter link, 2400 frames per point,
// n=3. TRUST rb_max, NOT THE BRAKE PERCENTAGES -- rb_max is deterministic
// (identical every run), while braking here is a high-variance MATCH-START
// transient that reads 0.0% or 22.6% at the SAME setting:
//
//     delay 15 -> rb_max 16 (pegged)   startup braking 7.5% (0.0-22.6)
//     delay 21 -> rb_max  0            startup braking 0.8% (0.0- 2.5)
//     delay 30 -> rb_max  0            startup braking 8.6% (1.3-16.2)
//
// The load-bearing result is rb_max: at 15 the netcode predicts the FULL
// window on every frame; at 21 it never predicts at all. That is the same
// quantity a real framedrop report showed pegged at 17 on every beat for a
// minute, which is why this ceiling mattered in the field.
//
// Two cautions for whoever changes this next:
//   * Braking is concentrated in the FIRST beat. Averaging it over a run
//     makes the number depend on RUN LENGTH -- that artifact made an auto
//     run at delay 24 look 10x worse than a manual run at the same delay.
//     Split startup from steady state before comparing anything.
//   * n=1 is not enough. A single sample said "delay 15 brakes 7.4%"; the
//     next run at the same setting braked 0.0%.
//
// NOT UNCAPPED: 30 stayed worse than 21 at n=3 on both mean and spread, and
// was the only setting with non-zero STEADY-STATE braking (1.4%). 24 leaves
// headroom above the measured optimum without reaching that.
constexpr int MAX_AUTO_DELAY = 24;

// Delay (frames) to cover a link, from its RTT and optionally its jitter.
//
// one_way = rtt/2, plus 2x jitter of headroom. The 2x is empirical: at the
// measured 330-340 ms / 14-27 ms link the formula lands on 21, which is
// exactly where braking collapsed. With jitter_ms = 0 this is byte-identical
// to the original one_way-only sizing, so every pre-existing case is
// unchanged -- callers that have no jitter measurement keep the old answer.
inline int DelayCandidateFromRtt(uint32_t rtt_ms, uint32_t jitter_ms = 0) {
    if (rtt_ms == 0) return -1;
    int d = (int)((((rtt_ms / 2) + 2 * jitter_ms) + 9) / 10);
    if (d < 2)  d = 2;
    if (d > MAX_AUTO_DELAY) d = MAX_AUTO_DELAY;
    return d;
}

}  // namespace fm2k::delay
