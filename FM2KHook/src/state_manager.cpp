#include "state_manager.h"
#include <SDL3/SDL.h>

namespace FM2K {
namespace State {

// Static state
static HANDLE process_handle = nullptr;
static GameState* current_state = nullptr;

bool Init(HANDLE process) {
    if (!process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle");
        return false;
    }

    process_handle = process;

    // Allocate state buffer
    current_state = new GameState();
    if (!current_state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to allocate state buffer");
        return false;
    }

    return true;
}

void Shutdown() {
    delete current_state;
    current_state = nullptr;
    process_handle = nullptr;
}

bool SaveState(GameState* state, uint32_t* checksum) {
    if (!process_handle || !state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid save state parameters");
        return false;
    }

    // Read game memory into state buffer
    SIZE_T bytes_read;
    if (!ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::VISUAL_EFFECTS_BASE),
                          state->memory,
                          Memory::VISUAL_EFFECTS_SIZE,
                          &bytes_read)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to read visual effects memory: %lu", GetLastError());
        return false;
    }

    // Calculate checksum if requested
    if (checksum) {
        *checksum = Fletcher32(state->memory, Memory::VISUAL_EFFECTS_SIZE);
    }

    return true;
}

bool LoadState(const GameState* state) {
    if (!process_handle || !state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid load state parameters");
        return false;
    }

    // Write state buffer back to game memory
    SIZE_T bytes_written;
    if (!WriteProcessMemory(process_handle,
                           reinterpret_cast<LPVOID>(Memory::VISUAL_EFFECTS_BASE),
                           state->memory,
                           Memory::VISUAL_EFFECTS_SIZE,
                           &bytes_written)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to write visual effects memory: %lu", GetLastError());
        return false;
    }

    return true;
}

uint32_t CalculateStateChecksum() {
    if (!current_state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "State buffer not allocated");
        return 0;
    }

    uint32_t checksum;
    if (!SaveState(current_state, &checksum)) {
        return 0;
    }

    return checksum;
}

uint32_t Fletcher32(const uint8_t* data, size_t len) {
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    size_t blocks = len / 2;

    // Process 2-byte blocks
    while (blocks) {
        size_t tlen = blocks > 359 ? 359 : blocks;
        blocks -= tlen;
        do {
            sum1 += (data[0] << 8) | data[1];
            sum2 += sum1;
            data += 2;
        } while (--tlen);

        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }

    // Handle remaining byte if length is odd
    if (len & 1) {
        sum1 += *data << 8;
        sum2 += sum1;
        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }

    // Final reduction
    sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
    sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);

    return (sum2 << 16) | sum1;
}

} // namespace State
} // namespace FM2K 