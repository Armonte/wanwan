# FM2K Rollback Implementation Plan

## 1. Frame Counter Implementation (fm2k_hook.cpp)

```cpp
uint32_t GetFrameNumber() {
    // Based on research doc, we can read this from memory
    static uint32_t* frame_counter = (uint32_t*)0x447EE0; // g_input_buffer_index
    return *frame_counter;
}
```

## 2. State Management (fm2k_hook.cpp)

```cpp
bool ShouldSaveState() {
    // Save state on these conditions:
    // 1. After processing inputs
    // 2. Before any game state modification
    // 3. When visual effects change
    static uint32_t last_frame = 0;
    uint32_t current_frame = GetFrameNumber();
    
    if (current_frame != last_frame) {
        last_frame = current_frame;
        return true;
    }
    return false;
}

bool VisualStateChanged() {
    // From july7-2025.md research:
    static uint32_t last_effect_state = 0;
    uint32_t* current_effects = (uint32_t*)0x40CC30; // EFFECT_ACTIVE_FLAGS
    
    if (last_effect_state != *current_effects) {
        last_effect_state = *current_effects;
        return true;
    }
    return false;
}
```

## 3. Hook Installation (FM2K_GameInstance.cpp)

```cpp
bool FM2KGameInstance::InstallHooks() {
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize MinHook");
        return false;
    }

    // Create hooks at known addresses from research
    if (MH_CreateHook(reinterpret_cast<LPVOID>(0x41474A), // VS_P1_KEY
                      reinterpret_cast<LPVOID>(Hook_ProcessGameInputs),
                      reinterpret_cast<LPVOID*>(&original_process_inputs)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<LPVOID>(0x404CD0), // update_game_state
                      reinterpret_cast<LPVOID>(Hook_UpdateGameState),
                      reinterpret_cast<LPVOID*>(&original_update_game)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<LPVOID>(0x417A22), // RAND_FUNC
                      reinterpret_cast<LPVOID>(Hook_GameRand),
                      reinterpret_cast<LPVOID*>(&original_rand_func)) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create hooks");
        MH_Uninitialize();
        return false;
    }

    // Enable all hooks
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable hooks");
        MH_Uninitialize();
        return false;
    }

    return true;
}

bool FM2KGameInstance::UninstallHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    return true;
}
```

## 4. Frame Advance Implementation (FM2K_GameInstance.cpp)

```cpp
bool FM2KGameInstance::AdvanceFrame() {
    // From research doc: FM2K runs at fixed 100 FPS (10ms per frame)
    // We need to:
    // 1. Process inputs
    // 2. Update game state
    // 3. Handle visual effects
    
    if (!process_handle_) return false;

    // Trigger input processing
    if (original_process_inputs) {
        original_process_inputs();
    }

    // Update game state
    if (original_update_game) {
        original_update_game();
    }

    // Save state if needed
    if (ShouldSaveState()) {
        SaveState(game_state_.get(), sizeof(FM2K::GameState));
    }

    return true;
}
```

## 5. IPC Event Processing (FM2K_GameInstance.cpp)

```cpp
void FM2KGameInstance::ProcessIPCEvents() {
    // Process all pending IPC events
    IPC::Event event;
    while (IPC::PollEvent(&event)) {
        switch (event.type) {
            case IPC::EventType::FRAME_ADVANCED:
                OnFrameAdvanced(event);
                break;
            case IPC::EventType::STATE_SAVED:
                OnStateSaved(event);
                break;
            case IPC::EventType::STATE_LOADED:
                OnStateLoaded(event);
                break;
            case IPC::EventType::VISUAL_STATE_CHANGED:
                OnVisualStateChanged(event);
                break;
            case IPC::EventType::ERROR:
                OnHookError(event);
                break;
        }
    }
}
```

## 6. Game Executable Loading (FM2K_GameInstance.cpp)

```cpp
bool FM2KGameInstance::LoadGameExecutable(const std::filesystem::path& exe_path) {
    // Verify executable exists
    if (!SDL_GetPathInfo(exe_path.string().c_str(), nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Game executable not found: %s", exe_path.string().c_str());
        return false;
    }

    // Create process in suspended state
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::wstring wide_path = exe_path.wstring();
    std::wstring cmd_line = L"\"" + wide_path + L"\"";

    if (!CreateProcessW(
        wide_path.c_str(),
        const_cast<LPWSTR>(cmd_line.c_str()),
        nullptr, nullptr, FALSE,
        CREATE_SUSPENDED,
        nullptr, nullptr,
        &si, &pi)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to create game process: %lu", GetLastError());
        return false;
    }

    process_handle_ = pi.hProcess;
    process_id_ = pi.dwProcessId;
    process_info_ = pi;

    return true;
}
```

## Implementation Order

1. Frame Counter (GetFrameNumber) - Essential for all other functionality
2. Hook Installation/Uninstallation - Required for intercepting game functions
3. State Management (ShouldSaveState, VisualStateChanged) - Core rollback functionality
4. Frame Advance - Builds on hooks and state management
5. IPC Event Processing - Enables communication between game and launcher
6. Game Executable Loading - Required for initial setup

## Testing Strategy

1. Frame Counter
   - Verify counter increments correctly
   - Check wraparound behavior at 1024 frames

2. Hooks
   - Confirm all hooks are installed
   - Verify original functions are called
   - Test hook removal

3. State Management
   - Test state save triggers
   - Verify visual state detection
   - Validate state consistency

4. Frame Advance
   - Test single frame advancement
   - Verify state saves occur
   - Check input processing

5. IPC Events
   - Test all event types
   - Verify event ordering
   - Check error handling

6. Game Loading
   - Test process creation
   - Verify suspended state
   - Check error conditions 