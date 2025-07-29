#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <cstdint>

// Function declarations for input handling logic
void PatchInputBufferWrites(bool block);
uint16_t PollSDLKeyboard();
int __cdecl Hook_GetPlayerInput(int player_id, int input_type);
uint32_t ConvertNetworkInputToGameFormat(uint32_t network_input);
bool IsWindowFocused();

// CCCaster-style direct input capture
uint16_t CaptureDirectInput();

// REMOVED: All intermediate input writing functions
// Following Heat's advice - only override FINAL processed state in GekkoNet callbacks

#endif // INPUT_HANDLER_H 