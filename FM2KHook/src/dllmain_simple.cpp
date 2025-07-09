#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdint>

// Simple shared memory structure for input communication
struct SharedInputData {
    uint32_t frame_number;
    uint16_t p1_input;
    uint16_t p2_input;
    bool valid;
};

static HANDLE shared_memory_handle = nullptr;
static SharedInputData* shared_data = nullptr;

// Simple hook function types (matching FM2K patterns)
typedef int (__cdecl *ProcessGameInputsFn)();
typedef int (__cdecl *UpdateGameStateFn)();

// Original function pointers
static ProcessGameInputsFn original_process_inputs = nullptr;
static UpdateGameStateFn original_update_game = nullptr;

// Hook state
static uint32_t g_frame_counter = 0;

// Key FM2K addresses (from IDA analysis)
static constexpr uintptr_t PROCESS_INPUTS_ADDR = 0x4146D0;
static constexpr uintptr_t UPDATE_GAME_ADDR = 0x404CD0;
static constexpr uintptr_t FRAME_COUNTER_ADDR = 0x447EE0;

// Input buffer addresses
static constexpr uintptr_t P1_INPUT_ADDR = 0x470100;
static constexpr uintptr_t P2_INPUT_ADDR = 0x470300;

// Simple hook implementations (like your working ML2 code)
int __cdecl Hook_ProcessGameInputs() {
    g_frame_counter++;
    
    // Read the actual frame counter from game memory (with basic validation)
    uint32_t game_frame = 0;
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    if (frame_ptr && !IsBadReadPtr(frame_ptr, sizeof(uint32_t))) {
        game_frame = *frame_ptr;
    }
    
    // Multiple output methods to ensure we see it
    char debug_msg[256];
    snprintf(debug_msg, sizeof(debug_msg), 
             "FM2K HOOK: process_game_inputs called! Hook frame %u, Game frame %u\n", 
             g_frame_counter, game_frame);
    
    // No console output - using debug strings and log file
    
    // Output to Windows debug console (visible in DebugView)
    OutputDebugStringA(debug_msg);
    
    // Write to log file for verification
    FILE* log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
    if (log) {
        fprintf(log, "%s", debug_msg);
        fflush(log);
        fclose(log);
    }
    
    // Capture current inputs from game memory (with validation)
    uint16_t p1_input = 0;
    uint16_t p2_input = 0;
    
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    
    if (p1_input_ptr && !IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) {
        p1_input = *p1_input_ptr;
    }
    if (p2_input_ptr && !IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) {
        p2_input = *p2_input_ptr;
    }
    
    // Log every 60 frames to reduce spam
    if (g_frame_counter % 60 == 0) {
        char input_msg[256];
        snprintf(input_msg, sizeof(input_msg), 
                 "FM2K HOOK: Frame %u - P1: 0x%04X, P2: 0x%04X\n", 
                 g_frame_counter, p1_input, p2_input);
        OutputDebugStringA(input_msg);
        
        FILE* log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
        if (log) {
            fprintf(log, "%s", input_msg);
            fflush(log);
            fclose(log);
        }
    }
    
    // Forward inputs to launcher via shared memory
    if (shared_data) {
        shared_data->frame_number = g_frame_counter;
        shared_data->p1_input = p1_input;
        shared_data->p2_input = p2_input;
        shared_data->valid = true;
    }
    
    // Call original function
    int result = 0;
    if (original_process_inputs) {
        result = original_process_inputs();
    }
    
    return result;
}

int __cdecl Hook_UpdateGameState() {
    char debug_msg[] = "FM2K HOOK: update_game_state called!\n";
    
    OutputDebugStringA(debug_msg);
    
    // Call original function
    int result = 0;
    if (original_update_game) {
        result = original_update_game();
    }
    
    return result;
}

// Simple initialization function
bool InitializeHooks() {
    OutputDebugStringA("FM2K HOOK: Initializing MinHook...\n");
    
    // Initialize MinHook
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "ERROR FM2K HOOK: MH_Initialize failed: %d\n", mh_init);
        OutputDebugStringA(err_msg);
        return false;
    }
    
    // Validate target addresses before hooking
    if (IsBadCodePtr((FARPROC)PROCESS_INPUTS_ADDR) || IsBadCodePtr((FARPROC)UPDATE_GAME_ADDR)) {
        OutputDebugStringA("ERROR FM2K HOOK: Target addresses are invalid or not yet mapped\n");
        return false;
    }
    
    // Install hook for process_game_inputs function
    void* inputFuncAddr = (void*)PROCESS_INPUTS_ADDR;
    MH_STATUS status1 = MH_CreateHook(inputFuncAddr, (void*)Hook_ProcessGameInputs, (void**)&original_process_inputs);
    if (status1 != MH_OK) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "ERROR FM2K HOOK: Failed to create input hook: %d\n", status1);
        OutputDebugStringA(err_msg);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable1 = MH_EnableHook(inputFuncAddr);
    if (enable1 != MH_OK) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "ERROR FM2K HOOK: Failed to enable input hook: %d\n", enable1);
        OutputDebugStringA(err_msg);
        MH_Uninitialize();
        return false;
    }
    
    // Install hook for update_game_state function  
    void* updateFuncAddr = (void*)UPDATE_GAME_ADDR;
    MH_STATUS status2 = MH_CreateHook(updateFuncAddr, (void*)Hook_UpdateGameState, (void**)&original_update_game);
    if (status2 != MH_OK) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "ERROR FM2K HOOK: Failed to create update hook: %d\n", status2);
        OutputDebugStringA(err_msg);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable2 = MH_EnableHook(updateFuncAddr);
    if (enable2 != MH_OK) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "ERROR FM2K HOOK: Failed to enable update hook: %d\n", enable2);
        OutputDebugStringA(err_msg);
        MH_Uninitialize();
        return false;
    }
    
    OutputDebugStringA("SUCCESS FM2K HOOK: All hooks installed successfully!\n");
    char msg1[128], msg2[128];
    snprintf(msg1, sizeof(msg1), "   - Input processing hook at 0x%08X\n", PROCESS_INPUTS_ADDR);
    snprintf(msg2, sizeof(msg2), "   - Game state update hook at 0x%08X\n", UPDATE_GAME_ADDR);
    OutputDebugStringA(msg1);
    OutputDebugStringA(msg2);
    return true;
}

// Simple shutdown function
void ShutdownHooks() {
    OutputDebugStringA("FM2K HOOK: Shutting down hooks...\n");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    OutputDebugStringA("FM2K HOOK: Hooks shut down\n");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        {
            // Disable thread library calls
            DisableThreadLibraryCalls(hModule);
            
            // Don't allocate separate console - use debug output and log file instead
            
            OutputDebugStringA("FM2K HOOK: DLL attached to process!\n");
            
            // Initialize shared memory for input communication
            shared_memory_handle = CreateFileMappingA(
                INVALID_HANDLE_VALUE,
                nullptr,
                PAGE_READWRITE,
                0,
                sizeof(SharedInputData),
                "FM2K_InputSharedMemory"
            );
            
            if (shared_memory_handle != nullptr) {
                shared_data = (SharedInputData*)MapViewOfFile(
                    shared_memory_handle,
                    FILE_MAP_ALL_ACCESS,
                    0,
                    0,
                    sizeof(SharedInputData)
                );
                
                if (shared_data) {
                    // Initialize shared data
                    shared_data->frame_number = 0;
                    shared_data->p1_input = 0;
                    shared_data->p2_input = 0;
                    shared_data->valid = false;
                    OutputDebugStringA("FM2K HOOK: Shared memory initialized\n");
                } else {
                    OutputDebugStringA("FM2K HOOK: Failed to map shared memory view\n");
                }
            } else {
                OutputDebugStringA("FM2K HOOK: Failed to create shared memory\n");
            }

            // Write initial log entry
            FILE* log = fopen("C:\\Games\\fm2k_hook_log.txt", "w");
            if (log) {
                fprintf(log, "FM2K HOOK: DLL attached to process at %lu\n", GetTickCount());
                fclose(log);
            }
            
            // Wait a bit for the game to initialize before installing hooks
            Sleep(100);
            
            // Initialize hooks after game has had time to start
            if (!InitializeHooks()) {
                    OutputDebugStringA("ERROR FM2K HOOK: Failed to initialize hooks!\n");
                return FALSE;
            }
            
            OutputDebugStringA("SUCCESS FM2K HOOK: DLL initialization complete!\n");
            break;
        }
        
    case DLL_PROCESS_DETACH:
        OutputDebugStringA("FM2K HOOK: DLL detaching from process\n");
        
        // Cleanup shared memory
        if (shared_data) {
            UnmapViewOfFile(shared_data);
            shared_data = nullptr;
        }
        if (shared_memory_handle) {
            CloseHandle(shared_memory_handle);
            shared_memory_handle = nullptr;
        }
        
        ShutdownHooks();
        break;
    }
    return TRUE;
}