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

    return true;
}

uint32_t CalculateStateChecksum() {
    // TODO: Implement Fletcher32 or similar checksum
    return 0;
}

bool VerifyState(uint32_t expected_checksum) {
    return CalculateStateChecksum() == expected_checksum;
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
    // TODO: Read from sprite effect system memory
    return true;
}

bool WriteVisualState(const VisualState* state) {
    // TODO: Write to sprite effect system memory
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
    SIZE_T bytes_read;
    return ReadProcessMemory(g_process,
                            reinterpret_cast<LPCVOID>(address),
                            buffer, size, &bytes_read) &&
           bytes_read == size;
}

bool WriteMemoryRegion(uintptr_t address, const void* buffer, size_t size) {
    SIZE_T bytes_written;
    return WriteProcessMemory(g_process,
                             reinterpret_cast<LPVOID>(address),
                             buffer, size, &bytes_written) &&
           bytes_written == size;
}

} // namespace Memory
} // namespace State
} // namespace FM2K 