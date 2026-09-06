#pragma once

// Palettes 6 and 7, reachable at last.
//
// A local organiser asked on 2026-09-05 whether the FN1/FN2 binds did
// anything, and suggested spending them on "alt 7 and 8 -- the colors in
// 2dfm css that aren't selectable by normal means". Both halves were right.
//
// AssignPlayerColor @ 0x406F20, disassembled rather than guessed, picks the
// colour with a hardcoded ladder over exactly six bits (0x10 A .. 0x200 F),
// last press wins, giving colours 0..5. But the collision-avoidance loop
// underneath wraps with `and esi, 0x80000007` and retries up to 8 times --
// the engine's colour space is ALREADY 0..7, matching the eight entries in
// the KGT structure (2dfm/CommonResource.hpp: sharedPalettes[8]). Six and
// seven are reachable today only by accident, when two players collide on
// the same pick and the second is bumped into them.
//
// So nothing is missing but a way to ASK for them. This extends the ladder
// with the two meta-bits that already exist, already have default binds
// (F1/F2) and, until now, had no consumer anywhere in the codebase.
//
// It also lifts a restriction nobody asked for: the engine's collision loop
// compares ONLY the colour, so two players on completely different
// characters still could not both pick palette 3. Here a collision counts
// only when the other slot is on the same character. Where the characters
// cannot be established (team modes, where slots 0..3 / 4..7 are not
// described by the 1v1 character grid) the engine's stricter rule stands.
//
// Online works with no protocol change: the CSS path hands the engine
// Netplay_GetCSSInput UNMASKED (hooks_getinput.cpp), and Input_CaptureLocal
// never strips meta-bits either, so the FN bits are already on the wire and
// identical on both peers. The colour byte lives in the savestate, so that
// agreement is what makes this safe.
namespace AltColors {
bool Install();
void Uninstall();
}  // namespace AltColors
