#include "game_patches.h"
#include "globals.h"
#include "logging.h"
#include <windows.h>
#include <SDL3/SDL.h>

// New hook for boot-to-character-select hack
// This hook modifies the game's initialization to boot directly to character select screen
// instead of showing the title screen and splash screens. It does this by:
// 1. Setting the character select mode flag to 1 (vs player mode instead of vs CPU)
// 2. Changing the initialization object byte from 0x11 to 0x0A to skip to character select
void ApplyBootToCharacterSelectPatches() {
    // Change initialization object from 0x11 to 0x0A to boot to character select
    uint8_t* init_object_ptr = (uint8_t*)0x409CD9;
    if (!IsBadReadPtr(init_object_ptr, sizeof(uint16_t))) {
        // Make the memory writable
        DWORD old_protect;
        if (VirtualProtect(init_object_ptr, 2, PAGE_EXECUTE_READWRITE, &old_protect)) {
            // Write the instruction: 6A 0A (push 0x0A)
            init_object_ptr[0] = 0x6A;  // push instruction
            init_object_ptr[1] = 0x0A;  // immediate value
            
            // Restore original protection
            VirtualProtect(init_object_ptr, 2, old_protect, &old_protect);
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Wrote instruction 6A 0A at 0x409CD9");
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to make memory writable at 0x409CD9");
        }
    }
    
}

// Set character select mode flag - this ensures VS player mode instead of VS CPU
void ApplyCharacterSelectModePatches() {
    uint8_t* char_select_mode_ptr = (uint8_t*)FM2K::State::Memory::CHARACTER_SELECT_MODE_ADDR;
    if (!IsBadWritePtr(char_select_mode_ptr, sizeof(uint8_t))) {
        DWORD old_protect;
        if (VirtualProtect(char_select_mode_ptr, sizeof(uint8_t), PAGE_READWRITE, &old_protect)) {
            *char_select_mode_ptr = 1;
            VirtualProtect(char_select_mode_ptr, sizeof(uint8_t), old_protect, &old_protect);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Set character select mode to VS player");
        }
    }
}

// Hook for game_rand function to ensure deterministic RNG
uint32_t __cdecl Hook_GameRand() {
    // SIMPLE FIX: Force g_rand_seed to constant value to prevent RNG desyncs
    uint32_t* g_rand_seed_ptr = (uint32_t*)0x41FB1C;
    *g_rand_seed_ptr = 1337;
    
    // Return a constant value to prevent any RNG divergence
    return 1337;
}
