#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdint>

// Direct GekkoNet integration
#include "gekkonet.h"

// Direct GekkoNet session (no shared memory needed)
static GekkoSession* gekko_session = nullptr;
static int p1_handle = -1;
static int p2_handle = -1;
static bool gekko_initialized = false;

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

// GekkoNet initialization function
bool InitializeGekkoNet() {
    OutputDebugStringA("FM2K HOOK: *** INSIDE InitializeGekkoNet FUNCTION ***\n");
    OutputDebugStringA("FM2K HOOK: Initializing GekkoNet session...\n");
    
    // Create GekkoNet session
    OutputDebugStringA("FM2K HOOK: Calling gekko_create...\n");
    gekko_create(&gekko_session);
    if (!gekko_session) {
        OutputDebugStringA("ERROR: gekko_create failed - session is null\n");
        return false;
    }
    OutputDebugStringA("FM2K HOOK: gekko_create succeeded\n");
    
    // Configure for local session (both players local)
    GekkoConfig config = {};
    config.num_players = 2;                           // FM2K is 2-player
    config.input_size = sizeof(uint8_t);              // Use 8-bit inputs like bridge
    config.max_spectators = 0;                        // No spectators for local testing
    config.input_prediction_window = 0;               // No prediction needed for local testing
    
    gekko_start(gekko_session, &config);
    
    // Add local players (both local for offline mode)
    p1_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);  // P1
    p2_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);  // P2
    
    if (p1_handle < 0 || p2_handle < 0) {
        OutputDebugStringA("Failed to add players to GekkoNet session\n");
        return false;
    }
    
    // Set input delay (2 frames = 20ms at 100 FPS)
    gekko_set_local_delay(gekko_session, p1_handle, 2);
    gekko_set_local_delay(gekko_session, p2_handle, 2);
    
    gekko_initialized = true;
    OutputDebugStringA("FM2K HOOK: GekkoNet session initialized successfully!\n");
    return true;
}

// Simple hook implementations (like your working ML2 code)
int __cdecl Hook_ProcessGameInputs() {
    g_frame_counter++;
    
    // Always output on first few calls to verify hook is working
    if (g_frame_counter <= 5) {
        char init_msg[128];
        snprintf(init_msg, sizeof(init_msg), "FM2K HOOK: Hook called! Frame %u\n", g_frame_counter);
        OutputDebugStringA(init_msg);
    }
    
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
    
    // Log more frequently to debug input capture
    if (g_frame_counter % 10 == 0) {
        char input_msg[512];
        snprintf(input_msg, sizeof(input_msg), 
                 "FM2K HOOK: Frame %u - Game frame: %u - P1: 0x%04X (addr valid: %s), P2: 0x%04X (addr valid: %s)\n", 
                 g_frame_counter, game_frame, p1_input, 
                 (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) ? "YES" : "NO",
                 p2_input,
                 (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) ? "YES" : "NO");
        OutputDebugStringA(input_msg);
        
        FILE* log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
        if (log) {
            fprintf(log, "%s", input_msg);
            fflush(log);
            fclose(log);
        }
    }
    
    // Forward inputs directly to GekkoNet (no shared memory needed)
    if (gekko_initialized && gekko_session) {
        // Convert 16-bit FM2K inputs to 8-bit GekkoNet format
        uint8_t p1_gekko = 0;
        uint8_t p2_gekko = 0;
        
        // Map FM2K input bits to 8-bit GekkoNet format (matching bridge logic)
        if (p1_input & 0x01) p1_gekko |= 0x01;  // left
        if (p1_input & 0x02) p1_gekko |= 0x02;  // right
        if (p1_input & 0x04) p1_gekko |= 0x04;  // up
        if (p1_input & 0x08) p1_gekko |= 0x08;  // down
        if (p1_input & 0x10) p1_gekko |= 0x10;  // button1
        if (p1_input & 0x20) p1_gekko |= 0x20;  // button2
        if (p1_input & 0x40) p1_gekko |= 0x40;  // button3
        if (p1_input & 0x80) p1_gekko |= 0x80;  // button4
        
        if (p2_input & 0x01) p2_gekko |= 0x01;  // left
        if (p2_input & 0x02) p2_gekko |= 0x02;  // right
        if (p2_input & 0x04) p2_gekko |= 0x04;  // up
        if (p2_input & 0x08) p2_gekko |= 0x08;  // down
        if (p2_input & 0x10) p2_gekko |= 0x10;  // button1
        if (p2_input & 0x20) p2_gekko |= 0x20;  // button2
        if (p2_input & 0x40) p2_gekko |= 0x40;  // button3
        if (p2_input & 0x80) p2_gekko |= 0x80;  // button4
        
        // Add inputs to GekkoNet session
        gekko_add_local_input(gekko_session, p1_handle, &p1_gekko);
        gekko_add_local_input(gekko_session, p2_handle, &p2_gekko);
        
        // Process GekkoNet updates after adding inputs
        int update_count = 0;
        auto updates = gekko_update_session(gekko_session, &update_count);
        
        // Log successful input processing occasionally
        if (g_frame_counter % 60 == 0) {
            char gekko_msg[256];
            snprintf(gekko_msg, sizeof(gekko_msg), 
                     "GekkoNet: Frame %u - P1: 0x%04X→0x%02X, P2: 0x%04X→0x%02X, Updates: %d\n", 
                     g_frame_counter, p1_input, p1_gekko, p2_input, p2_gekko, update_count);
            OutputDebugStringA(gekko_msg);
        }
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
            
            // Write initial log entry first
            FILE* log = fopen("C:\\Games\\fm2k_hook_log.txt", "w");
            if (log) {
                fprintf(log, "FM2K HOOK: DLL attached to process at %lu\n", GetTickCount());
                fprintf(log, "FM2K HOOK: About to initialize GekkoNet...\n");
                fflush(log);
                fclose(log);
            }
            
            // Initialize GekkoNet session directly in DLL
            OutputDebugStringA("FM2K HOOK: About to initialize GekkoNet...\n");
            
            bool gekko_result = InitializeGekkoNet();
            OutputDebugStringA("FM2K HOOK: InitializeGekkoNet returned\n");
            
            if (!gekko_result) {
                OutputDebugStringA("ERROR FM2K HOOK: Failed to initialize GekkoNet!\n");
                FILE* error_log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
                if (error_log) {
                    fprintf(error_log, "ERROR FM2K HOOK: Failed to initialize GekkoNet!\n");
                    fflush(error_log);
                    fclose(error_log);
                }
                // Continue anyway - we can still hook without rollback
            } else {
                OutputDebugStringA("FM2K HOOK: GekkoNet initialized successfully!\n");
                FILE* success_log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
                if (success_log) {
                    fprintf(success_log, "FM2K HOOK: GekkoNet initialized successfully!\n");
                    fflush(success_log);
                    fclose(success_log);
                }
            }

            // Log entry already written above
            
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
        
        // Cleanup GekkoNet session
        if (gekko_session) {
            gekko_destroy(gekko_session);
            gekko_session = nullptr;
            gekko_initialized = false;
            OutputDebugStringA("FM2K HOOK: GekkoNet session closed\n");
        }
        
        ShutdownHooks();
        break;
    }
    return TRUE;
}