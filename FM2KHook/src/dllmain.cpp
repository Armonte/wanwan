#include <windows.h>
#include "fm2k_hook.h"
#include "ipc.h"
#include "state_manager.h"
#include <SDL3/SDL.h>

// Global state
static HANDLE g_process = NULL;
static HANDLE g_init_event = NULL;

// DLL entry point
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "FM2KHook.dll attached to process");
            g_process = GetCurrentProcess();
            DisableThreadLibraryCalls(module);
            
            // Open the initialization event
            g_init_event = CreateEventW(nullptr, TRUE, FALSE, L"FM2KHook_Initialized");
            if (!g_init_event) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                    "Failed to create init event. Error: %lu", GetLastError());
            }
            break;
            
        case DLL_PROCESS_DETACH:
            if (!reserved) {
                // Clean shutdown
                FM2K::Hooks::Shutdown();
                
                if (g_init_event) {
                    CloseHandle(g_init_event);
                    g_init_event = NULL;
                }
            }
            break;
    }
    return TRUE;
}

// Public API implementation
extern "C" {

int FM2KHook_Init() {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Initializing FM2KHook...");
    
    if (!g_process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle");
        return -1;
    }

    // Initialize SDL for logging
    if (SDL_Init(SDL_INIT_EVENTS) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize SDL: %s", SDL_GetError());
        return -1;
    }

    // Initialize hooks
    if (!FM2K::Hooks::Init(g_process)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize hooks");
        SDL_Quit();
        return -1;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K hooks initialized successfully");
    
    // Signal that initialization is complete
    if (g_init_event) {
        if (!SetEvent(g_init_event)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                "Failed to signal init event. Error: %lu", GetLastError());
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Signaled initialization complete");
        }
    }
    
    return 0;
}

void FM2KHook_Shutdown() {
    FM2K::Hooks::Shutdown();
    SDL_Quit();
}

const char* FM2KHook_GetLastError() {
    return SDL_GetError();
}

} // extern "C" 