// seam_trace.h -- diagnostic instruments for the match-end seam (the 967f89f
// desync campaign -- docs/dev/matchend_seam_campaign.md). Everything here is dark by
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

// One call per LOAD-SITE Fm2k_ClearAfterimageIndices() (savestate_fm2k_load.cpp,
// the `live_substate_pre >= 900 || crossing_teardown` branch). Lane A rec 3:
// after Phase 2c deleted the blanket load-park, this is the LAST load-site write
// into checksummed sim state gated on a live, rollback-schedule-dependent
// predicate -- the direct structural sibling of the write that caused the
// 967f89f desync -- and it is completely uninstrumented, which is exactly why
// p4e R3b produced no evidence. Counters are unconditional and appear in the
// [SEAM] summary (so a dark run is still evidence, as R3b's crossings=0 was);
// the per-call detail line and the +0x151 non-zero census are dark unless
// FM2K_SEAM_TRACE=1.
void SeamTrace_OnAfterimageClear(int frame, int live_substate_pre,
                                 uint32_t snap_mode, bool rolling_back,
                                 bool crossing_teardown);

// One call per SaveState_Save, after the fingerprint is computed.
void SeamTrace_PushSave(int frame, bool is_replay_save, bool rolling_back);

// RNG CALL-SITE RING (FM2K_SEAM_RNGSITE=1, default off).
//
// The intermittent seam violation has one precise signature: at match 3
// frame 827 the bad resim makes EXACTLY 3 fewer gameplay game_rand draws
// than the good one, while every region the savestate saves is byte-
// identical at the last agreeing frame. Eight cycles of "which saved region
// differs" answered "none of them" -- so stop asking that and ask which 3
// draws went missing instead.
//
// Records (match, frame, replay flag, caller, caller-of-caller) for every
// gameplay-seed draw inside a narrow frame window. Memory-only, dumped with
// the save ring, and bounded to ~1000 entries for a whole run -- so unlike
// the per-save region hashes this adds no cost on the hot path and cannot
// suppress the very bug it measures (see docs/dev/seam_ring_intermittent.md
// for why that matters here).
//
// `ra2` is the one that identifies anything: ra1 is ALWAYS the 0x4139A8
// wrapper, which is why the per-caller bucket classifier attributes 100 %
// of draws to character_state_machine and says nothing.
void SeamTrace_NoteRngDraw(uint32_t ra1, uint32_t ra2, uint32_t ra3);

// BROAD MEMORY SNAPSHOT (FM2K_SEAM_MEMSNAP=1, default off).
//
// Last systematic approach. Everything the savestate TOUCHES is now excluded:
// every saved region is byte-identical at the last agreeing frame, restoring
// inactive object-pool residue changed nothing (6/20 violations), and
// restoring the whole deliberate carve-out class changed nothing (3/20 vs
// ~30% baseline, p ~= 0.45). So the differing state is memory NOBODY saves
// and nobody carves out -- which no targeted hash can find, because the whole
// problem is not knowing where to point it.
//
// So stop pointing. Snapshot the game's entire writable data range at the
// frames around the divergence, dump raw, and diff offline between the pass
// that went on to produce the correct frame 827 and the pass that did not.
// A raw memcpy is also CHEAPER than hashing the same bytes (~150 us vs ~1 ms
// for 1.4 MB), which matters given this bug's sensitivity to added cost.
void SeamTrace_MemSnap(int frame, bool is_replay_save);

// True when the call-site ring is armed AND the current frame is inside the
// window. Checked before the (relatively costly) frame-pointer walk so an
// unarmed build pays one cached-bool test per draw.
bool SeamTrace_RngSiteWanted(int frame);

// Current battle index within the session (1-based; bumped per battle).
// Exposed because g_netplay_frame RESTARTS every battle, so a frame number
// alone cannot say WHICH match a trace line belongs to -- frame 827 exists in
// all three. Diagnostics that target the match-end seam need both.
uint16_t SeamTrace_MatchIdx();

// SCRIPT-VM OPCODE RING (armed by FM2K_CSM_SEAM=1 via csm_diag).
//
// The per-opcode fprintf tracer SUPPRESSES the violation it exists to
// diagnose: 7 fully-covered runs with it on produced 0 violations against a
// measured 50-75% base rate (p ~= 0.008 at the low end), and coverage itself
// fell to 21%. Same lesson as the per-save region hashes -- weight on the hot
// path reshuffles which frames gekko re-simulates.
//
// So record into RAM and write once at teardown, exactly like the save ring.
// ~20 bytes per opcode, bounded; no I/O while the window is live.
void SeamTrace_NoteOpcode(uint32_t seq, int32_t frame, uint32_t obj,
                          uint32_t item_idx, uint32_t script_idx,
                          int32_t f3c, uint8_t opcode,
                          const uint8_t* script_bytes);

// Flush both buffers + the episode list. Legal ONLY from paths that are about
// to terminate the process (HandleDesyncDetected, harness auto-terminate) --
// it is ~1000 fprintfs and hook logging is synchronous. Its two callers make it
// the campaign's shared "terminating path" fan-out, so it also drives
// SeamFreeProbe_LogSummary and EnvelopeShadow_Dump (both dark by default).
void SeamTrace_Dump(int player_index, const char* reason);

// Per-battle-session reset. Clears the primary ring and the episode list and
// bumps the match index; the seam-window buffer deliberately SURVIVES, because
// a green multi-match run's completed seams are exactly what the per-match
// reset used to destroy.
void SeamTrace_Reset();
