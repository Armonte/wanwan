#include <windows.h>
#include "fm2k_hook.h"
#include "ipc.h"
#include "state_manager.h"
#include <SDL3/SDL.h>

// Global state
static HANDLE g_process = NULL;

// DLL entry point
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_process = GetCurrentProcess();
            DisableThreadLibraryCalls(module);
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

int FM2KHook_Init() {
    if (!g_process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle");
        return -1;
    }

    // Initialize SDL for logging
    if (SDL_Init(SDL_INIT_EVENTS) < 0) {
        return -1;
    }

    // Initialize hooks
    if (!FM2K::Hooks::Init(g_process)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize hooks");
        return -1;
    }

    SDL_Log("FM2K hooks initialized successfully");
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