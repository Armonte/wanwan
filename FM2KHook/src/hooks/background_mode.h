// FM2K_TEST_BACKGROUND -- harness-only "do not steal my foreground" mode.
//
// WHY THIS EXISTS: every gate stage (determinism, spec_selftest, the seam gate,
// the hub E2E, the Phase 6 sweep) pops 2-5 launcher windows and 2-5 game windows
// that grab the foreground while the owner is doing something else. A run is
// ~2-70 minutes; the owner cannot use the machine during it. This mode makes the
// spawned windows come up minimized-without-activation and stay there.
//
// SHIPPING SAFETY: the switch is OFF unless FM2K_TEST_BACKGROUND is set to an
// exact recognised value. A real user launching normally can never end up with
// a hidden window -- an unset var, an empty var, "0", or ANY unrecognised
// spelling all mean OFF (and an unrecognised spelling logs loudly rather than
// being silently treated as ON, so a typo in a harness cannot quietly disable
// the owner's ability to see a run either).
//
// THE THREE LAYERS (this file is layer 3, the game process):
//   1. harness  -- tools/*.py|sh set FM2K_TEST_BACKGROUND=1 by default.
//   2. launcher -- SDL window created not-focusable and minimized only AFTER
//                  its renderer exists (launcher/core/launcher_init.cpp), and
//                  the game process spawned with STARTF_USESHOWWINDOW /
//                  SW_SHOWNOACTIVATE (launcher/core/FM2K_GameInstance.cpp).
//   3. game     -- this file: MinHook detours on the user32 entry points that
//                  activate a window. The STARTUPINFO route alone is NOT
//                  enough, because it only governs the process's FIRST
//                  ShowWindow call and cnc-ddraw (2DFMD.dll, loaded in the game
//                  process on the default redirect path) imports
//                  SetForegroundWindow / SetFocus / ShowWindow / SetWindowPos /
//                  MoveWindow / ClipCursor and re-activates the window after
//                  the engine is done with it. Detouring the user32 export
//                  itself covers every module in the process, including
//                  cnc-ddraw, without touching cnc-ddraw's own IAT.
//
// The parse + install run in DllMain (pre-ResumeThread), so the detours are
// live before the engine reaches InitializeMainWindow @0x4056C0 and its single
// hardcoded ShowWindow(hwnd, SW_SHOW).
#pragma once

namespace FM2KBackground {

enum class Mode {
    OFF = 0,        // shipping default
    MINIMIZE,       // SW_SHOWMINNOACTIVE: minimized, never activated
    NOACTIVATE,     // visible at normal size, never activated, bottom of z-order
};

// Strict parse of FM2K_TEST_BACKGROUND. Read once, logged once.
//   unset / "" / "0"  -> OFF
//   "1" / "minimize"  -> MINIMIZE
//   "noactivate" / "2"-> NOACTIVATE
//   anything else     -> OFF + a loud warning naming the value
Mode GetMode();

// Installs the user32 detours if GetMode() != OFF. Idempotent; safe to call
// when MinHook is already initialized (locale_spoof gets there first on the
// normal path). No-op in OFF mode -- zero cost and zero risk when shipping.
void Install();

// ORDERING CONTRACT -- read before changing when the minimize happens.
//
// The detours NEVER minimize. They only ever de-activate (SW_SHOWNOACTIVATE +
// bottom of z-order), which leaves the window normal-size and visible for the
// entire boot. That is deliberate: cnc-ddraw creates its Direct3D9 device
// against this window, and a hidden/minimized window with a zero-size client
// area can fail device creation, dropping cnc-ddraw into its software-blit
// fallback -- observable as "-WARNING- Using slow software rendering, please
// update your graphics card driver" in the title bar. Software blitting
// distorts frame pacing (it does not break sim determinism, but a green taken
// on a software-rendered run is not the green we wanted).
//
// So MINIMIZE mode defers the actual minimize to OnFrameRendered(), which the
// trampoline calls immediately after original_render_game() returns in
// RenderFrameWithSnapshot. A returned render is the observable marker that the
// renderer context exists and has presented -- not a timer, not a sleep.
// Cheap early-out on every call but the one that performs the demotion.
void OnFrameRendered();

}  // namespace FM2KBackground
