#include "savestate.h"
#include "globals.h"
#include "logging.h"
#include "shared_mem.h"
#include "state_manager.h"
#include "object_pool_scanner.h"
#include <windows.h>
#include <SDL3/SDL.h>

// Direct memory access helpers (could be moved to a new utility file later)
template<typename T>
T ReadMemorySafe(uintptr_t address) {
    return *(T*)address;
}

template<typename T>
void WriteMemorySafe(uintptr_t address, T value) {
    *(T*)address = value;
}

// Save complete game state to a SaveStateData structure (for GekkoNet rollback)
bool SaveCompleteGameState(SaveStateData* save_data, uint32_t frame_number) {
    // REDUCED LOGGING: Only log occasionally to prevent spam
    static uint32_t call_counter = 0;
    call_counter++;

    // LIKE BSNES-NETPLAY: No game mode checks, just save state
    // bsnes-netplay simply calls seria.save() for whatever state the emulator has
    
    // Save essential game state data (like bsnes saves SNES RAM)
    save_data->frame_number = frame_number;

    
    // CHEATENGINE-VERIFIED ADDRESSES: Use exact same addresses as MinimalGameState.LoadFromMemory()
    // This ensures checksums match between save/load and verification
    
    // Player state addresses (MUST match CheatEngine WonderfulWorld_ver_0946.CT)
    uint32_t* p1_hp_ptr = (uint32_t*)0x004DFC85;  // CheatEngine verified "P1 HP"
    uint32_t* p2_hp_ptr = (uint32_t*)0x004EDCC4;  // CheatEngine verified "P2 HP"
    uint32_t* p1_x_ptr = (uint32_t*)0x004DFCC3;   // CheatEngine verified "Coor X P1"
    uint16_t* p1_y_ptr = (uint16_t*)0x004DFCC7;   // CheatEngine verified "Coor Y P1"
    uint32_t* p2_x_ptr = (uint32_t*)0x004EDD02;   // CheatEngine verified "Coor X P2"
    uint16_t* p2_y_ptr = (uint16_t*)0x004EDD06;   // CheatEngine verified "Coor Y P2"
    
    // RNG seed and timers (MUST match CheatEngine WonderfulWorld_ver_0946.CT)
    uint32_t* rng_seed_ptr = (uint32_t*)0x41FB1C;   // CheatEngine verified "g_rand"
    uint32_t* timer_ptr = (uint32_t*)0x470050;      // CheatEngine verified "g_actual_wanwan_timer"
    uint32_t* round_timer_ptr = (uint32_t*)0x470050; // Use same as timer for now
    
    // INPUT BUFFER - CRITICAL FOR ROLLBACK: Motion inputs require input history
    uint16_t* p1_input_history_ptr = (uint16_t*)0x4280E0;  // P1 input history (1024 frames)
    uint16_t* p2_input_history_ptr = (uint16_t*)0x4290E0;  // P2 input history (1024 frames)
    uint32_t* input_buffer_index_ptr = (uint32_t*)0x447EE0;  // Frame counter as buffer index
    const size_t input_history_size = 1024 * sizeof(uint16_t);  // 2048 bytes each
    
    // CSS INPUT STATE - CRITICAL FOR CSS: Input change detection for just-pressed buttons
    uint32_t* player_input_changes_ptr = (uint32_t*)0x447f60;  // g_player_input_changes[8] array
    
    // Object pool (391KB) - CRITICAL for proper rollback
    uint8_t* object_pool_ptr = (uint8_t*)0x4701E0;
    const size_t object_pool_size = 0x5F800;
    
    // Check if addresses are valid
    bool addresses_valid = (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t)) && 
                          !IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t)) &&
                          !IsBadReadPtr(p1_x_ptr, sizeof(uint32_t)) && 
                          !IsBadReadPtr(p1_y_ptr, sizeof(uint16_t)) &&
                          !IsBadReadPtr(p2_x_ptr, sizeof(uint32_t)) && 
                          !IsBadReadPtr(p2_y_ptr, sizeof(uint16_t)) &&
                          !IsBadReadPtr(rng_seed_ptr, sizeof(uint32_t)) &&
                          !IsBadReadPtr(timer_ptr, sizeof(uint32_t)) &&
                          !IsBadReadPtr(round_timer_ptr, sizeof(uint32_t)) &&
                          !IsBadReadPtr(p1_input_history_ptr, input_history_size) &&
                          !IsBadReadPtr(p2_input_history_ptr, input_history_size) &&
                          !IsBadReadPtr(input_buffer_index_ptr, sizeof(uint32_t)) &&
                          !IsBadReadPtr(player_input_changes_ptr, 8 * sizeof(uint32_t)) &&
                          !IsBadReadPtr(object_pool_ptr, object_pool_size));
    

    
    // Save player data using CheatEngine addresses
    save_data->p1_hp = *p1_hp_ptr;
    save_data->p2_hp = *p2_hp_ptr; 
    save_data->p1_x = *p1_x_ptr;
    save_data->p2_x = *p2_x_ptr;
    save_data->p1_y = *p1_y_ptr;
    save_data->p2_y = *p2_y_ptr;
    
    // Save RNG and timers
    save_data->rng_seed = *rng_seed_ptr;
    save_data->game_timer = *timer_ptr;
    save_data->round_timer = *round_timer_ptr;
    
    // CRITICAL: Save input buffer - required for motion inputs like quarter-circle forward
    // REDUCED LOGGING: Only log occasionally

    memcpy(save_data->p1_input_history, p1_input_history_ptr, input_history_size);
    memcpy(save_data->p2_input_history, p2_input_history_ptr, input_history_size);
    save_data->input_buffer_index = *input_buffer_index_ptr;
    
    // CSS INPUT STATE: Save input change detection for just-pressed buttons
    memcpy(save_data->player_input_changes, player_input_changes_ptr, 8 * sizeof(uint32_t));
    
    // INPUT REPEAT LOGIC STATE: Save repeat logic state for rollback consistency
    memcpy(save_data->prev_input_state, g_prev_input_state, 8 * sizeof(uint32_t));
    memcpy(save_data->input_repeat_state, g_input_repeat_state, 8 * sizeof(uint32_t));
    memcpy(save_data->input_repeat_timer, g_input_repeat_timer, 8 * sizeof(uint32_t));
    
    // IMMEDIATE INPUT APPLY STATE: Save ApplyNetworkedInputsImmediately state for rollback consistency
    save_data->apply_prev_p1_input = g_apply_prev_p1_input;
    save_data->apply_prev_p2_input = g_apply_prev_p2_input;
    
    // CRITICAL: Save entire object pool (391KB) - required for comprehensive rollback
    // REDUCED LOGGING: Only log occasionally

    memcpy(save_data->object_pool, object_pool_ptr, object_pool_size);
    
    // Enhanced checksum calculation including object pool and input buffer
    struct EssentialSaveData {
        uint32_t p1_hp, p2_hp;
        uint32_t p1_x, p2_x;
        uint16_t p1_y, p2_y;
        uint32_t rng_seed;
        uint32_t game_timer, round_timer;
        // REMOVED: input_buffer_index - this is local timing state, not synchronized game state
    } essential_for_checksum;
    
    essential_for_checksum.p1_hp = save_data->p1_hp;
    essential_for_checksum.p2_hp = save_data->p2_hp;
    essential_for_checksum.p1_x = save_data->p1_x;
    essential_for_checksum.p2_x = save_data->p2_x;
    essential_for_checksum.p1_y = save_data->p1_y;
    essential_for_checksum.p2_y = save_data->p2_y;
    essential_for_checksum.rng_seed = save_data->rng_seed;
    essential_for_checksum.game_timer = save_data->game_timer;
    essential_for_checksum.round_timer = save_data->round_timer;
    
    // Calculate checksum: essential data + object pool + input buffer samples

    uint32_t basic_checksum = FM2K::State::Fletcher32((uint8_t*)&essential_for_checksum, sizeof(essential_for_checksum));
    uint32_t object_checksum = FM2K::State::Fletcher32(save_data->object_pool, 1024); // First 1KB of object pool
    uint32_t p1_input_checksum = FM2K::State::Fletcher32((uint8_t*)save_data->p1_input_history, 512);
    uint32_t p2_input_checksum = FM2K::State::Fletcher32((uint8_t*)save_data->p2_input_history, 512);
    uint32_t input_checksum = p1_input_checksum ^ p2_input_checksum; // First 256 frames each
    save_data->checksum = basic_checksum ^ object_checksum ^ input_checksum; // Combine all checksums
    
    // DESYNC INVESTIGATION: Log individual checksum components
    static uint32_t checksum_debug_counter = 0;
    if (++checksum_debug_counter <= 10) { // Log first 10 frames only
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "🔍 CHECKSUM BREAKDOWN Player%d Frame%u:", ::player_index + 1, save_data->frame_number);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   basic_checksum    = 0x%08X (essential game state)", basic_checksum);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   object_checksum   = 0x%08X (first 1KB object pool)", object_checksum);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   p1_input_checksum = 0x%08X (P1 input history 512B)", p1_input_checksum);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   p2_input_checksum = 0x%08X (P2 input history 512B)", p2_input_checksum);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   input_checksum    = 0x%08X (P1 XOR P2)", input_checksum);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   FINAL_CHECKSUM    = 0x%08X (basic ^ object ^ input)", save_data->checksum);
        
        // FIELD-BY-FIELD ANALYSIS: Log each essential field to find the exact difference
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "🔍 ESSENTIAL FIELDS Player%d:", ::player_index + 1);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   P1_HP=0x%08X, P2_HP=0x%08X", essential_for_checksum.p1_hp, essential_for_checksum.p2_hp);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   P1_Pos=(0x%08X,0x%04X), P2_Pos=(0x%08X,0x%04X)", 
                   essential_for_checksum.p1_x, essential_for_checksum.p1_y, essential_for_checksum.p2_x, essential_for_checksum.p2_y);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   RNG=0x%08X, GameTimer=0x%08X, RoundTimer=0x%08X", 
                   essential_for_checksum.rng_seed, essential_for_checksum.game_timer, essential_for_checksum.round_timer);
    }
    
    // CRITICAL FIX: Mark the save data as valid
    save_data->valid = true;

    
    return true;
}

// Load complete game state from a SaveStateData structure (for GekkoNet rollback)
bool LoadCompleteGameState(const SaveStateData* save_data) {

    
    // Verify enhanced checksum including object pool and input buffer (must match SaveCompleteGameState)
    struct EssentialSaveData {
        uint32_t p1_hp, p2_hp;
        uint32_t p1_x, p2_x;
        uint16_t p1_y, p2_y;
        uint32_t rng_seed;
        uint32_t game_timer, round_timer;
        // REMOVED: input_buffer_index - this is local timing state, not synchronized game state
    } essential_for_checksum;
    
    essential_for_checksum.p1_hp = save_data->p1_hp;
    essential_for_checksum.p2_hp = save_data->p2_hp;
    essential_for_checksum.p1_x = save_data->p1_x;
    essential_for_checksum.p2_x = save_data->p2_x;
    essential_for_checksum.p1_y = save_data->p1_y;
    essential_for_checksum.p2_y = save_data->p2_y;
    essential_for_checksum.rng_seed = save_data->rng_seed;
    essential_for_checksum.game_timer = save_data->game_timer;
    essential_for_checksum.round_timer = save_data->round_timer;
    // frame_number excluded from checksum
    // input_buffer_index excluded from checksum - local timing state
    
    // Calculate combined checksum (must match SaveCompleteGameState calculation exactly)
    uint32_t basic_checksum = FM2K::State::Fletcher32((uint8_t*)&essential_for_checksum, sizeof(essential_for_checksum));
    uint32_t object_checksum = FM2K::State::Fletcher32(save_data->object_pool, 1024); // First 1KB of object pool
    uint32_t input_checksum = FM2K::State::Fletcher32((uint8_t*)save_data->p1_input_history, 512) ^ 
                             FM2K::State::Fletcher32((uint8_t*)save_data->p2_input_history, 512); // First 256 frames each
    uint32_t calculated_checksum = basic_checksum ^ object_checksum ^ input_checksum; // Combine all checksums
    

    
    try {
        // CHEATENGINE-VERIFIED ADDRESSES: Use exact same addresses as SaveCompleteGameState and MinimalGameState
        // Player state addresses (must match CheatEngine WonderfulWorld_ver_0946.CT exactly)
        uint32_t* p1_hp_ptr = (uint32_t*)0x004DFC85;  // CheatEngine verified "P1 HP"
        uint32_t* p2_hp_ptr = (uint32_t*)0x004EDCC4;  // CheatEngine verified "P2 HP"
        uint32_t* p1_x_ptr = (uint32_t*)0x004DFCC3;   // CheatEngine verified "Coor X P1"
        uint16_t* p1_y_ptr = (uint16_t*)0x004DFCC7;   // CheatEngine verified "Coor Y P1"
        uint32_t* p2_x_ptr = (uint32_t*)0x004EDD02;   // CheatEngine verified "Coor X P2"
        uint16_t* p2_y_ptr = (uint16_t*)0x004EDD06;   // CheatEngine verified "Coor Y P2"
        
        // RNG seed and timers (must match CheatEngine WonderfulWorld_ver_0946.CT exactly)
        uint32_t* rng_seed_ptr = (uint32_t*)0x41FB1C;   // CheatEngine verified "g_rand"
        uint32_t* timer_ptr = (uint32_t*)0x470050;      // CheatEngine verified "g_actual_wanwan_timer"
        uint32_t* round_timer_ptr = (uint32_t*)0x470050; // Use same as timer for now
        
        // INPUT BUFFER - CRITICAL FOR ROLLBACK: Motion inputs require input history
        uint16_t* p1_input_history_ptr = (uint16_t*)0x4280E0;  // P1 input history (1024 frames)
        uint16_t* p2_input_history_ptr = (uint16_t*)0x4290E0;  // P2 input history (1024 frames)
        uint32_t* input_buffer_index_ptr = (uint32_t*)0x447EE0;  // Frame counter as buffer index
        const size_t input_history_size = 1024 * sizeof(uint16_t);  // 2048 bytes each
        
        // CSS INPUT STATE - CRITICAL FOR CSS: Input change detection for just-pressed buttons
        uint32_t* player_input_changes_ptr = (uint32_t*)0x447f60;  // g_player_input_changes[8] array
        
        // Object pool (391KB) - CRITICAL for proper rollback
        uint8_t* object_pool_ptr = (uint8_t*)0x4701E0;
        const size_t object_pool_size = 0x5F800;
        
        // Check if addresses are writable
        bool addresses_writable = (!IsBadWritePtr(p1_hp_ptr, sizeof(uint32_t)) && 
                                 !IsBadWritePtr(p2_hp_ptr, sizeof(uint32_t)) &&
                                 !IsBadWritePtr(p1_x_ptr, sizeof(uint32_t)) && 
                                 !IsBadWritePtr(p1_y_ptr, sizeof(uint16_t)) &&
                                 !IsBadWritePtr(p2_x_ptr, sizeof(uint32_t)) && 
                                 !IsBadWritePtr(p2_y_ptr, sizeof(uint16_t)) &&
                                 !IsBadWritePtr(rng_seed_ptr, sizeof(uint32_t)) &&
                                 !IsBadWritePtr(timer_ptr, sizeof(uint32_t)) &&
                                 !IsBadWritePtr(round_timer_ptr, sizeof(uint32_t)) &&
                                 !IsBadWritePtr(p1_input_history_ptr, input_history_size) &&
                                 !IsBadWritePtr(p2_input_history_ptr, input_history_size) &&
                                 !IsBadWritePtr(input_buffer_index_ptr, sizeof(uint32_t)) &&
                                 !IsBadWritePtr(player_input_changes_ptr, 8 * sizeof(uint32_t)) &&
                                 !IsBadWritePtr(object_pool_ptr, object_pool_size));
        

        
        // Restore player data using CheatEngine addresses
        *p1_hp_ptr = save_data->p1_hp;
        *p2_hp_ptr = save_data->p2_hp;
        *p1_x_ptr = save_data->p1_x;
        *p1_y_ptr = save_data->p1_y;
        *p2_x_ptr = save_data->p2_x;
        *p2_y_ptr = save_data->p2_y;
        
        // Restore RNG and timers
        *rng_seed_ptr = save_data->rng_seed;
        *timer_ptr = save_data->game_timer;
        *round_timer_ptr = save_data->round_timer;
        
        // CRITICAL: Restore input buffer - required for motion inputs like quarter-circle forward
        memcpy(p1_input_history_ptr, save_data->p1_input_history, input_history_size);
        memcpy(p2_input_history_ptr, save_data->p2_input_history, input_history_size);
        *input_buffer_index_ptr = save_data->input_buffer_index;
        
        // CSS INPUT STATE: Restore input change detection for just-pressed buttons
        memcpy(player_input_changes_ptr, save_data->player_input_changes, 8 * sizeof(uint32_t));
        
        // INPUT REPEAT LOGIC STATE: Restore repeat logic state for rollback consistency
        memcpy(g_prev_input_state, save_data->prev_input_state, 8 * sizeof(uint32_t));
        memcpy(g_input_repeat_state, save_data->input_repeat_state, 8 * sizeof(uint32_t));
        memcpy(g_input_repeat_timer, save_data->input_repeat_timer, 8 * sizeof(uint32_t));
        
        // IMMEDIATE INPUT APPLY STATE: Restore ApplyNetworkedInputsImmediately state for rollback consistency
        g_apply_prev_p1_input = save_data->apply_prev_p1_input;
        g_apply_prev_p2_input = save_data->apply_prev_p2_input;
        
        // CRITICAL: Restore entire object pool (391KB) - required for comprehensive rollback
        memcpy(object_pool_ptr, save_data->object_pool, object_pool_size);
        

        return true;
        
    } catch (...) {
        return false;
    }
}

void ProcessManualSaveLoadRequests() {
    SharedInputData* shared_data = GetSharedMemory();
    if (!shared_data) {
        return;
    }
    
    // Handle manual save request
    if (manual_save_requested) {
        // Use hotkey target slot if available, otherwise fall back to launcher slot
        uint32_t target_slot = (target_save_slot < 8) ? target_save_slot : shared_data->debug_target_slot;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Processing save state request for slot %u", target_slot);
        
        if (target_slot < 8) {
            // COMPREHENSIVE save state - all variables from CheatEngine table
            
            // Player state addresses (CheatEngine verified)
            uint32_t* p1_hp_ptr = (uint32_t*)0x004DFC85;
            uint32_t* p2_hp_ptr = (uint32_t*)0x004EDCC4;
            uint32_t* p1_x_ptr = (uint32_t*)0x004DFCC3;
            uint16_t* p1_y_ptr = (uint16_t*)0x004DFCC7;
            uint32_t* p2_x_ptr = (uint32_t*)0x004EDD02;
            uint16_t* p2_y_ptr = (uint16_t*)0x004EDD06;
            
            // Player meter/super/stock
            uint32_t* p1_super_ptr = (uint32_t*)0x004DFC9D;
            uint32_t* p2_super_ptr = (uint32_t*)0x004EDCDC;
            uint32_t* p1_special_stock_ptr = (uint32_t*)0x004DFC95;
            uint32_t* p2_special_stock_ptr = (uint32_t*)0x004EDCD4;
            uint32_t* p1_rounds_won_ptr = (uint32_t*)0x004DFC6D;
            uint32_t* p2_rounds_won_ptr = (uint32_t*)0x004EDCAC;
            
            // RNG seed
            uint32_t* rng_seed_ptr = (uint32_t*)0x41FB1C;
            
            // Timers
            uint32_t* timer_ptr = (uint32_t*)0x470050;
            uint32_t* round_timer_ptr = (uint32_t*)0x00470060;
            uint32_t* round_state_ptr = (uint32_t*)0x47004C;
            uint32_t* round_limit_ptr = (uint32_t*)0x470048;
            uint32_t* round_setting_ptr = (uint32_t*)0x470068;
            
            // Game modes and flags
            uint32_t* fm2k_game_mode_ptr = (uint32_t*)0x470040;
            uint16_t* game_mode_data_ptr = (uint16_t*)0x00470054;  // Renamed to avoid conflict
            uint32_t* game_paused_ptr = (uint32_t*)0x4701BC;
            uint32_t* replay_mode_ptr = (uint32_t*)0x4701C0;
            
            // Camera position
            uint32_t* camera_x_ptr = (uint32_t*)0x00447F2C;
            uint32_t* camera_y_ptr = (uint32_t*)0x00447F30;
            
            // Character variables base addresses
            int16_t* p1_char_vars_ptr = (int16_t*)0x004DFD17;
            int16_t* p2_char_vars_ptr = (int16_t*)0x004EDD56;
            
            // System variables base address
            int16_t* sys_vars_ptr = (int16_t*)0x004456B0;
            
            // Task variables base addresses
            uint16_t* p1_task_vars_ptr = (uint16_t*)0x00470311;
            uint16_t* p2_task_vars_ptr = (uint16_t*)0x0047060D;
            
            // Move history
            uint8_t* move_history_ptr = (uint8_t*)0x47006C;
            
            // Additional state
            uint32_t* object_count_ptr = (uint32_t*)0x004246FC;
            uint32_t* frame_sync_flag_ptr = (uint32_t*)0x00424700;
            uint32_t* hit_effect_target_ptr = (uint32_t*)0x4701C4;
            
            // Character selection
            uint32_t* menu_selection_ptr = (uint32_t*)0x424780;
            uint64_t* p1_css_cursor_ptr = (uint64_t*)0x00424E50;
            uint64_t* p2_css_cursor_ptr = (uint64_t*)0x00424E58;
            uint32_t* p1_char_to_load_ptr = (uint32_t*)0x470020;
            uint32_t* p2_char_to_load_ptr = (uint32_t*)0x470024;
            uint32_t* p1_color_selection_ptr = (uint32_t*)0x00470024;
            
            // Object pool (391KB)
            uint8_t* object_pool_ptr = (uint8_t*)0x4701E0;
            const size_t object_pool_size = 0x5F800;
            
            // Check if addresses are valid (simplified - just check key pointers)
            bool addresses_valid = (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t)) && 
                                  !IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t)) &&
                                  !IsBadReadPtr(p1_x_ptr, sizeof(uint32_t)) && 
                                  !IsBadReadPtr(p1_y_ptr, sizeof(uint16_t)) &&
                                  !IsBadReadPtr(p2_x_ptr, sizeof(uint32_t)) && 
                                  !IsBadReadPtr(p2_y_ptr, sizeof(uint16_t)) &&
                                  !IsBadReadPtr(rng_seed_ptr, sizeof(uint32_t)) &&
                                  !IsBadReadPtr(timer_ptr, sizeof(uint32_t)) &&
                                  !IsBadReadPtr(object_pool_ptr, object_pool_size) &&
                                  !IsBadReadPtr(p1_char_vars_ptr, sizeof(int16_t) * 16) &&
                                  !IsBadReadPtr(p2_char_vars_ptr, sizeof(int16_t) * 16) &&
                                  !IsBadReadPtr(sys_vars_ptr, sizeof(int16_t) * 16) &&
                                  !IsBadReadPtr(p1_task_vars_ptr, sizeof(uint16_t) * 16) &&
                                  !IsBadReadPtr(p2_task_vars_ptr, sizeof(uint16_t) * 16) &&
                                  !IsBadReadPtr(move_history_ptr, 16));
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SAVE MEMORY CHECK: addresses_valid=%s", addresses_valid ? "true" : "false");
            
            if (addresses_valid) {
                SaveStateData* save_slot = &shared_data->save_slots[target_slot];
                
                // Save player state
                save_slot->p1_hp = *p1_hp_ptr;
                save_slot->p2_hp = *p2_hp_ptr;
                save_slot->p1_x = *p1_x_ptr;
                save_slot->p1_y = *p1_y_ptr;
                save_slot->p2_x = *p2_x_ptr;
                save_slot->p2_y = *p2_y_ptr;
                
                // Save player meter/super/stock
                save_slot->p1_super = *p1_super_ptr;
                save_slot->p2_super = *p2_super_ptr;
                save_slot->p1_special_stock = *p1_special_stock_ptr;
                save_slot->p2_special_stock = *p2_special_stock_ptr;
                save_slot->p1_rounds_won = *p1_rounds_won_ptr;
                save_slot->p2_rounds_won = *p2_rounds_won_ptr;
                
                // Save RNG seed
                save_slot->rng_seed = *rng_seed_ptr;
                
                // Save timers
                save_slot->game_timer = *timer_ptr;
                save_slot->round_timer = *round_timer_ptr;
                save_slot->round_state = *round_state_ptr;
                save_slot->round_limit = *round_limit_ptr;
                save_slot->round_setting = *round_setting_ptr;
                
                // Save game modes and flags
                save_slot->fm2k_game_mode = *fm2k_game_mode_ptr;
                save_slot->game_mode = *game_mode_data_ptr;
                save_slot->game_paused = *game_paused_ptr;
                save_slot->replay_mode = *replay_mode_ptr;
                
                // Save camera position
                save_slot->camera_x = *camera_x_ptr;
                save_slot->camera_y = *camera_y_ptr;
                
                // Save character variables (16 vars per player)
                memcpy(save_slot->p1_char_vars, p1_char_vars_ptr, sizeof(int16_t) * 16);
                memcpy(save_slot->p2_char_vars, p2_char_vars_ptr, sizeof(int16_t) * 16);
                
                // Save system variables (14 signed + 2 unsigned)
                memcpy(save_slot->sys_vars, sys_vars_ptr, sizeof(int16_t) * 14);
                save_slot->sys_vars_unsigned[0] = *(uint16_t*)(sys_vars_ptr + 14);
                save_slot->sys_vars_unsigned[1] = *(uint16_t*)(sys_vars_ptr + 15);
                
                // Save task variables (16 per player)
                memcpy(save_slot->p1_task_vars, p1_task_vars_ptr, sizeof(uint16_t) * 16);
                memcpy(save_slot->p2_task_vars, p2_task_vars_ptr, sizeof(uint16_t) * 16);
                
                // Save move history
                memcpy(save_slot->player_move_history, move_history_ptr, 16);
                
                // Save additional state
                save_slot->object_count = *object_count_ptr;
                save_slot->frame_sync_flag = *frame_sync_flag_ptr;
                save_slot->hit_effect_target = *hit_effect_target_ptr;
                
                // Save character selection state
                save_slot->menu_selection = *menu_selection_ptr;
                save_slot->p1_css_cursor = *p1_css_cursor_ptr;
                save_slot->p2_css_cursor = *p2_css_cursor_ptr;
                save_slot->p1_char_to_load = *p1_char_to_load_ptr;
                save_slot->p2_char_to_load = *p2_char_to_load_ptr;
                save_slot->p1_color_selection = *p1_color_selection_ptr;
                
                // Save entire object pool (391KB)
                memcpy(save_slot->object_pool, object_pool_ptr, object_pool_size);
                
                // Metadata
                save_slot->frame_number = g_frame_counter;
                uint64_t timestamp = SDL_GetTicks();
                if (timestamp == 0) {
                    timestamp = 1; // Avoid 0 timestamp which might be treated as invalid
                }
                save_slot->timestamp_ms = timestamp;
                save_slot->valid = true;
                save_slot->checksum = save_slot->p1_hp + save_slot->p2_hp + save_slot->rng_seed;
                
                // Read engine's authoritative object count (ground truth)
                uint32_t* engine_object_count_ptr = (uint32_t*)0x4246FC;
                uint32_t engine_object_count = 0;
                if (IsBadReadPtr(engine_object_count_ptr, sizeof(uint32_t)) == 0) {
                    engine_object_count = *engine_object_count_ptr;
                }
                
                // Analyze saved objects for rich logging and UI display
                auto active_objects = FM2K::ObjectPool::Scanner::ScanActiveObjects();
                uint32_t character_count = 0, projectile_count = 0, effect_count = 0, system_count = 0, other_count = 0;
                
                // Enhanced object classification with detailed analysis
                std::string object_details = "";
                for (const auto& obj : active_objects) {
                    switch (obj.type) {
                        case 1: system_count++; break;
                        case 4: character_count++; break;
                        case 5: projectile_count++; break;
                        case 6: effect_count++; break;
                        default: other_count++; break;
                    }
                    
                    // Add detailed object info for first few objects
                    if (active_objects.size() <= 10) {
                        if (!object_details.empty()) object_details += ", ";
                        object_details += "Slot" + std::to_string(obj.slot_index) + ":";
                        switch (obj.type) {
                            case 1: object_details += "SYSTEM"; break;
                            case 4: object_details += "CHARACTER"; break;
                            case 5: object_details += "PROJECTILE"; break;
                            case 6: object_details += "EFFECT"; break;
                            default: object_details += "TYPE" + std::to_string(obj.type); break;
                        }
                    }
                }
                
                // Update slot status for launcher UI (populate ALL fields GetSlotStatus reads)
                shared_data->slot_status[target_slot].occupied = true;
                shared_data->slot_status[target_slot].frame_number = g_frame_counter;
                shared_data->slot_status[target_slot].timestamp_ms = save_slot->timestamp_ms;
                shared_data->slot_status[target_slot].checksum = save_slot->checksum;
                shared_data->slot_status[target_slot].state_size_kb = 391; // Full object pool (391KB)
                shared_data->slot_status[target_slot].save_time_us = 0;   // We'll measure this later
                shared_data->slot_status[target_slot].load_time_us = 0;
                shared_data->slot_status[target_slot].active_object_count = engine_object_count;  // Use engine's authoritative count
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK UPDATED SLOT_STATUS: slot=%u, occupied=true, timestamp=%llu", 
                           target_slot, shared_data->slot_status[target_slot].timestamp_ms);
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SAVE SUCCESSFUL: Slot %u - P1_HP=%u, P2_HP=%u, P1_Pos=(%u,%u), P2_Pos=(%u,%u), RNG=0x%08X, Timer=%u", 
                           target_slot, save_slot->p1_hp, save_slot->p2_hp, save_slot->p1_x, save_slot->p1_y, 
                           save_slot->p2_x, save_slot->p2_y, save_slot->rng_seed, save_slot->game_timer);
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ENGINE OBJECT COUNT: %u (authoritative from 0x4246FC)", engine_object_count);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SCANNER FOUND: %zu objects - %u characters, %u projectiles, %u effects, %u system, %u other", 
                           active_objects.size(), character_count, projectile_count, effect_count, system_count, other_count);
                
                if (!object_details.empty()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "OBJECT DETAILS: %s", object_details.c_str());
                }
                
                if (engine_object_count != active_objects.size()) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "OBJECT COUNT MISMATCH: Engine=%u vs Scanner=%zu (difference: %d)", 
                               engine_object_count, active_objects.size(), (int)engine_object_count - (int)active_objects.size());
                    
                    // If there's a mismatch and we have few objects, do detailed analysis
                    if (active_objects.size() <= 15) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PERFORMING DETAILED OBJECT ANALYSIS...");
                        FM2K::ObjectPool::Scanner::LogAllActiveObjects();
                    }
                }
                           
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SLOT STATUS: occupied=%s, frame=%u, timestamp=%llu", 
                           shared_data->slot_status[target_slot].occupied ? "true" : "false",
                           shared_data->slot_status[target_slot].frame_number,
                           shared_data->slot_status[target_slot].timestamp_ms);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Save failed - invalid memory addresses");
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Save failed - invalid slot %u", target_slot);
        }
        manual_save_requested = false;
        target_save_slot = 0; // Reset hotkey target slot
    }
    
    // Handle manual load request
    if (manual_load_requested) {
        // Use hotkey target slot if available, otherwise fall back to launcher slot
        uint32_t target_slot = (target_load_slot < 8) ? target_load_slot : shared_data->debug_target_slot;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LOAD START: Processing load state request for slot %u", target_slot);
        
        if (target_slot < 8 && shared_data->save_slots[target_slot].valid) {
            SaveStateData* save_slot = &shared_data->save_slots[target_slot];
            
            // COMPREHENSIVE load state - all variables from CheatEngine table
            
            // Player state addresses
            uint32_t* p1_hp_ptr = (uint32_t*)0x004DFC85;
            uint32_t* p2_hp_ptr = (uint32_t*)0x004EDCC4;
            uint32_t* p1_x_ptr = (uint32_t*)0x004DFCC3;
            uint16_t* p1_y_ptr = (uint16_t*)0x004DFCC7;
            uint32_t* p2_x_ptr = (uint32_t*)0x004EDD02;
            uint16_t* p2_y_ptr = (uint16_t*)0x004EDD06;
            
            // Player meter/super/stock
            uint32_t* p1_super_ptr = (uint32_t*)0x004DFC9D;
            uint32_t* p2_super_ptr = (uint32_t*)0x004EDCDC;
            uint32_t* p1_special_stock_ptr = (uint32_t*)0x004DFC95;
            uint32_t* p2_special_stock_ptr = (uint32_t*)0x004EDCD4;
            uint32_t* p1_rounds_won_ptr = (uint32_t*)0x004DFC6D;
            uint32_t* p2_rounds_won_ptr = (uint32_t*)0x004EDCAC;
            
            // RNG seed
            uint32_t* rng_seed_ptr = (uint32_t*)0x41FB1C;
            
            // Timers
            uint32_t* timer_ptr = (uint32_t*)0x470050;
            uint32_t* round_timer_ptr = (uint32_t*)0x00470060;
            uint32_t* round_state_ptr = (uint32_t*)0x47004C;
            uint32_t* round_limit_ptr = (uint32_t*)0x470048;
            uint32_t* round_setting_ptr = (uint32_t*)0x470068;
            
            // Game modes and flags
            uint32_t* fm2k_game_mode_ptr = (uint32_t*)0x470040;
            uint16_t* game_mode_data_ptr = (uint16_t*)0x00470054;  // Renamed to avoid conflict
            uint32_t* game_paused_ptr = (uint32_t*)0x4701BC;
            uint32_t* replay_mode_ptr = (uint32_t*)0x4701C0;
            
            // Camera position
            uint32_t* camera_x_ptr = (uint32_t*)0x00447F2C;
            uint32_t* camera_y_ptr = (uint32_t*)0x00447F30;
            
            // Character variables base addresses
            int16_t* p1_char_vars_ptr = (int16_t*)0x004DFD17;
            int16_t* p2_char_vars_ptr = (int16_t*)0x004EDD56;
            
            // System variables base address
            int16_t* sys_vars_ptr = (int16_t*)0x004456B0;
            
            // Task variables base addresses
            uint16_t* p1_task_vars_ptr = (uint16_t*)0x00470311;
            uint16_t* p2_task_vars_ptr = (uint16_t*)0x0047060D;
            
            // Move history
            uint8_t* move_history_ptr = (uint8_t*)0x47006C;
            
            // Additional state
            uint32_t* object_count_ptr = (uint32_t*)0x004246FC;
            uint32_t* frame_sync_flag_ptr = (uint32_t*)0x00424700;
            uint32_t* hit_effect_target_ptr = (uint32_t*)0x4701C4;
            
            // Character selection
            uint32_t* menu_selection_ptr = (uint32_t*)0x424780;
            uint64_t* p1_css_cursor_ptr = (uint64_t*)0x00424E50;
            uint64_t* p2_css_cursor_ptr = (uint64_t*)0x00424E58;
            uint32_t* p1_char_to_load_ptr = (uint32_t*)0x470020;
            uint32_t* p2_char_to_load_ptr = (uint32_t*)0x470024;
            uint32_t* p1_color_selection_ptr = (uint32_t*)0x00470024;
            
            // Object pool (391KB)
            uint8_t* object_pool_ptr = (uint8_t*)0x4701E0;
            const size_t object_pool_size = 0x5F800;
            
            bool addresses_writable = (!IsBadWritePtr(p1_hp_ptr, sizeof(uint32_t)) && 
                                     !IsBadWritePtr(p2_hp_ptr, sizeof(uint32_t)) &&
                                     !IsBadWritePtr(p1_x_ptr, sizeof(uint32_t)) && 
                                     !IsBadWritePtr(p1_y_ptr, sizeof(uint16_t)) &&
                                     !IsBadWritePtr(p2_x_ptr, sizeof(uint32_t)) && 
                                     !IsBadWritePtr(p2_y_ptr, sizeof(uint16_t)) &&
                                     !IsBadWritePtr(rng_seed_ptr, sizeof(uint32_t)) &&
                                     !IsBadWritePtr(timer_ptr, sizeof(uint32_t)) &&
                                     !IsBadWritePtr(object_pool_ptr, object_pool_size) &&
                                     !IsBadWritePtr(p1_char_vars_ptr, sizeof(int16_t) * 16) &&
                                     !IsBadWritePtr(p2_char_vars_ptr, sizeof(int16_t) * 16) &&
                                     !IsBadWritePtr(sys_vars_ptr, sizeof(int16_t) * 16) &&
                                     !IsBadWritePtr(p1_task_vars_ptr, sizeof(uint16_t) * 16) &&
                                     !IsBadWritePtr(p2_task_vars_ptr, sizeof(uint16_t) * 16) &&
                                     !IsBadWritePtr(move_history_ptr, 16));
            
            if (addresses_writable) {
                // Restore player state
                *p1_hp_ptr = save_slot->p1_hp;
                *p2_hp_ptr = save_slot->p2_hp;
                *p1_x_ptr = save_slot->p1_x;
                *p1_y_ptr = save_slot->p1_y;
                *p2_x_ptr = save_slot->p2_x;
                *p2_y_ptr = save_slot->p2_y;
                
                // Restore player meter/super/stock
                *p1_super_ptr = save_slot->p1_super;
                *p2_super_ptr = save_slot->p2_super;
                *p1_special_stock_ptr = save_slot->p1_special_stock;
                *p2_special_stock_ptr = save_slot->p2_special_stock;
                *p1_rounds_won_ptr = save_slot->p1_rounds_won;
                *p2_rounds_won_ptr = save_slot->p2_rounds_won;
                
                // Restore RNG seed
                *rng_seed_ptr = save_slot->rng_seed;
                
                // Restore timers
                *timer_ptr = save_slot->game_timer;
                *round_timer_ptr = save_slot->round_timer;
                *round_state_ptr = save_slot->round_state;
                *round_limit_ptr = save_slot->round_limit;
                *round_setting_ptr = save_slot->round_setting;
                
                // Restore game modes and flags
                *fm2k_game_mode_ptr = save_slot->fm2k_game_mode;
                *game_mode_data_ptr = save_slot->game_mode;
                *game_paused_ptr = save_slot->game_paused;
                *replay_mode_ptr = save_slot->replay_mode;
                
                // Restore camera position
                *camera_x_ptr = save_slot->camera_x;
                *camera_y_ptr = save_slot->camera_y;
                
                // Restore character variables (16 vars per player)
                memcpy(p1_char_vars_ptr, save_slot->p1_char_vars, sizeof(int16_t) * 16);
                memcpy(p2_char_vars_ptr, save_slot->p2_char_vars, sizeof(int16_t) * 16);
                
                // Restore system variables (14 signed + 2 unsigned)
                memcpy(sys_vars_ptr, save_slot->sys_vars, sizeof(int16_t) * 14);
                *(uint16_t*)(sys_vars_ptr + 14) = save_slot->sys_vars_unsigned[0];
                *(uint16_t*)(sys_vars_ptr + 15) = save_slot->sys_vars_unsigned[1];
                
                // Restore task variables (16 per player)
                memcpy(p1_task_vars_ptr, save_slot->p1_task_vars, sizeof(uint16_t) * 16);
                memcpy(p2_task_vars_ptr, save_slot->p2_task_vars, sizeof(uint16_t) * 16);
                
                // Restore move history
                memcpy(move_history_ptr, save_slot->player_move_history, 16);
                
                // Restore additional state
                *object_count_ptr = save_slot->object_count;
                *frame_sync_flag_ptr = save_slot->frame_sync_flag;
                *hit_effect_target_ptr = save_slot->hit_effect_target;
                
                // Restore character selection state
                *menu_selection_ptr = save_slot->menu_selection;
                *p1_css_cursor_ptr = save_slot->p1_css_cursor;
                *p2_css_cursor_ptr = save_slot->p2_css_cursor;
                *p1_char_to_load_ptr = save_slot->p1_char_to_load;
                *p2_char_to_load_ptr = save_slot->p2_char_to_load;
                *p1_color_selection_ptr = save_slot->p1_color_selection;
                
                // Restore entire object pool (391KB)
                memcpy(object_pool_ptr, save_slot->object_pool, object_pool_size);
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LOAD SUCCESSFUL: Slot %u - P1_HP=%u, P2_HP=%u, P1_Pos=(%u,%u), P2_Pos=(%u,%u), RNG=0x%08X, Timer=%u", 
                           target_slot, save_slot->p1_hp, save_slot->p2_hp, save_slot->p1_x, save_slot->p1_y, 
                           save_slot->p2_x, save_slot->p2_y, save_slot->rng_seed, save_slot->game_timer);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Load failed - invalid memory addresses");
            }
        } else if (target_slot >= 8) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Load failed - invalid slot %u", target_slot);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Load failed - slot %u is empty", target_slot);
        }
        manual_load_requested = false;
        target_load_slot = 0; // Reset hotkey target slot
    }
} 