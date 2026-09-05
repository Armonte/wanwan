#ifndef IMGUI_OVERLAY_H
#define IMGUI_OVERLAY_H

#include <windows.h>
#include <d3d9.h>

// Initialize ImGui overlay system
bool InitializeImGuiOverlay();

// There is deliberately NO ShutdownImGuiOverlay. The overlay's lifetime is the
// process's: the only unload point is DLL_PROCESS_DETACH, by which time
// ExitProcess has already killed the render thread that owns the D3D9 device,
// so releasing the backend's device objects from DllMain is a crash risk that
// buys nothing. The one part of teardown that does matter -- getting this DLL's
// WndProc out of the window -- is done by FM2KWndProc::Uninstall at detach,
// which restores the game's original proc over the whole chain.

// Toggle overlay visibility
void ToggleOverlay();

// Check for overlay hotkey (F9)
void CheckOverlayHotkey();

// Window proc handler
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#endif // IMGUI_OVERLAY_H