#pragma once

#include <windows.h>

namespace FM2K {
namespace WindowHooks {

// Window creation hook functions
HWND WINAPI Hook_CreateWindowExA(
    DWORD dwExStyle,
    LPCSTR lpClassName,
    LPCSTR lpWindowName,
    DWORD dwStyle,
    int X,
    int Y,
    int nWidth,
    int nHeight,
    HWND hWndParent,
    HMENU hMenu,
    HINSTANCE hInstance,
    LPVOID lpParam
);

// Initialize window hooks
bool InitializeWindowHooks();
void ShutdownWindowHooks();

} // namespace WindowHooks
} // namespace FM2K