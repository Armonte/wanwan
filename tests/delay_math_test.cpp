// Unit test for the input-delay sizing math (#24).
//
// Exercises the exact pure functions the hook uses (FM2KHook/src/netplay/
// delay_math.h), so the auto-delay table is proven without a game or two
// live peers. FM2K runs at 100 Hz: one frame is 10 ms.
//
// Build + run (host compiler, no cross-toolchain needed), from repo root:
//   g++ -std=c++17 -Wall -o delay_math_test tests/delay_math_test.cpp
//   ./delay_math_test
#include "../FM2KHook/src/netplay/delay_math.h"
#include <cstdio>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr)                                                    \
    do {                                                               \
        if (expr) {                                                    \
            ++g_pass;                                                  \
        } else {                                                       \
            ++g_fail;                                                  \
            std::printf("  FAIL  line %d:  %s\n", __LINE__, #expr);     \
        }                                                              \
    } while (0)

using namespace fm2k::delay;

static bool Plan(uint32_t rtt, int window, int rcap, int L, int R, int D) {
    const DelayPlan p = SplitDelay(rtt, window, rcap);
    if (p.L == L && p.R == R && p.D == D) return true;
    std::printf("  rtt=%u W=%d rcap=%d -> L=%d R=%d D=%d (want %d/%d/%d)\n",
                rtt, window, rcap, p.L, p.R, p.D, L, R, D);
    return false;
}

int main() {
    // --- PercentileFromWindow ----------------------------------------
    CHECK(PercentileFromWindow(nullptr, 0, 50) == 0);
    { const uint32_t one[] = {42}; CHECK(PercentileFromWindow(one, 1, 95) == 42); }
    {
        // n=20: p50 index 9, p95 index floor(19*95/100) = 18.
        std::vector<uint32_t> s;
        for (uint32_t i = 1; i <= 20; ++i) s.push_back(i * 10);  // 10..200
        CHECK(PercentileFromWindow(s.data(), 20, 50) == 100);
        CHECK(PercentileFromWindow(s.data(), 20, 95) == 190);
        CHECK(PercentileFromWindow(s.data(), 20, 100) == 200);
        CHECK(PercentileFromWindow(s.data(), 20, 0) == 10);
        std::reverse(s.begin(), s.end());                         // order-independent
        CHECK(PercentileFromWindow(s.data(), 20, 95) == 190);
    }
    {
        // A lone cold-start spike in a 64-sample window does not move p95:
        // index floor(63*95/100) = 59, the spike sits at 63.
        std::vector<uint32_t> s(63, 40);
        s.push_back(400);
        CHECK(PercentileFromWindow(s.data(), 64, 95) == 40);
    }

    // --- SplitDelay: the shipped table (W=32, auto rcap 8) ------------
    //   p90 RTT   L   R   D      (0.2.84 delay / 0.2.89-0.2.90 split delay)
    CHECK(Plan(  20, 32, AUTO_RCAP,  1, 0,  1));   //  2 / 1
    CHECK(Plan(  50, 32, AUTO_RCAP,  3, 0,  3));   //  3 / 1
    CHECK(Plan( 100, 32, AUTO_RCAP,  5, 0,  5));   //  5 / 2
    CHECK(Plan( 150, 32, AUTO_RCAP,  8, 0,  8));   //  8 / 2   (the field's manual pick: 7-8)
    CHECK(Plan( 200, 32, AUTO_RCAP, 10, 2,  8));   // 10 / 3
    CHECK(Plan( 300, 32, AUTO_RCAP, 15, 5, 10));   // 15 / 7
    CHECK(Plan( 400, 32, AUTO_RCAP, 20, 8, 12));   // 15 / 12
    CHECK(Plan(1000, 32, AUTO_RCAP, 50, 8, MAX_SPLIT_DELAY));  // capped

    // --- "auto (less rollback)": rcap 0 = the whole trip (0.2.84) ------
    CHECK(Plan( 100, 32, AUTO_SMOOTH_RCAP,  5, 0,  5));
    CHECK(Plan( 200, 32, AUTO_SMOOTH_RCAP, 10, 0, 10));
    CHECK(Plan( 300, 32, AUTO_SMOOTH_RCAP, 15, 0, 15));

    // --- window interaction -------------------------------------------
    // W=0 is lockstep: no rollback, delay covers the whole trip.
    CHECK(Plan(200, 0, AUTO_RCAP, 10, 0, 10));
    // A small window caps R at W-3 and buys the rest back as delay.
    CHECK(Plan(300, 8, AUTO_RCAP, 15, 5, 10));
    // W=4: R capped at 1 (the plan wanted 2); the headroom term is 3-(4-1)=0.
    CHECK(Plan(200, 4, AUTO_RCAP, 10, 1, 9));
    // W=2: W-3 < 0, so no rollback is planned at all, plus 1 frame headroom.
    CHECK(Plan(200, 2, AUTO_RCAP, 10, 0, 11));

    // --- no measurement ------------------------------------------------
    { const DelayPlan p = SplitDelay(0, 32, AUTO_RCAP);
      CHECK(p.L == 0 && p.R == 0 && p.D == 1); }

    // --- ApplyDelayHysteresis -------------------------------------------
    CHECK(ApplyDelayHysteresis(-1, 7) == 7);   // nothing cached: take it
    CHECK(ApplyDelayHysteresis( 5, 3) == 3);   // drops freely
    CHECK(ApplyDelayHysteresis( 5, 5) == 5);
    CHECK(ApplyDelayHysteresis( 5, 6) == 5);   // +1 is noise: hold
    CHECK(ApplyDelayHysteresis( 5, 7) == 7);   // +2 is a real change

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
