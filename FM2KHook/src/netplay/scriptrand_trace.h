#pragma once

#include <cstdint>

// WHICH SCRIPT OBJECT DREW THE RANDOM.
//
// Measured 2026-09-06: 100% of gameplay-seed game_rand draws during a netplay
// match come from ONE call site -- the script VM's random-branch opcode at
// 0x4139A3 (`game_rand() % (operand+1)` compared against a threshold, then a
// conditional jump to a script label). Every other site is either render-only
// (and isolated behind g_render_rng_seed) or CPU-AI-only (and both fighters
// are human). See docs/dev/desync_campaign_state.md.
//
// The surviving player-vs-player desync is EXACTLY ONE extra such draw on one
// peer, with p1_hp/p2_hp/round still identical. The draw happens before the
// branch is evaluated, so the count does not depend on the branch outcome --
// only on whether the opcode was REACHED. That makes the question precise:
// which object's script reached it one extra time, and at which instruction.
//
// update_game_state @0x404CD0 publishes g_css_object_ptr (0x4CFA00) before
// every update_game_object dispatch, so at the instant of a draw that pointer
// IS the object executing the script. Reading it plus the object's script
// cursor costs a handful of loads on a path that runs ~7 times a frame. No
// stack scanning, no frame-pointer walk, no heuristics.
namespace fm2k::scriptrand_trace {

// Called from Hook_GameRand's gameplay branch, AFTER the draw, when the call
// site is the script random branch. Records {frame, object slot, kind, owning
// player slot, script_id, script cursor, drawn value}.
//
// The VALUE is the point. Two peers that entered a frame with the same seed
// and made the same earlier draws necessarily roll the SAME number here, so if
// one takes the branch and the other does not, the difference is in the branch
// operands ([edi+1] modulus and [edi+3] threshold, read from the script blob)
// or in which instruction edi actually points at -- not in the randomness.
// Recording the value is what tells those apart.
//
// NO CUMULATIVE HASH. An earlier version folded every draw into a running
// hash and carried it in the desync ring. That term is invalid between peers
// by construction: a peer that speculates and rolls back folds each replayed
// draw again, so the host folded 16365 draws where the guest folded 1475 and
// the hash "diverged" at frame 4 of a divergence that really begins at 194.
// Compare the per-frame draw SEQUENCES instead (tools/scriptrand_diff.py).
void Note(uint32_t frame, uint32_t value);

// Write the ring next to the desync ring. Called from the desync handler.
void Dump(const char* reason, int32_t desync_frame);

}  // namespace fm2k::scriptrand_trace
