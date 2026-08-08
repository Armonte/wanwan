#pragma once
// Shared internals for the spec_playback cluster:
//
//   spec_playback.cpp       -- ApplySessionEvent + SpectatorNode_PopFrameInputs
//                              (the two sim-facing apply paths)
//   spec_playback_seam.cpp  -- the match-boundary (pb_boundary) state machine
//   spec_playback_state.cpp -- admission pacing, adaptive bank, accessors
//
// Split out of spec_playback.cpp when it reached the 1000-line cap. Everything
// here was previously file-static / inline inside that TU, so this header is
// the ONLY thing the two playback TUs share beyond the common
// spectator_node_internal.h state model.
//
// Deliberately no constants and no typedefs live here: the split history's
// standing trap is that a name given at namespace scope in a shared header
// becomes AMBIGUOUS in a TU that still holds an anonymous-namespace copy of the
// same name (DialogBoxParamA_t / kSpoofedCodePage, commit 93b6497). Function
// declarations plus one enum that exists nowhere else cannot hit that.

#include <cstdint>

namespace specnode {

// What the seam step did with this tick. The boundary block used to sit inline
// in PopFrameInputs and expressed these three outcomes as `return false` /
// `return true` / falling off the end of the block; moving it into its own TU
// makes them explicit and keeps the caller's behaviour byte-for-byte.
//
//   FALL_THROUGH -- no boundary is active, or the boundary just RELEASED into
//                   normal playback on this very tick. The caller continues to
//                   the normal pop path.
//   FEED         -- the seam drove the sim on synthetic input this tick and has
//                   already written pb_current_p1/p2 and the out-params.
//                   Caller returns true.
//   HOLD         -- nothing may advance this tick (unsubscribed, or the walk is
//                   waiting on the stream). Caller returns false.
enum class SeamStepResult : uint8_t { FALL_THROUGH, FEED, HOLD };

// The pb_boundary state machine (SEAM / PINNING), called from PopFrameInputs
// after the battle-align hold and before the deep-join hold, exactly where the
// inline block used to be.
SeamStepResult SeamStep(uint16_t* p1_input, uint16_t* p2_input);

// Re-arm the seam walk's per-boundary bookkeeping. Called from
// ApplySessionEvent's MATCH_END case on the single NONE -> SEAM edge -- the
// counters it resets are file-static in spec_playback_seam.cpp, and left sticky
// they decay the results-overrun walk's bound and the PINNING release's edge
// trigger back into level triggers from the second boundary onward.
void SeamResetWalkState();

}  // namespace specnode
