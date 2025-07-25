#include "hooks.h"
#include "globals.h"
#include "logging.h"
#include "input_handler.h"
#include "savestate.h"
#include "debug_features.h"
#include "game_patches.h"
#include "state_monitor.h"
#include "css_handler.h"
#include "gekkonet_hooks.h"
#include <MinHook.h>
#include <cstdlib>
#include <cstring>

bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing MinHook...");
    
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: MH_Initialize failed: %d", mh_init);
        return false;
    }
    
    // Hook ProcessGameInputs
    void* inputFuncAddr = (void*)FM2K::State::Memory::PROCESS_INPUTS_ADDR;
    if (MH_CreateHook(inputFuncAddr, (void*)FM2K_ProcessGameInputs_GekkoNet, (void**)&original_process_inputs) != MH_OK ||
        MH_EnableHook(inputFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook ProcessGameInputs");
        MH_Uninitialize();
        return false;
    }

    // Hook GetPlayerInput
    void* getInputFuncAddr = (void*)FM2K::State::Memory::GET_PLAYER_INPUT_ADDR;
    if (MH_CreateHook(getInputFuncAddr, (void*)Hook_GetPlayerInput, (void**)&original_get_player_input) != MH_OK ||
        MH_EnableHook(getInputFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook GetPlayerInput");
        MH_Uninitialize();
        return false;
    }

    // Hook UpdateGameState
    void* updateFuncAddr = (void*)FM2K::State::Memory::UPDATE_GAME_ADDR;
    if (MH_CreateHook(updateFuncAddr, (void*)Hook_UpdateGameState, (void**)&original_update_game) != MH_OK ||
        MH_EnableHook(updateFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook UpdateGameState");
        MH_Uninitialize();
        return false;
    }

    // Hook RunGameLoop
    void* runGameLoopFuncAddr = (void*)FM2K::State::Memory::RUN_GAME_LOOP_ADDR;
    if (MH_CreateHook(runGameLoopFuncAddr, (void*)Hook_RunGameLoop, (void**)&original_run_game_loop) != MH_OK ||
        MH_EnableHook(runGameLoopFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook RunGameLoop");
        MH_Uninitialize();
        return false;
    }

    // Hook RenderGame
    void* renderFuncAddr = (void*)0x404DD0;
    if (MH_CreateHook(renderFuncAddr, (void*)Hook_RenderGame, (void**)&original_render_game) != MH_OK ||
        MH_EnableHook(renderFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook RenderGame");
        MH_Uninitialize();
        return false;
    }

    // Hook game_rand
    void* gameRandFuncAddr = (void*)0x417A22;
    if (MH_CreateHook(gameRandFuncAddr, (void*)Hook_GameRand, (void**)&original_game_rand) != MH_OK ||
        MH_EnableHook(gameRandFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook game_rand");
        MH_Uninitialize();
        return false;
    }
    
    // Apply boot-to-character-select patches directly
    ApplyBootToCharacterSelectPatches();
    
    // Apply character select mode patches
    ApplyCharacterSelectModePatches();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: BSNES-level architecture installed successfully!");
    
    return true;
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hooks shut down");
}

int __cdecl Hook_UpdateGameState() {
    // Frame control is now handled at the main loop level (Hook_RunGameLoop)
    // This function just processes per-frame logic when the main loop allows it to run
    
    MonitorGameStateTransitions();
    CheckForDebugCommands();
    CheckForHotkeys();
    ProcessManualSaveLoadRequests();
    ProcessCSSDelayedInputs();
    
    if (original_update_game) {
        return original_update_game();
    }
    return 0;
}

void __cdecl Hook_RenderGame() {
    if (original_render_game) {
        original_render_game();
    }
}

BOOL __cdecl Hook_RunGameLoop() {
    // COMPLETE MAIN LOOP REPLACEMENT: Like BSNES, we completely replace the game loop
    // instead of trying to patch the existing complex one
    
    // Determine if we should use GekkoNet rollback
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
    bool dual_client_mode = (::player_index == 0 || ::player_index == 1);
    bool use_gekko_replacement = !is_true_offline && dual_client_mode && gekko_initialized && gekko_session;
    
    if (use_gekko_replacement) {
        // COMPLETE GEKKO-INTEGRATED MAIN LOOP: This replaces the entire original loop
        return GekkoNet_MainLoop();
    } else {
        // Use original main loop for offline/single-player
        ApplyCharacterSelectModePatches();
        return original_run_game_loop ? original_run_game_loop() : FALSE;
    }
}