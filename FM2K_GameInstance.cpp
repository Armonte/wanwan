#include "FM2K_Integration.h"
#include <iostream>
#include <thread>
#include <filesystem>
#include "FM2K_Hooks.h"
#include "FM2K_DLLInjector.h"
#include "SDL3/SDL.h"

// Fletcher32 checksum implementation
uint32_t Fletcher32(const uint16_t* data, size_t len) {
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    size_t words = len / 2;

    while (words) {
        size_t tlen = words > 359 ? 359 : words;
        words -= tlen;
        do {
            sum1 += *data++;
            sum2 += sum1;
        } while (--tlen);
        
        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }

    // Handle remaining byte if length is odd
    if (len & 1) {
        sum1 += *reinterpret_cast<const uint8_t*>(data);
        sum2 += sum1;
        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }

    sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
    sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    return (sum2 << 16) | sum1;
}

// FM2KGameInstance Implementation
FM2KGameInstance::FM2KGameInstance() 
    : process_handle_(nullptr)
    , process_id_(0)
    , process_info_{}
    , event_buffer_(nullptr)
{
}

FM2KGameInstance::~FM2KGameInstance() {
    Terminate();
}

bool FM2KGameInstance::Launch(const FM2KGameInfo& game) {
    if (IsRunning()) {
        std::cerr << "Game is already running\n";
        return false;
    }
    
    std::cout << "Launching FM2K game: " << game.name << std::endl;
    std::cout << "Executable: " << game.exe_path << std::endl;
    
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    
    // Launch game with debug privileges for memory access
    BOOL result = CreateProcessA(
        game.exe_path.c_str(),     // lpApplicationName
        nullptr,                   // lpCommandLine  
        nullptr,                   // lpProcessAttributes
        nullptr,                   // lpThreadAttributes
        FALSE,                     // bInheritHandles
        CREATE_SUSPENDED,          // dwCreationFlags - Start suspended for hook setup
        nullptr,                   // lpEnvironment
        nullptr,                   // lpCurrentDirectory
        &si,                       // lpStartupInfo
        &process_info_             // lpProcessInformation
    );
    
    if (!result) {
        DWORD error = GetLastError();
        std::cerr << "Failed to launch FM2K game. Error: " << error << std::endl;
        return false;
    }
    
    process_handle_ = process_info_.hProcess;
    process_id_ = process_info_.dwProcessId;
    
    std::cout << "? FM2K game launched (PID: " << process_id_ << ")" << std::endl;
    std::cout << "? Process started in suspended state for hook setup" << std::endl;
    
    // Setup hooks before resuming
    if (!SetupProcessForHooking()) {
        std::cerr << "Failed to setup process for hooking" << std::endl;
        Terminate();
        return false;
    }
    
    // Resume the process
    if (ResumeThread(process_info_.hThread) == (DWORD)-1) {
        std::cerr << "Failed to resume FM2K process" << std::endl;
        Terminate();
        return false;
    }
    
    std::cout << "? FM2K game resumed and running" << std::endl;
    
    // Wait a moment for the game to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    return true;
}

void FM2KGameInstance::Terminate() {
    if (!IsRunning()) {
        return;
    }
    
    std::cout << "Terminating FM2K game process (PID: " << process_id_ << ")" << std::endl;
    
    // Remove hooks first
    RemoveHooks();
    
    // Terminate the process
    if (process_handle_) {
        TerminateProcess(process_handle_, 0);
        
        // Wait for process to exit
        WaitForSingleObject(process_handle_, 5000);  // 5 second timeout
        
        CloseHandle(process_info_.hThread);
        CloseHandle(process_handle_);
        
        process_handle_ = nullptr;
        process_id_ = 0;
        memset(&process_info_, 0, sizeof(process_info_));
    }
    
    std::cout << "? FM2K game terminated" << std::endl;
}

bool FM2KGameInstance::SetupProcessForHooking() {
    if (!process_handle_) {
        return false;
    }
    
    // TODO: Install memory hooks for rollback integration
    // This is where we'd use MinHook to install our rollback hooks
    // For now, just return success
    
    std::cout << "? Process setup for hooking complete" << std::endl;
    return true;
}

bool FM2KGameInstance::InstallHooks() {
    if (!IsRunning()) {
        std::cerr << "Cannot install hooks - game not running" << std::endl;
        return false;
    }

    if (!FM2K::Hooks::Init(process_handle_)) {
        std::cerr << "Failed to install FM2K hooks" << std::endl;
        return false;
    }

    std::cout << "? Hooks installed" << std::endl;
    return true;
}

void FM2KGameInstance::RemoveHooks() {
    FM2K::Hooks::Shutdown();
    std::cout << "? Hooks removed" << std::endl;
}

bool FM2KGameInstance::SaveState(void* buffer, size_t buffer_size) {
    if (!IsRunning() || !buffer) {
        return false;
    }
    
    if (buffer_size < sizeof(FM2K::GameState)) {
        std::cerr << "Buffer too small for FM2K state" << std::endl;
        return false;
    }
    
    FM2K::GameState* state = static_cast<FM2K::GameState*>(buffer);
    
    // Read frame and timing state
    ReadMemory(FM2K::INPUT_BUFFER_INDEX_ADDR, &state->input_buffer_index);
    ReadMemory(FM2K::RANDOM_SEED_ADDR, &state->random_seed);
    
    // Read player states
    ReadMemory(FM2K::P1_INPUT_ADDR, &state->players[0].input_current);
    ReadMemory(FM2K::P1_STAGE_X_ADDR, &state->players[0].stage_x);
    ReadMemory(FM2K::P1_STAGE_Y_ADDR, &state->players[0].stage_y);
    ReadMemory(FM2K::P1_HP_ADDR, &state->players[0].hp);
    ReadMemory(FM2K::P1_MAX_HP_ADDR, &state->players[0].max_hp);
    
    ReadMemory(FM2K::P2_INPUT_ADDR, &state->players[1].input_current);
    ReadMemory(FM2K::P2_HP_ADDR, &state->players[1].hp);
    ReadMemory(FM2K::P2_MAX_HP_ADDR, &state->players[1].max_hp);
    
    // Read input histories
    SIZE_T bytes_read;
    ReadProcessMemory(process_handle_, (LPVOID)FM2K::P1_INPUT_HISTORY_ADDR, 
                     &state->players[0].input_history, sizeof(state->players[0].input_history), &bytes_read);
    ReadProcessMemory(process_handle_, (LPVOID)FM2K::P2_INPUT_HISTORY_ADDR, 
                     &state->players[1].input_history, sizeof(state->players[1].input_history), &bytes_read);
    
    // Read global timers
    ReadMemory(FM2K::ROUND_TIMER_ADDR, &state->round_timer);
    ReadMemory(FM2K::GAME_TIMER_ADDR, &state->game_timer);
    
    // TODO: Read critical object pool state for complete accuracy
    // This would involve reading the full 1024-object pool (~390KB)
    
    return true;
}

bool FM2KGameInstance::LoadState(const void* buffer, size_t buffer_size) {
    if (!IsRunning() || !buffer) {
        return false;
    }
    
    if (buffer_size < sizeof(FM2K::GameState)) {
        std::cerr << "Invalid buffer size for FM2K state" << std::endl;
        return false;
    }
    
    const FM2K::GameState* state = static_cast<const FM2K::GameState*>(buffer);
    
    // Write frame and timing state
    WriteMemory(FM2K::INPUT_BUFFER_INDEX_ADDR, &state->input_buffer_index);
    WriteMemory(FM2K::RANDOM_SEED_ADDR, &state->random_seed);
    
    // Write player states
    WriteMemory(FM2K::P1_INPUT_ADDR, &state->players[0].input_current);
    WriteMemory(FM2K::P1_STAGE_X_ADDR, &state->players[0].stage_x);
    WriteMemory(FM2K::P1_STAGE_Y_ADDR, &state->players[0].stage_y);
    WriteMemory(FM2K::P1_HP_ADDR, &state->players[0].hp);
    WriteMemory(FM2K::P1_MAX_HP_ADDR, &state->players[0].max_hp);
    
    WriteMemory(FM2K::P2_INPUT_ADDR, &state->players[1].input_current);
    WriteMemory(FM2K::P2_HP_ADDR, &state->players[1].hp);
    WriteMemory(FM2K::P2_MAX_HP_ADDR, &state->players[1].max_hp);
    
    // Write input histories
    SIZE_T bytes_written;
    WriteProcessMemory(process_handle_, (LPVOID)FM2K::P1_INPUT_HISTORY_ADDR, 
                      &state->players[0].input_history, sizeof(state->players[0].input_history), &bytes_written);
    WriteProcessMemory(process_handle_, (LPVOID)FM2K::P2_INPUT_HISTORY_ADDR, 
                      &state->players[1].input_history, sizeof(state->players[1].input_history), &bytes_written);
    
    // Write global timers
    WriteMemory(FM2K::ROUND_TIMER_ADDR, &state->round_timer);
    WriteMemory(FM2K::GAME_TIMER_ADDR, &state->game_timer);
    
    // TODO: Restore complete object pool state
    
    return true;
}

void FM2KGameInstance::InjectInputs(uint32_t p1_input, uint32_t p2_input) {
    if (!IsRunning()) {
        return;
    }
    
    // Inject inputs directly into FM2K's input system
    WriteMemory(FM2K::P1_INPUT_ADDR, &p1_input);
    WriteMemory(FM2K::P2_INPUT_ADDR, &p2_input);
}

DWORD WINAPI FM2KGameInstance::ProcessMonitorThread(LPVOID param) {
    FM2KGameInstance* instance = static_cast<FM2KGameInstance*>(param);
    
    if (!instance || !instance->process_handle_) {
        return 1;
    }
    
    // Monitor the process and clean up when it exits
    WaitForSingleObject(instance->process_handle_, INFINITE);
    
    std::cout << "FM2K process has exited" << std::endl;
    
    // Clean up handles
    CloseHandle(instance->process_info_.hThread);
    CloseHandle(instance->process_handle_);
    instance->process_handle_ = nullptr;
    instance->process_id_ = 0;
    
    return 0;
}

bool FM2KGameInstance::Initialize() {
    // Get the current executable path
    std::filesystem::path exe_path = std::filesystem::absolute(std::filesystem::path("."));
    
    // Load the game executable
    if (!LoadGameExecutable(exe_path / "WonderfulWorld_ver_0946.exe")) {
        SDL_Log("Failed to load game executable");
        return false;
    }
    
    // Initialize game state
    game_state_ = std::make_unique<FM2K::GameState>();
    if (!game_state_) {
        SDL_Log("Failed to allocate game state");
        return false;
    }
    
    return true;
}

bool FM2KGameInstance::LoadGameExecutable(const std::filesystem::path& exe_path) {
    if (!std::filesystem::exists(exe_path)) {
        std::cerr << "Game executable not found: " << exe_path << std::endl;
        return false;
    }
    
    // Create process suspended for hook setup
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    
    BOOL result = CreateProcessA(
        exe_path.string().c_str(),  // lpApplicationName
        nullptr,                    // lpCommandLine  
        nullptr,                    // lpProcessAttributes
        nullptr,                    // lpThreadAttributes
        FALSE,                      // bInheritHandles
        CREATE_SUSPENDED,           // dwCreationFlags
        nullptr,                    // lpEnvironment
        nullptr,                    // lpCurrentDirectory
        &si,                        // lpStartupInfo
        &process_info_              // lpProcessInformation
    );
    
    if (!result) {
        DWORD error = GetLastError();
        std::cerr << "Failed to create game process. Error: " << error << std::endl;
        return false;
    }
    
    process_handle_ = process_info_.hProcess;
    process_id_ = process_info_.dwProcessId;
    
    std::cout << "? Game process created (PID: " << process_id_ << ")" << std::endl;
    return true;
}

void FM2KGameInstance::ProcessIPCEvents() {
    if (!event_buffer_) return;

    // Process all available events
    while (true) {
        FM2K::IPC::Event event;
        if (!FM2K::IPC::ReadEvent(event_buffer_, &event)) {
            break;  // No more events
        }

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
            case FM2K::IPC::EventType::HIT_TABLES_INIT:
                OnHitTablesInit(event);
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

void FM2KGameInstance::OnFrameAdvanced(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "Frame advanced: player=%u frame=%u time=%u",
                 event.player_index, event.frame_number, event.timestamp_ms);
    
    // TODO: Notify GekkoNet that frame is ready
}

void FM2KGameInstance::OnStateSaved(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "State saved: frame=%u", event.frame_number);
    
    // TODO: Copy state from shared memory to GekkoNet buffer
}

void FM2KGameInstance::OnStateLoaded(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "State loaded: frame=%u", event.frame_number);
    
    // TODO: Verify state loaded correctly
}

void FM2KGameInstance::OnHitTablesInit(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "Hit judge tables initialized: size=%u checksum=0x%08x",
                 event.data.hit_tables.table_size,
                 event.data.hit_tables.checksum);
    
    // TODO: Verify tables are valid via checksum
}

void FM2KGameInstance::OnVisualStateChanged(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "Visual state changed: effect=%u duration=%u",
                 event.data.visual.effect_id,
                 event.data.visual.duration);
    
    // TODO: Track visual state for rollback verification
}

void FM2KGameInstance::OnHookError(const FM2K::IPC::Event& event) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Hook error occurred at frame %u", event.frame_number);
    
    // TODO: Surface error to UI and consider terminating game
}

uint32_t CalculateStateChecksum() {
    GameState current_state;
    if (!SaveState(&current_state, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to save state for checksum calculation");
        return 0;
    }

    // Calculate Fletcher32 checksum over the entire state structure
    return Fletcher32(reinterpret_cast<const uint16_t*>(&current_state), sizeof(current_state) / 2);
}

bool ReadVisualState(VisualState* state) {
    if (!state) return false;

    // Read active effects bitfield
    if (!ReadMemory(EFFECT_ACTIVE_FLAGS, &state->active_effects)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read active effects flags");
        return false;
    }

    // Read effect timers array
    if (!BulkCopyOut(process_handle_, 
                     state->effect_timers,
                     EFFECT_TIMERS_BASE, 
                     sizeof(state->effect_timers))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read effect timers");
        return false;
    }

    // Read effect colors array
    if (!BulkCopyOut(process_handle_,
                     state->color_values,
                     EFFECT_COLORS_BASE,
                     sizeof(state->color_values))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read effect colors");
        return false;
    }

    // Read effect target IDs array
    if (!BulkCopyOut(process_handle_,
                     state->target_ids,
                     EFFECT_TARGETS_BASE,
                     sizeof(state->target_ids))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read effect targets");
        return false;
    }

    return true;
}

bool WriteVisualState(const VisualState* state) {
    if (!state) return false;

    // Write active effects bitfield
    if (!WriteMemory(EFFECT_ACTIVE_FLAGS, state->active_effects)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write active effects flags");
        return false;
    }

    // Write effect timers array
    if (!BulkCopyIn(process_handle_,
                    EFFECT_TIMERS_BASE,
                    state->effect_timers,
                    sizeof(state->effect_timers))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write effect timers");
        return false;
    }

    // Write effect colors array
    if (!BulkCopyIn(process_handle_,
                    EFFECT_COLORS_BASE,
                    state->color_values,
                    sizeof(state->color_values))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write effect colors");
        return false;
    }

    // Write effect target IDs array
    if (!BulkCopyIn(process_handle_,
                    EFFECT_TARGETS_BASE,
                    state->target_ids,
                    sizeof(state->target_ids))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write effect targets");
        return false;
    }

    return true;
} 