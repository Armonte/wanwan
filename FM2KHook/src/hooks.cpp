#include "hooks.h"
#include "globals.h"
#include "gekkonet_hooks.h"
#include "state_manager.h"
#include "logging.h"
#include "shared_mem.h"
#include "game_state_machine.h"
#include "object_tracker.h"
#include "object_analysis.h"
#include "object_pool_scanner.h"
// #include "boot_object_analyzer.cpp"  // REMOVED: Performance optimization
#include <windows.h>
#include <mmsystem.h>
#include <limits>
#include <exception>
#include <string>
#include <cstdlib>

// Global variables for manual save/load requests
bool battle_sync_done = false; // Flag to ensure sync happens only once per battle

// Original game_rand function pointer and deterministic RNG state
typedef uint32_t (*GameRandFunc)();
static GameRandFunc original_game_rand = nullptr;
static uint32_t deterministic_rng_seed = 12345678;
static bool use_deterministic_rng = false;
static bool manual_save_requested = false;
static bool manual_load_requested = false;
static uint32_t target_save_slot = 0;
static uint32_t target_load_slot = 0;

// CSS Input injection system
DelayedInput css_delayed_inputs[2] = {{0, 0, false}, {0, 0, false}};

// Auto-save tracking (separate from globals.h version)
static uint32_t hook_last_auto_save_frame = 0;

// Input buffer write patches for motion input preservation
static bool buffer_writes_patched = false;
static uint8_t original_bytes_1[7] = {0};
static uint8_t original_bytes_2[7] = {0};

static void PatchInputBufferWrites(bool block) {
    // Addresses where process_game_inputs writes to input history buffer
    uint8_t* write_addr_1 = (uint8_t*)0x41472E;
    uint8_t* write_addr_2 = (uint8_t*)0x41474F;
    
    if (block && !buffer_writes_patched) {
        // Save original bytes
        memcpy(original_bytes_1, write_addr_1, 7);
        memcpy(original_bytes_2, write_addr_2, 7);
        
        // Make memory writable
        DWORD old_protect;
        VirtualProtect(write_addr_1, 7, PAGE_EXECUTE_READWRITE, &old_protect);
        VirtualProtect(write_addr_2, 7, PAGE_EXECUTE_READWRITE, &old_protect);
        
        // Patch to NOPs
        memset(write_addr_1, 0x90, 7); // NOP the mov instruction
        memset(write_addr_2, 0x90, 7); // NOP the mov instruction
        
        buffer_writes_patched = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FRAME STEP: Patched input buffer writes - motion inputs preserved");
    } else if (!block && buffer_writes_patched) {
        // Restore original bytes
        DWORD old_protect;
        VirtualProtect(write_addr_1, 7, PAGE_EXECUTE_READWRITE, &old_protect);
        VirtualProtect(write_addr_2, 7, PAGE_EXECUTE_READWRITE, &old_protect);
        
        memcpy(write_addr_1, original_bytes_1, 7);
        memcpy(write_addr_2, original_bytes_2, 7);
        
        buffer_writes_patched = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FRAME STEP: Restored input buffer writes - normal operation");
    }
}

// ARCHITECTURE FIX: Real input capture following CCCaster/GekkoNet pattern
static void CaptureRealInputs() {
    // In online mode, we only read the input for the local player.
    // In true offline (local VS) mode, we read both.
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);

    if (original_get_player_input) {
        if (is_true_offline) {
            // TRUE OFFLINE: Read both players from local hardware.
            live_p1_input = original_get_player_input(0, 0);
            live_p2_input = original_get_player_input(1, 0);
        } else {
            // ONLINE: Both host and client read their local controls from the P1 slot.
            // The netcode layer (GekkoNet) will map this to the correct player in-game.
            uint32_t local_hardware_input = original_get_player_input(0, 0);

            if (::is_host) {
                live_p1_input = local_hardware_input;
                live_p2_input = 0;
            } else {
                live_p1_input = 0;
                // The client's local input becomes P2's input in the session.
                live_p2_input = local_hardware_input;
            }
        }

        // The P2 left/right bit swap is a hardware/engine quirk, apply it whenever P2 input is generated.
        // This needs to happen for the client's input as it will control the P2 character.
        uint32_t p2_left = (live_p2_input & 0x001);
        uint32_t p2_right = (live_p2_input & 0x002);
        live_p2_input &= ~0x003;
        live_p2_input |= (p2_left << 1);
        live_p2_input |= (p2_right >> 1);

    } else {
        live_p1_input = 0;
        live_p2_input = 0;
    }

    // Debug logging for button issues
    static uint32_t debug_counter = 0;
    if (debug_counter++ % 60 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "2DFM INPUT: P1=0x%03X P2=0x%03X",
                   live_p1_input & 0x7FF, live_p2_input & 0x7FF);
    }
}

// Use global function pointers from globals.h

// Direct memory access - addresses are known to be valid
template<typename T>
T ReadMemorySafe(uintptr_t address) {
    return *(T*)address;
}

template<typename T>
void WriteMemorySafe(uintptr_t address, T value) {
    *(T*)address = value;
}

// Proper input bit mapping
static inline uint32_t ConvertNetworkInputToGameFormat(uint32_t network_input) {
    uint32_t game_input = 0;
    
    if (network_input & 0x001) game_input |= 0x001;  // LEFT
    if (network_input & 0x002) game_input |= 0x002;  // RIGHT
    if (network_input & 0x004) game_input |= 0x004;  // UP
    if (network_input & 0x008) game_input |= 0x008;  // DOWN
    if (network_input & 0x010) game_input |= 0x010;  // BUTTON1
    if (network_input & 0x020) game_input |= 0x020;  // BUTTON2
    if (network_input & 0x040) game_input |= 0x040;  // BUTTON3
    if (network_input & 0x080) game_input |= 0x080;  // BUTTON4
    if (network_input & 0x100) game_input |= 0x100;  // BUTTON5
    if (network_input & 0x200) game_input |= 0x200;  // BUTTON6
    if (network_input & 0x400) game_input |= 0x400;  // BUTTON7 (7th button at bit 1024)
    
    return game_input;
}

// Process manual save/load requests
// Save complete game state to a SaveStateData structure (for GekkoNet rollback)
static bool SaveCompleteGameState(SaveStateData* save_data, uint32_t frame_number) {
    if (!save_data) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SaveCompleteGameState: Invalid save_data pointer");
        return false;
    }
    
    // Only save states when in battle mode (game_mode 3000) - this is a secondary check
    uint16_t* game_mode_ptr = (uint16_t*)0x470054;  // Use g_game_mode instead of g_fm2k_game_mode
    if (!IsBadReadPtr(game_mode_ptr, sizeof(uint16_t))) {
        uint16_t current_mode = *game_mode_ptr;
        if (current_mode != 3000) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveCompleteGameState: Secondary check - not in battle mode (frame: %d, game_mode: %d)", frame_number, current_mode);
            return false;
        }
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SaveCompleteGameState: IsBadReadPtr failed for game_mode (0x470054) - memory not accessible");
        return false;
    }
    
    // Clear the save data structure
    memset(save_data, 0, sizeof(SaveStateData));
    
    // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveCompleteGameState: Starting memory access for frame %d", frame_number);
    
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
    
    try {
        // CRITICAL ROLLBACK STATE: Save essential data for proper desync detection
        
        // Basic player state (HP)
        if (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t))) {
            save_data->p1_hp = *p1_hp_ptr;
        }
        if (!IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t))) {
            save_data->p2_hp = *p2_hp_ptr;
        }
        
        // Player positions (critical for rollback)
        uint32_t* p1_x_ptr = (uint32_t*)0x004DFCC3;
        uint16_t* p1_y_ptr = (uint16_t*)0x004DFCC7;
        uint32_t* p2_x_ptr = (uint32_t*)0x004EDD02;
        uint16_t* p2_y_ptr = (uint16_t*)0x004EDD06;
        
        if (!IsBadReadPtr(p1_x_ptr, sizeof(uint32_t))) {
            save_data->p1_x = *p1_x_ptr;
        }
        if (!IsBadReadPtr(p1_y_ptr, sizeof(uint16_t))) {
            save_data->p1_y = *p1_y_ptr;
        }
        if (!IsBadReadPtr(p2_x_ptr, sizeof(uint32_t))) {
            save_data->p2_x = *p2_x_ptr;
        }
        if (!IsBadReadPtr(p2_y_ptr, sizeof(uint16_t))) {
            save_data->p2_y = *p2_y_ptr;
        }
        
        // RNG seed (critical for determinism)
        if (!IsBadReadPtr(rng_seed_ptr, sizeof(uint32_t))) {
            save_data->rng_seed = *rng_seed_ptr;
        }
        
        // Game timers (critical for game state)
        uint32_t* game_timer_ptr = (uint32_t*)0x470050;
        uint32_t* round_timer_ptr = (uint32_t*)0x470060;
        
        if (!IsBadReadPtr(game_timer_ptr, sizeof(uint32_t))) {
            save_data->game_timer = *game_timer_ptr;
        }
        if (!IsBadReadPtr(round_timer_ptr, sizeof(uint32_t))) {
            save_data->round_timer = *round_timer_ptr;
        }
        
        // Set metadata
        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveCompleteGameState: Setting metadata...");
        save_data->frame_number = frame_number;
        save_data->timestamp_ms = GetTickCount64();
        save_data->valid = true;
        
        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveCompleteGameState: Calculating Fletcher32 checksum...");
        // Calculate checksum over only the essential data we actually set
        struct EssentialSaveData {
            uint32_t p1_hp, p2_hp;
            uint32_t p1_x, p2_x;
            uint16_t p1_y, p2_y;
            uint32_t rng_seed;
            uint32_t game_timer, round_timer;
            // NOTE: frame_number excluded from checksum - it shouldn't affect game state validation
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
        
        save_data->checksum = FM2K::State::Fletcher32((uint8_t*)&essential_for_checksum, sizeof(essential_for_checksum));
        
        // Log critical save data for desync debugging - ALWAYS log first 40 frames to see sync
        static int save_log_counter = 0;
        bool should_log = (frame_number <= 40) || (++save_log_counter % 120 == 0); // First 40 frames OR every 2nd second
        
        if (should_log) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveState F%d: P1(HP:%d X:%d Y:%d) P2(HP:%d X:%d Y:%d) RNG:%d GT:%d RT:%d CK:%u", 
                       frame_number, save_data->p1_hp, save_data->p1_x, save_data->p1_y,
                       save_data->p2_hp, save_data->p2_x, save_data->p2_y, 
                       save_data->rng_seed, save_data->game_timer, save_data->round_timer, save_data->checksum);
        }
        
        return true;
        
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SaveCompleteGameState: Standard exception during memory access (frame %d): %s", frame_number, e.what());
        return false;
    } catch (...) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SaveCompleteGameState: Unknown exception during memory access (frame %d)", frame_number);
        return false;
    }
}

// Load complete game state from a SaveStateData structure (for GekkoNet rollback)
static bool LoadCompleteGameState(const SaveStateData* save_data) {
    if (!save_data || !save_data->valid) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LoadCompleteGameState: Invalid save_data pointer or data not valid");
        return false;
    }
    
    // Verify checksum using the same essential data structure as save
    struct EssentialSaveData {
        uint32_t p1_hp, p2_hp;
        uint32_t p1_x, p2_x;
        uint16_t p1_y, p2_y;
        uint32_t rng_seed;
        uint32_t game_timer, round_timer;
        // NOTE: frame_number excluded from checksum - it shouldn't affect game state validation
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
    
    uint32_t calculated_checksum = FM2K::State::Fletcher32((uint8_t*)&essential_for_checksum, sizeof(essential_for_checksum));
    if (calculated_checksum != save_data->checksum) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LoadCompleteGameState: Checksum mismatch (calculated: %u, stored: %u)",
                    calculated_checksum, save_data->checksum);
        return false;
    }
    
    try {
        // CRITICAL: Only restore the essential data we actually save
        // Player state addresses
        uint32_t* p1_hp_ptr = (uint32_t*)0x004DFC85;
        uint32_t* p2_hp_ptr = (uint32_t*)0x004EDCC4;
        uint32_t* p1_x_ptr = (uint32_t*)0x004DFCC3;
        uint16_t* p1_y_ptr = (uint16_t*)0x004DFCC7;
        uint32_t* p2_x_ptr = (uint32_t*)0x004EDD02;
        uint16_t* p2_y_ptr = (uint16_t*)0x004EDD06;
        
        // RNG seed
        uint32_t* rng_seed_ptr = (uint32_t*)0x41FB1C;
        
        // Game timers
        uint32_t* game_timer_ptr = (uint32_t*)0x470050;
        uint32_t* round_timer_ptr = (uint32_t*)0x470060;
        
        // Restore ONLY the essential data we actually save (matches SaveCompleteGameState)
        if (!IsBadWritePtr(p1_hp_ptr, sizeof(uint32_t))) *p1_hp_ptr = save_data->p1_hp;
        if (!IsBadWritePtr(p2_hp_ptr, sizeof(uint32_t))) *p2_hp_ptr = save_data->p2_hp;
        if (!IsBadWritePtr(p1_x_ptr, sizeof(uint32_t))) *p1_x_ptr = save_data->p1_x;
        if (!IsBadWritePtr(p1_y_ptr, sizeof(uint16_t))) *p1_y_ptr = save_data->p1_y;
        if (!IsBadWritePtr(p2_x_ptr, sizeof(uint32_t))) *p2_x_ptr = save_data->p2_x;
        if (!IsBadWritePtr(p2_y_ptr, sizeof(uint16_t))) *p2_y_ptr = save_data->p2_y;
        if (!IsBadWritePtr(rng_seed_ptr, sizeof(uint32_t))) *rng_seed_ptr = save_data->rng_seed;
        if (!IsBadWritePtr(game_timer_ptr, sizeof(uint32_t))) *game_timer_ptr = save_data->game_timer;
        if (!IsBadWritePtr(round_timer_ptr, sizeof(uint32_t))) *round_timer_ptr = save_data->round_timer;
        
        // Log critical load data for desync debugging (first 40 frames only)
        if (save_data->frame_number <= 40) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LoadState F%d: P1(HP:%d X:%d Y:%d) P2(HP:%d X:%d Y:%d) RNG:%d GT:%d RT:%d CK:%u", 
                       save_data->frame_number, save_data->p1_hp, save_data->p1_x, save_data->p1_y,
                       save_data->p2_hp, save_data->p2_x, save_data->p2_y, 
                       save_data->rng_seed, save_data->game_timer, save_data->round_timer, save_data->checksum);
        }
        
        return true;
        
    } catch (...) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LoadCompleteGameState: Exception during memory access (frame %d)", save_data->frame_number);
        return false;
    }
}

static void ProcessManualSaveLoadRequests() {
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

// Check for debug commands from launcher via shared memory
static void CheckForDebugCommands() {
    SharedInputData* shared_data = GetSharedMemory();
    if (!shared_data) {
        return; // No shared memory available
    }
    
    // Check for slot-based save state request (launcher uses these fields)
    if (shared_data->debug_save_to_slot_requested && !manual_save_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher requested save to slot %u", shared_data->debug_target_slot);
        manual_save_requested = true;
        shared_data->debug_save_to_slot_requested = false;
    }
    
    // Check for slot-based load state request
    if (shared_data->debug_load_from_slot_requested && !manual_load_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher requested load from slot %u", shared_data->debug_target_slot);
        manual_load_requested = true;
        shared_data->debug_load_from_slot_requested = false;
    }
    
    // Process force rollback requests
    if (shared_data->debug_rollback_frames > 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher requested rollback of %u frames", 
                   shared_data->debug_rollback_frames);
        // TODO: Implement force rollback through GekkoNet
        shared_data->debug_rollback_frames = 0;
    }
    
    // Frame stepping is now handled in Hook_ProcessGameInputs()
    
    // Update enhanced action data for launcher (reduced frequency for performance)
    static uint32_t last_action_update_frame = 0;
    if (g_frame_counter - last_action_update_frame >= 60) {  // Update every 60 frames (0.6 seconds)
        last_action_update_frame = g_frame_counter;
        // Reduced logging frequency
        if (g_frame_counter % 300 == 0) {  // Log every 3 seconds
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Updating enhanced action data at frame %u", g_frame_counter);
        }
        UpdateEnhancedActionData();
    }
}

// Keyboard hotkey handler for save states and frame stepping
static void CheckForHotkeys() {
    SharedInputData* shared_data = GetSharedMemory();
    if (!shared_data) {
        return; // No shared memory available
    }
    
    static bool keys_pressed[256] = {false}; // Track key press states to avoid repeats
    
    // Check for save state hotkeys: Shift+1-8
    bool shift_pressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (shift_pressed) {
        for (int i = 0; i < 8; i++) {
            int vk_key = '1' + i; // VK codes for 1-8
            bool key_currently_pressed = (GetAsyncKeyState(vk_key) & 0x8000) != 0;
            
            if (key_currently_pressed && !keys_pressed[vk_key]) {
                // Save to slot i
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey: Save to slot %d", i);
                if (!manual_save_requested) {
                    manual_save_requested = true;
                    target_save_slot = i;
                }
            }
            keys_pressed[vk_key] = key_currently_pressed;
        }
    }
    
    // Check for load state hotkeys: 1-8 (without Shift)
    if (!shift_pressed) {
        for (int i = 0; i < 8; i++) {
            int vk_key = '1' + i; // VK codes for 1-8
            bool key_currently_pressed = (GetAsyncKeyState(vk_key) & 0x8000) != 0;
            
            if (key_currently_pressed && !keys_pressed[vk_key]) {
                // Load from slot i
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey: Load from slot %d", i);
                if (!manual_load_requested) {
                    manual_load_requested = true;
                    target_load_slot = i;
                }
            }
            keys_pressed[vk_key] = key_currently_pressed;
        }
    }
    
    // Check for pause/resume hotkey: 0
    bool key_0_pressed = (GetAsyncKeyState('0') & 0x8000) != 0;
    if (key_0_pressed && !keys_pressed['0']) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey: Toggle pause/resume");
        if (shared_data->frame_step_is_paused) {
            shared_data->frame_step_resume_requested = true;
        } else {
            shared_data->frame_step_pause_requested = true;
        }
    }
    keys_pressed['0'] = key_0_pressed;
    
    // Check for single step hotkeys: - and +/=
    bool key_minus_pressed = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0; // - key
    bool key_plus_pressed = (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0;   // +/= key
    
    if (key_minus_pressed && !keys_pressed[VK_OEM_MINUS]) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey: Single step advance");
        shared_data->frame_step_single_requested = true;
    }
    keys_pressed[VK_OEM_MINUS] = key_minus_pressed;
    
    if (key_plus_pressed && !keys_pressed[VK_OEM_PLUS]) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey: Single step advance");  
        shared_data->frame_step_single_requested = true;
    }
    keys_pressed[VK_OEM_PLUS] = key_plus_pressed;
    
    // Check for F5 key to toggle hitjudge flag
    bool key_f5_pressed = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (key_f5_pressed && !keys_pressed[VK_F5]) {
        // Toggle hitjudge flag at 0x42470C
        uint8_t* hitjudge_flag = (uint8_t*)0x42470C;
        uint8_t current_value = *hitjudge_flag;
        uint8_t new_value = current_value ? 0 : 1;
        *hitjudge_flag = new_value;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey F5: Toggled hitjudge flag from %d to %d", current_value, new_value);
    }
    keys_pressed[VK_F5] = key_f5_pressed;
}

// New hook for boot-to-character-select hack
// This hook modifies the game's initialization to boot directly to character select screen
// instead of showing the title screen and splash screens. It does this by:
// 1. Setting the character select mode flag to 1 (vs player mode instead of vs CPU)
// 2. Changing the initialization object byte from 0x11 to 0x0A to skip to character select
void ApplyBootToCharacterSelectPatches() {
    // Change initialization object from 0x11 to 0x0A to boot to character select
    uint8_t* init_object_ptr = (uint8_t*)0x409CD9;
    if (!IsBadReadPtr(init_object_ptr, sizeof(uint16_t))) {
        // Make the memory writable
        DWORD old_protect;
        if (VirtualProtect(init_object_ptr, 2, PAGE_EXECUTE_READWRITE, &old_protect)) {
            // Write the instruction: 6A 0A (push 0x0A)
            init_object_ptr[0] = 0x6A;  // push instruction
            init_object_ptr[1] = 0x0A;  // immediate value
            
            // Restore original protection
            VirtualProtect(init_object_ptr, 2, old_protect, &old_protect);
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Wrote instruction 6A 0A at 0x409CD9");
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to make memory writable at 0x409CD9");
        }
    }
    
}

// Hook implementations
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    // FIXED: Return pre-captured inputs to eliminate frame delay
    // Inputs were captured in CaptureRealInputs() BEFORE frame processing started
    
    static int call_count = 0;
    if (++call_count % 100 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hook_GetPlayerInput: player=%d, type=%d, p1=0x%03X, p2=0x%03X",
                   player_id, input_type, live_p1_input & 0x7FF, live_p2_input & 0x7FF);
    }
    
    // Check for true offline mode
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
    
    // Use networked inputs if available (rollback netcode) - but NOT for true offline
    if (!is_true_offline && use_networked_inputs && gekko_initialized && gekko_session) {
        static int input_debug_counter = 0;
        // Disabled verbose input logging
        // if (++input_debug_counter % 100 == 0) {
        //     SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT: Using networked inputs - P%d requested, giving networked value (P1:%d P2:%d)", 
        //                player_id + 1, networked_p1_input, networked_p2_input);
        // }
        
        if (player_id == 0) {
            int converted = ConvertNetworkInputToGameFormat(networked_p1_input);
            return converted;
        } else if (player_id == 1) {
            int converted = ConvertNetworkInputToGameFormat(networked_p2_input);
            return converted;
        }
    }
    
    // Use pre-captured inputs (eliminates 1-frame delay)
    if (player_id == 0) {
        return live_p1_input;
    } else if (player_id == 1) {
        return live_p2_input;
    }
    
    // Fallback to original function if needed
    static int fallback_debug_counter = 0;
    if (++fallback_debug_counter % 200 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT: Using fallback original inputs - P%d", player_id + 1);
    }
    
    return original_get_player_input ? original_get_player_input(player_id, input_type) : 0;
}

// Hook for game_rand function to ensure deterministic RNG
uint32_t __cdecl Hook_GameRand() {
    if (use_deterministic_rng) {
        // Use our own deterministic RNG algorithm (Linear Congruential Generator)
        deterministic_rng_seed = (deterministic_rng_seed * 1103515245 + 12345) & 0x7FFFFFFF;
        uint32_t result = deterministic_rng_seed;
        
        // Mimic the original game_rand behavior (shift right 16, mask to 0x7FFF)
        result = (result >> 16) & 0x7FFF;
        
        return result;
    } else {
        // Use original RNG when not in deterministic mode
        return original_game_rand();
    }
}

int __cdecl Hook_ProcessGameInputs() {
    // Moved CaptureRealInputs() to after pause logic to prevent button consumption
    
    // Check for true offline mode
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);

    // 3.5. CHECK: Wait for all players to be connected before normal gameplay
    // Skip this check for true offline mode (GekkoNet not even initialized) - no network synchronization needed
    if (!is_true_offline && gekko_initialized && gekko_session) {
        // Call AllPlayersValid() which will trigger the deferred GekkoNet start if needed
        bool all_valid = AllPlayersValid();
        
        if (!all_valid) {
            static uint32_t wait_log_counter = 0;
            if (++wait_log_counter % 120 == 0) { // Log every ~2 seconds
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Waiting for all players to connect...");
            }
            // Do NOT call original_process_inputs here. We must freeze the game state completely.
            return 0; // Block further game logic until synchronized.
        }
    }

    // DEBUG: Log when this function is called to find frame controller
    static uint32_t input_call_count = 0;
    if (++input_call_count % 100 == 0) { // Log every 100 calls to avoid spam
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "Hook_ProcessGameInputs() called #%d - frame %u - gekko_frame_control_enabled=%s, gekko_session_started=%s, can_advance_frame=%s", 
            input_call_count, g_frame_counter,
            gekko_frame_control_enabled ? "YES" : "NO",
            gekko_session_started ? "YES" : "NO",
            can_advance_frame ? "YES" : "NO");
    }
    
    // CORRECT APPROACH: Following OnlineSession example - NO BLOCKING
    // Let the game run normally and just process synchronized inputs on AdvanceEvents
    
    // FRAME STEPPING: This is the main control point since it's called repeatedly in the game loop
    // Get shared memory for frame stepping control
    SharedInputData* shared_data = GetSharedMemory();
    
    // Initialize GekkoNet on first input hook call (safer than main loop hook)
    // Initialize GekkoNet early for networking support
    // Skip GekkoNet initialization entirely for true offline mode
    if (!gekko_initialized && !is_true_offline) {
        static bool initialization_attempted = false;
        if (!initialization_attempted) {
            initialization_attempted = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: First call - initializing GekkoNet...");
            
            if (InitializeGekkoNet()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: GekkoNet initialized successfully from input hook");
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: GekkoNet initialization failed");
            }
        }
    } else if (is_true_offline) {
        static bool offline_log_shown = false;
        if (!offline_log_shown) {
            offline_log_shown = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: TRUE OFFLINE mode - skipping GekkoNet initialization completely");
        }
    }
    
    // Wait for GekkoNet connection (moved from main loop hook) - REMOVED, handled by AllPlayersValid() now
    
    // DEBUG: Log that input hook is being called
    static uint32_t input_hook_call_count = 0;
    input_hook_call_count++;
    // Disabled verbose input hook logging
    // if (input_hook_call_count % 100 == 0) { // Log every 100 calls to avoid spam
    //     SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Called %u times, frame %u", input_hook_call_count, g_frame_counter);
    // }
    
    // ARCHITECTURE FIX: Process debug commands (including save/load) BEFORE the pause check
    // This allows save/load to work even when the game is paused
    CheckForDebugCommands();
    CheckForHotkeys(); // Check for keyboard hotkeys for save/load and frame stepping
    ProcessManualSaveLoadRequests();
    
    // Check for frame stepping commands
    if (shared_data) {
        // ONE-TIME-FIX: Handle the initial state where memset sets remaining_frames to 0.
        // This state should mean "running indefinitely".
        static bool initial_state_fixed = false;
        if (!initial_state_fixed && !shared_data->frame_step_is_paused && shared_data->frame_step_remaining_frames == 0) {
            shared_data->frame_step_remaining_frames = UINT32_MAX;
            initial_state_fixed = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Corrected initial frame step state to RUNNING.");
        }

        // DEBUG: Log frame stepping state
        if (shared_data->frame_step_pause_requested || 
            shared_data->frame_step_resume_requested || 
            shared_data->frame_step_single_requested || 
            shared_data->frame_step_multi_count > 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame stepping command detected - pause=%d, resume=%d, single=%d, multi=%u", 
                       shared_data->frame_step_pause_requested,
                       shared_data->frame_step_resume_requested,
                       shared_data->frame_step_single_requested,
                       shared_data->frame_step_multi_count);
        }
        
        // Always log single step requests for debugging
        if (shared_data->frame_step_single_requested) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: SINGLE STEP REQUEST DETECTED at frame %u", g_frame_counter);
        }
        
        // Handle frame stepping commands
        if (shared_data->frame_step_pause_requested) {
            frame_step_paused_global = true;
            shared_data->frame_step_is_paused = true;
            shared_data->frame_step_pause_requested = false;
            shared_data->frame_step_remaining_frames = 0; // No stepping, just pause
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame stepping PAUSED at frame %u", g_frame_counter);
        }
        if (shared_data->frame_step_resume_requested) {
            frame_step_paused_global = false;
            shared_data->frame_step_is_paused = false;
            shared_data->frame_step_resume_requested = false;
            shared_data->frame_step_remaining_frames = UINT32_MAX; // Use sentinel for "running"
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame stepping RESUMED at frame %u", g_frame_counter);
        }
        if (shared_data->frame_step_single_requested) {
            // Single step: run one frame then pause
            shared_data->frame_step_single_requested = false;
            frame_step_paused_global = false; // Allow one frame
            shared_data->frame_step_is_paused = false;
            shared_data->frame_step_remaining_frames = 1; // Allow exactly 1 frame
            shared_data->frame_step_needs_input_refresh = true; // Capture fresh inputs right before execution
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: SINGLE STEP ENABLED - allowing 1 frame at frame %u", g_frame_counter);
        }
        // Multi-step disabled - focus on single step only
        if (shared_data->frame_step_multi_count > 0) {
            shared_data->frame_step_multi_count = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Multi-step disabled - use single step instead");
        }
        
        // If paused, keep input system alive but preserve motion input buffer SURGICALLY
        if (frame_step_paused_global && shared_data->frame_step_is_paused) {
            // SURGICAL APPROACH: Keep input system alive but prevent frame counter advancement
            
            // Save critical state that original_process_inputs modifies
            uint32_t* frame_counter_ptr = (uint32_t*)0x447EE0;  // g_frame_counter
            uint32_t* p1_history_ptr = (uint32_t*)0x4280E0;     // g_player_input_history  
            uint32_t* p2_history_ptr = (uint32_t*)0x4284E0;     // g_p2_input_history (assuming offset)
            
            uint32_t saved_frame_counter = 0;
            uint32_t saved_p1_history = 0;
            uint32_t saved_p2_history = 0;
            
            if (!IsBadReadPtr(frame_counter_ptr, sizeof(uint32_t))) {
                saved_frame_counter = *frame_counter_ptr;
                
                // Save the input history entries that would be overwritten
                uint32_t next_frame_index = (saved_frame_counter + 1) & 0x3FF;
                if (!IsBadReadPtr(p1_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    saved_p1_history = p1_history_ptr[next_frame_index];
                }
                if (!IsBadReadPtr(p2_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    saved_p2_history = p2_history_ptr[next_frame_index];
                }
            }
            
            // Update input system to keep it alive
            CaptureRealInputs();
            if (original_process_inputs) {
                original_process_inputs();
            }
            
            // Restore the critical state to preserve motion inputs
            if (!IsBadWritePtr(frame_counter_ptr, sizeof(uint32_t))) {
                *frame_counter_ptr = saved_frame_counter;  // Undo frame counter advance
                
                // Restore the input history entries
                uint32_t next_frame_index = (saved_frame_counter + 1) & 0x3FF;
                if (!IsBadWritePtr(p1_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    p1_history_ptr[next_frame_index] = saved_p1_history;
                }
                if (!IsBadWritePtr(p2_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    p2_history_ptr[next_frame_index] = saved_p2_history;
                }
            }
            
            return 0; // Block game advancement but keep inputs fresh
        }
        
        // CRITICAL: Inputs are now captured at the top of the function.
        
        // Handle frame stepping countdown AFTER processing the frame
        // This ensures the frame actually gets processed before we count it down
    }
    
    // Normal input capture - but skip if we're going to do fresh capture right before execution
    if (!shared_data || !shared_data->frame_step_needs_input_refresh) {
        static int capture_log = 0;
        if (++capture_log % 30 == 0) {  // More frequent logging
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Calling CaptureRealInputs() - shared_data=%p, frame_step_needs_input_refresh=%s", 
                       shared_data, shared_data ? (shared_data->frame_step_needs_input_refresh ? "YES" : "NO") : "N/A");
        }
        CaptureRealInputs();
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Skipping normal capture, will do fresh capture before execution at frame %u", g_frame_counter);
    }
    
    
    // TRUE OFFLINE MODE: Handle completely without GekkoNet
    if (is_true_offline) {
        
        // FIXED: Increment frame counter BEFORE processing to fix 1-frame input delay
        g_frame_counter++;
        
        // RADICAL SOLUTION: Call original_process_inputs TWICE on step frames to eliminate delay
        if (shared_data && shared_data->frame_step_needs_input_refresh) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: DOUBLE CALL to eliminate 1-frame delay at frame %u", g_frame_counter);
            
            // Capture fresh inputs with detailed logging
            uint32_t old_p1 = live_p1_input;
            uint32_t old_p2 = live_p2_input;
            CaptureRealInputs();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Step capture - P1: 0x%03X->0x%03X, P2: 0x%03X->0x%03X", 
                       old_p1, live_p1_input, old_p2, live_p2_input);
            
            // First call: This "primes" the input system with current inputs
            if (original_process_inputs) {
                original_process_inputs();
            }
            
            // Second call: This ensures the inputs are processed for THIS frame, not next
            if (original_process_inputs) {
                original_process_inputs();
            }
            
            shared_data->frame_step_needs_input_refresh = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Double call complete - inputs should be immediate");
        } else {
            // Normal single call
            if (original_process_inputs) {
                original_process_inputs();
            }
        }
        
        // UNIFIED LOGIC: Handle frame stepping countdown. Re-pausing is now in the render hook.
        if (shared_data && shared_data->frame_step_remaining_frames > 0 && shared_data->frame_step_remaining_frames != UINT32_MAX) {
            // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame ADVANCED to %u during stepping (normal path)", g_frame_counter);
            shared_data->frame_step_remaining_frames--;
            if (shared_data->frame_step_remaining_frames == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Step processing complete for frame %u, will pause in render hook", g_frame_counter);
            }
        }
        
        // TRUE OFFLINE: Early return to prevent double frame execution
        return 0;
    }
    
    
    // CORRECT GEKKONET PROCESSING: Following OnlineSession example pattern
    // Game runs normally, GekkoNet processes events each frame and provides synchronized inputs
    // Skip this for true offline mode (GekkoNet not initialized) - no GekkoNet processing needed
    if (!is_true_offline && gekko_initialized && gekko_session) {
        
        // Send inputs to GekkoNet for all modes (CSS and battle)
        if (css_mode_active) {
            // CSS MODE: Process delayed inputs but send actual inputs to GekkoNet
            ProcessCSSDelayedInputs();
        }
        
        // Always send actual inputs to GekkoNet
        if (is_local_session) {
            uint16_t p1_input = (uint16_t)(live_p1_input & 0x7FF);
            uint16_t p2_input = (uint16_t)(live_p2_input & 0x7FF);
            gekko_add_local_input(gekko_session, p1_player_handle, &p1_input);
            gekko_add_local_input(gekko_session, p2_player_handle, &p2_input);
        } else {
            uint16_t local_input = (::is_host) ? (uint16_t)(live_p1_input & 0x7FF) : (uint16_t)(live_p2_input & 0x7FF);
            gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        }
        gekko_network_poll(gekko_session);
        
        // SECOND: Process GekkoNet events (always process to keep session alive)
        if (gekko_session_started) {
            // 4. EVENTS: Handle session events
        int session_event_count = 0;
        auto session_events = gekko_session_events(gekko_session, &session_event_count);
        for (int i = 0; i < session_event_count; i++) {
            auto event = session_events[i];
            if (event->type == DesyncDetected) {
                auto desync = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet DESYNC detected at frame %d", desync.frame);
                static int desync_count = 0;
                if (++desync_count <= 5) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: CRITICAL DESYNC #%d - frame synchronization may have failed", desync_count);
                }
            } else if (event->type == PlayerDisconnected) {
                auto disco = event->data.disconnected;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "DISCONNECT: handle %d", disco.handle);
            }
        }
        
        // 5. UPDATES: Process game updates (like OnlineSession gekko_update_session)
        int update_count = 0;
        auto updates = gekko_update_session(gekko_session, &update_count);
        
        bool frame_advanced = false;
        for (int i = 0; i < update_count; i++) {
            auto update = updates[i];
            
            switch (update->type) {
                case AdvanceEvent: {
                    // This is the authoritative event that drives the game forward.
                    uint16_t received_p1 = ((uint16_t*)update->data.adv.inputs)[0];
                    uint16_t received_p2 = ((uint16_t*)update->data.adv.inputs)[1];
                    
                    // Log the received inputs to confirm network traffic.
                    static int advance_log_counter = 0;
                    if (++advance_log_counter % 60 == 0) { // Log every second
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet AdvanceEvent: Frame %d - P1_In:0x%03X P2_In:0x%03X", 
                                   update->data.adv.frame, received_p1, received_p2);
                    }
                    
                    // During CSS mode, don't overwrite live inputs - we handle CSS separately
                    if (!css_mode_active) {
                        // BATTLE MODE: Overwrite the live inputs with the synchronized values from GekkoNet.
                        live_p1_input = ConvertNetworkInputToGameFormat(received_p1);
                        live_p2_input = ConvertNetworkInputToGameFormat(received_p2);
                    }
                    // In CSS mode, keep the local inputs as captured

                    // We must now advance the game state using these inputs.
                    g_frame_counter++;
                    if (original_process_inputs) {
                        original_process_inputs();
                    }
                    frame_advanced = true;
                    
                    // Allow the next frame to advance
                    can_advance_frame = true;

                    break;
                }
                case SaveEvent: {
                    // Check if we're in battle mode before processing SaveEvent
                    // Read multiple game mode addresses to diagnose the issue
                    uint32_t* fm2k_mode_ptr = (uint32_t*)0x470040;  // g_fm2k_game_mode
                    uint16_t* game_mode_ptr = (uint16_t*)0x470054;  // g_game_mode  
                    
                    uint32_t fm2k_mode = 0;
                    uint16_t game_mode = 0;
                    bool fm2k_readable = false;
                    bool game_readable = false;
                    
                    // Read fm2k_game_mode (0x470040)
                    if (!IsBadReadPtr(fm2k_mode_ptr, sizeof(uint32_t))) {
                        fm2k_mode = *fm2k_mode_ptr;
                        fm2k_readable = true;
                    }
                    
                    // Read g_game_mode (0x470054) 
                    if (!IsBadReadPtr(game_mode_ptr, sizeof(uint16_t))) {
                        game_mode = *game_mode_ptr;
                        game_readable = true;
                    }
                    
                    // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet SaveEvent frame %d: fm2k_mode=%d (%s), game_mode=%d (%s)", 
                    //            update->data.save.frame, 
                    //            fm2k_mode, fm2k_readable ? "readable" : "unreadable",
                    //            game_mode, game_readable ? "readable" : "unreadable");
                    
                    // Use game_mode for battle detection (3000 = battle)
                    bool in_battle_mode = game_readable && (game_mode == 3000 || fm2k_mode == 3000);
                    uint32_t current_mode = game_readable ? game_mode : fm2k_mode;
                    
                    // Only process saves during battle mode (3000)
                    if (!in_battle_mode) {
                        static int skip_log_counter = 0;
                        // Only log every 100 skipped saves (about every 1-2 seconds)
                        if (++skip_log_counter % 100 == 0) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Skipping SaveEvent frames - not in battle mode (fm2k_mode: %d, game_mode: %d)",
                                       fm2k_mode, game_mode);
                        }
                        break;
                    }
                    
                    // Battle sync is now handled earlier during CSS->Battle transition
                    // This ensures frame synchronization happens BEFORE any SaveStates occur
                    
                    // Save current game state for rollback using local static storage (no shared memory)
                    // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet SaveEvent: Processing frame %d in battle mode", update->data.save.frame);
                    
                    // Use local static storage for rollback saves (avoids shared memory crashes)
                    static SaveStateData local_rollback_slots[16];
                    
                    // Use frame-based slot selection for rollback saves (16 slots available)
                    uint32_t rollback_slot = update->data.save.frame % 16;
                    
                    // Get the dedicated rollback slot from local storage
                    SaveStateData* rollback_save_slot = &local_rollback_slots[rollback_slot];
                    
                    // Save comprehensive game state directly to rollback slot
                    // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Calling SaveCompleteGameState for frame %d...", update->data.save.frame);
                    if (SaveCompleteGameState(rollback_save_slot, update->data.save.frame)) {
                        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SaveCompleteGameState returned success, copying to GekkoNet buffer...");
                        
                        void* state_buffer = update->data.save.state;
                        size_t* state_len = update->data.save.state_len;
                        uint32_t* checksum = update->data.save.checksum;
                        
                        // Copy expanded essential data for proper desync detection
                        struct ExpandedEssentialData {
                            uint32_t p1_hp, p2_hp;
                            uint32_t p1_x, p2_x;
                            uint16_t p1_y, p2_y;
                            uint32_t rng_seed;
                            uint32_t game_timer, round_timer;
                            uint32_t frame_number;
                        } essential_data;
                        
                        essential_data.p1_hp = rollback_save_slot->p1_hp;
                        essential_data.p2_hp = rollback_save_slot->p2_hp;
                        essential_data.p1_x = rollback_save_slot->p1_x;
                        essential_data.p2_x = rollback_save_slot->p2_x;
                        essential_data.p1_y = rollback_save_slot->p1_y;
                        essential_data.p2_y = rollback_save_slot->p2_y;
                        essential_data.rng_seed = rollback_save_slot->rng_seed;
                        essential_data.game_timer = rollback_save_slot->game_timer;
                        essential_data.round_timer = rollback_save_slot->round_timer;
                        essential_data.frame_number = rollback_save_slot->frame_number;
                        
                        size_t essential_size = sizeof(essential_data); // Now ~38 bytes
                        *state_len = essential_size;
                        
                        std::memcpy(state_buffer, &essential_data, essential_size);
                        
                        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Setting checksum to %u", rollback_save_slot->checksum);
                        *checksum = rollback_save_slot->checksum;
                        
                        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Saved COMPLETE rollback state for frame %d to local slot %d",
                        //            update->data.save.frame, rollback_slot);
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Rollback save failed for frame %d",
                                    update->data.save.frame);
                    }
                    break;
                }
                case LoadEvent: {
                    // Restore game state for rollback using local static storage (no shared memory)
                    // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet LoadEvent: frame %d", update->data.load.frame);
                    
                    // Use same local static storage as SaveEvent (avoids shared memory crashes)
                    static SaveStateData local_rollback_slots[16];
                    
                    // Use frame-based slot selection for rollback loads (16 slots available)
                    uint32_t rollback_slot = update->data.load.frame % 16;
                    SaveStateData* rollback_save_slot = &local_rollback_slots[rollback_slot];
                    
                    // Copy GekkoNet's state to our local rollback slot
                    void* state_buffer = update->data.load.state;
                    std::memcpy(rollback_save_slot, state_buffer, sizeof(SaveStateData));
                    
                    // Load comprehensive game state directly from rollback slot
                    if (LoadCompleteGameState(rollback_save_slot)) {
                        // Update frame counter to match the loaded state
                        g_frame_counter = update->data.load.frame;
                        
                        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Loaded COMPLETE rollback state for frame %d from local slot %d",
                        //            update->data.load.frame, rollback_slot);
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Rollback load failed for frame %d",
                                    update->data.load.frame);
                    }
                    break;
                }
            }
        }
        
        // If GekkoNet didn't advance the frame (e.g., waiting for remote input),
        // we must not advance it ourselves. Return here to keep the state frozen.
        if (!frame_advanced) {
            return 0;
        }
    } else if (css_mode_active) {
        // CSS MODE: Process frame with local inputs (no GekkoNet frame advancement)
        g_frame_counter++;
        if (original_process_inputs) {
            original_process_inputs();
        }
        return 0;
    } else {
        // Session not started yet - just return, no frame processing
        return 0;
    }
    }
    
    // Handle frame stepping countdown for GekkoNet path
    if (shared_data && shared_data->frame_step_remaining_frames > 0 && shared_data->frame_step_remaining_frames != UINT32_MAX) {
        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame ADVANCED to %u during stepping (GekkoNet path)", g_frame_counter);
        shared_data->frame_step_remaining_frames--;
        if (shared_data->frame_step_remaining_frames == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Step processing complete for frame %u, will pause in render hook", g_frame_counter);
        }
    }
    
    // CRITICAL: Reset networked input flag AFTER frame processing is complete
    // This ensures networked inputs are used for the entire frame when AdvanceEvents arrive
    if (use_networked_inputs) {
        use_networked_inputs = false;  // No logging for performance
    }
    
    // Keep essential non-GekkoNet processing
    if (shared_data) {
        // Enhanced action data for launcher analysis
        if (g_frame_counter % 10 == 0) {
            UpdateEnhancedActionData();
        }
        
        // Auto-save functionality
        if (shared_data->auto_save_enabled) {
            if ((g_frame_counter - hook_last_auto_save_frame) >= shared_data->auto_save_interval_frames) {
                manual_save_requested = true;
                shared_data->debug_target_slot = 0;
                hook_last_auto_save_frame = g_frame_counter;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "AUTO-SAVE triggered: slot 0, frame %u", g_frame_counter);
            }
        }
    }
    
    // Call original function to process the game's input system
    // GekkoNet controls WHEN frames advance, but the game still needs to process inputs normally
    if (original_process_inputs) {
        return original_process_inputs();
    }
    
    return 0;
}

int __cdecl Hook_UpdateGameState() {
    // Check for GekkoNet session readiness.
    // If not ready, block this part of the game loop to prevent desync.
    // We check the 'gekko_session_started' flag, which is set by the input hook.
    // This prevents polling the network twice per frame.
    if (gekko_initialized && gekko_session && !gekko_session_started) {
        return 0; // Block game state updates
    }

    // DEBUG: Log when this function is called to find frame controller
    static uint32_t update_call_count = 0;
    update_call_count++; // Still increment counter even if not logging
    // Disabled verbose update logging
    // if (++update_call_count % 50 == 0) { // Log every 50 calls to avoid spam
    //     SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
    //         "Hook_UpdateGameState() called #%d - frame %u - gekko_frame_control_enabled=%s, can_advance_frame=%s", 
    //         update_call_count, g_frame_counter,
    //         gekko_frame_control_enabled ? "YES" : "NO",
    //         can_advance_frame ? "YES" : "NO");
    // }
    
    // NO BLOCKING: Following OnlineSession pattern - let game run freely
    // GekkoNet synchronization is handled in Hook_ProcessGameInputs via AdvanceEvents
    
    // FRAME STEPPING: Block game state updates when paused, BUT completely bypass hook during step frames
    SharedInputData* shared_data = GetSharedMemory();
    if (shared_data && frame_step_paused_global && shared_data->frame_step_is_paused) {
        return 0; // Block game state updates when truly paused
    }
    
    // BYPASS HOOK ENTIRELY during step frames - let original function run without interference
    if (shared_data && shared_data->frame_step_remaining_frames > 0 && shared_data->frame_step_remaining_frames != UINT32_MAX) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "UPDATE HOOK: BYPASSING hook during step frame %u - calling original directly", g_frame_counter);
        return original_update_game ? original_update_game() : 0;
    }
    
    // Only monitor state transitions every 30 frames
    static uint32_t state_check_counter = 0;
    if (++state_check_counter % 30 == 0) {
        MonitorGameStateTransitions();
        
        // Debug log current mode
        if (state_check_counter % 300 == 0) { // Every 10 seconds
            uint32_t* game_mode_ptr = (uint32_t*)FM2K::State::Memory::GAME_MODE_ADDR;
            uint32_t game_mode = IsBadReadPtr(game_mode_ptr, sizeof(uint32_t)) ? 0xFFFFFFFF : *game_mode_ptr;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "STATE CHECK: game_mode=0x%08X, css_mode_active=%s", 
                       game_mode, css_mode_active ? "YES" : "NO");
        }
    }
    
    // NO BLOCKING: Let game state updates run normally even during GekkoNet initialization
    
    if (original_update_game) {
        return original_update_game();
    }
    return 0;
}

// Render hook - allow rendering even when paused for visual feedback
void __cdecl Hook_RenderGame() {
    SharedInputData* shared_data = GetSharedMemory();
    
    // FRAME STEPPING: Re-pause after a step has finished.
    // This is done in the render hook to ensure that the game state for the stepped frame
    // has been fully updated before the pause is re-engaged.
    if (shared_data && !shared_data->frame_step_is_paused && shared_data->frame_step_remaining_frames == 0) {
        frame_step_paused_global = true;
        shared_data->frame_step_is_paused = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RENDER HOOK: Step complete, PAUSING at frame %u", g_frame_counter);
    }

    // We always render to give visual feedback, even when paused.
    if (original_render_game) {
        original_render_game();
    }
}

BOOL __cdecl Hook_RunGameLoop() {
    // FIRST LINE: Always log when this function is called to verify hook installation
    static uint32_t run_loop_call_count = 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "Hook_RunGameLoop() called #%d - This hook is only called ONCE at startup, not per frame", 
        ++run_loop_call_count);
    
    // Set character select mode flag after memory clearing (preserve existing functionality)
    uint8_t* char_select_mode_ptr = (uint8_t*)FM2K::State::Memory::CHARACTER_SELECT_MODE_ADDR;
    if (!IsBadReadPtr(char_select_mode_ptr, sizeof(uint8_t))) {
        DWORD old_protect;
        if (VirtualProtect(char_select_mode_ptr, sizeof(uint8_t), PAGE_READWRITE, &old_protect)) {
            *char_select_mode_ptr = 1;
            VirtualProtect(char_select_mode_ptr, sizeof(uint8_t), old_protect, &old_protect);
        }
    }
    
    // NOTE: Frame blocking logic moved to Hook_ProcessGameInputs() - the real frame controller
    // This hook (Hook_RunGameLoop) is only called once at startup, not per frame
    
    // Always call original function - no blocking logic here
    return original_run_game_loop ? original_run_game_loop() : FALSE;
}

bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing MinHook...");
    
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: MH_Initialize failed: %d", mh_init);
        return false;
    }
    
    // Check each address individually for better debugging
    bool process_inputs_valid = !IsBadCodePtr((FARPROC)FM2K::State::Memory::PROCESS_INPUTS_ADDR);
    bool get_input_valid = !IsBadCodePtr((FARPROC)FM2K::State::Memory::GET_PLAYER_INPUT_ADDR);
    bool update_game_valid = !IsBadCodePtr((FARPROC)FM2K::State::Memory::UPDATE_GAME_ADDR);
    bool run_loop_valid = !IsBadCodePtr((FARPROC)FM2K::State::Memory::RUN_GAME_LOOP_ADDR);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Address validation:");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  PROCESS_INPUTS_ADDR (0x%08X): %s", 
               FM2K::State::Memory::PROCESS_INPUTS_ADDR, process_inputs_valid ? "VALID" : "INVALID");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  GET_PLAYER_INPUT_ADDR (0x%08X): %s", 
               FM2K::State::Memory::GET_PLAYER_INPUT_ADDR, get_input_valid ? "VALID" : "INVALID");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  UPDATE_GAME_ADDR (0x%08X): %s", 
               FM2K::State::Memory::UPDATE_GAME_ADDR, update_game_valid ? "VALID" : "INVALID");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  RUN_GAME_LOOP_ADDR (0x%08X): %s", 
               FM2K::State::Memory::RUN_GAME_LOOP_ADDR, run_loop_valid ? "VALID" : "INVALID");
    
    if (!process_inputs_valid || !get_input_valid || !update_game_valid || !run_loop_valid) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: One or more target addresses are invalid or not yet mapped");
        return false;
    }
    
    void* inputFuncAddr = (void*)FM2K::State::Memory::PROCESS_INPUTS_ADDR;
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
    
    void* getInputFuncAddr = (void*)FM2K::State::Memory::GET_PLAYER_INPUT_ADDR;
    MH_STATUS status_getinput = MH_CreateHook(getInputFuncAddr, (void*)Hook_GetPlayerInput, (void**)&original_get_player_input);
    if (status_getinput != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create get_player_input hook: %d", status_getinput);
        MH_Uninitialize();
        return false;
    }

    MH_STATUS enable_getinput = MH_EnableHook(getInputFuncAddr);
    if (enable_getinput != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable get_player_input hook: %d", enable_getinput);
        MH_Uninitialize();
        return false;
    }
    
    void* updateFuncAddr = (void*)FM2K::State::Memory::UPDATE_GAME_ADDR;
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
    
    // Re-enable main loop hook for CSS flag setting (after game memzeros)
    void* runGameLoopFuncAddr = (void*)FM2K::State::Memory::RUN_GAME_LOOP_ADDR;
    MH_STATUS status3 = MH_CreateHook(runGameLoopFuncAddr, (void*)Hook_RunGameLoop, (void**)&original_run_game_loop);
    if (status3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create run_game_loop hook: %d", status3);
        MH_Uninitialize();
        return false;
    }
    
    // Enable main loop hook for CSS flag setting
    MH_STATUS enable3 = MH_EnableHook(runGameLoopFuncAddr);
    if (enable3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable run_game_loop hook: %d", enable3);
        MH_Uninitialize();
        return false;
    }
    
    // Function pointers for main loop implementation (not used in current approach)
    original_render_game = (RenderGameFunc)0x404DD0;
    original_process_input_history = (ProcessInputHistoryFunc)0x4025A0;
    original_check_game_continue = (CheckGameContinueFunc)0x402600;
    
    // Install render hook for frame stepping
    void* renderFuncAddr = (void*)0x404DD0;
    MH_STATUS status4 = MH_CreateHook(renderFuncAddr, (void*)Hook_RenderGame, (void**)&original_render_game);
    if (status4 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create render hook: %d", status4);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable4 = MH_EnableHook(renderFuncAddr);
    if (enable4 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable render hook: %d", enable4);
        MH_Uninitialize();
        return false;
    }
    
    // Install game_rand hook for deterministic RNG
    void* gameRandFuncAddr = (void*)0x417A22;
    MH_STATUS status5 = MH_CreateHook(gameRandFuncAddr, (void*)Hook_GameRand, (void**)&original_game_rand);
    if (status5 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create game_rand hook: %d", status5);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable5 = MH_EnableHook(gameRandFuncAddr);
    if (enable5 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable game_rand hook: %d", enable5);
        MH_Uninitialize();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: game_rand hook installed for deterministic RNG");
    
    // Apply boot-to-character-select patches directly
    ApplyBootToCharacterSelectPatches();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: BSNES-level architecture installed successfully!");
    
    // DEBUG: Test that hooks are working by logging when they're first called
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Waiting for first hook calls to verify installation...");
    
    return true;
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hooks shut down");
}

// Game state monitoring implementation
void MonitorGameStateTransitions() {
    // Read current game mode values from memory
    uint32_t* game_mode_ptr = (uint32_t*)FM2K::State::Memory::GAME_MODE_ADDR;
    uint32_t* fm2k_mode_ptr = (uint32_t*)FM2K::State::Memory::FM2K_GAME_MODE_ADDR;
    uint32_t* char_select_ptr = (uint32_t*)FM2K::State::Memory::CHARACTER_SELECT_MODE_ADDR;
    
    // Safely read values
    uint32_t new_game_mode = 0xFFFFFFFF;
    uint32_t new_fm2k_mode = 0xFFFFFFFF;
    uint32_t new_char_select = 0xFFFFFFFF;
    
    if (!IsBadReadPtr(game_mode_ptr, sizeof(uint32_t))) {
        new_game_mode = *game_mode_ptr;
    }
    if (!IsBadReadPtr(fm2k_mode_ptr, sizeof(uint32_t))) {
        new_fm2k_mode = *fm2k_mode_ptr;
    }
    if (!IsBadReadPtr(char_select_ptr, sizeof(uint32_t))) {
        new_char_select = *char_select_ptr;
    }
    
    // Update the game state machine with current mode
    if (new_game_mode != 0xFFFFFFFF) {
        FM2K::State::g_game_state_machine.Update(new_game_mode);
    }
    
    // Check for state transitions and log them
    bool state_changed = false;
    if (new_game_mode != current_game_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: game_mode changed from %s (0x%08X) to %s (0x%08X)", 
                   GetGameModeString(current_game_mode), current_game_mode,
                   GetGameModeString(new_game_mode), new_game_mode);
        
        // Handle CSS mode transitions based on game_mode (not fm2k_mode)
        HandleCSSModeTransition(current_game_mode, new_game_mode);
        
        current_game_mode = new_game_mode;
        state_changed = true;
    }
    
    if (new_fm2k_mode != current_fm2k_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: fm2k_mode changed from 0x%08X to 0x%08X", 
                   current_fm2k_mode, new_fm2k_mode);
        
        current_fm2k_mode = new_fm2k_mode;
        state_changed = true;
    }
    
    if (new_char_select != current_char_select_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: char_select_mode changed from 0x%08X to 0x%08X", 
                   current_char_select_mode, new_char_select);
        current_char_select_mode = new_char_select;
        state_changed = true;
    }
    
    // Manage rollback activation based on state changes
    if (state_changed) {
        ManageRollbackActivation(new_game_mode, new_fm2k_mode, new_char_select);
    }
    
    // Mark as initialized after first read
    if (!game_state_initialized) {
        game_state_initialized = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: Initial state - game_mode=0x%08X, fm2k_mode=0x%08X, char_select=0x%08X", 
                   new_game_mode, new_fm2k_mode, new_char_select);
    }
}

void ManageRollbackActivation(uint32_t game_mode, uint32_t fm2k_mode, uint32_t char_select_mode) {
    // SIMPLIFIED: Remove all CSS filtering and state machine interference
    // Keep GekkoNet control active throughout the entire session
    
    // Always enable frame sync when GekkoNet is initialized
    if (gekko_initialized && gekko_session_started && !waiting_for_gekko_advance) {
        waiting_for_gekko_advance = true;
        rollback_active = true;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "FM2K STATE: GekkoNet control ALWAYS ACTIVE - no CSS filtering (game_mode=0x%X)", 
            game_mode);
    }
    
    // Never disable GekkoNet control - remove all state machine interference
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "FM2K STATE: Maintaining continuous GekkoNet sync (game_mode=0x%X, rollback_active=%s)", 
        game_mode, rollback_active ? "YES" : "NO");
}

bool ShouldActivateRollback(uint32_t game_mode, uint32_t fm2k_mode) {
    // SIMPLIFIED: Always return true - no CSS filtering
    // Keep rollback active throughout the entire session
    return true;
}

const char* GetGameModeString(uint32_t mode) {
    switch (mode) {
        case 0xFFFFFFFF: return "UNINITIALIZED";
        case 0x0: return "STARTUP";
        default:
            if (mode >= 1000 && mode < 2000) return "TITLE_SCREEN";
            if (mode >= 2000 && mode < 3000) return "CHARACTER_SELECT";
            if (mode >= 3000 && mode < 4000) return "IN_BATTLE";
            return "UNKNOWN";
    }
}

// CSS management functions
void HandleCSSModeTransition(uint32_t old_mode, uint32_t new_mode) {
    bool was_css = (old_mode >= 2000 && old_mode < 3000);
    bool is_css = (new_mode >= 2000 && new_mode < 3000);
    
    if (!was_css && is_css) {
        // Entering CSS mode
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Entering character select mode (game_mode: 0x%08X)", new_mode);
        css_mode_active = true;
        
        // Reset battle sync flag when entering CSS (allows re-sync for next battle)
        battle_sync_done = false;
        
        // Disable deterministic RNG when leaving battle
        use_deterministic_rng = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Reset battle sync flag and disabled deterministic RNG for fresh battle start");
        
        
    } else if (was_css && !is_css) {
        // Leaving CSS mode  
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Leaving character select mode (game_mode: 0x%08X)", new_mode);
        css_mode_active = false;
        
        
        if (new_mode >= 3000 && new_mode < 4000) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Transitioning to battle - enabling networked inputs");
            
            // Enable networked inputs for battle mode
            use_networked_inputs = true;
            
            // CRITICAL: Synchronize RNG seed when battle starts to prevent desyncs
            uint32_t* rng_seed_ptr = (uint32_t*)0x41FB1C;
            uint32_t* game_timer_ptr = (uint32_t*)0x470050;
            
            // Force both clients to the same deterministic starting state
            uint32_t sync_rng_seed = 12345678; // Fixed seed for both clients
            uint32_t sync_game_timer = 10000;  // Fixed timer for both clients
            
            if (!IsBadWritePtr(rng_seed_ptr, sizeof(uint32_t))) {
                *rng_seed_ptr = sync_rng_seed;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "BATTLE SYNC: Set RNG seed to %u", sync_rng_seed);
            }
            
            if (!IsBadWritePtr(game_timer_ptr, sizeof(uint32_t))) {
                *game_timer_ptr = sync_game_timer;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "BATTLE SYNC: Set game timer to %u", sync_game_timer);
            }
            
            // CRITICAL: Synchronize frame counters BEFORE GekkoNet starts saving states
            uint32_t sync_frame = 100;
            
            // Frame counters already synchronized at GekkoNet startup
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "BATTLE SYNC: Using frame counters synchronized at GekkoNet startup");
            
            // Enable deterministic RNG to prevent future desync from random values
            deterministic_rng_seed = sync_rng_seed;
            use_deterministic_rng = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "BATTLE SYNC: Enabled deterministic RNG with seed %u", sync_rng_seed);
            
            // CRITICAL: Reset and restart GekkoNet rollback state with synchronized frame counters
            if (gekko_initialized) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "BATTLE SYNC: Resetting GekkoNet rollback state with synchronized frame counters");
                
                // Force a clean restart of GekkoNet's rollback system
                gekko_session_started = false;
                gekko_frame_control_enabled = false;
                waiting_for_gekko_advance = false;
                rollback_active = false;
                
                // Small delay to ensure reset takes effect
                SDL_Delay(50);
                
                // Restart with synchronized state
                gekko_session_started = true;
                gekko_frame_control_enabled = true;
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "BATTLE SYNC: GekkoNet rollback state restarted with synchronized frame counters");
            }
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Battle mode activated - GekkoNet will now control inputs");
        }
    }
}


// CSS Input Injection System Implementation
void ProcessCSSDelayedInputs() {
    for (int i = 0; i < 2; i++) {
        if (css_delayed_inputs[i].active && css_delayed_inputs[i].frames_remaining > 0) {
            // Inject the input this frame
            InjectPlayerInput(i, css_delayed_inputs[i].input_value);
            css_delayed_inputs[i].frames_remaining--;
            
            if (css_delayed_inputs[i].frames_remaining == 0) {
                css_delayed_inputs[i].active = false;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Input injection completed for player %d", i);
            }
        }
    }
}

void QueueCSSDelayedInput(int player, uint16_t input, uint8_t delay_frames) {
    if (player >= 0 && player < 2) {
        css_delayed_inputs[player].input_value = input;
        css_delayed_inputs[player].frames_remaining = delay_frames;
        css_delayed_inputs[player].active = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Queued input 0x%X for player %d (delay: %d frames)", 
                   input, player, delay_frames);
    }
}

uint16_t ExtractColorButton(uint16_t input_flags) {
    // Extract color button from input flags (0x3F0 mask covers 0x010-0x200)
    if (input_flags & 0x010) return 0x010;   // Button A (bit 4)
    if (input_flags & 0x020) return 0x020;   // Button B (bit 5)
    if (input_flags & 0x040) return 0x040;   // Button C (bit 6)
    if (input_flags & 0x080) return 0x080;   // Button D (bit 7)
    if (input_flags & 0x100) return 0x100;   // Button E (bit 8)
    if (input_flags & 0x200) return 0x200;   // Button F (bit 9)
    return 0x00; // No color button pressed
}

void InjectPlayerInput(int player, uint16_t input_value) {
    // Inject directly into the live input variables that the game uses
    if (player == 0) {
        live_p1_input |= input_value;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Injected button 0x%X into live_p1_input (result: 0x%03X)", 
                   input_value, live_p1_input & 0x7FF);
    } else if (player == 1) {
        live_p2_input |= input_value;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Injected button 0x%X into live_p2_input (result: 0x%03X)", 
                   input_value, live_p2_input & 0x7FF);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Invalid player %d for injection", player);
    }
} 