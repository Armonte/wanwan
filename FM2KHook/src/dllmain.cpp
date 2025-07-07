#include <windows.h>
#include <MinHook.h>
#include "fm2k_hook.h"
#include "ipc.h"

// Last error message buffer
static char g_last_error[256] = "";

// Original function pointers (trampolines)
static void (__stdcall *original_process_inputs)() = nullptr;
static void (__stdcall *original_update_game)() = nullptr;
static int  (__stdcall *original_rand_func)() = nullptr;
static void (__stdcall *original_hit_judge_set)() = nullptr;
static void (__stdcall *original_sprite_effect)() = nullptr;

// Hook function implementations
static void __stdcall Hook_ProcessGameInputs() {
    // Call original first
    original_process_inputs();
    
    // Notify launcher that frame advanced
    FM2K::IPC::PostEvent(FM2K::IPC::EventType::FRAME_ADVANCED);
}

static void __stdcall Hook_UpdateGameState() {
    original_update_game();
    // TODO: Add state tracking
}

static int __stdcall Hook_GameRand() {
    return original_rand_func();
    // TODO: Add deterministic RNG
}

// New: Hook for hit judge table initialization
static void __stdcall Hook_HitJudgeSet() {
    // Call original first to let it set up tables
    original_hit_judge_set();
    
    // Notify launcher that hit tables were initialized
    FM2K::IPC::PostEvent(FM2K::IPC::EventType::HIT_TABLES_INIT);
}

// New: Hook for sprite effect system
static void __stdcall Hook_SpriteEffect() {
    original_sprite_effect();
    // We'll track visual state changes if needed
}

// Initialize MinHook and install our hooks
static bool InstallHooks() {
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        strcpy_s(g_last_error, "MH_Initialize failed");
        return false;
    }
    
    // Create hooks
    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(0x4146D0),
            reinterpret_cast<LPVOID>(Hook_ProcessGameInputs),
            reinterpret_cast<LPVOID*>(&original_process_inputs)
        ) != MH_OK) {
        strcpy_s(g_last_error, "Failed to create process_inputs hook");
        return false;
    }
    
    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(0x404CD0),
            reinterpret_cast<LPVOID>(Hook_UpdateGameState),
            reinterpret_cast<LPVOID*>(&original_update_game)
        ) != MH_OK) {
        strcpy_s(g_last_error, "Failed to create update_game hook");
        return false;
    }
    
    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(0x417A22),
            reinterpret_cast<LPVOID>(Hook_GameRand),
            reinterpret_cast<LPVOID*>(&original_rand_func)
        ) != MH_OK) {
        strcpy_s(g_last_error, "Failed to create rand hook");
        return false;
    }
    
    // New: Hook hit judge initialization
    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(0x414C90),
            reinterpret_cast<LPVOID>(Hook_HitJudgeSet),
            reinterpret_cast<LPVOID*>(&original_hit_judge_set)
        ) != MH_OK) {
        strcpy_s(g_last_error, "Failed to create hit_judge_set hook");
        return false;
    }
    
    // New: Hook sprite effect system
    if (MH_CreateHook(
            reinterpret_cast<LPVOID>(0x40CC30),
            reinterpret_cast<LPVOID>(Hook_SpriteEffect),
            reinterpret_cast<LPVOID*>(&original_sprite_effect)
        ) != MH_OK) {
        strcpy_s(g_last_error, "Failed to create sprite_effect hook");
        return false;
    }
    
    // Enable all hooks
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        strcpy_s(g_last_error, "Failed to enable hooks");
        return false;
    }
    
    return true;
}

// DLL entry point
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            break;
            
        case DLL_PROCESS_DETACH:
            FM2KHook_Shutdown();
            break;
    }
    return TRUE;
}

// Exported functions
extern "C" {

FM2KHOOK_API int FM2KHook_Init() {
    // Initialize IPC first
    if (!FM2K::IPC::Init()) {
        strcpy_s(g_last_error, "Failed to initialize IPC");
        return FM2KHOOK_ERROR_IPC_INIT;
    }
    
    // Install hooks
    if (!InstallHooks()) {
        FM2K::IPC::Shutdown();
        return FM2KHOOK_ERROR_CREATE_HOOK;
    }
    
    return FM2KHOOK_OK;
}

FM2KHOOK_API void FM2KHook_Shutdown() {
    // Disable and remove hooks
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    
    // Shutdown IPC
    FM2K::IPC::Shutdown();
}

FM2KHOOK_API const char* FM2KHook_GetLastError() {
    return g_last_error;
}

} 