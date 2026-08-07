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
#include "../core/globals.h"           // original_get_player_input, g_player_index
#include "../netplay/savestate.h"      // CHAR_SLOT_BASE, CHAR_SLOT_SIZE
#include "../ui/input_binder.h"        // FM2KInputBinder::Sample_Win32 + Init/Load
#include <windows.h>
#include <cstdio>
#include <cstdlib>
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
            // Mask out START (0x400) on CSS -- pressing it on the
            // character-select screen returns the local game to title
            // and desyncs/wedges netplay. We can't safely strip it in
            // battle (it's pause) or on title (it's confirm), so the
            // mask is gated to mode 2000.
            const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
            if (game_mode == 2000) bound &= (uint16_t)~0x400u;
            return bound;
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
    if (game_mode == 2000) input &= (uint16_t)~0x400u;
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

    return input;
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
    if (game_mode == 2000) bound &= (uint16_t)~0x400u;
    return bound;
}
