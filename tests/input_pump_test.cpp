// Unit test for the SDL pump throttle.
//
// This exists because the behaviour it guards only appears with TWO PHYSICAL
// PADS connected, which no machine we test on has: with no pad open the hook
// never pumps at all, so a live run here cannot tell a working throttle from
// a broken one. The decision is therefore pure and exercised directly, the
// same arrangement delay_math_test.cpp and alt_colors_test.cpp use.
//
// Build + run (host compiler), from repo root:
//   g++ -std=c++17 -Wall -o /tmp/input_pump_test tests/input_pump_test.cpp
//   /tmp/input_pump_test
#include "../FM2KHook/src/ui/input_pump.h"
#include <cstdio>

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
    using fm2k::inpump::PumpState;
    using fm2k::inpump::ShouldPump;

    // --- THE CASE THIS WAS WRITTEN FOR: local 2P, one frame ------------
    // Two players sampled in the same frame must produce exactly ONE pump.
    {
        PumpState st;
        CHECK(ShouldPump(st, 100, true) == true);    // P1 samples: pumps
        CHECK(ShouldPump(st, 100, true) == false);   // P2 samples: reuses
        // Solo-driver paths call Sample_Win32(0) again on the same frame.
        CHECK(ShouldPump(st, 100, true) == false);
        CHECK(ShouldPump(st, 100, true) == false);
    }

    // --- the frame advances, so state must refresh ----------------------
    {
        PumpState st;
        CHECK(ShouldPump(st, 7, true) == true);
        CHECK(ShouldPump(st, 7, true) == false);
        CHECK(ShouldPump(st, 8, true) == true);      // new frame -> pump
        CHECK(ShouldPump(st, 8, true) == false);
        CHECK(ShouldPump(st, 9, true) == true);
    }

    // --- frame 0 is a real frame, not "unset" --------------------------
    // A sentinel-based implementation gets this wrong on the very first
    // frame of a match, which is where input matters most.
    {
        PumpState st;
        CHECK(ShouldPump(st, 0, true) == true);
        CHECK(ShouldPump(st, 0, true) == false);
    }

    // --- 0xFFFFFFFF is a legal counter reading, not a sentinel ----------
    // The first draft used it as "no last frame", which would have meant
    // never pumping on the frame that happened to equal it.
    {
        PumpState st;
        CHECK(ShouldPump(st, 0xFFFFFFFFu, true) == true);
        CHECK(ShouldPump(st, 0xFFFFFFFFu, true) == false);
        CHECK(ShouldPump(st, 0u, true) == true);          // wrap keeps working
    }

    // --- no frame counter: behave exactly as before the throttle --------
    // FM95, or any context whose counter does not advance. Pumping every
    // time is the safe answer; silently never refreshing input is not.
    {
        PumpState st;
        for (int i = 0; i < 5; ++i) {
            CHECK(ShouldPump(st, 0, false) == true);
        }
    }

    // --- a frozen counter must not wedge into "never pump again" --------
    // If the counter stops advancing while the flag says it is available,
    // we pump once and then stop -- deliberate, and the binder UI does its
    // own explicit pumps. Encoded so the behaviour is a choice, not a
    // surprise someone rediscovers.
    {
        PumpState st;
        CHECK(ShouldPump(st, 42, true) == true);
        for (int i = 0; i < 10; ++i) {
            CHECK(ShouldPump(st, 42, true) == false);
        }
        CHECK(ShouldPump(st, 43, true) == true);   // recovers the moment it moves
    }

    // --- a counter that jumps backwards (rollback re-sim) still pumps ---
    {
        PumpState st;
        CHECK(ShouldPump(st, 500, true) == true);
        CHECK(ShouldPump(st, 496, true) == true);  // different frame -> pump
        CHECK(ShouldPump(st, 496, true) == false);
    }

    std::printf("input_pump_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
