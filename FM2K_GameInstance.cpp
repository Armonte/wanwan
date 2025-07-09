#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include "FM2K_DLLInjector.h"
#include "FM2KHook/src/ipc.h"
#include "FM2KHook/src/state_manager.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <locale>
#include <codecvt>
#include <windows.h>

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

// Helper function to convert UTF-8 to wide string using Windows API
std::wstring UTF8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), wstr.data(), size_needed);
    return wstr;
}

} // anonymous namespace

FM2KGameInstance::FM2KGameInstance()
    : process_handle_(nullptr)
    , process_id_(0)
    , game_state_(std::make_unique<FM2K::GameState>())
    , session_(nullptr)
{
    process_info_ = {};
    SDL_zero(*game_state_);
}

FM2KGameInstance::~FM2KGameInstance() {
    Terminate();
    FM2K::IPC::Shutdown();
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
    
    // Note: IPC initialization moved to after game launch to avoid race condition
    
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

    // Look for FM2KHook.dll beside the launcher executable
    wchar_t buffer[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::filesystem::path launcher_path(buffer);
    std::filesystem::path hook_dll_path_fs = launcher_path.parent_path() / "FM2KHook.dll";
    std::string hook_dll_path = hook_dll_path_fs.string();

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Looking for FM2KHook.dll at: %s", hook_dll_path.c_str());

    if (!SDL_GetPathInfo(hook_dll_path.c_str(), nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "FM2KHook.dll not found beside launcher: %s (%s)",
            hook_dll_path.c_str(), SDL_GetError());
        return false;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating game process in suspended state...");

    // Convert path to Windows format for CreateProcess
    std::string exe_path = game.exe_path;
    std::replace(exe_path.begin(), exe_path.end(), '/', '\\');

    // Extract directory from exe path for working directory
    std::filesystem::path exe_file_path(exe_path);
    std::string working_dir = exe_file_path.parent_path().string();
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::wstring wide_exe_path(exe_path.begin(), exe_path.end());
    std::wstring wide_cmd_line = L"\"" + wide_exe_path + L"\"";
    std::wstring wide_working_dir(working_dir.begin(), working_dir.end());

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating process: %s", exe_path.c_str());
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Working directory: %s", working_dir.c_str());

    if (!CreateProcessW(
        wide_exe_path.c_str(),    // Application name
        const_cast<LPWSTR>(wide_cmd_line.c_str()), // Command line
        nullptr,                   // Process handle not inheritable
        nullptr,                   // Thread handle not inheritable
        FALSE,                     // Set handle inheritance to FALSE
        CREATE_SUSPENDED,          // Create in suspended state
        nullptr,                   // Use parent's environment block
        wide_working_dir.c_str(), // Use game's directory as starting directory
        &si,                       // Pointer to STARTUPINFO structure
        &pi                        // Pointer to PROCESS_INFORMATION structure
    )) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "CreateProcess failed for %s with error: %lu", 
            exe_path.c_str(), GetLastError());
        return false;
    }

    process_info_ = pi;
    process_handle_ = pi.hProcess;
    process_id_ = pi.dwProcessId;

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Process created with ID: %lu", process_id_);

    // Setup process for hooking (inject DLL, etc)
    if (!SetupProcessForHooking(hook_dll_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to setup process for hooking");
        TerminateProcess(process_handle_, 1);
        CloseHandle(process_info_.hProcess);
        CloseHandle(process_info_.hThread);
        return false;
    }

    // Resume the process
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Resuming process thread...");
    ResumeThread(process_info_.hThread);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game process launched successfully");
    
    // Wait a moment for the hook DLL to initialize and create IPC shared memory
    SDL_Delay(500); // 500ms should be enough
    
    // Now initialize IPC connection to read events from the injected DLL
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher: Connecting to hook DLL IPC...");
    if (!FM2K::IPC::Init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "LAUNCHER: Failed to connect to IPC system - hook may not be initialized");
        // Don't fail the launch, just log the error
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher: Successfully connected to IPC");
    }
    
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
    // Note: Hooks are actually installed by the injected DLL (FM2KHook.dll)
    // This function is called after DLL injection to verify hooks are working
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Waiting for hook installation confirmation...");
    
    // Wait for initialization event from DLL for up to 5 seconds
    const Uint32 timeout_ms = 5000;
    const Uint32 start_time = SDL_GetTicks();
    
    while (SDL_GetTicks() - start_time < timeout_ms) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Check for custom events from the DLL
            if (event.type >= SDL_EVENT_USER) {
                Uint32 event_code = event.user.code;
                
                if (event_code == 0) { // HOOKS_INITIALIZED
                    bool success = reinterpret_cast<uintptr_t>(event.user.data1) != 0;
                    if (success) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                            "Hooks installation confirmed by DLL");
                        return true;
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                            "Hook installation failed according to DLL");
                        return false;
                    }
                } else {
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
                        "Received other event from DLL: code %u", event_code);
                }
            }
        }
        
        // Small delay to avoid busy waiting
        SDL_Delay(10);
    }
    
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
        "Timeout waiting for hook installation confirmation, assuming success");
    return true;
}

bool FM2KGameInstance::UninstallHooks() {
    // Note: Hooks are uninstalled by the DLL when it's unloaded
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks uninstallation delegated to DLL unload");
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
    if (!process_handle_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No valid process handle");
        return false;
    }

    // For GekkoNet integration, frame advancement is handled by the hook
    // The game runs naturally and the hook coordinates with GekkoNet
    // No need to call remote functions - the hook does this automatically
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "AdvanceFrame called - letting hook handle frame advancement");

    // Process any pending IPC events
    ProcessIPCEvents();

    return true;
}

void FM2KGameInstance::InjectInputs(uint32_t p1_input, uint32_t p2_input) {
    if (!game_state_) return;

    game_state_->players[0].input_current = p1_input;
    game_state_->players[1].input_current = p2_input;
}

bool FM2KGameInstance::SetupProcessForHooking(const std::string& dll_path) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Setting up process for hooking...");

    // Convert dll_path to wstring for Windows API
    std::wstring wide_dll_path = UTF8ToWide(dll_path);
    
    // Inject the DLL into the target process
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Injecting FM2KHook.dll...");
    if (!FM2K::DLLInjector::InjectAndInit(process_handle_, wide_dll_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to inject FM2KHook.dll");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Successfully injected FM2KHook.dll");
    return true;
}

bool FM2KGameInstance::LoadGameExecutable(const std::filesystem::path& exe_path) {
    (void)exe_path; // Unused for now
    // TODO: Implement game executable loading
    return true;
}

void FM2KGameInstance::ProcessIPCEvents() {
    // First process IPC events from the hook DLL
    FM2K::IPC::Event ipc_event;
    int events_processed = 0;
    while (FM2K::IPC::PollEvent(&ipc_event)) {
        HandleIPCEvent(ipc_event);
        events_processed++;
        
        // Prevent infinite loop in case of issues
        if (events_processed > 1000) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                "Too many IPC events in single frame - breaking");
            break;
        }
    }
    
    // Reduced logging - only log significant processing
    static int total_processed = 0;
    total_processed += events_processed;
    if (total_processed % 1000 == 0 && events_processed > 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
            "Processed %d total IPC events", total_processed);
    }
    
    // Then process SDL events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Process custom events from the injected DLL
        if (event.type >= SDL_EVENT_USER) {
            HandleDLLEvent(event);
        }
        // Note: Other SDL events (window, input, etc.) are handled by the main UI loop
    }
}

void FM2KGameInstance::HandleDLLEvent(const SDL_Event& event) {
    // Decode event data based on event type
    Uint32 event_subtype = event.user.code;
    void* data1 = event.user.data1;
    void* data2 = event.user.data2;
    
    switch (event_subtype) {
        case 0: // HOOKS_INITIALIZED
            {
                bool success = reinterpret_cast<uintptr_t>(data1) != 0;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hook initialization event: %s", success ? "success" : "failed");
            }
            break;
            
        case 1: // FRAME_ADVANCED
            {
                uint32_t frame_number = reinterpret_cast<uintptr_t>(data1);
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame advanced: %u", frame_number);
            }
            break;
            
        case 2: // STATE_SAVED
            {
                uint32_t frame_number = reinterpret_cast<uintptr_t>(data1);
                uint32_t checksum = reinterpret_cast<uintptr_t>(data2);
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                    "State saved: frame %u, checksum %08x", frame_number, checksum);
            }
            break;
            
        case 3: // VISUAL_STATE_CHANGED
            {
                uint32_t frame_number = reinterpret_cast<uintptr_t>(data1);
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                    "Visual state changed at frame %u", frame_number);
            }
            break;
            
        case 255: // HOOK_ERROR
            {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Hook error reported by DLL");
            }
            break;
            
        default:
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                "Unknown DLL event subtype: %u", event_subtype);
            break;
    }
}

void FM2KGameInstance::HandleIPCEvent(const FM2K::IPC::Event& event) {
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
            
        case FM2K::IPC::EventType::INPUT_CAPTURED:
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
            "input captured");
            OnInputCaptured(event);

            break;
            
        case FM2K::IPC::EventType::HOOK_ERROR:
            OnHookError(event);
            break;
            
        case FM2K::IPC::EventType::LOG_MESSAGE:
            // Route the log from the DLL to the launcher's own SDL log.
            SDL_LogMessage(event.data.log.category, 
                           (SDL_LogPriority)event.data.log.priority, 
                           "[HOOK DLL] %s", 
                           event.data.log.message);
            break;

        default:
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                "Unknown IPC event type: %d", static_cast<int>(event.type));
            break;
    }
}

// Helper function to execute a function in the game process
bool FM2KGameInstance::ExecuteRemoteFunction(HANDLE process, uintptr_t function_address) {
    HANDLE thread = CreateRemoteThread(process, 
                                     nullptr, 
                                     0, 
                                     reinterpret_cast<LPTHREAD_START_ROUTINE>(function_address),
                                     nullptr, 
                                     0, 
                                     nullptr);
    
    if (!thread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to create remote thread at 0x%08X: %lu",
            function_address, GetLastError());
        return false;
    }

    // Wait for the function to complete
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return true;
}

void FM2KGameInstance::OnFrameAdvanced(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "Frame advanced: %u", event.frame_number);
}

void FM2KGameInstance::OnStateSaved(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "State saved: frame %u, checksum %08x",
        event.frame_number, event.data.state.checksum);
}

void FM2KGameInstance::OnStateLoaded(const FM2K::IPC::Event& event) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "State loaded: frame %u, checksum %08x",
        event.frame_number, event.data.state.checksum);
}

void FM2KGameInstance::OnInputCaptured(const FM2K::IPC::Event& event) {
    auto& input_event = event.data.input;
    if (session_) {
        // Choose input method based on session mode
        if (session_->GetSessionMode() == SessionMode::LOCAL) {
            // LOCAL mode: Forward both P1 and P2 inputs (LocalSession pattern)
            uint32_t p1_input_32 = static_cast<uint32_t>(input_event.p1_input);
            uint32_t p2_input_32 = static_cast<uint32_t>(input_event.p2_input);
            
            session_->AddBothInputs(p1_input_32, p2_input_32);
            
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                "LOCAL mode: Both inputs forwarded to Session: P1=0x%04x, P2=0x%04x, frame=%u",
                input_event.p1_input, input_event.p2_input, event.frame_number);
        } else {
            // ONLINE mode: Forward only local player input (OnlineSession pattern)
            uint32_t p1_input_32 = static_cast<uint32_t>(input_event.p1_input);
            
            session_->AddLocalInput(p1_input_32);
            
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                "ONLINE mode: Local input forwarded to Session: P1=0x%04x, frame=%u",
                input_event.p1_input, event.frame_number);
        }
    } else {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
            "Input captured but no Session connected: P1=0x%04x, P2=0x%04x",
            input_event.p1_input, input_event.p2_input);
    }
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