#include <windows.h>
#include <cstdio>
#include <MinHook.h>
#include "sdl3_hooks.h"
#include "ddraw_hooks.h"

// --- Globals ---
static FILE* g_console_stream = nullptr;
void LogMessage(const char* message);

// --- Function Pointers for Original Game Functions ---
    static HRESULT (WINAPI* original_directdraw_create)(void* lpGUID, void** lplpDD, void* pUnkOuter) = nullptr;
static HWND (WINAPI* original_create_window_ex_a)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) = nullptr;
static LONG (WINAPI* original_set_window_long_a)(HWND, int, LONG) = nullptr;
static BOOL (WINAPI* original_process_input_history)() = nullptr;

// --- Hook Implementations ---

HWND WINAPI Hook_CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam) {
    // Let the game create its window first.
    HWND gameHwnd = original_create_window_ex_a(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);

    if (gameHwnd && lpClassName && strcmp(lpClassName, "KGT2KGAME") == 0) {
        LogMessage("*** DETECTED MAIN GAME WINDOW - INITIATING DIRECT TAKEOVER ***");
        if (InitializeSDL3()) {
            if (CreateSDL3Context(gameHwnd)) {
                // After docking SDL, we must explicitly show the window,
                // as the game's own ShowWindow call might be missed or ignored.
                LogMessage("Forcing game window to show.");
                ShowWindow(gameHwnd, SW_SHOW);
                UpdateWindow(gameHwnd);
            }
        }
    }
    return gameHwnd;
}

HRESULT WINAPI Hook_DirectDrawCreate(void* lpGUID, void** lplpDD, void* pUnkOuter) {
    LogMessage("*** Hook_DirectDrawCreate called - intercepting DirectDraw creation ***");
    *lplpDD = GetFakeDirectDraw();
    return DD_OK;
}

LONG WINAPI Hook_SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong) {
    if (nIndex == GWLP_WNDPROC) {
        LogMessage("Hook_SetWindowLongA: Intercepted game's attempt to set a new window procedure.");
        
        // Store the game's intended window procedure so our hook can call it.
        SetOriginalWindowProc((WNDPROC)dwNewLong);
        
        // IMPORTANT: Do NOT call the original SetWindowLongA for GWLP_WNDPROC.
        // Doing so would overwrite our hook. We've already subclassed the window
        // in CreateSDL3Context, and our goal here is just to capture the game's
        // intended procedure. We can return the handle to our own hook,
        // which mimics the behavior of SetWindowLongA returning the previous WNDPROC.
        return (LONG)(LONG_PTR)InterceptedWindowProc;
    }
    
    // For any other nIndex value, pass the call through to the original function.
    return original_set_window_long_a(hWnd, nIndex, dwNewLong);
}

BOOL WINAPI Hook_ProcessInputHistory() {
    PollSDLEvents();
    BOOL result = original_process_input_history();
    RenderGame();
    return result;
}

// --- Initialization and Cleanup ---

void InitializeHooks() {
    if (MH_Initialize() != MH_OK) {
        LogMessage("MinHook failed to initialize.");
        return;
    }

    MH_CreateHook((LPVOID)&CreateWindowExA, (LPVOID)&Hook_CreateWindowExA, (void**)&original_create_window_ex_a);
    MH_CreateHook((LPVOID)&DirectDrawCreate, (LPVOID)&Hook_DirectDrawCreate, (void**)&original_directdraw_create);
    MH_CreateHook((LPVOID)&SetWindowLongA, (LPVOID)&Hook_SetWindowLongA, (void**)&original_set_window_long_a);
    
    // We hook the game's main loop (process_input_history at 0x4025A0) to drive our rendering and event polling.
    uintptr_t baseAddress = (uintptr_t)GetModuleHandle(NULL);
    void* processInputTarget = (void*)(baseAddress + 0x25A0); // 0x4025A0
    MH_CreateHook(processInputTarget, (LPVOID)&Hook_ProcessInputHistory, (void**)&original_process_input_history);

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        LogMessage("Failed to enable hooks.");
        return;
    }
    LogMessage("All hooks initialized successfully.");
}

void CleanupHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    CleanupDirectDrawHooks();
    CleanupSDL3();
    LogMessage("All hooks cleaned up.");
}

DWORD WINAPI MainThread(LPVOID hModule) {
    // Setup console for logging
    AllocConsole();
    freopen_s(&g_console_stream, "CONOUT$", "w", stdout);
    LogMessage("Hook DLL Attached. Initializing...");
    
    InitializeHooks();
    
    // Signal launcher that we are ready
    HANDLE init_event = CreateEventW(nullptr, TRUE, FALSE, L"FM2KHook_Initialized");
    if (init_event) {
        SetEvent(init_event);
        CloseHandle(init_event);
    }
    
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
            break;
        case DLL_PROCESS_DETACH:
            CleanupHooks();
            if (g_console_stream) {
                fclose(g_console_stream);
            }
            FreeConsole();
            break;
    }
    return TRUE;
}

void LogMessage(const char* message) {
    // Log to the console if it's available.
    if (g_console_stream) {
        fprintf(g_console_stream, "FM2K HOOK: %s\n", message);
        fflush(g_console_stream);
    }

    // Also write to a persistent log file.
    FILE* logFile = nullptr;
    if (fopen_s(&logFile, "C:\\games\\fm2k_hook_log.txt", "a") == 0 && logFile) {
        fprintf(logFile, "FM2K HOOK: %s\n", message);
        fclose(logFile);
    }
}