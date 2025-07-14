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
            uint32_t round_timer;
            uint32_t game_timer;
            uint32_t random_seed;
            uint32_t timer_countdown1;
            uint32_t timer_countdown2;
            uint32_t round_timer_counter;
            uint32_t object_list_heads;
            uint32_t object_list_tails;
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
    }
} 