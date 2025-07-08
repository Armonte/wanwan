#pragma once

#include "SDL3/SDL.h"
#include <string>
#include <memory>
#include <filesystem>
#include <windows.h>
#include "FM2KHook/src/ipc.h"

// Forward declarations
class NetworkSession;

namespace FM2K {
    struct FM2KGameInfo;
    struct GameState;
    namespace IPC {
        struct Event;
    }
}

class FM2KGameInstance {
public:
    FM2KGameInstance();
    ~FM2KGameInstance();
    
    bool Initialize();
    bool Launch(const FM2K::FM2KGameInfo& game);
    void Terminate();
    bool IsRunning() const { 
        if (!process_handle_) return false;
        DWORD exit_code;
        if (!GetExitCodeProcess(process_handle_, &exit_code)) return false;
        return exit_code == STILL_ACTIVE;
    }
    
    // Memory access
    template<typename T>
    inline bool ReadMemory(DWORD address, T* value) {
        if (!process_handle_ || !value) return false;
        SIZE_T bytes_read;
        return ReadProcessMemory(process_handle_, reinterpret_cast<LPCVOID>(address), 
            value, sizeof(T), &bytes_read) && bytes_read == sizeof(T);
    }
    
    template<typename T>
    inline bool WriteMemory(DWORD address, const T* value) {
        if (!process_handle_ || !value) return false;
        SIZE_T bytes_written;
        return WriteProcessMemory(process_handle_, reinterpret_cast<LPVOID>(address),
            value, sizeof(T), &bytes_written) && bytes_written == sizeof(T);
    }
    
    // Hook management
    bool InstallHooks();
    bool UninstallHooks();
    
    // State management
    bool SaveState(void* buffer, size_t buffer_size);
    bool LoadState(const void* buffer, size_t buffer_size);
    bool AdvanceFrame();
    
    // Input injection
    void InjectInputs(uint32_t p1_input, uint32_t p2_input);
    
    // IPC event processing
    void ProcessIPCEvents();
    
    // Event handlers
    void OnFrameAdvanced(const FM2K::IPC::Event& event);
    void OnStateSaved(const FM2K::IPC::Event& event);
    void OnStateLoaded(const FM2K::IPC::Event& event);
    void OnHitTablesInit(const FM2K::IPC::Event& event);
    void OnVisualStateChanged(const FM2K::IPC::Event& event);
    void OnInputCaptured(const FM2K::IPC::Event& event);
    void OnHookError(const FM2K::IPC::Event& event);
    
    // Network session for input forwarding
    void SetNetworkSession(NetworkSession* session) {
        network_session_ = session;
    }

protected:
    // Process management
    bool SetupProcessForHooking(const std::string& dll_path);
    bool LoadGameExecutable(const std::filesystem::path& exe_path);
    void HandleIPCEvent(const FM2K::IPC::Event& event);
    void HandleDLLEvent(const SDL_Event& event);
    bool ExecuteRemoteFunction(HANDLE process, uintptr_t function_address);

private:
    HANDLE process_handle_;
    DWORD process_id_;
    PROCESS_INFORMATION process_info_;
    std::unique_ptr<FM2K::GameState> game_state_;
    std::string game_exe_path_;  // Store the game executable path
    std::string game_dll_path_;  // Store the game's KGT/DLL path
    NetworkSession* network_session_;  // For input forwarding to GekkoNet
};