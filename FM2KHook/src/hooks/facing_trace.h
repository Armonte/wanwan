// facing_trace.h -- Phase 4b diagnostic instrument for the spectator
// across-match-boundary divergence (docs/dev/ seam campaign corpus,
// seam_p4a_vanpri_forensics.md hypothesis H3).
//
// H3: the left/right facing swap is the last piece of the host's input
// post-processing that the spectator RE-DERIVES instead of RECEIVING.
// Host derivation:      hooks_getinput.cpp, the Netplay_IsActive() battle branch.
// Spectator derivation: hooks_getinput.cpp, the g_spectator_mode branch.
// Both read the same live char-slot bytes (CHAR_SLOT_BASE + 0xDF41 /+ 0x7CA6)
// at their own point in the frame; nothing forces the two reads to agree.
//
// Everything here is DARK by default (FM2K_FACING_TRACE=1 arms it) and is not
// a shipping behaviour. Sync-logging constraint: the hook's quill is hardcoded
// OFF, so this is an in-memory ring with file writes ONLY at a match boundary
// (between matches, never mid-match) and on process-exit paths.
#pragma once
#include <cstdint>

// FM2K_FACING_TRACE=1 arms the ring. Strict parse, default OFF, one contract
// line at first call.
bool FacingTrace_Enabled();

// One call per get_player_input facing decision, on BOTH planes.
//
// Returns `input_post` UNCHANGED, so the call wraps an existing expression
// rather than adding statements to hooks_getinput.cpp -- that TU is at 998
// lines and the repo's hard rule is "compiled sources stay under 1000".
//
//   plane           0 = player/host process, 1 = spectator process
//   input_post      the input AFTER the swap, BEFORE SOCD. The pre-swap value
//                   is exactly swap(input_post) when facing_reversed is false
//                   (the bit0<->bit1 swap is an involution), so recording the
//                   post value loses nothing and costs no extra local.
//   facing_reversed the decision itself: true = NO swap applied.
//
// Records nothing outside battle (game_mode in [3000,4000)) -- the facing
// derivation is battle-only and CSS frames would only dilute the ring.
uint16_t FacingTrace_Note(int plane, int player_id, int input_type,
                          uint32_t game_mode, bool facing_reversed,
                          uint16_t input_post);

// Flush the ring to FM2K_<tag>_facing.csv (append) and reset it. Legal ONLY
// from a match boundary (the battle -> non-battle edge, i.e. between matches)
// or from a path that is about to end the process. Never mid-match: it is one
// buffered write of up to ~130k rows.
void FacingTrace_Dump(const char* reason);
