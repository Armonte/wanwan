#ifndef IMGUI_OVERLAY_H
#define IMGUI_OVERLAY_H

#include <windows.h>
#include <d3d9.h>

// Initialize ImGui overlay system
bool InitializeImGuiOverlay();

// Shutdown ImGui overlay system
void ShutdownImGuiOverlay();

// Check if overlay is visible
bool IsOverlayVisible();

// Toggle overlay visibility
void ToggleOverlay();

// Check for overlay hotkey (F9)
void CheckOverlayHotkey();

// Window proc handler
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#endif // IMGUI_OVERLAY_H