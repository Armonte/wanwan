// seam_free_probe.cpp -- [ENDSEAM-FREE] telemetry. See seam_free_probe.h for
// why this exists and what question it answers. TELEMETRY ONLY.
//
// LOGGING CONSTRAINT (repo-wide, and the reason this file counts instead of
// logging): hook logging is SYNCHRONOUS -- quill is hardcoded off in
// FM2KHook, every SDL_Log* is fputs+fflush. So the detour NEVER logs per
// call. It counts, and it emits at most a handful of episodic lines per match
// plus one summary line at dump time.

#if !defined(ENGINE_FM95)

#include "seam_free_probe.h"

#include <SDL3/SDL_log.h>
#include <cstdint>
#include <windows.h>

#include "MinHook.h"

#include "../core/globals.h"   // g_is_rolling_back, g_player_index, FM2K addrs
#include "hooks.h"             // IsBattleMode

// Confirmed battle frame, for correlating an in-window free against a
// [SEAM] episode / a DESYNC line. Declared here rather than pulled from
// netplay_internal.h (which is netplay-private) for the same reason
// round_events.cpp declares Hook_GetSOCDModePublic locally.
extern uint32_t g_netplay_frame;

namespace {

// ClearCharacterSlot @ 0x4039B0 -- resource_cleanup_manager (GlobalFree of
// +0x110/+0x114/+0x118/+0x221C) + memory_clear(slot, 0xDEED) +
// g_slot_loaded_char_idx[slot] = -1. It is the ONLY route to a character-slot
// free: player_data_file_loader (0x4039F0) reaches a free only through it, and
// css_fastsound's cache-eviction path calls this address directly. A detour
// here therefore observes BOTH producers, which is what a free counter wants.
constexpr uintptr_t ADDR_CLEAR_CHARACTER_SLOT = 0x004039B0;

// g_processed_input @ 0x447F40 -- u32 per player. The CSS cursor is driven
// from these bits (game_state_manager STATE 1, sub_406E70 x4), and netplay
// hard-zeroes them for both players whenever the live mode is not battle
// (hooks_getinput.cpp). Sampling it inside the window is what separates
// "case B does not occur" from "case B was never exercised".
constexpr uintptr_t ADDR_PROCESSED_INPUT = 0x00447F40;

using ClearSlotFn = int (__cdecl*)(int slot);
ClearSlotFn orig_ClearCharacterSlot = nullptr;

// Window latch. Set from the sim-side match-complete edge, cleared at match
// start and at Netplay_EndBattle. Sticky by design (a rollback back past the
// edge keeps it open) -- a crossing load is precisely the event the window is
// meant to cover. DO NOT "fix" the stickiness.
bool     g_window_open        = false;
uint32_t g_free_total         = 0;   // every ClearCharacterSlot this process
uint32_t g_free_in_window     = 0;   // of those, with the window open
uint32_t g_window_ticks       = 0;   // ticks observed with the window open
uint32_t g_window_css_ticks   = 0;   // of those, with a NON-battle live mode
uint32_t g_window_input_ticks = 0;   // of those, with non-zero CSS dir input
bool     g_spectator_proc     = false;  // window structurally disabled here
int      g_lines              = 0;   // episodic line budget, per match

// One free is a loud finding; a burst is still bounded. Same shape as
// SEAM_DETAIL_LINE_CAP in seam_trace.cpp, and for the same fflush reason.
constexpr int SEAM_FREE_LINE_CAP = 8;

int __cdecl Hook_ClearCharacterSlot(int slot) {
    ++g_free_total;
    if (g_window_open) {
        ++g_free_in_window;
        if (g_lines < SEAM_FREE_LINE_CAP) {
            ++g_lines;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[ENDSEAM-FREE] slot=%d live_mode=%u frame=%u rb=%d "
                "in_window=1 (#%u)",
                slot, *(const uint32_t*)FM2K::ADDR_GAME_MODE,
                g_netplay_frame, g_is_rolling_back ? 1 : 0, g_free_in_window);
        }
    }
    return orig_ClearCharacterSlot ? orig_ClearCharacterSlot(slot) : 0;
}

}  // namespace

bool SeamFreeProbe_Install() {
    if (MH_CreateHook((void*)ADDR_CLEAR_CHARACTER_SLOT,
                      (void*)Hook_ClearCharacterSlot,
                      (void**)&orig_ClearCharacterSlot) != MH_OK) {
        // Loud, not a warn: a silently-absent probe reads as "0 in-window
        // frees", which is exactly the wrong triage conclusion (this is the
        // trap seam_p2b_adversarial.md A7 found in css_fastsound's installer).
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[ENDSEAM-FREE] MH_CreateHook(ClearCharacterSlot @ %p) FAILED -- "
            "in-window free counts will read 0 for the WRONG reason",
            (void*)ADDR_CLEAR_CHARACTER_SLOT);
        return false;
    }
    if (MH_QueueEnableHook((void*)ADDR_CLEAR_CHARACTER_SLOT) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[ENDSEAM-FREE] MH_QueueEnableHook(ClearCharacterSlot) FAILED -- "
            "in-window free counts will read 0 for the WRONG reason");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[ENDSEAM-FREE] probe queued on ClearCharacterSlot @ 0x%08X "
        "(telemetry only)",
        (unsigned)ADDR_CLEAR_CHARACTER_SLOT);
    return true;
}

void SeamFreeProbe_OnMatchCompleteTick() {
    // PLAYER PROCESSES ONLY (g_player_index 0/1; 2 is the spectator/replay
    // sentinel set in Netplay_Init). The window models "a rollback can still
    // restore a pre-902 frame over a torn-down battle", and per the Phase 2b
    // review (seam_p2b_adversarial.md A1/A2) that predicate is structurally
    // unreachable in a spectator or replay process: Gekko::SpectatorSession
    // emits no Load events at all, and the only snapshot apply a viewer takes
    // is gated on the VIEWER's own game_mode >= 3000, i.e. on it being in
    // battle, which makes crossing_teardown false by construction.
    //
    // This gate is load-bearing for the telemetry, not cosmetic: a spectator
    // DOES walk its own CSS between matches with a moving cursor (it replays
    // the host's CSS inputs), so it genuinely frees character slots there.
    // Measured 8 such frees per 3-match run. Counting them as in_window would
    // make the one counter that gates the design's Stage 2 fire on every
    // healthy run forever. They stay in `frees` (the total), which is where
    // they belong.
    if (g_player_index >= 2) {
        g_spectator_proc = true;
        return;
    }
    g_window_open = true;
}

void SeamFreeProbe_OnTick() {
    // Sampled from the engine's per-tick control point rather than from the
    // 902 edge, and this placement is load-bearing: vs_round_function stops
    // being walked almost immediately after the edge (game_state_manager takes
    // over the transition), and every tick it DOES run is still a battle-mode
    // tick, where non-zero input is normal and says nothing. The interesting
    // part of the window is the CSS-mode tail, which only this site sees.
    if (!g_window_open) return;
    ++g_window_ticks;
    // Only the NON-BATTLE tail of the window can move a CSS cursor, and only a
    // CSS cursor move can reach a free. The window opens at the 902 edge while
    // the live mode is still battle, where non-zero input is ordinary and says
    // nothing -- counting those would report "input was live in the window" on
    // every healthy run and invert the conclusion this counter exists to
    // support. css_ticks is the denominator that makes input_ticks readable.
    const uint32_t mode = *(const uint32_t*)FM2K::ADDR_GAME_MODE;
    if (IsBattleMode(mode)) return;
    ++g_window_css_ticks;
    const uint32_t* in = (const uint32_t*)ADDR_PROCESSED_INPUT;
    if (in[0] != 0 || in[1] != 0) ++g_window_input_ticks;
}

void SeamFreeProbe_CloseWindow() {
    g_window_open = false;
    g_lines       = 0;
}

uint32_t SeamFreeProbe_InWindowFreeCount() { return g_free_in_window; }

void SeamFreeProbe_LogSummary(const char* reason) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[ENDSEAM-FREE] summary: frees=%u in_window=%u window_ticks=%u "
        "css_ticks=%u css_input_ticks=%u open=%d spectator=%d (%s)",
        g_free_total, g_free_in_window, g_window_ticks, g_window_css_ticks,
        g_window_input_ticks, g_window_open ? 1 : 0, g_spectator_proc ? 1 : 0,
        reason ? reason : "");
}

#else  // ENGINE_FM95

#include "seam_free_probe.h"

// FM95 has different character-slot plumbing and no 0x4039B0; the whole seam
// campaign is FM2K-only.
bool     SeamFreeProbe_Install()              { return true; }
void     SeamFreeProbe_OnMatchCompleteTick()  {}
void     SeamFreeProbe_OnTick()               {}
void     SeamFreeProbe_CloseWindow()          {}
uint32_t SeamFreeProbe_InWindowFreeCount()    { return 0; }
void     SeamFreeProbe_LogSummary(const char*) {}

#endif
