#pragma once

#include "common.h"

namespace FM2K {
    struct MinimalGameState {
        uint32_t p1_hp, p2_hp;
        uint32_t p1_max_hp, p2_max_hp;
        uint32_t p1_x, p1_y;
        uint32_t p2_x, p2_y;
        uint32_t round_timer;
        uint32_t random_seed;
        uint32_t frame_number;
        uint32_t input_checksum;
        
        uint32_t CalculateChecksum() const;
        bool LoadFromMemory();
        bool SaveToMemory() const;
    };

    // Active object info structure
    struct ActiveObjectInfo {
        uint32_t index;
        uint32_t type_or_id;
        bool is_active;
    };

    namespace State {
        struct CoreGameState {
            uint32_t input_buffer_index;
            uint16_t p1_input_current;
            uint16_t p2_input_current;
            uint32_t p1_hp;                    // 0x004DFC85
            uint32_t p2_hp;                    // 0x004EDCC4
            uint32_t p1_x;                     // 0x004DFCC3 - P1 position X
            uint32_t p1_y;                     // 0x004DFCC7 - P1 position Y (actually 2 bytes)
            uint32_t p2_x;                     // 0x004EDD02 - P2 position X
            uint32_t p2_y;                     // 0x004EDD06 - P2 position Y (actually 2 bytes)
            uint32_t round_timer;              // 0x00470060
            uint32_t game_timer;               // 0x470050 - g_actual_wanwan_timer
            uint32_t random_seed;              // 0x41FB1C - g_rand
            uint32_t timer_countdown1;
            uint32_t timer_countdown2;
            uint32_t round_timer_counter;
            uint32_t object_list_heads;
            uint32_t object_list_tails;
            
            // Game mode and menu state synchronization
            uint32_t game_mode;                // 0x00470054 (2 bytes)
            uint32_t fm2k_game_mode;           // 0x470040
            uint32_t character_select_mode;
            
            // Character Select Menu State (critical for CSS synchronization)
            uint32_t menu_selection;           // 0x424780 - g_menu_selection
            uint64_t p1_css_cursor;            // 0x00424E50 - p1Cursor (8 bytes)
            uint64_t p2_css_cursor;            // 0x00424E58 - p2Cursor (8 bytes)
            uint32_t p1_char_to_load;          // 0x470020 - p1CharToDisplayAndLoad
            uint32_t p2_char_to_load;          // 0x470024 - p2CharToDisplayAndLoad
            uint32_t p1_char_related;          // 0x4CF9E0 - u_p1_related
            uint32_t p2_char_related;          // 0x4CF9E4 - u_p2_related
            
            // Additional game state variables
            uint32_t frame_skip_count;         // 0x004246F4 - g_frame_skip_count
            uint32_t object_count;             // 0x004246FC - g_object_count
            uint32_t frame_sync_flag;          // 0x00424700 - g_frame_sync_flag
            uint32_t player_char_selection;    // 0x470020 - g_player_character_selection
            uint32_t p1_color_selection;       // 0x00470024 - g_iPlayer1_Color_Selection
            uint32_t round_limit;              // 0x470048 - g_round_limit
            uint32_t round_state;              // 0x47004C - g_round_state
            uint32_t css_mode_rel;             // 0x470058 - g_css_mode_rel
            uint32_t team_round_setting;       // 0x470064 - g_team_round_setting
            uint32_t round_setting;            // 0x470068 - g_round_setting
            uint32_t game_paused;              // 0x4701BC - g_game_paused
            uint32_t replay_mode;              // 0x4701C0 - g_replay_mode
            uint32_t hit_effect_target;        // 0x4701C4 - g_hit_effect_target
            
            // Player meter/super/stock
            uint32_t p1_super;                 // 0x004DFC9D - P1 Super
            uint32_t p2_super;                 // 0x004EDCDC - P2 Super
            uint32_t p1_special_stock;         // 0x004DFC95 - P1 Special Stock
            uint32_t p2_special_stock;         // 0x004EDCD4 - P2 Special Stock
            uint32_t p1_rounds_won;            // 0x004DFC6D - P1 Rounds Won
            uint32_t p2_rounds_won;            // 0x004EDCAC - P2 Rounds Won
            
            // Camera position
            uint32_t camera_x;                 // 0x00447F2C - g_camera_x
            uint32_t camera_y;                 // 0x00447F30 - g_camera_y
            
            // Character variables (A-P for each player)
            int16_t p1_char_var_a;             // 0x004DFD17 - Char Var A
            int16_t p1_char_var_b;             // 0x004DFD19 - Char Var B
            int16_t p1_char_var_c;             // 0x004DFD1B - Char Var C
            int16_t p1_char_var_d;             // 0x004DFD1D - Char Var D
            int16_t p1_char_var_e;             // 0x004DFD1F - Char Var E
            int16_t p1_char_var_f;             // 0x004DFD21 - Char Var F
            int16_t p1_char_var_g;             // 0x004DFD23 - Char Var G
            int16_t p1_char_var_h;             // 0x004DFD25 - Char Var H
            int16_t p1_char_var_i;             // 0x004DFD27 - Char Var I
            int16_t p1_char_var_j;             // 0x004DFD29 - Char Var J
            int16_t p1_char_var_k;             // 0x004DFD2B - Char Var K
            int16_t p1_char_var_l;             // 0x004DFD2D - Char Var L
            int16_t p1_char_var_m;             // 0x004DFD2F - Char Var M
            int16_t p1_char_var_n;             // 0x004DFD31 - Char Var N
            int16_t p1_char_var_o;             // 0x004DFD33 - Char Var O
            int16_t p1_char_var_p;             // 0x004DFD35 - Char Var P
            
            int16_t p2_char_var_a;             // 0x004EDD56 - Char Var A P2
            int16_t p2_char_var_b;             // 0x004EDD58 - Char Var B P2
            int16_t p2_char_var_c;             // 0x004EDD5A - Char Var C P2
            int16_t p2_char_var_d;             // 0x004EDD5C - Char Var D P2
            int16_t p2_char_var_e;             // 0x004EDD5E - Char Var E P2
            int16_t p2_char_var_f;             // 0x004EDD60 - Char Var F P2
            int16_t p2_char_var_g;             // 0x004EDD62 - Char Var G P2
            int16_t p2_char_var_h;             // 0x004EDD64 - Char Var H P2
            int16_t p2_char_var_i;             // 0x004EDD66 - Char Var I P2
            int16_t p2_char_var_j;             // 0x004EDD68 - Char Var J P2
            int16_t p2_char_var_k;             // 0x004EDD6A - Char Var K P2
            int16_t p2_char_var_l;             // 0x004EDD6C - Char Var L P2
            int16_t p2_char_var_m;             // 0x004EDD6E - Char Var M P2
            int16_t p2_char_var_n;             // 0x004EDD70 - Char Var N P2
            int16_t p2_char_var_o;             // 0x004EDD72 - Char Var O P2
            int16_t p2_char_var_p;             // 0x004EDD74 - Char Var P P2
            
            // System variables (A-P)
            int16_t sys_var_a;                 // 0x004456B0 - System Var A
            int16_t sys_var_b;                 // 0x004456B2 - System Var B
            int16_t sys_var_c;                 // 0x004456B4 - System Var C
            int16_t sys_var_d;                 // 0x004456B6 - System Var D
            int16_t sys_var_e;                 // 0x004456B8 - system Var E
            int16_t sys_var_f;                 // 0x004456BA - System Var F
            int16_t sys_var_g;                 // 0x004456BC - System Var G
            int16_t sys_var_h;                 // 0x004456BE - System Var H
            int16_t sys_var_i;                 // 0x004456C0 - System Var I
            int16_t sys_var_j;                 // 0x004456C2 - system Var J
            int16_t sys_var_k;                 // 0x004456C4 - System Var K
            int16_t sys_var_l;                 // 0x004456C6 - System Var L
            int16_t sys_var_m;                 // 0x004456C8 - System Var M
            int16_t sys_var_n;                 // 0x004456CA - System Var N
            uint16_t sys_var_o;                // 0x004456CC - System Var O (unsigned)
            uint16_t sys_var_p;                // 0x004456CE - system Var P (unsigned)
            
            // Task variables (A-P for each player)
            uint16_t p1_task_var_a;            // 0x00470311 - Task Var A
            int16_t p1_task_var_b;             // 0x00470313 - Task Var B
            uint16_t p1_task_var_c;            // 0x00470315 - Task Var C
            uint16_t p1_task_var_d;            // 0x00470317 - Task Var D
            uint16_t p1_task_var_e;            // 0x00470319 - Task Var E
            uint16_t p1_task_var_f;            // 0x0047031B - Task Var F
            uint16_t p1_task_var_g;            // 0x0047031D - Task Var G
            uint16_t p1_task_var_h;            // 0x0047031F - Task Var H
            uint16_t p1_task_var_i;            // 0x00470321 - Task Var I
            uint16_t p1_task_var_j;            // 0x00470323 - Task Var J
            uint16_t p1_task_var_k;            // 0x00470325 - Task Var K
            uint16_t p1_task_var_l;            // 0x00470327 - Task Var L
            uint16_t p1_task_var_m;            // 0x00470329 - Task Var M
            uint16_t p1_task_var_n;            // 0x0047032B - Task Var N
            uint16_t p1_task_var_o;            // 0x0047032D - Task Var O
            int16_t p1_task_var_p;             // 0x0047032F - Task Var P
            
            uint16_t p2_task_var_a;            // 0x0047060D - Task Var A P2
            uint16_t p2_task_var_b;            // 0x0047060F - Task Var B P2
            uint16_t p2_task_var_c;            // 0x00470611 - Task Var C P2
            uint16_t p2_task_var_d;            // 0x00470613 - Task Var D P2
            uint16_t p2_task_var_e;            // 0x00470615 - Task Var E P2
            uint16_t p2_task_var_f;            // 0x00470617 - Task Var F P2
            uint16_t p2_task_var_g;            // 0x00470619 - Task Var G P2
            uint16_t p2_task_var_h;            // 0x0047061B - Task Var H P2
            uint16_t p2_task_var_i;            // 0x0047061D - Task Var I P2
            uint16_t p2_task_var_j;            // 0x0047061F - Task Var J P2
            uint16_t p2_task_var_k;            // 0x00470621 - Task Var K P2
            uint16_t p2_task_var_l;            // 0x00470623 - Task Var L P2
            uint16_t p2_task_var_m;            // 0x00470625 - Task Var M P2
            uint16_t p2_task_var_n;            // 0x00470627 - Task Var N P2
            uint16_t p2_task_var_o;            // 0x00470629 - Task Var O P2
            uint16_t p2_task_var_p;            // 0x0047062B - Task Var P P2
            
            // Move history (16 bytes)
            uint8_t player_move_history[16];   // 0x47006C - g_player_move_history
        };

        struct GameState {
            CoreGameState core;
            uint32_t frame_number;
            uint64_t timestamp_ms;
            uint32_t checksum;
        };

        bool InitializeStateManager();
        void CleanupStateManager();
        bool SaveStateFast(GameState* state, uint32_t frame_number);
        bool RestoreStateFast(const GameState* state, uint32_t target_frame);
        bool SaveStateToSlot(uint32_t slot, uint32_t frame_number);
        bool LoadStateFromSlot(uint32_t slot);
        bool SaveCoreStateBasic(GameState* state, uint32_t frame_number);
        bool RestoreStateFromStruct(const GameState* state, uint32_t target_frame);
        void SaveStateToBuffer(uint32_t frame_number);
        uint32_t Fletcher32(const uint8_t* data, size_t len);
        
        // GEKKONET INTERFACE: Functions required for proper GekkoNet save/load
        uint32_t GetGameStateSize();
        void* GetGameStatePointer(); 
        uint32_t CalculateStateChecksum(const void* state, uint32_t size);
        
        // SLOT ACCESS: Functions to access save slot system
        GameState* GetSaveSlot(uint32_t slot);
        bool IsSlotOccupied(uint32_t slot);
        void SetSaveSlot(uint32_t slot, const GameState& state);
        void SetSlotOccupied(uint32_t slot, bool occupied);
        
        // BSNES PATTERN: In-memory rollback buffer system (fast, no file I/O)
        bool SaveStateToMemoryBuffer(uint32_t slot, uint32_t frame_number);
        bool LoadStateFromMemoryBuffer(uint32_t slot);
        uint32_t GetStateChecksum(uint32_t slot);
        
        // SDL2 PATTERN: Minimal state for checksumming (only essential data, no volatile fields)
        struct MinimalChecksumState {
            uint32_t p1_hp;
            uint32_t p2_hp;
            uint32_t p1_x;
            uint32_t p1_y;
            uint32_t p2_x;
            uint32_t p2_y;
            uint32_t game_mode;
            // Add more essential fields as needed
        };
    }
} 