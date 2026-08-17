#pragma once

#include "SDL3/SDL.h"
#include "FM2K_Integration.h"  // FM2K::Engine
#include <string>
#include <memory>
#include <filesystem>
#include <map>
#include <windows.h>



class FM2KGameInstance {
public:
    FM2KGameInstance();
    ~FM2KGameInstance();

    bool Initialize();
    // Launches the game in suspended state, injects the appropriate hook DLL
    // (FM2KHook.dll for FM2K, FM95Hook.dll for FM95), then resumes. The hook
    // DLL drives the rollback netcode. Engine selection also activates the
    // Japanese-locale spoof at DLL_PROCESS_ATTACH (always-on for FM95Hook,
    // env-gated for FM2KHook via FM2K_JP_LOCALE=1).
    bool Launch(const std::string& exe_path,
                FM2K::Engine engine = FM2K::Engine::FM2K);
    void Terminate();
    bool IsRunning() const { 
        if (!process_handle_) return false;
        DWORD exit_code;
        if (!GetExitCodeProcess(process_handle_, &exit_code)) return false;
        return exit_code == STILL_ACTIVE;
    }
    
    DWORD GetProcessId() const { return process_id_; }
    
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
    
    // DLL communication (simplified)
    void ProcessDLLEvents();
    
    // Network configuration
    void SetNetworkConfig(bool is_online, bool is_host, const std::string& remote_addr = "", uint16_t port = 12345, uint8_t input_delay = 2);
    
    // Shared memory input polling
    void PollInputs();
    void InitializeSharedMemory();
    // ONE non-blocking attach attempt, safe to call every frame until it
    // succeeds. InitializeSharedMemory() above is the blocking 40x50ms variant
    // and has NO callers -- so shared_memory_data_ was permanently null and
    // GetSharedMemoryData() always returned nullptr. This exists because the
    // launcher must be able to read the hook's LAST published outcome AFTER
    // the game process exits: the section object is kernel-refcounted, so
    // holding our own view keeps it readable past process death. Without a
    // held view the mapping dies with the process and a refusal that
    // terminates (no-VS-mode, desync) is unreadable on any non-hub path.
    bool TryAttachSharedMemory();
    void ApplyDeferredSettings();
    void CleanupSharedMemory();

protected:
    // Process management
    bool SetupProcessForHooking(const std::string& dll_path);
    bool LoadGameExecutable(const std::filesystem::path& exe_path);
    void HandleDLLEvent(const SDL_Event& event);
    bool ExecuteRemoteFunction(HANDLE process, uintptr_t function_address);
    
public:
    // Debug state management functions
    bool TriggerManualSaveState();
    bool TriggerManualLoadState();
    bool TriggerForceRollback(uint32_t frames);
    
    // Frame stepping functions
    void SetFrameStepPause(bool pause);
    void StepSingleFrame();
    void StepMultipleFrames(uint32_t frames);
    
    // Slot-based save/load functions
    bool TriggerSaveToSlot(uint32_t slot);
    bool TriggerLoadFromSlot(uint32_t slot);
    
    // Auto-save configuration
    bool SetAutoSaveEnabled(bool enabled);
    bool SetAutoSaveInterval(uint32_t frames);
    
    struct AutoSaveConfig {
        bool enabled;
        uint32_t interval_frames;
    };
    bool GetAutoSaveConfig(AutoSaveConfig& config);
    
    // Debug and testing configuration
    bool SetProductionMode(bool enabled);
    bool SetInputRecording(bool enabled);
    
    // Shared memory access
    void* GetSharedMemoryData() const { return shared_memory_data_; }
    bool SetMinimalGameStateTesting(bool enabled);
    
    // Save state profile removed - now using optimized FastGameState system
    
    // GekkoNet client role configuration
    bool SetClientRole(uint8_t player_index, bool is_host);
    
    // Environment variable configuration for OnlineSession-style networking
    void SetEnvironmentVariable(const std::string& name, const std::string& value);
    
    // Slot status information
    struct SlotStatus {
        bool occupied;
        uint32_t frame_number;
        uint64_t timestamp_ms;
        uint32_t checksum;
        uint32_t state_size_kb;
        uint32_t save_time_us;
        uint32_t load_time_us;
        uint32_t active_object_count;
    };
    bool GetSlotStatus(uint32_t slot, SlotStatus& status);

private:
    HANDLE process_handle_;
    DWORD process_id_;
    PROCESS_INFORMATION process_info_;
    std::string game_exe_path_;  // Store the game executable path
    std::string game_dll_path_;  // Store the game's KGT/DLL path
    FM2K::Engine engine_ = FM2K::Engine::FM2K;  // Selects FM2KHook.dll vs FM95Hook.dll at injection
    
    // Shared memory for input communication with injected DLL
    HANDLE shared_memory_handle_;
    void* shared_memory_data_;
    uint32_t last_processed_frame_;
    
    // Environment variables for process creation
    std::map<std::string, std::string> environment_variables_;
    
    // Deferred settings (applied when shared memory becomes available)
    struct DeferredSettings {
        bool has_minimal_gamestate_testing = false;
        bool minimal_gamestate_testing_value = false;
        bool has_production_mode = false;
        bool production_mode_value = false;
        bool has_input_recording = false;
        bool input_recording_value = false;
    } deferred_settings_;
};