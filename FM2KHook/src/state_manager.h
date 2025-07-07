#pragma once

#include "ipc.h"
#include <windows.h>

namespace FM2K {
namespace State {

// Initialize state management system
bool Init(HANDLE process);

// Shutdown and cleanup
void Shutdown();

// Save complete game state to buffer
bool SaveState(GameState* state, uint32_t* checksum);

// Load complete game state from buffer
bool LoadState(const GameState* state);

// Calculate checksum of current game state
uint32_t CalculateStateChecksum();

// Verify loaded state matches expected checksum
bool VerifyState(uint32_t expected_checksum);

namespace Memory {
    // Memory region helpers
    bool ReadHitJudgeTables(uint8_t* buffer, size_t size);
    bool WriteHitJudgeTables(const uint8_t* buffer, size_t size);
    
    bool ReadVisualState(VisualState* state);
    bool WriteVisualState(const VisualState* state);
    
    bool ReadRoundState(uint32_t* timer, uint32_t* limit, uint32_t* state, uint32_t* mode);
    bool WriteRoundState(uint32_t timer, uint32_t limit, uint32_t state, uint32_t mode);
    
    // Utility to read/write any memory region
    bool ReadMemoryRegion(uintptr_t address, void* buffer, size_t size);
    bool WriteMemoryRegion(uintptr_t address, const void* buffer, size_t size);
} // namespace Memory

} // namespace State
} // namespace FM2K 