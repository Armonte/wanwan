#pragma once

#include "common.h"

namespace FM2K {
    // REMOVED: MinimalGameState - using only MinimalChecksumState for consistency

    // Active object info structure
    struct ActiveObjectInfo {
        uint32_t index;
        uint32_t type_or_id;
        bool is_active;
    };

    namespace State {
        // Forward declaration for Fletcher32 checksum function
        uint32_t Fletcher32(const uint8_t* data, size_t len);
        
        // UNIFIED: Single state structure for both rollback and checksumming (no padding, consistent across clients)
        #pragma pack(push, 1)  // Ensure no padding between fields
        struct CoreGameState {
            uint32_t input_buffer_index;
            uint16_t p1_input_current;
            uint16_t p2_input_current;
            uint32_t p1_hp;
            uint32_t p2_hp;
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
            
            // UNIFIED: Single checksum calculation method for consistency
            uint32_t CalculateChecksum() const {
                return FM2K::State::Fletcher32((const uint8_t*)this, sizeof(CoreGameState));
            }
        };
        #pragma pack(pop)

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
        
        // REMOVED: MinimalChecksumState - now using CoreGameState for both rollback and checksumming
    }
} 