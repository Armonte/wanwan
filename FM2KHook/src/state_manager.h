#pragma once

#include <windows.h>
#include "ipc.h"

namespace FM2K {
namespace State {

// Memory addresses for game state
namespace Memory {
    // Visual state memory addresses
    constexpr uintptr_t VISUAL_EFFECTS_BASE = 0x40CC30;
    constexpr size_t VISUAL_EFFECTS_SIZE = 0x40CCD4 - 0x40CC30;
    
    // Hit judge table addresses
    constexpr uintptr_t HIT_JUDGE_BASE = 0x40CD00;
    constexpr size_t HIT_JUDGE_SIZE = 0x1000;
    
    // Round state addresses
    constexpr uintptr_t ROUND_TIMER = 0x40CE00;
    constexpr uintptr_t ROUND_LIMIT = 0x40CE04;
    constexpr uintptr_t ROUND_STATE = 0x40CE08;
    constexpr uintptr_t GAME_MODE = 0x40CE0C;
    
    // RNG state
    constexpr uintptr_t RNG_SEED = 0x40CF00;
    constexpr size_t RNG_SEED_SIZE = sizeof(uint32_t);
} // namespace Memory

// Game state structure
struct GameState {
    uint8_t memory[0x1000];  // Main game memory block
    uint32_t checksum;       // Fletcher32 checksum of memory
};

// Initialize state manager
bool Init(HANDLE process);

// Shutdown state manager
void Shutdown();

// Save/load state
bool SaveState(GameState* state, uint32_t* checksum = nullptr);
bool LoadState(const GameState* state);

// Calculate checksum of current state
uint32_t CalculateStateChecksum();

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