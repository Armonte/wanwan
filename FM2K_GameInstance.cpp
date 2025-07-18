#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
// DLL injection approach - no direct hooks needed
#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <locale>
#include <codecvt>
#include <windows.h>

// Shared memory structure matching the DLL
struct SharedInputData {
    uint32_t frame_number;
    uint16_t p1_input;
    uint16_t p2_input;
    bool valid;
    
    // Network configuration
    bool is_online_mode;
    bool is_host;
    char remote_address[64];
    uint16_t port;
    uint8_t input_delay;
    bool config_updated;
};

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
    , shared_memory_handle_(nullptr)
    , shared_memory_data_(nullptr)
    , last_processed_frame_(0)
{
    process_info_ = {};
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
    
    // Note: IPC initialization moved to after game launch to avoid race condition
    
    return true;
}

bool FM2KGameInstance::Launch(const std::string& exe_path) {
    // Use SDL3's cross-platform filesystem helpers for existence checks.
    if (!SDL_GetPathInfo(exe_path.c_str(), nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Game executable not found: %s (%s)",
            exe_path.c_str(), SDL_GetError());
        return false;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating game process in suspended state...");

    // Convert path to Windows format for CreateProcess
    std::string exe_path_win = exe_path;
    std::replace(exe_path_win.begin(), exe_path_win.end(), '/', '\\');

    // Extract directory from exe path for working directory
    std::filesystem::path exe_file_path(exe_path_win);
    std::string working_dir = exe_file_path.parent_path().string();
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::wstring wide_exe_path(exe_path_win.begin(), exe_path_win.end());
    std::wstring wide_cmd_line = L"\"" + wide_exe_path + L"\"";
    std::wstring wide_working_dir(working_dir.begin(), working_dir.end());

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating process: %s", exe_path_win.c_str());
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Working directory: %s", working_dir.c_str());

    if (!CreateProcessW(
        wide_exe_path.c_str(),    // Application name
        const_cast<LPWSTR>(wide_cmd_line.c_str()), // Command line
        nullptr,                   // Process handle not inheritable
        nullptr,                   // Thread handle not inheritable
        FALSE,                     // Set handle inheritance to FALSE
        CREATE_SUSPENDED,          // Create suspended for DLL injection
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

    // Inject simple hook DLL
    std::wstring dll_path = GetDLLPath();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using DLL path: %s", 
                std::string(dll_path.begin(), dll_path.end()).c_str());
    
    if (!SetupProcessForHooking(std::string(dll_path.begin(), dll_path.end()))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to inject hook DLL");
        TerminateProcess(process_handle_, 1);
        CloseHandle(process_info_.hProcess);
        CloseHandle(process_info_.hThread);
        return false;
    }

    // Resume the game process
    ResumeThread(process_info_.hThread);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game process launched successfully");
    
    // Simple DLL injection complete - no IPC needed
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hook DLL injected successfully");
    
    // Initialize shared memory for configuration passing
    InitializeSharedMemory();
    
    return true;
}

void FM2KGameInstance::Terminate() {
    UninstallHooks();
    
    // Cleanup shared memory
    CleanupSharedMemory();

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
    // Hooks are installed via DLL injection in SetupProcessForHooking
    // No additional action needed from launcher side
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks managed by injected DLL");
    return true;
}

bool FM2KGameInstance::UninstallHooks() {
    // Hooks will be uninstalled when DLL is unloaded (process termination)
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks will be uninstalled with process termination");
    return true;
}

bool FM2KGameInstance::SaveState(void* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Invalid buffer for state save");
        return false;
    }

    // DLL handles state saving directly
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "State save requested");
    return true;
}

bool FM2KGameInstance::LoadState(const void* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Invalid buffer for state load");
        return false;
    }

    // DLL handles state loading directly
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "State load requested");
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

    // Direct hooks - no IPC events to process

    return true;
}

void FM2KGameInstance::InjectInputs(uint32_t p1_input, uint32_t p2_input) {
    // DLL handles input injection directly
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
        "Input injection requested: P1=0x%04X, P2=0x%04X", p1_input, p2_input);
}

bool FM2KGameInstance::SetupProcessForHooking(const std::string& dll_path) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Setting up process for DLL injection...");

    if (dll_path.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DLL path is empty");
        return false;
    }

    // Allocate memory in the target process for the DLL path
    SIZE_T path_size = dll_path.length() + 1;
    LPVOID remote_memory = VirtualAllocEx(process_handle_, nullptr, path_size, 
                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_memory) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "VirtualAllocEx failed: %lu", GetLastError());
        return false;
    }

    // Write the DLL path to the allocated memory
    SIZE_T bytes_written;
    if (!WriteProcessMemory(process_handle_, remote_memory, dll_path.c_str(), 
                           path_size, &bytes_written)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "WriteProcessMemory failed: %lu", GetLastError());
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    // Get LoadLibraryA address
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (!kernel32) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get kernel32 handle");
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    LPTHREAD_START_ROUTINE load_library = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryA");
    if (!load_library) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get LoadLibraryA address");
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    // Create remote thread to load the DLL
    DWORD thread_id;
    HANDLE remote_thread = CreateRemoteThread(process_handle_, nullptr, 0, 
                                             load_library, remote_memory, 0, &thread_id);
    if (!remote_thread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CreateRemoteThread failed: %lu", GetLastError());
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    // Wait for the DLL to load
    DWORD wait_result = WaitForSingleObject(remote_thread, 5000); // 5 second timeout
    if (wait_result != WAIT_OBJECT_0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DLL injection timeout or failed: %lu", wait_result);
        TerminateThread(remote_thread, 1);
        CloseHandle(remote_thread);
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    // Get the return value (module handle)
    DWORD exit_code;
    if (!GetExitCodeThread(remote_thread, &exit_code)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GetExitCodeThread failed: %lu", GetLastError());
        CloseHandle(remote_thread);
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    CloseHandle(remote_thread);
    VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);

    if (exit_code == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DLL failed to load (LoadLibrary returned NULL)");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DLL injection successful: %s", dll_path.c_str());
    return true;
}

bool FM2KGameInstance::LoadGameExecutable(const std::filesystem::path& exe_path) {
    (void)exe_path; // Unused for now
    // TODO: Implement game executable loading
    return true;
}

void FM2KGameInstance::ProcessDLLEvents() {
    // Process SDL events (for UI)
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Process custom events from the injected DLL
        if (event.type >= SDL_EVENT_USER) {
            HandleDLLEvent(event);
        }
        // Note: Other SDL events (window, input, etc.) are handled by the main UI loop
    }
}

void FM2KGameInstance::SetNetworkConfig(bool is_online, bool is_host, const std::string& remote_addr, uint16_t port, uint8_t input_delay) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Setting network config - Online: %s, Host: %s, Addr: %s, Port: %d, Delay: %d",
                is_online ? "YES" : "NO", is_host ? "YES" : "NO", remote_addr.c_str(), port, input_delay);
    
    // If shared memory is not initialized, initialize it first
    if (!shared_memory_data_) {
        InitializeSharedMemory();
    }
    
    // Write configuration to shared memory
    if (shared_memory_data_) {
        SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data_);
        shared_data->is_online_mode = is_online;
        shared_data->is_host = is_host;
        shared_data->port = port;
        shared_data->input_delay = input_delay;
        shared_data->config_updated = true;
        
        // Copy remote address safely
        strncpy_s(shared_data->remote_address, sizeof(shared_data->remote_address), 
                  remote_addr.c_str(), _TRUNCATE);
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Network configuration written to shared memory");
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot set network config - shared memory not available");
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



void FM2KGameInstance::InitializeSharedMemory() {
    // Open the shared memory created by the DLL
    shared_memory_handle_ = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        "FM2K_InputSharedMemory"
    );
    
    if (shared_memory_handle_ != nullptr) {
        shared_memory_data_ = MapViewOfFile(
            shared_memory_handle_,
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            sizeof(SharedInputData)
        );
        
        if (shared_memory_data_) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Shared memory opened successfully");
            last_processed_frame_ = 0;
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to map shared memory view");
        }
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to open shared memory (DLL might not be ready yet)");
    }
}

void FM2KGameInstance::CleanupSharedMemory() {
    if (shared_memory_data_) {
        UnmapViewOfFile(shared_memory_data_);
        shared_memory_data_ = nullptr;
    }
    if (shared_memory_handle_) {
        CloseHandle(shared_memory_handle_);
        shared_memory_handle_ = nullptr;
    }
}

void FM2KGameInstance::PollInputs() {
    // DLL handles input polling and GekkoNet directly
    // No need for shared memory polling from launcher
} 