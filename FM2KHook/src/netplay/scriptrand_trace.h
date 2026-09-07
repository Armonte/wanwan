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

// Called from Hook_GameRand's gameplay branch when the call site is the script
// random branch. Records {frame, object slot, script_id, script cursor, kind}
// into an in-memory ring and folds the same tuple into a cumulative hash.
void Note(uint32_t frame);

// Cumulative FNV-1a over every tuple Note() has seen. This is the CROSS-PEER
// term: two peers running the same sim must reach the same script instructions
// in the same order, so this hash must match frame for frame. It is recorded
// per row in the desync ring, which turns "the seeds diverged" into "the two
// peers stopped agreeing about which script instruction ran".
uint32_t ContextHash();

// Write the ring next to the desync ring. Called from the desync handler.
void Dump(const char* reason, int32_t desync_frame);

}  // namespace fm2k::scriptrand_trace
