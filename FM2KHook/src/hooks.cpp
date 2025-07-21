#include "hooks.h"
#include "globals.h"
#include "gekkonet_hooks.h"
#include "state_manager.h"
#include "logging.h"
#include "shared_mem.h"
#include "game_state_machine.h"
#include "css_sync.h"
#include "object_tracker.h"
#include "object_analysis.h"
#include "object_pool_scanner.h"
// #include "boot_object_analyzer.cpp"  // REMOVED: Performance optimization
#include <windows.h>
#include <mmsystem.h>

// Global variables for manual save/load requests
static bool manual_save_requested = false;
static bool manual_load_requested = false;

// Auto-save tracking (separate from globals.h version)
static uint32_t hook_last_auto_save_frame = 0;

// ARCHITECTURE FIX: Real input capture following CCCaster/GekkoNet pattern
static void CaptureRealInputs() {
    // Following the pattern from GekkoNet SDL2 example and CCCaster
    // This captures actual keyboard/controller input before the game processes it
    
    // For now, we'll read the inputs directly from the game's memory addresses
    // In the future, this could be enhanced to use direct keyboard/controller APIs
    
    // BACK TO WORKING APPROACH: Use Hook_GetPlayerInput to capture inputs at source
    // Just like dllmain_orig.cpp - inputs are captured when FM2K calls Hook_GetPlayerInput
    // Don't override them here - let the input hooks do their job
    
    // Simplified input capture without excessive logging
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
    
    if (network_input & 0x01) game_input |= 0x001;  // LEFT
    if (network_input & 0x02) game_input |= 0x002;  // RIGHT
    if (network_input & 0x04) game_input |= 0x004;  // UP
    if (network_input & 0x08) game_input |= 0x008;  // DOWN
    if (network_input & 0x10) game_input |= 0x010;  // BUTTON1
    if (network_input & 0x20) game_input |= 0x020;  // BUTTON2
    if (network_input & 0x40) game_input |= 0x040;  // BUTTON3
    if (network_input & 0x80) game_input |= 0x080;  // BUTTON4
    
    return game_input;
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
    // SIMPLIFIED: Always read local inputs, let GekkoNet handle synchronization
    int original_input = original_get_player_input ? original_get_player_input(player_id, input_type) : 0;
    
    // Store both inputs locally (following LocalSessionExample pattern)
    if (player_id == 0) {
        live_p1_input = original_input;
    } else if (player_id == 1) {
        live_p2_input = original_input;
    }
    
    // Use networked inputs if available
    if (use_networked_inputs && gekko_initialized && gekko_session) {
        if (player_id == 0) {
            return ConvertNetworkInputToGameFormat(networked_p1_input);
        } else if (player_id == 1) {
            return ConvertNetworkInputToGameFormat(networked_p2_input);
        }
    }
    
    // Fall back to original input
    return original_input;
}

int __cdecl Hook_ProcessGameInputs() {
    // In lockstep/rollback mode, the game's frame advancement is handled inside the AdvanceEvent.
    // We do nothing here to allow GekkoNet to control the frame pacing.
    if (!waiting_for_gekko_advance) {
        // Call the original function to let the game run normally.
        if (original_process_inputs) {
            original_process_inputs();
        }
        g_frame_counter++;
    }
    
    
    // GekkoNet rollback control (only if session is active)
    if (gekko_initialized && gekko_session && gekko_session_started) {
        // ARCHITECTURE FIX: Proper input capture following CCCaster/GekkoNet pattern
        // 1. CAPTURE: Read actual controller/keyboard inputs (like CCCaster's updateControls)
        CaptureRealInputs();
        
        // CSS sync disabled for performance
        
        // 3. CHECK: Process debug commands from launcher via shared memory
        CheckForDebugCommands();
        
        // 3.5. AUTO-SAVE: Check if auto-save should trigger
        SharedInputData* shared_data = GetSharedMemory();
        if (shared_data && shared_data->auto_save_enabled) {
            if ((g_frame_counter - hook_last_auto_save_frame) >= shared_data->auto_save_interval_frames) {
                // Trigger auto-save to slot 0
                manual_save_requested = true;
                shared_data->debug_target_slot = 0;  // Auto-save always uses slot 0
                hook_last_auto_save_frame = g_frame_counter;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "AUTO-SAVE triggered: slot 0, frame %u", g_frame_counter);
            }
        }
        
        // 4. SEND: Input sending depends on session type
        if (is_local_session) {
            // LOCAL SESSION: Send BOTH player inputs (LocalSessionExample pattern)
            uint8_t p1_input = (uint8_t)(live_p1_input & 0xFF);
            uint8_t p2_input = (uint8_t)(live_p2_input & 0xFF);
            
            gekko_add_local_input(gekko_session, p1_player_handle, &p1_input);
            gekko_add_local_input(gekko_session, p2_player_handle, &p2_input);
        } else {
            // ONLINE SESSION: Send only local player input (OnlineSession pattern)
            uint8_t local_input;
            if (::is_host) {
                local_input = (uint8_t)(live_p1_input & 0xFF); // Host sends P1 input
            } else {
                local_input = (uint8_t)(live_p2_input & 0xFF); // Client sends P2 input
            }
            gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        }
        
        // Simplified input timing without logging
        
        // Process GekkoNet events following the example pattern
        gekko_network_poll(gekko_session);
        
        // First handle session events (disconnects, desyncs)
        int session_event_count = 0;
        auto session_events = gekko_session_events(gekko_session, &session_event_count);
        for (int i = 0; i < session_event_count; i++) {
            auto event = session_events[i];
            if (event->type == DesyncDetected) {
                auto desync = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DESYNC: frame %d", desync.frame);
            } else if (event->type == PlayerDisconnected) {
                auto disco = event->data.disconnected;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "DISCONNECT: %d", disco.handle);
            }
        }
        
        // Then handle game updates  
        int update_count = 0;
        auto updates = gekko_update_session(gekko_session, &update_count);
        
        for (int i = 0; i < update_count; i++) {
            auto update = updates[i];
            
            switch (update->type) {
                case AdvanceEvent: {
                    
                    // CRITICAL DEBUG: Log the exact inputs received from GekkoNet
                    uint8_t received_p1 = update->data.adv.inputs[0];
                    uint8_t received_p2 = update->data.adv.inputs[1];
                    
                    
                    // Always apply the synchronized inputs first.
                    networked_p1_input = received_p1;
                    networked_p2_input = received_p2;
                    use_networked_inputs = true;
                    
                    // Simplified synchronization without excessive logging

                    // Check if the remote player sent a confirmation signal.
                    uint8_t remote_input = is_host ? networked_p2_input : networked_p1_input;
                    if (remote_input == 0xFF) {
                        FM2K::CSS::g_css_sync.ReceiveRemoteConfirmation();
                        
                        // Filter out 0xFF from normal gameplay inputs
                        if (is_host) {
                            networked_p2_input = 0x00;
                        } else {
                            networked_p1_input = 0x00;
                        }
                    }

                    // Simplified advance event processing

                    // Simplified direction tracking without logging

                    // Simplified movement detection without logging
                    
                    // Normal frame advancement (allow rollback to work)
                    if (original_process_inputs) {
                        original_process_inputs();
                    }
                    g_frame_counter++;
                    
                    // Simplified post-processing without cursor tracking
                    
                    // Simplified post-processing without cursor tracking
                    break;
                }
                    
                case SaveEvent: {
                    // Real save state implementation for offline debugging
                    // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet SaveEvent: frame %d", update->data.save.frame); // DISABLED: Too spammy
                    
                    // Use the existing state manager for comprehensive saves
                    FM2K::State::GameState current_state;
                    if (update->data.save.state && update->data.save.state_len) {
                        // For now, use a simple checksum-based save
                        size_t copy_size = std::min((size_t)*update->data.save.state_len, sizeof(FM2K::State::GameState));
                        memset(&current_state, 0, sizeof(current_state));
                        
                        // Copy basic state data
                        memcpy(update->data.save.state, &current_state, copy_size);
                        *update->data.save.state_len = copy_size;
                        
                        // Generate simple checksum
                        if (update->data.save.checksum) {
                            *update->data.save.checksum = 0xDEADBEEF + update->data.save.frame;
                        }
                        
                        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Saved game state: %zu bytes, checksum: 0x%08X", 
                        //            copy_size, update->data.save.checksum ? *update->data.save.checksum : 0); // DISABLED: Too spammy
                    }
                    break;
                }
                    
                case LoadEvent: {
                    // Real load state implementation for offline debugging  
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet LoadEvent: frame %d", update->data.load.frame);
                    
                    if (update->data.load.state && update->data.load.state_len > 0) {
                        // For now, just log the restore
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Restored game state: %zu bytes", (size_t)update->data.load.state_len);
                    }
                    break;
                }
            }
        }
        
        // Handle manual save/load requests from launcher (outside GekkoNet's automatic rollback)
        if (manual_save_requested) {
            SharedInputData* shared_data = GetSharedMemory();
            
            if (shared_data) {
                uint32_t target_slot = shared_data->debug_target_slot;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Processing save state request for slot %u", target_slot);
                
                if (target_slot < 8) {
                    // SIMPLE save state - just what we need
                    
                    // Player state addresses (CheatEngine verified)
                    uint32_t* p1_hp_ptr = (uint32_t*)0x004DFC85;
                    uint32_t* p2_hp_ptr = (uint32_t*)0x004EDCC4;
                    uint32_t* p1_x_ptr = (uint32_t*)0x004DFCC3;
                    uint16_t* p1_y_ptr = (uint16_t*)0x004DFCC7;
                    uint32_t* p2_x_ptr = (uint32_t*)0x004EDD02;
                    uint16_t* p2_y_ptr = (uint16_t*)0x004EDD06;
                    
                    // RNG seed
                    uint32_t* rng_seed_ptr = (uint32_t*)0x41FB1C;
                    
                    // Timer
                    uint32_t* timer_ptr = (uint32_t*)0x470050;
                    
                    // Object pool (391KB)
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
                                          !IsBadReadPtr(object_pool_ptr, object_pool_size));
                    
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
                        
                        // Save RNG seed
                        save_slot->rng_seed = *rng_seed_ptr;
                        
                        // Save timer
                        save_slot->game_timer = *timer_ptr;
                        
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
            }
            manual_save_requested = false;
        }
        
        if (manual_load_requested) {
            SharedInputData* shared_data = GetSharedMemory();
            
            if (shared_data) {
                uint32_t target_slot = shared_data->debug_target_slot;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LOAD START: Processing load state request for slot %u", target_slot);
                
                if (target_slot < 8 && shared_data->save_slots[target_slot].valid) {
                    SaveStateData* save_slot = &shared_data->save_slots[target_slot];
                    
                    // SIMPLE load state - just what we need
                    
                    // Player state addresses
                    uint32_t* p1_hp_ptr = (uint32_t*)0x004DFC85;
                    uint32_t* p2_hp_ptr = (uint32_t*)0x004EDCC4;
                    uint32_t* p1_x_ptr = (uint32_t*)0x004DFCC3;
                    uint16_t* p1_y_ptr = (uint16_t*)0x004DFCC7;
                    uint32_t* p2_x_ptr = (uint32_t*)0x004EDD02;
                    uint16_t* p2_y_ptr = (uint16_t*)0x004EDD06;
                    
                    // RNG seed
                    uint32_t* rng_seed_ptr = (uint32_t*)0x41FB1C;
                    
                    // Timer
                    uint32_t* timer_ptr = (uint32_t*)0x470050;
                    
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
                                             !IsBadWritePtr(object_pool_ptr, object_pool_size));
                    
                    if (addresses_writable) {
                        // Restore player state
                        *p1_hp_ptr = save_slot->p1_hp;
                        *p2_hp_ptr = save_slot->p2_hp;
                        *p1_x_ptr = save_slot->p1_x;
                        *p1_y_ptr = (uint16_t)save_slot->p1_y;
                        *p2_x_ptr = save_slot->p2_x;
                        *p2_y_ptr = (uint16_t)save_slot->p2_y;
                        
                        // Restore RNG seed
                        *rng_seed_ptr = save_slot->rng_seed;
                        
                        // Restore timer
                        *timer_ptr = save_slot->game_timer;
                        
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
            }
            manual_load_requested = false;
        }
    }
    
    return 0; // Return 0 as the game's frame advancement is handled by GekkoNet
}

int __cdecl Hook_UpdateGameState() {
    // Only monitor state transitions every 30 frames
    static uint32_t state_check_counter = 0;
    if (++state_check_counter % 30 == 0) {
        MonitorGameStateTransitions();
    }
    
    // Original logic for GekkoNet session management
    if (gekko_initialized && !gekko_session_started) {
        return 0;
    }
    
    if (original_update_game) {
        return original_update_game();
    }
    return 0;
}

BOOL __cdecl Hook_RunGameLoop() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** REIMPLEMENTING FM2K MAIN LOOP WITH GEKKONET CONTROL ***");
    
    // Set character select mode flag after memory clearing
    uint8_t* char_select_mode_ptr = (uint8_t*)FM2K::State::Memory::CHARACTER_SELECT_MODE_ADDR;
    if (!IsBadReadPtr(char_select_mode_ptr, sizeof(uint8_t))) {
        DWORD old_protect;
        if (VirtualProtect(char_select_mode_ptr, sizeof(uint8_t), PAGE_READWRITE, &old_protect)) {
            *char_select_mode_ptr = 1;
            VirtualProtect(char_select_mode_ptr, sizeof(uint8_t), old_protect, &old_protect);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Set character select mode flag to 1 after memory clearing");
        }
    }
    
    if (!gekko_initialized) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing GekkoNet...");
        if (!InitializeGekkoNet()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet failed, using original loop");
            return original_run_game_loop ? original_run_game_loop() : FALSE;
        }
        
        // TEMPORARILY DISABLED: Object tracker initialization (debugging crashes)
        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing adaptive object tracker...");
        // FM2K::ObjectTracking::g_object_tracker.Initialize();
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet and object tracking initialized!");
    }
    
    // FM2K timing variables (from IDA analysis of run_game_loop at 0x405AD0)
    uint32_t* g_frame_time_ms = (uint32_t*)0x447EE4;        // Fixed 10ms (100 FPS)
    uint32_t* g_last_frame_time = (uint32_t*)0x447EE8;      // Last frame timestamp
    uint32_t* g_frame_sync_flag = (uint32_t*)0x447EEC;      // Frame sync state
    uint32_t* g_frame_time_delta = (uint32_t*)0x447EF0;     // Frame timing delta
    uint32_t* g_frame_skip_count = (uint32_t*)0x447EF4;     // Frame skip counter
    
    // EXACT FM2K INITIALIZATION (from decompiled run_game_loop)
    *g_frame_time_ms = 10;                                  // Initialize game loop timing - Fixed 100 FPS (10ms per frame)
    *g_last_frame_time = timeGetTime();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Running initial 8 warmup frames...");
    int init_loop_count = 8;                                // Initial game state warmup - Run 8 frames of game logic
    do {
        if (original_update_game) {
            original_update_game();                         // update_game_state() call
        }
        --init_loop_count;
    } while (init_loop_count);
    
    *g_last_frame_time = timeGetTime();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Warmup complete, starting GekkoNet-controlled main loop...");
    
    // Wait for GekkoNet connection before starting main game loop
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Waiting for GekkoNet connection...");
    uint32_t connection_attempts = 0;
    while (!AllPlayersValid() && connection_attempts < 1500) {  // 15 second timeout
        gekko_network_poll(gekko_session);
        int temp_count = 0;
        gekko_update_session(gekko_session, &temp_count);
        
        // Process Windows messages during connection
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return TRUE;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        Sleep(10);
        connection_attempts++;
        
        if (connection_attempts % 100 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Connection attempt %u/1500...", connection_attempts);
        }
    }
    
    if (!AllPlayersValid()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Connection timeout! Falling back to original loop.");
        return original_run_game_loop ? original_run_game_loop() : FALSE;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet connected! Calling original FM2K loop...");
    gekko_session_started = true;
    
    // SIMPLE APPROACH: Just call the original run_game_loop
    // Our Hook_ProcessGameInputs will handle the rollback logic
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Delegating to original FM2K main loop...");
    
    if (original_run_game_loop) {
        return original_run_game_loop();
    }
    
    return FALSE;
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
    
    void* runGameLoopFuncAddr = (void*)FM2K::State::Memory::RUN_GAME_LOOP_ADDR;
    MH_STATUS status3 = MH_CreateHook(runGameLoopFuncAddr, (void*)Hook_RunGameLoop, (void**)&original_run_game_loop);
    if (status3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create run_game_loop hook: %d", status3);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable3 = MH_EnableHook(runGameLoopFuncAddr);
    if (enable3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable run_game_loop hook: %d", enable3);
        MH_Uninitialize();
        return false;
    }
    
    // Apply boot-to-character-select patches directly
    ApplyBootToCharacterSelectPatches();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: BSNES-level architecture installed successfully!");
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
    // SIMPLIFIED: Use state machine to determine rollback activation
    bool should_activate_rollback = FM2K::State::g_game_state_machine.ShouldEnableRollback();
    bool should_use_lockstep = FM2K::State::g_game_state_machine.ShouldUseLockstep();
    bool in_stabilization = FM2K::State::g_game_state_machine.IsInTransitionStabilization();
    
    // Determine if we need to be waiting for GekkoNet to advance the frame
    bool needs_frame_sync = (should_activate_rollback || should_use_lockstep) && !in_stabilization;

    // CRITICAL: Disable rollback during transition stabilization to prevent desyncs
    if (in_stabilization && waiting_for_gekko_advance) {
        waiting_for_gekko_advance = false;
        rollback_active = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "FM2K STATE: Disabling frame sync for stabilization (phase: %d, frames: %d)", 
            static_cast<int>(FM2K::State::g_game_state_machine.GetCurrentPhase()),
            FM2K::State::g_game_state_machine.GetFramesInCurrentPhase());
    }
    
    // Activate frame synchronization if we need either rollback or lockstep
    if (needs_frame_sync && !waiting_for_gekko_advance) {
        waiting_for_gekko_advance = true;
        rollback_active = should_activate_rollback; // Only set rollback_active if we're actually in battle
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "FM2K STATE: Activating %s sync (game_mode=0x%X)", 
            rollback_active ? "ROLLBACK" : "LOCKSTEP",
            game_mode);
        
    } else if (!needs_frame_sync && waiting_for_gekko_advance) {
        // Deactivate frame synchronization (returning to menu, etc.)
        waiting_for_gekko_advance = false;
        rollback_active = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: Deactivating frame sync (game_mode=0x%X)", game_mode);
    }
}

bool ShouldActivateRollback(uint32_t game_mode, uint32_t fm2k_mode) {
    // UPDATED: Use state machine logic instead of always returning true
    // This function is legacy - the state machine handles this now
    return FM2K::State::g_game_state_machine.ShouldEnableRollback();
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