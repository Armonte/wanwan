# FM2K Hook Implementation Guide

## Critical TODOs for Game Stability

### 1. Frame Counter Implementation (Highest Priority)
```cpp
// In fm2k_hook.cpp
uint32_t GetFrameNumber() {
    static SDL_atomic_t frame_counter = {0};
    return SDL_AtomicGet(&frame_counter);
}
```

### 2. Hook Installation (Critical)
```cpp
// In FM2K_GameInstance.cpp
bool FM2KGameInstance::InstallHooks() {
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize MinHook!!!!!!!!!!!!!!!!!");
        return false;
    }

    // Create hooks at known addresses from research
    if (MH_CreateHook(reinterpret_cast<LPVOID>(0x4146D0),  // process_game_inputs
                      reinterpret_cast<LPVOID>(Hook_ProcessGameInputs),
                      reinterpret_cast<LPVOID*>(&original_process_inputs)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<LPVOID>(0x404CD0),  // update_game_state
                      reinterpret_cast<LPVOID>(Hook_UpdateGameState),
                      reinterpret_cast<LPVOID*>(&original_update_game)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<LPVOID>(0x417A22),  // game_rand
                      reinterpret_cast<LPVOID>(Hook_GameRand),
                      reinterpret_cast<LPVOID*>(&original_rand_func)) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create hooks!!!!!!!!!!!!!!!!!!!!!!");
        MH_Uninitialize();
        return false;
    }

    // Enable all hooks
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable hooks!!!!!!!!!!!!!!!!!!!");
        MH_Uninitialize();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Successfully installed hooks!!!!!!!!!!!!!!!!!!!");
    return true;
}
```

### 3. Hook Removal (Critical)
```cpp
// In FM2K_GameInstance.cpp
bool FM2KGameInstance::UninstallHooks() {
    // Disable all hooks
    if (MH_DisableHook(MH_ALL_HOOKS) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to disable hooks");
        return false;
    }

    // Uninitialize MinHook
    if (MH_Uninitialize() != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to uninitialize MinHook");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Successfully uninstalled hooks");
    return true;
}
```

### 4. Frame Advance Implementation (Critical)
```cpp
// In FM2K_GameInstance.cpp
bool FM2KGameInstance::AdvanceFrame() {
    if (!process_handle_) return false;

    // Increment frame counter
    SDL_AtomicIncRef(&frame_counter_);
    
    // Call process_game_inputs at 0x4146D0
    if (!ExecuteRemoteFunction(process_handle_, 0x4146D0)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to execute process_game_inputs");
        return false;
    }

    // Call update_game_state at 0x404CD0
    if (!ExecuteRemoteFunction(process_handle_, 0x404CD0)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to execute update_game_state");
        return false;
    }

    return true;
}
```

### 5. State Save Condition (Important)
```cpp
// In fm2k_hook.cpp
bool ShouldSaveState() {
    // For now, save state every frame for testing
    // Later, optimize based on:
    // - Input changes
    // - Critical game state changes
    // - Network prediction window
    return true;
}
```

### 6. Visual State Change Detection (Important)
```cpp
// In fm2k_hook.cpp
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
```

### 7. IPC Event Processing (Important)
```cpp
// In FM2K_GameInstance.cpp
void FM2KGameInstance::ProcessIPCEvents() {
    FM2K::IPC::Event event;
    while (FM2K::IPC::PollEvent(&event)) {
        switch (event.type) {
            case FM2K::IPC::EventType::FRAME_ADVANCED:
                OnFrameAdvanced(event);
                break;
            case FM2K::IPC::EventType::STATE_SAVED:
                OnStateSaved(event);
                break;
            case FM2K::IPC::EventType::STATE_LOADED:
                OnStateLoaded(event);
                break;
            case FM2K::IPC::EventType::VISUAL_STATE_CHANGED:
                OnVisualStateChanged(event);
                break;
            case FM2K::IPC::EventType::ERROR:
                OnHookError(event);
                break;
        }
    }
}
```

## Implementation Order

1. First Pass (Game Stability):
   - InstallHooks()
   - UninstallHooks()
   - GetFrameNumber()
   - AdvanceFrame()

2. Second Pass (State Management):
   - ShouldSaveState()
   - ProcessIPCEvents()
   - Visual state tracking

## Critical Memory Addresses (from FM2K_Rollback_Research.md)

```cpp
// Core Function Hooks
#define PROCESS_INPUTS_ADDR    0x4146D0
#define UPDATE_GAME_STATE_ADDR 0x404CD0
#define GAME_RAND_ADDR        0x417A22

// Visual Effect System
#define EFFECT_FLAGS_ADDR     0x40CC30
#define EFFECT_TIMERS_ADDR    0x40CC34
#define EFFECT_COLORS_ADDR    0x40CC54
#define EFFECT_TARGETS_ADDR   0x40CCD4

// Input System
#define P1_INPUT_ADDR         0x4259C0
#define P2_INPUT_ADDR         0x4259C4
#define INPUT_HISTORY_P1_ADDR 0x4280E0
#define INPUT_HISTORY_P2_ADDR 0x4290E0
#define INPUT_BUFFER_IDX_ADDR 0x447EE0
```

## Next Steps

1. Implement the hooks in order of priority
2. Test each hook with logging before adding full functionality
3. Verify game stays running after hook installation
4. Add state saving once basic hooks are stable
5. Implement IPC events after core stability is achieved

Remember: The goal is to keep the game running first, then add functionality incrementally. 