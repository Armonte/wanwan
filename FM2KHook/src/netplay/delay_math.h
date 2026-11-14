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

// Percentile (0..100) of a window of RTT samples: sorted, index
// floor(pct * (n-1) / 100). Order-independent, so the circular ring can be
// passed as-is. Returns 0 for an empty window.
inline uint32_t PercentileFromWindow(const uint32_t* samples, int n, int pct) {
    if (n <= 0 || samples == nullptr) return 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    std::vector<uint32_t> sorted(samples, samples + n);
    std::sort(sorted.begin(), sorted.end());
    return sorted[(size_t)((n - 1) * pct / 100)];
}

// ---------------------------------------------------------------------------
// Delay/rollback sizing (2026-11-13; replaces the 2026-11-08 quarter-share split)
//
// L = ceil(p90_rtt/2 / 10) is the one-way trip in FM2K frames (10 ms).
// The 11-08 split gave input delay only max(2, L/4) and planned rollback for
// the rest, so a 160 ms link ran at 2 delay with 6 frames of rollback. The
// field rejected it: at p90 120-240 ms most 0.2.89/0.2.90 players switched to
// MANUAL and chose 7-8 (logs since 11-09), i.e. about the whole trip, and the
// manual choices level off around 7-10 beyond that. So:
//
//   auto:               D = L up to 8 frames, then 8 + (L-8)/3; rollback covers the
//                       rest, at most rcap (AUTO_RCAP) frames
//   auto (less rollback): D = L (rcap 0: the 0.2.84 sizing, no planned rollback)
//
//   R = clamp(L - D, 0, min(rcap, W-3)); D = max(1, L - R) + max(0, 3 - (W - R))
//
// p90 already contains the link's jitter, so there is no separate jitter term.
// ---------------------------------------------------------------------------
constexpr int AUTO_FULL_TRIP     = 8;   // auto delays the whole trip up to this many frames
constexpr int AUTO_RCAP          = 8;   // "auto": at most 80 ms of planned rollback
constexpr int AUTO_SMOOTH_RCAP   = 0;   // "auto (less rollback)": none planned
constexpr int MAX_SPLIT_DELAY    = 24;  // only reached around 600 ms RTT
constexpr int MIN_DELAY_SAMPLES  = 12;  // ~1.5 s of post-warm-up pings
// The control-ping window is short (64 samples = 8 s at 8 Hz), so p90 rather
// than p95: two pre-battle stall samples in a 20-sample window must not decide.
constexpr int AUTO_RTT_PERCENTILE = 90;
constexpr uint32_t RTT_WARMUP_MS  = 2000;  // see control_channel.cpp

struct DelayPlan {
    int L;  // one-way trip, frames
    int R;  // planned rollback, frames
    int D;  // input delay, frames
};

inline DelayPlan SplitDelay(uint32_t rtt_ms, int window, int rcap) {
    DelayPlan p{};
    p.L = (int)(((rtt_ms / 2) + 9) / 10);
    if (window <= 0) {                        // lockstep: delay is the trip
        p.R = 0;
        p.D = std::min(std::max(1, p.L), MAX_SPLIT_DELAY);
        return p;
    }
    const int share = p.L <= AUTO_FULL_TRIP ? p.L : AUTO_FULL_TRIP + (p.L - AUTO_FULL_TRIP) / 3;
    const int rmax  = std::max(0, std::min(rcap, window - 3));
    p.R = std::min(std::max(p.L - share, 0), rmax);
    int d = std::max(1, p.L - p.R);
    d += std::max(0, 3 - (window - p.R));
    p.D = std::min(d, MAX_SPLIT_DELAY);
    return p;
}

// Session delay across matches: drop freely, rise only on a real change
// (>= 2 frames), so one noisy window neither sticks for the whole session nor
// makes the delay flap match to match. cached < 0 = nothing cached yet.
inline int ApplyDelayHysteresis(int cached, int fresh) {
    if (cached < 0) return fresh;
    if (fresh <= cached) return fresh;
    return (fresh >= cached + 2) ? fresh : cached;
}

}  // namespace fm2k::delay
