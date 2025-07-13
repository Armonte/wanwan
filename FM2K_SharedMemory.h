#pragma once

#include <cstdint>

// Shared memory structures for communication between FM2K Hook DLL and Launcher

// Save state profile enumeration (shared between hook and launcher)
enum class SaveStateProfile : uint32_t {
    MINIMAL = 0,    // ~50KB - Core state + active objects only
    STANDARD = 1,   // ~200KB - Essential runtime state  
    COMPLETE = 2    // ~850KB - Everything (current implementation)
};

// Shared performance statistics structure
struct SharedPerformanceStats {
    uint32_t total_saves;
    uint32_t total_loads;
    uint32_t avg_save_time_us;
    uint32_t avg_load_time_us;
    uint32_t memory_usage_mb;
    
    // Rollback performance counters
    uint32_t rollback_count;          // Total rollbacks since session start
    uint32_t max_rollback_frames;     // Maximum rollback distance ever seen
    uint32_t total_rollback_frames;   // Total frames rolled back
    uint32_t avg_rollback_frames;     // Average rollback distance
    uint64_t last_rollback_time_us;   // Last rollback timestamp (microseconds)
    uint32_t rollbacks_this_second;   // Current second rollback count
    uint64_t current_second_start;    // Start time of current second window
};

// Slot status information
struct SharedSlotInfo {
    bool occupied;
    uint32_t frame_number;
    uint64_t timestamp_ms;
    uint32_t checksum;
    uint32_t state_size_kb;  // Size in KB for analysis
    uint32_t save_time_us;   // Save time in microseconds
    uint32_t load_time_us;   // Load time in microseconds
};

// Main shared memory structure between Hook DLL and Launcher
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
    
    // Debug commands from launcher
    bool debug_save_state_requested;
    bool debug_load_state_requested;
    uint32_t debug_rollback_frames;
    bool debug_rollback_requested;
    uint32_t debug_command_id;  // Incremented for each command to ensure processing
    
    // Slot-based save/load system
    bool debug_save_to_slot_requested;
    bool debug_load_from_slot_requested;
    uint32_t debug_target_slot;  // Which slot to save to / load from (0-7)
    
    // Auto-save configuration
    bool auto_save_enabled;
    uint32_t auto_save_interval_frames;  // How often to auto-save
    SaveStateProfile save_profile;       // Which save state profile to use
    
    // Production mode settings
    bool production_mode;                // Enable production mode (reduced logging)
    bool enable_input_recording;         // Record inputs to file for testing
    
    // Slot status feedback to UI (8 save slots)
    SharedSlotInfo slot_status[8];
    
    // Performance statistics (including rollback counters)
    SharedPerformanceStats perf_stats;
    
    // GekkoNet client role coordination (simplified)
    uint8_t player_index;            // 0 for Player 1, 1 for Player 2
    uint8_t session_role;            // 0 = Host, 1 = Guest
};