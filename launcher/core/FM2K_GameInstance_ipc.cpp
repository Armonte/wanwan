// FM2K_GameInstance_ipc.cpp -- runtime IPC surface (split from
// FM2K_GameInstance.cpp): rollback save/load/advance/inject, the DLL-event
// pump, shared-memory setup, and the Trigger/Set/Get/Step debug commands.
// Member fns of FM2KGameInstance (class in FM2K_GameInstance.h).
#include "FM2K_GameInstance.h"
#include "FM2K_DDrawRedirect.h"
#include "FM2K_GameIni.h"
#include "FM2K_Integration.h"
#include "FM2KHook/src/ui/shared_mem.h"  // FM2KSharedMemData (read-only stats from hook)
// DLL injection approach - no direct hooks needed
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <locale>
#include <codecvt>
#include <vector>
#include <windows.h>

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
    // Removed - hook reads config from env vars
    (void)is_online; (void)is_host; (void)remote_addr; (void)port; (void)input_delay;
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
void FM2KGameInstance::InitializeSharedMemory() {
    // Create unique shared memory name using process ID
    std::string shared_memory_name = "FM2K_SharedMem_" + std::to_string(process_id_);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LAUNCHER: Opening shared memory with name: %s (PID=%lu)", shared_memory_name.c_str(), process_id_);

    // Retry opening shared memory for up to 2 seconds (hook DLL needs time to initialize)
    for (int attempt = 0; attempt < 40; attempt++) {
        shared_memory_handle_ = OpenFileMappingA(
            FILE_MAP_READ,
            FALSE,
            shared_memory_name.c_str()
        );

        if (shared_memory_handle_ != nullptr) {
            shared_memory_data_ = MapViewOfFile(
                shared_memory_handle_,
                FILE_MAP_READ,
                0,
                0,
                sizeof(FM2KSharedMemData)
            );

            if (shared_memory_data_) {
                // Validate magic number
                FM2KSharedMemData* shared_data = static_cast<FM2KSharedMemData*>(shared_memory_data_);
                if (shared_data->magic != FM2K_SHARED_MEM_MAGIC) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Shared memory magic mismatch: 0x%08X (expected 0x%08X)",
                                shared_data->magic, FM2K_SHARED_MEM_MAGIC);
                    UnmapViewOfFile(shared_memory_data_);
                    shared_memory_data_ = nullptr;
                    CloseHandle(shared_memory_handle_);
                    shared_memory_handle_ = nullptr;
                    return;
                }

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Shared memory opened successfully on attempt %d (version=%u)",
                           attempt + 1, shared_data->version);
                last_processed_frame_ = 0;
                return;
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to map shared memory view");
                CloseHandle(shared_memory_handle_);
                shared_memory_handle_ = nullptr;
                return;
            }
        }

        // Wait 50ms before next attempt
        SDL_Delay(50);
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open shared memory after 40 attempts (2 seconds)");
}

bool FM2KGameInstance::TryAttachSharedMemory() {
    if (shared_memory_data_) return true;
    if (process_id_ == 0) return false;

    const std::string name = "FM2K_SharedMem_" + std::to_string(process_id_);
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
    if (!h) return false;   // hook has not created it yet -- try again next frame

    void* view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(FM2KSharedMemData));
    if (!view) { CloseHandle(h); return false; }

    const auto* data = static_cast<const FM2KSharedMemData*>(view);
    if (data->magic != FM2K_SHARED_MEM_MAGIC) {
        // Mapping exists but is not ours (or not written yet). Do NOT latch --
        // drop it and retry; the hook writes magic in InitializeSharedMemory
        // before any other field.
        UnmapViewOfFile(view);
        CloseHandle(h);
        return false;
    }

    shared_memory_handle_ = h;
    shared_memory_data_   = view;
    last_processed_frame_ = 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Shared memory attached (%s, version=%u) -- the hook's outcome channel "
        "stays readable after the game process exits.",
        name.c_str(), data->version);
    return true;
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

// Debug state management functions
bool FM2KGameInstance::TriggerManualSaveState() {
    // Removed - hook reads config from env vars
    return false;
}

bool FM2KGameInstance::TriggerManualLoadState() {
    // Removed - hook reads config from env vars
    return false;
}

bool FM2KGameInstance::TriggerForceRollback(uint32_t frames) {
    // Removed - hook reads config from env vars
    (void)frames;
    return false;
}

// Frame stepping functions
void FM2KGameInstance::SetFrameStepPause(bool pause) {
    // Removed - hook reads config from env vars
    (void)pause;
}

void FM2KGameInstance::StepSingleFrame() {
    // Removed - hook reads config from env vars
}

void FM2KGameInstance::StepMultipleFrames(uint32_t frames) {
    // Removed - hook reads config from env vars
    (void)frames;
}

// Slot-based save/load functions
bool FM2KGameInstance::TriggerSaveToSlot(uint32_t slot) {
    // Removed - hook reads config from env vars
    (void)slot;
    return false;
}

bool FM2KGameInstance::TriggerLoadFromSlot(uint32_t slot) {
    // Removed - hook reads config from env vars
    (void)slot;
    return false;
}

// Auto-save configuration
bool FM2KGameInstance::SetAutoSaveEnabled(bool enabled) {
    // Removed - hook reads config from env vars
    (void)enabled;
    return false;
}

bool FM2KGameInstance::SetAutoSaveInterval(uint32_t frames) {
    // Removed - hook reads config from env vars
    (void)frames;
    return false;
}

bool FM2KGameInstance::GetAutoSaveConfig(AutoSaveConfig& config) {
    // Removed - hook reads config from env vars
    (void)config;
    return false;
}

// Get slot status information
bool FM2KGameInstance::GetSlotStatus(uint32_t slot, SlotStatus& status) {
    // Removed - hook reads config from env vars
    (void)slot; (void)status;
    return false;
}

// Set client role for LocalNetworkAdapter (HOST = 0, GUEST = 1)
bool FM2KGameInstance::SetClientRole(uint8_t player_index, bool is_host) {
    // Removed - hook reads config from env vars
    (void)player_index; (void)is_host;
    return false;
}

// Debug and testing configuration
bool FM2KGameInstance::SetProductionMode(bool enabled) {
    // Removed - hook reads config from env vars
    (void)enabled;
    return false;
}

bool FM2KGameInstance::SetInputRecording(bool enabled) {
    // Removed - hook reads config from env vars
    (void)enabled;
    return false;
}

bool FM2KGameInstance::SetMinimalGameStateTesting(bool enabled) {
    // Removed - hook reads config from env vars
    (void)enabled;
    return false;
}

void FM2KGameInstance::ApplyDeferredSettings() {
    // Removed - hook reads config from env vars
}

// Environment variable configuration for OnlineSession-style networking
void FM2KGameInstance::SetEnvironmentVariable(const std::string& name, const std::string& value) {
    environment_variables_[name] = value;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Set environment variable: %s=%s", name.c_str(), value.c_str());
}

