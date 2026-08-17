// title_mode_select.h -- resolve the TITLE-MENU VS entry instead of guessing
// index 1, and refuse a netplay/spectator session on story-only content.
//
// WHY THIS EXISTS (DragonPuppy, Phase 6 finding F5).
//
// title_screen_manager @0x4080A0 builds the title menu in substate 4 out of one
// config byte read from the game's .kgt, into a COMPACTED list:
//
//     dl = g_gameConfig_modeEnableBits (0x4438A4)
//     n  = 0
//     if (dl & 0x04) g_titleMenu_modeList[n++] = 0   ; 1P / STORY
//     if (dl & 0x08) g_titleMenu_modeList[n++] = 1   ; VS 1v1
//     if (dl & 0x10) g_titleMenu_modeList[n++] = 2   ; TEAM
//     g_titleMenu_maxIndex (0x424E60) = n - 1
//
// and in substate 5, on the confirm edge:
//
//     g_game_mode_flag (0x470058) = g_titleMenu_modeList[g_menu_selection]
//
// The list is COMPACTED, so index 1 is "the second ENABLED mode", not "VS".
// The hook used to hardcode g_menu_selection = 1 ("VS Player is always index
// 1"). On the 90 games in the tested library whose menu is [STORY, VS] or
// [STORY, VS, TEAM] that is correct. On the 8 story-only titles ([STORY]) the
// list has ONE entry, index 1 was never written, it reads 0 out of BSS, and
// all three planes boot into 1P/STORY netplay -- where the engine samples ONE
// pad per battle frame instead of two and picks the fighters out of the story
// progression table instead of g_p1/p2_selected_char_idx. Both are load-bearing
// assumptions of our netplay and spectator protocols, so the session desyncs
// from battle frame 0 and wedges after match 1.
//
// Diagnosis: /home/teo/specrel-2026-08-07/dragonpuppy_diagnosis.md
//
// WHAT THIS DOES
//
//   1. HARDENING (all content): once the engine has actually BUILT the list,
//      scan it for the VS value (1) and point g_menu_selection at the real
//      index. This also repairs the latent [VS] / [VS, TEAM] layouts where the
//      hardcoded 1 is wrong today (0 such games in the tested library, so this
//      is hygiene, not a repair).
//
//   2. REFUSAL (story-only content): if the content's own config byte says VS
//      is NOT among the enabled modes, netplay and spectating are meaningless
//      on this title (DragonPuppy additionally has ZERO VS-selectable
//      characters, so even forcing the flag yields an empty VS grid). Fail
//      LOUDLY at session setup: publish FM2K_MATCH_OUTCOME_NO_VS_MODE on the
//      existing shared-mem outcome channel and terminate, exactly the way the
//      #57 hash-mismatch and the desync refusals already do, so the launcher
//      surfaces a localized error and tears the session down cleanly.
//      OFFLINE IS UNTOUCHED -- story-only games play offline, replay offline
//      and pass the offline determinism sweep, and must keep doing so.
//
// BYTE-IDENTICAL ON THE VS PATH. The ONE write this file can make to
// g_menu_selection is guarded by `current != resolved_index`. On every
// [STORY, VS] and [STORY, VS, TEAM] title the resolved index IS 1, which is
// what the pre-set below already wrote at the first game_mode==1000 tick (the
// same one-shot hooks_getinput.cpp used to carry, moved here verbatim), so the
// guard is false and NO write happens -- same values, same frames, same number
// of stores. Everything else here is three reads of engine globals that
// (IDA xref, WonderfulWorld_ver_0946) are referenced by title_screen_manager
// and NOTHING ELSE in the binary.
#pragma once

#include <cstdint>

enum class TitleModeVerdict : int {
    Pending  = 0,   // menu list not built yet -- keep looking each frame
    VsFound  = 1,   // VS entry located; g_menu_selection points at it
    NoVsMode = 2,   // content has NO VS mode -- netplay/spectate refused
};

// Per-frame tick. Cheap and self-latching: three reads while the verdict is
// Pending, nothing at all after.
//
// Called from TWO places on purpose:
//   * the FM2K get_player_input detour (from_input_hook = TRUE), which is where
//     the menu cursor has always been written and where the correction belongs;
//   * CheckGameModeTransition (from_input_hook = FALSE), which the trampolines
//     drive EVERY outer-loop iteration even while the sim is not ticking.
//     Measured on DragonPuppy: a HOST parked in the pre-rendezvous HELLO loop
//     reaches game_mode 1000 and then runs NO input frames at all, so a refusal
//     hung off the input hook alone fired on the guest in 89ms and left the host
//     spinning for the full leg timeout. The tick is idempotent, so calling it
//     from both costs nothing.
//
// from_input_hook GATES THE g_menu_selection PRE-SET (the historical
// "TitleMenuCursor: pre-set g_menu_selection=1" one-shot, moved here verbatim so
// one TU owns the title cursor). It must keep firing from the input hook and
// ONLY from there: that is the frame it has always landed on, and moving it
// earlier would change when the cursor value appears. The detection and the
// refusal are unaffected by the flag -- they are what needs the second call
// site.
//
// FM2K_AUTO_TITLE_SKIP is read internally: detection runs either way (it is
// read-only), the writes only under auto-skip, because with the auto-skip off a
// human is driving the menu and we must not move their cursor.
void TitleModeSelect_Tick(uint32_t game_mode, bool from_input_hook);

TitleModeVerdict TitleModeSelect_Verdict();

// True once the content has been proven to have no VS entry. Sticky.
bool TitleModeSelect_HasNoVsMode();
