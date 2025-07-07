#include "FM2K_Integration.h"
#include "FM2K_DLLInjector.h"
#include "FM2KHook/src/ipc.h"
#include "FM2KHook/src/state_manager.h"
#include <SDL3/SDL.h>
#include <filesystem>

namespace {

// Constants
constexpr uint32_t PROCESS_MONITOR_INTERVAL_MS = 100;
constexpr uint32_t DLL_INIT_TIMEOUT_MS = 5000;
constexpr uint32_t IPC_EVENT_TIMEOUT_MS = 100;

// Helper functions
[[maybe_unused]] static std::wstring GetDLLPath() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::filesystem::path exe_path(buffer);
    return exe_path.parent_path() / L"FM2KHook.dll";
}

} // anonymous namespace

FM2KGameInstance::FM2KGameInstance()
    : process_handle_(nullptr)
    , process_id_(0)
    , game_state_(std::make_unique<FM2K::GameState>()) {
    memset(&process_info_, 0, sizeof(process_info_));
}

FM2KGameInstance::~FM2KGameInstance() {
    Terminate();
}

bool FM2KGameInstance::Initialize() {
    // Initialize SDL if not already done
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0) {
        if (SDL_Init(SDL_INIT_EVENTS) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Failed to initialize SDL: %s", SDL_GetError());
            return false;
        }
    }

    // Set up logging
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
    return true;
}

bool FM2KGameInstance::Launch(const FM2K::FM2KGameInfo& game) {
    // Use SDL3's cross-platform filesystem helpers for existence checks.
    if (!SDL_GetPathInfo(game.exe_path.c_str(), nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Game executable not found: %s (%s)",
            game.exe_path.c_str(), SDL_GetError());
        return false;
    }

    if (!SDL_GetPathInfo(game.dll_path.c_str(), nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Hook DLL not found: %s (%s)",
            game.dll_path.c_str(), SDL_GetError());
        return false;
    }

    // Convert path to Windows format for CreateProcess
    std::string windows_path = game.exe_path;
    for (char& c : windows_path) {
        if (c == '/') c = '\\';
    }

    // Create process suspended
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (!CreateProcessW(
        std::filesystem::path(windows_path).wstring().c_str(),
        nullptr, nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NEW_CONSOLE,
        nullptr, nullptr, &si, &process_info_)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to create game process: %lu", GetLastError());
        return false;
    }

    process_handle_ = process_info_.hProcess;
    process_id_ = process_info_.dwProcessId;

    // Set up process for hooking
    if (!SetupProcessForHooking()) {
        Terminate();
        return false;
    }

    // Resume the process
    ResumeThread(process_info_.hThread);
    return true;
}

void FM2KGameInstance::Terminate() {
    UninstallHooks();

    if (process_handle_) {
        TerminateProcess(process_handle_, 0);
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
    }

    if (process_info_.hThread) {
        CloseHandle(process_info_.hThread);
        process_info_.hThread = nullptr;
    }

    process_id_ = 0;
    memset(&process_info_, 0, sizeof(process_info_));
}

bool FM2KGameInstance::InstallHooks() {
    // TODO: Implement hook installation
    return true;
}

bool FM2KGameInstance::UninstallHooks() {
    // TODO: Implement hook removal
    return true;
}

bool FM2KGameInstance::SaveState(void* buffer, size_t buffer_size) {
    if (!buffer || buffer_size < sizeof(FM2K::GameState)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Invalid buffer for state save");
        return false;
    }

    // Copy current state to buffer
    memcpy(buffer, game_state_.get(), sizeof(FM2K::GameState));
    return true;
}

bool FM2KGameInstance::LoadState(const void* buffer, size_t buffer_size) {
    if (!buffer || buffer_size < sizeof(FM2K::GameState)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Invalid buffer for state load");
        return false;
    }

    // Copy buffer to current state
    memcpy(game_state_.get(), buffer, sizeof(FM2K::GameState));
    return true;
}

bool FM2KGameInstance::AdvanceFrame() {
    // TODO: Implement frame advance
    return true;
}

void FM2KGameInstance::InjectInputs(uint32_t p1_input, uint32_t p2_input) {
    if (!game_state_) return;

    game_state_->players[0].input_current = p1_input;
    game_state_->players[1].input_current = p2_input;
}

bool FM2KGameInstance::SetupProcessForHooking() {
    // TODO: Implement process setup for hooking
    return true;
}

bool FM2KGameInstance::LoadGameExecutable(const std::filesystem::path& exe_path) {
    (void)exe_path; // Unused for now
    // TODO: Implement game executable loading
    return true;
}

void FM2KGameInstance::ProcessIPCEvents() {
    // TODO: Implement IPC event processing
}

void FM2KGameInstance::OnFrameAdvanced(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "Frame advanced: %u", event.data.state.frame_number);
}

void FM2KGameInstance::OnStateSaved(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "State saved: frame %u, checksum %08x",
        event.data.state.frame_number, event.data.state.checksum);
}

void FM2KGameInstance::OnStateLoaded(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "State loaded: frame %u, checksum %08x",
        event.data.state.frame_number, event.data.state.checksum);
}

void FM2KGameInstance::OnHitTablesInit(const FM2K::IPC::Event& event) {
    (void)event; // currently unused
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "Hit tables initialized");
}

void FM2KGameInstance::OnVisualStateChanged(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "Visual state changed: flags %08x",
        event.data.visual.effect_flags);
}

void FM2KGameInstance::OnHookError(const FM2K::IPC::Event& event) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "Hook error: %s", event.data.error.message);
} 