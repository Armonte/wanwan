#include <windows.h>
#include "fm2k_hook.h"
#include "ipc.h"
#include "state_manager.h"
#include <SDL3/SDL.h>

// Global state
static HANDLE g_process = nullptr;

DWORD WINAPI InitializeHooksThread(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Initializing FM2KHook in background thread...");

    // Initialize SDL for logging
    if (SDL_Init(SDL_INIT_EVENTS) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize SDL: %s", SDL_GetError());
        return 1; // Indicate failure
    }

    // Initialize hooks
    if (!FM2K::Hooks::Init(g_process)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize hooks");
        SDL_Quit();
        return 1; // Indicate failure
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K hooks initialized successfully");

    // Open the event created by the launcher and signal it.
    HANDLE init_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"FM2KHook_Initialized");
    if (init_event) {
        if (!SetEvent(init_event)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Failed to signal init event. Error: %lu", GetLastError());
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Signaled initialization complete");
        }
        CloseHandle(init_event);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to open init event. Error: %lu", GetLastError());
    }

    return 0; // Success
}

// DLL entry point
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "FM2KHook.dll attached to process");
                g_process = GetCurrentProcess();
                DisableThreadLibraryCalls(module);

                // Create a thread to do the initialization. This is important to avoid
                // deadlocks inside DllMain.
                HANDLE hThread = CreateThread(nullptr, 0, InitializeHooksThread, nullptr, 0, nullptr);
                if (hThread) {
                    CloseHandle(hThread); // We don't need to manage the thread.
                } else {
                     SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to create initialization thread. Error: %lu", GetLastError());
                }
            }
            break;

        case DLL_PROCESS_DETACH:
            if (!reserved) {
                // Clean shutdown
                FM2K::Hooks::Shutdown();
            }
            break;
    }
    return TRUE;
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