// Pure colour-selection math for the palette picker.
//
// Kept dependency-free (no windows.h, no SDL, no game addresses) so the
// EXACT code that runs in the hook can be exercised by a host-compiled unit
// test -- the same arrangement delay_math.h uses, and for the same reason:
// the two-instance netplay harness cannot request specific colours (its
// FM2K_TEST_AUTO_CSS is pulse-only by design, because per-peer memory
// pinning raced), so the only honest way to test the rules is to run them
// directly.
#pragma once

#include <cstdint>

namespace fm2k::altcolor {

constexpr int kColours = 8;      // engine wraps mod 8; data holds 8 palettes
constexpr uint16_t kFn1 = 0x1000;
constexpr uint16_t kFn2 = 0x2000;

// Replicates AssignPlayerColor @ 0x406F20's ladder and extends it.
//
// The original tests six bits, last press winning. Note A (0x10) is absent
// on purpose: its `test al,0x10 / je` jumps to the very next instruction, so
// confirming with A leaves colour 0. FN1/FN2 are tested last, which makes
// them win over any attack button held at the same time.
inline int LadderColour(uint16_t in) {
    int c = 0;
    if (in & 0x0020u) c = 1;
    if (in & 0x0040u) c = 2;
    if (in & 0x0080u) c = 3;
    if (in & 0x0100u) c = 4;
    if (in & 0x0200u) c = 5;
    if (in & kFn1)    c = 6;
    if (in & kFn2)    c = 7;
    return c;
}

// Resolve `slot`'s colour against the other slots.
//
// The engine's loop treats ANY slot holding the colour as a collision, so
// two players on completely different characters could not share a palette.
// Here a collision counts only when the other slot is on the SAME character.
// A character of -1 means "unknown" (team modes, whose slots the 1v1
// character grid does not describe) and falls back to the engine's stricter
// behaviour rather than guessing.
//
// The bump is the engine's own: +1 wrapping modulo 8, up to eight attempts,
// then give up and change nothing (which is what `cmp ebx,8` does).
// Returns the resolved colour, or -1 for "leave it alone".
inline int Resolve(int slot, int desired,
                   const int* colours, const int* chars, int n_slots) {
    if (slot < 0 || slot >= n_slots) return -1;
    const int mine = chars[slot];
    for (int attempt = 0; attempt < kColours; ++attempt) {
        bool taken = false;
        for (int other = 0; other < n_slots; ++other) {
            if (other == slot) continue;
            // An unoccupied slot holds -1, never a valid colour, so it
            // cannot collide. Measured 2026-09-05: every slot reads
            // 0xFFFFFFFF until someone confirms into it, which is exactly
            // why the engine can scan all eight regardless of game mode.
            if (colours[other] != desired) continue;
            const int theirs = chars[other];
            if (mine < 0 || theirs < 0 || theirs == mine) { taken = true; break; }
        }
        if (!taken) return desired;
        desired = (desired + 1) % kColours;
    }
    return -1;
}

}  // namespace fm2k::altcolor
