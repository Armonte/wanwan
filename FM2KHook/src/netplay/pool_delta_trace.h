#pragma once

#include <cstdint>

// WHICH OBJECT APPEARED OR DISAPPEARED.
//
// Measured 2026-09-06: in the captured player-vs-player desync the LIVE OBJECT
// COUNT (update_game_state's own g_objects_updated_this_frame) diverges at
// f=98, about a hundred frames before the rng diverges at f=194 -- and on a
// run that PASSES the two peers agree on that count at every confirmed frame,
// so the divergence is real and not a chronically-red term. The script draws
// stay identical across that whole span, which means the extra/missing objects
// do not themselves run random branches; they are upstream evidence.
//
// The count says THAT the pool diverged. This says WHICH SLOT.
//
// Each call rebuilds a 1024-bit live-slot bitmap (object type at +0x000 is
// non-zero). The scan is 1024 strided reads over the 391 KB pool -- the same
// walk update_game_state already performs every frame, so the lines are hot.
//
// NO SPAWN/DESPAWN EVENT RING. The first version diffed each call against the
// PREVIOUS call and recorded the changes. That is invalid under rollback: the
// host saves speculatively at frame N+k, rewinds to N and re-simulates, so
// objects appear to spawn and despawn repeatedly when nothing happened. It
// reported slots 88 and 89 "spawning" on eight consecutive frames. Consecutive
// calls are simply not adjacent in simulation time.
//
// What IS valid is a SNAPSHOT per row: the bitmap and its digest are recomputed
// from the restored pool on every re-simulation, so the last row for a
// confirmed frame describes that frame's agreed state and compares directly
// against the peer's. Same distinction as the deleted cumulative hash in
// scriptrand_trace.h.
namespace fm2k::pool_delta_trace {

constexpr int kBitmapWords = 32;   // 1024 slots

// Called once per gekko Save, alongside the desync ring. Fills `out_bitmap`
// with the live-slot bitmap and returns an FNV-1a digest over (slot, type) for
// every live slot.
uint32_t Note(uint32_t bitmap_out[kBitmapWords]);

}  // namespace fm2k::pool_delta_trace
