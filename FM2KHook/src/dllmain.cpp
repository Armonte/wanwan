#include "common.h"
#include "globals.h"
#include "logging.h"
#include "shared_mem.h"
#include "state_manager.h"
#include "gekkonet_hooks.h"
#include "hooks.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        {
            DisableThreadLibraryCalls(hModule);
            
            AllocConsole();
            FILE* fDummy;
            freopen_s(&fDummy, "CONOUT$", "w", stdout);
            freopen_s(&fDummy, "CONOUT$", "w", stderr);
            freopen_s(&fDummy, "CONIN$", "r", stdin);
            
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** DLL_PROCESS_ATTACH - Starting initialization ***");
            
            // CRITICAL: Read player index from environment BEFORE initializing logging
            char* env_player = getenv("FM2K_PLAYER_INDEX");
            if (env_player) {
                ::player_index = static_cast<uint8_t>(atoi(env_player));
                ::is_host = (::player_index == 0);
            }
            
            InitializeFileLogging();
            
            // Check if this is true offline mode - safe to enable shared memory for single client
            char* env_offline = getenv("FM2K_TRUE_OFFLINE");
            bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
            
            // FIXED: Enable shared memory for network sessions (GekkoNet requires it)
            // Only disable for true offline mode if conflicts occur
            if (is_true_offline) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "TRUE OFFLINE mode detected - enabling shared memory for debugging features");
                InitializeSharedMemory();
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Network mode detected - enabling shared memory for GekkoNet coordination");
                InitializeSharedMemory(); // FIXED: Enable for GekkoNet networking
            }
            
            char* forced_seed = getenv("FM2K_FORCE_RNG_SEED");
            if (forced_seed) {
                // ... (force RNG seed)
            }
            
            FM2K::State::InitializeStateManager();
            // Don't call ConfigureNetworkMode here - it would override the correct is_host value
            // ConfigureNetworkMode(false, false); // REMOVED: This was setting is_host=false for all clients
            
            // Log the network configuration that was set based on player_index
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network mode configured - Online: NO, Host: %s", 
                       ::is_host ? "YES" : "NO");
            
            InitializeInputRecording();
            
            if (!InitializeHooks()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to initialize hooks!");
                return FALSE;
            }
            
            // DUAL CLIENT TESTING: Initialize GekkoNet immediately when player_index is set
            bool dual_client_mode = (::player_index == 0 || ::player_index == 1);
            bool should_init_gekko = !is_true_offline || dual_client_mode;
            
            if (should_init_gekko) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DLL_MAIN: DUAL CLIENT mode detected (player_index=%d) - initializing GekkoNet early...", ::player_index);
                if (InitializeGekkoNet()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DLL_MAIN: GekkoNet initialized successfully!");
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DLL_MAIN: GekkoNet initialization failed!");
                }
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DLL_MAIN: SINGLE CLIENT offline mode - GekkoNet will be skipped");
            }
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: DLL initialization complete!");
            break;
        }
        
    case DLL_PROCESS_DETACH:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: DLL detaching from process");
        
        CleanupGekkoNet();
        CleanupFileLogging();
        CleanupInputRecording();
        CleanupSharedMemory(); // FIXED: Re-enabled to match init
        ShutdownHooks();
        break;
    }
    return TRUE;
}