#ifndef CSS_HANDLER_H
#define CSS_HANDLER_H

#include <cstdint>

void ProcessCSSDelayedInputs();
void QueueCSSDelayedInput(int player, uint16_t input, uint8_t delay_frames);
uint16_t ExtractColorButton(uint16_t input_flags);
void InjectPlayerInput(int player, uint16_t input_value);

#endif // CSS_HANDLER_H
