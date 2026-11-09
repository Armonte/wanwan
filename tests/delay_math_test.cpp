// Unit test for the input-delay negotiation math (#24).
//
// Exercises the exact pure functions the hook uses for delay sizing,
// so the formula behind the "delay 13 on a 10 ms link" bug is proven
// without needing a game / two live peers.
//
// Build + run (host compiler, no cross-toolchain needed), from repo root:
//   g++ -std=c++17 -Wall -o /tmp/delay_math_test tests/delay_math_test.cpp
//   /tmp/delay_math_test
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

int main() {
    using fm2k::delay::RttFromWindow;
    using fm2k::delay::DelayCandidateFromRtt;

    // --- empty window -------------------------------------------------
    CHECK(RttFromWindow(nullptr, 0, 0) == 0);
    { uint32_t s[1] = {99}; CHECK(RttFromWindow(s, 0, 1) == 0); }

    // --- avg mode = arithmetic mean -----------------------------------
    { uint32_t s[] = {10, 10, 10, 10}; CHECK(RttFromWindow(s, 4, 0) == 10); }
    { uint32_t s[] = {10, 20, 30};     CHECK(RttFromWindow(s, 3, 0) == 20); }
    { uint32_t s[] = {12, 14, 16, 18}; CHECK(RttFromWindow(s, 4, 0) == 15); }

    // --- peak mode p90 ignores a lone cold-start spike ----------------
    // THE delay-13 bug: 20 pings of 10 ms + one 250 ms handshake spike.
    // Old code took the all-time max (250). p90 must take 10.
    {
        std::vector<uint32_t> s(20, 10);
        s.push_back(250);
        CHECK(RttFromWindow(s.data(), (int)s.size(), 1) == 10);
    }
    // p90 still drops the top spike with only 6 samples.
    { uint32_t s[] = {10, 10, 10, 10, 10, 250};
      CHECK(RttFromWindow(s, 6, 1) == 10); }

    // --- peak mode DOES rise under sustained jitter -------------------
    // That is the point of peak mode: 16 pings @ 10 ms, 4 @ 60 ms.
    {
        std::vector<uint32_t> s(16, 10);
        for (int i = 0; i < 4; ++i) s.push_back(60);
        CHECK(RttFromWindow(s.data(), 20, 1) == 60);
    }
    // Single-sample window: p90 can only return that sample.
    { uint32_t s[] = {250}; CHECK(RttFromWindow(s, 1, 1) == 250); }

    // --- DelayCandidateFromRtt: ceil(one_way/10), clamp [2,15] --------
    CHECK(DelayCandidateFromRtt(0)   == -1);  // no measurement
    CHECK(DelayCandidateFromRtt(10)  == 2);   // one_way 5   -> 1 -> clamp 2
    CHECK(DelayCandidateFromRtt(20)  == 2);   // one_way 10  -> 1 -> clamp 2
    CHECK(DelayCandidateFromRtt(100) == 5);   // one_way 50  -> 5
    CHECK(DelayCandidateFromRtt(200) == 10);  // one_way 100 -> 10
    CHECK(DelayCandidateFromRtt(202) == 11);  // one_way 101 -> ceil -> 11
    CHECK(DelayCandidateFromRtt(250) == 13);  // one_way 125 -> 13  (the bug)
    CHECK(DelayCandidateFromRtt(300) == 15);  // one_way 150 -> 15

    // Ceiling back to 15 (2026-11-08): 0.2.85-0.2.87's 24 cap plus jitter
    // headroom sized ordinary links to 19-24 in the field.
    CHECK(DelayCandidateFromRtt(400) == 15);
    CHECK(DelayCandidateFromRtt(900) == 15);

    // --- PercentileFromWindow ----------------------------------------
    {
        using fm2k::delay::PercentileFromWindow;
        CHECK(PercentileFromWindow(nullptr, 0, 50) == 0);
        const uint32_t one[] = {42};
        CHECK(PercentileFromWindow(one, 1, 95) == 42);
        // n=20: p50 index 9, p95 index 18 (floor(19*95/100) = 18).
        std::vector<uint32_t> s;
        for (uint32_t i = 1; i <= 20; ++i) s.push_back(i * 10);  // 10..200
        CHECK(PercentileFromWindow(s.data(), 20, 50) == 100);
        CHECK(PercentileFromWindow(s.data(), 20, 95) == 190);
        CHECK(PercentileFromWindow(s.data(), 20, 100) == 200);
        CHECK(PercentileFromWindow(s.data(), 20, 0) == 10);
        // order-independent
        std::reverse(s.begin(), s.end());
        CHECK(PercentileFromWindow(s.data(), 20, 95) == 190);
    }

    // --- full chain: 10 ms link + cold-start spike --------------------
    // Peak mode: p90 -> 10 ms -> delay 2. Was 250 ms -> delay 13.
    {
        std::vector<uint32_t> s(20, 10);
        s.push_back(250);
        uint32_t rtt = RttFromWindow(s.data(), 21, 1);
        CHECK(rtt == 10);
        CHECK(DelayCandidateFromRtt(rtt) == 2);
    }
    // Avg mode, same window: mean (200+250)/21 = 21 ms -> delay 2.
    {
        std::vector<uint32_t> s(20, 10);
        s.push_back(250);
        uint32_t rtt = RttFromWindow(s.data(), 21, 0);
        CHECK(rtt == 21);
        CHECK(DelayCandidateFromRtt(rtt) == 2);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
