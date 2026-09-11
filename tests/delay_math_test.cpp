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

    // --- jitter headroom (2026-09-10) -------------------------------
    // jitter_ms defaults to 0, so every case above is unchanged. With a
    // jitter measurement the candidate gains 2x that headroom, because
    // braking tracks residual PREDICTION depth and it is the late samples
    // that force a prediction. Measured on a 300ms/20ms link: braking
    // collapsed from 7.4% to 0.5% exactly when delay reached 21.
    CHECK(DelayCandidateFromRtt(300, 0)  == 15);  // no jitter == old answer
    CHECK(DelayCandidateFromRtt(340, 20) == 21);  // the measured optimum
    CHECK(DelayCandidateFromRtt(60,  3)  == 4);   // a good link stays low
    // Ceiling is 24, not unbounded: delay 30 measured WORSE than 21.
    CHECK(DelayCandidateFromRtt(900, 90) == 24);
    CHECK(DelayCandidateFromRtt(472, 30) == 24);

    // --- JitterFromWindow: p90 - p50 spread -------------------------
    {
        using fm2k::delay::JitterFromWindow;
        const uint32_t flat[] = {100, 100, 100, 100, 100};
        CHECK(JitterFromWindow(flat, 5) == 0);     // steady link, no headroom
        // p90 index is (n-1)*9/10, so a 5-sample window puts p90 at index 3 --
        // a single outlier at index 4 is DISCARDED, exactly as RttFromWindow's
        // peak mode discards the worst ~10%. That is the intent: one cold-start
        // spike must not inflate everyone's delay.
        const uint32_t wobbly[] = {100, 100, 100, 100, 160};
        CHECK(JitterFromWindow(wobbly, 5) == 0);   // lone outlier discarded
        // n=10 puts p50 at index 4 and p90 at index 8.
        const uint32_t spread[] = {100, 100, 100, 100, 100, 100, 100, 100, 160, 200};
        CHECK(JitterFromWindow(spread, 10) == 60); // p90 160 - p50 100
        CHECK(JitterFromWindow(nullptr, 0) == 0);
        CHECK(JitterFromWindow(flat, 1) == 0);     // too small to judge
    }
    // Ceiling raised 15 -> 24 (MAX_AUTO_DELAY) on 2026-09-10: the old clamp
    // capped AUTO play at ~300 ms RTT, which is why cross-region matches ran
    // with prediction pegged and ~7% of frames braked. 400 ms now sizes
    // honestly instead of being clamped down to a value that cannot cover it.
    CHECK(DelayCandidateFromRtt(400) == 20);  // one_way 200 -> 20 (was clamp 15)

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
