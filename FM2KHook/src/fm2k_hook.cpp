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

// Global frame counter
static SDL_AtomicInt g_frame_counter;
static bool g_frame_counter_initialized = false;

// Hook implementations
static int __cdecl Hook_ProcessGameInputs() {
    // Initialize frame counter if needed
    if (!g_frame_counter_initialized) {
        SDL_SetAtomicInt(&g_frame_counter, 0);
        g_frame_counter_initialized = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Frame counter initialized");
    }
    
    // Increment global frame counter first
    SDL_AddAtomicInt(&g_frame_counter, 1);
    uint32_t current_frame = SDL_GetAtomicInt(&g_frame_counter);
    
    // Log ALL calls for now to track execution
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "[Hook] process_game_inputs ENTRY - frame %u", current_frame);

    // Call original function to ensure game state is updated
    SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, 
        "[Hook] Calling original process_game_inputs at %p", original_process_inputs);
    
    if (original_process_inputs) {
        original_process_inputs();
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] Original process_game_inputs returned successfully");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] original_process_inputs is NULL!");
    }

    // Notify launcher that frame has advanced using SDL3 events
    SDL_Event sdl_event;
    SDL_zero(sdl_event);
    sdl_event.type = SDL_EVENT_USER;
    sdl_event.user.code = 1; // FRAME_ADVANCED
    sdl_event.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(current_frame));
    sdl_event.user.data2 = reinterpret_cast<void*>(static_cast<uintptr_t>(SDL_GetTicks()));
    
    if (!SDL_PushEvent(&sdl_event)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to push frame advanced event for frame %u: %s", 
            current_frame, SDL_GetError());
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "[Hook] process_game_inputs EXIT - frame %u", current_frame);
    
    // Return the same value as original function
    return 0; // Original returns result * 4, but we'll return 0 for safety
}

static int __cdecl Hook_UpdateGameState() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[Hook] update_game_state ENTRY");
    
    // Call original first
    SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, 
        "[Hook] Calling original update_game_state at %p", original_update_game);
        
    if (original_update_game) {
        original_update_game();
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] Original update_game_state returned successfully");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] original_update_game is NULL!");
    }

    // Save state if needed
    if (ShouldSaveState()) {
        // TODO: Implement actual state saving when state manager is ready
        uint32_t current_frame = GetFrameNumber();
        uint32_t checksum = 0x12345678; // Placeholder checksum
        
        // Notify launcher of state save using SDL3 events
        SDL_Event sdl_event;
        SDL_zero(sdl_event);
        sdl_event.type = SDL_EVENT_USER;
        sdl_event.user.code = 2; // STATE_SAVED
        sdl_event.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(current_frame));
        sdl_event.user.data2 = reinterpret_cast<void*>(static_cast<uintptr_t>(checksum));
        
        if (!SDL_PushEvent(&sdl_event)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Failed to push state saved event: %s", SDL_GetError());
        }
    }

    // Check for visual state changes
    if (VisualStateChanged()) {
        uint32_t current_frame = GetFrameNumber();
        
        // Notify launcher of visual state change using SDL3 events
        SDL_Event sdl_event;
        SDL_zero(sdl_event);
        sdl_event.type = SDL_EVENT_USER;
        sdl_event.user.code = 3; // VISUAL_STATE_CHANGED
        sdl_event.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(current_frame));
        sdl_event.user.data2 = nullptr;
        
        if (!SDL_PushEvent(&sdl_event)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Failed to push visual state changed event: %s", SDL_GetError());
        }
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[Hook] update_game_state EXIT");
    
    // Return appropriate value (original returns an int)
    return 0;
}

static int __cdecl Hook_GameRand() {
    SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "[Hook] game_rand ENTRY");
    
    // Call original RNG
    int result = 0;
    if (original_rng) {
        result = original_rng();
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] Original game_rand returned: %d", result);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] original_rng is NULL!");
    }

    // Log RNG calls for debugging
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "RNG called at frame %u, result: %d",
        GetFrameNumber(), result);

    SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "[Hook] game_rand EXIT");
    return result;
}

bool Init(HANDLE process) {
    if (!process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle");
        return false;
    }

    // Set SDL logging to maximum verbosity to catch everything
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL logging set to verbose mode");

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initializing FM2K hooks...");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Process handle: %p", process);

    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize MinHook");
        return false;
    }

    // Initialize IPC system
    if (!IPC::Init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize IPC");
        MH_Uninitialize();
        return false;
    }

    // Initialize state manager
    if (!State::Init(process)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize state manager");
        IPC::Shutdown();
        MH_Uninitialize();
        return false;
    }

    // Create hooks using correct addresses from research
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating hook for process_game_inputs at 0x4146D0");
    MH_STATUS status1 = MH_CreateHook(reinterpret_cast<LPVOID>(0x4146D0),  // process_game_inputs
                                      reinterpret_cast<LPVOID>(Hook_ProcessGameInputs),
                                      reinterpret_cast<LPVOID*>(&original_process_inputs));
    if (status1 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create process_game_inputs hook: %d", status1);
        MH_Uninitialize();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating hook for update_game_state at 0x404CD0");
    MH_STATUS status2 = MH_CreateHook(reinterpret_cast<LPVOID>(0x404CD0),  // update_game_state
                                      reinterpret_cast<LPVOID>(Hook_UpdateGameState),
                                      reinterpret_cast<LPVOID*>(&original_update_game));
    if (status2 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create update_game_state hook: %d", status2);
        MH_Uninitialize();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating hook for game_rand at 0x417A22");
    MH_STATUS status3 = MH_CreateHook(reinterpret_cast<LPVOID>(0x417A22),  // game_rand
                                      reinterpret_cast<LPVOID>(Hook_GameRand),
                                      reinterpret_cast<LPVOID*>(&original_rng));
    if (status3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create game_rand hook: %d", status3);
        MH_Uninitialize();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "All hooks created successfully");

    // Enable hooks
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable hooks");
        State::Shutdown();
        IPC::Shutdown();
        MH_Uninitialize();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K hooks installed successfully");
    
    // Send initialization confirmation event to launcher
    SDL_Event sdl_event;
    SDL_zero(sdl_event);
    sdl_event.type = SDL_EVENT_USER;
    sdl_event.user.code = 0; // HOOKS_INITIALIZED
    sdl_event.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(1)); // Success
    sdl_event.user.data2 = nullptr;
    
    if (!SDL_PushEvent(&sdl_event)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to push hooks initialized event: %s", SDL_GetError());
    }
    
    return true;
}

void Shutdown() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Shutting down FM2K hooks...");
    
    // Disable and remove hooks
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    
    // Shutdown subsystems
    State::Shutdown();
    IPC::Shutdown();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K hooks shutdown complete");
}

uint32_t GetFrameNumber() {
    if (!g_frame_counter_initialized) {
        return 0;
    }
    return SDL_GetAtomicInt(&g_frame_counter);
}

bool ShouldSaveState() {
    // Only save state every 10 frames for now to avoid buffer overflow
    // Later, optimize based on:
    // - Input changes
    // - Critical game state changes
    // - Network prediction window
    uint32_t frame = GetFrameNumber();
    return (frame % 10) == 0;
}

bool VisualStateChanged() {
    static uint32_t last_effect_flags = 0;
    uint32_t current_effect_flags;

    // Read effect flags from 0x40CC30 (from research doc)
    if (ReadProcessMemory(GetCurrentProcess(), 
                         (LPCVOID)0x40CC30,
                         &current_effect_flags,
                         sizeof(current_effect_flags),
                         nullptr)) {
        if (current_effect_flags != last_effect_flags) {
            last_effect_flags = current_effect_flags;
            return true;
        }
    }
    return false;
}

} // namespace Hooks
} // namespace FM2K 