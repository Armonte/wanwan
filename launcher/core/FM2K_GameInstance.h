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
    // KEEP THE EXIT CODE. This used to read it and throw it away, so when a
    // player's game vanished mid-match the launcher knew THAT it had gone and
    // never why -- and if the hook's crash handler did not fire (a hard kill,
    // or a fault it cannot catch) that was the end of the evidence. It cost a
    // real investigation on 2026-09-04: a match where one side's log simply
    // stops mid-battle, no crash record, no bundle, nothing to distinguish an
    // access violation from someone closing the window.
    //
    // The code is captured on the running -> gone transition and reported
    // once. Values worth recognising: 0 is a clean exit, 1 is usually our own
    // Terminate, and anything of the form 0xC0000005 or 0xC0000094 is an
    // unhandled exception the process died of, which tells us the fault kind
    // even with no crash log.
    bool IsRunning() const {
        if (!process_handle_) return false;
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(process_handle_, &exit_code)) return false;
        if (exit_code == STILL_ACTIVE) return true;
        NoteExited(exit_code);
        return false;
    }

    // The last observed exit code, or 0 if the game has not been seen to
    // exit. STILL_ACTIVE is never stored here.
    DWORD LastExitCode() const { return last_exit_code_; }
    
    DWORD GetProcessId() const { return process_id_; }

    // Report the exit exactly once, from the const IsRunning path.
    void NoteExited(DWORD code) const;
    
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
    bool UninstallHooks();
    
    // DLL communication (simplified)
    void ProcessDLLEvents();
    
    // Network configuration
    void SetNetworkConfig(bool is_online, bool is_host, const std::string& remote_addr = "", uint16_t port = 12345, uint8_t input_delay = 2);
    
    // Shared memory attach. ONE non-blocking attempt, safe to call every frame
    // until it succeeds. (It replaced a blocking 40x50ms variant that had no
    // callers, so shared_memory_data_ was permanently null and
    // GetSharedMemoryData() always returned nullptr.) This exists because the
    // launcher must be able to read the hook's LAST published outcome AFTER
    // the game process exits: the section object is kernel-refcounted, so
    // holding our own view keeps it readable past process death. Without a
    // held view the mapping dies with the process and a refusal that
    // terminates (no-VS-mode, desync) is unreadable on any non-hub path.
    bool TryAttachSharedMemory();
    void CleanupSharedMemory();

protected:
    // Process management
    bool SetupProcessForHooking(const std::string& dll_path);
    void HandleDLLEvent(const SDL_Event& event);
    
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
    // Mutable so the const IsRunning() can record what it saw. Not thread
    // state: IsRunning is called from the launcher's own loop.
    mutable DWORD last_exit_code_ = 0;
    mutable bool  exit_reported_  = false;
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