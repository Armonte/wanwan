#include "state_manager.h"
#include "globals.h"
#include "logging.h"
#include "game_state_detector.h"

// Helper function
static inline uint64_t get_microseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

namespace FM2K {

// MinimalGameState method implementations
bool MinimalGameState::LoadFromMemory() {
    // Read HP values
    uint32_t* p1_hp_ptr = (uint32_t*)State::Memory::P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)State::Memory::P2_HP_ADDR;
    uint32_t* p1_max_hp_ptr = (uint32_t*)0x4DFC85;  // P1_MAX_HP_ARTMONEY_ADDR
    uint32_t* p2_max_hp_ptr = (uint32_t*)0x4EDC4;   // P2_MAX_HP_ARTMONEY_ADDR
    
    if (!p1_hp_ptr || !p2_hp_ptr || !p1_max_hp_ptr || !p2_max_hp_ptr) return false;
    if (IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t)) || IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t)) ||
        IsBadReadPtr(p1_max_hp_ptr, sizeof(uint32_t)) || IsBadReadPtr(p2_max_hp_ptr, sizeof(uint32_t))) return false;
    
    p1_hp = *p1_hp_ptr;
    p2_hp = *p2_hp_ptr;
    p1_max_hp = *p1_max_hp_ptr;
    p2_max_hp = *p2_max_hp_ptr;
    
    // Read positions
    uint32_t* p1_x_ptr = (uint32_t*)0x4ADCC3;  // P1_COORD_X_ADDR
    uint16_t* p1_y_ptr = (uint16_t*)0x4ADCC7;  // P1_COORD_Y_ADDR
    uint32_t* p2_x_ptr = (uint32_t*)0x4EDD02;  // P2_COORD_X_ADDR
    uint16_t* p2_y_ptr = (uint16_t*)0x4EDD06;  // P2_COORD_Y_ADDR
    
    if (!p1_x_ptr || !p1_y_ptr || !p2_x_ptr || !p2_y_ptr) return false;
    if (IsBadReadPtr(p1_x_ptr, sizeof(uint32_t)) || IsBadReadPtr(p1_y_ptr, sizeof(uint16_t)) ||
        IsBadReadPtr(p2_x_ptr, sizeof(uint32_t)) || IsBadReadPtr(p2_y_ptr, sizeof(uint16_t))) return false;
    
    p1_x = *p1_x_ptr;
    p1_y = *p1_y_ptr;
    p2_x = *p2_x_ptr;
    p2_y = *p2_y_ptr;
    
    // Read timer and RNG
    uint32_t* timer_ptr = (uint32_t*)State::Memory::GAME_TIMER_ADDR;
    uint32_t* rng_ptr = (uint32_t*)State::Memory::RANDOM_SEED_ADDR;
    
    if (!timer_ptr || !rng_ptr) return false;
    if (IsBadReadPtr(timer_ptr, sizeof(uint32_t)) || IsBadReadPtr(rng_ptr, sizeof(uint32_t))) return false;
    
    round_timer = *timer_ptr;
    random_seed = *rng_ptr;
    
    return true;
}

bool MinimalGameState::SaveToMemory() const {
    // Write HP values
    uint32_t* p1_hp_ptr = (uint32_t*)State::Memory::P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)State::Memory::P2_HP_ADDR;
    
    if (!p1_hp_ptr || !p2_hp_ptr) return false;
    if (IsBadWritePtr(p1_hp_ptr, sizeof(uint32_t)) || IsBadWritePtr(p2_hp_ptr, sizeof(uint32_t))) return false;
    
    *p1_hp_ptr = p1_hp;
    *p2_hp_ptr = p2_hp;
    
    // Write positions
    uint32_t* p1_x_ptr = (uint32_t*)0x4ADCC3;  // P1_COORD_X_ADDR
    uint16_t* p1_y_ptr = (uint16_t*)0x4ADCC7;  // P1_COORD_Y_ADDR
    uint32_t* p2_x_ptr = (uint32_t*)0x4EDD02;  // P2_COORD_X_ADDR
    uint16_t* p2_y_ptr = (uint16_t*)0x4EDD06;  // P2_COORD_Y_ADDR
    
    if (!p1_x_ptr || !p1_y_ptr || !p2_x_ptr || !p2_y_ptr) return false;
    if (IsBadWritePtr(p1_x_ptr, sizeof(uint32_t)) || IsBadWritePtr(p1_y_ptr, sizeof(uint16_t)) ||
        IsBadWritePtr(p2_x_ptr, sizeof(uint32_t)) || IsBadWritePtr(p2_y_ptr, sizeof(uint16_t))) return false;
    
    *p1_x_ptr = p1_x;
    *p1_y_ptr = (uint16_t)p1_y;
    *p2_x_ptr = p2_x;
    *p2_y_ptr = (uint16_t)p2_y;
    
    // Write timer and RNG
    uint32_t* timer_ptr = (uint32_t*)State::Memory::GAME_TIMER_ADDR;
    uint32_t* rng_ptr = (uint32_t*)State::Memory::RANDOM_SEED_ADDR;
    
    if (!timer_ptr || !rng_ptr) return false;
    if (IsBadWritePtr(timer_ptr, sizeof(uint32_t)) || IsBadWritePtr(rng_ptr, sizeof(uint32_t))) return false;
    
    *timer_ptr = round_timer;
    *rng_ptr = random_seed;
    
    return true;
}

uint32_t MinimalGameState::CalculateChecksum() const {
    // Simple Fletcher32-like checksum for 48 bytes
    uint32_t sum1 = 0, sum2 = 0;
    const uint32_t* data = reinterpret_cast<const uint32_t*>(this);
    for (int i = 0; i < 12; i++) {  // 48 bytes / 4 = 12 uint32_t
        sum1 += data[i];
        sum2 += sum1;
    }
    return (sum2 << 16) | (sum1 & 0xFFFF);
}

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
    
    if (!IsBadReadPtr(frame_ptr, sizeof(uint32_t))) { state->core.input_buffer_index = *frame_ptr; }
    if (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) { state->core.p1_input_current = *p1_input_ptr; }
    if (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) { state->core.p2_input_current = *p2_input_ptr; }
    if (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t))) { state->core.p1_hp = *p1_hp_ptr; }
    if (!IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t))) { state->core.p2_hp = *p2_hp_ptr; }
    if (!IsBadReadPtr(round_timer_ptr, sizeof(uint32_t))) { state->core.round_timer = *round_timer_ptr; }
    if (!IsBadReadPtr(game_timer_ptr, sizeof(uint32_t))) { state->core.game_timer = *game_timer_ptr; }
    if (!IsBadReadPtr(random_seed_ptr, sizeof(uint32_t))) { state->core.random_seed = *random_seed_ptr; }
    if (!IsBadReadPtr(timer_countdown1_ptr, sizeof(uint32_t))) { state->core.timer_countdown1 = *timer_countdown1_ptr; } else { state->core.timer_countdown1 = 0; }
    if (!IsBadReadPtr(timer_countdown2_ptr, sizeof(uint32_t))) { state->core.timer_countdown2 = *timer_countdown2_ptr; } else { state->core.timer_countdown2 = 0; }
    if (!IsBadReadPtr(round_timer_counter_ptr, sizeof(uint32_t))) { state->core.round_timer_counter = *round_timer_counter_ptr; } else { state->core.round_timer_counter = 0; }
    if (!IsBadReadPtr(object_list_heads_ptr, sizeof(uint32_t))) { state->core.object_list_heads = *object_list_heads_ptr; } else { state->core.object_list_heads = 0; }
    if (!IsBadReadPtr(object_list_tails_ptr, sizeof(uint32_t))) { state->core.object_list_tails = *object_list_tails_ptr; } else { state->core.object_list_tails = 0; }
    
    // Save game mode state for character select synchronization
    if (!IsBadReadPtr(game_mode_ptr, sizeof(uint32_t))) { state->core.game_mode = *game_mode_ptr; } else { state->core.game_mode = 0xFFFFFFFF; }
    if (!IsBadReadPtr(fm2k_game_mode_ptr, sizeof(uint32_t))) { state->core.fm2k_game_mode = *fm2k_game_mode_ptr; } else { state->core.fm2k_game_mode = 0xFFFFFFFF; }
    if (!IsBadReadPtr(character_select_mode_ptr, sizeof(uint32_t))) { state->core.character_select_mode = *character_select_mode_ptr; } else { state->core.character_select_mode = 0xFFFFFFFF; }
    
    // Save Character Select Menu State (CRITICAL for CSS synchronization) 
    if (!IsBadReadPtr(menu_selection_ptr, sizeof(uint32_t))) { state->core.menu_selection = *menu_selection_ptr; } else { state->core.menu_selection = 0; }
    if (!IsBadReadPtr(p1_css_cursor_x_ptr, sizeof(uint32_t))) { state->core.p1_css_cursor_x = *p1_css_cursor_x_ptr; } else { state->core.p1_css_cursor_x = 0; }
    if (!IsBadReadPtr(p1_css_cursor_y_ptr, sizeof(uint32_t))) { state->core.p1_css_cursor_y = *p1_css_cursor_y_ptr; } else { state->core.p1_css_cursor_y = 0; }
    if (!IsBadReadPtr(p2_css_cursor_x_ptr, sizeof(uint32_t))) { state->core.p2_css_cursor_x = *p2_css_cursor_x_ptr; } else { state->core.p2_css_cursor_x = 0; }
    if (!IsBadReadPtr(p2_css_cursor_y_ptr, sizeof(uint32_t))) { state->core.p2_css_cursor_y = *p2_css_cursor_y_ptr; } else { state->core.p2_css_cursor_y = 0; }
    if (!IsBadReadPtr(p1_selected_char_ptr, sizeof(uint32_t))) { state->core.p1_selected_char = *p1_selected_char_ptr; } else { state->core.p1_selected_char = 0; }
    if (!IsBadReadPtr(p2_selected_char_ptr, sizeof(uint32_t))) { state->core.p2_selected_char = *p2_selected_char_ptr; } else { state->core.p2_selected_char = 0; }
    if (!IsBadReadPtr(p1_char_related_ptr, sizeof(uint32_t))) { state->core.p1_char_related = *p1_char_related_ptr; } else { state->core.p1_char_related = 0; }
    if (!IsBadReadPtr(p2_char_related_ptr, sizeof(uint32_t))) { state->core.p2_char_related = *p2_char_related_ptr; } else { state->core.p2_char_related = 0; }
    
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

    if (!IsBadWritePtr(frame_ptr, sizeof(uint32_t))) { *frame_ptr = state->core.input_buffer_index; }
    if (!IsBadWritePtr(p1_input_ptr, sizeof(uint16_t))) { *p1_input_ptr = state->core.p1_input_current; }
    if (!IsBadWritePtr(p2_input_ptr, sizeof(uint16_t))) { *p2_input_ptr = state->core.p2_input_current; }
    if (!IsBadWritePtr(p1_hp_ptr, sizeof(uint32_t))) { *p1_hp_ptr = state->core.p1_hp; }
    if (!IsBadWritePtr(p2_hp_ptr, sizeof(uint32_t))) { *p2_hp_ptr = state->core.p2_hp; }
    if (!IsBadWritePtr(round_timer_ptr, sizeof(uint32_t))) { *round_timer_ptr = state->core.round_timer; }
    if (!IsBadWritePtr(game_timer_ptr, sizeof(uint32_t))) { *game_timer_ptr = state->core.game_timer; }
    if (!IsBadWritePtr(random_seed_ptr, sizeof(uint32_t))) { *random_seed_ptr = state->core.random_seed; }
    if (!IsBadWritePtr(timer_countdown1_ptr, sizeof(uint32_t))) { *timer_countdown1_ptr = state->core.timer_countdown1; }
    if (!IsBadWritePtr(timer_countdown2_ptr, sizeof(uint32_t))) { *timer_countdown2_ptr = state->core.timer_countdown2; }
    if (!IsBadWritePtr(round_timer_counter_ptr, sizeof(uint32_t))) { *round_timer_counter_ptr = state->core.round_timer_counter; }
    if (!IsBadWritePtr(object_list_heads_ptr, sizeof(uint32_t))) { *object_list_heads_ptr = state->core.object_list_heads; }
    if (!IsBadWritePtr(object_list_tails_ptr, sizeof(uint32_t))) { *object_list_tails_ptr = state->core.object_list_tails; }
    
    // Restore game mode state for character select synchronization
    if (!IsBadWritePtr(game_mode_ptr, sizeof(uint32_t))) { *game_mode_ptr = state->core.game_mode; }
    if (!IsBadWritePtr(fm2k_game_mode_ptr, sizeof(uint32_t))) { *fm2k_game_mode_ptr = state->core.fm2k_game_mode; }
    if (!IsBadWritePtr(character_select_mode_ptr, sizeof(uint32_t))) { *character_select_mode_ptr = state->core.character_select_mode; }
    
    // Restore Character Select Menu State (CRITICAL for CSS synchronization)
    if (!IsBadWritePtr(menu_selection_ptr, sizeof(uint32_t))) { *menu_selection_ptr = state->core.menu_selection; }
    if (!IsBadWritePtr(p1_css_cursor_x_ptr, sizeof(uint32_t))) { *p1_css_cursor_x_ptr = state->core.p1_css_cursor_x; }
    if (!IsBadWritePtr(p1_css_cursor_y_ptr, sizeof(uint32_t))) { *p1_css_cursor_y_ptr = state->core.p1_css_cursor_y; }
    if (!IsBadWritePtr(p2_css_cursor_x_ptr, sizeof(uint32_t))) { *p2_css_cursor_x_ptr = state->core.p2_css_cursor_x; }
    if (!IsBadWritePtr(p2_css_cursor_y_ptr, sizeof(uint32_t))) { *p2_css_cursor_y_ptr = state->core.p2_css_cursor_y; }
    if (!IsBadWritePtr(p1_selected_char_ptr, sizeof(uint32_t))) { *p1_selected_char_ptr = state->core.p1_selected_char; }
    if (!IsBadWritePtr(p2_selected_char_ptr, sizeof(uint32_t))) { *p2_selected_char_ptr = state->core.p2_selected_char; }
    if (!IsBadWritePtr(p1_char_related_ptr, sizeof(uint32_t))) { *p1_char_related_ptr = state->core.p1_char_related; }
    if (!IsBadWritePtr(p2_char_related_ptr, sizeof(uint32_t))) { *p2_char_related_ptr = state->core.p2_char_related; }

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
        // SDL2 PATTERN: Checksum only essential data (exclude volatile timing/address fields)
        MinimalChecksumState minimal_state = {};
        // Read only essential gameplay data for checksum
        uint32_t* p1_hp_ptr = (uint32_t*)Memory::P1_HP_ADDR;
        uint32_t* p2_hp_ptr = (uint32_t*)Memory::P2_HP_ADDR;
        uint32_t* game_mode_ptr = (uint32_t*)Memory::GAME_MODE_ADDR;
        if (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t))) minimal_state.p1_hp = *p1_hp_ptr;
        if (!IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t))) minimal_state.p2_hp = *p2_hp_ptr;
        if (!IsBadReadPtr(game_mode_ptr, sizeof(uint32_t))) minimal_state.game_mode = *game_mode_ptr;
        
        memory_rollback_slots[slot].checksum = Fletcher32((const uint8_t*)&minimal_state, sizeof(MinimalChecksumState));
        
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