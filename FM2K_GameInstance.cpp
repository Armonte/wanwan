#include "FM2K_Integration.h"
#include <iostream>
#include <thread>
#include <filesystem>

// FM2KGameInstance Implementation
FM2KGameInstance::FM2KGameInstance() 
    : process_handle_(nullptr)
    , process_id_(0)
    , process_info_{}
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
    
    // TODO: Implement MinHook installation
    // Key hook points from research:
    // - 0x4146D0: process_game_inputs (PRIMARY HOOK)
    // - 0x404CD0: update_game_state
    // - 0x417A22: random number generation
    
    std::cout << "? Hooks installed (placeholder)" << std::endl;
    return true;
}

void FM2KGameInstance::RemoveHooks() {
    // TODO: Remove all installed hooks
    for (auto hook : installed_hooks_) {
        // MH_RemoveHook(hook);
    }
    installed_hooks_.clear();
    
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