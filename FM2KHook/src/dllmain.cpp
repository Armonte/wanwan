#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <SDL3/SDL.h>
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

// Simple test function without GekkoNet
bool InitializeGekkoNet() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** INSIDE InitializeGekkoNet FUNCTION ***");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Function is being called!");
    
    // Skip GekkoNet for now - just test the function call
    gekko_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Function returning true");
    return true;
}

// Simple hook implementations (like your working ML2 code)
int __cdecl Hook_ProcessGameInputs() {
    g_frame_counter++;
    
    // Always output on first few calls to verify hook is working
    if (g_frame_counter <= 5) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hook called! Frame %u", g_frame_counter);
    }
    
    // Read the actual frame counter from game memory (with basic validation)
    uint32_t game_frame = 0;
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    if (frame_ptr && !IsBadReadPtr(frame_ptr, sizeof(uint32_t))) {
        game_frame = *frame_ptr;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: process_game_inputs called! Hook frame %u, Game frame %u", 
             g_frame_counter, game_frame);
    
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
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Frame %u - Game frame: %u - P1: 0x%04X (addr valid: %s), P2: 0x%04X (addr valid: %s)", 
                 g_frame_counter, game_frame, p1_input, 
                 (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) ? "YES" : "NO",
                 p2_input,
                 (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) ? "YES" : "NO");
        
        // Write to log file for verification
        FILE* log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
        if (log) {
            fprintf(log, "FM2K HOOK: Frame %u - Game frame: %u - P1: 0x%04X (addr valid: %s), P2: 0x%04X (addr valid: %s)\n", 
                    g_frame_counter, game_frame, p1_input, 
                    (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) ? "YES" : "NO",
                    p2_input,
                    (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) ? "YES" : "NO");
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
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Frame %u - P1: 0x%04X->0x%02X, P2: 0x%04X->0x%02X, Updates: %d", 
                     g_frame_counter, p1_input, p1_gekko, p2_input, p2_gekko, update_count);
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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: update_game_state called!");
    
    // Call original function
    int result = 0;
    if (original_update_game) {
        result = original_update_game();
    }
    
    return result;
}

// Simple initialization function
bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing MinHook...");
    
    // Initialize MinHook
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: MH_Initialize failed: %d", mh_init);
        return false;
    }
    
    // Validate target addresses before hooking
    if (IsBadCodePtr((FARPROC)PROCESS_INPUTS_ADDR) || IsBadCodePtr((FARPROC)UPDATE_GAME_ADDR)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Target addresses are invalid or not yet mapped");
        return false;
    }
    
    // Install hook for process_game_inputs function
    void* inputFuncAddr = (void*)PROCESS_INPUTS_ADDR;
    MH_STATUS status1 = MH_CreateHook(inputFuncAddr, (void*)Hook_ProcessGameInputs, (void**)&original_process_inputs);
    if (status1 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create input hook: %d", status1);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable1 = MH_EnableHook(inputFuncAddr);
    if (enable1 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable input hook: %d", enable1);
        MH_Uninitialize();
        return false;
    }
    
    // Install hook for update_game_state function  
    void* updateFuncAddr = (void*)UPDATE_GAME_ADDR;
    MH_STATUS status2 = MH_CreateHook(updateFuncAddr, (void*)Hook_UpdateGameState, (void**)&original_update_game);
    if (status2 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create update hook: %d", status2);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable2 = MH_EnableHook(updateFuncAddr);
    if (enable2 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable update hook: %d", enable2);
        MH_Uninitialize();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: All hooks installed successfully!");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   - Input processing hook at 0x%08X", PROCESS_INPUTS_ADDR);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   - Game state update hook at 0x%08X", UPDATE_GAME_ADDR);
    return true;
}

// Simple shutdown function
void ShutdownHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Shutting down hooks...");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hooks shut down");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        {
            // Disable thread library calls
            DisableThreadLibraryCalls(hModule);
            
            // Allocate a console window for debugging purposes
            AllocConsole();
            FILE* fDummy;
            freopen_s(&fDummy, "CONOUT$", "w", stdout);
            freopen_s(&fDummy, "CONOUT$", "w", stderr);
            freopen_s(&fDummy, "CONIN$", "r", stdin);
            std::cout.clear();
            std::cerr.clear();
            std::cin.clear();
            
            // Initialize SDL's logging system
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
            SDL_SetLogOutputFunction([](void* userdata, int category, SDL_LogPriority priority, const char* message) {
                printf("%s\n", message);
            }, nullptr);
            
            // The console is now available
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Console window opened for debugging.");

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: DLL attached to process!");
            
            // Write initial log entry first
            FILE* log = fopen("C:\\Games\\fm2k_hook_log.txt", "w");
            if (log) {
                fprintf(log, "FM2K HOOK: DLL attached to process at %lu\n", GetTickCount());
                fprintf(log, "FM2K HOOK: About to initialize GekkoNet...\n");
                fflush(log);
                fclose(log);
            }
            
            // Initialize GekkoNet session directly in DLL
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: About to initialize GekkoNet...");
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Calling InitializeGekkoNet() now...");
            bool gekko_result = InitializeGekkoNet();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: InitializeGekkoNet returned");
            
            if (!gekko_result) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to initialize GekkoNet!");
                FILE* error_log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
                if (error_log) {
                    fprintf(error_log, "ERROR FM2K HOOK: Failed to initialize GekkoNet!\n");
                    fflush(error_log);
                    fclose(error_log);
                }
                // Continue anyway - we can still hook without rollback
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialized successfully!");
                FILE* success_log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
                if (success_log) {
                    fprintf(success_log, "FM2K HOOK: GekkoNet initialized successfully!\n");
                    fflush(success_log);
                    fclose(success_log);
                }
            }

            // Wait a bit for the game to initialize before installing hooks
            Sleep(100);
            
            // Initialize hooks after game has had time to start
            if (!InitializeHooks()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to initialize hooks!");
                return FALSE;
            }
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: DLL initialization complete!");
            break;
        }
        
    case DLL_PROCESS_DETACH:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: DLL detaching from process");
        
        // Cleanup GekkoNet session
        if (gekko_session) {
            gekko_destroy(gekko_session);
            gekko_session = nullptr;
            gekko_initialized = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session closed");
        }
        
        ShutdownHooks();
        break;
    }
    return TRUE;
}