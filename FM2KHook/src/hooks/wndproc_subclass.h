#pragma once

// Early subclass of the FM2K main game window's WndProc. Independent of
// the D3D9 / cnc-ddraw path so it works in pure DirectDraw mode too --
// imgui_overlay's later WndProc hook layers on top of this one via the
// chained CallWindowProc.
//
// What this hook fixes:
//
//   1) Alt-tap freeze. Default DefWindowProc on a no-menu Alt press
//      sends WM_SYSCOMMAND/SC_KEYMENU which enters a modal "menu
//      active" state that blocks input until you press Esc or a menu
//      key. We swallow SC_KEYMENU + the underlying VK_MENU/VK_F10
//      sysk events + WM_SYSCHAR (Alt+letter ding), so Alt becomes a
//      pure no-op.
//
//   2) Window-drag disconnect. DefWindowProc enters a modal loop on
//      WM_NCLBUTTONDOWN/HTCAPTION (title-drag) and on WM_INITMENU,
//      blocking DispatchMessage. The control_channel.cpp MM-timer
//      keepalive sends pings during this, but the *receive* side is
//      tied to the main loop's ControlChannel_Poll. We catch
//      WM_ENTERSIZEMOVE to install a fast WM_TIMER that fires inside
//      the modal loop and drives ControlChannel_Poll() -- so packets
//      get drained, GekkoNet stays alive, and the peer doesn't see
//      a long silence. WM_EXITSIZEMOVE tears the timer back down.
//
// Lookup is by class name "KGT2KGAME" (FM2K's RegisterClass name from
// IDA). Idempotent -- once installed for a given HWND, subsequent
// install attempts are no-ops.

#include <windows.h>

namespace FM2KWndProc {

// Try to find the FM2K main window and install our subclass. Cheap when
// already installed (early-out check). Call once per main-loop tick from
// the trampoline; first successful call installs, the rest are NOPs.
void TryInstall();

// Tear down the subclass. Safe to call even if never installed.
void Uninstall();

// The main game window this process owns, or nullptr before TryInstall has
// found it. Exposed so other hook-side code does not have to repeat the
// class-name EnumWindows walk (background_mode.cpp needs it for the deferred
// minimize).
HWND GetHwnd();

}  // namespace FM2KWndProc
