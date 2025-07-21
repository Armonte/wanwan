#pragma once

#include "common.h"

// Forward declaration for DetailedObject
namespace FM2K {
namespace ObjectPool {
    struct DetailedObject;
}
}

// Save state data structure - SIMPLIFIED
struct SaveStateData {
    // Basic player state (CheatEngine verified)
    uint32_t p1_hp;          // 0x004DFC85 - P1 HP
    uint32_t p2_hp;          // 0x004EDCC4 - P2 HP
    uint32_t p1_x;           // 0x004DFCC3 - P1 X coordinate
    uint32_t p1_y;           // 0x004DFCC7 - P1 Y coordinate
    uint32_t p2_x;           // 0x004EDD02 - P2 X coordinate
    uint32_t p2_y;           // 0x004EDD06 - P2 Y coordinate
    
    // RNG seed for deterministic behavior
    uint32_t rng_seed;       // 0x41FB1C - Critical for rollback
    
    // Timer
    uint32_t game_timer;     // 0x470050 - Actual WanWan timer
    
    // Object pool (391KB - 1024 objects * 382 bytes each)
    uint8_t object_pool[0x5F800]; // 0x4701E0 - Full object pool capture
    
    // Metadata
    uint32_t frame_number;   // Frame when this state was saved
    uint64_t timestamp_ms;   // When this save was created
    bool valid;              // Is this save slot occupied
    uint32_t checksum;       // Simple validation checksum
};

// Shared memory structure matching the launcher
struct SharedInputData {
    uint32_t frame_number;
    uint16_t p1_input;
    uint16_t p2_input;
    bool valid;
    
    bool is_online_mode;
    bool is_host;
    char remote_address[64];
    uint16_t port;
    uint8_t input_delay;
    bool config_updated;
    
    bool debug_save_state_requested;
    bool debug_load_state_requested;
    uint32_t debug_rollback_frames;
    bool debug_rollback_requested;
    uint32_t debug_command_id;
    
    bool debug_save_to_slot_requested;
    bool debug_load_from_slot_requested;
    uint32_t debug_target_slot;
    
    bool auto_save_enabled;
    uint32_t auto_save_interval_frames;
    
    // Frame stepping controls
    bool frame_step_pause_requested;
    bool frame_step_resume_requested;
    bool frame_step_single_requested;
    uint32_t frame_step_multi_count;
    bool frame_step_is_paused;
    uint32_t frame_step_remaining_frames; // Number of frames to allow before pausing again
    
    bool production_mode;
    bool enable_input_recording;
    
    bool use_minimal_gamestate_testing;
    
    uint32_t config_version;
    
    struct SlotInfo {
        bool occupied;
        uint32_t frame_number;
        uint64_t timestamp_ms;
        uint32_t checksum;
        uint32_t state_size_kb;
        uint32_t save_time_us;
        uint32_t load_time_us;
        uint32_t active_object_count;  // Number of active objects saved
    } slot_status[8];
    
    struct PerformanceStats {
        uint32_t total_saves;
        uint32_t total_loads;
        uint32_t avg_save_time_us;
        uint32_t avg_load_time_us;
        uint32_t memory_usage_mb;
        uint32_t rollback_count;
        uint32_t max_rollback_frames;
        uint32_t total_rollback_frames;
        uint32_t avg_rollback_frames;
        uint64_t last_rollback_time_us;
        uint32_t rollbacks_this_second;
        uint64_t current_second_start;
    } perf_stats;
    
    uint8_t player_index;
    uint8_t session_role;
    
    // 2DFM Script Command Types (from 2dfm_object_system.md)
    enum class ScriptCommandType : uint32_t {
        START = 0,      // Initialize script execution
        MOVE = 1,       // Movement commands  
        SOUND = 3,      // Audio playback
        OBJECT = 4,     // Object creation and management
        END = 5,        // Terminate script execution
        LOOP = 9,       // Looping constructs
        JUMP = 10,      // Jump to different script
        CALL = 11,      // Call sub-script
        PIC = 12,       // Display picture/sprite
        COLOR = 35,     // Color effects and blending
        VARIABLE = 31,  // Variable operations
        RANDOM = 32,    // Random branching logic
        AFTERIMAGE = 37 // Visual effects (afterimages)
    };

    // 2DFM Script Special Flags (from 2dfm_object_system.md)
    enum class ScriptSpecialFlag : uint32_t {
        NORMAL = 0,                 // General scripts
        BACKGROUND = 1,             // Background elements, cursors
        SYSTEM = 3,                 // System UI elements
        STAGE_MAIN_UI = 9,         // Main UI (HP bars, skill bars)
        COMBO_SYMBOL = 33,         // Combo number displays
        ROUND = 57,                // Round start/end animations
        TIME_NUMBER = 65,          // Timer displays
        HIT_SYMBOL = 97,           // Hit combo symbols
        SKILL_POINT_NUMBER = 129,  // Skill point numbers
        VICTORY_FLAG = 193,        // Victory indicators
        TIMER_POS = 131,           // Timer position
        PLAYER_1_AVATAR_POS = 195, // Player 1 avatar position
        PLAYER_2_AVATAR_POS = 259, // Player 2 avatar position
        PLAYER_1_SKILL_POINT_POS = 323, // Player 1 skill point position
        PLAYER_2_SKILL_POINT_POS = 387, // Player 2 skill point position
        PLAYER_1_VICTORY_POS = 451, // Player 1 victory position
        PLAYER_2_VICTORY_POS = 515, // Player 2 victory position
    };

    // Enhanced action analysis data (FM2K "objects" are actually "actions" in 2DFM editor)
    struct EnhancedActionData {
        // Core action data
        uint16_t slot_index;
        uint32_t type;              // Action type (SYSTEM, CHARACTER, PROJECTILE, EFFECT, etc.)
        uint32_t id;                // Action ID (determines specific behavior)
        uint32_t position_x, position_y;
        uint32_t velocity_x, velocity_y;
        uint32_t animation_state;   // Current animation frame/state
        uint32_t health_damage;
        uint32_t state_flags;
        uint32_t timer_counter;
        
        // Enhanced 2DFM integration data
        char type_name[32];         // Human readable action type
        char action_name[64];       // Current action being performed
        uint32_t script_id;         // Associated script ID
        uint32_t animation_frame;   // Animation frame number
        
        // 2DFM script system analysis
        uint32_t script_command_type;  // ScriptCommandType enum value
        uint32_t script_special_flag;  // ScriptSpecialFlag enum value
        char script_command_name[32];  // Human readable command type
        uint32_t render_layer;         // Render layer (0-127, characters: 70-80)
        uint32_t management_number;    // Management number (0-9, -1 for none)
        uint32_t object_flags;         // Object behavior flags
        char layer_description[32];    // Layer position description
        
        // Character-specific data (for CHARACTER actions)
        char character_name[32];    // Character performing the action
        char current_move[64];      // Move/technique being executed
        uint32_t facing_direction;  // 0=left, 1=right
        uint32_t combo_count;       // Hit combo counter
        
        // Raw action data for deep inspection
        uint8_t raw_data[382];      // Complete 382-byte action data
    };
    
    // Action analysis buffer (up to 64 active actions at once)
    uint32_t enhanced_actions_count;
    bool enhanced_actions_updated;
    EnhancedActionData enhanced_actions[64];
    
    // Dedicated save state storage (8 slots)
    SaveStateData save_slots[8];
};

// Functions for managing shared memory
bool InitializeSharedMemory();
void CleanupSharedMemory();
void ProcessDebugCommands();
bool CheckConfigurationUpdates();
void UpdateRollbackStats(uint32_t frames_rolled_back);
SharedInputData* GetSharedMemory();

// Enhanced action analysis functions (FM2K "objects" are actually "actions")
void UpdateEnhancedActionData();
void PopulateEnhancedActionInfo(const FM2K::ObjectPool::DetailedObject& detailed_obj, SharedInputData::EnhancedActionData& enhanced_action);
void AnalyzeScriptCommand(const FM2K::ObjectPool::DetailedObject& detailed_obj, SharedInputData::EnhancedActionData& enhanced_action); 