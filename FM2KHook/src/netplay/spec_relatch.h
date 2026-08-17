// spec_relatch.h -- ROUND-LIMIT LATCH re-derive, SPECTATOR/REPLAY PLANE.
//
// Declarations only. The bug, the measured evidence, the precedes-every-save
// proof, the ordering proof against the snapshot apply and every trap
// disposition live in spec_relatch.cpp's header comment.
//
// One-line statement of the bug this closes: the engine latches g_round_limit
// (0x470048) from g_default_round (0x430124) when a sim crosses into battle,
// and a VIEWER whose engine crosses before the host's settings have landed
// keeps its own game.ini round count for the whole match. 3297b25 fixed this
// for the two PLAYER processes at the battle-entry barrier; that barrier is
// unreachable on a viewer (hooks_update.cpp:243 returns before the battle-sync
// block when g_spectator_mode), so nothing corrected a viewer's stale latch.
// Measured on a live viewer: rounds=1 latch_rounds=3 (phase6_sweep_impl.md F4).
#pragma once

#include <cstdint>

// THE ONE KILL SWITCH FOR BOTH PLANES: FM2K_ROUNDS_RELATCH (default ON).
// Defined here rather than in netplay_barriers.cpp so the player-side
// re-derive and this one can never disagree about what "off" means, and so a
// single A/B arm moves both. Strict parse ("0"/"1" only), fails SAFE (fix
// stays on) on anything else, emits ONE contract line per process on first
// evaluation naming both planes.
bool RoundsRelatch_Enabled();

namespace specnode {

// Called from the MATCH_START op apply (spec_playback.cpp), immediately after
// the existing restore of the SOURCE word 0x430124 from the same header.
// ApplySessionEvent has TWO callers -- the ordered drain and the seam
// results-overrun walk (spec_playback_seam.cpp:191, which applies non-INPUT
// head ops while mode == 2000) -- and the second is the path by which the
// pre-emptive (mode < 3000) apply actually happens.
//   hdr_round_latch -- THE AUTHORITY. The host's g_round_limit (0x470048), the
//                      LATCH its own sim ran this match on, as stamped at
//                      MATCH_START header h+89 (ReplayHeader::reserved[0])
//                      AFTER the host's own RederiveBattleEntryLatches.
//                      0 = NOT CARRIED (legacy producer, foreign/older stream,
//                      or a host latch outside 1..9) -> refusal, never a write.
//   hdr_round_count -- the host's g_default_round (h+85), the config SOURCE.
//                      Diagnostic here ONLY: it is what the viewer's own engine
//                      will re-latch from if it has not latched yet, so the two
//                      being different is the interesting thing to print. NEVER
//                      the write source -- that inversion is the bug 3297b25
//                      exists to refute one plane over.
//   hdr_stage_id    -- the header's stage id (h+80). DETECT ONLY, never written.
void SpecRelatch_OnMatchStart(uint8_t hdr_round_latch, uint32_t hdr_round_count,
                              int32_t hdr_stage_id);

// Always-on tripwire, called at the viewer's FIRST battle frame of a match
// (trampoline_spectator.cpp, next to the CSS-park tripwire). Re-reads
// g_round_limit and logs LOUDLY if anything between the MATCH_START apply and
// the first simulated frame put a different value back -- the one live
// candidate being the pool-resync / deep-join snapshot apply, which memcpys
// the GAME_STATE region that CONTAINS this word. Silent when no MATCH_START
// header has been applied for this match (nothing to assert against).
//
// COVERAGE, stated honestly (review E1(b)): SpectatorNode_ApplyPendingSnapshot
// runs at the TOP of RunSpectatorTick, before the sim loop, so this tripwire
// witnesses only the snapshot applies that land BEFORE the viewer's first
// battle frame of the match. A blob applied on a LATER tick is outside the
// assertion; that window is covered by argument (the anchor == consumed admit
// rule) rather than by measurement. Deciding observable if it is ever doubted:
// a mid-match SaveState_LoadFromBytes line in a viewer log that post-dates
// that match's "frame-zero ok".
void SpecRelatch_OnBattleFrameZero();

}  // namespace specnode
