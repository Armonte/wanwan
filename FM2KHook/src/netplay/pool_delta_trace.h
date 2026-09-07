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

constexpr int kBitmapWords = 32;    // 1024 slots
// Live objects sit at ~98 for whole matches, so this holds every one of them
// with room to spare; the count is reported so a truncation can never pass as
// agreement.
constexpr int kMaxLiveTypes = 192;

// Called once per gekko Save, alongside the desync ring. Fills `bitmap_out`
// with the live-slot bitmap and `types_out` with those slots' object types in
// ascending slot order, returns an FNV-1a digest over (slot, type) for every
// live slot, and reports the live count via `live_out`.
//
// TYPES, not just liveness. Measured 2026-09-06 22:47: at the first diverging
// frame the two peers hold the SAME 98 live slots -- nothing spawned on one
// and not the other -- yet the digest differs. The digest folds (slot, type),
// so some slot holds a DIFFERENT OBJECT TYPE on the two peers. Liveness alone
// cannot see that; this can.
// `scripts_out` carries each live slot's script_id (obj +0x030). Knowing the
// TYPE says an object retired early; knowing the SCRIPT says which script
// decided to, which is what can actually be read back in the game data.
// `cursors_out` carries each live slot's script cursor (obj +0x02C) and
// `waits_out` its wait timer (obj +0x03C). Together these say whether an
// object that retires early got there by RUNNING ITS SCRIPT FASTER (cursor
// ahead of the peer's for frames beforehand) or by being deactivated from
// outside (cursors identical right up to the frame its type flips).
// `xs_out` / `ys_out` carry each live slot's position (obj +0x008 / +0x00C,
// 16.16 fixed point). POSITION IS THE POINT: character_state_machine @0x4124C9
// culls an object -- retiring it via ClearObjectReferences -- as soon as its
// position leaves the play field (x outside -50..1330, y outside -50..1010).
// The GekkoNet desync fingerprint contains NO POSITION FIELDS (see
// SaveState_CalculateFingerprint), so positions can drift apart between peers
// completely unnoticed until a drifting object crosses that boundary on one
// peer a few frames before the other -- which is exactly the observed shape.
uint32_t Note(uint32_t bitmap_out[kBitmapWords],
              uint8_t types_out[kMaxLiveTypes],
              uint16_t scripts_out[kMaxLiveTypes],
              uint32_t cursors_out[kMaxLiveTypes],
              uint16_t waits_out[kMaxLiveTypes],
              int32_t xs_out[kMaxLiveTypes],
              int32_t ys_out[kMaxLiveTypes],
              int* live_out);

// WHO RETIRED THE OBJECT.
//
// An object leaves the pool by having its type set to 1. Every in-battle path
// that does so funnels through ClearObjectReferences @0x40E4A0, which retires
// the CURRENT object and is called from four sites inside
// character_state_machine -- one of which is the opcode dispatcher's DEFAULT
// case, i.e. an unrecognised opcode, which logs "ScriptMainLoopError" to the
// game's own debug overlay (not to any file we keep) and then destroys the
// object.
//
// Guessing between those four sites from downstream state has failed twice
// already, so this records the answer directly: the retiring object's slot and
// the RETURN ADDRESS, which names the call site outright.
//   0x4124F5 / 0x412510 / 0x412756 -- normal script paths
//   0x413BCB                       -- the ScriptMainLoopError default case
bool InstallRetireHook();
void DumpRetires(const char* reason, int32_t desync_frame);
// Set by the netplay layer so each retire can be stamped with its frame.
void SetRetireFrame(uint32_t frame);

}  // namespace fm2k::pool_delta_trace
