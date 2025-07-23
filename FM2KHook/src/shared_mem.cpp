#include "shared_mem.h"
#include "globals.h"
#include "state_manager.h"
#include "logging.h"
#include "object_pool_scanner.h"
#include <algorithm>
#include <cstring>

static HANDLE shared_memory_handle = nullptr;
static void* shared_memory_data = nullptr;

// Performance tracking
static uint32_t total_saves = 0;
static uint32_t total_loads = 0;
static uint64_t total_save_time_us = 0;
static uint64_t total_load_time_us = 0;
static uint32_t rollback_count = 0;
static uint32_t max_rollback_frames = 0;
static uint32_t total_rollback_frames = 0;

static inline uint64_t get_microseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

bool InitializeSharedMemory() {
    DWORD process_id = GetCurrentProcessId();
    std::string shared_memory_name = "FM2K_InputSharedMemory_" + std::to_string(process_id);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Creating shared memory with name: %s (PID=%lu)", shared_memory_name.c_str(), process_id);

    // First try to open existing shared memory (launcher creates it first)
    shared_memory_handle = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        shared_memory_name.c_str()
    );
    
    bool created_new = false;
    if (shared_memory_handle == nullptr) {
        // If it doesn't exist, create it
        shared_memory_handle = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            sizeof(SharedInputData),
            shared_memory_name.c_str()
        );
        created_new = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Created NEW shared memory segment");
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Opened EXISTING shared memory segment");
    }

    if (shared_memory_handle == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to create/open shared memory");
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

    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    
    // Only zero out if we created brand new memory
    if (created_new || shared_data->config_version == 0) {
        memset(shared_data, 0, sizeof(SharedInputData));
        shared_data->config_version = 1;
        shared_data->player_index = player_index;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initialized fresh shared memory segment");
    } else {
        // Just update our player index, preserve existing data
        shared_data->player_index = player_index;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Connected to existing shared memory, preserving slot data");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Shared memory initialized successfully");
    return true;
}

void CleanupSharedMemory() {
    if (shared_memory_data) {
        UnmapViewOfFile(shared_memory_data);
        shared_memory_data = nullptr;
    }
    if (shared_memory_handle) {
        CloseHandle(shared_memory_handle);
        shared_memory_handle = nullptr;
    }
}

void ProcessDebugCommands() {
    if (!shared_memory_data) return;

    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    static uint32_t last_processed_command_id = 0;

    if (shared_data->debug_command_id == last_processed_command_id) {
        return;
    }

    // ... (rest of command processing)
    
    last_processed_command_id = shared_data->debug_command_id;
}

bool CheckConfigurationUpdates() {
    if (!shared_memory_data) return false;

    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    if (shared_data->config_updated) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Configuration update received - Online: %s, Host: %s", 
                    shared_data->is_online_mode ? "YES" : "NO", shared_data->is_host ? "YES" : "NO");
        
        is_online_mode = shared_data->is_online_mode;
        is_host = shared_data->is_host;
        use_minimal_gamestate_testing = shared_data->use_minimal_gamestate_testing;

        shared_data->config_updated = false;
        return true;
    }
    return false;
}

void UpdateRollbackStats(uint32_t frames_rolled_back) {
    if (!shared_memory_data) return;

    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    // ... (update perf_stats)
}

SharedInputData* GetSharedMemory() {
    if (!shared_memory_data) return nullptr;
    return static_cast<SharedInputData*>(shared_memory_data);
}

void UpdateEnhancedActionData() {
    if (!shared_memory_data) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "UpdateEnhancedActionData: No shared memory data");
        return;
    }
    
    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    
    // Get detailed actions from action pool scanner (FM2K "objects" are actually "actions")
    auto detailed_actions = FM2K::ObjectPool::Scanner::ScanDetailedObjects();
    
    // Limit to maximum of 64 actions for shared memory buffer
    uint32_t count = std::min(static_cast<uint32_t>(detailed_actions.size()), 64u);
    shared_data->enhanced_actions_count = count;
    
    // Populate enhanced action data
    for (uint32_t i = 0; i < count; i++) {
        PopulateEnhancedActionInfo(detailed_actions[i], shared_data->enhanced_actions[i]);
    }
    
    // Mark as updated for launcher
    shared_data->enhanced_actions_updated = true;
    
    // Disabled verbose logging
    // static uint32_t log_counter = 0;
    // if (log_counter++ % 10 == 0) {
    //     SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "UpdateEnhancedActionData: Updated shared memory with %u actions", count);
    // }
}

void PopulateEnhancedActionInfo(const FM2K::ObjectPool::DetailedObject& detailed_obj, SharedInputData::EnhancedActionData& enhanced_action) {
    // Copy core action data (FM2K "objects" are actually "actions" in 2DFM terminology)
    enhanced_action.slot_index = detailed_obj.slot_index;
    enhanced_action.type = detailed_obj.type;
    enhanced_action.id = detailed_obj.id;
    enhanced_action.position_x = detailed_obj.position_x;
    enhanced_action.position_y = detailed_obj.position_y;
    enhanced_action.velocity_x = detailed_obj.velocity_x;
    enhanced_action.velocity_y = detailed_obj.velocity_y;
    enhanced_action.animation_state = detailed_obj.animation_state;
    enhanced_action.health_damage = detailed_obj.health_damage;
    enhanced_action.state_flags = detailed_obj.state_flags;
    enhanced_action.timer_counter = detailed_obj.timer_counter;
    
    // Copy raw action data
    memcpy(enhanced_action.raw_data, detailed_obj.raw_data, 382);
    
    // Get action type description (using existing method from DetailedObject)
    std::string type_desc = detailed_obj.GetTypeDescription();
    strncpy(enhanced_action.type_name, type_desc.c_str(), 31);
    enhanced_action.type_name[31] = '\0';
    
    // Enhanced 2DFM action integration mapping based on comprehensive script analysis
    enhanced_action.script_id = detailed_obj.id; // Use action ID as script reference
    enhanced_action.animation_frame = detailed_obj.animation_state;
    
    // Analyze action to determine 2DFM script command type and special flags
    AnalyzeScriptCommand(detailed_obj, enhanced_action);
    
    // Map action types to 2DFM script types (corrected terminology)
    switch (detailed_obj.type) {
        case 1: // SYSTEM actions
            strcpy(enhanced_action.action_name, "System Processing");
            break;
        case 4: // CHARACTER actions
            // Map animation states to character actions (based on 2DFM KgtPlayer actions)
            switch (detailed_obj.animation_state) {
                case 0:
                    strcpy(enhanced_action.action_name, "Stand Action");
                    break;
                case 1:
                    strcpy(enhanced_action.action_name, "Forward Move");
                    break;
                case 2:
                    strcpy(enhanced_action.action_name, "Backward Move");
                    break;
                case 3:
                    strcpy(enhanced_action.action_name, "Jump Up");
                    break;
                case 4:
                    strcpy(enhanced_action.action_name, "Jump Forward");
                    break;
                case 5:
                    strcpy(enhanced_action.action_name, "Jump Backward");
                    break;
                case 6:
                    strcpy(enhanced_action.action_name, "Falling");
                    break;
                case 7:
                    strcpy(enhanced_action.action_name, "Crouch Down");
                    break;
                case 8:
                    strcpy(enhanced_action.action_name, "Crouching");
                    break;
                case 9:
                    strcpy(enhanced_action.action_name, "Stand Up");
                    break;
                case 10:
                    strcpy(enhanced_action.action_name, "Turn Around");
                    break;
                case 11:
                    strcpy(enhanced_action.action_name, "Block Standing");
                    break;
                case 12:
                    strcpy(enhanced_action.action_name, "Block Crouching");
                    break;
                default:
                    if (detailed_obj.animation_state >= 100) {
                        strcpy(enhanced_action.action_name, "Special Attack");
                    } else if (detailed_obj.animation_state >= 50) {
                        strcpy(enhanced_action.action_name, "Combat Action");
                    } else {
                        snprintf(enhanced_action.action_name, 63, "Action_%u", detailed_obj.animation_state);
                    }
                    break;
            }
            break;
        case 5: // PROJECTILE actions
            strcpy(enhanced_action.action_name, "Projectile Flight");
            if (detailed_obj.velocity_x == 0 && detailed_obj.velocity_y == 0) {
                strcpy(enhanced_action.action_name, "Projectile Impact");
            }
            break;
        case 6: // EFFECT actions
            if (detailed_obj.timer_counter > 0) {
                strcpy(enhanced_action.action_name, "Effect Animation");
            } else {
                strcpy(enhanced_action.action_name, "Effect Complete");
            }
            break;
        case 10: // TYPE10 actions (special system actions)
            strcpy(enhanced_action.action_name, "Trigger/Event");
            break;
        default:
            snprintf(enhanced_action.action_name, 63, "Unknown_Type_%u", detailed_obj.type);
            break;
    }
    
    // Character-specific data initialization
    if (detailed_obj.type == 4) { // CHARACTER action type
        // Map action IDs to character names (based on common FM2K patterns)
        switch (detailed_obj.id) {
            case 10:
                strcpy(enhanced_action.character_name, "Menu Cursor");
                break;
            case 12:
                strcpy(enhanced_action.character_name, "Menu Element");
                break;
            case 50:
                strcpy(enhanced_action.character_name, "Player 1");
                break;
            case 51:
                strcpy(enhanced_action.character_name, "Player 2");
                break;
            case 100:
                strcpy(enhanced_action.character_name, "Fighter A");
                break;
            case 101:
                strcpy(enhanced_action.character_name, "Fighter B");
                break;
            case 200:
                strcpy(enhanced_action.character_name, "Stage Boss");
                break;
            default:
                if (detailed_obj.id >= 1000) {
                    strcpy(enhanced_action.character_name, "Special Character");
                } else if (detailed_obj.id >= 100) {
                    snprintf(enhanced_action.character_name, 31, "Fighter_%u", detailed_obj.id - 100);
                } else {
                    snprintf(enhanced_action.character_name, 31, "Entity_%u", detailed_obj.id);
                }
                break;
        }
        
        // Determine facing direction based on velocity or other indicators
        if (detailed_obj.velocity_x > 0) {
            enhanced_action.facing_direction = 1; // Right
        } else if (detailed_obj.velocity_x < 0) {
            enhanced_action.facing_direction = 0; // Left
        } else {
            enhanced_action.facing_direction = 1; // Default right
        }
        
        // Initialize move name based on animation state (enhanced 2DFM mapping)
        switch (detailed_obj.animation_state) {
            case 0:
                strcpy(enhanced_action.current_move, "Stand Idle");
                break;
            case 1:
                strcpy(enhanced_action.current_move, "Walk Forward");
                break;
            case 2:
                strcpy(enhanced_action.current_move, "Walk Backward");
                break;
            case 3:
                strcpy(enhanced_action.current_move, "Jump Up");
                break;
            case 4:
                strcpy(enhanced_action.current_move, "Jump Forward");
                break;
            case 5:
                strcpy(enhanced_action.current_move, "Jump Backward");
                break;
            case 6:
                strcpy(enhanced_action.current_move, "Air Falling");
                break;
            case 7:
                strcpy(enhanced_action.current_move, "Crouch Down");
                break;
            case 8:
                strcpy(enhanced_action.current_move, "Crouch Idle");
                break;
            case 9:
                strcpy(enhanced_action.current_move, "Stand Up");
                break;
            case 10:
                strcpy(enhanced_action.current_move, "Crouch Forward");
                break;
            case 11:
                strcpy(enhanced_action.current_move, "Crouch Backward");
                break;
            case 12:
                strcpy(enhanced_action.current_move, "Turn Around");
                break;
            case 13:
                strcpy(enhanced_action.current_move, "Block Standing");
                break;
            case 14:
                strcpy(enhanced_action.current_move, "Block Crouching");
                break;
            case 15:
                strcpy(enhanced_action.current_move, "Block Air");
                break;
            case 20:
                strcpy(enhanced_action.current_move, "Light Attack");
                break;
            case 21:
                strcpy(enhanced_action.current_move, "Medium Attack");
                break;
            case 22:
                strcpy(enhanced_action.current_move, "Heavy Attack");
                break;
            case 30:
                strcpy(enhanced_action.current_move, "Special Move");
                break;
            case 40:
                strcpy(enhanced_action.current_move, "Super Move");
                break;
            case 50:
                strcpy(enhanced_action.current_move, "Hit Stun");
                break;
            case 51:
                strcpy(enhanced_action.current_move, "Block Stun");
                break;
            case 60:
                strcpy(enhanced_action.current_move, "Knockdown");
                break;
            case 61:
                strcpy(enhanced_action.current_move, "Get Up");
                break;
            case 100:
                strcpy(enhanced_action.current_move, "Victory Pose");
                break;
            case 101:
                strcpy(enhanced_action.current_move, "Defeat");
                break;
            default:
                if (detailed_obj.animation_state >= 200) {
                    strcpy(enhanced_action.current_move, "Custom Action");
                } else if (detailed_obj.animation_state >= 100) {
                    strcpy(enhanced_action.current_move, "Story Action");
                } else if (detailed_obj.animation_state >= 70) {
                    strcpy(enhanced_action.current_move, "Combo Action");
                } else {
                    snprintf(enhanced_action.current_move, 63, "State_%u", detailed_obj.animation_state);
                }
                break;
        }
        
        enhanced_action.combo_count = 0; // TODO: Extract from game state
    } else {
        // Non-character actions
        strcpy(enhanced_action.character_name, "");
        strcpy(enhanced_action.current_move, "");
        enhanced_action.facing_direction = 0;
        enhanced_action.combo_count = 0;
    }
}

void AnalyzeScriptCommand(const FM2K::ObjectPool::DetailedObject& detailed_obj, SharedInputData::EnhancedActionData& enhanced_action) {
    // Initialize with default values
    enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::OBJECT);
    enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::NORMAL);
    enhanced_action.render_layer = 70; // Default character layer
    enhanced_action.management_number = static_cast<uint32_t>(-1); // -1 for none
    enhanced_action.object_flags = 0;
    strcpy(enhanced_action.script_command_name, "OBJECT");
    strcpy(enhanced_action.layer_description, "Character Layer");
    
    // Analyze action type and context to determine 2DFM script equivalents
    switch (detailed_obj.type) {
        case 0: // INACTIVE
            enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::END);
            enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::NORMAL);
            enhanced_action.render_layer = 0;
            strcpy(enhanced_action.script_command_name, "END");
            strcpy(enhanced_action.layer_description, "Inactive");
            break;
            
        case 1: // SYSTEM
            enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::OBJECT);
            enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::SYSTEM);
            enhanced_action.render_layer = 127; // Top layer for system elements
            strcpy(enhanced_action.script_command_name, "SYSTEM");
            strcpy(enhanced_action.layer_description, "System UI");
            
            // Analyze system action subtypes based on ID and position
            if (detailed_obj.id == 10 || detailed_obj.id == 12) {
                enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::BACKGROUND);
                strcpy(enhanced_action.layer_description, "Cursor/Menu");
            }
            break;
            
        case 4: // CHARACTER
            enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::OBJECT);
            enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::NORMAL);
            
            // Character layer system (70-80 for characters)
            if (detailed_obj.id >= 50 && detailed_obj.id <= 51) {
                enhanced_action.render_layer = 75; // Main player characters
                enhanced_action.management_number = detailed_obj.id - 50; // 0 or 1
            } else if (detailed_obj.id >= 100 && detailed_obj.id <= 200) {
                enhanced_action.render_layer = 70 + (detailed_obj.id % 10); // Varied character layers
                enhanced_action.management_number = (detailed_obj.id - 100) % 10;
            } else {
                enhanced_action.render_layer = 75; // Default character layer
            }
            
            strcpy(enhanced_action.script_command_name, "CHARACTER");
            strcpy(enhanced_action.layer_description, "Character");
            
            // Movement analysis for command type refinement
            if (detailed_obj.velocity_x != 0 || detailed_obj.velocity_y != 0) {
                enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::MOVE);
                strcpy(enhanced_action.script_command_name, "MOVE");
            }
            break;
            
        case 5: // PROJECTILE
            enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::OBJECT);
            enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::NORMAL);
            enhanced_action.render_layer = 80; // In front of characters
            enhanced_action.management_number = detailed_obj.id % 10; // Management numbers 0-9
            strcpy(enhanced_action.script_command_name, "PROJECTILE");
            strcpy(enhanced_action.layer_description, "Front Layer");
            
            // Always involves movement
            if (detailed_obj.velocity_x != 0 || detailed_obj.velocity_y != 0) {
                enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::MOVE);
                strcpy(enhanced_action.script_command_name, "MOVE");
            }
            break;
            
        case 6: // EFFECT
            enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::PIC);
            enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::NORMAL);
            enhanced_action.render_layer = 85; // Above characters for effects
            strcpy(enhanced_action.script_command_name, "PIC");
            strcpy(enhanced_action.layer_description, "Effect Layer");
            
            // Check for special effect types
            if (detailed_obj.timer_counter > 0) {
                // Time-based effect - could be afterimage
                enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::AFTERIMAGE);
                strcpy(enhanced_action.script_command_name, "AFTERIMAGE");
            }
            
            // Color effects analysis
            if (detailed_obj.state_flags & 0xFF00) { // Color-related flags
                enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::COLOR);
                strcpy(enhanced_action.script_command_name, "COLOR");
            }
            break;
            
        case 10: // TYPE10 (Special triggers/events)
            enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::VARIABLE);
            enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::SYSTEM);
            enhanced_action.render_layer = 0; // Background layer for triggers
            strcpy(enhanced_action.script_command_name, "VARIABLE");
            strcpy(enhanced_action.layer_description, "Trigger/Event");
            
            // Could be jump/call based on context
            if (detailed_obj.animation_state > 0) {
                enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::JUMP);
                strcpy(enhanced_action.script_command_name, "JUMP");
            }
            break;
            
        default:
            // Unknown type - treat as generic object
            enhanced_action.script_command_type = static_cast<uint32_t>(SharedInputData::ScriptCommandType::OBJECT);
            enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::NORMAL);
            enhanced_action.render_layer = 50; // Middle layer
            strcpy(enhanced_action.script_command_name, "UNKNOWN");
            strcpy(enhanced_action.layer_description, "Unknown");
            break;
    }
    
    // Special flag analysis based on position and behavior patterns
    if (detailed_obj.position_y < 100) {
        // Top area - likely UI elements
        if (enhanced_action.script_special_flag == static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::NORMAL)) {
            enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::STAGE_MAIN_UI);
        }
    }
    
    // Timer-based elements analysis
    if (detailed_obj.timer_counter > 0 && detailed_obj.type == 1) {
        enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::TIME_NUMBER);
        strcpy(enhanced_action.layer_description, "Timer Display");
    }
    
    // Combo/hit analysis
    if (detailed_obj.health_damage > 0 && detailed_obj.type == 6) {
        enhanced_action.script_special_flag = static_cast<uint32_t>(SharedInputData::ScriptSpecialFlag::HIT_SYMBOL);
        strcpy(enhanced_action.layer_description, "Hit Effect");
    }
    
    // Object flags analysis (from 2DFM ObjectCmd)
    enhanced_action.object_flags = 0;
    
    // Shadow flag - objects with certain state flags
    if (detailed_obj.state_flags & 0x08) {
        enhanced_action.object_flags |= 0b1000; // isShowShadow
    }
    
    // Attach as child flag - objects with parent relationships
    if (detailed_obj.unknown_1C != 0 && detailed_obj.unknown_1C != 0xFFFFFFFF) {
        enhanced_action.object_flags |= 0b00100000; // isAttachAsChild
    }
    
    // Layer-specific behavior
    if (enhanced_action.render_layer == 127) {
        enhanced_action.object_flags |= 0b01; // Top layer flag
    } else if (enhanced_action.render_layer == 0) {
        enhanced_action.object_flags |= 0b00; // Default layer flag
    } else {
        enhanced_action.object_flags |= 0b10; // Custom layer flag
    }
    
    // Management number refinement
    if (detailed_obj.type == 4 || detailed_obj.type == 5) { // CHARACTER or PROJECTILE
        if (enhanced_action.management_number == static_cast<uint32_t>(-1)) {
            enhanced_action.management_number = detailed_obj.id % 10;
        }
    }
} 