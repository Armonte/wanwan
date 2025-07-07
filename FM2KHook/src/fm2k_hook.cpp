#include "fm2k_hook.h"
#include "ipc.h"
#include "state_manager.h"
#include <SDL3/SDL.h>
#include <MinHook.h>

namespace FM2K {
namespace Hooks {

// Original function pointers
static ProcessGameInputsFn original_process_inputs = nullptr;
static UpdateGameStateFn original_update_game = nullptr;
static RNGFn original_rng = nullptr;

// Hook implementations
void __stdcall Hook_ProcessGameInputs() {
    // Call original first to ensure game state is updated
    if (original_process_inputs) {
        original_process_inputs();
    }

    // Notify launcher that frame has advanced
    IPC::Event event;
    event.type = IPC::EventType::FRAME_ADVANCED;
    event.frame_number = GetFrameNumber();
    event.timestamp_ms = SDL_GetTicks();
    
    if (!IPC::PostEvent(event)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to post frame advanced event");
    }
}

void __stdcall Hook_UpdateGameState() {
    // Call original first
    if (original_update_game) {
        original_update_game();
    }

    // Save state and calculate checksum
    State::GameState state;
    uint32_t checksum;
    if (State::SaveState(&state, &checksum)) {
        // Notify launcher of state change
        IPC::Event event;
        event.type = IPC::EventType::STATE_SAVED;
        event.frame_number = GetFrameNumber();
        event.timestamp_ms = SDL_GetTicks();
        event.data.state.checksum = checksum;
        event.data.state.frame_number = event.frame_number;

        if (!IPC::PostEvent(event)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Failed to post state saved event");
        }
    }
}

int __stdcall Hook_RNG() {
    // Call original RNG
    int result = 0;
    if (original_rng) {
        result = original_rng();
    }

    // Notify launcher of RNG call
    IPC::Event event;
    event.type = IPC::EventType::FRAME_ADVANCED;
    event.frame_number = GetFrameNumber();
    event.timestamp_ms = SDL_GetTicks();

    if (!IPC::PostEvent(event)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to post RNG event");
    }

    return result;
}

bool Init(HANDLE process) {
    if (!process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle");
        return false;
    }

    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize MinHook");
        return false;
    }

    // Create hooks
    if (MH_CreateHook(reinterpret_cast<LPVOID>(0x401000),
                      reinterpret_cast<LPVOID>(Hook_ProcessGameInputs),
                      reinterpret_cast<LPVOID*>(&original_process_inputs)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<LPVOID>(0x401100),
                      reinterpret_cast<LPVOID>(Hook_UpdateGameState),
                      reinterpret_cast<LPVOID*>(&original_update_game)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<LPVOID>(0x401200),
                      reinterpret_cast<LPVOID>(Hook_RNG),
                      reinterpret_cast<LPVOID*>(&original_rng)) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create hooks");
        MH_Uninitialize();
        return false;
    }

    // Enable hooks
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable hooks");
        MH_Uninitialize();
        return false;
    }

    return true;
}

void Shutdown() {
    // Disable and remove hooks
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

uint32_t GetFrameNumber() {
    // TODO: Implement frame counter
    return 0;
}

bool ShouldSaveState() {
    // TODO: Implement state save condition
    return false;
}

bool VisualStateChanged() {
    // TODO: Implement visual state change detection
    return false;
}

} // namespace Hooks
} // namespace FM2K 