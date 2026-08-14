// seam_trace.h -- diagnostic instruments for the match-end seam (the 967f89f
// desync campaign, /home/teo/specrel-2026-08-07/). Everything here is dark by
// default and none of it is a shipping behaviour.
//
// Four pieces, all documented in seam_trace.cpp:
//   1. SeamTrace_OnCrossingTeardown -- episodic [SEAM] marker at the
//      crossing_teardown observation point in SaveState_Load. The predicate
//      no longer parks anything (Phase 2c deleted the blanket load-park); it
//      survives as a PURE OBSERVATION of "this run entered the hazard".
//   2. SeamTrace_PushSave / SeamTrace_Dump -- an in-memory ring of the
//      fingerprint components per SAVE (forward and resim), plus a SECOND
//      seam-window buffer that survives the per-match ring reset, flushed to
//      FM2K_P<N>_seamring.csv from HandleDesyncDetected and from the harness
//      auto-terminate path (both already terminate the process).
//   3. SeamGuard_LegacyLoadParkEnabled -- FM2K_SEAM_LEGACY_PARK=1 restores
//      the deleted blanket park. DIAGNOSTIC A/B LEVER ONLY, default OFF.
//   4. SeamTrace_Reset -- per-battle-session reset.
#pragma once
#include <cstdint>

// FM2K_SEAM_TRACE=1 arms the per-save rings and the per-load [SEAM] lines.
bool SeamTrace_Enabled();

// FM2K_SEAM_LEGACY_PARK=1 restores the pre-Phase-2c blanket load-site park.
// Strict parse, default OFF (= the new, fixed behaviour); unrecognised values
// fail to OFF with an Error line. Exactly one contract line at first call.
//
// This is the A/B lever that proves the fix is the causal variable: with it
// set, the match-end-seam DESYNC reappears. It is NOT a safety switch and it
// must never become a default.
bool SeamGuard_LegacyLoadParkEnabled();

// One call per crossing_teardown load. parkable = how many type-4 slots a
// blanket park WOULD change (0 = the restore is an identity write, > 0 = this
// restore resurrected VMs the live process had already parked). park_ran
// records whether the legacy lever actually parked.
void SeamTrace_OnCrossingTeardown(int frame, uint32_t live_mode_pre,
                                  uint32_t snap_mode, int parkable,
                                  bool rolling_back, bool park_ran);

// One call per SaveState_Save, after the fingerprint is computed.
void SeamTrace_PushSave(int frame, bool is_replay_save, bool rolling_back);

// Flush both buffers + the episode list. Legal ONLY from paths that are about
// to terminate the process (HandleDesyncDetected, harness auto-terminate) --
// it is ~1000 fprintfs and hook logging is synchronous.
void SeamTrace_Dump(int player_index, const char* reason);

// Per-battle-session reset. Clears the primary ring and the episode list and
// bumps the match index; the seam-window buffer deliberately SURVIVES, because
// a green multi-match run's completed seams are exactly what the per-match
// reset used to destroy.
void SeamTrace_Reset();
