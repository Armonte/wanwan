#include "css_handler.h"
#include "globals.h"
#include "logging.h"
#include <SDL3/SDL.h>

// CSS Input Injection System Implementation
void ProcessCSSDelayedInputs() {
    for (int i = 0; i < 2; i++) {
        if (css_delayed_inputs[i].active && css_delayed_inputs[i].frames_remaining > 0) {
            // Inject the input this frame
            InjectPlayerInput(i, css_delayed_inputs[i].input_value);
            css_delayed_inputs[i].frames_remaining--;
            
            if (css_delayed_inputs[i].frames_remaining == 0) {
                css_delayed_inputs[i].active = false;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Input injection completed for player %d", i);
            }
        }
    }
}

void QueueCSSDelayedInput(int player, uint16_t input, uint8_t delay_frames) {
    if (player >= 0 && player < 2) {
        css_delayed_inputs[player].input_value = input;
        css_delayed_inputs[player].frames_remaining = delay_frames;
        css_delayed_inputs[player].active = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Queued input 0x%X for player %d (delay: %d frames)", 
                   input, player, delay_frames);
    }
}

uint16_t ExtractColorButton(uint16_t input_flags) {
    // Extract color button from input flags (0x3F0 mask covers 0x010-0x200)
    if (input_flags & 0x010) return 0x010;   // Button A (bit 4)
    if (input_flags & 0x020) return 0x020;   // Button B (bit 5)
    if (input_flags & 0x040) return 0x040;   // Button C (bit 6)
    if (input_flags & 0x080) return 0x080;   // Button D (bit 7)
    if (input_flags & 0x100) return 0x100;   // Button E (bit 8)
    if (input_flags & 0x200) return 0x200;   // Button F (bit 9)
    return 0x00; // No color button pressed
}

void InjectPlayerInput(int player, uint16_t input_value) {
    // Inject directly into the live input variables that the game uses
    if (player == 0) {
        live_p1_input |= input_value;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Injected button 0x%X into live_p1_input (result: 0x%03X)", 
                   input_value, live_p1_input & 0x7FF);
    } else if (player == 1) {
        live_p2_input |= input_value;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Injected button 0x%X into live_p2_input (result: 0x%03X)", 
                   input_value, live_p2_input & 0x7FF);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Invalid player %d for injection", player);
    }
}
