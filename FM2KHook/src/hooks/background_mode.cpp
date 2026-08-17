// FM2K_TEST_BACKGROUND layer 3 -- game-process user32 detours. See
// background_mode.h for the why, the shipping-safety argument and the layer map.
#include "background_mode.h"

#include "../core/globals.h"
#include "wndproc_subclass.h"   // GetHwnd() for the deferred minimize

#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdlib>
#include <cstring>

#include "MinHook.h"

namespace FM2KBackground {
namespace {

// ---------------------------------------------------------------------------
// Trampolines
// ---------------------------------------------------------------------------
using ShowWindow_t          = BOOL (WINAPI*)(HWND, int);
using SetWindowPos_t        = BOOL (WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using SetForegroundWindow_t = BOOL (WINAPI*)(HWND);
using BringWindowToTop_t    = BOOL (WINAPI*)(HWND);
using SetActiveWindow_t     = HWND (WINAPI*)(HWND);
using ClipCursor_t          = BOOL (WINAPI*)(const RECT*);

ShowWindow_t          p_ShowWindow          = nullptr;
SetWindowPos_t        p_SetWindowPos        = nullptr;
SetForegroundWindow_t p_SetForegroundWindow = nullptr;
BringWindowToTop_t    p_BringWindowToTop    = nullptr;
SetActiveWindow_t     p_SetActiveWindow     = nullptr;
ClipCursor_t          p_ClipCursor          = nullptr;

// ---------------------------------------------------------------------------
// Effective mode
// ---------------------------------------------------------------------------
// MINIMIZE is only safe while the ENGINE is in fullscreen mode. Traced in the
// IDB (WonderfulWorld_ver_0946): main_window_proc @0x405F50 handles WM_SIZE
// (msg 5) as
//     if (g_graphics_mode) return 0;                   // early out
//     g_window_size_x = LOWORD(lParam);                // else: adopt the size
//     g_window_size_y = HIWORD(lParam);
// (g_graphics_mode @0x424704 is the one flag that means BOTH "fullscreen /
// DirectDraw" and "RGB565 rather than RGB555" -- see render_simd.cpp, which
// already documents the render half of it.)
// and minimizing a window delivers WM_SIZE(SIZE_MINIMIZED) with cx=cy=0. In
// WINDOWED mode that would write 0/0 into g_window_size_{x,y} @0x447F20/0x447F24
// -- the same pair config_file_writer @0x414CA0 persists back to game.ini as
// GameWindowSize_x/y, i.e. a minimize would corrupt the user's install on the
// way out. So: probe the engine's own GameScreenMode value and DEGRADE to
// NOACTIVATE when the engine is windowed, rather than assume.
//
// g_cfg_game_screen_mode @0x4D1D60 is the raw game.ini "GameScreenMode" value,
// read by hit_judge_set_function @0x414930 -- which InitializeMainWindow calls
// BEFORE it creates the window, so the value is valid by the time the first
// ShowWindow lands. (We read the CONFIG source rather than g_graphics_mode
// @0x424704 because InitializeMainWindow only copies into the latter AFTER the
// ShowWindow call.) It is FM2K-specific; the FM95 build has no traced
// equivalent, so FM95 degrades to NOACTIVATE too. The launcher's
// ForceFullscreenForLaunch pins fullscreen whenever the cnc-ddraw redirect is
// active (the default), so the degrade is the exception, not the rule -- but it
// is measured per run, and logged when it fires.
//
// SECOND, INDEPENDENT REASON MINIMIZE IS SAFE (both verified, neither assumed):
// main_window_proc's WM_ACTIVATEAPP(0x1C) handler PostQuitMessage(0)s when the
// app is DEACTIVATED, but only if g_debug_mode @0x424744 AND
// g_cfg_testplay_exit_on_deactivate @0x430118 ("Editor.TestPlay.exit") are both
// nonzero. That ini key is absent from every game.ini we ship or test against
// (GetPrivateProfileIntA nDefault = 0), and per_game_patches_battle.cpp already
// zeroes g_debug_mode after the /F boot-to-battle switch. Two independent zeros:
// a minimized instance cannot quit itself.
constexpr uintptr_t kAddrGameScreenMode = 0x4D1D60;

Mode s_effective = Mode::OFF;   // resolved lazily on the first detoured call
bool s_resolved  = false;

Mode EffectiveMode() {
    if (s_resolved) return s_effective;
    const Mode requested = GetMode();
    s_effective = requested;
    if (requested == Mode::MINIMIZE) {
        bool engine_fullscreen = false;
        if constexpr (FM2K::kIsFM2K) {
            engine_fullscreen = (*reinterpret_cast<const uint32_t*>(kAddrGameScreenMode) != 0);
        }
        if (!engine_fullscreen) {
            s_effective = Mode::NOACTIVATE;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[BGMODE] MINIMIZE degraded to NOACTIVATE: engine is in WINDOWED "
                "mode (GameScreenMode=0%s), where main_window_proc's WM_SIZE "
                "handler would adopt the minimized 0x0 client size into "
                "GameWindowSize_x/y and config_file_writer would persist it to "
                "game.ini. The window stays visible but is never activated.",
                FM2K::kIsFM2K ? "" : " -- FM95 build, no traced equivalent");
        }
    }
    s_resolved = true;
    return s_effective;
}

// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------
// nCmdShow values that make a window visible AND activate it. Everything else
// (SW_HIDE, SW_MINIMIZE, the already-no-activate spellings) passes through
// untouched -- we only ever weaken an activation, never introduce one.
bool IsActivatingShow(int cmd) {
    switch (cmd) {
        case SW_SHOWNORMAL:      // 1
        case SW_SHOWMAXIMIZED:   // 3
        case SW_SHOW:            // 5
        case SW_RESTORE:         // 9
        case SW_SHOWDEFAULT:     // 10
            return true;
        default:
            return false;
    }
}

// Set once the deferred minimize has run, i.e. once the renderer is proven up.
// Before that point the ORDERING CONTRACT (background_mode.h) forbids
// minimizing; after it, minimizing is exactly what we want.
bool s_renderer_up = false;

// BEFORE the renderer is up: never minimizes -- an activating show becomes
// SW_SHOWNOACTIVATE, window stays normal-size and visible, pushed to the back.
// AFTER it: an activating show becomes SW_SHOWMINNOACTIVE.
//
// That second case is not hypothetical. Measured in the run-8 gate poll: all
// three game processes were correctly iconic from ~00:53:15, then came BACK
// visible at ~00:54:01-05 (as they crossed CSS -> BATTLE) and stayed visible
// ~14-19s, during which 13 foreground samples landed on them. cnc-ddraw
// re-shows the window when it reconfigures presentation, and mapping that to
// SW_SHOWNOACTIVATE RESTORES a minimized window. A one-shot demotion therefore
// silently decays over a long run.
BOOL WINAPI Hook_ShowWindow(HWND hwnd, int nCmdShow) {
    if (GetMode() != Mode::OFF && IsActivatingShow(nCmdShow)) {
        const int replacement = (s_renderer_up && EffectiveMode() == Mode::MINIMIZE)
                                ? SW_SHOWMINNOACTIVE   // 7
                                : SW_SHOWNOACTIVATE;   // 4
        const BOOL r = p_ShowWindow(hwnd, replacement);
        p_SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return r;
    }
    return p_ShowWindow(hwnd, nCmdShow);
}

// NOTE for the four below: only ShowWindow needs EffectiveMode() (minimize vs
// noactivate is a ShowWindow-only distinction). These four behave identically in
// both modes, so they ask the cheaper GetMode() -- which also keeps the
// engine-state probe inside EffectiveMode() from being forced to resolve before
// hit_judge_set_function @0x414930 has populated GameScreenMode.
BOOL WINAPI Hook_SetWindowPos(HWND hwnd, HWND insert_after, int x, int y,
                              int cx, int cy, UINT flags) {
    if (GetMode() != Mode::OFF) {
        flags |= SWP_NOACTIVATE;
        // Only rewrite the z-order when the caller actually asked for one
        // (SWP_NOZORDER means insert_after is ignored) and only when it asked
        // to come FORWARD. A caller placing itself behind a specific window is
        // left alone -- cnc-ddraw does its own child-window stacking.
        if ((flags & SWP_NOZORDER) == 0 &&
            (insert_after == HWND_TOP || insert_after == HWND_TOPMOST ||
             insert_after == HWND_NOTOPMOST)) {
            insert_after = HWND_BOTTOM;
        }
    }
    return p_SetWindowPos(hwnd, insert_after, x, y, cx, cy, flags);
}

BOOL WINAPI Hook_SetForegroundWindow(HWND hwnd) {
    if (GetMode() != Mode::OFF) return TRUE;   // claim success, do nothing
    return p_SetForegroundWindow(hwnd);
}

BOOL WINAPI Hook_BringWindowToTop(HWND hwnd) {
    if (GetMode() != Mode::OFF) return TRUE;
    return p_BringWindowToTop(hwnd);
}

HWND WINAPI Hook_SetActiveWindow(HWND hwnd) {
    if (GetMode() != Mode::OFF) return GetActiveWindow();
    return p_SetActiveWindow(hwnd);
}

// cnc-ddraw imports ClipCursor and confines the pointer to its client area.
// A backgrounded test instance stealing the owner's mouse is the same class of
// annoyance as stealing the foreground, so unclip under the flag.
BOOL WINAPI Hook_ClipCursor(const RECT* rc) {
    if (GetMode() != Mode::OFF) return p_ClipCursor(nullptr);
    return p_ClipCursor(rc);
}

template <typename F>
bool Detour(const char* symbol, F detour, F* trampoline) {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) return false;
    void* target = reinterpret_cast<void*>(GetProcAddress(user32, symbol));
    if (!target) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[BGMODE] user32!%s not found -- skipped", symbol);
        return false;
    }
    void* tramp = nullptr;
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(detour), &tramp);
    if (s != MH_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[BGMODE] MH_CreateHook(%s) failed: %d", symbol, (int)s);
        return false;
    }
    s = MH_QueueEnableHook(target);
    if (s != MH_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[BGMODE] MH_QueueEnableHook(%s) failed: %d", symbol, (int)s);
        return false;
    }
    *trampoline = reinterpret_cast<F>(tramp);
    return true;
}

}  // namespace

Mode GetMode() {
    static int cached = -1;
    if (cached < 0) {
        cached = (int)Mode::OFF;
        const char* v = std::getenv("FM2K_TEST_BACKGROUND");
        if (v && v[0] != '\0') {
            if (std::strcmp(v, "0") == 0) {
                cached = (int)Mode::OFF;
            } else if (std::strcmp(v, "1") == 0 || std::strcmp(v, "minimize") == 0) {
                cached = (int)Mode::MINIMIZE;
            } else if (std::strcmp(v, "2") == 0 || std::strcmp(v, "noactivate") == 0) {
                cached = (int)Mode::NOACTIVATE;
            } else {
                // STRICT: an unrecognised spelling is OFF, and says so. Silently
                // treating it as ON would hide a harness typo behind a run the
                // owner can no longer see.
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[BGMODE] FM2K_TEST_BACKGROUND='%s' is not a recognised value "
                    "(0 | 1 | minimize | 2 | noactivate) -- background mode OFF", v);
            }
        }
    }
    return (Mode)cached;
}

void OnFrameRendered() {
    // Ordered cheapest-first: the common case is "flag not set", which costs one
    // cached int compare per rendered frame.
    static bool s_disabled = false;
    if (s_disabled) return;
    if (GetMode() != Mode::MINIMIZE) { s_disabled = true; return; }
    if (EffectiveMode() != Mode::MINIMIZE) { s_disabled = true; return; }  // windowed degrade

    // RE-ASSERT, not one-shot. The ShowWindow detour above now minimizes once
    // s_renderer_up, which covers restores that come through ShowWindow -- but
    // a window can also be un-minimized by SetWindowPos(SWP_SHOWWINDOW), by
    // WM_SYSCOMMAND/SC_RESTORE, or by the shell. This is the backstop: a cheap
    // IsIconic poll (~1 per 300 rendered frames, i.e. ~3s) that re-minimizes if
    // anything put the window back. Costs one syscall per 300 frames, and does
    // nothing at all when the window is already where we want it.
    if (s_renderer_up) {
        static unsigned s_poll = 0;
        if (++s_poll < 300) return;
        s_poll = 0;
        HWND w = FM2KWndProc::GetHwnd();
        if (!w || IsIconic(w)) return;
        if (p_ShowWindow)   p_ShowWindow(w, SW_SHOWMINNOACTIVE);
        if (p_SetWindowPos) p_SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0,
                                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        static unsigned s_reasserts = 0;
        if (++s_reasserts <= 5) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[BGMODE] re-minimized (#%u): something restored the window after "
                "the initial demotion", s_reasserts);
        }
        return;
    }

    // Require TWO completed renders, not one: the first return proves the
    // renderer produced a frame, the second proves it survived to produce
    // another. Still a frame-count on observed renders, not a wall-clock wait --
    // a stalled renderer never reaches this at all, which is the correct
    // failure mode (we simply stay un-minimized).
    static unsigned s_renders = 0;
    if (++s_renders < 2) return;

    HWND hwnd = FM2KWndProc::GetHwnd();
    if (!hwnd) return;   // subclass has not found the window yet; retry next frame

    // Call through the trampolines so our own ShowWindow detour cannot re-map
    // this (it would not -- SW_SHOWMINNOACTIVE is not an activating show -- but
    // depending on that is how a later edit to IsActivatingShow silently breaks
    // the deferred minimize).
    if (p_ShowWindow)   p_ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    if (p_SetWindowPos) p_SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    s_renderer_up = true;   // from here on the ShowWindow detour may minimize
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[BGMODE] minimized after %u completed renders (hwnd=%p) -- deferred so "
        "cnc-ddraw's device was created against a normal-size visible window",
        s_renders, (void*)hwnd);
}

void Install() {
    static bool installed = false;
    if (installed) return;
    const Mode requested = GetMode();
    if (requested == Mode::OFF) return;   // shipping path: nothing is patched
    installed = true;

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[BGMODE] MH_Initialize failed: %d -- background mode INERT",
                    (int)init);
        return;
    }

    int n = 0;
    n += Detour("ShowWindow",          &Hook_ShowWindow,          &p_ShowWindow)          ? 1 : 0;
    n += Detour("SetWindowPos",        &Hook_SetWindowPos,        &p_SetWindowPos)        ? 1 : 0;
    n += Detour("SetForegroundWindow", &Hook_SetForegroundWindow, &p_SetForegroundWindow) ? 1 : 0;
    n += Detour("BringWindowToTop",    &Hook_BringWindowToTop,    &p_BringWindowToTop)    ? 1 : 0;
    n += Detour("SetActiveWindow",     &Hook_SetActiveWindow,     &p_SetActiveWindow)     ? 1 : 0;
    n += Detour("ClipCursor",          &Hook_ClipCursor,          &p_ClipCursor)          ? 1 : 0;

    MH_STATUS apply = MH_ApplyQueued();
    if (apply != MH_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[BGMODE] MH_ApplyQueued failed: %d", (int)apply);
        return;
    }
    // Contract line the harnesses grep for: proves the mode actually armed in
    // THIS process rather than being assumed from the env var being exported.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[BGMODE] ARMED mode=%s detours=%d/6 -- windows will not take the foreground",
        requested == Mode::MINIMIZE ? "minimize" : "noactivate", n);
}

}  // namespace FM2KBackground
