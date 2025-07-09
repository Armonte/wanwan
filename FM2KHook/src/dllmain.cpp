#include "fm2k_hook.h"
#include <SDL3/SDL.h>
#include <windows.h>

// Forward declaration for the initialization thread
DWORD WINAPI InitThread(LPVOID lpParam);

BOOL APIENTRY DllMain(HMODULE hModule,
                      DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
    {
        // CRITICAL: First thing - prove DllMain is called
        OutputDebugStringA("[FM2K HOOK] *** DllMain CALLED - DLL LOADING STARTED ***\n");
        
        // Disable thread library calls to prevent deadlocks
        DisableThreadLibraryCalls(hModule);
        
        OutputDebugStringA("[FM2K HOOK] Creating initialization thread...\n");
        // Create a new thread to run our initialization code.
        // This is crucial to avoid deadlocking the process loader.
        HANDLE hThread = CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
        if (hThread) {
            OutputDebugStringA("[FM2K HOOK] Initialization thread created successfully\n");
            // We don't need the handle, so close it to allow the thread to clean up
            CloseHandle(hThread);
        } else {
            OutputDebugStringA("[FM2K HOOK] CRITICAL: Failed to create initialization thread!\n");
            // Log an error if thread creation fails
            // Note: SDL logging might not be ready here, but we try anyway.
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create initialization thread!");
        }
        break;
    }
        
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        // On detach, perform cleanup
        if (ul_reason_for_call == DLL_PROCESS_DETACH) {
            FM2K::Hooks::Shutdown();
        }
        break;
    }
    return TRUE;
}

// This function runs in a separate thread to initialize the hooks
DWORD WINAPI InitThread(LPVOID lpParam) {
    // CRITICAL: Add fallback logging before anything else
    OutputDebugStringA("[FM2K HOOK] InitThread started\n");
    
    // Check if SDL is already initialized by the game to avoid conflicts.
    // If it's not running, we initialize the minimal systems we need.
    OutputDebugStringA("[FM2K HOOK] Checking SDL initialization status\n");
    if (SDL_WasInit(0) == 0) {
        OutputDebugStringA("[FM2K HOOK] SDL not initialized, calling SDL_Init(0)\n");
        if (SDL_Init(0) != 0) {
            // Cannot log this error, as logging itself failed.
            OutputDebugStringA("[FM2K HOOK] CRITICAL: SDL_Init(0) failed!\n");
            return 1;
        }
        OutputDebugStringA("[FM2K HOOK] SDL_Init(0) succeeded\n");
    } else {
        OutputDebugStringA("[FM2K HOOK] SDL already initialized by game\n");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DLL main thread started, initializing hooks...");
    OutputDebugStringA("[FM2K HOOK] About to call FM2K::Hooks::Init()\n");

    // Call the main Init function from fm2k_hook.cpp
    // We pass GetCurrentProcess() as the handle to the game process
    if (!FM2K::Hooks::Init(GetCurrentProcess())) {
        OutputDebugStringA("[FM2K HOOK] CRITICAL: FM2K::Hooks::Init() FAILED!\n");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K::Hooks::Init failed!");
        // On failure, we simply exit the thread. The launcher will time out,
        // which is the expected behavior for a failed injection.
        return 1;
    }

    OutputDebugStringA("[FM2K HOOK] FM2K::Hooks::Init() succeeded\n");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K::Hooks::Init succeeded.");

    // Signal the launcher that initialization is complete.
    OutputDebugStringA("[FM2K HOOK] Opening FM2KHook_Initialized event\n");
    HANDLE init_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"FM2KHook_Initialized");
    if (init_event) {
        OutputDebugStringA("[FM2K HOOK] Signaling FM2KHook_Initialized event\n");
        if (!SetEvent(init_event)) {
             OutputDebugStringA("[FM2K HOOK] CRITICAL: Failed to signal init event\n");
             SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Failed to signal init event. Error: %lu", GetLastError());
        } else {
             OutputDebugStringA("[FM2K HOOK] Successfully signaled initialization complete\n");
             SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Signaled initialization complete event.");
        }
        CloseHandle(init_event);
    } else {
        OutputDebugStringA("[FM2K HOOK] CRITICAL: Failed to open FM2KHook_Initialized event\n");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to open init event 'FM2KHook_Initialized'. Error: %lu", GetLastError());
    }

    // SDL_Quit(); // Optional: clean up SDL if it's only used here
    return 0;
}

// Public API implementation
extern "C" {

void FM2KHook_Shutdown() {
    FM2K::Hooks::Shutdown();
    SDL_Quit();
}

const char* FM2KHook_GetLastError() {
    return SDL_GetError();
}

} // extern "C" 