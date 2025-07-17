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
            uint32_t p1_hp;
            uint32_t p2_hp;
            uint32_t p1_x;                     // P1 position X
            uint32_t p1_y;                     // P1 position Y
            uint32_t p2_x;                     // P2 position X
            uint32_t p2_y;                     // P2 position Y
            uint32_t round_timer;
            uint32_t game_timer;
            uint32_t random_seed;
            uint32_t timer_countdown1;
            uint32_t timer_countdown2;
            uint32_t round_timer_counter;
            uint32_t object_list_heads;
            uint32_t object_list_tails;
            
            // Game mode and menu state synchronization
            uint32_t game_mode;
            uint32_t fm2k_game_mode;
            uint32_t character_select_mode;
            
            // Character Select Menu State (critical for CSS synchronization)
            uint32_t menu_selection;           // Main menu cursor
            uint32_t p1_css_cursor_x;          // P1 cursor X (column)
            uint32_t p1_css_cursor_y;          // P1 cursor Y (row)
            uint32_t p2_css_cursor_x;          // P2 cursor X (column)
            uint32_t p2_css_cursor_y;          // P2 cursor Y (row)
            uint32_t p1_selected_char;         // P1 selected character ID
            uint32_t p2_selected_char;         // P2 selected character ID
            uint32_t p1_char_related;          // P1 related character
            uint32_t p2_char_related;          // P2 related character
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