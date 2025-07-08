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

bool SaveCoreState(CoreGameState* state) {
    if (!process_handle || !state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid save core state parameters");
        return false;
    }

    SIZE_T bytes_read;
    
    // Read input system state
    if (!ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::INPUT_BUFFER_INDEX_ADDR),
                          &state->input_buffer_index, sizeof(uint32_t), &bytes_read)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read input buffer index");
        return false;
    }
    
    // Read current inputs
    if (!ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::P1_INPUT_ADDR),
                          &state->p1_input_current, sizeof(uint32_t), &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::P2_INPUT_ADDR),
                          &state->p2_input_current, sizeof(uint32_t), &bytes_read)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read current inputs");
        return false;
    }
    
    // Read input histories
    if (!ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::P1_INPUT_HISTORY_ADDR),
                          state->p1_input_history, Memory::INPUT_HISTORY_SIZE, &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::P2_INPUT_HISTORY_ADDR),
                          state->p2_input_history, Memory::INPUT_HISTORY_SIZE, &bytes_read)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read input histories");
        return false;
    }
    
    // Read player state
    if (!ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::P1_STAGE_X_ADDR),
                          &state->p1_stage_x, sizeof(uint32_t), &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::P1_STAGE_Y_ADDR),
                          &state->p1_stage_y, sizeof(uint32_t), &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::P1_HP_ADDR),
                          &state->p1_hp, sizeof(uint32_t), &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::P1_MAX_HP_ADDR),
                          &state->p1_max_hp, sizeof(uint32_t), &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::P2_HP_ADDR),
                          &state->p2_hp, sizeof(uint32_t), &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::P2_MAX_HP_ADDR),
                          &state->p2_max_hp, sizeof(uint32_t), &bytes_read)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read player state");
        return false;
    }
    
    // Read global state
    if (!ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::ROUND_TIMER_ADDR),
                          &state->round_timer, sizeof(uint32_t), &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::GAME_TIMER_ADDR),
                          &state->game_timer, sizeof(uint32_t), &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::RANDOM_SEED_ADDR),
                          &state->random_seed, sizeof(uint32_t), &bytes_read)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read global state");
        return false;
    }
    
    // Read visual effects
    if (!ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::EFFECT_ACTIVE_FLAGS),
                          &state->effect_active_flags, sizeof(uint32_t), &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::EFFECT_TIMERS_BASE),
                          state->effect_timers, Memory::EFFECT_TIMERS_SIZE, &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::EFFECT_COLORS_BASE),
                          state->effect_colors, Memory::EFFECT_COLORS_SIZE, &bytes_read) ||
        !ReadProcessMemory(process_handle, 
                          reinterpret_cast<LPCVOID>(Memory::EFFECT_TARGETS_BASE),
                          state->effect_targets, Memory::EFFECT_TARGETS_SIZE, &bytes_read)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read visual effects");
        return false;
    }
    
    return true;
}

bool LoadCoreState(const CoreGameState* state) {
    if (!process_handle || !state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid load core state parameters");
        return false;
    }

    SIZE_T bytes_written;
    
    // Write input system state
    if (!WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::INPUT_BUFFER_INDEX_ADDR),
                           &state->input_buffer_index, sizeof(uint32_t), &bytes_written)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write input buffer index");
        return false;
    }
    
    // Write current inputs
    if (!WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::P1_INPUT_ADDR),
                           &state->p1_input_current, sizeof(uint32_t), &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::P2_INPUT_ADDR),
                           &state->p2_input_current, sizeof(uint32_t), &bytes_written)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write current inputs");
        return false;
    }
    
    // Write input histories
    if (!WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::P1_INPUT_HISTORY_ADDR),
                           state->p1_input_history, Memory::INPUT_HISTORY_SIZE, &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::P2_INPUT_HISTORY_ADDR),
                           state->p2_input_history, Memory::INPUT_HISTORY_SIZE, &bytes_written)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write input histories");
        return false;
    }
    
    // Write player state
    if (!WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::P1_STAGE_X_ADDR),
                           &state->p1_stage_x, sizeof(uint32_t), &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::P1_STAGE_Y_ADDR),
                           &state->p1_stage_y, sizeof(uint32_t), &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::P1_HP_ADDR),
                           &state->p1_hp, sizeof(uint32_t), &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::P1_MAX_HP_ADDR),
                           &state->p1_max_hp, sizeof(uint32_t), &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::P2_HP_ADDR),
                           &state->p2_hp, sizeof(uint32_t), &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::P2_MAX_HP_ADDR),
                           &state->p2_max_hp, sizeof(uint32_t), &bytes_written)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write player state");
        return false;
    }
    
    // Write global state
    if (!WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::ROUND_TIMER_ADDR),
                           &state->round_timer, sizeof(uint32_t), &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::GAME_TIMER_ADDR),
                           &state->game_timer, sizeof(uint32_t), &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::RANDOM_SEED_ADDR),
                           &state->random_seed, sizeof(uint32_t), &bytes_written)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write global state");
        return false;
    }
    
    // Write visual effects
    if (!WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::EFFECT_ACTIVE_FLAGS),
                           &state->effect_active_flags, sizeof(uint32_t), &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::EFFECT_TIMERS_BASE),
                           state->effect_timers, Memory::EFFECT_TIMERS_SIZE, &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::EFFECT_COLORS_BASE),
                           state->effect_colors, Memory::EFFECT_COLORS_SIZE, &bytes_written) ||
        !WriteProcessMemory(process_handle, 
                           reinterpret_cast<LPVOID>(Memory::EFFECT_TARGETS_BASE),
                           state->effect_targets, Memory::EFFECT_TARGETS_SIZE, &bytes_written)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write visual effects");
        return false;
    }
    
    return true;
}

bool LoadGameState(const GameState* state) {
    if (!state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid load game state parameters");
        return false;
    }
    
    // Load core state to game memory
    return LoadCoreState(&state->core);
}

bool SaveGameState(GameState* state, uint32_t frame_number) {
    if (!state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid save game state parameters");
        return false;
    }
    
    // Save core state
    if (!SaveCoreState(&state->core)) {
        return false;
    }
    
    // Set metadata
    state->frame_number = frame_number;
    state->timestamp_ms = SDL_GetTicks();
    
    // Calculate checksum over the core state
    state->checksum = CalculateCoreStateChecksum(&state->core);
    
    return true;
}

bool SaveState(GameState* state, uint32_t* checksum) {
    if (!state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid save state parameters");
        return false;
    }

    // Use new core state saving
    if (!SaveCoreState(&state->core)) {
        return false;
    }

    // Calculate checksum over the core state
    state->checksum = CalculateCoreStateChecksum(&state->core);
    
    // Return checksum if requested (legacy compatibility)
    if (checksum) {
        *checksum = state->checksum;
    }

    return true;
}

bool LoadState(const GameState* state) {
    if (!state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid load state parameters");
        return false;
    }

    // Use new core state loading
    return LoadCoreState(&state->core);
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

uint32_t CalculateCoreStateChecksum(const CoreGameState* state) {
    if (!state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid core state for checksum");
        return 0;
    }

    // Calculate Fletcher32 checksum over the entire CoreGameState structure
    return Fletcher32(reinterpret_cast<const uint8_t*>(state), sizeof(CoreGameState));
}

} // namespace State
} // namespace FM2K 