#pragma once

#include "common.h"

// Shared memory structure matching the launcher
struct SharedInputData {
    uint32_t frame_number;
    uint16_t p1_input;
    uint16_t p2_input;
    bool valid;
    
    bool is_online_mode;
    bool is_host;
    char remote_address[64];
    uint16_t port;
    uint8_t input_delay;
    bool config_updated;
    
    bool debug_save_state_requested;
    bool debug_load_state_requested;
    uint32_t debug_rollback_frames;
    bool debug_rollback_requested;
    uint32_t debug_command_id;
    
    bool debug_save_to_slot_requested;
    bool debug_load_from_slot_requested;
    uint32_t debug_target_slot;
    
    bool auto_save_enabled;
    uint32_t auto_save_interval_frames;
    
    bool production_mode;
    bool enable_input_recording;
    
    bool use_minimal_gamestate_testing;
    
    uint32_t config_version;
    
    struct SlotInfo {
        bool occupied;
        uint32_t frame_number;
        uint64_t timestamp_ms;
        uint32_t checksum;
        uint32_t state_size_kb;
        uint32_t save_time_us;
        uint32_t load_time_us;
    } slot_status[8];
    
    struct PerformanceStats {
        uint32_t total_saves;
        uint32_t total_loads;
        uint32_t avg_save_time_us;
        uint32_t avg_load_time_us;
        uint32_t memory_usage_mb;
        uint32_t rollback_count;
        uint32_t max_rollback_frames;
        uint32_t total_rollback_frames;
        uint32_t avg_rollback_frames;
        uint64_t last_rollback_time_us;
        uint32_t rollbacks_this_second;
        uint64_t current_second_start;
    } perf_stats;
    
    uint8_t player_index;
    uint8_t session_role;
};

// Functions for managing shared memory
bool InitializeSharedMemory();
void CleanupSharedMemory();
void ProcessDebugCommands();
bool CheckConfigurationUpdates();
void UpdateRollbackStats(uint32_t frames_rolled_back);
SharedInputData* GetSharedMemory(); 