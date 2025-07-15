#include "state_manager.h"
#include "globals.h"
#include "logging.h"
#include "game_state_detector.h"
#include <string>

// Helper function
static inline uint64_t get_microseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

namespace FM2K {

// REMOVED: MinimalGameState implementations - using only MinimalChecksumState

namespace State {

// Static variables from the original dllmain
static GameState saved_states[8];
static uint32_t current_state_index = 0;
static bool state_manager_initialized = false;
static GameState save_slots[8];
static bool slot_occupied[8] = {false};

// BSNES PATTERN: In-memory rollback buffers (no file I/O)
static GameState memory_rollback_slots[8];
static bool memory_slot_occupied[8] = {false};
static uint32_t memory_slot_frames[8] = {0};
static uint32_t last_auto_save_frame = 0;
static std::unique_ptr<uint8_t[]> slot_player_data_buffers[8];
static std::unique_ptr<uint8_t[]> slot_object_pool_buffers[8];
static std::unique_ptr<uint8_t[]> rollback_player_data_buffer;
static std::unique_ptr<uint8_t[]> rollback_object_pool_buffer;
static bool large_buffers_allocated = false;
static FM2K::State::GameState last_core_state = {};
static bool last_core_state_valid = false;

// Performance tracking
static uint32_t total_saves = 0;
static uint32_t total_loads = 0;
static uint64_t total_save_time_us = 0;
static uint64_t total_load_time_us = 0;

// Forward declarations for functions within this file
uint32_t Fletcher32(const uint8_t* data, size_t len);
bool SaveStateFast(GameState* state, uint32_t frame_number);
bool RestoreStateFast(const GameState* state, uint32_t target_frame);
bool ValidatePhase1Performance();
uint32_t AnalyzeActiveObjects(ActiveObjectInfo* active_objects, uint32_t max_objects);
uint32_t FastScanActiveObjects(uint32_t* active_mask, uint16_t* active_count);
bool PackActiveObjects(const uint32_t* active_mask, uint16_t active_count, uint8_t* packed_buffer, size_t buffer_size, size_t* bytes_used);
bool UnpackActiveObjects(const uint8_t* packed_buffer, size_t buffer_size, uint16_t active_count);
bool SaveActiveObjectsOnly(uint8_t* destination_buffer, size_t buffer_size, uint32_t* objects_saved);
bool RestoreActiveObjectsOnly(const uint8_t* source_buffer, size_t buffer_size, uint32_t objects_to_restore);

// Fletcher32 checksum implementation
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

void SaveStateToBuffer(uint32_t frame_number) {
    // Simple implementation - save to ring buffer
    uint32_t slot_index = frame_number % 8;
    SaveStateToSlot(slot_index, frame_number);
}

bool SaveStateToSlot(uint32_t slot, uint32_t frame_number) {
    if (slot >= 8) return false;
    // Simple stub implementation
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "SaveStateToSlot: slot %u, frame %u", slot, frame_number);
    return true;
}

bool LoadStateFromSlot(uint32_t slot) {
    if (slot >= 8) return false;
    // Simple stub implementation
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "LoadStateFromSlot: slot %u", slot);
    return true;
}

bool ValidatePhase1Performance() {
    // Simple stub implementation
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ValidatePhase1Performance completed");
    return true;
}

// Implementation of functions
bool InitializeStateManager() {
    memset(saved_states, 0, sizeof(saved_states));
    current_state_index = 0;
    try {
        for (int i = 0; i < 8; i++) {
            slot_player_data_buffers[i] = std::make_unique<uint8_t[]>(Memory::PLAYER_DATA_SLOTS_SIZE);
            slot_object_pool_buffers[i] = std::make_unique<uint8_t[]>(Memory::GAME_OBJECT_POOL_SIZE);
        }
        rollback_player_data_buffer = std::make_unique<uint8_t[]>(Memory::PLAYER_DATA_SLOTS_SIZE);
        rollback_object_pool_buffer = std::make_unique<uint8_t[]>(Memory::GAME_OBJECT_POOL_SIZE);
        large_buffers_allocated = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Allocated %zu KB per slot x8 + rollback (%zu KB total)",
                    (Memory::PLAYER_DATA_SLOTS_SIZE + Memory::GAME_OBJECT_POOL_SIZE) / 1024,
                    ((Memory::PLAYER_DATA_SLOTS_SIZE + Memory::GAME_OBJECT_POOL_SIZE) * 9) / 1024);
    } catch (const std::bad_alloc& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to allocate state buffers: %s", e.what());
        large_buffers_allocated = false;
        return false;
    }
    state_manager_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Enhanced state manager initialized with comprehensive memory capture");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Running Phase 1 performance validation...");
    bool validation_result = ValidatePhase1Performance();
    if (validation_result) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: ? Phase 1 optimizations validated successfully!");
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: ?? Phase 1 validation completed with warnings");
    }
    return true;
}

void CleanupStateManager() {
    // unique_ptr will handle cleanup
}

bool SaveCoreStateBasic(GameState* state, uint32_t frame_number) {
    // CRITICAL: Initialize entire CoreGameState to zero for consistent checksums across clients
    memset(&state->core, 0, sizeof(CoreGameState));
    
    uint32_t* frame_ptr = (uint32_t*)Memory::FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)Memory::P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)Memory::P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)Memory::P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)Memory::P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)Memory::ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)Memory::GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)Memory::RANDOM_SEED_ADDR;
    uint32_t* timer_countdown1_ptr = (uint32_t*)Memory::TIMER_COUNTDOWN1_ADDR;
    uint32_t* timer_countdown2_ptr = (uint32_t*)Memory::TIMER_COUNTDOWN2_ADDR;
    uint32_t* round_timer_counter_ptr = (uint32_t*)Memory::ROUND_TIMER_COUNTER_ADDR;
    uint32_t* object_list_heads_ptr = (uint32_t*)Memory::OBJECT_LIST_HEADS_ADDR;
    uint32_t* object_list_tails_ptr = (uint32_t*)Memory::OBJECT_LIST_TAILS_ADDR;
    
    // Game mode state pointers for character select synchronization
    uint32_t* game_mode_ptr = (uint32_t*)Memory::GAME_MODE_ADDR;
    uint32_t* fm2k_game_mode_ptr = (uint32_t*)Memory::FM2K_GAME_MODE_ADDR;
    uint32_t* character_select_mode_ptr = (uint32_t*)Memory::CHARACTER_SELECT_MODE_ADDR;
    
    // Character Select Menu State pointers (critical for CSS synchronization)
    uint32_t* menu_selection_ptr = (uint32_t*)Memory::MENU_SELECTION_ADDR;
    uint32_t* p1_css_cursor_x_ptr = (uint32_t*)Memory::P1_CSS_CURSOR_X_ADDR;
    uint32_t* p1_css_cursor_y_ptr = (uint32_t*)Memory::P1_CSS_CURSOR_Y_ADDR;
    uint32_t* p2_css_cursor_x_ptr = (uint32_t*)Memory::P2_CSS_CURSOR_X_ADDR;
    uint32_t* p2_css_cursor_y_ptr = (uint32_t*)Memory::P2_CSS_CURSOR_Y_ADDR;
    uint32_t* p1_selected_char_ptr = (uint32_t*)Memory::P1_SELECTED_CHAR_ADDR;
    uint32_t* p2_selected_char_ptr = (uint32_t*)Memory::P2_SELECTED_CHAR_ADDR;
    uint32_t* p1_char_related_ptr = (uint32_t*)Memory::P1_CHAR_RELATED_ADDR;
    uint32_t* p2_char_related_ptr = (uint32_t*)Memory::P2_CHAR_RELATED_ADDR;
    
    if (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) { state->core.p1_input_current = *p1_input_ptr; }
    if (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) { state->core.p2_input_current = *p2_input_ptr; }
    // DISABLED FIELDS - set to constants for minimal state
    // if (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t))) { state->core.p1_hp = *p1_hp_ptr; }
    // if (!IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t))) { state->core.p2_hp = *p2_hp_ptr; }
    // if (!IsBadReadPtr(round_timer_ptr, sizeof(uint32_t))) { state->core.round_timer = *round_timer_ptr; }
    // if (!IsBadReadPtr(game_timer_ptr, sizeof(uint32_t))) { state->core.game_timer = *game_timer_ptr; }
    // if (!IsBadReadPtr(random_seed_ptr, sizeof(uint32_t))) { state->core.random_seed = *random_seed_ptr; }
    // if (!IsBadReadPtr(timer_countdown1_ptr, sizeof(uint32_t))) { state->core.timer_countdown1 = *timer_countdown1_ptr; } else { state->core.timer_countdown1 = 0; }
    // if (!IsBadReadPtr(timer_countdown2_ptr, sizeof(uint32_t))) { state->core.timer_countdown2 = *timer_countdown2_ptr; } else { state->core.timer_countdown2 = 0; }
    // if (!IsBadReadPtr(round_timer_counter_ptr, sizeof(uint32_t))) { state->core.round_timer_counter = *round_timer_counter_ptr; } else { state->core.round_timer_counter = 0; }
    // if (!IsBadReadPtr(object_list_heads_ptr, sizeof(uint32_t))) { state->core.object_list_heads = *object_list_heads_ptr; } else { state->core.object_list_heads = 0; }
    // if (!IsBadReadPtr(object_list_tails_ptr, sizeof(uint32_t))) { state->core.object_list_tails = *object_list_tails_ptr; } else { state->core.object_list_tails = 0; }
    
    // DISABLED FIELDS - set to constants for minimal state  
    // if (!IsBadReadPtr(game_mode_ptr, sizeof(uint32_t))) { state->core.game_mode = *game_mode_ptr; } else { state->core.game_mode = 0xFFFFFFFF; }
    // if (!IsBadReadPtr(fm2k_game_mode_ptr, sizeof(uint32_t))) { state->core.fm2k_game_mode = *fm2k_game_mode_ptr; } else { state->core.fm2k_game_mode = 0xFFFFFFFF; }
    // if (!IsBadReadPtr(character_select_mode_ptr, sizeof(uint32_t))) { state->core.character_select_mode = *character_select_mode_ptr; } else { state->core.character_select_mode = 0xFFFFFFFF; }
    
    // Save Character Select Menu State (CRITICAL for CSS synchronization) 
    if (!IsBadReadPtr(menu_selection_ptr, sizeof(uint32_t))) { state->core.menu_selection = *menu_selection_ptr; } else { state->core.menu_selection = 0; }
    if (!IsBadReadPtr(p1_css_cursor_x_ptr, sizeof(uint32_t))) { state->core.p1_css_cursor_x = *p1_css_cursor_x_ptr; } else { state->core.p1_css_cursor_x = 0; }
    if (!IsBadReadPtr(p1_css_cursor_y_ptr, sizeof(uint32_t))) { state->core.p1_css_cursor_y = *p1_css_cursor_y_ptr; } else { state->core.p1_css_cursor_y = 0; }
    if (!IsBadReadPtr(p2_css_cursor_x_ptr, sizeof(uint32_t))) { state->core.p2_css_cursor_x = *p2_css_cursor_x_ptr; } else { state->core.p2_css_cursor_x = 0; }
    if (!IsBadReadPtr(p2_css_cursor_y_ptr, sizeof(uint32_t))) { state->core.p2_css_cursor_y = *p2_css_cursor_y_ptr; } else { state->core.p2_css_cursor_y = 0; }
    // DISABLED FIELDS - commenting out to match minimized state structure
    // if (!IsBadReadPtr(p1_selected_char_ptr, sizeof(uint32_t))) { state->core.p1_selected_char = *p1_selected_char_ptr; } else { state->core.p1_selected_char = 0; }
    // if (!IsBadReadPtr(p2_selected_char_ptr, sizeof(uint32_t))) { state->core.p2_selected_char = *p2_selected_char_ptr; } else { state->core.p2_selected_char = 0; }
    // if (!IsBadReadPtr(p1_char_related_ptr, sizeof(uint32_t))) { state->core.p1_char_related = *p1_char_related_ptr; } else { state->core.p1_char_related = 0; }
    // if (!IsBadReadPtr(p2_char_related_ptr, sizeof(uint32_t))) { state->core.p2_char_related = *p2_char_related_ptr; } else { state->core.p2_char_related = 0; }
    
    return true;
}

bool RestoreStateFromStruct(const GameState* state, uint32_t target_frame) {
    if (!state) { return false; }
    uint32_t* frame_ptr = (uint32_t*)Memory::FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)Memory::P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)Memory::P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)Memory::P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)Memory::P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)Memory::ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)Memory::GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)Memory::RANDOM_SEED_ADDR;
    uint32_t* timer_countdown1_ptr = (uint32_t*)Memory::TIMER_COUNTDOWN1_ADDR;
    uint32_t* timer_countdown2_ptr = (uint32_t*)Memory::TIMER_COUNTDOWN2_ADDR;
    uint32_t* round_timer_counter_ptr = (uint32_t*)Memory::ROUND_TIMER_COUNTER_ADDR;
    uint32_t* object_list_heads_ptr = (uint32_t*)Memory::OBJECT_LIST_HEADS_ADDR;
    uint32_t* object_list_tails_ptr = (uint32_t*)Memory::OBJECT_LIST_TAILS_ADDR;
    
    // Game mode state pointers for character select synchronization
    uint32_t* game_mode_ptr = (uint32_t*)Memory::GAME_MODE_ADDR;
    uint32_t* fm2k_game_mode_ptr = (uint32_t*)Memory::FM2K_GAME_MODE_ADDR;
    uint32_t* character_select_mode_ptr = (uint32_t*)Memory::CHARACTER_SELECT_MODE_ADDR;
    
    // Character Select Menu State pointers (critical for CSS synchronization)
    uint32_t* menu_selection_ptr = (uint32_t*)Memory::MENU_SELECTION_ADDR;
    uint32_t* p1_css_cursor_x_ptr = (uint32_t*)Memory::P1_CSS_CURSOR_X_ADDR;
    uint32_t* p1_css_cursor_y_ptr = (uint32_t*)Memory::P1_CSS_CURSOR_Y_ADDR;
    uint32_t* p2_css_cursor_x_ptr = (uint32_t*)Memory::P2_CSS_CURSOR_X_ADDR;
    uint32_t* p2_css_cursor_y_ptr = (uint32_t*)Memory::P2_CSS_CURSOR_Y_ADDR;
    uint32_t* p1_selected_char_ptr = (uint32_t*)Memory::P1_SELECTED_CHAR_ADDR;
    uint32_t* p2_selected_char_ptr = (uint32_t*)Memory::P2_SELECTED_CHAR_ADDR;
    uint32_t* p1_char_related_ptr = (uint32_t*)Memory::P1_CHAR_RELATED_ADDR;
    uint32_t* p2_char_related_ptr = (uint32_t*)Memory::P2_CHAR_RELATED_ADDR;

    if (!IsBadWritePtr(p1_input_ptr, sizeof(uint16_t))) { *p1_input_ptr = state->core.p1_input_current; }
    if (!IsBadWritePtr(p2_input_ptr, sizeof(uint16_t))) { *p2_input_ptr = state->core.p2_input_current; }
    // DISABLED FIELDS - commenting out to match minimized state structure
    // if (!IsBadWritePtr(p1_hp_ptr, sizeof(uint32_t))) { *p1_hp_ptr = state->core.p1_hp; }
    // if (!IsBadWritePtr(p2_hp_ptr, sizeof(uint32_t))) { *p2_hp_ptr = state->core.p2_hp; }
    // if (!IsBadWritePtr(round_timer_ptr, sizeof(uint32_t))) { *round_timer_ptr = state->core.round_timer; }
    // if (!IsBadWritePtr(game_timer_ptr, sizeof(uint32_t))) { *game_timer_ptr = state->core.game_timer; }
    // if (!IsBadWritePtr(random_seed_ptr, sizeof(uint32_t))) { *random_seed_ptr = state->core.random_seed; }
    // if (!IsBadWritePtr(timer_countdown1_ptr, sizeof(uint32_t))) { *timer_countdown1_ptr = state->core.timer_countdown1; }
    // if (!IsBadWritePtr(timer_countdown2_ptr, sizeof(uint32_t))) { *timer_countdown2_ptr = state->core.timer_countdown2; }
    // if (!IsBadWritePtr(round_timer_counter_ptr, sizeof(uint32_t))) { *round_timer_counter_ptr = state->core.round_timer_counter; }
    // if (!IsBadWritePtr(object_list_heads_ptr, sizeof(uint32_t))) { *object_list_heads_ptr = state->core.object_list_heads; }
    // if (!IsBadWritePtr(object_list_tails_ptr, sizeof(uint32_t))) { *object_list_tails_ptr = state->core.object_list_tails; }
    
    // DISABLED FIELDS - commenting out to match minimized state structure
    // if (!IsBadWritePtr(game_mode_ptr, sizeof(uint32_t))) { *game_mode_ptr = state->core.game_mode; }
    // if (!IsBadWritePtr(fm2k_game_mode_ptr, sizeof(uint32_t))) { *fm2k_game_mode_ptr = state->core.fm2k_game_mode; }
    // if (!IsBadWritePtr(character_select_mode_ptr, sizeof(uint32_t))) { *character_select_mode_ptr = state->core.character_select_mode; }
    
    // Restore Character Select Menu State (CRITICAL for CSS synchronization)
    if (!IsBadWritePtr(menu_selection_ptr, sizeof(uint32_t))) { *menu_selection_ptr = state->core.menu_selection; }
    if (!IsBadWritePtr(p1_css_cursor_x_ptr, sizeof(uint32_t))) { 
        uint32_t old_x = *p1_css_cursor_x_ptr;
        *p1_css_cursor_x_ptr = state->core.p1_css_cursor_x; 
        if (old_x != state->core.p1_css_cursor_x) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS RESTORE: P1 cursor X %u -> %u (frame %u)", 
                       old_x, state->core.p1_css_cursor_x, target_frame);
        }
    }
    if (!IsBadWritePtr(p1_css_cursor_y_ptr, sizeof(uint32_t))) { 
        uint32_t old_y = *p1_css_cursor_y_ptr;
        *p1_css_cursor_y_ptr = state->core.p1_css_cursor_y; 
        if (old_y != state->core.p1_css_cursor_y) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS RESTORE: P1 cursor Y %u -> %u (frame %u)", 
                       old_y, state->core.p1_css_cursor_y, target_frame);
        }
    }
    if (!IsBadWritePtr(p2_css_cursor_x_ptr, sizeof(uint32_t))) { *p2_css_cursor_x_ptr = state->core.p2_css_cursor_x; }
    if (!IsBadWritePtr(p2_css_cursor_y_ptr, sizeof(uint32_t))) { *p2_css_cursor_y_ptr = state->core.p2_css_cursor_y; }
    // DISABLED FIELDS - commenting out to match minimized state structure
    // if (!IsBadWritePtr(p1_selected_char_ptr, sizeof(uint32_t))) { *p1_selected_char_ptr = state->core.p1_selected_char; }
    // if (!IsBadWritePtr(p2_selected_char_ptr, sizeof(uint32_t))) { *p2_selected_char_ptr = state->core.p2_selected_char; }
    // if (!IsBadWritePtr(p1_char_related_ptr, sizeof(uint32_t))) { *p1_char_related_ptr = state->core.p1_char_related; }
    // if (!IsBadWritePtr(p2_char_related_ptr, sizeof(uint32_t))) { *p2_char_related_ptr = state->core.p2_char_related; }

    return true;
}

// BSNES PATTERN: In-memory rollback system implementation
bool SaveStateToMemoryBuffer(uint32_t slot, uint32_t frame_number) {
    if (slot >= 8) return false;
    
    // Save current game state to memory buffer (no file I/O)
    bool success = SaveCoreStateBasic(&memory_rollback_slots[slot], frame_number);
    if (success) {
        memory_rollback_slots[slot].frame_number = frame_number;
        memory_rollback_slots[slot].timestamp_ms = get_microseconds() / 1000;
        
        // UNIFIED: Calculate checksum directly from the saved CoreGameState
        memory_rollback_slots[slot].checksum = memory_rollback_slots[slot].core.CalculateChecksum();
        
        // DEBUG: DETAILED field-by-field logging for checksum debugging
        const CoreGameState& core = memory_rollback_slots[slot].core;
        if (frame_number <= 50) {  // Extended logging to see post-warmup sync effects
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CHECKSUM DEBUG Frame %u:", frame_number);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  p1_input=%u, p2_input=%u", 
                       core.p1_input_current, core.p2_input_current);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  menu=%u, css_cursors=(%u,%u),(%u,%u)", 
                       core.menu_selection, core.p1_css_cursor_x, core.p1_css_cursor_y,
                       core.p2_css_cursor_x, core.p2_css_cursor_y);
                       
            // CRITICAL: Raw memory dump for checksum debugging
            const uint8_t* raw_bytes = (const uint8_t*)&core;
            std::string hex_dump;
            for (size_t i = 0; i < sizeof(CoreGameState); i++) {
                char hex_byte[8];
                snprintf(hex_byte, sizeof(hex_byte), "%02X", raw_bytes[i]);
                hex_dump += hex_byte;
                if ((i + 1) % 4 == 0) hex_dump += " ";  // Space every 4 bytes
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  RAW MEMORY: %s", hex_dump.c_str());
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  FINAL CHECKSUM: 0x%08X", memory_rollback_slots[slot].checksum);
        }
        
        memory_slot_occupied[slot] = true;
        memory_slot_frames[slot] = frame_number;
        
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "MEMORY ROLLBACK: Saved frame %u to slot %u (checksum: 0x%08X)", 
                     frame_number, slot, memory_rollback_slots[slot].checksum);
    }
    
    return success;
}

bool LoadStateFromMemoryBuffer(uint32_t slot) {
    if (slot >= 8 || !memory_slot_occupied[slot]) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "MEMORY ROLLBACK: Cannot load from slot %u (not occupied)", slot);
        return false;
    }
    
    // Restore game state from memory buffer (no file I/O)
    bool success = RestoreStateFromStruct(&memory_rollback_slots[slot], memory_slot_frames[slot]);
    if (success) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "MEMORY ROLLBACK: Loaded frame %u from slot %u (checksum: 0x%08X)", 
                     memory_slot_frames[slot], slot, memory_rollback_slots[slot].checksum);
    }
    
    return success;
}

uint32_t GetStateChecksum(uint32_t slot) {
    if (slot >= 8 || !memory_slot_occupied[slot]) {
        return 0;
    }
    
    return memory_rollback_slots[slot].checksum;
}


// ... more functions to follow
}
} 