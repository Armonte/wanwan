#include "FM2K_DirectHooks.h"
#include "FM2K_Integration.h"
#include <SDL3/SDL.h>
#include <MinHook.h>
#include <cstdio>
#include <windows.h>

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

// Key FM2K addresses (from IDA analysis)
static constexpr uintptr_t FRAME_COUNTER_ADDR = 0x447EE0;
static constexpr uintptr_t PROCESS_INPUTS_ADDR = 0x4146D0;
static constexpr uintptr_t UPDATE_GAME_ADDR = 0x404CD0;

// Hook implementations (capture real game state like working code)
int __cdecl Hook_ProcessGameInputs() {
    // Read the actual frame counter from game memory
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint32_t game_frame = frame_ptr ? *frame_ptr : 0;
    
    g_frame_counter++;
    
    // Log both our counter and the game's frame counter
    printf("🎯 HOOK: process_game_inputs called! Hook frame %u, Game frame %u\n", 
           g_frame_counter, game_frame);
    
    // TODO: Read input state from game memory and forward to GekkoNet
    
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

// Install hooks using the working pattern from simple_input_hooks.cpp
bool InstallHooks(HANDLE process) {
    if (g_hooks_installed) {
        return true;
    }
    
    printf("🔧 Installing direct hooks...\n");
    g_target_process = process;
    
    // Initialize MinHook
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK) {
        printf("❌ MH_Initialize failed: %d\n", mh_init);
        return false;
    }
    
    // Get the game module handle (in-process hooking like working examples)
    HMODULE gameModule = GetModuleHandle(NULL);
    if (!gameModule) {
        printf("❌ Failed to get game module handle\n");
        MH_Uninitialize();
        return false;
    }
    
    uintptr_t baseAddr = (uintptr_t)gameModule;
    printf("🎯 Game base address: 0x%08X\n", baseAddr);
    
    // Install hook for process_game_inputs function
    void* inputFuncAddr = (void*)(PROCESS_INPUTS_ADDR);
    MH_STATUS status1 = MH_CreateHook(inputFuncAddr, (void*)Hook_ProcessGameInputs, (void**)&original_process_inputs);
    if (status1 != MH_OK) {
        printf("❌ Failed to create input hook: %d\n", status1);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable1 = MH_EnableHook(inputFuncAddr);
    if (enable1 != MH_OK) {
        printf("❌ Failed to enable input hook: %d\n", enable1);
        MH_Uninitialize();
        return false;
    }
    
    g_hooks_installed = true;
    printf("✅ Direct hooks installed successfully!\n");
    printf("   - Input processing hook at 0x%08X\n", PROCESS_INPUTS_ADDR);
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