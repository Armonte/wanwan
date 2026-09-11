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
// WAS 15 (150 ms), WHICH CAPPED AUTO PLAY AT ~300 ms RTT. Measured 2026-09-10
// on a 300 ms / 20 ms-jitter link, 2400 frames per point:
//     delay 15 -> rb_max 16, 7.4% of frames BRAKED    (the shipped ceiling)
//     delay 17 -> rb_max 11, 6.5%
//     delay 19 -> rb_max  7, 6.0%
//     delay 21 -> rb_max  0, 0.5%   <-- braking collapses exactly where
//     delay 24 -> rb_max  0, 0.9%       prediction reaches ZERO
//     delay 30 -> rb_max  0, 5.0%   <-- and gets WORSE again past that
// Braking tracks residual prediction depth, so partial coverage buys almost
// nothing: 15->19 costs 40 ms of input lag for 1.4 points. The useful setting
// is the first one where prediction is not needed at all.
//
// NOT UNCAPPED, deliberately: 30 was worse than 21 on every measure that
// matters, so "more is better" is false and a ceiling is still required.
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
