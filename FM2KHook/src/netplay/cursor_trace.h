// cursor_trace.h -- CROSS-PEER SAVE TRACE for the 2026-09-03 false-positive
// desync class (docs/dev/desync_false_positive_2026-09-03.md).
//
// THE QUESTION IT EXISTS TO ANSWER. SaveState_CalculateFingerprint folds two
// input words into the checksum GekkoNet compares between peers, read at the
// engine's LIVE cursor `*(uint32_t*)0x447EE0 & 0x3FF`. Field bundles show two
// peers at the SAME netplay frame holding cursor values one slot apart while
// rng, hp and both timers agree and the rngtrace streams re-converge. That is
// the shape of a detector firing on state the peers were never required to
// agree on -- but the WHY was never established, and the fix (taking the input
// words off the wire) must not be written before it is.
//
// The seam ring (seam_trace.h) already records the same components, but it is
// an INTRA-process instrument: capacity-bounded, reset per match, and read by
// seam_ring_check.py which compares a frame's forward save against its own
// resim saves inside ONE process. The question here is a CROSS-PEER one --
// peer 1's rows against peer 2's rows at equal (match, frame) -- and it needs
// the whole session, every generation of every save, not a window.
//
// BOTH ARMS IN ONE RUN. Since a3c1173 the wire carries `wire_fingerprint`
// (rng + both hp + the round number at 0x470044) while `gameplay_fingerprint`
// is unchanged for intra-process use. A row records BOTH, so a single session
// measures the old checksum reproducing the mismatch and the new one not, on
// the identical schedule -- two separate runs would be two different dice.
//
// AND THE PROPOSED THIRD ARM. 0x470048 (g_round_limit, what the game actually
// compares the round counter against at 0x4088C0) and 0x470050 (the per-round
// countdown, decremented at 0x408E5F) are recorded RAW. That lets the offline
// comparator synthesise the checksum a wire including them WOULD have produced
// and report whether it fires -- measuring a proposed change without shipping
// it, which is the only way to keep a new false positive distinguishable from
// a failure to fix the old one.
//
// WHAT EACH ROW CARRIES, beyond the fingerprint components:
//   * `seq`  -- a monotonic per-process save counter, so the GENERATIONS of a
//     frame are ordered. GekkoNet's SendSessionHealthCheck reads whatever
//     checksum sits in its state store at the moment the frame falls out of
//     the prediction window (game_session.cpp), NOT necessarily the last one
//     the sim produced. If both peers hold the SAME SET of generations for a
//     frame and the detector still fires, the divergence is in WHICH
//     generation each peer sampled, not in the state.
//   * the ring words at idx-1 and idx+1 as well as at idx. If peer A's word at
//     its cursor equals peer B's word one slot back, that is a cursor PHASE
//     offset over identical content, and it is provable from the CSV alone
//     rather than inferred.
//   * game_mode, because the engine's own process_game_inputs advances the
//     cursor once per engine frame and Hook_ProcessGameInputs only BLOCKS it
//     inside battle -- so any frame budget spent at a non-battle game_mode is
//     a candidate source of a one-slot skew.
//
// DARK BY DEFAULT: FM2K_CURSOR_TRACE=1 arms it. Armed, a row costs ~10 loads
// and one 60-byte store (well under 1 us) -- deliberately far below the ~6 us
// that was measured to SUPPRESS the seam violation it sits next to
// (docs/dev/seam_ring_intermittent.md). Nothing is written while a match is
// live: rows accumulate in memory and the CSV is emitted only from the paths
// that are already tearing the process down, exactly like the seam ring and
// the rngtrace. Hook logging is SYNCHRONOUS (quill is hardcoded off in
// FM2KHook), so a per-frame log here would itself change the schedule it
// measures.
#pragma once
#include <cstdint>

// FM2K_CURSOR_TRACE=1 arms the ring. Strict parse, default OFF; one contract
// line at the first call so an armed run says so in the log and a dark run
// cannot be mistaken for an armed one that found nothing.
bool CursorTrace_Enabled();

// One call per SaveState_Save, AFTER SaveState_CalculateFingerprint has run --
// the row is read out of SaveState_GetRegionChecksums().fp_inputs plus a fresh
// read of the ring neighbourhood, so it records exactly the bytes the
// fingerprint was built from and not a re-sample taken at a different instant.
void CursorTrace_OnSave(int frame, bool is_replay_save, bool rolling_back);

// Flush to FM2K_P<N>_cursortrace.csv plus a one-line summary. The summary is
// unconditional (a dark run still says "0 rows", which is the evidence that
// dark means dark); the CSV is written only when rows exist. Legal ONLY from
// paths that are about to terminate the process -- HandleDesyncDetected and
// the harness auto-terminate path -- which it reaches through SeamTrace_Dump,
// this campaign's shared "we are tearing down, flush the rings" fan-out.
void CursorTrace_Dump(int player_index, const char* reason);
