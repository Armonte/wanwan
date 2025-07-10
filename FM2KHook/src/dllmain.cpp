#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <SDL3/SDL.h>
// Direct GekkoNet integration
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include "state_manager.h"

// Direct GekkoNet session (no shared memory needed)
static GekkoSession* gekko_session = nullptr;
static int p1_handle = -1;
static int p2_handle = -1;
static bool gekko_initialized = false;
static bool is_online_mode = false;
static bool is_host = false;

// Shared memory for configuration
static HANDLE shared_memory_handle = nullptr;
static void* shared_memory_data = nullptr;

// State management
static FM2K::State::GameState saved_states[8];  // Ring buffer for 8 frames
static uint32_t current_state_index = 0;
static bool state_manager_initialized = false;

// Shared memory structure matching the launcher
struct SharedInputData {
    uint32_t frame_number;
    uint16_t p1_input;
    uint16_t p2_input;
    bool valid;
    
    // Network configuration
    bool is_online_mode;
    bool is_host;
    char remote_address[64];
    uint16_t port;
    uint8_t input_delay;
    bool config_updated;
};

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

// Input buffer addresses (correct addresses from IDA analysis)
static constexpr uintptr_t P1_INPUT_ADDR = 0x4259C0;  // g_p1_input[0]
static constexpr uintptr_t P2_INPUT_ADDR = 0x4259C4;  // g_p2_input

// State memory addresses (from state_manager.h)
static constexpr uintptr_t P1_HP_ADDR = 0x47010C;
static constexpr uintptr_t P2_HP_ADDR = 0x47030C;
static constexpr uintptr_t ROUND_TIMER_ADDR = 0x470060;
static constexpr uintptr_t GAME_TIMER_ADDR = 0x470044;
static constexpr uintptr_t RANDOM_SEED_ADDR = 0x41FB1C;

// Fletcher32 function is in state_manager.cpp

// Initialize shared memory for configuration
bool InitializeSharedMemory() {
    // Create shared memory for communication with launcher
    shared_memory_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(SharedInputData),
        "FM2K_InputSharedMemory"
    );
    
    if (shared_memory_handle == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to create shared memory");
        return false;
    }
    
    shared_memory_data = MapViewOfFile(
        shared_memory_handle,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(SharedInputData)
    );
    
    if (shared_memory_data == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to map shared memory view");
        CloseHandle(shared_memory_handle);
        shared_memory_handle = nullptr;
        return false;
    }
    
    // Initialize shared memory data
    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    memset(shared_data, 0, sizeof(SharedInputData));
    shared_data->config_updated = false;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Shared memory initialized successfully");
    return true;
}

// Check for configuration updates from launcher
bool CheckConfigurationUpdates() {
    if (!shared_memory_data) return false;
    
    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    if (shared_data->config_updated) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Configuration update received - Online: %s, Host: %s", 
                    shared_data->is_online_mode ? "YES" : "NO", shared_data->is_host ? "YES" : "NO");
        
        // Update local configuration
        is_online_mode = shared_data->is_online_mode;
        is_host = shared_data->is_host;
        
        // Clear the update flag
        shared_data->config_updated = false;
        
        // Reconfigure GekkoNet session if needed
        if (gekko_session && gekko_initialized) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Reconfiguring GekkoNet session...");
            // TODO: Implement session reconfiguration
        }
        
        return true;
    }
    
    return false;
}

// Initialize state manager for rollback
bool InitializeStateManager() {
    // Clear state buffer
    memset(saved_states, 0, sizeof(saved_states));
    current_state_index = 0;
    state_manager_initialized = true;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: State manager initialized");
    return true;
}

// Save game state directly (in-process)
bool SaveGameStateDirect(FM2K::State::GameState* state, uint32_t frame_number) {
    if (!state) return false;
    
    // Read game state directly from memory (no ReadProcessMemory needed)
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)RANDOM_SEED_ADDR;
    
    // Validate pointers and read state
    if (!IsBadReadPtr(frame_ptr, sizeof(uint32_t))) {
        state->core.input_buffer_index = *frame_ptr;
    }
    if (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) {
        state->core.p1_input_current = *p1_input_ptr;
    }
    if (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) {
        state->core.p2_input_current = *p2_input_ptr;
    }
    if (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t))) {
        state->core.p1_hp = *p1_hp_ptr;
    }
    if (!IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t))) {
        state->core.p2_hp = *p2_hp_ptr;
    }
    if (!IsBadReadPtr(round_timer_ptr, sizeof(uint32_t))) {
        state->core.round_timer = *round_timer_ptr;
    }
    if (!IsBadReadPtr(game_timer_ptr, sizeof(uint32_t))) {
        state->core.game_timer = *game_timer_ptr;
    }
    if (!IsBadReadPtr(random_seed_ptr, sizeof(uint32_t))) {
        state->core.random_seed = *random_seed_ptr;
    }
    
    // Set metadata
    state->frame_number = frame_number;
    state->timestamp_ms = SDL_GetTicks();
    
    // Calculate checksum using Fletcher32
    state->checksum = FM2K::State::Fletcher32(reinterpret_cast<const uint8_t*>(&state->core), sizeof(FM2K::State::CoreGameState));
    
    return true;
}

// Load game state directly (in-process)
bool LoadGameStateDirect(const FM2K::State::GameState* state) {
    if (!state) return false;
    
    // Write game state directly to memory (no WriteProcessMemory needed)
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)RANDOM_SEED_ADDR;
    
    // Validate pointers and write state
    if (!IsBadWritePtr(frame_ptr, sizeof(uint32_t))) {
        *frame_ptr = state->core.input_buffer_index;
    }
    if (!IsBadWritePtr(p1_input_ptr, sizeof(uint16_t))) {
        *p1_input_ptr = (uint16_t)state->core.p1_input_current;
    }
    if (!IsBadWritePtr(p2_input_ptr, sizeof(uint16_t))) {
        *p2_input_ptr = (uint16_t)state->core.p2_input_current;
    }
    if (!IsBadWritePtr(p1_hp_ptr, sizeof(uint32_t))) {
        *p1_hp_ptr = state->core.p1_hp;
    }
    if (!IsBadWritePtr(p2_hp_ptr, sizeof(uint32_t))) {
        *p2_hp_ptr = state->core.p2_hp;
    }
    if (!IsBadWritePtr(round_timer_ptr, sizeof(uint32_t))) {
        *round_timer_ptr = state->core.round_timer;
    }
    if (!IsBadWritePtr(game_timer_ptr, sizeof(uint32_t))) {
        *game_timer_ptr = state->core.game_timer;
    }
    if (!IsBadWritePtr(random_seed_ptr, sizeof(uint32_t))) {
        *random_seed_ptr = state->core.random_seed;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: State loaded for frame %u", state->frame_number);
    return true;
}

// Save state to ring buffer
bool SaveStateToBuffer(uint32_t frame_number) {
    if (!state_manager_initialized) return false;
    
    uint32_t index = frame_number % 8;
    return SaveGameStateDirect(&saved_states[index], frame_number);
}

// Load state from ring buffer
bool LoadStateFromBuffer(uint32_t frame_number) {
    if (!state_manager_initialized) return false;
    
    uint32_t index = frame_number % 8;
    return LoadGameStateDirect(&saved_states[index]);
}

// Configure network session based on mode
bool ConfigureNetworkMode(bool online_mode, bool host_mode) {
    is_online_mode = online_mode;
    is_host = host_mode;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network mode configured - Online: %s, Host: %s", 
                online_mode ? "YES" : "NO", host_mode ? "YES" : "NO");
    return true;
}

// Initialize GekkoNet session for rollback netcode
bool InitializeGekkoNet() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** INSIDE InitializeGekkoNet FUNCTION ***");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Creating GekkoNet session...");
    
    // Create GekkoNet session
    if (!gekko_create(&gekko_session)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to create GekkoNet session!");
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session created successfully");
    
    // Configure session for 2-player fighting game
    GekkoConfig config;
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = 8;
    config.spectator_delay = 0;
    config.input_size = 1;  // 1 byte per player (8-bit input)
    config.state_size = 1024;  // Starting with 1KB state size
    config.limited_saving = false;
    config.post_sync_joining = false;
    config.desync_detection = true;
    
    gekko_start(gekko_session, &config);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session configured for 2 players");
    
    // Add players based on session mode
    if (is_online_mode) {
        // Online mode: Add local player and wait for remote player
        if (is_host) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Adding local player (host)");
            p1_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
            // P2 will be added when remote player connects
            p2_handle = -1;
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Adding local player (client)");
            p2_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
            // P1 will be added when we connect to host
            p1_handle = -1;
        }
    } else {
        // Offline mode: Add both players as local
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Adding both players as local (offline mode)");
        p1_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        p2_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
    }
    
    // Validate player handles
    if ((!is_online_mode && (p1_handle < 0 || p2_handle < 0)) ||
        (is_online_mode && is_host && p1_handle < 0) ||
        (is_online_mode && !is_host && p2_handle < 0)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to add players! P1: %d, P2: %d", p1_handle, p2_handle);
        gekko_destroy(gekko_session);
        gekko_session = nullptr;
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Players added - P1 handle: %d, P2 handle: %d", p1_handle, p2_handle);
    
    // Set input delay (can be configured later)
    if (p1_handle >= 0) {
        gekko_set_local_delay(gekko_session, p1_handle, 2);
    }
    if (p2_handle >= 0) {
        gekko_set_local_delay(gekko_session, p2_handle, 2);
    }
    
    gekko_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialization complete!");
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
    
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: process_game_inputs called! Hook frame %u, Game frame %u", 
             //g_frame_counter, game_frame);
    
    // Capture current inputs from game memory (with enhanced validation)
    uint32_t p1_input = 0;
    uint32_t p2_input = 0;
    bool p1_input_valid = false;
    bool p2_input_valid = false;
    
    uint32_t* p1_input_ptr = (uint32_t*)P1_INPUT_ADDR;
    uint32_t* p2_input_ptr = (uint32_t*)P2_INPUT_ADDR;
    
    if (p1_input_ptr && !IsBadReadPtr(p1_input_ptr, sizeof(uint32_t))) {
        p1_input = *p1_input_ptr;
        p1_input_valid = true;
    }
    if (p2_input_ptr && !IsBadReadPtr(p2_input_ptr, sizeof(uint32_t))) {
        p2_input = *p2_input_ptr;
        p2_input_valid = true;
    }
    
    // Validate input ranges (FM2K uses 11-bit inputs)
    if (p1_input_valid && (p1_input & 0xFFFFF800)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: P1 input has invalid high bits: 0x%08X", p1_input);
        p1_input &= 0x07FF;  // Mask to 11 bits
    }
    if (p2_input_valid && (p2_input & 0xFFFFF800)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: P2 input has invalid high bits: 0x%08X", p2_input);
        p2_input &= 0x07FF;  // Mask to 11 bits
    }
    
    // Check for configuration updates from launcher
    CheckConfigurationUpdates();
    
    // Log more frequently to debug input capture
    if (g_frame_counter % 10 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Frame %u - Game frame: %u - P1: 0x%08X (addr valid: %s), P2: 0x%08X (addr valid: %s)", 
                 g_frame_counter, game_frame, p1_input, 
                 (!IsBadReadPtr(p1_input_ptr, sizeof(uint32_t))) ? "YES" : "NO",
                 p2_input,
                 (!IsBadReadPtr(p2_input_ptr, sizeof(uint32_t))) ? "YES" : "NO");
        
        // Write to log file for verification
        FILE* log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
        if (log) {
            fprintf(log, "FM2K HOOK: Frame %u - Game frame: %u - P1: 0x%08X (addr valid: %s), P2: 0x%08X (addr valid: %s)\n", 
                    g_frame_counter, game_frame, p1_input, 
                    (!IsBadReadPtr(p1_input_ptr, sizeof(uint32_t))) ? "YES" : "NO",
                    p2_input,
                    (!IsBadReadPtr(p2_input_ptr, sizeof(uint32_t))) ? "YES" : "NO");
            fflush(log);
            fclose(log);
        }
    }
    
    // Forward inputs directly to GekkoNet (with enhanced error handling)
    if (gekko_initialized && gekko_session) {
        // Only process inputs if we have valid data
        if (p1_input_valid || p2_input_valid) {
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
            
            // Add inputs to GekkoNet session based on valid player handles and input data
            if (p1_handle >= 0 && p1_input_valid) {
                gekko_add_local_input(gekko_session, p1_handle, &p1_gekko);
            }
            if (p2_handle >= 0 && p2_input_valid) {
                gekko_add_local_input(gekko_session, p2_handle, &p2_gekko);
            }
            
            // Save current state before processing GekkoNet updates
            if (state_manager_initialized && (g_frame_counter % 1) == 0) {  // Save every frame
                SaveStateToBuffer(g_frame_counter);
            }
            
            // Process GekkoNet updates after adding inputs
            int update_count = 0;
            auto updates = gekko_update_session(gekko_session, &update_count);
            
            // Handle GekkoNet rollback events with validation
            if (updates && update_count > 0) {
                for (int i = 0; i < update_count; i++) {
                    auto* update = updates[i];
                    if (!update) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Null update at index %d", i);
                        continue;
                    }
                    
                    if (update->type == LoadEvent) {
                        // Rollback to specific frame
                        uint32_t target_frame = update->data.load.frame;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Rollback to frame %u (current: %u)", target_frame, g_frame_counter);
                        
                        if (state_manager_initialized && target_frame <= g_frame_counter) {
                            if (!LoadStateFromBuffer(target_frame)) {
                                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Failed to load state for frame %u", target_frame);
                            }
                        } else {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Invalid rollback target frame %u", target_frame);
                        }
                    }
                }
            }
            
            // Log successful input processing occasionally
            if (g_frame_counter % 100 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Frame %u - P1: 0x%08X->0x%02X (%s), P2: 0x%08X->0x%02X (%s), Updates: %d", 
                         g_frame_counter, p1_input, p1_gekko, p1_input_valid ? "valid" : "invalid", 
                         p2_input, p2_gekko, p2_input_valid ? "valid" : "invalid", update_count);
            }
        } else {
            // No valid inputs - still need to update GekkoNet
            if (g_frame_counter % 300 == 0) {  // Log every 5 seconds
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: No valid inputs at frame %u", g_frame_counter);
            }
        }
    } else {
        // GekkoNet not initialized - log occasionally
        if (g_frame_counter % 300 == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session not initialized at frame %u", g_frame_counter);
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
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: update_game_state called!");
    
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
            
            // Initialize SDL's logging system
            SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_DEBUG);
            SDL_SetLogOutputFunction([](void* userdata, int category, SDL_LogPriority priority, const char* message) {
                // Forward to launcher's log system via SDL
                SDL_LogMessage(category, priority, "FM2K HOOK: %s", message);
            }, nullptr);
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DLL attached to process!");
            
            // Initialize shared memory for configuration
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initializing shared memory...");
            if (!InitializeSharedMemory()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize shared memory");
            }
            
            // Initialize state manager for rollback
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initializing state manager...");
            if (!InitializeStateManager()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize state manager");
            }
            
            // Initialize GekkoNet session directly in DLL
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "About to initialize GekkoNet...");
            
            // Default to local mode (offline) - can be changed later via configuration
            ConfigureNetworkMode(false, false);
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Calling InitializeGekkoNet() now...");
            bool gekko_result = InitializeGekkoNet();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "InitializeGekkoNet returned");
            
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
        
        // Cleanup shared memory
        if (shared_memory_data) {
            UnmapViewOfFile(shared_memory_data);
            shared_memory_data = nullptr;
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