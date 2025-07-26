#pragma once

#include "common.h"

// Forward declaration for DetailedObject
namespace FM2K {
namespace ObjectPool {
    struct DetailedObject;
}
}

// Save state data structure - COMPREHENSIVE
struct SaveStateData {
    // Basic player state (CheatEngine verified)
    uint32_t p1_hp;                    // 0x004DFC85 - P1 HP
    uint32_t p2_hp;                    // 0x004EDCC4 - P2 HP
    uint32_t p1_x;                     // 0x004DFCC3 - P1 X coordinate
    uint16_t p1_y;                     // 0x004DFCC7 - P1 Y coordinate (2 bytes)
    uint32_t p2_x;                     // 0x004EDD02 - P2 X coordinate
    uint16_t p2_y;                     // 0x004EDD06 - P2 Y coordinate (2 bytes)
    
    // Player meter/super/stock
    uint32_t p1_super;                 // 0x004DFC9D - P1 Super
    uint32_t p2_super;                 // 0x004EDCDC - P2 Super
    uint32_t p1_special_stock;         // 0x004DFC95 - P1 Special Stock
    uint32_t p2_special_stock;         // 0x004EDCD4 - P2 Special Stock
    uint32_t p1_rounds_won;            // 0x004DFC6D - P1 Rounds Won
    uint32_t p2_rounds_won;            // 0x004EDCAC - P2 Rounds Won
    
    // RNG seed for deterministic behavior
    uint32_t rng_seed;                 // 0x41FB1C - Critical for rollback
    
    // Timers and state
    uint32_t game_timer;               // 0x470050 - g_actual_wanwan_timer
    uint32_t round_timer;              // 0x00470060 - g_round_timer
    uint32_t round_state;              // 0x47004C - g_round_state
    uint32_t round_limit;              // 0x470048 - g_round_limit
    uint32_t round_setting;            // 0x470068 - g_round_setting
    
    // Game modes and flags
    uint32_t fm2k_game_mode;           // 0x470040 - g_fm2k_game_mode
    uint16_t game_mode;                // 0x00470054 - g_game_mode
    uint32_t game_paused;              // 0x4701BC - g_game_paused
    uint32_t replay_mode;              // 0x4701C0 - g_replay_mode
    
    // Camera position
    uint32_t camera_x;                 // 0x00447F2C - g_camera_x (Map X Coor)
    uint32_t camera_y;                 // 0x00447F30 - g_camera_y (Map Y Coor)
    
    // Character variables (A-P for each player)
    int16_t p1_char_vars[16];          // 0x004DFD17-0x004DFD35
    int16_t p2_char_vars[16];          // 0x004EDD56-0x004EDD74
    
    // System variables (A-P)
    int16_t sys_vars[14];              // 0x004456B0-0x004456CA (A-N signed)
    uint16_t sys_vars_unsigned[2];     // 0x004456CC-0x004456CE (O-P unsigned)
    
    // Task variables (A-P for each player)
    uint16_t p1_task_vars[16];         // 0x00470311-0x0047032F (mostly unsigned, P is signed)
    uint16_t p2_task_vars[16];         // 0x0047060D-0x0047062B
    
    // Move history
    uint8_t player_move_history[16];   // 0x47006C - g_player_move_history
    
    // INPUT BUFFER - CRITICAL FOR ROLLBACK
    // Motion inputs like quarter-circle forward (236+P) require input history
    uint16_t p1_input_history[1024];   // 0x4280E0 - P1 input history buffer (1024 frames)
    uint16_t p2_input_history[1024];   // 0x4290E0 - P2 input history buffer (1024 frames)
    uint32_t input_buffer_index;       // Current position in circular input buffer
    
    // CSS INPUT STATE - CRITICAL FOR CSS: Input change detection for just-pressed buttons
    uint32_t player_input_changes[8];  // 0x447f60 - g_player_input_changes[8] array
    
    // INPUT REPEAT LOGIC STATE - CRITICAL FOR ROLLBACK: Static variables from input processing
    uint32_t prev_input_state[8];       // Previous frame input states for repeat logic
    uint32_t input_repeat_state[8];     // Current repeat states for repeat logic  
    uint32_t input_repeat_timer[8];     // Timers for repeat logic
    
    // IMMEDIATE INPUT APPLY STATE - CRITICAL FOR ROLLBACK: ApplyNetworkedInputsImmediately variables
    uint32_t apply_prev_p1_input;       // Previous P1 input for immediate apply change detection
    uint32_t apply_prev_p2_input;       // Previous P2 input for immediate apply change detection
    
    // Object pool (391KB - 1024 objects * 382 bytes each)
    uint8_t object_pool[0x5F800];      // 0x4701E0 - Full object pool capture
    
    // Additional state
    uint32_t object_count;             // 0x004246FC - g_object_count
    uint32_t frame_sync_flag;          // 0x00424700 - g_frame_sync_flag
    uint32_t hit_effect_target;        // 0x4701C4 - g_hit_effect_target
    
    // Character selection state
    uint32_t menu_selection;           // 0x424780 - g_menu_selection
    uint64_t p1_css_cursor;            // 0x00424E50 - p1Cursor (8 bytes)
    uint64_t p2_css_cursor;            // 0x00424E58 - p2Cursor (8 bytes)
    uint32_t p1_char_to_load;          // 0x470020 - p1CharToDisplayAndLoad
    uint32_t p2_char_to_load;          // 0x470024 - p2CharToDisplayAndLoad
    uint32_t p1_color_selection;       // 0x00470024 - g_iPlayer1_Color_Selection
    
    // Metadata
    uint32_t frame_number;             // Frame when this state was saved
    uint64_t timestamp_ms;             // When this save was created
    bool valid;                        // Is this save slot occupied
    uint32_t checksum;                 // Simple validation checksum
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
    bool frame_step_needs_input_refresh;  // Flag to refresh inputs right before frame execution
    
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
    
    // Dedicated save state storage (8 slots for manual saves Shift+1-8)
    SaveStateData save_slots[8];
    
    // Dedicated rollback save state storage for GekkoNet (16 slots)
    SaveStateData rollback_save_slots[16];
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