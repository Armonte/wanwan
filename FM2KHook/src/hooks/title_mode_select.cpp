// title_mode_select.cpp -- see title_mode_select.h for the full mechanism.
//
// Two decisions live here, and they are DELIBERATELY sourced differently:
//
//   REFUSAL is decided from the CONTENT BYTE g_gameConfig_modeEnableBits
//   (0x4438A4), which is the same byte title_screen_manager's own menu builder
//   reads. It is loaded out of the .kgt at content-load time, long before any
//   title/CSS state machine runs, and it is authoritative for "does this game
//   have a VS mode at all" independent of whether the player ever reaches the
//   title (FM2K_BOOT_TO_CSS_DIRECT=1 skips the title entirely -- a fail-closed
//   rule keyed on "we never saw the built menu list" would have refused that
//   perfectly good path). 5/5 agreement with the measured mode flag across the
//   sweep, and it predicts the flag as a pure function of the file.
//
//   THE INDEX is decided by scanning the list the engine actually BUILT
//   (g_titleMenu_modeList @0x424E40, valid entries 0..g_titleMenu_maxIndex
//   @0x424E60) for the VS value 1. Reading the built list rather than
//   re-deriving it from the config byte means a future engine build that
//   orders or gates the menu differently still resolves correctly.
//
// IDA (WonderfulWorld_ver_0946): 0x424780, 0x424E40 and 0x424E60 have 8, 5 and
// 3 xrefs respectively and EVERY ONE of them is inside title_screen_manager
// @0x4080A0. Nothing else in the binary reads or writes the menu cursor or the
// mode list, which is what bounds the blast radius of the one write below.

#include "title_mode_select.h"

#include "../core/globals.h"
#include "../ui/shared_mem.h"

#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// --- engine globals (FM2K) -------------------------------------------------
constexpr uintptr_t kAddrMenuSelection  = 0x424780;  // g_menu_selection
constexpr uintptr_t kAddrModeList       = 0x424E40;  // g_titleMenu_modeList[]
constexpr uintptr_t kAddrModeListMaxIdx = 0x424E60;  // g_titleMenu_maxIndex = n-1
constexpr uintptr_t kAddrModeEnableBits = 0x4438A4;  // g_gameConfig_modeEnableBits

// Bits of the config byte, in the order the engine appends them.
constexpr uint8_t kCfgStory = 0x04;
constexpr uint8_t kCfgVs    = 0x08;
constexpr uint8_t kCfgTeam  = 0x10;
constexpr uint8_t kCfgAnyMode = (uint8_t)(kCfgStory | kCfgVs | kCfgTeam);

// Values written into g_titleMenu_modeList[] (and thence g_game_mode_flag):
// 0 = 1P/STORY, 1 = VS 1v1, 2 = TEAM.
constexpr int32_t kModeStory = 0;
constexpr int32_t kModeVs    = 1;

// g_titleMenu_maxIndex sits at kAddrModeList + 0x20, so the list can never
// hold more than 8 dwords no matter what a future engine build does. Bound
// the scan by that and by the 3 modes the builder can actually append.
constexpr int32_t kMaxListEntries = 8;

TitleModeVerdict g_verdict = TitleModeVerdict::Pending;

bool EnvIsExactly(const char* name, const char* want) {
    const char* v = std::getenv(name);
    return v && std::strcmp(v, want) == 0;
}

// Kill switch, default ON. FM2K_NO_VS_REFUSE=0 restores the pre-fix behaviour
// byte-for-byte (silent 1P/story netplay) for A/B triage. Never ship it off.
bool RefusalEnabled() {
    static const bool s_on = !EnvIsExactly("FM2K_NO_VS_REFUSE", "0");
    return s_on;
}

// TEST-ONLY levers. Both are loud every time they act so a forced run can
// never be mistaken for a real verdict.
bool ForceNoVs() {
    static const bool s_on = EnvIsExactly("FM2K_TITLE_FORCE_NO_VS", "1");
    return s_on;
}
// FM2K_TITLE_FORCE_MODE_INDEX=<n> pins g_menu_selection at n instead of the
// resolved VS index. This is the causality lever from the diagnosis: =0 on a
// [STORY, VS] title yields a real mode-0 (story) session on VS-capable content,
// which is what the harness's mode stamp must catch.
int ForcedMenuIndex() {
    static const int s_idx = []{
        const char* v = std::getenv("FM2K_TITLE_FORCE_MODE_INDEX");
        if (!v || !v[0]) return -1;
        for (const char* p = v; *p; ++p) if (*p < '0' || *p > '9') return -1;
        return std::atoi(v);
    }();
    return s_idx;
}

// Is a LIVE netplay or spectator session armed in this process? Offline play,
// offline replay, the stress/determinism harness and true-offline must all be
// left alone: story-only games are legitimately playable offline and pass the
// offline determinism sweep today.
//
// This mirrors dllmain.cpp's OWN arming condition verbatim -- it calls
// Netplay_InitAsSpectator under `g_spectator_mode` and Netplay_Init under
// `!g_offline_mode && !g_stress_mode` -- so "we refuse" and "a session was
// armed" cannot drift apart. Launcher side: StartLocalSession sets
// FM2K_TRUE_OFFLINE=1 (offline play), StartStressSession sets
// FM2K_STRESS_MODE=1 (the determinism sweep), the replay path sets
// FM2K_REPLAY_FILE. FM2KLauncher::LaunchGame sets none of them, but it is
// declared-and-defined-and-never-called dead code (audited 2026-08-17).
bool LiveSessionArmed() {
    static const bool s_offline_replay = []{
        const char* v = std::getenv("FM2K_REPLAY_FILE");
        return v && v[0] != '\0';
    }();
    if (g_spectator_mode) return !s_offline_replay;
    return !g_offline_mode && !g_stress_mode;
}

void DescribeMenu(uint8_t cfg, char* out, size_t out_sz) {
    std::snprintf(out, out_sz, "%s%s%s",
                  (cfg & kCfgStory) ? "STORY " : "",
                  (cfg & kCfgVs)    ? "VS "    : "",
                  (cfg & kCfgTeam)  ? "TEAM "  : "");
    if (out[0] == '\0') std::snprintf(out, out_sz, "<none> ");
}

void RefuseNoVsMode(uint8_t cfg) {
    g_verdict = TitleModeVerdict::NoVsMode;
    char modes[64];
    DescribeMenu(cfg, modes, sizeof(modes));

    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* stem = exe;
    for (const char* p = exe; *p; ++p) if (*p == '\\' || *p == '/') stem = p + 1;

    if (!LiveSessionArmed()) {
        // Offline / replay / stress. Note it ONCE and carry on -- this content
        // plays offline exactly as it always has.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[NOVSMODE] '%s' has NO VS mode (cfg=0x%02X menu=[ %s]) -- offline "
            "play is unaffected; netplay and spectating would be refused.",
            stem, (unsigned)cfg, modes);
        return;
    }

    if (!RefusalEnabled()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[NOVSMODE] KILL-SWITCH ARM: '%s' has NO VS mode (cfg=0x%02X "
            "menu=[ %s]) but FM2K_NO_VS_REFUSE=0 -- proceeding into a 1P/STORY "
            "netplay session. This WILL desync. Diagnostic only.",
            stem, (unsigned)cfg, modes);
        return;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[NOVSMODE] REFUSING SESSION: '%s' has NO VS mode. Its title menu "
        "enables [ %s] only (cfg=0x%02X), so g_game_mode_flag can never reach 1 "
        "(VS 1v1) on this content. In 1P/STORY the engine samples ONE pad per "
        "battle frame and picks the fighters from the story progression table, "
        "not from the two character indices our MATCH_START / SPEC_JOIN_ACK "
        "carry -- netplay and spectating are not merely broken here, they are "
        "meaningless. Story-only titles are a documented non-goal alongside "
        "team mode. Offline play and replays still work.",
        stem, modes, (unsigned)cfg);
    SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_NO_VS_MODE);
    // Same shape as the desync and hash-mismatch refusals: publish first so the
    // launcher can name the reason, then stop the process so it cannot proceed
    // into a silent mode-0 session.
    std::fflush(stdout);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 1);
}

}  // namespace

void TitleModeSelect_Tick(uint32_t game_mode, bool from_input_hook) {
    if constexpr (!FM2K::kIsFM2K) {
        (void)game_mode; (void)from_input_hook;
        return;
    } else {
        static const bool auto_skip = !EnvIsExactly("FM2K_AUTO_TITLE_SKIP", "0");

        // The historical pre-set, moved here VERBATIM from hooks_getinput.cpp:
        // same condition, same value, same one-shot latch, same log text, same
        // call site (from_input_hook). It must fire on BOTH host AND spectator
        // -- session_history records only returned input values, so a spectator
        // replaying the host's recorded auto-mash button-A pulses would navigate
        // from g_menu_selection=0 and land in a different scene tree.
        static bool s_cursor_set_global = false;
        if (from_input_hook && auto_skip && !s_cursor_set_global &&
            game_mode == 1000) {
            // FM2K_TITLE_FORCE_MODE_INDEX must win HERE too, or this one-shot
            // would stomp the lever's write and the causality arm would silently
            // run VS anyway. Default (no lever) is the historical `= 1`.
            const int pre = ForcedMenuIndex();
            *(uint32_t*)kAddrMenuSelection = (pre >= 0) ? (uint32_t)pre : 1u;
            s_cursor_set_global = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "TitleMenuCursor: pre-set g_menu_selection=%u (host or spectator)",
                (pre >= 0) ? (unsigned)pre : 1u);
        }

        if (g_verdict != TitleModeVerdict::Pending) return;
        // Floor: the game's own state machine is running, so the .kgt config
        // block has been read. Nothing below reads a half-loaded byte.
        if (game_mode < 1000) return;

        const uint8_t cfg = *(volatile uint8_t*)kAddrModeEnableBits;
        if ((cfg & kCfgAnyMode) == 0 && !ForceNoVs()) {
            // No mode bits at all: either not loaded yet, or a menu the engine
            // itself refuses to build (it bails back to substate 1). Wait.
            return;
        }

        if (ForceNoVs()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[NOVSMODE] TEST LEVER FM2K_TITLE_FORCE_NO_VS=1 -- treating "
                "this title as story-only regardless of cfg=0x%02X.",
                (unsigned)cfg);
            RefuseNoVsMode(cfg);
            return;
        }

        if ((cfg & kCfgVs) == 0) {
            RefuseNoVsMode(cfg);
            return;
        }

        // VS exists in the content. Find WHERE the engine put it. The list is
        // only populated once title_screen_manager's substate 4 has run; until
        // then it reads as BSS zeros and the scan simply finds nothing, so we
        // retry next frame. On FM2K_BOOT_TO_CSS_DIRECT the list is never built
        // and we stay Pending forever, which is correct: there is no menu
        // cursor to correct and no refusal to make.
        int32_t max_idx = *(volatile int32_t*)kAddrModeListMaxIdx;
        if (max_idx < 0 || max_idx >= kMaxListEntries) return;
        const int32_t* list = (const int32_t*)kAddrModeList;
        int32_t vs_index = -1;
        for (int32_t i = 0; i <= max_idx; ++i) {
            if (list[i] == kModeVs) { vs_index = i; break; }
        }
        if (vs_index < 0) return;   // list not built yet

        // TEST LEVER, checked BEFORE the verdict latches. It deliberately stays
        // Pending and re-applies on every title tick: two call sites plus the
        // one-shot pre-set write the cursor at different points in the frame, so
        // a latch-once lever could be stomped and the arm would quietly run VS.
        const int forced = ForcedMenuIndex();
        if (forced >= 0) {
            if (auto_skip && game_mode == 1000) {
                *(uint32_t*)kAddrMenuSelection = (uint32_t)forced;
            }
            static bool s_forced_logged = false;
            if (!s_forced_logged) {
                s_forced_logged = true;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "[TITLEMODE] TEST LEVER FM2K_TITLE_FORCE_MODE_INDEX=%d -- "
                    "overriding the resolved VS index %d. The session will run "
                    "g_game_mode_flag=%d, NOT VS 1v1. Diagnostic only.",
                    forced, (int)vs_index,
                    (forced <= max_idx) ? (int)list[forced] : 0);
            }
            return;
        }

        g_verdict = TitleModeVerdict::VsFound;

        const uint32_t cur = *(volatile uint32_t*)kAddrMenuSelection;
        if ((int32_t)cur == vs_index) {
            // THE ONLY PATH TAKEN ON EVERY VS-CAPABLE GAME IN THE LIBRARY.
            // The pre-set above already wrote 1 at the first game_mode==1000
            // tick and the VS entry IS at index 1 on every [STORY, VS] and
            // [STORY, VS, TEAM] title, so we store nothing: same values, same
            // frames, same number of writes as before this file existed.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[TITLEMODE] VS entry resolved at g_titleMenu_modeList[%d] "
                "(cfg=0x%02X, %d entr%s) -- g_menu_selection already correct, "
                "no write.",
                (int)vs_index, (unsigned)cfg, (int)max_idx + 1,
                max_idx == 0 ? "y" : "ies");
            return;
        }

        if (!auto_skip || game_mode != 1000) {
            // FM2K_AUTO_TITLE_SKIP=0: a human is driving this menu. Say what we
            // found and keep our hands off their cursor.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[TITLEMODE] VS entry is at g_titleMenu_modeList[%d] but "
                "g_menu_selection=%u -- NOT corrected (auto-title-skip off or "
                "past the title).", (int)vs_index, (unsigned)cur);
            return;
        }

        *(uint32_t*)kAddrMenuSelection = (uint32_t)vs_index;
        char modes[64];
        DescribeMenu(cfg, modes, sizeof(modes));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[TITLEMODE] CORRECTED g_menu_selection %u -> %d: this title's menu "
            "is [ %s] (cfg=0x%02X), so the VS entry is NOT at the hardcoded "
            "index 1. Without this the session would have latched "
            "g_game_mode_flag=%d.",
            (unsigned)cur, (int)vs_index, modes, (unsigned)cfg,
            ((int32_t)cur <= max_idx) ? (int)list[cur] : (int)kModeStory);
    }
}

TitleModeVerdict TitleModeSelect_Verdict() { return g_verdict; }

bool TitleModeSelect_HasNoVsMode() {
    return g_verdict == TitleModeVerdict::NoVsMode;
}
