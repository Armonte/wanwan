#pragma once

#include <windows.h>
#include "ipc.h"

namespace FM2K {
namespace State {

// Memory addresses for game state (from FM2K_Integration.h)
namespace Memory {
    // Core game state addresses
    constexpr uintptr_t INPUT_BUFFER_INDEX_ADDR = 0x470000;
    constexpr uintptr_t RANDOM_SEED_ADDR = 0x41FB1C;
    
    // Player 1 state
    constexpr uintptr_t P1_INPUT_ADDR = 0x470100;
    constexpr uintptr_t P1_STAGE_X_ADDR = 0x470104;
    constexpr uintptr_t P1_STAGE_Y_ADDR = 0x470108;
    constexpr uintptr_t P1_HP_ADDR = 0x47010C;
    constexpr uintptr_t P1_MAX_HP_ADDR = 0x470110;
    constexpr uintptr_t P1_INPUT_HISTORY_ADDR = 0x470200;
    
    // Player 2 state
    constexpr uintptr_t P2_INPUT_ADDR = 0x470300;
    constexpr uintptr_t P2_HP_ADDR = 0x47030C;
    constexpr uintptr_t P2_MAX_HP_ADDR = 0x470310;
    constexpr uintptr_t P2_INPUT_HISTORY_ADDR = 0x470400;
    
    // Global state
    constexpr uintptr_t ROUND_TIMER_ADDR = 0x470060;
    constexpr uintptr_t GAME_TIMER_ADDR = 0x470064;
    
    // Visual effects
    constexpr uintptr_t EFFECT_ACTIVE_FLAGS = 0x40CC30;
    constexpr uintptr_t EFFECT_TIMERS_BASE = 0x40CC34;
    constexpr uintptr_t EFFECT_COLORS_BASE = 0x40CC54;
    constexpr uintptr_t EFFECT_TARGETS_BASE = 0x40CCD4;
    
    // Memory region sizes
    constexpr size_t INPUT_HISTORY_SIZE = 1024 * sizeof(uint32_t); // 1024 frames
    constexpr size_t EFFECT_TIMERS_SIZE = 8 * sizeof(uint32_t);
    constexpr size_t EFFECT_COLORS_SIZE = 8 * 3 * sizeof(uint32_t); // 8 RGB sets
    constexpr size_t EFFECT_TARGETS_SIZE = 8 * sizeof(uint32_t);
} // namespace Memory

// Comprehensive game state structure
struct CoreGameState {
    // Input system
    uint32_t input_buffer_index;     // Current position in input ring buffer
    uint32_t p1_input_current;       // Player 1 current input
    uint32_t p2_input_current;       // Player 2 current input
    uint32_t p1_input_history[1024]; // Player 1 input history (1024 frames)
    uint32_t p2_input_history[1024]; // Player 2 input history (1024 frames)
    
    // Player state
    uint32_t p1_stage_x, p1_stage_y; // Player 1 position
    uint32_t p1_hp, p1_max_hp;       // Player 1 health
    uint32_t p2_hp, p2_max_hp;       // Player 2 health
    
    // Global state
    uint32_t round_timer;             // Round timer
    uint32_t game_timer;              // Game timer
    uint32_t random_seed;             // RNG seed
    
    // Visual effects
    uint32_t effect_active_flags;     // Bitfield of active effects
    uint32_t effect_timers[8];        // Effect timer array
    uint32_t effect_colors[8][3];     // Effect colors (RGB)
    uint32_t effect_targets[8];       // Effect target IDs
};

// Enhanced game state structure
struct GameState {
    CoreGameState core;           // Main game state
    uint32_t checksum;           // Fletcher32 checksum
    uint32_t frame_number;       // Frame when state was captured
    uint64_t timestamp_ms;       // SDL timestamp when captured
};

// Initialize state manager
bool Init(HANDLE process);

// Shutdown state manager
void Shutdown();

// Save/load state - enhanced functions
bool SaveGameState(GameState* state, uint32_t frame_number);
bool LoadGameState(const GameState* state);

// Core state operations
bool SaveCoreState(CoreGameState* state);
bool LoadCoreState(const CoreGameState* state);

// Legacy compatibility
bool SaveState(GameState* state, uint32_t* checksum = nullptr);
bool LoadState(const GameState* state);

// Calculate checksum of current state
uint32_t CalculateStateChecksum();
uint32_t CalculateCoreStateChecksum(const CoreGameState* state);

// Helper functions
uint32_t Fletcher32(const uint8_t* data, size_t len);

// Visual state operations
bool ReadVisualState(IPC::VisualState* state);
bool WriteVisualState(const IPC::VisualState* state);

namespace Memory {
    // Memory region helpers
    bool ReadHitJudgeTables(uint8_t* buffer, size_t size);
    bool WriteHitJudgeTables(const uint8_t* buffer, size_t size);
    
    bool ReadRoundState(uint32_t* timer, uint32_t* limit, uint32_t* state, uint32_t* mode);
    bool WriteRoundState(uint32_t timer, uint32_t limit, uint32_t state, uint32_t mode);
    
    // Utility to read/write any memory region
    bool ReadMemoryRegion(uintptr_t address, void* buffer, size_t size);
    bool WriteMemoryRegion(uintptr_t address, const void* buffer, size_t size);
} // namespace Memory

} // namespace State
} // namespace FM2K 