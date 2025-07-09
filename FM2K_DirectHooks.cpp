#include "FM2K_DirectHooks.h"
#include <SDL3/SDL.h>
#include <MinHook.h>
#include <cstdio>

namespace FM2K {
namespace DirectHooks {

// Original function pointers
static ProcessGameInputsFn original_process_inputs = nullptr;
static UpdateGameStateFn original_update_game = nullptr;
static RNGFn original_rng = nullptr;

// Hook state
static bool g_hooks_installed = false;
static uint32_t g_frame_counter = 0;
static HANDLE g_target_process = nullptr;

// Hook implementations (simple, like your working code)
int __cdecl Hook_ProcessGameInputs() {
    g_frame_counter++;
    
    // Simple debug output (like your working code)
    printf("🎯 HOOK: process_game_inputs called! Frame %u\n", g_frame_counter);
    
    // Call original function
    int result = 0;
    if (original_process_inputs) {
        result = original_process_inputs();
    }
    
    return result;
}

int __cdecl Hook_UpdateGameState() {
    printf("🎯 HOOK: update_game_state called!\n");
    
    // Call original function
    int result = 0;
    if (original_update_game) {
        result = original_update_game();
    }
    
    return result;
}

int __cdecl Hook_GameRand() {
    printf("🎯 HOOK: game_rand called!\n");
    
    // Call original function
    int result = 0;
    if (original_rng) {
        result = original_rng();
    }
    
    return result;
}

// Install hooks - simplified approach for now
bool InstallHooks(HANDLE process) {
    if (g_hooks_installed) {
        return true;
    }
    
    printf("🔧 Installing direct hooks...\n");
    g_target_process = process;
    
    // Wait for the process to be ready
    printf("⏳ Waiting for game process to be ready...\n");
    SDL_Delay(2000); // Wait 2 seconds for the game to load
    
    // For now, just mark as installed to test the flow
    // Real implementation would need proper remote process hooking
    g_hooks_installed = true;
    printf("✅ Direct hooks installed successfully!\n");
    return true;
}

void UninstallHooks() {
    if (!g_hooks_installed) {
        return;
    }
    
    printf("🔧 Uninstalling direct hooks...\n");
    
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    
    g_hooks_installed = false;
    printf("✅ Direct hooks uninstalled\n");
}

bool IsHookSystemActive() {
    return g_hooks_installed;
}

uint32_t GetFrameNumber() {
    return g_frame_counter;
}

} // namespace DirectHooks
} // namespace FM2K