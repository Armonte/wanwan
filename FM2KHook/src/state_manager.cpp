#include "state_manager.h"
#include "SDL3/SDL.h"

namespace FM2K {
namespace State {

// Memory addresses from research
namespace Addresses {
    constexpr uintptr_t HIT_JUDGE_TABLES_START = 0x42470C;
    constexpr uintptr_t HIT_JUDGE_TABLES_END   = 0x430120;
    constexpr uintptr_t ROUND_TIMER    = 0x470060;
    constexpr uintptr_t ROUND_LIMIT    = 0x470048;
    constexpr uintptr_t ROUND_STATE    = 0x47004C;
    constexpr uintptr_t GAME_MODE      = 0x470040;
    constexpr uintptr_t RANDOM_SEED    = 0x41FB1C;
    
    // Sprite effect system addresses
    constexpr uintptr_t EFFECT_ACTIVE_FLAGS = 0x40CC30;
    constexpr uintptr_t EFFECT_TIMERS_BASE = 0x40CC34;
    constexpr uintptr_t EFFECT_COLORS_BASE = 0x40CC54;
    constexpr uintptr_t EFFECT_TARGETS_BASE = 0x40CCD4;
}

static HANDLE g_process = nullptr;

bool Init(HANDLE process) {
    g_process = process;
    return true;
}

void Shutdown() {
    g_process = nullptr;
}

bool SaveState(GameState* state, uint32_t* checksum) {
    if (!state || !g_process) return false;

    // Read hit judge tables
    if (!Memory::ReadHitJudgeTables(state->hit_judge_tables,
                                   sizeof(state->hit_judge_tables))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to read hit judge tables");
        return false;
    }

    // Read visual state
    if (!Memory::ReadVisualState(&state->visual_state)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to read visual state");
        return false;
    }

    // Read round system state
    if (!Memory::ReadRoundState(&state->round_timer,
                               &state->round_limit,
                               &state->round_state,
                               &state->game_mode)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to read round state");
        return false;
    }

    // Read RNG state
    if (!Memory::ReadMemoryRegion(Addresses::RANDOM_SEED,
                                 &state->random_seed,
                                 sizeof(state->random_seed))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to read RNG state");
        return false;
    }

    // Calculate checksum if requested
    if (checksum) {
        *checksum = CalculateStateChecksum();
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                    "State saved with checksum: 0x%08x", *checksum);
    }

    return true;
}

bool LoadState(const GameState* state) {
    if (!state || !g_process) return false;

    // Write hit judge tables
    if (!Memory::WriteHitJudgeTables(state->hit_judge_tables,
                                    sizeof(state->hit_judge_tables))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to write hit judge tables");
        return false;
    }

    // Write visual state
    if (!Memory::WriteVisualState(&state->visual_state)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to write visual state");
        return false;
    }

    // Write round system state
    if (!Memory::WriteRoundState(state->round_timer,
                                state->round_limit,
                                state->round_state,
                                state->game_mode)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to write round state");
        return false;
    }

    // Write RNG state
    if (!Memory::WriteMemoryRegion(Addresses::RANDOM_SEED,
                                  &state->random_seed,
                                  sizeof(state->random_seed))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to write RNG state");
        return false;
    }

    // Verify state was loaded correctly
    uint32_t current_checksum = CalculateStateChecksum();
    uint32_t expected_checksum = Fletcher32(
        reinterpret_cast<const uint16_t*>(state),
        sizeof(GameState) / 2
    );

    if (current_checksum != expected_checksum) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "State load verification failed: expected=0x%08x, got=0x%08x",
                    expected_checksum, current_checksum);
        return false;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "State loaded and verified with checksum: 0x%08x",
                 current_checksum);

    return true;
}

uint32_t CalculateStateChecksum() {
    GameState temp_state;
    if (!SaveState(&temp_state, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to save state for checksum calculation");
        return 0;
    }

    return Fletcher32(
        reinterpret_cast<const uint16_t*>(&temp_state),
        sizeof(GameState) / 2
    );
}

bool VerifyState(uint32_t expected_checksum) {
    uint32_t current_checksum = CalculateStateChecksum();
    if (current_checksum != expected_checksum) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "State verification failed: expected=0x%08x, got=0x%08x",
                    expected_checksum, current_checksum);
        return false;
    }
    return true;
}

namespace Memory {

bool ReadHitJudgeTables(uint8_t* buffer, size_t size) {
    return ReadMemoryRegion(Addresses::HIT_JUDGE_TABLES_START,
                           buffer, size);
}

bool WriteHitJudgeTables(const uint8_t* buffer, size_t size) {
    return WriteMemoryRegion(Addresses::HIT_JUDGE_TABLES_START,
                            buffer, size);
}

bool ReadVisualState(VisualState* state) {
    if (!state || !g_process) return false;

    // Read active effects bitfield
    if (!ReadMemoryRegion(Addresses::EFFECT_ACTIVE_FLAGS, 
                         &state->active_effects, 
                         sizeof(state->active_effects))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                    "Failed to read active effects flags");
        return false;
    }

    // Read effect timers array
    if (!ReadMemoryRegion(Addresses::EFFECT_TIMERS_BASE,
                         state->effect_timers,
                         sizeof(state->effect_timers))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to read effect timers");
        return false;
    }

    // Read effect colors array
    if (!ReadMemoryRegion(Addresses::EFFECT_COLORS_BASE,
                         state->color_values,
                         sizeof(state->color_values))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to read effect colors");
        return false;
    }

    // Read effect target IDs
    if (!ReadMemoryRegion(Addresses::EFFECT_TARGETS_BASE,
                         state->target_ids,
                         sizeof(state->target_ids))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to read effect targets");
        return false;
    }

    return true;
}

bool WriteVisualState(const VisualState* state) {
    if (!state || !g_process) return false;

    // Write active effects bitfield
    if (!WriteMemoryRegion(Addresses::EFFECT_ACTIVE_FLAGS,
                          &state->active_effects,
                          sizeof(state->active_effects))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to write active effects flags");
        return false;
    }

    // Write effect timers array
    if (!WriteMemoryRegion(Addresses::EFFECT_TIMERS_BASE,
                          state->effect_timers,
                          sizeof(state->effect_timers))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to write effect timers");
        return false;
    }

    // Write effect colors array
    if (!WriteMemoryRegion(Addresses::EFFECT_COLORS_BASE,
                          state->color_values,
                          sizeof(state->color_values))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to write effect colors");
        return false;
    }

    // Write effect target IDs
    if (!WriteMemoryRegion(Addresses::EFFECT_TARGETS_BASE,
                          state->target_ids,
                          sizeof(state->target_ids))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to write effect targets");
        return false;
    }

    return true;
}

bool ReadRoundState(uint32_t* timer, uint32_t* limit,
                   uint32_t* state, uint32_t* mode) {
    return ReadMemoryRegion(Addresses::ROUND_TIMER, timer, sizeof(*timer)) &&
           ReadMemoryRegion(Addresses::ROUND_LIMIT, limit, sizeof(*limit)) &&
           ReadMemoryRegion(Addresses::ROUND_STATE, state, sizeof(*state)) &&
           ReadMemoryRegion(Addresses::GAME_MODE, mode, sizeof(*mode));
}

bool WriteRoundState(uint32_t timer, uint32_t limit,
                    uint32_t state, uint32_t mode) {
    return WriteMemoryRegion(Addresses::ROUND_TIMER, &timer, sizeof(timer)) &&
           WriteMemoryRegion(Addresses::ROUND_LIMIT, &limit, sizeof(limit)) &&
           WriteMemoryRegion(Addresses::ROUND_STATE, &state, sizeof(state)) &&
           WriteMemoryRegion(Addresses::GAME_MODE, &mode, sizeof(mode));
}

bool ReadMemoryRegion(uintptr_t address, void* buffer, size_t size) {
    if (!g_process || !buffer || !size) return false;

    SIZE_T bytes_read;
    if (!ReadProcessMemory(g_process, (LPCVOID)address, buffer, size, &bytes_read) ||
        bytes_read != size) {
        return false;
    }
    return true;
}

bool WriteMemoryRegion(uintptr_t address, const void* buffer, size_t size) {
    if (!g_process || !buffer || !size) return false;

    SIZE_T bytes_written;
    if (!WriteProcessMemory(g_process, (LPVOID)address, buffer, size, &bytes_written) ||
        bytes_written != size) {
        return false;
    }
    return true;
}

} // namespace Memory
} // namespace State
} // namespace FM2K 