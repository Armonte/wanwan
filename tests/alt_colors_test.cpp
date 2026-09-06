// Unit test for the palette picker.
//
// This exists because the two-instance netplay harness CANNOT test it: its
// FM2K_TEST_AUTO_CSS is pulse-only by design (per-peer memory pinning raced,
// so colour targeting was deliberately removed), and the default gate drives
// one fixed character and colour, which makes every colour path a no-op
// there. A green gate proves nothing was broken; it says nothing about this.
//
// So the rules are pure and exercised directly, the same arrangement
// delay_math_test.cpp uses.
//
// Build + run (host compiler), from repo root:
//   g++ -std=c++17 -Wall -o /tmp/alt_colors_test tests/alt_colors_test.cpp
//   /tmp/alt_colors_test
#include "../FM2KHook/src/hooks/alt_colors_math.h"
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
    using namespace fm2k::altcolor;

    // --- the ladder, against the disassembly of 0x406F20 ---------------
    CHECK(LadderColour(0x0000) == 0);
    CHECK(LadderColour(0x0010) == 0);   // A confirms and leaves colour 0
    CHECK(LadderColour(0x0020) == 1);
    CHECK(LadderColour(0x0040) == 2);
    CHECK(LadderColour(0x0080) == 3);
    CHECK(LadderColour(0x0100) == 4);
    CHECK(LadderColour(0x0200) == 5);
    // Last press wins, exactly as the engine's fall-through chain does.
    CHECK(LadderColour(0x0020 | 0x0200) == 5);
    // The two the engine could hold but no attack button could ask for.
    CHECK(LadderColour(0x1000) == 6);
    CHECK(LadderColour(0x2000) == 7);
    // FN is tested last, so it beats an attack button held with it -- which
    // matters because FN also has to carry bit 4 to make the engine confirm.
    CHECK(LadderColour(0x0010 | 0x1000) == 6);
    CHECK(LadderColour(0x0200 | 0x2000) == 7);

    // --- the collision rule --------------------------------------------
    // Slot 1 already holds colour 2.
    {
        // -1 = nobody has confirmed into that slot (measured).
        int colours[8] = {-1, 2, -1, -1, -1, -1, -1, -1};

        // DIFFERENT characters may now share a colour. The engine refused
        // this outright -- its loop compares only the colour.
        int diff[8] = {7, 9, -1, -1, -1, -1, -1, -1};
        CHECK(Resolve(0, 2, colours, diff, 8) == 2);

        // SAME character must not, so a mirror match stays legible. The
        // bump is the engine's: +1 modulo 8.
        int same[8] = {9, 9, -1, -1, -1, -1, -1, -1};
        CHECK(Resolve(0, 2, colours, same, 8) == 3);

        // Unknown character (team modes) keeps the engine's stricter rule.
        int unknown[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        CHECK(Resolve(0, 2, colours, unknown, 8) == 3);
    }

    // The bump wraps modulo 8 rather than stopping at 5, which is what
    // makes 6 and 7 reachable by collision even without an FN press --
    // exactly what `and esi, 0x80000007` does in the original.
    {
        int colours[8] = {-1, 7, -1, -1, -1, -1, -1, -1};
        int same[8]    = {9, 9, -1, -1, -1, -1, -1, -1};
        CHECK(Resolve(0, 7, colours, same, 8) == 0);   // 7 taken -> wraps to 0
    }

    // Two players on different characters may BOTH sit on a new palette.
    {
        int colours[8] = {6, -1, -1, -1, -1, -1, -1, -1};
        int diff[8]    = {3, 5, -1, -1, -1, -1, -1, -1};
        CHECK(Resolve(1, 6, colours, diff, 8) == 6);
    }

    // Out-of-range slots are refused rather than writing somewhere unowned.
    {
        int colours[8] = {-1}; int chars[8] = {-1};
        CHECK(Resolve(-1, 0, colours, chars, 8) == -1);
        CHECK(Resolve(8,  0, colours, chars, 8) == -1);
    }

    // An empty table must never block: every slot -1 means the first
    // player to confirm keeps exactly the colour they confirmed with,
    // including colour 0. Getting this wrong would have bumped everyone
    // off palette 0 in every match.
    {
        int colours[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        int chars[8]   = {5, -1, -1, -1, -1, -1, -1, -1};
        CHECK(Resolve(0, 0, colours, chars, 8) == 0);
        CHECK(Resolve(0, 5, colours, chars, 8) == 5);
    }

    std::printf("alt_colors_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
