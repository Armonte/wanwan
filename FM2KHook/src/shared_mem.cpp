#include "shared_mem.h"
#include "globals.h"
#include "state_manager.h"
#include "logging.h"

static HANDLE shared_memory_handle = nullptr;
static void* shared_memory_data = nullptr;

// Performance tracking
static uint32_t total_saves = 0;
static uint32_t total_loads = 0;
static uint64_t total_save_time_us = 0;
static uint64_t total_load_time_us = 0;
static uint32_t rollback_count = 0;
static uint32_t max_rollback_frames = 0;
static uint32_t total_rollback_frames = 0;

static inline uint64_t get_microseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

bool InitializeSharedMemory() {
    DWORD process_id = GetCurrentProcessId();
    std::string shared_memory_name = "FM2K_InputSharedMemory_" + std::to_string(process_id);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Creating shared memory with name: %s (PID=%lu)", shared_memory_name.c_str(), process_id);

    // First try to open existing shared memory (launcher creates it first)
    shared_memory_handle = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        shared_memory_name.c_str()
    );
    
    bool created_new = false;
    if (shared_memory_handle == nullptr) {
        // If it doesn't exist, create it
        shared_memory_handle = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            sizeof(SharedInputData),
            shared_memory_name.c_str()
        );
        created_new = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Created NEW shared memory segment");
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Opened EXISTING shared memory segment");
    }

    if (shared_memory_handle == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to create/open shared memory");
        return false;
    }

    shared_memory_data = MapViewOfFile(
        shared_memory_handle,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(SharedInputData)
    );

    if (shared_memory_data == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to map shared memory view");
        CloseHandle(shared_memory_handle);
        shared_memory_handle = nullptr;
        return false;
    }

    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    
    // Only zero out if we created brand new memory
    if (created_new || shared_data->config_version == 0) {
        memset(shared_data, 0, sizeof(SharedInputData));
        shared_data->config_version = 1;
        shared_data->player_index = player_index;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initialized fresh shared memory segment");
    } else {
        // Just update our player index, preserve existing data
        shared_data->player_index = player_index;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Connected to existing shared memory, preserving slot data");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Shared memory initialized successfully");
    return true;
}

void CleanupSharedMemory() {
    if (shared_memory_data) {
        UnmapViewOfFile(shared_memory_data);
        shared_memory_data = nullptr;
    }
    if (shared_memory_handle) {
        CloseHandle(shared_memory_handle);
        shared_memory_handle = nullptr;
    }
}

void ProcessDebugCommands() {
    if (!shared_memory_data) return;

    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    static uint32_t last_processed_command_id = 0;

    if (shared_data->debug_command_id == last_processed_command_id) {
        return;
    }

    // ... (rest of command processing)
    
    last_processed_command_id = shared_data->debug_command_id;
}

bool CheckConfigurationUpdates() {
    if (!shared_memory_data) return false;

    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    if (shared_data->config_updated) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Configuration update received - Online: %s, Host: %s", 
                    shared_data->is_online_mode ? "YES" : "NO", shared_data->is_host ? "YES" : "NO");
        
        is_online_mode = shared_data->is_online_mode;
        is_host = shared_data->is_host;
        use_minimal_gamestate_testing = shared_data->use_minimal_gamestate_testing;

        shared_data->config_updated = false;
        return true;
    }
    return false;
}

void UpdateRollbackStats(uint32_t frames_rolled_back) {
    if (!shared_memory_data) return;

    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    // ... (update perf_stats)
}

SharedInputData* GetSharedMemory() {
    if (!shared_memory_data) return nullptr;
    return static_cast<SharedInputData*>(shared_memory_data);
} 