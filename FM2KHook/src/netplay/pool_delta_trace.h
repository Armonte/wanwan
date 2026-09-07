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
// non-zero) and records only the CHANGES against the previous call. Spawns and
// despawns are rare, so the ring stays tiny while carrying the exact history.
// The scan is 1024 strided reads over the 391 KB pool -- the same walk
// update_game_state already performs every frame, so the lines are hot.
namespace fm2k::pool_delta_trace {

// Called once per gekko Save, alongside the desync ring. Returns a digest of
// the CURRENT live set: FNV-1a over (slot, type) for every live slot.
//
// The digest is a SNAPSHOT, not a running total, which is what makes it valid
// between peers: a re-simulated frame recomputes it from the restored pool, so
// rollback cannot make it drift the way a cumulative hash does. (That mistake
// is documented in scriptrand_trace.h.)
uint32_t Note(uint32_t frame);

// Write the spawn/despawn history. Called from the desync handler.
void Dump(const char* reason, int32_t desync_frame);

}  // namespace fm2k::pool_delta_trace
