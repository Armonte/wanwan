#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <cstdint>

// Function declarations for input handling logic
void PatchInputBufferWrites(bool block);
void CaptureRealInputs();
uint16_t PollSDLKeyboard();
int __cdecl Hook_GetPlayerInput(int player_id, int input_type);
uint32_t ConvertNetworkInputToGameFormat(uint32_t network_input);
int __cdecl FM2K_ProcessGameInputs_GekkoNet();
int __cdecl Hook_ProcessGameInputs();

#endif // INPUT_HANDLER_H 