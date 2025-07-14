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
            
            InitializeFileLogging();
            InitializeSharedMemory();
            
            char* forced_seed = getenv("FM2K_FORCE_RNG_SEED");
            if (forced_seed) {
                // ... (force RNG seed)
            }
            
            FM2K::State::InitializeStateManager();
            ConfigureNetworkMode(false, false);
            InitializeInputRecording();
            
            if (!InitializeHooks()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to initialize hooks!");
                return FALSE;
            }
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: DLL initialization complete!");
            break;
        }
        
    case DLL_PROCESS_DETACH:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: DLL detaching from process");
        
        CleanupGekkoNet();
        CleanupFileLogging();
        CleanupInputRecording();
        CleanupSharedMemory();
        ShutdownHooks();
        break;
    }
    return TRUE;
}