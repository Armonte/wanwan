#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <memory>
#include <chrono>
#include <SDL3/SDL.h>
// Direct GekkoNet integration
#include "gekkonet.h"
#include "state_manager.h"
#include "../../LocalNetworkAdapter.h"

// Save state profile enumeration
enum class SaveStateProfile : uint32_t {
    MINIMAL = 0,    // ~50KB - Core state + active objects only
    STANDARD = 1,   // ~200KB - Essential runtime state  
    COMPLETE = 2    // ~850KB - Everything (current implementation)
};

// Forward declarations
bool SaveCoreStateBasic(FM2K::State::GameState* state, uint32_t frame_number);
bool SaveGameStateDirect(FM2K::State::GameState* state, uint32_t frame_number);
uint32_t CalculateStateChecksum(const FM2K::State::GameState* state);
bool RestoreStateFromStruct(const FM2K::State::GameState* state, uint32_t target_frame);

// Direct GekkoNet session (no shared memory needed)
static GekkoSession* gekko_session = nullptr;
static LocalNetworkAdapter* local_adapter = nullptr;
static int p1_handle = -1;
static int p2_handle = -1;
static bool gekko_initialized = false;
static bool is_online_mode = false;
static bool is_host = false;

// Shared memory for configuration
static HANDLE shared_memory_handle = nullptr;
static void* shared_memory_data = nullptr;

// Enhanced state management with comprehensive memory capture
static FM2K::State::GameState saved_states[8];  // Ring buffer for 8 frames (rollback buffer)
static uint32_t current_state_index = 0;
static bool state_manager_initialized = false;

// Named save slots for manual save/load
static FM2K::State::GameState save_slots[8];     // 8 manual save slots
static bool slot_occupied[8] = {false};          // Track which slots have saves
static SaveStateProfile slot_profiles[8];        // Track which profile was used for each slot
static uint32_t slot_active_object_counts[8];    // Track how many active objects were saved per slot
static uint32_t last_auto_save_frame = 0;

// Per-slot buffers for large memory regions (each slot gets its own buffers)
static std::unique_ptr<uint8_t[]> slot_player_data_buffers[8];
static std::unique_ptr<uint8_t[]> slot_object_pool_buffers[8];

// Temporary buffers for rollback (shared)
static std::unique_ptr<uint8_t[]> rollback_player_data_buffer;
static std::unique_ptr<uint8_t[]> rollback_object_pool_buffer;
static bool large_buffers_allocated = false;

// Performance tracking
static uint32_t total_saves = 0;
static uint32_t total_loads = 0;
static uint64_t total_save_time_us = 0;
static uint64_t total_load_time_us = 0;

// High-resolution timing helper
static inline uint64_t get_microseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

// State change debugging
static FM2K::State::GameState last_core_state = {};
static bool last_core_state_valid = false;

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
    
    // Debug commands from launcher
    bool debug_save_state_requested;
    bool debug_load_state_requested;
    uint32_t debug_rollback_frames;
    bool debug_rollback_requested;
    uint32_t debug_command_id;  // Incremented for each command to ensure processing
    
    // Slot-based save/load system
    bool debug_save_to_slot_requested;
    bool debug_load_from_slot_requested;
    uint32_t debug_target_slot;  // Which slot to save to / load from (0-7)
    
    // Auto-save configuration
    bool auto_save_enabled;
    uint32_t auto_save_interval_frames;  // How often to auto-save
    SaveStateProfile save_profile;       // Which save state profile to use
    
    // Slot status feedback to UI
    struct SlotInfo {
        bool occupied;
        uint32_t frame_number;
        uint64_t timestamp_ms;
        uint32_t checksum;
        uint32_t state_size_kb;  // Size in KB for analysis
        uint32_t save_time_us;   // Save time in microseconds
        uint32_t load_time_us;   // Load time in microseconds
    } slot_status[8];
    
    // Performance statistics
    struct PerformanceStats {
        uint32_t total_saves;
        uint32_t total_loads;
        uint32_t avg_save_time_us;
        uint32_t avg_load_time_us;
        uint32_t memory_usage_mb;
    } perf_stats;
    
    // GekkoNet client role coordination (simplified)
    uint8_t player_index;            // 0 for Player 1, 1 for Player 2
    uint8_t session_role;            // 0 = Host, 1 = Guest
};

// Generate unique log file path based on player index
static std::string GetLogFilePath() {
    // Use process ID to create unique log files per client
    DWORD process_id = GetCurrentProcessId();
    
    // Check if we have shared data to determine client role
    if (shared_memory_data) {
        SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
        uint8_t player_index = shared_data->player_index;
        uint8_t session_role = shared_data->session_role;
        
        // Create descriptive log file names
        const char* role_name = (session_role == 0) ? "host" : "client";
        return "C:\\Games\\fm2k_hook_" + std::string(role_name) + ".txt";
    }
    
    // Fallback using process ID if no shared memory yet
    return "C:\\Games\\fm2k_hook_pid" + std::to_string(process_id) + ".txt";
}

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

// Enhanced state memory addresses (from save state documentation)
static constexpr uintptr_t P1_HP_ADDR = 0x47010C;
static constexpr uintptr_t P2_HP_ADDR = 0x47030C;
static constexpr uintptr_t ROUND_TIMER_ADDR = 0x470060;
static constexpr uintptr_t GAME_TIMER_ADDR = 0x470044;
static constexpr uintptr_t RANDOM_SEED_ADDR = 0x41FB1C;

// Major memory regions for comprehensive state capture
static constexpr uintptr_t PLAYER_DATA_SLOTS_ADDR = 0x4D1D80;  // g_player_data_slots
static constexpr size_t PLAYER_DATA_SLOTS_SIZE = 0x701F8;      // 459,256 bytes
static constexpr uintptr_t GAME_OBJECT_POOL_ADDR = 0x4701E0;   // g_game_object_pool  
static constexpr size_t GAME_OBJECT_POOL_SIZE = 0x5F800;       // 391,168 bytes (1024 * 382)

// Additional game state variables from documentation
static constexpr uintptr_t GAME_MODE_ADDR = 0x470054;          // g_game_mode
static constexpr uintptr_t ROUND_SETTING_ADDR = 0x470068;      // g_round_setting
static constexpr uintptr_t P1_ROUND_COUNT_ADDR = 0x4700EC;     // g_p1_round_count
static constexpr uintptr_t P1_ROUND_STATE_ADDR = 0x4700F0;     // g_p1_round_state
static constexpr uintptr_t P1_ACTION_STATE_ADDR = 0x47019C;    // g_p1_action_state
static constexpr uintptr_t P2_ACTION_STATE_ADDR = 0x4701A0;    // g_p2_action_state (estimated)
static constexpr uintptr_t CAMERA_X_ADDR = 0x447F2C;          // g_camera_x
static constexpr uintptr_t CAMERA_Y_ADDR = 0x447F30;          // g_camera_y
static constexpr uintptr_t TIMER_COUNTDOWN1_ADDR = 0x4456E4;   // g_timer_countdown1
static constexpr uintptr_t TIMER_COUNTDOWN2_ADDR = 0x447D91;   // g_timer_countdown2

// Object list management (critical for object pool iteration)
static constexpr uintptr_t OBJECT_LIST_HEADS_ADDR = 0x430240;  // g_object_list_heads
static constexpr uintptr_t OBJECT_LIST_TAILS_ADDR = 0x430244;  // g_object_list_tails

// Additional timer that may be the in-game timer (needs verification)
static constexpr uintptr_t ROUND_TIMER_COUNTER_ADDR = 0x424F00; // g_round_timer_counter

// Simple Fletcher32 implementation for checksums
namespace FM2K {
namespace State {
uint32_t Fletcher32(const uint8_t* data, size_t len) {
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    size_t blocks = len / 2;

    // Process 2-byte blocks
    while (blocks) {
        size_t tlen = blocks > 359 ? 359 : blocks;
        blocks -= tlen;
        do {
            sum1 += (data[0] << 8) | data[1];
            sum2 += sum1;
            data += 2;
        } while (--tlen);

        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }

    // Handle remaining byte if length is odd
    if (len & 1) {
        sum1 += *data << 8;
        sum2 += sum1;
        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }

    // Final reduction
    sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
    sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);

    return (sum2 << 16) | sum1;
}
} // namespace State
} // namespace FM2K

// Enhanced object analysis and selective saving
struct ActiveObjectInfo {
    uint32_t index;
    uint32_t type_or_id;
    bool is_active;
};

// Helper function to analyze and count active objects in the object pool
uint32_t AnalyzeActiveObjects(ActiveObjectInfo* active_objects = nullptr, uint32_t max_objects = 0) {
    uint32_t active_count = 0;
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    
    if (IsBadReadPtr(object_pool_ptr, GAME_OBJECT_POOL_SIZE)) {
        return 0;
    }
    
    // Each object is 382 bytes, analyze each slot
    for (int i = 0; i < 1024; i++) {
        uint32_t* object_ptr = (uint32_t*)(object_pool_ptr + (i * 382));
        if (!IsBadReadPtr(object_ptr, sizeof(uint32_t))) {
            uint32_t object_header = *object_ptr;
            
            // Check multiple patterns to determine if object is active:
            // 1. Non-zero first 4 bytes (type/ID)
            // 2. Check second DWORD for additional validation
            uint32_t* second_dword = object_ptr + 1;
            uint32_t second_value = (!IsBadReadPtr(second_dword, sizeof(uint32_t))) ? *second_dword : 0;
            
            bool is_active = (object_header != 0) && (object_header != 0xFFFFFFFF) && 
                           (second_value != 0xCCCCCCCC); // Common uninitialized pattern
            
            if (is_active) {
                if (active_objects && active_count < max_objects) {
                    active_objects[active_count].index = i;
                    active_objects[active_count].type_or_id = object_header;
                    active_objects[active_count].is_active = true;
                }
                active_count++;
            }
        }
    }
    
    return active_count;
}

// Helper function for backward compatibility
uint32_t CountActiveObjects() {
    return AnalyzeActiveObjects();
}

// Save only active objects to buffer (for MINIMAL profile)
bool SaveActiveObjectsOnly(uint8_t* destination_buffer, size_t buffer_size, uint32_t* objects_saved = nullptr) {
    if (!destination_buffer || buffer_size == 0) {
        return false;
    }
    
    // Get list of active objects
    ActiveObjectInfo active_objects[1024];
    uint32_t active_count = AnalyzeActiveObjects(active_objects, 1024);
    
    if (active_count == 0) {
        if (objects_saved) *objects_saved = 0;
        return true; // No objects to save is valid
    }
    
    // Calculate required buffer size (each object is 382 bytes + 4 bytes for index)
    size_t required_size = active_count * (382 + sizeof(uint32_t));
    if (required_size > buffer_size) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Buffer too small for active objects: need %zu, have %zu", 
                   required_size, buffer_size);
        return false;
    }
    
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    uint8_t* dest_ptr = destination_buffer;
    uint32_t saved_count = 0;
    
    // Save each active object with its index
    for (uint32_t i = 0; i < active_count; i++) {
        uint32_t obj_index = active_objects[i].index;
        
        // Write object index first (4 bytes)
        *(uint32_t*)dest_ptr = obj_index;
        dest_ptr += sizeof(uint32_t);
        
        // Write object data (382 bytes)
        uint8_t* source_obj = object_pool_ptr + (obj_index * 382);
        if (!IsBadReadPtr(source_obj, 382)) {
            memcpy(dest_ptr, source_obj, 382);
            dest_ptr += 382;
            saved_count++;
        }
    }
    
    if (objects_saved) *objects_saved = saved_count;
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Saved %u active objects (%.1fKB vs %.1fKB full pool)", 
                saved_count, (saved_count * 382) / 1024.0f, GAME_OBJECT_POOL_SIZE / 1024.0f);
    
    return true;
}

// Restore active objects from buffer
bool RestoreActiveObjectsOnly(const uint8_t* source_buffer, size_t buffer_size, uint32_t objects_to_restore) {
    if (!source_buffer || buffer_size == 0 || objects_to_restore == 0) {
        return true; // Nothing to restore is valid
    }
    
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    const uint8_t* src_ptr = source_buffer;
    uint32_t restored_count = 0;
    
    // First, clear the entire object pool (assuming inactive objects should be zeroed)
    if (!IsBadWritePtr(object_pool_ptr, GAME_OBJECT_POOL_SIZE)) {
        memset(object_pool_ptr, 0, GAME_OBJECT_POOL_SIZE);
    }
    
    // Restore each saved object to its original position
    for (uint32_t i = 0; i < objects_to_restore; i++) {
        // Read object index (4 bytes)
        uint32_t obj_index = *(const uint32_t*)src_ptr;
        src_ptr += sizeof(uint32_t);
        
        if (obj_index >= 1024) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid object index: %u", obj_index);
            break;
        }
        
        // Restore object data (382 bytes)
        uint8_t* dest_obj = object_pool_ptr + (obj_index * 382);
        if (!IsBadWritePtr(dest_obj, 382)) {
            memcpy(dest_obj, src_ptr, 382);
            restored_count++;
        }
        src_ptr += 382;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Restored %u active objects to object pool", restored_count);
    
    return restored_count == objects_to_restore;
}

// Profile-specific save functions
bool SaveStateMinimal(FM2K::State::GameState* state, uint32_t frame_number) {
    if (!state || !large_buffers_allocated) {
        return false;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Saving MINIMAL state for frame %u", frame_number);
    
    // 1. Save core state only (8KB)
    SaveCoreStateBasic(state, frame_number);
    
    // 2. Save only active objects using smart detection (~10-50KB typical vs 391KB full pool)
    uint32_t objects_saved = 0;
    bool active_objects_saved = SaveActiveObjectsOnly(rollback_object_pool_buffer.get(), GAME_OBJECT_POOL_SIZE, &objects_saved);
    
    // Set metadata
    state->frame_number = frame_number;
    state->timestamp_ms = SDL_GetTicks();
    
    // Calculate checksum including active objects if saved successfully
    uint32_t core_checksum = FM2K::State::Fletcher32(reinterpret_cast<const uint8_t*>(&state->core), sizeof(FM2K::State::CoreGameState));
    if (active_objects_saved && objects_saved > 0) {
        // Calculate checksum for active objects data
        size_t active_objects_data_size = objects_saved * (sizeof(uint32_t) + 382); // index + object data
        uint32_t objects_checksum = FM2K::State::Fletcher32(rollback_object_pool_buffer.get(), active_objects_data_size);
        state->checksum = core_checksum ^ objects_checksum;
    } else {
        state->checksum = core_checksum;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "MINIMAL state saved - Frame %u, Active objects: %u/%s, Core+Objects checksum: 0x%08X", 
                frame_number, objects_saved, active_objects_saved ? "saved" : "failed", state->checksum);
    return true;
}

bool SaveStateStandard(FM2K::State::GameState* state, uint32_t frame_number) {
    if (!state || !large_buffers_allocated) {
        return false;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Saving STANDARD state for frame %u", frame_number);
    
    // 1. Save core state (8KB)
    SaveCoreStateBasic(state, frame_number);
    
    // 2. Save essential runtime player data (~100KB estimated)
    // For now, save partial player data (first 100KB of each slot)
    uint8_t* player_data_ptr = (uint8_t*)PLAYER_DATA_SLOTS_ADDR;
    if (!IsBadReadPtr(player_data_ptr, 100 * 1024)) {
        memcpy(rollback_player_data_buffer.get(), player_data_ptr, 100 * 1024);
    }
    
    // 3. Save all active objects (~80KB estimated)
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (!IsBadReadPtr(object_pool_ptr, GAME_OBJECT_POOL_SIZE)) {
        memcpy(rollback_object_pool_buffer.get(), object_pool_ptr, GAME_OBJECT_POOL_SIZE);
    }
    
    // Set metadata
    state->frame_number = frame_number;
    state->timestamp_ms = SDL_GetTicks();
    
    // Calculate comprehensive checksum
    uint32_t core_checksum = FM2K::State::Fletcher32(reinterpret_cast<const uint8_t*>(&state->core), sizeof(FM2K::State::CoreGameState));
    uint32_t player_checksum = FM2K::State::Fletcher32(rollback_player_data_buffer.get(), 100 * 1024);
    uint32_t object_checksum = FM2K::State::Fletcher32(rollback_object_pool_buffer.get(), GAME_OBJECT_POOL_SIZE);
    state->checksum = core_checksum ^ player_checksum ^ object_checksum;
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "STANDARD state saved - Frame %u, Partial player + full objects, checksum: 0x%08X", 
                frame_number, state->checksum);
    return true;
}

bool SaveStateComplete(FM2K::State::GameState* state, uint32_t frame_number) {
    // Use the existing comprehensive save function
    return SaveGameStateDirect(state, frame_number);
}

// Helper function to save basic core state
bool SaveCoreStateBasic(FM2K::State::GameState* state, uint32_t frame_number) {
    // Read basic game state directly from memory
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)RANDOM_SEED_ADDR;
    
    // Additional critical timers and state
    uint32_t* timer_countdown1_ptr = (uint32_t*)TIMER_COUNTDOWN1_ADDR;
    uint32_t* timer_countdown2_ptr = (uint32_t*)TIMER_COUNTDOWN2_ADDR;
    uint32_t* round_timer_counter_ptr = (uint32_t*)ROUND_TIMER_COUNTER_ADDR;
    uint32_t* object_list_heads_ptr = (uint32_t*)OBJECT_LIST_HEADS_ADDR;
    uint32_t* object_list_tails_ptr = (uint32_t*)OBJECT_LIST_TAILS_ADDR;
    
    // Validate pointers and read core state
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
    
    // Read additional critical timers and state
    if (!IsBadReadPtr(timer_countdown1_ptr, sizeof(uint32_t))) {
        state->core.timer_countdown1 = *timer_countdown1_ptr;
    } else {
        state->core.timer_countdown1 = 0;  // Default if not accessible
    }
    
    if (!IsBadReadPtr(timer_countdown2_ptr, sizeof(uint32_t))) {
        state->core.timer_countdown2 = *timer_countdown2_ptr;
    } else {
        state->core.timer_countdown2 = 0;
    }
    
    if (!IsBadReadPtr(round_timer_counter_ptr, sizeof(uint32_t))) {
        state->core.round_timer_counter = *round_timer_counter_ptr;
        // Log this value to help identify if it's the in-game timer
        if (frame_number % 100 == 0) {  // Log every 100 frames
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Round timer counter: %u (frame %u)", 
                        state->core.round_timer_counter, frame_number);
        }
    } else {
        state->core.round_timer_counter = 0;
    }
    
    if (!IsBadReadPtr(object_list_heads_ptr, sizeof(uint32_t))) {
        state->core.object_list_heads = *object_list_heads_ptr;
    } else {
        state->core.object_list_heads = 0;
    }
    
    if (!IsBadReadPtr(object_list_tails_ptr, sizeof(uint32_t))) {
        state->core.object_list_tails = *object_list_tails_ptr;
    } else {
        state->core.object_list_tails = 0;
    }
    
    return true;
}

// Calculate simple checksum for state data (Fletcher32 algorithm)
uint32_t CalculateStateChecksum(const FM2K::State::GameState* state) {
    if (!state) return 0;
    
    const uint16_t* data = reinterpret_cast<const uint16_t*>(state);
    size_t length = sizeof(FM2K::State::GameState) / sizeof(uint16_t);
    
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    
    while (length) {
        size_t tlen = length > 360 ? 360 : length;
        length -= tlen;
        do {
            sum1 += *data++;
            sum2 += sum1;
        } while (--tlen);
        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }
    
    sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
    sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    
    return (sum2 << 16) | sum1;
}

// Restore game state from GekkoNet state structure
bool RestoreStateFromStruct(const FM2K::State::GameState* state, uint32_t target_frame) {
    if (!state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "RestoreStateFromStruct: Null state pointer");
        return false;
    }
    
    // Restore core game state to memory
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)RANDOM_SEED_ADDR;
    
    // Additional critical timers
    uint32_t* timer_countdown1_ptr = (uint32_t*)TIMER_COUNTDOWN1_ADDR;
    uint32_t* timer_countdown2_ptr = (uint32_t*)TIMER_COUNTDOWN2_ADDR;
    uint32_t* round_timer_counter_ptr = (uint32_t*)ROUND_TIMER_COUNTER_ADDR;
    uint32_t* object_list_heads_ptr = (uint32_t*)OBJECT_LIST_HEADS_ADDR;
    uint32_t* object_list_tails_ptr = (uint32_t*)OBJECT_LIST_TAILS_ADDR;
    
    // Write state back to game memory with validation
    if (!IsBadWritePtr(frame_ptr, sizeof(uint32_t))) {
        *frame_ptr = state->core.input_buffer_index;
    }
    if (!IsBadWritePtr(p1_input_ptr, sizeof(uint16_t))) {
        *p1_input_ptr = state->core.p1_input_current;
    }
    if (!IsBadWritePtr(p2_input_ptr, sizeof(uint16_t))) {
        *p2_input_ptr = state->core.p2_input_current;
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
    if (!IsBadWritePtr(timer_countdown1_ptr, sizeof(uint32_t))) {
        *timer_countdown1_ptr = state->core.timer_countdown1;
    }
    if (!IsBadWritePtr(timer_countdown2_ptr, sizeof(uint32_t))) {
        *timer_countdown2_ptr = state->core.timer_countdown2;
    }
    if (!IsBadWritePtr(round_timer_counter_ptr, sizeof(uint32_t))) {
        *round_timer_counter_ptr = state->core.round_timer_counter;
    }
    if (!IsBadWritePtr(object_list_heads_ptr, sizeof(uint32_t))) {
        *object_list_heads_ptr = state->core.object_list_heads;
    }
    if (!IsBadWritePtr(object_list_tails_ptr, sizeof(uint32_t))) {
        *object_list_tails_ptr = state->core.object_list_tails;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "RestoreStateFromStruct: Restored state for frame %u", target_frame);
    return true;
}

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
    shared_data->debug_save_state_requested = false;
    shared_data->debug_load_state_requested = false;
    shared_data->debug_rollback_requested = false;
    shared_data->debug_rollback_frames = 0;
    shared_data->debug_command_id = 0;
    shared_data->debug_save_to_slot_requested = false;
    shared_data->debug_load_from_slot_requested = false;
    shared_data->debug_target_slot = 0;
    shared_data->auto_save_enabled = true;  // Enable auto-save by default
    shared_data->auto_save_interval_frames = 120;  // Auto-save every 120 frames (1.2 seconds at 100 FPS)
    shared_data->save_profile = SaveStateProfile::STANDARD;  // Default to balanced profile
    
    // Initialize slot status
    for (int i = 0; i < 8; i++) {
        shared_data->slot_status[i].occupied = false;
        shared_data->slot_status[i].frame_number = 0;
        shared_data->slot_status[i].timestamp_ms = 0;
        shared_data->slot_status[i].checksum = 0;
        shared_data->slot_status[i].state_size_kb = 0;
        shared_data->slot_status[i].save_time_us = 0;
        shared_data->slot_status[i].load_time_us = 0;
    }
    
    // Initialize performance stats
    shared_data->perf_stats.total_saves = 0;
    shared_data->perf_stats.total_loads = 0;
    shared_data->perf_stats.avg_save_time_us = 0;
    shared_data->perf_stats.avg_load_time_us = 0;
    shared_data->perf_stats.memory_usage_mb = ((PLAYER_DATA_SLOTS_SIZE + GAME_OBJECT_POOL_SIZE) * 9) / (1024 * 1024); // 8 slots + rollback
    
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
        
        // Update auto-save settings
        // (Auto-save configuration can be changed from launcher later)
        
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
    
    // Allocate buffers for large memory regions - per-slot + rollback buffers
    try {
        // Allocate per-slot buffers
        for (int i = 0; i < 8; i++) {
            slot_player_data_buffers[i] = std::make_unique<uint8_t[]>(PLAYER_DATA_SLOTS_SIZE);
            slot_object_pool_buffers[i] = std::make_unique<uint8_t[]>(GAME_OBJECT_POOL_SIZE);
        }
        
        // Allocate rollback buffers
        rollback_player_data_buffer = std::make_unique<uint8_t[]>(PLAYER_DATA_SLOTS_SIZE);
        rollback_object_pool_buffer = std::make_unique<uint8_t[]>(GAME_OBJECT_POOL_SIZE);
        
        // Initialize slot metadata
        for (int i = 0; i < 8; i++) {
            slot_profiles[i] = SaveStateProfile::STANDARD;  // Default profile
            slot_active_object_counts[i] = 0;
        }
        
        large_buffers_allocated = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Allocated %zu KB per slot x8 + rollback (%zu KB total)", 
                    (PLAYER_DATA_SLOTS_SIZE + GAME_OBJECT_POOL_SIZE) / 1024, 
                    ((PLAYER_DATA_SLOTS_SIZE + GAME_OBJECT_POOL_SIZE) * 9) / 1024);
    } catch (const std::bad_alloc& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to allocate state buffers: %s", e.what());
        large_buffers_allocated = false;
        return false;
    }
    
    state_manager_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Enhanced state manager initialized with comprehensive memory capture");
    return true;
}

// Enhanced save game state with comprehensive memory capture
bool SaveGameStateDirect(FM2K::State::GameState* state, uint32_t frame_number) {
    if (!state || !large_buffers_allocated) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid state buffer or large buffers not allocated");
        return false;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Capturing comprehensive game state for frame %u", frame_number);
    
    // Read basic game state directly from memory (no ReadProcessMemory needed)
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)RANDOM_SEED_ADDR;
    
    // Extended game state pointers
    uint32_t* game_mode_ptr = (uint32_t*)GAME_MODE_ADDR;
    uint32_t* round_setting_ptr = (uint32_t*)ROUND_SETTING_ADDR;
    uint32_t* p1_round_count_ptr = (uint32_t*)P1_ROUND_COUNT_ADDR;
    uint32_t* p1_round_state_ptr = (uint32_t*)P1_ROUND_STATE_ADDR;
    uint32_t* p1_action_state_ptr = (uint32_t*)P1_ACTION_STATE_ADDR;
    uint32_t* p2_action_state_ptr = (uint32_t*)P2_ACTION_STATE_ADDR;
    uint32_t* camera_x_ptr = (uint32_t*)CAMERA_X_ADDR;
    uint32_t* camera_y_ptr = (uint32_t*)CAMERA_Y_ADDR;
    uint32_t* timer1_ptr = (uint32_t*)TIMER_COUNTDOWN1_ADDR;
    uint32_t* timer2_ptr = (uint32_t*)TIMER_COUNTDOWN2_ADDR;
    uint32_t* round_timer_counter_ptr = (uint32_t*)ROUND_TIMER_COUNTER_ADDR;
    uint32_t* object_list_heads_ptr = (uint32_t*)OBJECT_LIST_HEADS_ADDR;
    uint32_t* object_list_tails_ptr = (uint32_t*)OBJECT_LIST_TAILS_ADDR;
    
    // Validate basic pointers and read core state
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
    
    // Read additional critical timers and object management state
    if (!IsBadReadPtr(timer1_ptr, sizeof(uint32_t))) {
        state->core.timer_countdown1 = *timer1_ptr;
    }
    if (!IsBadReadPtr(timer2_ptr, sizeof(uint32_t))) {
        state->core.timer_countdown2 = *timer2_ptr;
    }
    if (!IsBadReadPtr(round_timer_counter_ptr, sizeof(uint32_t))) {
        state->core.round_timer_counter = *round_timer_counter_ptr;
    }
    if (!IsBadReadPtr(object_list_heads_ptr, sizeof(uint32_t))) {
        state->core.object_list_heads = *object_list_heads_ptr;
    }
    if (!IsBadReadPtr(object_list_tails_ptr, sizeof(uint32_t))) {
        state->core.object_list_tails = *object_list_tails_ptr;
    }
    
    // Capture major memory regions for comprehensive state
    bool player_data_captured = false;
    bool object_pool_captured = false;
    
    // 1. Player Data Slots (459KB) - most critical for rollback
    uint8_t* player_data_ptr = (uint8_t*)PLAYER_DATA_SLOTS_ADDR;
    if (!IsBadReadPtr(player_data_ptr, PLAYER_DATA_SLOTS_SIZE)) {
        memcpy(rollback_player_data_buffer.get(), player_data_ptr, PLAYER_DATA_SLOTS_SIZE);
        player_data_captured = true;
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Captured player data slots (%zu KB)", PLAYER_DATA_SLOTS_SIZE / 1024);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to capture player data slots - invalid memory");
    }
    
    // 2. Game Object Pool (391KB) - all game entities
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (!IsBadReadPtr(object_pool_ptr, GAME_OBJECT_POOL_SIZE)) {
        memcpy(rollback_object_pool_buffer.get(), object_pool_ptr, GAME_OBJECT_POOL_SIZE);
        object_pool_captured = true;
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Captured game object pool (%zu KB)", GAME_OBJECT_POOL_SIZE / 1024);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to capture game object pool - invalid memory");
    }
    
    // Set metadata
    state->frame_number = frame_number;
    state->timestamp_ms = SDL_GetTicks();
    
    // Calculate comprehensive checksum including large memory regions
    uint32_t core_checksum = FM2K::State::Fletcher32(reinterpret_cast<const uint8_t*>(&state->core), sizeof(FM2K::State::CoreGameState));
    uint32_t player_checksum = player_data_captured ? FM2K::State::Fletcher32(rollback_player_data_buffer.get(), PLAYER_DATA_SLOTS_SIZE) : 0;
    uint32_t object_checksum = object_pool_captured ? FM2K::State::Fletcher32(rollback_object_pool_buffer.get(), GAME_OBJECT_POOL_SIZE) : 0;
    
    // Combine checksums for comprehensive state validation
    state->checksum = core_checksum ^ player_checksum ^ object_checksum;
    
    // Debug what's changing between frames (reduced frequency to minimize spam)
    if (last_core_state_valid && frame_number % 300 == 0) {  // Log changes every 300 frames (3 seconds) to reduce spam
        bool core_changed = memcmp(&state->core, &last_core_state.core, sizeof(FM2K::State::CoreGameState)) != 0;
        if (core_changed) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Core state changes detected:");
            if (state->core.input_buffer_index != last_core_state.core.input_buffer_index) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  Input buffer index: %u → %u", last_core_state.core.input_buffer_index, state->core.input_buffer_index);
            }
            if (state->core.p1_input_current != last_core_state.core.p1_input_current) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  P1 input: 0x%08X → 0x%08X", last_core_state.core.p1_input_current, state->core.p1_input_current);
            }
            if (state->core.p2_input_current != last_core_state.core.p2_input_current) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  P2 input: 0x%08X → 0x%08X", last_core_state.core.p2_input_current, state->core.p2_input_current);
            }
            if (state->core.round_timer != last_core_state.core.round_timer) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  Round timer: %u → %u", last_core_state.core.round_timer, state->core.round_timer);
            }
            if (state->core.game_timer != last_core_state.core.game_timer) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  Game timer: %u → %u", last_core_state.core.game_timer, state->core.game_timer);
            }
            if (state->core.random_seed != last_core_state.core.random_seed) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  RNG seed: 0x%08X → 0x%08X", last_core_state.core.random_seed, state->core.random_seed);
            }
        }
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Checksums - Core: 0x%08X, Player: 0x%08X, Objects: 0x%08X", core_checksum, player_checksum, object_checksum);
        
        // Debug timer values to help identify the in-game timer
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Timer Debug - Round: %u, Game: %u, Counter1: %u, Counter2: %u, RoundCounter: %u", 
                     state->core.round_timer, state->core.game_timer, state->core.timer_countdown1, 
                     state->core.timer_countdown2, state->core.round_timer_counter);
    }
    
    // Store current state for next comparison
    last_core_state = *state;
    last_core_state_valid = true;
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Frame %u state captured - Core: %s, Player Data: %s, Objects: %s (checksum: 0x%08X)",
                frame_number,
                "OK",
                player_data_captured ? "OK" : "FAILED",
                object_pool_captured ? "OK" : "FAILED",
                state->checksum);
    
    return player_data_captured && object_pool_captured;  // Require both major regions for valid state
}

// Enhanced load game state with comprehensive memory restoration
bool LoadGameStateDirect(const FM2K::State::GameState* state) {
    if (!state || !large_buffers_allocated) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid state or large buffers not allocated");
        return false;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Restoring comprehensive game state for frame %u", state->frame_number);
    
    // Write basic game state directly to memory (no WriteProcessMemory needed)
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)RANDOM_SEED_ADDR;
    
    // Extended game state pointers
    uint32_t* game_mode_ptr = (uint32_t*)GAME_MODE_ADDR;
    uint32_t* round_setting_ptr = (uint32_t*)ROUND_SETTING_ADDR;
    uint32_t* p1_round_count_ptr = (uint32_t*)P1_ROUND_COUNT_ADDR;
    uint32_t* p1_round_state_ptr = (uint32_t*)P1_ROUND_STATE_ADDR;
    uint32_t* p1_action_state_ptr = (uint32_t*)P1_ACTION_STATE_ADDR;
    uint32_t* p2_action_state_ptr = (uint32_t*)P2_ACTION_STATE_ADDR;
    uint32_t* camera_x_ptr = (uint32_t*)CAMERA_X_ADDR;
    uint32_t* camera_y_ptr = (uint32_t*)CAMERA_Y_ADDR;
    uint32_t* timer1_ptr = (uint32_t*)TIMER_COUNTDOWN1_ADDR;
    uint32_t* timer2_ptr = (uint32_t*)TIMER_COUNTDOWN2_ADDR;
    uint32_t* round_timer_counter_ptr = (uint32_t*)ROUND_TIMER_COUNTER_ADDR;
    uint32_t* object_list_heads_ptr = (uint32_t*)OBJECT_LIST_HEADS_ADDR;
    uint32_t* object_list_tails_ptr = (uint32_t*)OBJECT_LIST_TAILS_ADDR;
    
    // Read current values before writing for comparison
    uint32_t before_frame = 0, before_p1_hp = 0, before_p2_hp = 0, before_round_timer = 0;
    uint16_t before_p1_input = 0, before_p2_input = 0;
    
    if (!IsBadReadPtr(frame_ptr, sizeof(uint32_t))) before_frame = *frame_ptr;
    if (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) before_p1_input = *p1_input_ptr;
    if (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) before_p2_input = *p2_input_ptr;
    if (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t))) before_p1_hp = *p1_hp_ptr;
    if (!IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t))) before_p2_hp = *p2_hp_ptr;
    if (!IsBadReadPtr(round_timer_ptr, sizeof(uint32_t))) before_round_timer = *round_timer_ptr;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: Before - Frame: %u, P1HP: %u, P2HP: %u, RoundTimer: %u, P1Input: 0x%04X, P2Input: 0x%04X",
                before_frame, before_p1_hp, before_p2_hp, before_round_timer, before_p1_input, before_p2_input);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: Target - Frame: %u, P1HP: %u, P2HP: %u, RoundTimer: %u, P1Input: 0x%08X, P2Input: 0x%08X",
                state->core.input_buffer_index, state->core.p1_hp, state->core.p2_hp, state->core.round_timer, 
                state->core.p1_input_current, state->core.p2_input_current);
    
    // Validate basic pointers and restore core state
    if (!IsBadWritePtr(frame_ptr, sizeof(uint32_t))) {
        *frame_ptr = state->core.input_buffer_index;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: Frame counter written: %u → %u", before_frame, *frame_ptr);
    }
    if (!IsBadWritePtr(p1_input_ptr, sizeof(uint16_t))) {
        *p1_input_ptr = (uint16_t)state->core.p1_input_current;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: P1 input written: 0x%04X → 0x%04X", before_p1_input, *p1_input_ptr);
    }
    if (!IsBadWritePtr(p2_input_ptr, sizeof(uint16_t))) {
        *p2_input_ptr = (uint16_t)state->core.p2_input_current;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: P2 input written: 0x%04X → 0x%04X", before_p2_input, *p2_input_ptr);
    }
    if (!IsBadWritePtr(p1_hp_ptr, sizeof(uint32_t))) {
        *p1_hp_ptr = state->core.p1_hp;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: P1 HP written: %u → %u", before_p1_hp, *p1_hp_ptr);
    }
    if (!IsBadWritePtr(p2_hp_ptr, sizeof(uint32_t))) {
        *p2_hp_ptr = state->core.p2_hp;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: P2 HP written: %u → %u", before_p2_hp, *p2_hp_ptr);
    }
    if (!IsBadWritePtr(round_timer_ptr, sizeof(uint32_t))) {
        *round_timer_ptr = state->core.round_timer;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: Round timer written: %u → %u", before_round_timer, *round_timer_ptr);
    }
    if (!IsBadWritePtr(game_timer_ptr, sizeof(uint32_t))) {
        *game_timer_ptr = state->core.game_timer;
    }
    if (!IsBadWritePtr(random_seed_ptr, sizeof(uint32_t))) {
        *random_seed_ptr = state->core.random_seed;
    }
    
    // Restore additional critical timers and object management state
    if (!IsBadWritePtr(timer1_ptr, sizeof(uint32_t))) {
        *timer1_ptr = state->core.timer_countdown1;
    }
    if (!IsBadWritePtr(timer2_ptr, sizeof(uint32_t))) {
        *timer2_ptr = state->core.timer_countdown2;
    }
    if (!IsBadWritePtr(round_timer_counter_ptr, sizeof(uint32_t))) {
        *round_timer_counter_ptr = state->core.round_timer_counter;
    }
    if (!IsBadWritePtr(object_list_heads_ptr, sizeof(uint32_t))) {
        *object_list_heads_ptr = state->core.object_list_heads;
    }
    if (!IsBadWritePtr(object_list_tails_ptr, sizeof(uint32_t))) {
        *object_list_tails_ptr = state->core.object_list_tails;
    }
    
    // Restore major memory regions for comprehensive rollback
    bool player_data_restored = false;
    bool object_pool_restored = false;
    
    // 1. Player Data Slots (459KB) - critical for character state
    uint8_t* player_data_ptr = (uint8_t*)PLAYER_DATA_SLOTS_ADDR;
    if (!IsBadWritePtr(player_data_ptr, PLAYER_DATA_SLOTS_SIZE)) {
        memcpy(player_data_ptr, rollback_player_data_buffer.get(), PLAYER_DATA_SLOTS_SIZE);
        player_data_restored = true;
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Restored player data slots (%zu KB)", PLAYER_DATA_SLOTS_SIZE / 1024);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to restore player data slots - invalid memory");
    }
    
    // 2. Game Object Pool (391KB) - all game entities
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (!IsBadWritePtr(object_pool_ptr, GAME_OBJECT_POOL_SIZE)) {
        memcpy(object_pool_ptr, rollback_object_pool_buffer.get(), GAME_OBJECT_POOL_SIZE);
        object_pool_restored = true;
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Restored game object pool (%zu KB)", GAME_OBJECT_POOL_SIZE / 1024);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to restore game object pool - invalid memory");
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Frame %u state restored - Core: %s, Player Data: %s, Objects: %s (checksum: 0x%08X)",
                state->frame_number,
                "OK",
                player_data_restored ? "OK" : "FAILED",
                object_pool_restored ? "OK" : "FAILED",
                state->checksum);
    
    return player_data_restored && object_pool_restored;  // Require both major regions for successful restore
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

// Save state to specific slot (with dedicated buffers)
bool SaveStateToSlot(uint32_t slot, uint32_t frame_number) {
    if (!state_manager_initialized || slot >= 8) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid slot %u or state manager not initialized", slot);
        return false;
    }
    
    uint64_t start_time = get_microseconds();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Saving state to slot %u at frame %u", slot, frame_number);
    
    // Get current save profile from shared memory
    SaveStateProfile current_profile = SaveStateProfile::STANDARD;  // Default fallback
    if (shared_memory_data) {
        SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
        current_profile = shared_data->save_profile;
    }
    
    // Save core state using selected profile
    bool save_result = false;
    switch (current_profile) {
        case SaveStateProfile::MINIMAL:
            save_result = SaveStateMinimal(&save_slots[slot], frame_number);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using MINIMAL profile for slot %u", slot);
            break;
        case SaveStateProfile::STANDARD:
            save_result = SaveStateStandard(&save_slots[slot], frame_number);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using STANDARD profile for slot %u", slot);
            break;
        case SaveStateProfile::COMPLETE:
            save_result = SaveStateComplete(&save_slots[slot], frame_number);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using COMPLETE profile for slot %u", slot);
            break;
    }
    
    if (!save_result) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to save state to slot %u using profile %d", slot, (int)current_profile);
        return false;
    }
    
    // Copy memory to slot-specific buffers based on profile
    uint8_t* player_data_ptr = (uint8_t*)PLAYER_DATA_SLOTS_ADDR;
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    
    bool player_saved = false, objects_saved = false;
    size_t player_data_size = 0, object_pool_size = 0;
    
    // Determine how much data to save based on profile
    switch (current_profile) {
        case SaveStateProfile::MINIMAL:
            // MINIMAL: Save minimal player data and active objects only
            player_data_size = 8 * 1024;  // 8KB essential player data
            object_pool_size = GAME_OBJECT_POOL_SIZE;  // Use full buffer size for active object storage
            break;
        case SaveStateProfile::STANDARD:
            // STANDARD: Save partial player data and full objects
            player_data_size = 100 * 1024;  // 100KB essential player data
            object_pool_size = GAME_OBJECT_POOL_SIZE;  // Full object pool
            break;
        case SaveStateProfile::COMPLETE:
            // COMPLETE: Save everything
            player_data_size = PLAYER_DATA_SLOTS_SIZE;  // Full player data
            object_pool_size = GAME_OBJECT_POOL_SIZE;   // Full object pool
            break;
    }
    
    if (!IsBadReadPtr(player_data_ptr, player_data_size)) {
        memcpy(slot_player_data_buffers[slot].get(), player_data_ptr, player_data_size);
        player_saved = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Slot %u: Player data saved (%zu KB)", slot, player_data_size / 1024);
    }
    
    // Save objects based on profile
    if (current_profile == SaveStateProfile::MINIMAL) {
        // For MINIMAL profile, save only active objects
        uint32_t objects_saved_count = 0;
        objects_saved = SaveActiveObjectsOnly(slot_object_pool_buffers[slot].get(), object_pool_size, &objects_saved_count);
        if (objects_saved) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Slot %u: %u active objects saved using MINIMAL profile", slot, objects_saved_count);
        }
    } else {
        // For STANDARD and COMPLETE profiles, save full object pool
        if (!IsBadReadPtr(object_pool_ptr, object_pool_size)) {
            memcpy(slot_object_pool_buffers[slot].get(), object_pool_ptr, object_pool_size);
            objects_saved = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Slot %u: Object pool saved (%zu KB)", slot, object_pool_size / 1024);
        }
    }
    
    if (player_saved && objects_saved) {
        uint64_t end_time = get_microseconds();
        uint32_t save_time_us = (uint32_t)(end_time - start_time);
        uint32_t state_size_kb = (player_data_size + object_pool_size + sizeof(FM2K::State::GameState)) / 1024;
        
        slot_occupied[slot] = true;
        slot_profiles[slot] = current_profile;  // Store which profile was used
        
        // For MINIMAL profile, store active object count for proper restoration
        if (current_profile == SaveStateProfile::MINIMAL) {
            slot_active_object_counts[slot] = CountActiveObjects();
        } else {
            slot_active_object_counts[slot] = 0;  // Not relevant for other profiles
        }
        
        total_saves++;
        total_save_time_us += save_time_us;
        
        // Update shared memory status for UI
        if (shared_memory_data) {
            SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
            shared_data->slot_status[slot].occupied = true;
            shared_data->slot_status[slot].frame_number = frame_number;
            shared_data->slot_status[slot].timestamp_ms = save_slots[slot].timestamp_ms;
            shared_data->slot_status[slot].checksum = save_slots[slot].checksum;
            shared_data->slot_status[slot].state_size_kb = state_size_kb;
            shared_data->slot_status[slot].save_time_us = save_time_us;
            
            // Update performance stats
            shared_data->perf_stats.total_saves = total_saves;
            shared_data->perf_stats.avg_save_time_us = (uint32_t)(total_save_time_us / total_saves);
        }
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "State saved to slot %u (frame %u, %uKB, %uμs, checksum: 0x%08X)", 
                    slot, frame_number, state_size_kb, save_time_us, save_slots[slot].checksum);
        return true;
    }
    
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to save memory regions to slot %u", slot);
    return false;
}

// Load state from specific slot (with dedicated buffers)
bool LoadStateFromSlot(uint32_t slot) {
    if (!state_manager_initialized || slot >= 8) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid slot %u or state manager not initialized", slot);
        return false;
    }
    
    if (!slot_occupied[slot]) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Slot %u is empty", slot);
        return false;
    }
    
    uint64_t start_time = get_microseconds();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Loading state from slot %u (frame %u)", slot, save_slots[slot].frame_number);
    
    // Load core state
    if (!LoadGameStateDirect(&save_slots[slot])) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load core state from slot %u", slot);
        return false;
    }
    
    // Use the profile that was actually used when saving this slot
    SaveStateProfile saved_profile = slot_profiles[slot];  // Use stored profile from save time
    
    // Determine restoration sizes based on profile used when saving
    size_t player_data_size = 0, object_pool_size = 0;
    switch (saved_profile) {
        case SaveStateProfile::MINIMAL:
            player_data_size = 8 * 1024;
            object_pool_size = GAME_OBJECT_POOL_SIZE;
            break;
        case SaveStateProfile::STANDARD:
            player_data_size = 100 * 1024;
            object_pool_size = GAME_OBJECT_POOL_SIZE;
            break;
        case SaveStateProfile::COMPLETE:
            player_data_size = PLAYER_DATA_SLOTS_SIZE;
            object_pool_size = GAME_OBJECT_POOL_SIZE;
            break;
    }
    
    // Restore memory from slot-specific buffers
    uint8_t* player_data_ptr = (uint8_t*)PLAYER_DATA_SLOTS_ADDR;
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    
    bool player_restored = false, objects_restored = false;
    
    if (!IsBadWritePtr(player_data_ptr, player_data_size)) {
        memcpy(player_data_ptr, slot_player_data_buffers[slot].get(), player_data_size);
        player_restored = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Slot %u: Player data restored (%zu KB)", slot, player_data_size / 1024);
    }
    
    // Restore objects based on save profile
    if (saved_profile == SaveStateProfile::MINIMAL) {
        // For MINIMAL profile, use smart active object restoration
        uint32_t active_object_count = slot_active_object_counts[slot];
        if (active_object_count > 0) {
            objects_restored = RestoreActiveObjectsOnly(slot_object_pool_buffers[slot].get(), object_pool_size, active_object_count);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Slot %u: %u active objects restored using MINIMAL profile", slot, active_object_count);
        } else {
            // Clear object pool if no active objects were saved
            if (!IsBadWritePtr(object_pool_ptr, object_pool_size)) {
                memset(object_pool_ptr, 0, object_pool_size);
                objects_restored = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Slot %u: Object pool cleared (no active objects)", slot);
            }
        }
    } else {
        // For STANDARD and COMPLETE profiles, restore normally
        if (!IsBadWritePtr(object_pool_ptr, object_pool_size)) {
            memcpy(object_pool_ptr, slot_object_pool_buffers[slot].get(), object_pool_size);
            objects_restored = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Slot %u: Object pool restored (%zu KB)", slot, object_pool_size / 1024);
        }
    }
    
    if (player_restored && objects_restored) {
        uint64_t end_time = get_microseconds();
        uint32_t load_time_us = (uint32_t)(end_time - start_time);
        
        total_loads++;
        total_load_time_us += load_time_us;
        
        // Update shared memory performance stats
        if (shared_memory_data) {
            SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
            shared_data->slot_status[slot].load_time_us = load_time_us;
            shared_data->perf_stats.total_loads = total_loads;
            shared_data->perf_stats.avg_load_time_us = (uint32_t)(total_load_time_us / total_loads);
        }
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "State loaded from slot %u (frame %u, %uμs, checksum: 0x%08X)", 
                    slot, save_slots[slot].frame_number, load_time_us, save_slots[slot].checksum);
        return true;
    }
    
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to restore memory regions from slot %u", slot);
    return false;
}

// Process debug commands from launcher UI
void ProcessDebugCommands() {
    if (!shared_memory_data) {
        // Only log this occasionally to avoid spam
        static uint32_t no_shared_memory_log_counter = 0;
        if ((no_shared_memory_log_counter++ % 1000) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HOOK: ProcessDebugCommands - no shared memory");
        }
        return;
    }
    
    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    static uint32_t last_processed_command_id = 0;
    
    // Only process new commands
    if (shared_data->debug_command_id == last_processed_command_id) {
        return;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Processing debug command ID %u (last: %u)", shared_data->debug_command_id, last_processed_command_id);
    
    // Log what commands are pending
    if (shared_data->debug_save_to_slot_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: -> debug_save_to_slot_requested = TRUE for slot %u", shared_data->debug_target_slot);
    }
    if (shared_data->debug_load_from_slot_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: -> debug_load_from_slot_requested = TRUE for slot %u", shared_data->debug_target_slot);
    }
    if (shared_data->debug_save_state_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: -> debug_save_state_requested = TRUE");
    }
    if (shared_data->debug_load_state_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: -> debug_load_state_requested = TRUE");
    }
    if (shared_data->debug_rollback_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: -> debug_rollback_requested = TRUE for %u frames", shared_data->debug_rollback_frames);
    }
    
    // Manual save state
    if (shared_data->debug_save_state_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Manual save state requested");
        if (state_manager_initialized) {
            uint32_t current_frame = g_frame_counter;
            if (SaveStateToBuffer(current_frame)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: State saved successfully for frame %u", current_frame);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Failed to save state for frame %u", current_frame);
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: State manager not initialized");
        }
        shared_data->debug_save_state_requested = false;
    }
    
    // Manual load state
    if (shared_data->debug_load_state_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Manual load state requested");
        if (state_manager_initialized) {
            uint32_t current_frame = g_frame_counter;
            // Load from previous frame (simple test)
            uint32_t load_frame = current_frame > 0 ? current_frame - 1 : current_frame;
            if (LoadStateFromBuffer(load_frame)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: State loaded successfully from frame %u", load_frame);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Failed to load state from frame %u", load_frame);
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: State manager not initialized");
        }
        shared_data->debug_load_state_requested = false;
    }
    
    // Force rollback
    if (shared_data->debug_rollback_requested) {
        uint32_t rollback_frames = shared_data->debug_rollback_frames;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Force rollback requested - %u frames", rollback_frames);
        
        if (state_manager_initialized && rollback_frames > 0) {
            uint32_t current_frame = g_frame_counter;
            uint32_t target_frame = current_frame > rollback_frames ? current_frame - rollback_frames : 0;
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Rolling back from frame %u to frame %u", current_frame, target_frame);
            
            if (LoadStateFromBuffer(target_frame)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Rollback successful - restored frame %u", target_frame);
                // Update frame counter to reflect rollback
                g_frame_counter = target_frame;
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Rollback failed - could not load frame %u", target_frame);
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Invalid rollback parameters - frames: %u, initialized: %s", 
                         rollback_frames, state_manager_initialized ? "YES" : "NO");
        }
        
        shared_data->debug_rollback_requested = false;
        shared_data->debug_rollback_frames = 0;
    }
    
    // Save to specific slot
    if (shared_data->debug_save_to_slot_requested) {
        uint32_t slot = shared_data->debug_target_slot;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Save to slot %u requested", slot);
        
        if (state_manager_initialized && slot < 8) {
            uint32_t current_frame = g_frame_counter;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Attempting to save frame %u to slot %u", current_frame, slot);
            if (SaveStateToSlot(slot, current_frame)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: State saved to slot %u successfully", slot);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Failed to save state to slot %u", slot);
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Invalid slot %u or state manager not initialized (initialized: %s)", slot, state_manager_initialized ? "YES" : "NO");
        }
        
        shared_data->debug_save_to_slot_requested = false;
    }
    
    // Load from specific slot
    if (shared_data->debug_load_from_slot_requested) {
        uint32_t slot = shared_data->debug_target_slot;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Load from slot %u requested", slot);
        
        if (state_manager_initialized && slot < 8) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Attempting to load from slot %u (occupied: %s)", slot, slot_occupied[slot] ? "YES" : "NO");
            if (LoadStateFromSlot(slot)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: State loaded from slot %u successfully", slot);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Failed to load state from slot %u", slot);
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Invalid slot %u or state manager not initialized (initialized: %s)", slot, state_manager_initialized ? "YES" : "NO");
        }
        
        shared_data->debug_load_from_slot_requested = false;
    }
    
    last_processed_command_id = shared_data->debug_command_id;
}

// Configure network session based on mode
bool ConfigureNetworkMode(bool online_mode, bool host_mode) {
    is_online_mode = online_mode;
    is_host = host_mode;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network mode configured - Online: %s, Host: %s", 
                online_mode ? "YES" : "NO", host_mode ? "YES" : "NO");
    return true;
}

// Initialize GekkoNet session for rollback netcode using LocalNetworkAdapter
bool InitializeGekkoNet() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** INSIDE InitializeGekkoNet FUNCTION (NEW INDEPENDENT SESSION APPROACH) ***");
    
    // Determine our role from shared memory (HOST or GUEST)
    LocalNetworkAdapter::Role adapter_role = LocalNetworkAdapter::HOST;
    uint8_t player_index = 0;
    
    if (shared_memory_data) {
        SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
        uint8_t session_role = shared_data->session_role;
        player_index = shared_data->player_index;
        
        adapter_role = (session_role == 0) ? LocalNetworkAdapter::HOST : LocalNetworkAdapter::GUEST;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Creating independent session as %s (Player %u)", 
                    adapter_role == LocalNetworkAdapter::HOST ? "HOST" : "GUEST", player_index);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: No shared memory available yet - defaulting to HOST role");
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Role will be updated when launcher sets configuration");
    }
    
    // Create LocalNetworkAdapter with our role
    local_adapter = new LocalNetworkAdapter(adapter_role);
    if (!local_adapter->Initialize()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to initialize LocalNetworkAdapter!");
        delete local_adapter;
        local_adapter = nullptr;
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: LocalNetworkAdapter initialized successfully as %s", 
                adapter_role == LocalNetworkAdapter::HOST ? "HOST" : "GUEST");
    
    // Create independent GekkoNet session
    if (!gekko_create(&gekko_session)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to create GekkoNet session!");
        delete local_adapter;
        local_adapter = nullptr;
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Independent GekkoNet session created successfully");
    
    // Set the LocalNetworkAdapter on the session
    gekko_net_adapter_set(gekko_session, local_adapter->GetAdapter());
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: LocalNetworkAdapter set on GekkoNet session");
    
    // Configure GekkoNet session
    GekkoConfig config;
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = 3;  // 3-frame window for smooth gameplay
    config.spectator_delay = 0;
    config.input_size = 2;              // 2 bytes per input frame (P1 + P2)
    config.state_size = 65536;          // 64KB state size for FM2K
    config.limited_saving = false;
    config.post_sync_joining = false;
    config.desync_detection = true;     // Enable desync detection
    
    gekko_start(gekko_session, &config);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session configured and started");
    
    // Add players to the session
    if (adapter_role == LocalNetworkAdapter::HOST) {
        // Host: Add local player as P1, remote player as P2
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Adding players - HOST mode");
        p1_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        p2_handle = gekko_add_actor(gekko_session, RemotePlayer, nullptr);
    } else {
        // Guest: Add remote player as P1, local player as P2
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Adding players - GUEST mode");
        p1_handle = gekko_add_actor(gekko_session, RemotePlayer, nullptr);
        p2_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
    }
    
    // Validate player handles
    if (p1_handle < 0 || p2_handle < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to add players! P1: %d, P2: %d", p1_handle, p2_handle);
        gekko_destroy(gekko_session);
        gekko_session = nullptr;
        delete local_adapter;
        local_adapter = nullptr;
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Players added successfully - P1: %d, P2: %d", p1_handle, p2_handle);
    
    // Set input delay (2 frames for stable rollback)
    if (adapter_role == LocalNetworkAdapter::HOST) {
        gekko_set_local_delay(gekko_session, p1_handle, 2);  // Local player (P1)
    } else {
        gekko_set_local_delay(gekko_session, p2_handle, 2);  // Local player (P2)
    }
    
    gekko_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialization complete with LocalNetworkAdapter!");
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
    
    // Process debug commands from launcher
    ProcessDebugCommands();
    
    // Log occasionally to debug input capture (reduced frequency to avoid spam)
    if (g_frame_counter % 600 == 0) {  // Log every 600 frames (6 seconds) instead of every frame
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Frame %u - Game frame: %u - P1: 0x%08X (addr valid: %s), P2: 0x%08X (addr valid: %s)", 
                 g_frame_counter, game_frame, p1_input, 
                 (!IsBadReadPtr(p1_input_ptr, sizeof(uint32_t))) ? "YES" : "NO",
                 p2_input,
                 (!IsBadReadPtr(p2_input_ptr, sizeof(uint32_t))) ? "YES" : "NO");
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
            
            // Save current state before processing GekkoNet updates (reduced frequency for performance)
            if (state_manager_initialized && (g_frame_counter % 8) == 0) {  // Save every 8 frames for rollback buffer
                SaveStateToBuffer(g_frame_counter);
            }
            
            // Auto-save to slot 0 if enabled (with proper enable/disable logic)
            if (shared_memory_data && state_manager_initialized) {
                SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
                if (shared_data->auto_save_enabled) {
                    // Only auto-save if enough frames have passed since last auto-save
                    if ((g_frame_counter - last_auto_save_frame) >= shared_data->auto_save_interval_frames) {
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Auto-save triggered at frame %u (interval: %u)", 
                                    g_frame_counter, shared_data->auto_save_interval_frames);
                        SaveStateToSlot(0, g_frame_counter);  // Auto-save always goes to slot 0
                        last_auto_save_frame = g_frame_counter;
                    }
                } else {
                    // Auto-save is disabled - log occasionally for debugging
                    if (g_frame_counter % 3000 == 0) {  // Log every 30 seconds when disabled
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Auto-save disabled at frame %u", g_frame_counter);
                    }
                }
            }
            
            // Process GekkoNet updates after adding inputs
            int update_count = 0;
            auto updates = gekko_update_session(gekko_session, &update_count);
            
            // Handle GekkoNet events (AdvanceEvent, SaveEvent, LoadEvent)
            if (updates && update_count > 0) {
                for (int i = 0; i < update_count; i++) {
                    auto* update = updates[i];
                    if (!update) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Null update at index %d", i);
                        continue;
                    }
                    
                    switch (update->type) {
                        case AdvanceEvent: {
                            // Game should advance one frame with predicted inputs
                            uint32_t target_frame = update->data.adv.frame;
                            uint32_t input_length = update->data.adv.input_len;
                            uint8_t* inputs = update->data.adv.inputs;
                            
                            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent to frame %u (inputs: %u bytes)", 
                                        target_frame, input_length);
                            
                            // Apply predicted inputs to game memory if available
                            if (inputs && input_length >= 2) {
                                uint8_t p1_predicted = inputs[0];
                                uint8_t p2_predicted = inputs[1];
                                
                                // Convert back to FM2K format and apply to game memory
                                uint32_t p1_fm2k = 0, p2_fm2k = 0;
                                if (p1_predicted & 0x01) p1_fm2k |= 0x01;  // left
                                if (p1_predicted & 0x02) p1_fm2k |= 0x02;  // right
                                if (p1_predicted & 0x04) p1_fm2k |= 0x04;  // up
                                if (p1_predicted & 0x08) p1_fm2k |= 0x08;  // down
                                if (p1_predicted & 0x10) p1_fm2k |= 0x10;  // button1
                                if (p1_predicted & 0x20) p1_fm2k |= 0x20;  // button2
                                if (p1_predicted & 0x40) p1_fm2k |= 0x40;  // button3
                                if (p1_predicted & 0x80) p1_fm2k |= 0x80;  // button4
                                
                                if (p2_predicted & 0x01) p2_fm2k |= 0x01;  // left
                                if (p2_predicted & 0x02) p2_fm2k |= 0x02;  // right
                                if (p2_predicted & 0x04) p2_fm2k |= 0x04;  // up
                                if (p2_predicted & 0x08) p2_fm2k |= 0x08;  // down
                                if (p2_predicted & 0x10) p2_fm2k |= 0x10;  // button1
                                if (p2_predicted & 0x20) p2_fm2k |= 0x20;  // button2
                                if (p2_predicted & 0x40) p2_fm2k |= 0x40;  // button3
                                if (p2_predicted & 0x80) p2_fm2k |= 0x80;  // button4
                                
                                // Write predicted inputs to game memory
                                uint32_t* p1_input_ptr = (uint32_t*)P1_INPUT_ADDR;
                                uint32_t* p2_input_ptr = (uint32_t*)P2_INPUT_ADDR;
                                if (p1_input_ptr && !IsBadWritePtr(p1_input_ptr, sizeof(uint32_t))) {
                                    *p1_input_ptr = p1_fm2k;
                                }
                                if (p2_input_ptr && !IsBadWritePtr(p2_input_ptr, sizeof(uint32_t))) {
                                    *p2_input_ptr = p2_fm2k;
                                }
                                
                                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Applied predicted inputs - P1: 0x%02X->0x%08X, P2: 0x%02X->0x%08X", 
                                            p1_predicted, p1_fm2k, p2_predicted, p2_fm2k);
                            }
                            break;
                        }
                        
                        case SaveEvent: {
                            // GekkoNet wants us to save the current state
                            uint32_t save_frame = update->data.save.frame;
                            uint32_t* checksum_ptr = update->data.save.checksum;
                            uint32_t* state_len_ptr = update->data.save.state_len;
                            uint8_t* state_ptr = update->data.save.state;
                            
                            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SaveEvent for frame %u", save_frame);
                            
                            if (state_manager_initialized && checksum_ptr && state_len_ptr && state_ptr) {
                                // Save state to GekkoNet's buffer
                                FM2K::State::GameState current_state;
                                if (SaveCoreStateBasic(&current_state, save_frame)) {
                                    // Calculate state size and checksum
                                    *state_len_ptr = sizeof(FM2K::State::GameState);
                                    *checksum_ptr = CalculateStateChecksum(&current_state);
                                    
                                    // Copy state data to GekkoNet buffer
                                    memcpy(state_ptr, &current_state, sizeof(FM2K::State::GameState));
                                    
                                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: State saved for frame %u (size: %u, checksum: 0x%08X)", 
                                                save_frame, *state_len_ptr, *checksum_ptr);
                                } else {
                                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Failed to save state for frame %u", save_frame);
                                }
                            }
                            break;
                        }
                        
                        case LoadEvent: {
                            // Rollback to specific frame
                            uint32_t target_frame = update->data.load.frame;
                            uint32_t state_length = update->data.load.state_len;
                            uint8_t* state_data = update->data.load.state;
                            
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: LoadEvent (rollback) to frame %u (current: %u)", 
                                       target_frame, g_frame_counter);
                            
                            if (state_manager_initialized && state_data && state_length == sizeof(FM2K::State::GameState)) {
                                // Load state from GekkoNet buffer
                                FM2K::State::GameState* loaded_state = reinterpret_cast<FM2K::State::GameState*>(state_data);
                                if (RestoreStateFromStruct(loaded_state, target_frame)) {
                                    g_frame_counter = target_frame;  // Update our frame counter
                                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Successfully rolled back to frame %u", target_frame);
                                } else {
                                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Failed to load state for frame %u", target_frame);
                                }
                            } else {
                                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Invalid rollback data for frame %u (state_len: %u)", 
                                           target_frame, state_length);
                            }
                            break;
                        }
                        
                        default:
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Unknown event type: %d", update->type);
                            break;
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
            std::string log_path = GetLogFilePath();
            FILE* log = fopen(log_path.c_str(), "w");
            if (log) {
                fprintf(log, "FM2K HOOK: DLL attached to process at %lu\n", GetTickCount());
                fprintf(log, "FM2K HOOK: About to initialize GekkoNet...\n");
                fflush(log);
                fclose(log);
            }
            
            // Initialize shared memory for configuration
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing shared memory...");
            if (!InitializeSharedMemory()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to initialize shared memory");
            }
            
            // Initialize state manager for rollback
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing state manager...");
            if (!InitializeStateManager()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to initialize state manager");
            }
            
            // Default to local mode (offline) - can be changed later via configuration
            ConfigureNetworkMode(false, false);
            
            // Wait a moment for launcher to set client role configuration
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Waiting for launcher to set client role...");
            Sleep(200);  // Give launcher time to set role
            
            // Initialize GekkoNet session directly in DLL
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: About to initialize GekkoNet...");
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Calling InitializeGekkoNet() now...");
            bool gekko_result = InitializeGekkoNet();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: InitializeGekkoNet returned");
            
            if (!gekko_result) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to initialize GekkoNet!");
                FILE* error_log = fopen(GetLogFilePath().c_str(), "a");
                if (error_log) {
                    fprintf(error_log, "ERROR FM2K HOOK: Failed to initialize GekkoNet!\n");
                    fflush(error_log);
                    fclose(error_log);
                }
                // Continue anyway - we can still hook without rollback
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialized successfully!");
                FILE* success_log = fopen(GetLogFilePath().c_str(), "a");
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
        
        // Cleanup LocalNetworkAdapter
        if (local_adapter) {
            local_adapter->Shutdown();
            delete local_adapter;
            local_adapter = nullptr;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: LocalNetworkAdapter cleaned up");
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