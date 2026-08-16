// seam_free_probe.h -- [ENDSEAM-FREE] telemetry for the match-end seam.
//
// Phase 2c instrument. TELEMETRY ONLY: it changes no engine behaviour, it
// parks nothing and it suppresses nothing. It exists to answer one question
// that no artifact in the seam campaign could answer: does a character-slot
// FREE ever actually happen while a teardown-crossing rollback could still
// restore a pre-902 frame?
//
// Why the question matters (see docs/dev/matchend_seam_campaign.md; the phase
// reports seam_p2a_design sec 1.3 and seam_p2b_adversarial A5/A9): the blanket load-site park that
// Phase 2c deletes only ever had a job in that case. The Phase 2b review
// derived from disasm that the case cannot occur in netplay -- netplay
// hard-zeroes both players' inputs whenever the live mode is not battle
// (hooks_getinput.cpp), so the CSS cursor cannot move inside the window,
// player_data_file_loader (0x4039F0) always early-returns on
// g_slot_loaded_char_idx (0x4CF9E0), and no GlobalFree is reachable. This
// probe turns that derivation into a measured claim, and it is the standing
// detector for the one residual the deletion leaves open: a DESYNC paired
// with in_window > 0 is the signature that would justify the design's Stage 2
// (character-reload suppression), which is deliberately NOT implemented now.
//
// The window is also sampled for non-zero CSS direction input, so a report can
// say "case B was not exercised" rather than the much stronger and unearned
// "case B does not occur".
#pragma once
#include <cstdint>

// MinHook detour on ClearCharacterSlot @ 0x4039B0 -- the ONLY route to a
// character-slot GlobalFree (it calls resource_cleanup_manager @ 0x403520).
// Verified free of a detour collision: css_fastsound.cpp only CALLS that
// address, it hooks 0x4039F0 / 0x403600 / 0x403520 instead. Queues with
// MH_QueueEnableHook so InitializeHooks pays one MH_ApplyQueued.
bool SeamFreeProbe_Install();

// Called from the post >= 902 branch of Hook_vs_round_function. Sticky latch:
// a rollback that takes the sim back before the edge keeps the window open,
// which is correct -- a crossing load is exactly what the window is about.
void SeamFreeProbe_OnMatchCompleteTick();

// Called from the engine's per-tick control point (Hook_UpdateGameState).
// Counts ticks with the window open, and of those, ticks with non-zero
// g_processed_input -- the "was case B exercised at all" observable. No
// logging, two loads and a branch; safe per frame.
void SeamFreeProbe_OnTick();

// Close the window. Called from RoundEvents_OnMatchStart and Netplay_EndBattle
// (the battle-end swap point, which is the last moment any SaveState_Load can
// reach a pre-902 frame).
void SeamFreeProbe_CloseWindow();

// One summary line, emitted from SeamTrace_Dump (desync / harness
// auto-terminate). Counts only -- never per-call spam; hook logging is
// synchronous fputs+fflush.
void SeamFreeProbe_LogSummary(const char* reason);

// In-window free count, for callers that want the scalar rather than the line.
uint32_t SeamFreeProbe_InWindowFreeCount();
