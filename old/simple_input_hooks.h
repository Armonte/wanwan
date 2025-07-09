#pragma once

#include <windows.h>

// Use the existing GInput definition from the rollback system
#include "hooks/game_logic/rollback_state.hpp"

// Function pointer types for original input functions
// Match the game's original function signature
typedef int (*HandleInputsFunc)(void);

// External variables for original functions (if needed for fallback)
extern HandleInputsFunc originalHandleP1Inputs;
extern HandleInputsFunc originalHandleP2Inputs;

// Hook function declarations
extern "C" int HandleP1InputsHook(void);
extern "C" unsigned char __fastcall HandleP2InputsHook(void);

// Installation function
bool installSimplifiedInputHooks();

// Initialize InputManager and perform auto-assignment (called from initgame_replacement.cpp)
bool initializeControllerSystem();

// Utility function for input conversion
unsigned char convertToGameFormat(unsigned int sdl3Input, bool isPlayerTwo);

// ==============================================================================
// NEW FUNCTIONS FOR GEKKONET ROLLBACK INTEGRATION
// ==============================================================================

// Window message input tracking (called from SDL3 window proc)
extern "C" void UpdateInputFromWindowMessage(UINT message, WPARAM wParam);
extern "C" void ClearConsumedInputs(bool force_clear);

// Direct Windows key to ML2 input conversion
unsigned char ConvertWindowsKeysToML2Input(bool isP2);

// GekkoNet integration functions
extern "C" void InitializeGekkoOfflineMode(void);
extern "C" void EnableGekkoOfflineMode(void);   // Explicitly enable for battle/testing
extern "C" void DisableGekkoOfflineMode(void);  // Disable when returning to menus
extern "C" void ProcessGekkoNetFrame(void);
extern "C" void DebugCurrentInputState(void);

// Bridge functions for rollback input system
extern "C" {
    GInput get_p1_input_bridge(void);
    GInput get_p2_input_bridge(void);
    unsigned char ginput_to_byte(GInput input);
    GInput byte_to_ginput(unsigned char input);
}

// Controller config interface functions
extern "C" {
    bool IsControllerConfigOpen(void);
    void CloseControllerConfig(void);
} 