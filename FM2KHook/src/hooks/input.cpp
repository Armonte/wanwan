// Input capture -- taps FM2K's native input pipeline instead of polling
// keys with GetAsyncKeyState.
//
// Why: 2DFM has its own input binding system (per-player keyboard +
// joystick, configurable via the game's options dialog, persisted to
// config). Hardcoding a keymap in the hook forces users onto our layout
// and duplicates the game's polling code. Instead we call the game's
// own input reader directly through the MinHook trampoline
// (original_get_player_input) for player index 0 -- this returns
// whatever key / joystick mapping FM2K has configured for P1.
//
// Every instance uses the P1 mapping regardless of its netplay slot:
// the user only ever configures one profile. The netplay layer routes
// this local input into the correct remote character slot via
// Hook_GetPlayerInput.
#include "input.h"
#include "hooks.h"   // Hook_ApplySOCDSlot -- per-player SOCD at capture
#include "../core/globals.h"           // original_get_player_input, g_player_index
#include "../netplay/savestate.h"      // CHAR_SLOT_BASE, CHAR_SLOT_SIZE
#include "../ui/input_binder.h"        // FM2KInputBinder::Sample_Win32 + Init/Load
#include "../netplay/netplay.h"       // Netplay_IsConnected -- CSS START is offline-only
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <SDL3/SDL_log.h>
#include <cstring>
#include <string>
#include <sys/stat.h>

// Player index we're capturing for (set externally)
extern int g_player_index;

// Cache our game window handle
static HWND g_our_window = NULL;

// Find our game window by matching process ID and class name.
// FM2K registers "KGT2KGAME"; FM95/CPW registers "KGT95GAME" (same
// convention wndproc_subclass.cpp uses). The old hardcoded KGT2KGAME
// meant the FM95 build never found its window, so the focus check was
// permanently false there.
static BOOL CALLBACK FindOurWindowCallback(HWND hwnd, LPARAM lParam) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);

    if (pid == GetCurrentProcessId()) {
        char class_name[64];
        if (GetClassNameA(hwnd, class_name, sizeof(class_name))) {
            const char* expect_cls = FM2K::kIsFM95 ? "KGT95GAME" : "KGT2KGAME";
            if (strcmp(class_name, expect_cls) == 0) {
                *(HWND*)lParam = hwnd;
                return FALSE;  // Found it, stop enumeration
            }
        }
    }
    return TRUE;
}

static HWND GetOurWindow() {
    // Re-check periodically in case window was recreated
    if (g_our_window && IsWindow(g_our_window)) {
        return g_our_window;
    }

    g_our_window = NULL;
    EnumWindows(FindOurWindowCallback, (LPARAM)&g_our_window);
    return g_our_window;
}

static bool IsOurWindowFocused() {
    HWND our_window = GetOurWindow();
    if (!our_window) {
        return false;
    }

    HWND focused = GetForegroundWindow();
    return (focused == our_window);
}

// START on character select: allowed OFFLINE, masked ONLINE.
//
// The engine supports backing out of character select -- game_state_manager
// tests bit 10 of the input word and writes its state 4 (disassembled at
// 0x407267: `test $0x4,%ah` / `movl $0x4,0x152(%ecx)`), which returns the
// local game to the mode select. It has always been there; we masked it
// unconditionally because taking it ONLINE moves one peer's state machine
// and not the other's, which desyncs or wedges the session.
//
// Offline there is no peer to disagree with, and this is the single most
// requested local-play fix: today the only way to undo a mis-picked
// character is to enter the match, pause, quit, and re-select.
//
// ONLINE IT IS NOW ALLOWED THROUGH, 2026-09-08, because it no longer reaches
// the engine's back-out at all. CSS is lockstep on gekko-confirmed inputs
// (Netplay_CssHandleAdvance feeds them to process_game_inputs +
// update_game_state and flushes the same words to the spectator stream), so
// letting 0x400 into that word means BOTH peers and every viewer see the press
// on the same frame. ApplyCssDeselect then swallows the bit from
// g_system_input_state BEFORE game_state_manager runs, so the state-4 write at
// 0x40726F never happens; what the player gets is an UN-CONFIRM that stays in
// character select.
//
// The distinction that makes this safe is the one the old comment was reaching
// for: leaving CSS is a session transition and still needs a both-peers
// barrier, but un-confirming is ordinary synced sim state, no different from
// moving the cursor.
static inline bool CssStartAllowed() {
    return true;
}

// FM2K_TEST_CSS_DESELECT=1: synthesize ONE START press at character select,
// the first frame this client is confirmed.
//
// Applied to EVERY exit of Input_CaptureLocal, not just the binder branch. The
// first version lived inside `if (s_binder_active)` and never fired in the
// harness at all -- the gate's games have no binder profile, so that branch is
// dead there and the run passed while testing nothing. A test that cannot fail
// is worse than no test.
//
// Injected into the LOCAL capture on purpose: that is the word gekko syncs, so
// the press reaches the peer and every spectator through the ordinary
// confirmed-input stream, exactly as a real player's press would. Poking the
// deselect straight into sim state would be the one implementation that
// genuinely desyncs -- so the test must not do that either, or it proves
// nothing about the shipping path.
uint16_t Input_MaybeInjectCssDeselect(uint16_t bound) {
    if (*(uint32_t*)FM2K::ADDR_GAME_MODE != 2000u) return bound;
    static const bool s_want = []{
        const char* v = std::getenv("FM2K_TEST_CSS_DESELECT");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    if (!s_want) return bound;
    static bool s_fired = false;
    if (s_fired) return bound;
    const uint32_t* action = (const uint32_t*)0x0047019C;   // g_action_state
    const int me = (g_player_index == 1) ? 1 : 0;
    if (action[me] == 0) return bound;                      // only once confirmed
    s_fired = true;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "[TEST-DESELECT] injecting one START press at CSS (P%d was confirmed) "
        "-- rides the synced input word like a real press", me + 1);
    return (uint16_t)(bound | 0x400u);
}

uint16_t Input_CaptureLocal() {
    // CRITICAL: Only capture input if our window is focused
    // This prevents cross-instance input bleeding
    if (!IsOurWindowFocused()) {
        return 0;  // Not focused, no input
    }

    // If the user has set up bindings via the launcher's Input Bindings UI,
    // use those bindings as the local input source -- that's what makes
    // launcher binds actually drive gameplay. The path resolution lives in
    // the binder so launcher and hook agree on it (currently
    // %APPDATA%\FM2K_Rollback\fm2k_inputs.ini, with FM2K_INPUT_CONFIG_PATH
    // env var override). Falls back to FM2K's own get_player_input below
    // when no config file is present, so vanilla FM2K key bindings keep
    // working out of the box.
    //
    // Re-checked on every call (cheap stat) instead of cached-on-first-call,
    // because the user might Save bindings AFTER the game has started -- we
    // want the new binds to take effect without needing to relaunch.
    {
        static int  s_last_check_tick = 0;
        static bool s_binder_active   = false;
        const int now_tick = (int)GetTickCount();
        if ((now_tick - s_last_check_tick) > 1000 || s_last_check_tick == 0) {
            s_last_check_tick = now_tick;
            // Init() is idempotent and resolves the same DefaultConfigPath()
            // as the launcher. Calling it once attempts to Load(); if the
            // file doesn't exist we fall through to the original game input.
            // Also route to the host EXE's per-game profile so launcher-side
            // overrides (e.g. fm2k_inputs_WonderfulWorld_ver_0946.ini) reach
            // the in-game hook even though we're in a separate process.
            static bool s_profile_routed = false;
            if (!s_profile_routed) {
                s_profile_routed = true;
                char buf[MAX_PATH] = {};
                if (GetModuleFileNameA(nullptr, buf, sizeof(buf)) > 0) {
                    // Strip directory + .exe suffix to match the launcher's
                    // SetGameProfile(stem). std::filesystem isn't portable
                    // enough to assume in this hook path; do it by hand.
                    const char* slash = std::strrchr(buf, '\\');
                    if (!slash) slash = std::strrchr(buf, '/');
                    const char* base = slash ? slash + 1 : buf;
                    std::string stem = base;
                    auto dot = stem.find_last_of('.');
                    if (dot != std::string::npos) stem.resize(dot);
                    FM2KInputBinder::SetGameProfile(stem.c_str());
                }
            }
            FM2KInputBinder::Init();
            // Re-Load() every tick of the periodic check so launcher-
            // side binding changes (Save in the binder UI) propagate
            // into the running game without restart. Init() is one-
            // shot (gated on g_initialized) so it doesn't refresh
            // bindings on its own; Load() is the actual file-read.
            FM2KInputBinder::Load();
            // Hot-plug refresh -- Suicidal Muffin's bug report:
            // unplugging a controller mid-match or plugging one in
            // after the game started required a session restart. We
            // walk SDL_GetGamepads / SDL_GetJoysticks on the same
            // 1-second cadence as Load(), so newly-attached devices
            // start receiving binder input within a second of being
            // plugged in. Cost is one SDL_PumpEvents + a couple of
            // SDL_GetGamepads -- negligible compared to the input poll.
            FM2KInputBinder::RefreshGamepads();
            // Detect "config file present and successfully loaded" by
            // checking whether any binding has a non-NONE source. Defaults
            // also have non-NONE bindings (P1 keyboard arrows + Z/X/C/V/A/S/D/Enter)
            // so this effectively says "binder is initialized" -- which is
            // exactly when we want to use it.
            const auto& pb = FM2KInputBinder::Bindings(0);
            s_binder_active = false;
            for (const auto& b : pb.bits) {
                if (b.source != FM2KInputBinder::Binding::Source::NONE) {
                    s_binder_active = true;
                    break;
                }
            }
        }
        if (s_binder_active) {
            // Local player always reads slot 0 of the binder config (the
            // user only ever configures one profile; netplay routes it to
            // the right remote slot). Same convention this file already
            // uses for original_get_player_input(0, 0) below.
            uint16_t bound = FM2KInputBinder::Sample_Win32(0);
            // START on CSS is admitted (see CssStartAllowed): online it is
            // consumed by ApplyCssDeselect as an un-confirm and never reaches
            // the engine's state-4 teardown; offline-and-unconfirmed it still
            // falls through and bails to title, which is the behaviour local
            // players asked for.
            const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
            if (game_mode == 2000 && !CssStartAllowed())
                bound &= (uint16_t)~0x400u;
            // FN1/FN2 pick palettes 6 and 7, and in 2DFM the button you
            // CONFIRM with is the button that picks your colour -- so an FN
            // press has to confirm as well as select, or it does nothing.
            // The engine only counts a frame as a confirm when
            // `input_changes & 0x3F0` is non-zero, so ride bit 4 alongside.
            //
            // THIS is the copy that matters online. In netplay CSS the
            // engine consumes Netplay_GetCSSInput for BOTH players and the
            // local binder path is deliberately skipped, so the same rule
            // living only there worked offline and silently did nothing in
            // a real match. Applied here it goes into the value this peer
            // sends to gekko, so both sides see it and lockstep is intact.
            if (game_mode == 2000 && (bound & 0x3000u)) bound |= 0x0010u;
            // Per-player SOCD, applied here at capture with THIS device's
            // mode -- before the value reaches gekko, the replay or the
            // spectator stream. See Hook_ApplySOCDSlot.
            return Input_MaybeInjectCssDeselect(Hook_ApplySOCDSlot(bound, 0));
        }
    }

    // Ask the game directly for P1's input. original_get_player_input is
    // MinHook's trampoline to the real get_player_input @ 0x414340 -- calling
    // it reads the game's configured keyboard + joystick binding for player 0
    // (the P1 mapping) regardless of which netplay slot this instance occupies.
    //
    // CRITICAL: the game's input function APPLIES FACING SWAP internally
    // before returning (LEFT/RIGHT bits get flipped when the referenced
    // character is facing the opposite direction). But gekko's job is to
    // deliver RAW inputs, and Hook_GetPlayerInput re-applies the swap
    // during sim. If we forwarded the game's pre-swapped value to gekko,
    // sim's hook would swap it AGAIN -- net effect, directions reversed
    // whenever the character faces the non-default direction.
    //
    // Fix: detect the same swap condition Hook_GetPlayerInput uses (slot 0
    // active + state_flags bit 3 clear during battle mode) and UNDO the
    // game's swap. Gekko then receives the raw key press; sim applies the
    // swap exactly once; user's LEFT key maps to LEFT on-screen regardless
    // of which side they're standing on.
    if (!original_get_player_input) return 0;
    uint16_t input = (uint16_t)(original_get_player_input(0, 0) & 0x7FF);

    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    // CSS START guard -- mirror of the binder path above. Pressing
    // START on character select bails to title and breaks netplay.
    if (game_mode == 2000 && !CssStartAllowed()) input &= (uint16_t)~0x400u;
    if (game_mode >= 3000 && game_mode < 4000) {
        constexpr size_t CHAR_ACTIVE_FLAG_OFFSET = 0xDF41;
        constexpr size_t CHAR_STATE_FLAGS_OFFSET = 0x7CA6;
        // input_type=0 matches the first arg original_get_player_input got
        // above -- same slot determines whether the swap was applied.
        uintptr_t slot_base = CHAR_SLOT_BASE + 0 * CHAR_SLOT_SIZE;
        uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET);
        if (char_active != 0) {
            uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET);
            if ((char_flags & 8) == 0) {
                // Game's fn swapped bit 0 (left) <-> bit 1 (right). Reverse.
                uint16_t left  = (input & 0x001);
                uint16_t right = (input & 0x002);
                input = (input & ~0x003) | (left << 1) | (right >> 1);
            }
        }
    }

    // Vanilla-binding path is P1's device by definition: slot 0's mode.
    return Input_MaybeInjectCssDeselect(Hook_ApplySOCDSlot(input, 0));
}

uint16_t Input_CaptureLocalPlayer(int player) {
    // Same focus guard as Input_CaptureLocal -- no input when we're not the
    // foreground window (prevents cross-instance bleed).
    if (!IsOurWindowFocused()) {
        return 0;
    }
    // Only the binder path is meaningful per-player; the vanilla
    // get_player_input fallback is P1-only. So if this slot has no bound
    // keys, return 0. Bindings are assumed already loaded this frame by the
    // Input_CaptureLocal() call that runs just before us in the stress path.
    const auto& pb = FM2KInputBinder::Bindings(player);
    bool active = false;
    for (const auto& b : pb.bits) {
        if (b.source != FM2KInputBinder::Binding::Source::NONE) {
            active = true;
            break;
        }
    }
    if (!active) {
        return 0;
    }
    uint16_t bound = FM2KInputBinder::Sample_Win32(player);
    // Mask START on CSS, same as Input_CaptureLocal (pressing it on
    // character-select bails to title and wedges the session).
    const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (game_mode == 2000 && !CssStartAllowed()) bound &= (uint16_t)~0x400u;
    // Same FN-confirms rule as Input_CaptureLocal -- both capture paths feed
    // the same session, so they must agree or P2 picks a palette on one side
    // only.
    if (game_mode == 2000 && (bound & 0x3000u)) bound |= 0x0010u;
    return Hook_ApplySOCDSlot(bound, player);
}
