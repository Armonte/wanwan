#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <MinHook.h>
#include <SDL3/SDL.h>

// Simple working DLL - just prove injection works first
static HANDLE g_logFile = nullptr;

// Simple logging function that writes to both file and SDL
void LogMessage(const char* message) {
    // Always try file logging first
    FILE* log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
    if (log) {
        fprintf(log, "%s\n", message);
        fflush(log);
        fclose(log);
    }
    
    // Also try SDL logging if available
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: %s", message);
}

// Ultra-simple hook functions - just to prove hooks work
typedef int (__cdecl *ProcessInputsFn)();
static ProcessInputsFn original_process_inputs = nullptr;
static uint32_t hook_call_count = 0;

int __cdecl Hook_ProcessInputs() {
    hook_call_count++;
    
    // Log every 60 calls (about once per second at 60fps)
    if (hook_call_count % 60 == 0) {
        char buffer[256];
        sprintf(buffer, "Hook called %u times", hook_call_count);
        LogMessage(buffer);
    }
    
    // Call original function
    if (original_process_inputs) {
        return original_process_inputs();
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        {
            DisableThreadLibraryCalls(hModule);
            
            LogMessage("DLL attached to process!");
            
            // Wait a bit for the process to initialize
            Sleep(100);
            
            LogMessage("Initializing MinHook...");
            
            // Initialize MinHook
            MH_STATUS mh_init = MH_Initialize();
            if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
                char buffer[256];
                sprintf(buffer, "ERROR: MH_Initialize failed with status %d", mh_init);
                LogMessage(buffer);
                return FALSE;
            }
            
            LogMessage("MinHook initialized successfully");
            
            // Check if target address is valid before hooking
            void* target = (void*)0x4025A0;
            if (IsBadCodePtr((FARPROC)target)) {
                LogMessage("ERROR: Target address 0x4025A0 is not valid code");
                MH_Uninitialize();
                return FALSE;
            }
            
            LogMessage("Target address validated, creating hook...");
            
            // Hook process_input_history function at 0x4025A0 (from IDA analysis)
            MH_STATUS status = MH_CreateHook(target, (void*)Hook_ProcessInputs, (void**)&original_process_inputs);
            if (status != MH_OK) {
                char buffer[256];
                sprintf(buffer, "ERROR: Failed to create hook, status %d", status);
                LogMessage(buffer);
                MH_Uninitialize();
                return FALSE;
            }
            
            LogMessage("Hook created, enabling...");
            
            // Enable the hook
            if (MH_EnableHook(target) != MH_OK) {
                LogMessage("ERROR: Failed to enable hook");
                MH_Uninitialize();
                return FALSE;
            }
            
            LogMessage("SUCCESS: FM2K hook initialized!");
            
            // Signal the launcher that DLL initialization is complete
            HANDLE init_event = CreateEventW(nullptr, TRUE, FALSE, L"FM2KHook_Initialized");
            if (init_event) {
                SetEvent(init_event);
                CloseHandle(init_event);
                LogMessage("Signaled launcher that initialization is complete");
            } else {
                LogMessage("ERROR: Failed to create initialization event");
            }
            
            break;
        }
        
    case DLL_PROCESS_DETACH:
        {
            LogMessage("DLL detaching from process");
            
            // Cleanup hooks
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            break;
        }
    }
    return TRUE;
}