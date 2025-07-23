#include "hooks.h"
#include "globals.h"
#include "gekkonet_hooks.h"
#include "state_manager.h"
#include "logging.h"
#include "shared_mem.h"
#include "game_state_machine.h"
// #include "css_sync.h"  // Removed CSS filtering
#include "object_tracker.h"
#include "object_analysis.h"
#include "object_pool_scanner.h"
// #include "boot_object_analyzer.cpp"  // REMOVED: Performance optimization
#include <windows.h>
#include <mmsystem.h>
#include <limits>
#include <exception>

// Global variables for manual save/load requests
static bool manual_save_requested = false;
static bool manual_load_requested = false;
static uint32_t target_save_slot = 0;
static uint32_t target_load_slot = 0;

// Auto-save tracking (separate from globals.h version)
static uint32_t hook_last_auto_save_frame = 0;

// ARCHITECTURE FIX: Real input capture following CCCaster/GekkoNet pattern
static void CaptureRealInputs() {
    // DIRECT INPUT CAPTURE: Read inputs directly from hardware to eliminate frame delay
    // This captures inputs BEFORE the game processes them, not after
    
    uint32_t p1_input = 0;
    uint32_t p2_input = 0;
    
    // Read Player 1 inputs directly from keyboard
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) p1_input |= 0x001;     // LEFT
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) p1_input |= 0x002;    // RIGHT  
    if (GetAsyncKeyState(VK_UP) & 0x8000) p1_input |= 0x004;       // UP
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) p1_input |= 0x008;     // DOWN
    if (GetAsyncKeyState('Z') & 0x8000) p1_input |= 0x010;         // BUTTON1
    if (GetAsyncKeyState('X') & 0x8000) p1_input |= 0x020;         // BUTTON2
    if (GetAsyncKeyState('C') & 0x8000) p1_input |= 0x040;         // BUTTON3
    if (GetAsyncKeyState('A') & 0x8000) p1_input |= 0x080;         // BUTTON4
    if (GetAsyncKeyState('S') & 0x8000) p1_input |= 0x100;         // BUTTON5
    if (GetAsyncKeyState('D') & 0x8000) p1_input |= 0x200;         // BUTTON6
    if (GetAsyncKeyState('Q') & 0x8000) p1_input |= 0x400;         // BUTTON7
    
    // TODO: Add Player 2 controls (WASD + different keys)
    // For now, P2 stays as previous input or 0
    
    // Store captured inputs for this frame
    live_p1_input = p1_input;
    live_p2_input = p2_input;
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
            uint16_t* game_mode_ptr = (uint16_t*)0x00470054;
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
                save_slot->game_mode = *game_mode_ptr;
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
            uint16_t* game_mode_ptr = (uint16_t*)0x00470054;
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
                *game_mode_ptr = save_slot->game_mode;
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
    
    // Use networked inputs if available (rollback netcode)
    if (use_networked_inputs && gekko_initialized && gekko_session) {
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

int __cdecl Hook_ProcessGameInputs() {
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
    if (!gekko_initialized) {
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
    }
    
    // Wait for GekkoNet connection (moved from main loop hook)
    if (gekko_initialized && gekko_session && !gekko_session_started) {
        // Check if this is true offline mode - no network handshake needed
        char* env_offline = getenv("FM2K_TRUE_OFFLINE");
        bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
        
        if (is_true_offline) {
            // TRUE OFFLINE: No network handshake needed, start immediately
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: TRUE OFFLINE mode - starting session immediately (no network handshake)");
            gekko_session_started = true;
        } else {
            // ONLINE/LOCALHOST: Wait for network handshake
            static uint32_t connection_attempts = 0;
            static uint32_t last_log_attempt = 0;
            
            // AllPlayersValid() handles all the polling and event processing
            if (!AllPlayersValid()) {
                connection_attempts++;
                
                // Log every 50 attempts for faster feedback (was 100)
                if (connection_attempts - last_log_attempt >= 50) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Waiting for GekkoNet connection... attempt %u (player_index=%d, is_host=%s)", 
                               connection_attempts, ::player_index, ::is_host ? "YES" : "NO");
                    last_log_attempt = connection_attempts;
                }
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: GekkoNet connected! Session ready.");
            }
        }
    }
    
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
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: SINGLE STEP ENABLED - allowing 1 frame at frame %u", g_frame_counter);
        }
        // Multi-step disabled - focus on single step only
        if (shared_data->frame_step_multi_count > 0) {
            shared_data->frame_step_multi_count = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Multi-step disabled - use single step instead");
        }
        
        // If paused, block frame processing
        if (frame_step_paused_global && shared_data->frame_step_is_paused) {
            // Don't call original function - this effectively pauses the game
            return 0; // Block frame processing completely
        }
        
        // Handle frame stepping countdown AFTER processing the frame
        // This ensures the frame actually gets processed before we count it down
    }
    
    // In lockstep/rollback mode, the game's frame advancement is handled inside the AdvanceEvent.
    // We do nothing here to allow GekkoNet to control the frame pacing.
    if (!waiting_for_gekko_advance) {
        // FIXED: Increment frame counter BEFORE processing to fix 1-frame input delay
        g_frame_counter++;
        
        // Call the original function to let the game run normally.
        if (original_process_inputs) {
            original_process_inputs();
        }
        
        // UNIFIED LOGIC: Handle frame stepping countdown. Re-pausing is now in the render hook.
        if (shared_data && shared_data->frame_step_remaining_frames > 0 && shared_data->frame_step_remaining_frames != UINT32_MAX) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame ADVANCED to %u during stepping (normal path)", g_frame_counter);
            shared_data->frame_step_remaining_frames--;
            if (shared_data->frame_step_remaining_frames == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Step processing complete for frame %u, will pause in render hook", g_frame_counter);
            }
        }
        
        // CRITICAL: Early return to prevent double frame execution
        return 0;
    }
    
    
    // CORRECT GEKKONET PROCESSING: Following OnlineSession example pattern
    // Game runs normally, GekkoNet processes events each frame and provides synchronized inputs
    if (gekko_initialized && gekko_session && gekko_session_started) {
        // 1. CAPTURE: Read local inputs (like OnlineSession get_key_inputs)
        CaptureRealInputs();
        
        // 2. SEND: Send inputs to GekkoNet (like OnlineSession gekko_add_local_input)
        if (is_local_session) {
            // LOCAL SESSION: Send BOTH player inputs
            uint16_t p1_input = (uint16_t)(live_p1_input & 0x7FF);
            uint16_t p2_input = (uint16_t)(live_p2_input & 0x7FF);
            gekko_add_local_input(gekko_session, p1_player_handle, &p1_input);
            gekko_add_local_input(gekko_session, p2_player_handle, &p2_input);
        } else {
            // ONLINE SESSION: Send only local player input
            uint16_t local_input;
            if (::is_host) {
                local_input = (uint16_t)(live_p1_input & 0x7FF);
            } else {
                local_input = (uint16_t)(live_p2_input & 0x7FF);
            }
            gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        }
        
        // 3. POLL: Network polling (like OnlineSession gekko_network_poll)
        gekko_network_poll(gekko_session);
        
        // 3.5. CHECK: Wait for all players to be connected before normal gameplay
        static bool all_players_connected = false;
        static bool waiting_for_connection = true;
        
        if (!all_players_connected && !is_local_session && waiting_for_connection) {
            // BLOCK P1 from running game loop until P2 connects
            // Check if we're getting actual network communication (AdvanceEvents)
            int update_count_check = 0;
            auto updates_check = gekko_update_session(gekko_session, &update_count_check);
            
            bool got_advance_events = false;
            for (int i = 0; i < update_count_check; i++) {
                if (updates_check[i]->type == AdvanceEvent) {
                    got_advance_events = true;
                    break;
                }
            }
            
            if (got_advance_events) {
                all_players_connected = true;
                waiting_for_connection = false;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: All players connected - starting synchronized gameplay");
            } else {
                // BLOCK: Don't advance FM2K until P2 connects
                static int wait_log_counter = 0;
                if (++wait_log_counter % 120 == 0) {  // Log every 2 seconds
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: P1 waiting for P2 to connect...");
                }
                return 0; // Don't advance FM2K - block until P2 connects
            }
        } else if (is_local_session) {
            // Local sessions are always "connected"
            all_players_connected = true;
            waiting_for_connection = false;
        }
        
        // 4. EVENTS: Handle session events
        int session_event_count = 0;
        auto session_events = gekko_session_events(gekko_session, &session_event_count);
        for (int i = 0; i < session_event_count; i++) {
            auto event = session_events[i];
            if (event->type == DesyncDetected) {
                auto desync = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DESYNC: frame %d", desync.frame);
            } else if (event->type == PlayerDisconnected) {
                auto disco = event->data.disconnected;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "DISCONNECT: handle %d", disco.handle);
            }
        }
        
        // 5. UPDATES: Process game updates (like OnlineSession gekko_update_session)
        int update_count = 0;
        auto updates = gekko_update_session(gekko_session, &update_count);
        
        for (int i = 0; i < update_count; i++) {
            auto update = updates[i];
            
            switch (update->type) {
                case AdvanceEvent: {
                    // CRITICAL: Update inputs but DON'T advance here
                    // Following OnlineSession example lines 307-315
                    uint16_t received_p1 = ((uint16_t*)update->data.adv.inputs)[0];
                    uint16_t received_p2 = ((uint16_t*)update->data.adv.inputs)[1];
                    
                    // Reduced logging - only log every 60 frames to avoid performance issues
                    static int advance_log_counter = 0;
                    if (++advance_log_counter % 600 == 0) { // Log every 600 frames (~6 seconds) instead of 60
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent Frame %d - P1:%d P2:%d", 
                                   update->data.adv.frame, received_p1, received_p2);
                    }
                    
                    // Apply synchronized inputs (like OnlineSession example)
                    networked_p1_input = received_p1;
                    networked_p2_input = received_p2;
                    use_networked_inputs = true;
                    
                    // Debug logging removed for performance
                    
                    // NOTE: Don't call original_process_inputs() here - let it happen below
                    break;
                }
                case SaveEvent: {
                    // Handle save state (like OnlineSession save_state) - no logging for performance
                    // TODO: Implement proper save state
                    break;
                }
                case LoadEvent: {
                    // Handle load state (like OnlineSession load_state) - no logging for performance
                    // TODO: Implement proper load state
                    break;
                }
            }
        }
    }
    
    // ALWAYS ADVANCE FM2K: Like OnlineSession example, game runs every frame
    // The synchronized inputs from AdvanceEvents (if any) will be used automatically
    // FIXED: Increment frame counter BEFORE processing to fix 1-frame input delay
    g_frame_counter++;
    
    if (original_process_inputs) {
        original_process_inputs();
    }
    
    // Handle frame stepping countdown for GekkoNet path
    if (shared_data && shared_data->frame_step_remaining_frames > 0 && shared_data->frame_step_remaining_frames != UINT32_MAX) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame ADVANCED to %u during stepping (GekkoNet path)", g_frame_counter);
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
    
    return 0; // Return 0 as the game's frame advancement is handled by GekkoNet
}

int __cdecl Hook_UpdateGameState() {
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
    
    // FRAME STEPPING: Block game state updates when paused
    SharedInputData* shared_data = GetSharedMemory();
    if (shared_data && frame_step_paused_global && shared_data->frame_step_is_paused) {
        return 0; // Block game state updates when paused
    }
    
    // Only monitor state transitions every 30 frames
    static uint32_t state_check_counter = 0;
    if (++state_check_counter % 30 == 0) {
        MonitorGameStateTransitions();
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
        current_game_mode = new_game_mode;
        state_changed = true;
        
        // Simplified CSS mode detection without state logging
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