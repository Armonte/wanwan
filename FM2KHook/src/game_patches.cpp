#include "game_patches.h"
#include "globals.h"
#include "logging.h"
#include <windows.h>
#include <SDL3/SDL.h>
#include <cstring>

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

void DisableInputRepeatDelays() {
    // DISABLE NATIVE REPEAT SUPPRESSION: Set delay values to 0 for instant response
    // Native FM2K has 50-frame initial delay and 5-frame repeat delay which blocks rapid inputs
    
    DWORD oldProtect;
    
    // g_input_initial_delay at 0x41e3fc - default is 50 (0x32) frames
    uint32_t* initial_delay = (uint32_t*)0x41e3fc;
    VirtualProtect(initial_delay, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
    *initial_delay = 0;  // Disable initial delay
    VirtualProtect(initial_delay, 4, oldProtect, &oldProtect);
    
    // g_input_repeat_delay at 0x41e400 - default is 5 (0x5) frames  
    uint32_t* repeat_delay = (uint32_t*)0x41e400;
    VirtualProtect(repeat_delay, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
    *repeat_delay = 0;  // Disable repeat delay
    VirtualProtect(repeat_delay, 4, oldProtect, &oldProtect);
    
    // Clear the repeat timer array at 0x4d1c40 (8 devices * 4 bytes each)
    uint32_t* repeat_timers = (uint32_t*)0x4d1c40;
    VirtualProtect(repeat_timers, 32, PAGE_EXECUTE_READWRITE, &oldProtect);
    memset(repeat_timers, 0, 32);  // Clear all 8 device timers
    VirtualProtect(repeat_timers, 32, oldProtect, &oldProtect);
    
    // Clear the repeat state array at 0x541f80 (8 devices * 4 bytes each)
    uint32_t* repeat_states = (uint32_t*)0x541f80;
    VirtualProtect(repeat_states, 32, PAGE_EXECUTE_READWRITE, &oldProtect);
    memset(repeat_states, 0, 32);  // Clear all 8 device states
    VirtualProtect(repeat_states, 32, oldProtect, &oldProtect);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                "FM2K PATCH: Disabled native input repeat delays (was 50/5 frames, now 0/0) and cleared timer arrays");
}

// NOP out ShowCursor(0) call to keep cursor visible for ImGui
void DisableCursorHiding() {
    // NOP the ShowCursor(0) call at +49E7 through +49ED (7 bytes)
    // This prevents FM2K from hiding the cursor in DirectDraw mode
    uint8_t* cursor_hide_addr = (uint8_t*)0x4049E7;  // Base address + 49E7
    
    DWORD old_protect;
    if (VirtualProtect(cursor_hide_addr, 7, PAGE_EXECUTE_READWRITE, &old_protect)) {
        // Fill with NOPs (0x90)
        memset(cursor_hide_addr, 0x90, 7);
        
        // Restore original protection
        VirtualProtect(cursor_hide_addr, 7, old_protect, &old_protect);
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K PATCH: NOPed ShowCursor(0) call at 0x4049E7-0x4049ED for ImGui cursor visibility");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K PATCH: Failed to make ShowCursor(0) memory writable at 0x4049E7");
    }
}
