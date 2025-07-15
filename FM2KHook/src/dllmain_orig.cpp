#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <memory>
#include <chrono>
#include <fstream>
#include <mutex>
#include <SDL3/SDL.h>
// Direct GekkoNet integration
#include "gekkonet.h"
#include "state_manager.h"

// Forward declare MinimalGameState for GekkoNet testing
namespace FM2K {
    // Minimal game state for GekkoNet rollback testing (48 bytes)
    // Contains only essential combat state to test desync detection
    struct MinimalGameState {
        // Core combat state (32 bytes)
        uint32_t p1_hp, p2_hp;              // Current HP (0x47010C, 0x47030C)
        uint32_t p1_max_hp, p2_max_hp;      // Max HP (0x4DFC85, 0x4EDC4)
        uint32_t p1_x, p1_y;                // Positions (0x4ADCC3, 0x4ADCC7)
        uint32_t p2_x, p2_y;                // Positions (0x4EDD02, 0x4EDD06)
        
        // Essential timers & RNG (16 bytes)
        uint32_t round_timer;                // 0x470044 or 0x47DB94
        uint32_t random_seed;                // 0x41FB1C
        uint32_t frame_number;               // Current frame
        uint32_t input_checksum;             // XOR of recent inputs
        
        // Calculate minimal state checksum
        uint32_t CalculateChecksum() const {
            // Simple Fletcher32-like checksum for 48 bytes
            uint32_t sum1 = 0, sum2 = 0;
            const uint32_t* data = reinterpret_cast<const uint32_t*>(this);
            for (int i = 0; i < 12; i++) {  // 48 bytes / 4 = 12 uint32_t
                sum1 += data[i];
                sum2 += sum1;
            }
            return (sum2 << 16) | (sum1 & 0xFFFF);
        }
        
        // Load minimal state from memory addresses
        bool LoadFromMemory();
        
        // Save minimal state to memory addresses
        bool SaveToMemory() const;
    };
}

// Save state system now uses optimized FastGameState for all saves

// Forward declarations
bool SaveCoreStateBasic(FM2K::State::GameState* state, uint32_t frame_number);
bool SaveGameStateDirect(FM2K::State::GameState* state, uint32_t frame_number);
uint32_t CalculateStateChecksum(const FM2K::State::GameState* state);
bool RestoreStateFromStruct(const FM2K::State::GameState* state, uint32_t target_frame);

// Direct GekkoNet session with real UDP networking
static GekkoSession* gekko_session = nullptr;
static int p1_handle = -1;
static int p2_handle = -1;
static bool gekko_initialized = false;
static bool gekko_session_started = false;      // Track if GekkoNet session has started (like BSNES AllPlayersValid)
static bool is_online_mode = false;
static bool is_host = false;
static uint8_t player_index = 0;               // 0 = Player 1, 1 = Player 2 (set during GekkoNet init)
static int local_player_handle = -1;           // Our local player handle for gekko_add_local_input

// File logging system for debug output
static std::ofstream log_file;
static std::mutex log_mutex;
static bool file_logging_enabled = false;
static bool production_mode = false;  // Global production mode flag

// Input recording system for testing
static std::ofstream input_record_file;
static std::mutex input_record_mutex;
static bool input_recording_enabled = false;

// Rollback performance tracking
static uint32_t rollback_count = 0;
static uint32_t max_rollback_frames = 0;
static uint32_t total_rollback_frames = 0;
static uint64_t last_rollback_time_us = 0;

// Thresholds for state detection (forward declarations)
static constexpr uint32_t STABILITY_THRESHOLD_FRAMES = 60;  // Objects stable for 1 second at 60 FPS
static constexpr uint32_t COMBAT_CREATION_THRESHOLD = 5;    // Objects/second for combat detection
static constexpr uint32_t TRANSITION_THRESHOLD = 10;        // Objects/second for transition detection

// Live input tracking (declared early for desync reports)
static uint32_t live_p1_input = 0;
static uint32_t live_p2_input = 0;

// MinimalGameState testing for GekkoNet desync detection (declared early for LogMinimalGameStateDesync)
static bool use_minimal_gamestate_testing = false;  // Enable via launcher UI
static FM2K::MinimalGameState minimal_state_ring[8];  // Ring buffer for minimal states
static uint32_t minimal_state_ring_index = 0;       // Current position in ring buffer

// Rift sync variables (similar to bsnes)
static uint32_t rift_sync_counter = 0;              // Frame counter for rift sync timing
static bool rift_sync_active = false;               // Flag to prevent rift sync loops

// Custom SDL log output function that writes to both console and file
static void CustomLogOutput(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    std::lock_guard<std::mutex> lock(log_mutex);
    
    // Production mode filtering: only show ERROR and WARN levels
    if (production_mode && priority > SDL_LOG_PRIORITY_WARN) {
        return;  // Skip INFO and DEBUG messages in production mode
    }
    
    // Get timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    struct tm* tm_info = localtime(&time_t);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
    
    // Format: [HH:MM:SS.mmm] [Player X] message
    char formatted_message[2048];
    snprintf(formatted_message, sizeof(formatted_message), "[%s.%03d] [Player %d] %s\n", 
             timestamp, (int)ms.count(), player_index + 1, message);
    
    // Write to console (original SDL behavior)
    printf("%s", formatted_message);
    
    // Write to file if enabled
    if (file_logging_enabled && log_file.is_open()) {
        log_file << formatted_message;
        log_file.flush();  // Ensure immediate write for debugging
    }
}

// Initialize file logging based on player index
static void InitializeFileLogging() {
    std::lock_guard<std::mutex> lock(log_mutex);
    
    if (file_logging_enabled) return;  // Already initialized
    
    // Create log filename based on player index
    char log_filename[128];
    snprintf(log_filename, sizeof(log_filename), "FM2K_Client%d_Debug.log", player_index + 1);
    
    // Open log file for writing (truncate existing)
    log_file.open(log_filename, std::ios::out | std::ios::trunc);
    if (log_file.is_open()) {
        file_logging_enabled = true;
        
        // Set custom SDL log output function
        SDL_SetLogOutputFunction(CustomLogOutput, nullptr);
        
        // Write initial header
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        struct tm* tm_info = localtime(&time_t);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
        
        log_file << "=== FM2K Hook Debug Log - Client " << (player_index + 1) << " ===" << std::endl;
        log_file << "Session started: " << timestamp << std::endl;
        log_file << "Player Index: " << (int)player_index << std::endl;
        log_file << "Is Host: " << (is_host ? "Yes" : "No") << std::endl;
        log_file << "===============================================" << std::endl;
        log_file.flush();
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "File logging initialized: %s", log_filename);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to open log file: %s", log_filename);
    }
}

// Initialize input recording based on player index
static void InitializeInputRecording() {
    std::lock_guard<std::mutex> lock(input_record_mutex);
    
    if (input_recording_enabled) return;  // Already initialized
    
    // Create input recording filename based on player index
    char record_filename[128];
    snprintf(record_filename, sizeof(record_filename), "FM2K_InputRecord_Client%d.dat", player_index + 1);
    
    // Open input recording file for binary writing (truncate existing)
    input_record_file.open(record_filename, std::ios::out | std::ios::binary | std::ios::trunc);
    if (input_record_file.is_open()) {
        input_recording_enabled = true;
        
        // Write file header
        struct InputRecordHeader {
            char magic[8] = "FM2KINP";
            uint32_t version = 1;
            uint32_t player_index;
            uint64_t timestamp;
        } header;
        
        header.player_index = player_index;
        header.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        input_record_file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        input_record_file.flush();
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Input recording initialized: %s", record_filename);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to open input recording file: %s", record_filename);
    }
}

// Record input to file
static void RecordInput(uint32_t frame, uint32_t p1_input, uint32_t p2_input) {
    if (!input_recording_enabled || !input_record_file.is_open()) return;
    
    std::lock_guard<std::mutex> lock(input_record_mutex);
    
    struct InputRecordEntry {
        uint32_t frame_number;
        uint32_t p1_input;
        uint32_t p2_input;
        uint64_t timestamp_us;
    } entry;
    
    entry.frame_number = frame;
    entry.p1_input = p1_input;
    entry.p2_input = p2_input;
    entry.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    
    input_record_file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    
    // Flush periodically for crash safety
    static uint32_t flush_counter = 0;
    if (++flush_counter % 100 == 0) {
        input_record_file.flush();
    }
}

// Cleanup file logging and input recording
static void CleanupFileLogging() {
    std::lock_guard<std::mutex> lock(log_mutex);
    
    if (file_logging_enabled && log_file.is_open()) {
        log_file << "=== Session ended ===" << std::endl;
        log_file.close();
        file_logging_enabled = false;
        
        // Restore default SDL log output
        SDL_SetLogOutputFunction(nullptr, nullptr);
    }
}

static void CleanupInputRecording() {
    std::lock_guard<std::mutex> lock(input_record_mutex);
    
    if (input_recording_enabled && input_record_file.is_open()) {
        input_record_file.close();
        input_recording_enabled = false;
    }
}

// Enhanced MinimalGameState desync analysis
static void LogMinimalGameStateDesync(uint32_t desync_frame, uint32_t local_checksum, uint32_t remote_checksum) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "=== MINIMAL GAMESTATE DESYNC ANALYSIS ===");
    
    // Load current MinimalGameState for comparison
    FM2K::MinimalGameState current_state;
    if (current_state.LoadFromMemory()) {
        current_state.frame_number = desync_frame;
        
        uint32_t calculated_checksum = current_state.CalculateChecksum();
        
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Current Local State:");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  P1 HP: %u / %u (%.1f%%)", current_state.p1_hp, current_state.p1_max_hp, 
                   current_state.p1_max_hp > 0 ? (float)current_state.p1_hp / current_state.p1_max_hp * 100.0f : 0.0f);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  P2 HP: %u / %u (%.1f%%)", current_state.p2_hp, current_state.p2_max_hp,
                   current_state.p2_max_hp > 0 ? (float)current_state.p2_hp / current_state.p2_max_hp * 100.0f : 0.0f);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  P1 Position: (%u, %u)", current_state.p1_x, current_state.p1_y);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  P2 Position: (%u, %u)", current_state.p2_x, current_state.p2_y);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  Round Timer: %u", current_state.round_timer);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  RNG Seed: 0x%08X", current_state.random_seed);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  Input Checksum: 0x%08X", current_state.input_checksum);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  Calculated Checksum: 0x%08X (expected: 0x%08X)", calculated_checksum, local_checksum);
        
        // Check if our calculated checksum matches the reported local checksum
        if (calculated_checksum != local_checksum) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "⚠️  WARNING: Calculated checksum doesn't match reported local checksum!");
        }
        
        // Analyze differences if we have a previous state
        uint32_t ring_index = (desync_frame - 1) % 8;
        FM2K::MinimalGameState& prev_state = minimal_state_ring[ring_index];
        if (prev_state.frame_number == desync_frame - 1) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Changes from previous frame:");
            if (current_state.p1_hp != prev_state.p1_hp) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  P1 HP: %u -> %u (Δ%d)", prev_state.p1_hp, current_state.p1_hp, (int)current_state.p1_hp - (int)prev_state.p1_hp);
            }
            if (current_state.p2_hp != prev_state.p2_hp) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  P2 HP: %u -> %u (Δ%d)", prev_state.p2_hp, current_state.p2_hp, (int)current_state.p2_hp - (int)prev_state.p2_hp);
            }
            if (current_state.p1_x != prev_state.p1_x || current_state.p1_y != prev_state.p1_y) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  P1 Position: (%u,%u) -> (%u,%u)", prev_state.p1_x, prev_state.p1_y, current_state.p1_x, current_state.p1_y);
            }
            if (current_state.p2_x != prev_state.p2_x || current_state.p2_y != prev_state.p2_y) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  P2 Position: (%u,%u) -> (%u,%u)", prev_state.p2_x, prev_state.p2_y, current_state.p2_x, current_state.p2_y);
            }
            if (current_state.round_timer != prev_state.round_timer) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  Timer: %u -> %u (Δ%d)", prev_state.round_timer, current_state.round_timer, (int)current_state.round_timer - (int)prev_state.round_timer);
            }
            if (current_state.random_seed != prev_state.random_seed) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  RNG: 0x%08X -> 0x%08X", prev_state.random_seed, current_state.random_seed);
            }
        }
        
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load current MinimalGameState for analysis");
    }
    
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "===============================================");
}

// Generate detailed desync report
static void GenerateDesyncReport(uint32_t desync_frame, uint32_t local_checksum, uint32_t remote_checksum) {
    char report_filename[128];
    snprintf(report_filename, sizeof(report_filename), "FM2K_DesyncReport_Client%d_Frame%u.txt", player_index + 1, desync_frame);
    
    std::ofstream report_file(report_filename);
    if (!report_file.is_open()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create desync report: %s", report_filename);
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm* tm_info = localtime(&time_t);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    report_file << "=== FM2K DESYNC REPORT ===" << std::endl;
    report_file << "Timestamp: " << timestamp << std::endl;
    report_file << "Player Index: " << (int)player_index << std::endl;
    report_file << "Is Host: " << (is_host ? "Yes" : "No") << std::endl;
    report_file << "Desync Frame: " << desync_frame << std::endl;
    report_file << "Local Checksum: 0x" << std::hex << local_checksum << std::endl;
    report_file << "Remote Checksum: 0x" << std::hex << remote_checksum << std::endl;
    report_file << std::dec;
    report_file << std::endl;
    
    // Game state at time of desync
    report_file << "=== GAME STATE AT DESYNC ===" << std::endl;
    
    uint32_t* p1_hp_ptr = (uint32_t*)FM2K::State::Memory::P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)FM2K::State::Memory::P2_HP_ADDR;
    uint32_t* frame_ptr = (uint32_t*)FM2K::State::Memory::FRAME_NUMBER_ADDR;
    uint32_t* p1_input_ptr = (uint32_t*)FM2K::State::Memory::P1_INPUT_ADDR;
    uint32_t* p2_input_ptr = (uint32_t*)FM2K::State::Memory::P2_INPUT_ADDR;
    
    if (p1_hp_ptr && !IsBadReadPtr(p1_hp_ptr, 4)) {
        report_file << "P1 HP: " << *p1_hp_ptr << std::endl;
    }
    if (p2_hp_ptr && !IsBadReadPtr(p2_hp_ptr, 4)) {
        report_file << "P2 HP: " << *p2_hp_ptr << std::endl;
    }
    if (frame_ptr && !IsBadReadPtr(frame_ptr, 4)) {
        report_file << "Game Frame: " << *frame_ptr << std::endl;
    }
    if (p1_input_ptr && !IsBadReadPtr(p1_input_ptr, 4)) {
        report_file << "P1 Memory Input: 0x" << std::hex << *p1_input_ptr << std::dec << std::endl;
    }
    if (p2_input_ptr && !IsBadReadPtr(p2_input_ptr, 4)) {
        report_file << "P2 Memory Input: 0x" << std::hex << *p2_input_ptr << std::dec << std::endl;
    }
    
    report_file << "P1 Live Input: 0x" << std::hex << live_p1_input << std::dec << std::endl;
    report_file << "P2 Live Input: 0x" << std::hex << live_p2_input << std::dec << std::endl;
    report_file << std::endl;
    
    // Rollback statistics
    report_file << "=== ROLLBACK STATISTICS ===" << std::endl;
    report_file << "Total Rollbacks: " << rollback_count << std::endl;
    report_file << "Max Rollback Frames: " << max_rollback_frames << std::endl;
    if (rollback_count > 0) {
        report_file << "Average Rollback Frames: " << ((float)total_rollback_frames / rollback_count) << std::endl;
    }
    report_file << std::endl;
    
    report_file << "=== INSTRUCTIONS ===" << std::endl;
    report_file << "1. Compare this report with the other client's report" << std::endl;
    report_file << "2. Check input recording files: FM2K_InputRecord_Client1.dat, FM2K_InputRecord_Client2.dat" << std::endl;
    report_file << "3. Look for differences in HP, inputs, or frame counts" << std::endl;
    report_file << "4. Review recent rollback activity that may have caused divergence" << std::endl;
    
    report_file.close();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Desync report generated: %s", report_filename);
}

// Shared memory for configuration
static HANDLE shared_memory_handle = nullptr;
static void* shared_memory_data = nullptr;

// Enhanced state management with comprehensive memory capture
static FM2K::State::GameState saved_states[8];  // Ring buffer for 8 frames (rollback buffer)
static uint32_t current_state_index = 0;
static bool state_manager_initialized = false;

// Named save slots for manual save/load
static FM2K::State::GameState save_slots[8];     // 8 manual save slots
static bool slot_occupied[8] = {false};          // Track which slots have saves
// All saves now use optimized FastGameState - no need for profile tracking
static uint32_t last_auto_save_frame = 0;

// Per-slot buffers for large memory regions (each slot gets its own buffers)
static std::unique_ptr<uint8_t[]> slot_player_data_buffers[8];
static std::unique_ptr<uint8_t[]> slot_object_pool_buffers[8];

// Temporary buffers for rollback (shared)
static std::unique_ptr<uint8_t[]> rollback_player_data_buffer;
static std::unique_ptr<uint8_t[]> rollback_object_pool_buffer;
static bool large_buffers_allocated = false;

// MinimalGameState variables moved to top of file (before LogMinimalGameStateDesync function)  
static uint32_t minimal_state_index = 0;

// MinimalGameState method implementations
bool FM2K::MinimalGameState::LoadFromMemory() {
    // Read HP values
    uint32_t* p1_hp_ptr = (uint32_t*)0x47010C;  // P1_HP_ADDR
    uint32_t* p2_hp_ptr = (uint32_t*)0x47030C;  // P2_HP_ADDR
    uint32_t* p1_max_hp_ptr = (uint32_t*)0x4DFC85;  // P1_MAX_HP_ARTMONEY_ADDR
    uint32_t* p2_max_hp_ptr = (uint32_t*)0x4EDC4;   // P2_MAX_HP_ARTMONEY_ADDR
    
    if (!p1_hp_ptr || !p2_hp_ptr || !p1_max_hp_ptr || !p2_max_hp_ptr) return false;
    
    p1_hp = *p1_hp_ptr;
    p2_hp = *p2_hp_ptr;
    p1_max_hp = *p1_max_hp_ptr;
    p2_max_hp = *p2_max_hp_ptr;
    
    // Read positions
    uint32_t* p1_x_ptr = (uint32_t*)0x4ADCC3;  // P1_COORD_X_ADDR
    uint16_t* p1_y_ptr = (uint16_t*)0x4ADCC7;  // P1_COORD_Y_ADDR
    uint32_t* p2_x_ptr = (uint32_t*)0x4EDD02;  // P2_COORD_X_ADDR
    uint16_t* p2_y_ptr = (uint16_t*)0x4EDD06;  // P2_COORD_Y_ADDR
    
    if (!p1_x_ptr || !p1_y_ptr || !p2_x_ptr || !p2_y_ptr) return false;
    
    p1_x = *p1_x_ptr;
    p1_y = *p1_y_ptr;
    p2_x = *p2_x_ptr;
    p2_y = *p2_y_ptr;
    
    // Read timers and RNG
    uint32_t* timer_ptr = (uint32_t*)0x470044;  // ROUND_TIMER_ADDR
    uint32_t* rng_ptr = (uint32_t*)0x41FB1C;    // RANDOM_SEED_ADDR
    
    if (!timer_ptr || !rng_ptr) return false;
    
    round_timer = *timer_ptr;
    random_seed = *rng_ptr;
    
    return true;
}

bool FM2K::MinimalGameState::SaveToMemory() const {
    // Write HP values
    uint32_t* p1_hp_ptr = (uint32_t*)0x47010C;  // P1_HP_ADDR
    uint32_t* p2_hp_ptr = (uint32_t*)0x47030C;  // P2_HP_ADDR
    
    if (!p1_hp_ptr || !p2_hp_ptr) return false;
    
    *p1_hp_ptr = p1_hp;
    *p2_hp_ptr = p2_hp;
    
    // Write positions
    uint32_t* p1_x_ptr = (uint32_t*)0x4ADCC3;  // P1_COORD_X_ADDR
    uint16_t* p1_y_ptr = (uint16_t*)0x4ADCC7;  // P1_COORD_Y_ADDR
    uint32_t* p2_x_ptr = (uint32_t*)0x4EDD02;  // P2_COORD_X_ADDR
    uint16_t* p2_y_ptr = (uint16_t*)0x4EDD06;  // P2_COORD_Y_ADDR
    
    if (!p1_x_ptr || !p1_y_ptr || !p2_x_ptr || !p2_y_ptr) return false;
    
    *p1_x_ptr = p1_x;
    *p1_y_ptr = (uint16_t)p1_y;
    *p2_x_ptr = p2_x;
    *p2_y_ptr = (uint16_t)p2_y;
    
    // Write timers and RNG
    uint32_t* timer_ptr = (uint32_t*)0x470044;  // ROUND_TIMER_ADDR
    uint32_t* rng_ptr = (uint32_t*)0x41FB1C;    // RANDOM_SEED_ADDR
    
    if (!timer_ptr || !rng_ptr) return false;
    
    *timer_ptr = round_timer;
    *rng_ptr = random_seed;
    
    return true;
}

// Performance tracking
static uint32_t total_saves = 0;
static uint32_t total_loads = 0;
static uint64_t total_save_time_us = 0;
static uint64_t total_load_time_us = 0;

// High-resolution timing helper
static inline uint64_t get_microseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

// State change debugging
static FM2K::State::GameState last_core_state = {};
static bool last_core_state_valid = false;

// Shared memory structure matching the launcher
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
    // All saves now use optimized FastGameState automatically
    
    // Production mode settings
    bool production_mode;                // Enable production mode (reduced logging)
    bool enable_input_recording;         // Record inputs to file for testing
    
    // MinimalGameState testing mode
    bool use_minimal_gamestate_testing;  // Enable 48-byte minimal state for GekkoNet testing
    
    // Configuration versioning to force hook to re-read settings
    uint32_t config_version;  // Incremented each time configuration is updated
    
    // Slot status feedback to UI
    struct SlotInfo {
        bool occupied;
        uint32_t frame_number;
        uint64_t timestamp_ms;
        uint32_t checksum;
        uint32_t state_size_kb;  // Size in KB for analysis
        uint32_t save_time_us;   // Save time in microseconds
        uint32_t load_time_us;   // Load time in microseconds
    } slot_status[8];
    
    // Performance statistics
    struct PerformanceStats {
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
    } perf_stats;
    
    // GekkoNet client role coordination (simplified)
    uint8_t player_index;            // 0 for Player 1, 1 for Player 2
    uint8_t session_role;            // 0 = Host, 1 = Guest
};

// Generate unique log file path based on player index
static std::string GetLogFilePath() {
    // Use process ID to create unique log files per client
    DWORD process_id = GetCurrentProcessId();
    
    // Check if we have shared data to determine client role
    if (shared_memory_data) {
        SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
        uint8_t player_index = shared_data->player_index;
        uint8_t session_role = shared_data->session_role;
        
        // Create descriptive log file names
        const char* role_name = (session_role == 0) ? "host" : "client";
        return "C:\\Games\\fm2k_hook_" + std::string(role_name) + ".txt";
    }
    
    // Fallback using process ID if no shared memory yet
    return "C:\\Games\\fm2k_hook_pid" + std::to_string(process_id) + ".txt";
}

// Update rollback performance statistics in shared memory
static void UpdateRollbackStats(uint32_t frames_rolled_back) {
    if (!shared_memory_data) return;
    
    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    uint64_t current_time_us = get_microseconds();
    
    // Update basic rollback counters
    shared_data->perf_stats.rollback_count = rollback_count;
    shared_data->perf_stats.max_rollback_frames = max_rollback_frames;
    shared_data->perf_stats.total_rollback_frames = total_rollback_frames;
    shared_data->perf_stats.avg_rollback_frames = rollback_count > 0 ? (total_rollback_frames / rollback_count) : 0;
    shared_data->perf_stats.last_rollback_time_us = current_time_us;
    
    // Track rollbacks per second (sliding window)
    uint64_t current_second = current_time_us / 1000000;  // Convert to seconds
    uint64_t shared_second = shared_data->perf_stats.current_second_start / 1000000;
    
    if (current_second != shared_second) {
        // New second - reset counter
        shared_data->perf_stats.rollbacks_this_second = 1;
        shared_data->perf_stats.current_second_start = current_time_us;
    } else {
        // Same second - increment counter
        shared_data->perf_stats.rollbacks_this_second++;
    }
}

// Simple hook function types (matching FM2K patterns)
typedef int (__cdecl *ProcessGameInputsFn)();
typedef int (__cdecl *UpdateGameStateFn)();
typedef BOOL (__cdecl *RunGameLoopFn)();
typedef int (__cdecl *GetPlayerInputFn)(int player_id, int input_type);

// Original function pointers
static ProcessGameInputsFn original_process_inputs = nullptr;
static UpdateGameStateFn original_update_game = nullptr;
static RunGameLoopFn original_run_game_loop = nullptr;
static GetPlayerInputFn original_get_player_input = nullptr;

// Hook state
static uint32_t g_frame_counter = 0;

// Key FM2K addresses (from IDA analysis)
static constexpr uintptr_t PROCESS_INPUTS_ADDR = 0x4146D0;
static constexpr uintptr_t GET_PLAYER_INPUT_ADDR = 0x414340;
static constexpr uintptr_t UPDATE_GAME_ADDR = 0x404CD0;
static constexpr uintptr_t RUN_GAME_LOOP_ADDR = 0x405AD0;  // run_game_loop function (perfect post-graphics hook point)
static constexpr uintptr_t FRAME_COUNTER_ADDR = 0x447EE0;

// Input buffer addresses (correct addresses from IDA analysis)
static constexpr uintptr_t P1_INPUT_ADDR = 0x4259C0;  // g_p1_input[0]
static constexpr uintptr_t P2_INPUT_ADDR = 0x4259C4;  // g_p2_input

// Enhanced state memory addresses (from save state documentation)
static constexpr uintptr_t P1_HP_ADDR = 0x47010C;
static constexpr uintptr_t P2_HP_ADDR = 0x47030C;
static constexpr uintptr_t ROUND_TIMER_ADDR = 0x470060;
static constexpr uintptr_t GAME_TIMER_ADDR = 0x470044;
static constexpr uintptr_t RANDOM_SEED_ADDR = 0x41FB1C;

// Major memory regions for comprehensive state capture
static constexpr uintptr_t PLAYER_DATA_SLOTS_ADDR = 0x4D1D80;  // g_player_data_slots
static constexpr size_t PLAYER_DATA_SLOTS_SIZE = 0x701F8;      // 459,256 bytes
static constexpr uintptr_t GAME_OBJECT_POOL_ADDR = 0x4701E0;   // g_game_object_pool  
static constexpr size_t GAME_OBJECT_POOL_SIZE = 0x5F800;       // 391,168 bytes (1024 * 382)

// Additional game state variables from documentation
static constexpr uintptr_t GAME_MODE_ADDR = 0x470054;          // g_game_mode
static constexpr uintptr_t ROUND_SETTING_ADDR = 0x470068;      // g_round_setting
static constexpr uintptr_t P1_ROUND_COUNT_ADDR = 0x4700EC;     // g_p1_round_count
static constexpr uintptr_t P1_ROUND_STATE_ADDR = 0x4700F0;     // g_p1_round_state
static constexpr uintptr_t P1_ACTION_STATE_ADDR = 0x47019C;    // g_p1_action_state
static constexpr uintptr_t P2_ACTION_STATE_ADDR = 0x4701A0;    // g_p2_action_state (estimated)
static constexpr uintptr_t CAMERA_X_ADDR = 0x447F2C;          // g_camera_x
static constexpr uintptr_t CAMERA_Y_ADDR = 0x447F30;          // g_camera_y
static constexpr uintptr_t TIMER_COUNTDOWN1_ADDR = 0x4456E4;   // g_timer_countdown1
static constexpr uintptr_t TIMER_COUNTDOWN2_ADDR = 0x447D91;   // g_timer_countdown2

// Object list management (critical for object pool iteration)
static constexpr uintptr_t OBJECT_LIST_HEADS_ADDR = 0x430240;  // g_object_list_heads
static constexpr uintptr_t OBJECT_LIST_TAILS_ADDR = 0x430244;  // g_object_list_tails

// Additional timer that may be the in-game timer (needs verification)
static constexpr uintptr_t ROUND_TIMER_COUNTER_ADDR = 0x424F00; // g_round_timer_counter

// Simple Fletcher32 implementation for checksums
namespace FM2K {
namespace State {
uint32_t Fletcher32(const uint8_t* data, size_t len) {
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    size_t blocks = len / 2;

    // Process 2-byte blocks
    while (blocks) {
        size_t tlen = blocks > 359 ? 359 : blocks;
        blocks -= tlen;
        do {
            sum1 += (data[0] << 8) | data[1];
            sum2 += sum1;
            data += 2;
        } while (--tlen);

        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }

    // Handle remaining byte if length is odd
    if (len & 1) {
        sum1 += *data << 8;
        sum2 += sum1;
        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }

    // Final reduction
    sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
    sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);

    return (sum2 << 16) | sum1;
}
} // namespace State
} // namespace FM2K

// Enhanced object analysis and selective saving
struct ActiveObjectInfo {
    uint32_t index;
    uint32_t type_or_id;
    bool is_active;
};

// ============================================================================
// GAME STATE DETECTION: Object Function Table Analysis
// ============================================================================

// Object function table indices (verified with IDA MCP @ 0x41ED58)
enum class ObjectFunctionIndex : uint32_t {
    NULLSUB_1 = 0,                              // 0x406990 - nullsub_1
    RESET_SPRITE_EFFECT = 1,                    // 0x4069A0 - ResetSpriteEffect
    GAME_INITIALIZE = 2,                        // 0x409A60 - Game_Initialize  
    CAMERA_MANAGER = 3,                         // 0x40AF30 - camera_manager
    CHARACTER_STATE_MACHINE = 4,                // 0x411BF0 - character_state_machine_ScriptMainLoop
    UPDATE_SCREEN_FADE = 5,                     // 0x40AC60 - UpdateScreenFade
    SCORE_DISPLAY_SYSTEM = 6,                   // 0x40A620 - score_display_system
    DISPLAY_SCORE = 7,                          // 0x40AB10 - DisplayScore
    UPDATE_TRANSITION_EFFECT = 8,               // 0x406CF0 - UpdateTransitionEffect
    INITIALIZE_SCREEN_TRANSITION = 9,           // 0x406D90 - InitializeScreenTransition
    GAME_STATE_MANAGER = 10,                    // 0x406FC0 - game_state_manager
    INITIALIZE_SCREEN_TRANSITION_ALT = 11,      // 0x406E50 - InitializeScreenTransition_Alt
    HANDLE_MAIN_MENU_AND_CHARACTER_SELECT = 12, // 0x408080 - handle_main_menu_and_character_select
    UPDATE_MAIN_MENU = 13,                      // 0x4084F0 - UpdateMainMenu
    VS_ROUND_FUNCTION = 14,                     // 0x4086A0 - vs_round_function
    UI_STATE_MANAGER = 15,                      // 0x409D00 - ui_state_manager
    MAX_FUNCTION_INDEX = 32
};

// Game states based on active object functions
enum class GameState : uint32_t {
    BOOT_SPLASH,        // 1-3 objects, minimal functions
    TITLE_SCREEN,       // title_screen_manager active
    MAIN_MENU,          // update_main_menu active
    CHARACTER_SELECT,   // update_post_char_select active
    INTRO_LOADING,      // update_intro_sequence active
    IN_GAME,            // character_state_machine active
    TRANSITION,         // transition effects active
    UNKNOWN
};

// ============================================================================
// ENHANCED OBJECT LIFECYCLE TRACKING
// ============================================================================

// Multi-frame object change tracking for dynamic behavior analysis
struct ObjectChangeTracker {
    uint32_t previous_active_mask[32];    // Previous frame's active objects
    uint32_t current_active_mask[32];     // Current frame's active objects
    uint32_t created_objects[32];         // Objects created this frame
    uint32_t destroyed_objects[32];       // Objects destroyed this frame
    uint32_t stable_objects[32];          // Objects unchanged for >N frames
    
    uint32_t frame_count;
    uint32_t creation_rate;               // Objects created per second
    uint32_t destruction_rate;            // Objects destroyed per second
    
    // Activity patterns for character state machines
    uint32_t stable_character_objects;    // Characters unchanged >60 frames (preview)
    uint32_t volatile_character_objects;  // Characters changing frequently (combat)
    
    // Stability tracking
    uint32_t frames_since_last_change;    // Frames since last object pool change
    bool objects_stable;                  // No changes for >STABILITY_THRESHOLD frames
    
    void Reset() {
        memset(this, 0, sizeof(ObjectChangeTracker));
    }
};

// Game state context integration with verified addresses
struct GameStateContext {
    // Core game state (IDA verified addresses)
    uint32_t game_mode;           // From 0x470054 (verified: 2000=char select, 3000=combat)
    uint32_t round_timer;         // From 0x470044 (verified)
    uint32_t game_timer;          // From 0x470060 (verified)
    uint32_t p1_hp, p2_hp;        // From 0x47010C, 0x47030C (verified)
    
    // Derived state indicators
    bool in_combat;               // HP changing frame-to-frame
    bool timer_running;           // Round timer decrementing
    uint32_t input_activity;      // Input changes per second
    bool objects_stable;          // No object creation/destruction for >N frames
    
    // Previous frame values for change detection
    uint32_t prev_p1_hp, prev_p2_hp;
    uint32_t prev_round_timer;
    uint32_t prev_game_timer;
    
    void UpdateFromMemory() {
        // Store previous values
        prev_p1_hp = p1_hp;
        prev_p2_hp = p2_hp;
        prev_round_timer = round_timer;
        prev_game_timer = game_timer;
        
        // Read current values
        game_mode = *(uint32_t*)0x470054;
        round_timer = *(uint32_t*)0x470044;
        game_timer = *(uint32_t*)0x470060;
        p1_hp = *(uint32_t*)0x47010C;
        p2_hp = *(uint32_t*)0x47030C;
        
        // Calculate derived states
        in_combat = (p1_hp != prev_p1_hp) || (p2_hp != prev_p2_hp);
        timer_running = (round_timer != prev_round_timer) || (game_timer != prev_game_timer);
    }
};

// Enhanced object lifecycle tracking globals (declared after struct definitions)
static ObjectChangeTracker g_object_tracker;
static GameStateContext g_game_context;
static bool g_enhanced_tracking_initialized = false;

// Analysis of currently active object functions
struct ActiveFunctionAnalysis {
    uint32_t total_objects;
    uint32_t function_counts[32];    // Count of objects using each function
    bool has_title_screen_manager;
    bool has_main_menu;
    bool has_character_select;
    bool has_intro_sequence;
    bool has_character_state_machine;
    bool has_transition_effects;
    GameState detected_state;
};

// ============================================================================
// PHASE 1: Fast & Performant Save State Implementation
// ============================================================================

// Optimized game state structure for high-performance rollback
struct FastGameState {
    // Core deterministic state (32 bytes) - existing minimal approach
    uint32_t deterministic_core[8];
    
    // Fast object tracking with bitfield compression
    uint32_t active_object_mask[32];  // 1024 bits = 32 uint32_t (bitfield)
    uint16_t active_object_count;     // Number of active objects (usually 5-20)
    uint16_t reserved;                // Padding for alignment
    
    // Variable size packed object data follows this header
    // Layout: [FastGameState header][packed active objects data]
    // Each packed object: [uint16_t object_index][382 bytes object_data]
};

// Fast object pool scanning using optimized pattern detection
uint32_t FastScanActiveObjects(uint32_t* active_mask, uint16_t* active_count) {
    if (!active_mask || !active_count) return 0;
    
    *active_count = 0;
    memset(active_mask, 0, 128); // Clear 32 uint32_t = 128 bytes
    
    uint8_t* object_pool = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (IsBadReadPtr(object_pool, GAME_OBJECT_POOL_SIZE)) {
        return 0;
    }
    
    // Fast bulk scanning - check 4 objects at once using DWORD alignment
    uint32_t* pool_dwords = (uint32_t*)object_pool;
    const uint32_t objects_per_scan = 4;  // 382*4 = 1528 bytes, but we check headers
    
    for (uint32_t i = 0; i < 1024; i++) {
        // Calculate object offset (382 bytes per object)
        uint32_t object_offset = i * 382;
        uint32_t* object_header = (uint32_t*)(object_pool + object_offset);
        
        if (!IsBadReadPtr(object_header, 8)) { // Check first 8 bytes
            uint32_t first_dword = *object_header;
            uint32_t second_dword = *(object_header + 1);
            
            // Correct empty detection: FM2K sets object type (first DWORD) to 0 for inactive objects
            bool is_empty = (first_dword == 0);
            
            if (!is_empty) {
                // Object appears active - set bit in mask
                uint32_t mask_index = i >> 5;          // i / 32
                uint32_t bit_index = i & 31;           // i % 32
                active_mask[mask_index] |= (1U << bit_index);
                (*active_count)++;
            }
        }
    }
    
    return *active_count;
}

// Analyze active object functions to determine game state
bool AnalyzeActiveObjectFunctions(ActiveFunctionAnalysis* analysis) {
    if (!analysis) return false;
    
    // Initialize analysis structure
    memset(analysis, 0, sizeof(ActiveFunctionAnalysis));
    
    uint8_t* object_pool = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (IsBadReadPtr(object_pool, GAME_OBJECT_POOL_SIZE)) {
        return false;
    }
    
    // Scan all 1024 objects for their types/function indices
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t object_offset = i * 382;
        uint32_t* object_header = (uint32_t*)(object_pool + object_offset);
        
        if (!IsBadReadPtr(object_header, 4)) {
            uint32_t object_type = *object_header;
            
            // Object is active if type != 0 AND type != 0xFFFFFFFF (uninitialized)
            if (object_type != 0 && object_type != 0xFFFFFFFF) {
                analysis->total_objects++;
                
                // Minimal debug logging: only log summary stats at specific intervals
                // Individual object logging removed for performance
                
                // Ensure function index is within bounds
                if (object_type < 32) {
                    analysis->function_counts[object_type]++;
                    
                    // Check for specific game state indicators (using verified function indices)
                    switch (object_type) {
                        case static_cast<uint32_t>(ObjectFunctionIndex::CHARACTER_STATE_MACHINE):
                            analysis->has_character_state_machine = true;
                            break;
                        case static_cast<uint32_t>(ObjectFunctionIndex::HANDLE_MAIN_MENU_AND_CHARACTER_SELECT):
                            analysis->has_main_menu = true;
                            analysis->has_character_select = true;  // This function handles both
                            break;
                        case static_cast<uint32_t>(ObjectFunctionIndex::UPDATE_MAIN_MENU):
                            analysis->has_main_menu = true;
                            break;
                        case static_cast<uint32_t>(ObjectFunctionIndex::GAME_INITIALIZE):
                            analysis->has_intro_sequence = true;
                            break;
                        case static_cast<uint32_t>(ObjectFunctionIndex::VS_ROUND_FUNCTION):
                            analysis->has_character_state_machine = true;  // VS rounds = in-game
                            break;
                        case static_cast<uint32_t>(ObjectFunctionIndex::UI_STATE_MANAGER):
                            // UI state manager could be menus or in-game UI
                            break;
                        case static_cast<uint32_t>(ObjectFunctionIndex::CAMERA_MANAGER):
                            // Camera manager typically active during gameplay
                            break;
                        case static_cast<uint32_t>(ObjectFunctionIndex::RESET_SPRITE_EFFECT):
                        case static_cast<uint32_t>(ObjectFunctionIndex::UPDATE_TRANSITION_EFFECT):
                        case static_cast<uint32_t>(ObjectFunctionIndex::INITIALIZE_SCREEN_TRANSITION):
                        case static_cast<uint32_t>(ObjectFunctionIndex::INITIALIZE_SCREEN_TRANSITION_ALT):
                        case static_cast<uint32_t>(ObjectFunctionIndex::UPDATE_SCREEN_FADE):
                            analysis->has_transition_effects = true;
                            break;
                        case static_cast<uint32_t>(ObjectFunctionIndex::SCORE_DISPLAY_SYSTEM):
                        case static_cast<uint32_t>(ObjectFunctionIndex::DISPLAY_SCORE):
                            // Score display typically during/after matches
                            break;
                        case static_cast<uint32_t>(ObjectFunctionIndex::GAME_STATE_MANAGER):
                            // General game state management
                            break;
                    }
                }
            }
        }
    }
    
    return true;
}

// ============================================================================
// ENHANCED OBJECT CHANGE TRACKING FUNCTIONS
// ============================================================================

// Update object change tracking with current frame data
void UpdateObjectChangeTracking(ObjectChangeTracker* tracker, const uint32_t* current_mask, uint16_t active_count) {
    if (!tracker || !current_mask) return;
    
    tracker->frame_count++;
    
    // Copy previous to current
    memcpy(tracker->previous_active_mask, tracker->current_active_mask, sizeof(tracker->current_active_mask));
    memcpy(tracker->current_active_mask, current_mask, sizeof(tracker->current_active_mask));
    
    // Calculate created and destroyed objects
    bool any_changes = false;
    for (int i = 0; i < 32; i++) {
        // Created: current & !previous
        tracker->created_objects[i] = tracker->current_active_mask[i] & ~tracker->previous_active_mask[i];
        
        // Destroyed: previous & !current
        tracker->destroyed_objects[i] = tracker->previous_active_mask[i] & ~tracker->current_active_mask[i];
        
        // Stable: current & previous
        tracker->stable_objects[i] = tracker->current_active_mask[i] & tracker->previous_active_mask[i];
        
        if (tracker->created_objects[i] || tracker->destroyed_objects[i]) {
            any_changes = true;
        }
    }
    
    // Update stability tracking
    if (any_changes) {
        tracker->frames_since_last_change = 0;
        tracker->objects_stable = false;
    } else {
        tracker->frames_since_last_change++;
        tracker->objects_stable = (tracker->frames_since_last_change >= STABILITY_THRESHOLD_FRAMES);
    }
    
    // Calculate creation/destruction rates (objects per second, assuming 60 FPS)
    if (tracker->frame_count > 0) {
        uint32_t created_count = 0, destroyed_count = 0;
        for (int i = 0; i < 32; i++) {
            created_count += __builtin_popcount(tracker->created_objects[i]);
            destroyed_count += __builtin_popcount(tracker->destroyed_objects[i]);
        }
        
        // Simple moving average over last 60 frames
        float time_window = std::min(tracker->frame_count, 60u) / 60.0f;  // seconds
        tracker->creation_rate = static_cast<uint32_t>(created_count / time_window);
        tracker->destruction_rate = static_cast<uint32_t>(destroyed_count / time_window);
    }
}

// Analyze character state machine stability
void AnalyzeCharacterObjectStability(ObjectChangeTracker* tracker, const ActiveFunctionAnalysis& functions) {
    if (!tracker) return;
    
    tracker->stable_character_objects = 0;
    tracker->volatile_character_objects = 0;
    
    // Count CHARACTER_STATE_MACHINE objects by stability
    uint32_t char_state_type = static_cast<uint32_t>(ObjectFunctionIndex::CHARACTER_STATE_MACHINE);
    
    uint8_t* object_pool = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (IsBadReadPtr(object_pool, GAME_OBJECT_POOL_SIZE)) return;
    
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t* object_header = (uint32_t*)(object_pool + (i * 382));
        if (!IsBadReadPtr(object_header, 4)) {
            uint32_t object_type = *object_header;
            
            if (object_type == char_state_type) {
                uint32_t mask_index = i >> 5;
                uint32_t bit_index = i & 31;
                uint32_t bit_mask = 1U << bit_index;
                
                // Check if this character object is stable
                if (tracker->stable_objects[mask_index] & bit_mask) {
                    tracker->stable_character_objects++;
                } else if (tracker->current_active_mask[mask_index] & bit_mask) {
                    tracker->volatile_character_objects++;
                }
            }
        }
    }
}

// Enhanced combat detection using multiple indicators
bool IsActiveCombat(const GameStateContext& context, const ObjectChangeTracker& tracker) {
    // Multiple indicators for robust detection
    bool game_mode_combat = (context.game_mode >= 3000);  // Combat modes are 3000+
    bool timer_active = context.timer_running;
    bool health_changing = context.in_combat;
    bool objects_volatile = (tracker.creation_rate > COMBAT_CREATION_THRESHOLD || 
                           tracker.destruction_rate > COMBAT_CREATION_THRESHOLD);
    bool characters_active = (tracker.volatile_character_objects > 0);
    bool objects_unstable = !tracker.objects_stable;
    
    // Require multiple indicators for combat classification
    return (game_mode_combat || 
            (timer_active && (health_changing || objects_volatile)) ||
            (characters_active && objects_unstable));
}

// Advanced game state detection with multi-dimensional analysis
GameState DetectGameStateAdvanced(const ActiveFunctionAnalysis& functions, 
                                 const GameStateContext& context,
                                 const ObjectChangeTracker& tracker) {
    // Priority 1: Game mode-based primary classification
    if (context.game_mode >= 3000) {
        // Combat modes - verify with additional checks
        if (functions.has_character_state_machine && IsActiveCombat(context, tracker)) {
            return GameState::IN_GAME;
        }
    } else if (context.game_mode >= 2000) {
        // Character select mode (2000-2999)
        if (functions.has_character_state_machine && !IsActiveCombat(context, tracker)) {
            return GameState::CHARACTER_SELECT;  // Real character select with preview models
        }
    } else if (context.game_mode >= 1000) {
        // Title/menu modes (1000-1999) - CHARACTER_STATE_MACHINE here is for menu effects
        if (functions.has_main_menu || functions.has_character_select) {
            return GameState::TITLE_SCREEN;  // Title screen with menu functions
        }
        if (functions.total_objects <= 5) {
            return GameState::BOOT_SPLASH;  // Minimal objects = boot/splash
        }
        return GameState::MAIN_MENU;  // General menu/title state
    }
    
    // Priority 2: Fallback for unknown game modes - use object-based detection
    if (functions.has_character_state_machine && IsActiveCombat(context, tracker)) {
        return GameState::IN_GAME;  // Active combat regardless of mode
    }
    
    // Priority 3: Loading/initialization
    if (functions.has_intro_sequence) {
        return GameState::INTRO_LOADING;  
    }
    
    // Priority 4: Transition detection (high object volatility)
    if (tracker.creation_rate > TRANSITION_THRESHOLD || 
        tracker.destruction_rate > TRANSITION_THRESHOLD ||
        functions.has_transition_effects) {
        return GameState::TRANSITION;  
    }
    
    // Priority 5: Boot/splash (minimal objects)
    if (functions.total_objects <= 5) {
        return GameState::BOOT_SPLASH;  
    }
    
    return GameState::UNKNOWN;
}

// Legacy function for backward compatibility
GameState DetectGameStateFromFunctions(const ActiveFunctionAnalysis& analysis) {
    // Use enhanced detection with default context if available
    if (g_enhanced_tracking_initialized) {
        return DetectGameStateAdvanced(analysis, g_game_context, g_object_tracker);
    }
    
    // Fallback to simple detection
    if (analysis.has_character_state_machine) {
        return GameState::IN_GAME;  
    }
    if (analysis.has_main_menu || analysis.has_character_select) {
        if (analysis.total_objects > 20) {
            return GameState::CHARACTER_SELECT;  
        } else {
            return GameState::MAIN_MENU;  
        }
    }
    if (analysis.has_intro_sequence) {
        return GameState::INTRO_LOADING;  
    }
    if (analysis.has_transition_effects) {
        return GameState::TRANSITION;  
    }
    if (analysis.total_objects <= 5) {
        return GameState::BOOT_SPLASH;  
    }
    
    return GameState::UNKNOWN;
}

// Pack active objects into compressed buffer
bool PackActiveObjects(const uint32_t* active_mask, uint16_t active_count, 
                      uint8_t* packed_buffer, size_t buffer_size, size_t* bytes_used) {
    if (!active_mask || !packed_buffer || !bytes_used || active_count == 0) {
        *bytes_used = 0;
        return true; // No objects to pack is valid
    }
    
    uint8_t* object_pool = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (IsBadReadPtr(object_pool, GAME_OBJECT_POOL_SIZE)) {
        return false;
    }
    
    size_t required_size = active_count * (sizeof(uint16_t) + 382); // index + object data
    if (required_size > buffer_size) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Pack buffer too small: need %zu bytes, have %zu", 
                    required_size, buffer_size);
        return false;
    }
    
    uint8_t* write_ptr = packed_buffer;
    uint32_t packed_count = 0;
    
    // Iterate through bitfield to find active objects
    for (uint32_t i = 0; i < 1024 && packed_count < active_count; i++) {
        uint32_t mask_index = i >> 5;
        uint32_t bit_index = i & 31;
        
        if (active_mask[mask_index] & (1U << bit_index)) {
            // This object is active - pack it
            *(uint16_t*)write_ptr = (uint16_t)i;  // Store object index
            write_ptr += sizeof(uint16_t);
            
            // Copy object data (382 bytes)
            uint8_t* object_data = object_pool + (i * 382);
            if (!IsBadReadPtr(object_data, 382)) {
                memcpy(write_ptr, object_data, 382);
                write_ptr += 382;
                packed_count++;
            } else {
                // Skip bad object, adjust write pointer back
                write_ptr -= sizeof(uint16_t);
            }
        }
    }
    
    *bytes_used = write_ptr - packed_buffer;
    return true;
}

// Unpack active objects from compressed buffer
bool UnpackActiveObjects(const uint8_t* packed_buffer, size_t buffer_size, 
                        uint16_t active_count) {
    if (!packed_buffer || active_count == 0) {
        return true; // No objects to unpack is valid
    }
    
    uint8_t* object_pool = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (IsBadWritePtr(object_pool, GAME_OBJECT_POOL_SIZE)) {
        return false;
    }
    
    // First, clear the entire object pool with 0xFF (empty marker)
    memset(object_pool, 0xFF, GAME_OBJECT_POOL_SIZE);
    
    const uint8_t* read_ptr = packed_buffer;
    const uint8_t* buffer_end = packed_buffer + buffer_size;
    
    for (uint16_t i = 0; i < active_count; i++) {
        if (read_ptr + sizeof(uint16_t) + 382 > buffer_end) {
            break; // Buffer overflow protection
        }
        
        // Read object index
        uint16_t object_index = *(const uint16_t*)read_ptr;
        read_ptr += sizeof(uint16_t);
        
        if (object_index >= 1024) {
            break; // Invalid index
        }
        
        // Restore object data
        uint8_t* object_dest = object_pool + (object_index * 382);
        if (!IsBadWritePtr(object_dest, 382)) {
            memcpy(object_dest, read_ptr, 382);
        }
        read_ptr += 382;
    }
    
    return true;
}

// High-performance save state using FastGameState structure
bool SaveStateFast(FM2K::State::GameState* state, uint32_t frame_number) {
    if (!state) return false;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 1. Save core deterministic state (32 bytes)
    if (!SaveCoreStateBasic(state, frame_number)) {
        return false;
    }
    
    // 2. Create FastGameState structure for optimized object storage
    FastGameState* fast_state = reinterpret_cast<FastGameState*>(rollback_object_pool_buffer.get());
    if (!fast_state) {
        return false;
    }
    
    // Copy core state to fast state structure
    memcpy(fast_state->deterministic_core, &state->core, sizeof(state->core));
    
    // 3. Initialize enhanced tracking if not already done
    if (!g_enhanced_tracking_initialized) {
        g_object_tracker.Reset();
        g_game_context = {}; // Zero-initialize
        g_enhanced_tracking_initialized = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Enhanced object tracking initialized");
    }
    
    // 4. Update game state context from memory
    g_game_context.UpdateFromMemory();
    
    // 5. Perform object function analysis and get preliminary object scan
    ActiveFunctionAnalysis function_analysis;
    bool analysis_success = AnalyzeActiveObjectFunctions(&function_analysis);
    
    // 6. Scan active objects for change tracking
    uint16_t active_count = 0;
    uint32_t scan_result = FastScanActiveObjects(fast_state->active_object_mask, &active_count);
    
    // 7. Update object change tracking
    UpdateObjectChangeTracking(&g_object_tracker, fast_state->active_object_mask, active_count);
    AnalyzeCharacterObjectStability(&g_object_tracker, function_analysis);
    
    // 8. Advanced game state detection using all available data
    GameState current_game_state = GameState::UNKNOWN;
    if (analysis_success) {
        current_game_state = DetectGameStateAdvanced(function_analysis, g_game_context, g_object_tracker);
        
        // State transition logging: only log when state changes
        static GameState previous_game_state = GameState::UNKNOWN;
        if (current_game_state != previous_game_state) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "STATE TRANSITION: %s -> %s", 
                       (previous_game_state == GameState::IN_GAME) ? "IN_GAME" :
                       (previous_game_state == GameState::CHARACTER_SELECT) ? "CHARACTER_SELECT" :
                       (previous_game_state == GameState::TITLE_SCREEN) ? "TITLE_SCREEN" :
                       (previous_game_state == GameState::MAIN_MENU) ? "MAIN_MENU" : "OTHER",
                       (current_game_state == GameState::IN_GAME) ? "IN_GAME" :
                       (current_game_state == GameState::CHARACTER_SELECT) ? "CHARACTER_SELECT" :
                       (current_game_state == GameState::TITLE_SCREEN) ? "TITLE_SCREEN" :
                       (current_game_state == GameState::MAIN_MENU) ? "MAIN_MENU" : "OTHER");
            previous_game_state = current_game_state;
        }
        
        // Minimal state logging: only on transitions or every 1000 frames
        static uint32_t last_log_frame = 0;
        if (current_game_state != previous_game_state || (g_frame_counter - last_log_frame) > 1000) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "State: %s (%u objs)", 
                       (current_game_state == GameState::IN_GAME) ? "IN_GAME" :
                       (current_game_state == GameState::CHARACTER_SELECT) ? "CHARACTER_SELECT" :
                       (current_game_state == GameState::TITLE_SCREEN) ? "TITLE_SCREEN" : "OTHER",
                       function_analysis.total_objects);
            last_log_frame = g_frame_counter;
        }
    }
    
    // 9. Adaptive save strategy based on detected game state
    bool use_full_objects = false;
    const char* save_strategy = "core-only";
    
    switch (current_game_state) {
        case GameState::IN_GAME:
            use_full_objects = true;
            save_strategy = "full-objects";
            break;
        case GameState::CHARACTER_SELECT:
            use_full_objects = (function_analysis.total_objects <= 100);  // Light object save
            save_strategy = use_full_objects ? "light-objects" : "core-only";
            break;
        case GameState::BOOT_SPLASH:
        case GameState::TITLE_SCREEN:
        case GameState::MAIN_MENU:
        case GameState::TRANSITION:
        case GameState::INTRO_LOADING:
        case GameState::UNKNOWN:
        default:
            use_full_objects = false;  // Core-only for menus/transitions
            save_strategy = "core-only";
            break;
    }
    
    // Save strategy logging removed for minimal output
    
    // 10. Execute save strategy
    if (!use_full_objects) {
        // Core-only save for menus/transitions
        fast_state->active_object_count = 0;
        memset(fast_state->active_object_mask, 0, sizeof(fast_state->active_object_mask));
        size_t total_state_size = sizeof(FastGameState);
        state->checksum = FM2K::State::Fletcher32((const uint8_t*)fast_state, total_state_size);
        state->timestamp_ms = SDL_GetTicks();
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        // Minimal save logging for core-only saves
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "SAVE: %s, %zu bytes, %.1f ms", 
                    save_strategy, total_state_size, duration_us / 1000.0f);
        return true;
    }
    
    // 11. Object packing for full/light saves (reuse already scanned data)
    fast_state->active_object_count = active_count;
    fast_state->reserved = 0;
    
    // Check if detection looks reasonable (0-200 active objects typical for gameplay)
    if (active_count > 200) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Suspicious object count (%u), falling back to core-only save", active_count);
        // Fall back to core-only save when object detection seems wrong
        fast_state->active_object_count = 0;
        memset(fast_state->active_object_mask, 0, sizeof(fast_state->active_object_mask));
        size_t total_state_size = sizeof(FastGameState);
        state->checksum = FM2K::State::Fletcher32((const uint8_t*)fast_state, total_state_size);
        state->timestamp_ms = SDL_GetTicks();
        return true;
    }
    
    // Object count logging removed for minimal output
    
    // 4. Pack active objects into compressed buffer
    size_t packed_size = 0;
    uint8_t* packed_data = (uint8_t*)(fast_state + 1); // Data follows header
    size_t available_space = GAME_OBJECT_POOL_SIZE - sizeof(FastGameState);
    
    bool pack_success = PackActiveObjects(fast_state->active_object_mask, active_count,
                                         packed_data, available_space, &packed_size);
    
    if (!pack_success) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to pack active objects: %u objects, %zu bytes available", 
                    active_count, available_space);
        return false;
    }
    
    // 6. Calculate total state size and checksum
    size_t total_state_size = sizeof(FastGameState) + packed_size;
    state->checksum = FM2K::State::Fletcher32((const uint8_t*)fast_state, total_state_size);
    state->timestamp_ms = SDL_GetTicks();
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    // Minimal save logging for object saves
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "SAVE: %s, %u objs, %zu bytes, %.1f ms", 
                save_strategy, active_count, total_state_size, duration_us / 1000.0f);
    
    return true;
}

// High-performance restore state from FastGameState structure
bool RestoreStateFast(const FM2K::State::GameState* state, uint32_t target_frame) {
    if (!state) return false;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 1. Restore core deterministic state
    if (!RestoreStateFromStruct(state, target_frame)) {
        return false;
    }
    
    // 2. Access FastGameState structure from buffer
    const FastGameState* fast_state = reinterpret_cast<const FastGameState*>(rollback_object_pool_buffer.get());
    if (!fast_state) {
        return false;
    }
    
    // 3. Unpack active objects from compressed buffer
    const uint8_t* packed_data = (const uint8_t*)(fast_state + 1);
    size_t packed_size = fast_state->active_object_count * (sizeof(uint16_t) + 382);
    
    bool unpack_success = UnpackActiveObjects(packed_data, packed_size, fast_state->active_object_count);
    
    if (!unpack_success) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to unpack active objects");
        return false;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    // Log performance metrics
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
               "FAST RESTORE: Frame %u, %u active objects restored, %.2f ms",
               target_frame, fast_state->active_object_count, duration_us / 1000.0f);
    
    return true;
}


// Helper function to analyze and count active objects in the object pool
uint32_t AnalyzeActiveObjects(ActiveObjectInfo* active_objects = nullptr, uint32_t max_objects = 0) {
    uint32_t active_count = 0;
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    
    if (IsBadReadPtr(object_pool_ptr, GAME_OBJECT_POOL_SIZE)) {
        return 0;
    }
    
    // Each object is 382 bytes, analyze each slot
    for (int i = 0; i < 1024; i++) {
        uint32_t* object_ptr = (uint32_t*)(object_pool_ptr + (i * 382));
        if (!IsBadReadPtr(object_ptr, sizeof(uint32_t))) {
            uint32_t object_header = *object_ptr;
            
            // Check multiple patterns to determine if object is active:
            // 1. Non-zero first 4 bytes (type/ID)
            // 2. Check second DWORD for additional validation
            uint32_t* second_dword = object_ptr + 1;
            uint32_t second_value = (!IsBadReadPtr(second_dword, sizeof(uint32_t))) ? *second_dword : 0;
            
            bool is_active = (object_header != 0) && (object_header != 0xFFFFFFFF) && 
                           (second_value != 0xCCCCCCCC); // Common uninitialized pattern
            
            if (is_active) {
                if (active_objects && active_count < max_objects) {
                    active_objects[active_count].index = i;
                    active_objects[active_count].type_or_id = object_header;
                    active_objects[active_count].is_active = true;
                }
                active_count++;
            }
        }
    }
    
    return active_count;
}

// Helper function for backward compatibility
uint32_t CountActiveObjects() {
    return AnalyzeActiveObjects();
}

// Save only active objects to buffer (for MINIMAL profile)
bool SaveActiveObjectsOnly(uint8_t* destination_buffer, size_t buffer_size, uint32_t* objects_saved = nullptr) {
    if (!destination_buffer || buffer_size == 0) {
        return false;
    }
    
    // Get list of active objects
    ActiveObjectInfo active_objects[1024];
    uint32_t active_count = AnalyzeActiveObjects(active_objects, 1024);
    
    if (active_count == 0) {
        if (objects_saved) *objects_saved = 0;
        return true; // No objects to save is valid
    }
    
    // Calculate required buffer size (each object is 382 bytes + 4 bytes for index)
    size_t required_size = active_count * (382 + sizeof(uint32_t));
    if (required_size > buffer_size) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Buffer too small for active objects: need %zu, have %zu", 
                   required_size, buffer_size);
        return false;
    }
    
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    uint8_t* dest_ptr = destination_buffer;
    uint32_t saved_count = 0;
    
    // Save each active object with its index
    for (uint32_t i = 0; i < active_count; i++) {
        uint32_t obj_index = active_objects[i].index;
        
        // Write object index first (4 bytes)
        *(uint32_t*)dest_ptr = obj_index;
        dest_ptr += sizeof(uint32_t);
        
        // Write object data (382 bytes)
        uint8_t* source_obj = object_pool_ptr + (obj_index * 382);
        if (!IsBadReadPtr(source_obj, 382)) {
            memcpy(dest_ptr, source_obj, 382);
            dest_ptr += 382;
            saved_count++;
        }
    }
    
    if (objects_saved) *objects_saved = saved_count;
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Saved %u active objects (%.1fKB vs %.1fKB full pool)", 
                saved_count, (saved_count * 382) / 1024.0f, GAME_OBJECT_POOL_SIZE / 1024.0f);
    
    return true;
}

// Restore active objects from buffer
bool RestoreActiveObjectsOnly(const uint8_t* source_buffer, size_t buffer_size, uint32_t objects_to_restore) {
    if (!source_buffer || buffer_size == 0 || objects_to_restore == 0) {
        return true; // Nothing to restore is valid
    }
    
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    const uint8_t* src_ptr = source_buffer;
    uint32_t restored_count = 0;
    
    // First, clear the entire object pool (assuming inactive objects should be zeroed)
    if (!IsBadWritePtr(object_pool_ptr, GAME_OBJECT_POOL_SIZE)) {
        memset(object_pool_ptr, 0, GAME_OBJECT_POOL_SIZE);
    }
    
    // Restore each saved object to its original position
    for (uint32_t i = 0; i < objects_to_restore; i++) {
        // Read object index (4 bytes)
        uint32_t obj_index = *(const uint32_t*)src_ptr;
        src_ptr += sizeof(uint32_t);
        
        if (obj_index >= 1024) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid object index: %u", obj_index);
            break;
        }
        
        // Restore object data (382 bytes)
        uint8_t* dest_obj = object_pool_ptr + (obj_index * 382);
        if (!IsBadWritePtr(dest_obj, 382)) {
            memcpy(dest_obj, src_ptr, 382);
            restored_count++;
        }
        src_ptr += 382;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Restored %u active objects to object pool", restored_count);
    
    return restored_count == objects_to_restore;
}

// ============================================================================
// PHASE 1: Performance Validation and Testing
// ============================================================================

// Performance comparison between old and new implementations
struct PerformanceMetrics {
    uint64_t save_time_us;
    uint64_t restore_time_us;
    size_t state_size_bytes;
    uint32_t active_objects_found;
    bool success;
};

// Benchmark function to validate Phase 1 performance improvements
bool ValidatePhase1Performance() {
    if (!state_manager_initialized || !large_buffers_allocated) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot validate performance - state manager not initialized");
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== Phase 1 Performance Validation ===");
    
    PerformanceMetrics old_metrics = {0};
    PerformanceMetrics new_metrics = {0};
    
    // Test data
    FM2K::State::GameState test_state_old;
    FM2K::State::GameState test_state_new;
    uint32_t test_frame = 12345;
    
    // === Test 1: Old Implementation (for comparison) ===
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Simulate old object scanning
    ActiveObjectInfo old_objects[1024];
    old_metrics.active_objects_found = AnalyzeActiveObjects(old_objects, 1024);
    
    // Old save approach (save all active objects individually)
    SaveCoreStateBasic(&test_state_old, test_frame);
    uint32_t objects_saved = 0;
    bool old_save_success = SaveActiveObjectsOnly(rollback_object_pool_buffer.get(), GAME_OBJECT_POOL_SIZE, &objects_saved);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    old_metrics.save_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    old_metrics.state_size_bytes = objects_saved * (sizeof(uint32_t) + 382) + sizeof(FM2K::State::CoreGameState);
    old_metrics.success = old_save_success;
    
    // === Test 2: New Fast Implementation ===
    start_time = std::chrono::high_resolution_clock::now();
    
    bool new_save_success = SaveStateFast(&test_state_new, test_frame);
    
    end_time = std::chrono::high_resolution_clock::now();
    new_metrics.save_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    new_metrics.success = new_save_success;
    
    // Calculate new implementation state size
    FastGameState* fast_state = reinterpret_cast<FastGameState*>(rollback_object_pool_buffer.get());
    if (fast_state) {
        new_metrics.active_objects_found = fast_state->active_object_count;
        new_metrics.state_size_bytes = sizeof(FastGameState) + (fast_state->active_object_count * (sizeof(uint16_t) + 382));
    }
    
    // === Test 3: Restore Performance ===
    start_time = std::chrono::high_resolution_clock::now();
    bool new_restore_success = RestoreStateFast(&test_state_new, test_frame);
    end_time = std::chrono::high_resolution_clock::now();
    new_metrics.restore_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    // === Performance Analysis ===
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Performance Comparison Results:");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Active Objects Found: %u (both implementations should match)", 
               new_metrics.active_objects_found);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Save Time:");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "    Old: %.2f ms", old_metrics.save_time_us / 1000.0f);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "    New: %.2f ms", new_metrics.save_time_us / 1000.0f);
    if (old_metrics.save_time_us > 0) {
        float speedup = (float)old_metrics.save_time_us / new_metrics.save_time_us;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "    Speedup: %.1fx faster", speedup);
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  State Size:");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "    Old: %zu KB", old_metrics.state_size_bytes / 1024);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "    New: %zu KB", new_metrics.state_size_bytes / 1024);
    if (old_metrics.state_size_bytes > 0) {
        float compression = (float)new_metrics.state_size_bytes / old_metrics.state_size_bytes;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "    Compression: %.1f%% of original size", compression * 100.0f);
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Restore Time: %.2f ms", new_metrics.restore_time_us / 1000.0f);
    
    // === Validation Checks ===
    bool validation_passed = true;
    
    // Check 1: Performance targets
    if (new_metrics.save_time_us > 500) { // Target: < 0.5ms
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "WARNING: Save time %.2f ms exceeds 0.5ms target", 
                   new_metrics.save_time_us / 1000.0f);
        validation_passed = false;
    }
    
    if (new_metrics.restore_time_us > 300) { // Target: < 0.3ms
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "WARNING: Restore time %.2f ms exceeds 0.3ms target", 
                   new_metrics.restore_time_us / 1000.0f);
        validation_passed = false;
    }
    
    if (new_metrics.state_size_bytes > 50 * 1024) { // Target: < 50KB
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "WARNING: State size %zu KB exceeds 50KB target", 
                   new_metrics.state_size_bytes / 1024);
        validation_passed = false;
    }
    
    // Check 2: Functionality
    if (!new_metrics.success) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR: Fast save/restore failed");
        validation_passed = false;
    }
    
    if (validation_passed) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "✅ Phase 1 validation PASSED - All performance targets met!");
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "⚠️  Phase 1 validation PARTIAL - Some targets not met");
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== End Performance Validation ===");
    return validation_passed;
}

// Profile-specific save functions
bool SaveStateMinimal(FM2K::State::GameState* state, uint32_t frame_number) {
    if (!state || !large_buffers_allocated) {
        return false;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Saving MINIMAL state using FAST implementation for frame %u", frame_number);
    
    // Use our new high-performance FastGameState implementation
    return SaveStateFast(state, frame_number);
}

bool SaveStateStandard(FM2K::State::GameState* state, uint32_t frame_number) {
    if (!state || !large_buffers_allocated) {
        return false;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Saving STANDARD state for frame %u", frame_number);
    
    // 1. Save core state (8KB)
    SaveCoreStateBasic(state, frame_number);
    
    // 2. Save essential runtime player data (~100KB estimated)
    // For now, save partial player data (first 100KB of each slot)
    uint8_t* player_data_ptr = (uint8_t*)PLAYER_DATA_SLOTS_ADDR;
    if (!IsBadReadPtr(player_data_ptr, 100 * 1024)) {
        memcpy(rollback_player_data_buffer.get(), player_data_ptr, 100 * 1024);
    }
    
    // 3. Save all active objects (~80KB estimated)
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (!IsBadReadPtr(object_pool_ptr, GAME_OBJECT_POOL_SIZE)) {
        memcpy(rollback_object_pool_buffer.get(), object_pool_ptr, GAME_OBJECT_POOL_SIZE);
    }
    
    // Set metadata
    state->frame_number = frame_number;
    state->timestamp_ms = SDL_GetTicks();
    
    // Calculate comprehensive checksum
    uint32_t core_checksum = FM2K::State::Fletcher32(reinterpret_cast<const uint8_t*>(&state->core), sizeof(FM2K::State::CoreGameState));
    uint32_t player_checksum = FM2K::State::Fletcher32(rollback_player_data_buffer.get(), 100 * 1024);
    uint32_t object_checksum = FM2K::State::Fletcher32(rollback_object_pool_buffer.get(), GAME_OBJECT_POOL_SIZE);
    state->checksum = core_checksum ^ player_checksum ^ object_checksum;
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "STANDARD state saved - Frame %u, Partial player + full objects, checksum: 0x%08X", 
                frame_number, state->checksum);
    return true;
}

bool SaveStateComplete(FM2K::State::GameState* state, uint32_t frame_number) {
    // Use optimized FastGameState instead of expensive comprehensive save
    return SaveStateFast(state, frame_number);
}

// Helper function to save basic core state
bool SaveCoreStateBasic(FM2K::State::GameState* state, uint32_t frame_number) {
    // Read basic game state directly from memory
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)RANDOM_SEED_ADDR;
    
    // Additional critical timers and state
    uint32_t* timer_countdown1_ptr = (uint32_t*)TIMER_COUNTDOWN1_ADDR;
    uint32_t* timer_countdown2_ptr = (uint32_t*)TIMER_COUNTDOWN2_ADDR;
    uint32_t* round_timer_counter_ptr = (uint32_t*)ROUND_TIMER_COUNTER_ADDR;
    uint32_t* object_list_heads_ptr = (uint32_t*)OBJECT_LIST_HEADS_ADDR;
    uint32_t* object_list_tails_ptr = (uint32_t*)OBJECT_LIST_TAILS_ADDR;
    
    // Validate pointers and read core state
    if (!IsBadReadPtr(frame_ptr, sizeof(uint32_t))) {
        state->core.input_buffer_index = *frame_ptr;
    }
    if (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) {
        state->core.p1_input_current = *p1_input_ptr;
    }
    if (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) {
        state->core.p2_input_current = *p2_input_ptr;
    }
    if (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t))) {
        state->core.p1_hp = *p1_hp_ptr;
    }
    if (!IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t))) {
        state->core.p2_hp = *p2_hp_ptr;
    }
    if (!IsBadReadPtr(round_timer_ptr, sizeof(uint32_t))) {
        state->core.round_timer = *round_timer_ptr;
    }
    if (!IsBadReadPtr(game_timer_ptr, sizeof(uint32_t))) {
        state->core.game_timer = *game_timer_ptr;
    }
    if (!IsBadReadPtr(random_seed_ptr, sizeof(uint32_t))) {
        state->core.random_seed = *random_seed_ptr;
    }
    
    // Read additional critical timers and state
    if (!IsBadReadPtr(timer_countdown1_ptr, sizeof(uint32_t))) {
        state->core.timer_countdown1 = *timer_countdown1_ptr;
    } else {
        state->core.timer_countdown1 = 0;  // Default if not accessible
    }
    
    if (!IsBadReadPtr(timer_countdown2_ptr, sizeof(uint32_t))) {
        state->core.timer_countdown2 = *timer_countdown2_ptr;
    } else {
        state->core.timer_countdown2 = 0;
    }
    
    if (!IsBadReadPtr(round_timer_counter_ptr, sizeof(uint32_t))) {
        state->core.round_timer_counter = *round_timer_counter_ptr;
        // Log this value to help identify if it's the in-game timer
        if (frame_number % 100 == 0) {  // Log every 100 frames
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Round timer counter: %u (frame %u)", 
                        state->core.round_timer_counter, frame_number);
        }
    } else {
        state->core.round_timer_counter = 0;
    }
    
    if (!IsBadReadPtr(object_list_heads_ptr, sizeof(uint32_t))) {
        state->core.object_list_heads = *object_list_heads_ptr;
    } else {
        state->core.object_list_heads = 0;
    }
    
    if (!IsBadReadPtr(object_list_tails_ptr, sizeof(uint32_t))) {
        state->core.object_list_tails = *object_list_tails_ptr;
    } else {
        state->core.object_list_tails = 0;
    }
    
    return true;
}

// Calculate simple checksum for state data (Fletcher32 algorithm)
uint32_t CalculateStateChecksum(const FM2K::State::GameState* state) {
    if (!state) return 0;
    
    const uint16_t* data = reinterpret_cast<const uint16_t*>(state);
    size_t length = sizeof(FM2K::State::GameState) / sizeof(uint16_t);
    
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    
    while (length) {
        size_t tlen = length > 360 ? 360 : length;
        length -= tlen;
        do {
            sum1 += *data++;
            sum2 += sum1;
        } while (--tlen);
        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }
    
    sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
    sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    
    return (sum2 << 16) | sum1;
}

// Restore game state from GekkoNet state structure
bool RestoreStateFromStruct(const FM2K::State::GameState* state, uint32_t target_frame) {
    if (!state) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "RestoreStateFromStruct: Null state pointer");
        return false;
    }
    
    // Restore core game state to memory
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)RANDOM_SEED_ADDR;
    
    // Additional critical timers
    uint32_t* timer_countdown1_ptr = (uint32_t*)TIMER_COUNTDOWN1_ADDR;
    uint32_t* timer_countdown2_ptr = (uint32_t*)TIMER_COUNTDOWN2_ADDR;
    uint32_t* round_timer_counter_ptr = (uint32_t*)ROUND_TIMER_COUNTER_ADDR;
    uint32_t* object_list_heads_ptr = (uint32_t*)OBJECT_LIST_HEADS_ADDR;
    uint32_t* object_list_tails_ptr = (uint32_t*)OBJECT_LIST_TAILS_ADDR;
    
    // Write state back to game memory with validation
    if (!IsBadWritePtr(frame_ptr, sizeof(uint32_t))) {
        *frame_ptr = state->core.input_buffer_index;
    }
    if (!IsBadWritePtr(p1_input_ptr, sizeof(uint16_t))) {
        *p1_input_ptr = state->core.p1_input_current;
    }
    if (!IsBadWritePtr(p2_input_ptr, sizeof(uint16_t))) {
        *p2_input_ptr = state->core.p2_input_current;
    }
    if (!IsBadWritePtr(p1_hp_ptr, sizeof(uint32_t))) {
        *p1_hp_ptr = state->core.p1_hp;
    }
    if (!IsBadWritePtr(p2_hp_ptr, sizeof(uint32_t))) {
        *p2_hp_ptr = state->core.p2_hp;
    }
    if (!IsBadWritePtr(round_timer_ptr, sizeof(uint32_t))) {
        *round_timer_ptr = state->core.round_timer;
    }
    if (!IsBadWritePtr(game_timer_ptr, sizeof(uint32_t))) {
        *game_timer_ptr = state->core.game_timer;
    }
    if (!IsBadWritePtr(random_seed_ptr, sizeof(uint32_t))) {
        *random_seed_ptr = state->core.random_seed;
    }
    if (!IsBadWritePtr(timer_countdown1_ptr, sizeof(uint32_t))) {
        *timer_countdown1_ptr = state->core.timer_countdown1;
    }
    if (!IsBadWritePtr(timer_countdown2_ptr, sizeof(uint32_t))) {
        *timer_countdown2_ptr = state->core.timer_countdown2;
    }
    if (!IsBadWritePtr(round_timer_counter_ptr, sizeof(uint32_t))) {
        *round_timer_counter_ptr = state->core.round_timer_counter;
    }
    if (!IsBadWritePtr(object_list_heads_ptr, sizeof(uint32_t))) {
        *object_list_heads_ptr = state->core.object_list_heads;
    }
    if (!IsBadWritePtr(object_list_tails_ptr, sizeof(uint32_t))) {
        *object_list_tails_ptr = state->core.object_list_tails;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "RestoreStateFromStruct: Restored state for frame %u", target_frame);
    return true;
}

// Initialize shared memory for configuration
bool InitializeSharedMemory() {
    // Create unique shared memory name using process ID
    DWORD process_id = GetCurrentProcessId();
    std::string shared_memory_name = "FM2K_InputSharedMemory_" + std::to_string(process_id);
    
    // Create shared memory for communication with launcher
    shared_memory_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(SharedInputData),
        shared_memory_name.c_str()
    );
    
    if (shared_memory_handle == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to create shared memory");
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
    
    // Initialize shared memory data
    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    memset(shared_data, 0, sizeof(SharedInputData));
    shared_data->config_updated = false;
    shared_data->debug_save_state_requested = false;
    shared_data->debug_load_state_requested = false;
    shared_data->debug_rollback_requested = false;
    shared_data->debug_rollback_frames = 0;
    shared_data->debug_command_id = 0;
    shared_data->debug_save_to_slot_requested = false;
    shared_data->debug_load_from_slot_requested = false;
    shared_data->debug_target_slot = 0;
    shared_data->auto_save_enabled = false;  // Disable auto-save by default (user can enable)
    shared_data->auto_save_interval_frames = 120;  // Auto-save every 120 frames (1.2 seconds at 100 FPS)
    shared_data->use_minimal_gamestate_testing = false;  // Disable minimal state testing by default
    shared_data->config_version = 0;  // Initialize configuration version counter
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Shared memory initialized: use_minimal_gamestate_testing = %s, config_version = %u", 
                shared_data->use_minimal_gamestate_testing ? "TRUE" : "FALSE", shared_data->config_version);
    // All saves now use optimized FastGameState automatically
    
    // Initialize slot status
    for (int i = 0; i < 8; i++) {
        shared_data->slot_status[i].occupied = false;
        shared_data->slot_status[i].frame_number = 0;
        shared_data->slot_status[i].timestamp_ms = 0;
        shared_data->slot_status[i].checksum = 0;
        shared_data->slot_status[i].state_size_kb = 0;
        shared_data->slot_status[i].save_time_us = 0;
        shared_data->slot_status[i].load_time_us = 0;
    }
    
    // Initialize performance stats
    shared_data->perf_stats.total_saves = 0;
    shared_data->perf_stats.total_loads = 0;
    shared_data->perf_stats.avg_save_time_us = 0;
    shared_data->perf_stats.avg_load_time_us = 0;
    shared_data->perf_stats.memory_usage_mb = 0;
    
    // Initialize rollback performance counters
    shared_data->perf_stats.rollback_count = 0;
    shared_data->perf_stats.max_rollback_frames = 0;
    shared_data->perf_stats.total_rollback_frames = 0;
    shared_data->perf_stats.avg_rollback_frames = 0;
    shared_data->perf_stats.last_rollback_time_us = 0;
    shared_data->perf_stats.rollbacks_this_second = 0;
    shared_data->perf_stats.current_second_start = get_microseconds();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Shared memory initialized successfully");
    return true;
}

// Check for configuration updates from launcher
bool CheckConfigurationUpdates() {
    if (!shared_memory_data) return false;
    
    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    if (shared_data->config_updated) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Configuration update received - Online: %s, Host: %s", 
                    shared_data->is_online_mode ? "YES" : "NO", shared_data->is_host ? "YES" : "NO");
        
        // Update local configuration
        is_online_mode = shared_data->is_online_mode;
        is_host = shared_data->is_host;
        
        // Clear the update flag
        shared_data->config_updated = false;
        
        // Update auto-save settings
        // (Auto-save configuration can be changed from launcher later)
        
        // Reconfigure GekkoNet session if needed
        if (gekko_session && gekko_initialized) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Reconfiguring GekkoNet session...");
            // TODO: Implement session reconfiguration
        }
        
        return true;
    }
    
    return false;
}

// Initialize state manager for rollback
bool InitializeStateManager() {
    // Clear state buffer
    memset(saved_states, 0, sizeof(saved_states));
    current_state_index = 0;
    
    // Allocate buffers for large memory regions - per-slot + rollback buffers
    try {
        // Allocate per-slot buffers
        for (int i = 0; i < 8; i++) {
            slot_player_data_buffers[i] = std::make_unique<uint8_t[]>(PLAYER_DATA_SLOTS_SIZE);
            slot_object_pool_buffers[i] = std::make_unique<uint8_t[]>(GAME_OBJECT_POOL_SIZE);
        }
        
        // Allocate rollback buffers
        rollback_player_data_buffer = std::make_unique<uint8_t[]>(PLAYER_DATA_SLOTS_SIZE);
        rollback_object_pool_buffer = std::make_unique<uint8_t[]>(GAME_OBJECT_POOL_SIZE);
        
        // Initialize slot metadata
        for (int i = 0; i < 8; i++) {
            // All slots now use optimized FastGameState automatically
        }
        
        large_buffers_allocated = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Allocated %zu KB per slot x8 + rollback (%zu KB total)", 
                    (PLAYER_DATA_SLOTS_SIZE + GAME_OBJECT_POOL_SIZE) / 1024, 
                    ((PLAYER_DATA_SLOTS_SIZE + GAME_OBJECT_POOL_SIZE) * 9) / 1024);
    } catch (const std::bad_alloc& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to allocate state buffers: %s", e.what());
        large_buffers_allocated = false;
        return false;
    }
    
    state_manager_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Enhanced state manager initialized with comprehensive memory capture");
    
    // Run Phase 1 performance validation
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Running Phase 1 performance validation...");
    bool validation_result = ValidatePhase1Performance();
    if (validation_result) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: ✅ Phase 1 optimizations validated successfully!");
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: ⚠️ Phase 1 validation completed with warnings");
    }
    
    return true;
}

// Enhanced save game state with comprehensive memory capture
bool SaveGameStateDirect(FM2K::State::GameState* state, uint32_t frame_number) {
    if (!state || !large_buffers_allocated) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid state buffer or large buffers not allocated");
        return false;
    }
    
    // Reduce logging frequency in production mode
    if (!production_mode || (frame_number % 100 == 0)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Capturing comprehensive game state for frame %u", frame_number);
    }
    
    // Read basic game state directly from memory (no ReadProcessMemory needed)
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)RANDOM_SEED_ADDR;
    
    // Extended game state pointers
    uint32_t* game_mode_ptr = (uint32_t*)GAME_MODE_ADDR;
    uint32_t* round_setting_ptr = (uint32_t*)ROUND_SETTING_ADDR;
    uint32_t* p1_round_count_ptr = (uint32_t*)P1_ROUND_COUNT_ADDR;
    uint32_t* p1_round_state_ptr = (uint32_t*)P1_ROUND_STATE_ADDR;
    uint32_t* p1_action_state_ptr = (uint32_t*)P1_ACTION_STATE_ADDR;
    uint32_t* p2_action_state_ptr = (uint32_t*)P2_ACTION_STATE_ADDR;
    uint32_t* camera_x_ptr = (uint32_t*)CAMERA_X_ADDR;
    uint32_t* camera_y_ptr = (uint32_t*)CAMERA_Y_ADDR;
    uint32_t* timer1_ptr = (uint32_t*)TIMER_COUNTDOWN1_ADDR;
    uint32_t* timer2_ptr = (uint32_t*)TIMER_COUNTDOWN2_ADDR;
    uint32_t* round_timer_counter_ptr = (uint32_t*)ROUND_TIMER_COUNTER_ADDR;
    uint32_t* object_list_heads_ptr = (uint32_t*)OBJECT_LIST_HEADS_ADDR;
    uint32_t* object_list_tails_ptr = (uint32_t*)OBJECT_LIST_TAILS_ADDR;
    
    // Validate basic pointers and read core state
    if (!IsBadReadPtr(frame_ptr, sizeof(uint32_t))) {
        state->core.input_buffer_index = *frame_ptr;
    }
    if (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) {
        state->core.p1_input_current = *p1_input_ptr;
    }
    if (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) {
        state->core.p2_input_current = *p2_input_ptr;
    }
    if (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t))) {
        state->core.p1_hp = *p1_hp_ptr;
    }
    if (!IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t))) {
        state->core.p2_hp = *p2_hp_ptr;
    }
    if (!IsBadReadPtr(round_timer_ptr, sizeof(uint32_t))) {
        state->core.round_timer = *round_timer_ptr;
    }
    if (!IsBadReadPtr(game_timer_ptr, sizeof(uint32_t))) {
        state->core.game_timer = *game_timer_ptr;
    }
    if (!IsBadReadPtr(random_seed_ptr, sizeof(uint32_t))) {
        state->core.random_seed = *random_seed_ptr;
    }
    
    // Read additional critical timers and object management state
    if (!IsBadReadPtr(timer1_ptr, sizeof(uint32_t))) {
        state->core.timer_countdown1 = *timer1_ptr;
    }
    if (!IsBadReadPtr(timer2_ptr, sizeof(uint32_t))) {
        state->core.timer_countdown2 = *timer2_ptr;
    }
    if (!IsBadReadPtr(round_timer_counter_ptr, sizeof(uint32_t))) {
        state->core.round_timer_counter = *round_timer_counter_ptr;
    }
    if (!IsBadReadPtr(object_list_heads_ptr, sizeof(uint32_t))) {
        state->core.object_list_heads = *object_list_heads_ptr;
    }
    if (!IsBadReadPtr(object_list_tails_ptr, sizeof(uint32_t))) {
        state->core.object_list_tails = *object_list_tails_ptr;
    }
    
    // STATE DEBUG removed
    
    // Capture major memory regions for comprehensive state
    bool player_data_captured = false;
    bool object_pool_captured = false;
    
    // 1. Player Data Slots (459KB) - most critical for rollback
    uint8_t* player_data_ptr = (uint8_t*)PLAYER_DATA_SLOTS_ADDR;
    if (!IsBadReadPtr(player_data_ptr, PLAYER_DATA_SLOTS_SIZE)) {
        memcpy(rollback_player_data_buffer.get(), player_data_ptr, PLAYER_DATA_SLOTS_SIZE);
        player_data_captured = true;
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Captured player data slots (%zu KB)", PLAYER_DATA_SLOTS_SIZE / 1024);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to capture player data slots - invalid memory");
    }
    
    // 2. Game Object Pool (391KB) - all game entities
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (!IsBadReadPtr(object_pool_ptr, GAME_OBJECT_POOL_SIZE)) {
        memcpy(rollback_object_pool_buffer.get(), object_pool_ptr, GAME_OBJECT_POOL_SIZE);
        object_pool_captured = true;
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Captured game object pool (%zu KB)", GAME_OBJECT_POOL_SIZE / 1024);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to capture game object pool - invalid memory");
    }
    
    // Set metadata
    state->frame_number = frame_number;
    state->timestamp_ms = SDL_GetTicks();
    
    // Calculate comprehensive checksum including large memory regions
    uint32_t core_checksum = FM2K::State::Fletcher32(reinterpret_cast<const uint8_t*>(&state->core), sizeof(FM2K::State::CoreGameState));
    uint32_t player_checksum = player_data_captured ? FM2K::State::Fletcher32(rollback_player_data_buffer.get(), PLAYER_DATA_SLOTS_SIZE) : 0;
    uint32_t object_checksum = object_pool_captured ? FM2K::State::Fletcher32(rollback_object_pool_buffer.get(), GAME_OBJECT_POOL_SIZE) : 0;
    
    // Combine checksums for comprehensive state validation
    state->checksum = core_checksum ^ player_checksum ^ object_checksum;
    
    // Debug what's changing between frames (reduced frequency to minimize spam)
    if (last_core_state_valid && frame_number % 300 == 0) {  // Log changes every 300 frames (3 seconds) to reduce spam
        bool core_changed = memcmp(&state->core, &last_core_state.core, sizeof(FM2K::State::CoreGameState)) != 0;
        if (core_changed) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Core state changes detected:");
            if (state->core.input_buffer_index != last_core_state.core.input_buffer_index) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  Input buffer index: %u -> %u", last_core_state.core.input_buffer_index, state->core.input_buffer_index);
            }
            if (state->core.p1_input_current != last_core_state.core.p1_input_current) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  P1 input: 0x%08X -> 0x%08X", last_core_state.core.p1_input_current, state->core.p1_input_current);
            }
            if (state->core.p2_input_current != last_core_state.core.p2_input_current) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  P2 input: 0x%08X -> 0x%08X", last_core_state.core.p2_input_current, state->core.p2_input_current);
            }
            if (state->core.round_timer != last_core_state.core.round_timer) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  Round timer: %u -> %u", last_core_state.core.round_timer, state->core.round_timer);
            }
            if (state->core.game_timer != last_core_state.core.game_timer) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  Game timer: %u -> %u", last_core_state.core.game_timer, state->core.game_timer);
            }
            if (state->core.random_seed != last_core_state.core.random_seed) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  RNG seed: 0x%08X -> 0x%08X", last_core_state.core.random_seed, state->core.random_seed);
            }
        }
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Checksums - Core: 0x%08X, Player: 0x%08X, Objects: 0x%08X", core_checksum, player_checksum, object_checksum);
        
        // Debug timer values to help identify the in-game timer
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Timer Debug - Round: %u, Game: %u, Counter1: %u, Counter2: %u, RoundCounter: %u", 
                     state->core.round_timer, state->core.game_timer, state->core.timer_countdown1, 
                     state->core.timer_countdown2, state->core.round_timer_counter);
    }
    
    // Store current state for next comparison
    last_core_state = *state;
    last_core_state_valid = true;
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Frame %u state captured - Core: %s, Player Data: %s, Objects: %s (checksum: 0x%08X)",
                frame_number,
                "OK",
                player_data_captured ? "OK" : "FAILED",
                object_pool_captured ? "OK" : "FAILED",
                state->checksum);
    
    return player_data_captured && object_pool_captured;  // Require both major regions for valid state
}

// Enhanced load game state with comprehensive memory restoration
bool LoadGameStateDirect(const FM2K::State::GameState* state) {
    if (!state || !large_buffers_allocated) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid state or large buffers not allocated");
        return false;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Restoring comprehensive game state for frame %u", state->frame_number);
    
    // Write basic game state directly to memory (no WriteProcessMemory needed)
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)P2_INPUT_ADDR;
    uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
    uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
    uint32_t* round_timer_ptr = (uint32_t*)ROUND_TIMER_ADDR;
    uint32_t* game_timer_ptr = (uint32_t*)GAME_TIMER_ADDR;
    uint32_t* random_seed_ptr = (uint32_t*)RANDOM_SEED_ADDR;
    
    // Extended game state pointers
    uint32_t* game_mode_ptr = (uint32_t*)GAME_MODE_ADDR;
    uint32_t* round_setting_ptr = (uint32_t*)ROUND_SETTING_ADDR;
    uint32_t* p1_round_count_ptr = (uint32_t*)P1_ROUND_COUNT_ADDR;
    uint32_t* p1_round_state_ptr = (uint32_t*)P1_ROUND_STATE_ADDR;
    uint32_t* p1_action_state_ptr = (uint32_t*)P1_ACTION_STATE_ADDR;
    uint32_t* p2_action_state_ptr = (uint32_t*)P2_ACTION_STATE_ADDR;
    uint32_t* camera_x_ptr = (uint32_t*)CAMERA_X_ADDR;
    uint32_t* camera_y_ptr = (uint32_t*)CAMERA_Y_ADDR;
    uint32_t* timer1_ptr = (uint32_t*)TIMER_COUNTDOWN1_ADDR;
    uint32_t* timer2_ptr = (uint32_t*)TIMER_COUNTDOWN2_ADDR;
    uint32_t* round_timer_counter_ptr = (uint32_t*)ROUND_TIMER_COUNTER_ADDR;
    uint32_t* object_list_heads_ptr = (uint32_t*)OBJECT_LIST_HEADS_ADDR;
    uint32_t* object_list_tails_ptr = (uint32_t*)OBJECT_LIST_TAILS_ADDR;
    
    // Read current values before writing for comparison
    uint32_t before_frame = 0, before_p1_hp = 0, before_p2_hp = 0, before_round_timer = 0;
    uint16_t before_p1_input = 0, before_p2_input = 0;
    
    if (!IsBadReadPtr(frame_ptr, sizeof(uint32_t))) before_frame = *frame_ptr;
    if (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) before_p1_input = *p1_input_ptr;
    if (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) before_p2_input = *p2_input_ptr;
    if (!IsBadReadPtr(p1_hp_ptr, sizeof(uint32_t))) before_p1_hp = *p1_hp_ptr;
    if (!IsBadReadPtr(p2_hp_ptr, sizeof(uint32_t))) before_p2_hp = *p2_hp_ptr;
    if (!IsBadReadPtr(round_timer_ptr, sizeof(uint32_t))) before_round_timer = *round_timer_ptr;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: Before - Frame: %u, P1HP: %u, P2HP: %u, RoundTimer: %u, P1Input: 0x%04X, P2Input: 0x%04X",
                before_frame, before_p1_hp, before_p2_hp, before_round_timer, before_p1_input, before_p2_input);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: Target - Frame: %u, P1HP: %u, P2HP: %u, RoundTimer: %u, P1Input: 0x%08X, P2Input: 0x%08X",
                state->core.input_buffer_index, state->core.p1_hp, state->core.p2_hp, state->core.round_timer, 
                state->core.p1_input_current, state->core.p2_input_current);
    
    // Validate basic pointers and restore core state
    if (!IsBadWritePtr(frame_ptr, sizeof(uint32_t))) {
        *frame_ptr = state->core.input_buffer_index;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: Frame counter written: %u -> %u", before_frame, *frame_ptr);
    }
    if (!IsBadWritePtr(p1_input_ptr, sizeof(uint16_t))) {
        *p1_input_ptr = (uint16_t)state->core.p1_input_current;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: P1 input written: 0x%04X -> 0x%04X", before_p1_input, *p1_input_ptr);
    }
    if (!IsBadWritePtr(p2_input_ptr, sizeof(uint16_t))) {
        *p2_input_ptr = (uint16_t)state->core.p2_input_current;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: P2 input written: 0x%04X -> 0x%04X", before_p2_input, *p2_input_ptr);
    }
    if (!IsBadWritePtr(p1_hp_ptr, sizeof(uint32_t))) {
        *p1_hp_ptr = state->core.p1_hp;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: P1 HP written: %u -> %u", before_p1_hp, *p1_hp_ptr);
    }
    if (!IsBadWritePtr(p2_hp_ptr, sizeof(uint32_t))) {
        *p2_hp_ptr = state->core.p2_hp;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: P2 HP written: %u -> %u", before_p2_hp, *p2_hp_ptr);
    }
    if (!IsBadWritePtr(round_timer_ptr, sizeof(uint32_t))) {
        *round_timer_ptr = state->core.round_timer;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RESTORE: Round timer written: %u -> %u", before_round_timer, *round_timer_ptr);
    }
    if (!IsBadWritePtr(game_timer_ptr, sizeof(uint32_t))) {
        *game_timer_ptr = state->core.game_timer;
    }
    if (!IsBadWritePtr(random_seed_ptr, sizeof(uint32_t))) {
        *random_seed_ptr = state->core.random_seed;
    }
    
    // Restore additional critical timers and object management state
    if (!IsBadWritePtr(timer1_ptr, sizeof(uint32_t))) {
        *timer1_ptr = state->core.timer_countdown1;
    }
    if (!IsBadWritePtr(timer2_ptr, sizeof(uint32_t))) {
        *timer2_ptr = state->core.timer_countdown2;
    }
    if (!IsBadWritePtr(round_timer_counter_ptr, sizeof(uint32_t))) {
        *round_timer_counter_ptr = state->core.round_timer_counter;
    }
    if (!IsBadWritePtr(object_list_heads_ptr, sizeof(uint32_t))) {
        *object_list_heads_ptr = state->core.object_list_heads;
    }
    if (!IsBadWritePtr(object_list_tails_ptr, sizeof(uint32_t))) {
        *object_list_tails_ptr = state->core.object_list_tails;
    }
    
    // Restore major memory regions for comprehensive rollback
    bool player_data_restored = false;
    bool object_pool_restored = false;
    
    // 1. Player Data Slots (459KB) - critical for character state
    uint8_t* player_data_ptr = (uint8_t*)PLAYER_DATA_SLOTS_ADDR;
    if (!IsBadWritePtr(player_data_ptr, PLAYER_DATA_SLOTS_SIZE)) {
        memcpy(player_data_ptr, rollback_player_data_buffer.get(), PLAYER_DATA_SLOTS_SIZE);
        player_data_restored = true;
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Restored player data slots (%zu KB)", PLAYER_DATA_SLOTS_SIZE / 1024);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to restore player data slots - invalid memory");
    }
    
    // 2. Game Object Pool (391KB) - all game entities
    uint8_t* object_pool_ptr = (uint8_t*)GAME_OBJECT_POOL_ADDR;
    if (!IsBadWritePtr(object_pool_ptr, GAME_OBJECT_POOL_SIZE)) {
        memcpy(object_pool_ptr, rollback_object_pool_buffer.get(), GAME_OBJECT_POOL_SIZE);
        object_pool_restored = true;
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Restored game object pool (%zu KB)", GAME_OBJECT_POOL_SIZE / 1024);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to restore game object pool - invalid memory");
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Frame %u state restored - Core: %s, Player Data: %s, Objects: %s (checksum: 0x%08X)",
                state->frame_number,
                "OK",
                player_data_restored ? "OK" : "FAILED",
                object_pool_restored ? "OK" : "FAILED",
                state->checksum);
    
    return player_data_restored && object_pool_restored;  // Require both major regions for successful restore
}

// === MinimalGameState Testing Framework ===

// Save minimal state for GekkoNet testing (48 bytes)
bool SaveMinimalState(FM2K::MinimalGameState* state, uint32_t frame_number) {
    if (!state) return false;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Load all minimal state data from memory
    if (!state->LoadFromMemory()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "MinimalState: Failed to load from memory");
        return false;
    }
    
    // Set frame number and input checksum
    state->frame_number = frame_number;
    
    // Calculate simple input checksum from recent inputs
    uint32_t input_hash = 0;
    uint32_t* p1_input_ptr = (uint32_t*)P1_INPUT_ADDR;
    uint32_t* p2_input_ptr = (uint32_t*)P2_INPUT_ADDR;
    if (p1_input_ptr && p2_input_ptr) {
        input_hash = (*p1_input_ptr) ^ (*p2_input_ptr << 16) ^ frame_number;
    }
    state->input_checksum = input_hash;
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "MinimalState: Saved frame %u (48 bytes, %.1f μs)", 
                frame_number, (float)duration_us);
    
    return true;
}

// Load minimal state for GekkoNet testing
bool LoadMinimalState(const FM2K::MinimalGameState* state) {
    if (!state) return false;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Save all minimal state data to memory
    if (!state->SaveToMemory()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "MinimalState: Failed to save to memory");
        return false;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "MinimalState: Loaded frame %u (48 bytes, %.1f μs)", 
                state->frame_number, (float)duration_us);
    
    return true;
}

// Save minimal state to ring buffer
bool SaveMinimalStateToBuffer(uint32_t frame_number) {
    if (!use_minimal_gamestate_testing) return false;
    
    uint32_t index = frame_number % 8;
    bool success = SaveMinimalState(&minimal_state_ring[index], frame_number);
    if (success) {
        minimal_state_ring_index = index;
    }
    return success;
}

// Load minimal state from ring buffer
bool LoadMinimalStateFromBuffer(uint32_t frame_number) {
    if (!use_minimal_gamestate_testing) return false;
    
    uint32_t index = frame_number % 8;
    return LoadMinimalState(&minimal_state_ring[index]);
}

// Save state to ring buffer using optimized FastGameState
bool SaveStateToBuffer(uint32_t frame_number) {
    if (!state_manager_initialized) return false;
    
    uint32_t index = frame_number % 8;
    return SaveStateFast(&saved_states[index], frame_number);
}

// Load state from ring buffer
bool LoadStateFromBuffer(uint32_t frame_number) {
    if (!state_manager_initialized) return false;
    
    uint32_t index = frame_number % 8;
    return LoadGameStateDirect(&saved_states[index]);
}

// Save state to specific slot using optimized FastGameState
bool SaveStateToSlot(uint32_t slot, uint32_t frame_number) {
    if (!state_manager_initialized || slot >= 8) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid slot %u or state manager not initialized", slot);
        return false;
    }
    
    uint64_t start_time = get_microseconds();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Saving FastGameState to slot %u at frame %u", slot, frame_number);
    
    // Use optimized FastGameState implementation (10-50KB with bitfield compression)
    bool save_result = SaveStateFast(&save_slots[slot], frame_number);
    
    if (!save_result) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to save FastGameState to slot %u", slot);
        return false;
    }
    
    uint64_t end_time = get_microseconds();
    uint32_t save_time_us = (uint32_t)(end_time - start_time);
    uint32_t state_size_kb = sizeof(FastGameState) / 1024; // Optimized size
    
    slot_occupied[slot] = true;
    total_saves++;
    total_save_time_us += save_time_us;
    
    // Update shared memory status for UI
    if (shared_memory_data) {
        SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
        shared_data->slot_status[slot].occupied = true;
        shared_data->slot_status[slot].frame_number = frame_number;
        shared_data->slot_status[slot].timestamp_ms = save_slots[slot].timestamp_ms;
        shared_data->slot_status[slot].checksum = save_slots[slot].checksum;
        shared_data->slot_status[slot].state_size_kb = state_size_kb;
        shared_data->slot_status[slot].save_time_us = save_time_us;
        
        // Update performance stats
        shared_data->perf_stats.total_saves = total_saves;
        shared_data->perf_stats.avg_save_time_us = (uint32_t)(total_save_time_us / total_saves);
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FastGameState saved to slot %u (frame %u, %uKB, %uμs, checksum: 0x%08X)", 
                slot, frame_number, state_size_kb, save_time_us, save_slots[slot].checksum);
    return true;
}

// Load state from specific slot using optimized FastGameState
bool LoadStateFromSlot(uint32_t slot) {
    if (!state_manager_initialized || slot >= 8) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid slot %u or state manager not initialized", slot);
        return false;
    }
    
    if (!slot_occupied[slot]) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Slot %u is empty", slot);
        return false;
    }
    
    uint64_t start_time = get_microseconds();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Loading FastGameState from slot %u (frame %u)", slot, save_slots[slot].frame_number);
    
    // Use optimized FastGameState restoration
    bool restore_result = RestoreStateFast(&save_slots[slot], save_slots[slot].frame_number);
    
    if (!restore_result) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to restore FastGameState from slot %u", slot);
        return false;
    }
    
    uint64_t end_time = get_microseconds();
    uint32_t load_time_us = (uint32_t)(end_time - start_time);
    
    total_loads++;
    total_load_time_us += load_time_us;
    
    // Update shared memory performance stats
    if (shared_memory_data) {
        SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
        shared_data->slot_status[slot].load_time_us = load_time_us;
        shared_data->perf_stats.total_loads = total_loads;
        shared_data->perf_stats.avg_load_time_us = (uint32_t)(total_load_time_us / total_loads);
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FastGameState loaded from slot %u (frame %u, %uμs, checksum: 0x%08X)", 
                slot, save_slots[slot].frame_number, load_time_us, save_slots[slot].checksum);
    return true;
}

// Process debug commands from launcher UI
void ProcessDebugCommands() {
    if (!shared_memory_data) {
        // Only log this occasionally to avoid spam
        static uint32_t no_shared_memory_log_counter = 0;
        if ((no_shared_memory_log_counter++ % 1000) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HOOK: ProcessDebugCommands - no shared memory");
        }
        return;
    }
    
    SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
    static uint32_t last_processed_command_id = 0;
    
    // Only process new commands
    if (shared_data->debug_command_id == last_processed_command_id) {
        return;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Processing debug command ID %u (last: %u)", shared_data->debug_command_id, last_processed_command_id);
    
    // Log what commands are pending
    if (shared_data->debug_save_to_slot_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: -> debug_save_to_slot_requested = TRUE for slot %u", shared_data->debug_target_slot);
    }
    if (shared_data->debug_load_from_slot_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: -> debug_load_from_slot_requested = TRUE for slot %u", shared_data->debug_target_slot);
    }
    if (shared_data->debug_save_state_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: -> debug_save_state_requested = TRUE");
    }
    if (shared_data->debug_load_state_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: -> debug_load_state_requested = TRUE");
    }
    if (shared_data->debug_rollback_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: -> debug_rollback_requested = TRUE for %u frames", shared_data->debug_rollback_frames);
    }
    
    // Manual save state
    if (shared_data->debug_save_state_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Manual save state requested");
        if (state_manager_initialized) {
            uint32_t current_frame = g_frame_counter;
            if (SaveStateToBuffer(current_frame)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: State saved successfully for frame %u", current_frame);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Failed to save state for frame %u", current_frame);
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: State manager not initialized");
        }
        shared_data->debug_save_state_requested = false;
    }
    
    // Manual load state
    if (shared_data->debug_load_state_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Manual load state requested");
        if (state_manager_initialized) {
            uint32_t current_frame = g_frame_counter;
            // Load from previous frame (simple test)
            uint32_t load_frame = current_frame > 0 ? current_frame - 1 : current_frame;
            if (LoadStateFromBuffer(load_frame)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: State loaded successfully from frame %u", load_frame);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Failed to load state from frame %u", load_frame);
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: State manager not initialized");
        }
        shared_data->debug_load_state_requested = false;
    }
    
    // Force rollback
    if (shared_data->debug_rollback_requested) {
        uint32_t rollback_frames = shared_data->debug_rollback_frames;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Force rollback requested - %u frames", rollback_frames);
        
        if (state_manager_initialized && rollback_frames > 0) {
            uint32_t current_frame = g_frame_counter;
            uint32_t target_frame = current_frame > rollback_frames ? current_frame - rollback_frames : 0;
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Rolling back from frame %u to frame %u", current_frame, target_frame);
            
            if (LoadStateFromBuffer(target_frame)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Rollback successful - restored frame %u", target_frame);
                // Update frame counter to reflect rollback
                g_frame_counter = target_frame;
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Rollback failed - could not load frame %u", target_frame);
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DEBUG: Invalid rollback parameters - frames: %u, initialized: %s", 
                         rollback_frames, state_manager_initialized ? "YES" : "NO");
        }
        
        shared_data->debug_rollback_requested = false;
        shared_data->debug_rollback_frames = 0;
    }
    
    // Save to specific slot
    if (shared_data->debug_save_to_slot_requested) {
        uint32_t slot = shared_data->debug_target_slot;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Save to slot %u requested", slot);
        
        if (state_manager_initialized && slot < 8) {
            uint32_t current_frame = g_frame_counter;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Attempting to save frame %u to slot %u", current_frame, slot);
            if (SaveStateToSlot(slot, current_frame)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: State saved to slot %u successfully", slot);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Failed to save state to slot %u", slot);
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Invalid slot %u or state manager not initialized (initialized: %s)", slot, state_manager_initialized ? "YES" : "NO");
        }
        
        shared_data->debug_save_to_slot_requested = false;
    }
    
    // Load from specific slot
    if (shared_data->debug_load_from_slot_requested) {
        uint32_t slot = shared_data->debug_target_slot;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Load from slot %u requested", slot);
        
        if (state_manager_initialized && slot < 8) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Attempting to load from slot %u (occupied: %s)", slot, slot_occupied[slot] ? "YES" : "NO");
            if (LoadStateFromSlot(slot)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: State loaded from slot %u successfully", slot);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Failed to load state from slot %u", slot);
            }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Invalid slot %u or state manager not initialized (initialized: %s)", slot, state_manager_initialized ? "YES" : "NO");
        }
        
        shared_data->debug_load_from_slot_requested = false;
    }
    
    last_processed_command_id = shared_data->debug_command_id;
}

// Configure network session based on mode
bool ConfigureNetworkMode(bool online_mode, bool host_mode) {
    is_online_mode = online_mode;
    is_host = host_mode;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network mode configured - Online: %s, Host: %s", 
                online_mode ? "YES" : "NO", host_mode ? "YES" : "NO");
    return true;
}

// Initialize GekkoNet session for rollback netcode using LocalNetworkAdapter
bool InitializeGekkoNet() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** INITIALIZING GEKKONET WITH REAL UDP NETWORKING (OnlineSession Style) ***");
    
    // TEMPORARY: Force online mode for testing (since launcher doesn't set config yet)
    is_online_mode = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: FORCING ONLINE MODE FOR TESTING");
    
    // Get networking configuration from environment variables (like OnlineSession example)
    uint16_t local_port = 7000;      // Default to host port
    std::string remote_address = "127.0.0.1:7001";  // Default to guest address
    uint8_t player_index = 0;        // Default to player 0
    
    // Read from environment variables
    char* env_player = getenv("FM2K_PLAYER_INDEX");
    char* env_port = getenv("FM2K_LOCAL_PORT");
    char* env_remote = getenv("FM2K_REMOTE_ADDR");
    
    if (env_player) {
        player_index = static_cast<uint8_t>(atoi(env_player));
    }
    
    // Initialize file logging after player_index is set
    ::player_index = player_index;  // Set global player_index for logging
    ::is_host = (player_index == 0); // Set global is_host for logging
    InitializeFileLogging();
    
    // Initialize input recording if requested
    char* env_input_recording = getenv("FM2K_INPUT_RECORDING");
    if (env_input_recording && strcmp(env_input_recording, "1") == 0) {
        InitializeInputRecording();
    }
    
    // Set production mode if requested
    char* env_production_mode = getenv("FM2K_PRODUCTION_MODE");
    if (env_production_mode && strcmp(env_production_mode, "1") == 0) {
        ::production_mode = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Production mode enabled - reduced logging");
    }
    
    if (env_port) {
        local_port = static_cast<uint16_t>(atoi(env_port));
    }
    
    if (env_remote) {
        remote_address = std::string(env_remote);
    }
    
    // Log the configuration (like OnlineSession example)
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network config - Player: %u, Local port: %u, Remote: %s", 
                player_index, local_port, remote_address.c_str());
    
    // Create GekkoNet session
    if (!gekko_create(&gekko_session)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to create GekkoNet session!");
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session created successfully");
    
    // Configure GekkoNet session (based on OnlineSession example)
    GekkoConfig config;
    config.num_players = 2;              // CRITICAL: FM2K is 2-player game
    config.max_spectators = 0;
    config.input_prediction_window = 10;  // Higher window like the example
    config.spectator_delay = 0;
    config.input_size = sizeof(uint8_t);  // 1 byte per PLAYER (2 total)
    config.state_size = sizeof(uint32_t); // Minimal state like bsnes
    config.limited_saving = false;
    config.post_sync_joining = false;
    config.desync_detection = true;
    
    gekko_start(gekko_session, &config);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session configured and started");
    
    // Set real UDP network adapter
    gekko_net_adapter_set(gekko_session, gekko_default_adapter(local_port));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Real UDP adapter set on port %u", local_port);
    
    // Add players following OnlineSession example EXACTLY
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Adding players - Player index: %u", player_index);
    
    // Use global local_player_handle
    
    // CRITICAL: Follow OnlineSession EXACTLY - reassign player_index to actual handle (lines 217-228)
    if (player_index == 0) {
        // add local player
        player_index = gekko_add_actor(gekko_session, LocalPlayer, nullptr);  // REASSIGN like OnlineSession line 219
        // add remote player
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), (unsigned int)remote_address.size() };
        gekko_add_actor(gekko_session, RemotePlayer, &remote);
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Player 0 - LOCAL handle: %d", player_index);
    } else {
        // add remote player
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), (unsigned int)remote_address.size() };
        gekko_add_actor(gekko_session, RemotePlayer, &remote);
        // add local player
        player_index = gekko_add_actor(gekko_session, LocalPlayer, nullptr);  // REASSIGN like OnlineSession line 228
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Player 1 - LOCAL handle: %d", player_index);
    }
    
    // Store local player handle for input processing
    local_player_handle = player_index;
    
    // Validate local player handle
    if (local_player_handle < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to add local player! Handle: %d", local_player_handle);
        gekko_destroy(gekko_session);
        gekko_session = nullptr;
        return false;
    }
    
    // Set input delay for local player (like OnlineSession example)
    gekko_set_local_delay(gekko_session, local_player_handle, 1);  // 1 frame delay like example
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Set input delay for local player handle %d", local_player_handle);
    
    gekko_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialization complete with real UDP networking!");
    return true;
}

// BSNES PATTERN: AllPlayersValid() implementation (gekko.cpp lines 523-544)
bool AllPlayersValid() {
    if (!gekko_session || !gekko_initialized) {
        return false;
    }
    
    if (!gekko_session_started) {
        // CRITICAL: Ensure network polling happens during handshake (like OnlineSession line 255)
        gekko_network_poll(gekko_session);
        
        // Check if all players are connected using session events (like BSNES CheckStatusActors)
        int session_event_count = 0;
        auto session_events = gekko_session_events(gekko_session, &session_event_count);
        
        bool session_started_event_found = false;
        for (int i = 0; i < session_event_count; i++) {
            auto event = session_events[i];
            
            // Log ALL events during handshake for debugging
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session Event: %d", event->type);
            
            // Handle all event types like BSNES
            if (event->type == SessionStarted) {
                session_started_event_found = true;
            } else if (event->type == DesyncDetected) {
                auto desync = event->data.desynced;
                
                // Enhanced desync reporting with state dumps
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "=== DESYNC DETECTED ===");
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Frame: %d, Remote handle: %d", desync.frame, desync.remote_handle);
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Local checksum: 0x%08X, Remote checksum: 0x%08X", desync.local_checksum, desync.remote_checksum);
                
                // Dump current game state for analysis
                uint32_t* p1_hp_ptr = (uint32_t*)P1_HP_ADDR;
                uint32_t* p2_hp_ptr = (uint32_t*)P2_HP_ADDR;
                uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
                uint32_t* p1_input_ptr = (uint32_t*)P1_INPUT_ADDR;
                uint32_t* p2_input_ptr = (uint32_t*)P2_INPUT_ADDR;
                
                if (p1_hp_ptr && !IsBadReadPtr(p1_hp_ptr, 4)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "P1 HP: %u", *p1_hp_ptr);
                }
                if (p2_hp_ptr && !IsBadReadPtr(p2_hp_ptr, 4)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "P2 HP: %u", *p2_hp_ptr);
                }
                if (frame_ptr && !IsBadReadPtr(frame_ptr, 4)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Game Frame: %u", *frame_ptr);
                }
                if (p1_input_ptr && !IsBadReadPtr(p1_input_ptr, 4)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "P1 Last Input: 0x%08X", *p1_input_ptr);
                }
                if (p2_input_ptr && !IsBadReadPtr(p2_input_ptr, 4)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "P2 Last Input: 0x%08X", *p2_input_ptr);
                }
                
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Live P1 Input: 0x%08X, Live P2 Input: 0x%08X", live_p1_input, live_p2_input);
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "========================");
                
                // Generate detailed desync report file
                GenerateDesyncReport(desync.frame, desync.local_checksum, desync.remote_checksum);
            } else if (event->type == PlayerDisconnected) {
                auto disco = event->data.disconnected;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player disconnected: %d", disco.handle);
            } else if (event->type == PlayerConnected) {
                auto connected = event->data.connected;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player connected: %d", connected.handle);
            }
        }
        
        // Debug: Log handshake status even if no events
        if (session_event_count == 0) {
            static int no_events_counter = 0;
            if (++no_events_counter % 300 == 0) { // Every 5 seconds at 60fps
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: No session events received yet - still waiting for network handshake... (attempt %d)", no_events_counter);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Debug - gekko_session_started=%s, gekko_session=%p", 
                           gekko_session_started ? "true" : "false", gekko_session);
            }
        }
        
        // BSNES PATTERN: Only trigger SessionStarted ONCE (gekko.cpp lines 531-535)
        if (session_started_event_found) {
            gekko_session_started = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SESSION STARTED - All players connected and synchronized! (BSNES AllPlayersValid pattern)");
            return true;
        }
        
        return false;  // Still waiting for session to start
    }
    
    // Session already started - always return true (BSNES pattern line 543)
    return true;
}

// Global variables to store current networked inputs for get_player_input hook
static uint32_t networked_p1_input = 0;
static uint32_t networked_p2_input = 0;
static bool use_networked_inputs = false;
static uint32_t last_network_frame = 0;

// live_p1_input and live_p2_input moved to top of file for desync reports

// Hook for get_player_input function - intercepts input reading at the source
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    // Get original input first
    int original_input = original_get_player_input ? original_get_player_input(player_id, input_type) : 0;
    
    // CAPTURE LIVE INPUTS for network transmission
    if (player_id == 0) {
        live_p1_input = original_input;
    } else if (player_id == 1) {
        live_p2_input = original_input;
    }
    
    // Debug logging for input capture issues
    static uint32_t debug_call_count = 0;
    debug_call_count++;
    if (debug_call_count <= 10 || (original_input != 0)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT DEBUG: Player %d, Type %d, Original: 0x%X, UseNet: %s, AllValid: %s", 
                   player_id, input_type, original_input, 
                   use_networked_inputs ? "YES" : "NO",
                   (gekko_initialized && gekko_session && AllPlayersValid()) ? "YES" : "NO");
    }
    
    // Debug: Show current networked input state
    if (debug_call_count <= 20 || (original_input != 0)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT DEBUG: P%d Networked: 0x%X, UseNet: %s, AllValid: %s", 
                   player_id, (player_id == 0) ? networked_p1_input : networked_p2_input,
                   use_networked_inputs ? "YES" : "NO",
                   AllPlayersValid() ? "YES" : "NO");
    }
    
    // ROLLBACK-AWARE: Only use networked inputs during synchronized session
    if (use_networked_inputs && gekko_initialized && gekko_session && AllPlayersValid()) {
        // Return networked inputs for synchronized players (including zero inputs)
        if (player_id == 0) {
            if (debug_call_count <= 10 || (networked_p1_input != original_input)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT DEBUG: Returning networked P1: 0x%X (was 0x%X)", networked_p1_input, original_input);
            }
            return networked_p1_input;
        } else if (player_id == 1) {
            if (debug_call_count <= 10 || (networked_p2_input != original_input)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT DEBUG: Returning networked P2: 0x%X (was 0x%X)", networked_p2_input, original_input);
            }
            return networked_p2_input;
        }
    }
    
    // Fall back to original input reading
    return original_input;
}

// Simple hook implementations (like your working ML2 code)
int __cdecl Hook_ProcessGameInputs() {
    g_frame_counter++;
    
    // Always output on first few calls to verify hook is working
    if (g_frame_counter <= 5) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hook called! Frame %u", g_frame_counter);
    }
    
    // GekkoNet should already be initialized by run_game_loop hook
    if (!gekko_initialized) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: ERROR - GekkoNet not initialized! run_game_loop hook may have failed!");
        // Don't try to initialize here - this should have been done in run_game_loop hook
    }
    
    // Read the actual frame counter from game memory (with basic validation)
    uint32_t game_frame = 0;
    uint32_t* frame_ptr = (uint32_t*)FRAME_COUNTER_ADDR;
    if (frame_ptr && !IsBadReadPtr(frame_ptr, sizeof(uint32_t))) {
        game_frame = *frame_ptr;
    }
    
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: process_game_inputs called! Hook frame %u, Game frame %u", 
             //g_frame_counter, game_frame);
    
    // Capture current inputs from game memory (with enhanced validation)
    uint32_t p1_input = 0;
    uint32_t p2_input = 0;
    bool p1_input_valid = false;
    bool p2_input_valid = false;
    
    uint32_t* p1_input_ptr = (uint32_t*)P1_INPUT_ADDR;
    uint32_t* p2_input_ptr = (uint32_t*)P2_INPUT_ADDR;
    
    if (p1_input_ptr && !IsBadReadPtr(p1_input_ptr, sizeof(uint32_t))) {
        p1_input = *p1_input_ptr;
        p1_input_valid = true;
    }
    if (p2_input_ptr && !IsBadReadPtr(p2_input_ptr, sizeof(uint32_t))) {
        p2_input = *p2_input_ptr;
        p2_input_valid = true;
    }
    
    // Validate input ranges (FM2K uses 11-bit inputs)
    if (p1_input_valid && (p1_input & 0xFFFFF800)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: P1 input has invalid high bits: 0x%08X", p1_input);
        p1_input &= 0x07FF;  // Mask to 11 bits
    }
    if (p2_input_valid && (p2_input & 0xFFFFF800)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: P2 input has invalid high bits: 0x%08X", p2_input);
        p2_input &= 0x07FF;  // Mask to 11 bits
    }
    
    // Minimal input detection logging
    static uint32_t last_p1 = 0, last_p2 = 0;
    if ((p1_input != last_p1 && p1_input != 0) || (p2_input != last_p2 && p2_input != 0)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT: P1=0x%02X, P2=0x%02X", p1_input & 0xFF, p2_input & 0xFF);
        last_p1 = p1_input; last_p2 = p2_input;
    }
    
    // Check for configuration updates from launcher
    CheckConfigurationUpdates();
    
    // Process debug commands from launcher
    ProcessDebugCommands();
    
    // Log occasionally to debug input capture (reduced frequency to avoid spam)
    if (g_frame_counter % 100 == 0) {  
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Frame %u - Game frame: %u - P1: 0x%08X (addr valid: %s), P2: 0x%08X (addr valid: %s)", 
                 g_frame_counter, game_frame, p1_input, 
                 (!IsBadReadPtr(p1_input_ptr, sizeof(uint32_t))) ? "YES" : "NO",
                 p2_input,
                 (!IsBadReadPtr(p2_input_ptr, sizeof(uint32_t))) ? "YES" : "NO");
    }
    
    // Forward inputs directly to GekkoNet (with enhanced error handling)
    if (gekko_initialized && gekko_session) {
        // CRITICAL: Call gekko_network_poll EVERY frame (like OnlineSession line 255)
        gekko_network_poll(gekko_session);
        
        // CRITICAL: Always process inputs for GekkoNet handshake (even with zero inputs)
        {
            // Convert 16-bit FM2K inputs to 8-bit GekkoNet format
            uint8_t p1_gekko = 0;
            uint8_t p2_gekko = 0;
            
            // CRITICAL FIX: Use live captured inputs instead of stale memory addresses
            // live_p1_input and live_p2_input are captured in real-time by Hook_GetPlayerInput
            p1_gekko = (uint8_t)(live_p1_input & 0xFF);  // Use live P1 input from hook
            p2_gekko = (uint8_t)(live_p2_input & 0xFF);  // Use live P2 input from hook
            
            // Record inputs for testing/debugging if enabled
            if (input_recording_enabled) {
                RecordInput(g_frame_counter, live_p1_input, live_p2_input);
            }
            
            // CRITICAL FIX: Send both player inputs like bsnes (each player only sends their own)
            // Player index 0 sends P1 input, Player index 1 sends P2 input  
            uint8_t local_input = (player_index == 0) ? p1_gekko : p2_gekko;
            
            gekko_add_local_input(gekko_session, local_player_handle, &local_input);
            
            // Enhanced input logging to debug transmission
            static uint32_t send_frame_count = 0;
            send_frame_count++;
            if (local_input != 0 || send_frame_count <= 10) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT SEND: Handle %d sending 0x%02X (Live P1=0x%02X, Live P2=0x%02X, Mem P1=0x%02X, Mem P2=0x%02X)", 
                           local_player_handle, local_input, p1_gekko, p2_gekko, 
                           (uint8_t)(p1_input & 0xFF), (uint8_t)(p2_input & 0xFF));
            }
            
            // Save current state before processing GekkoNet updates (production mode: less frequent)
            uint32_t save_interval = production_mode ? 32 : 8;  // Production: every 32 frames, Debug: every 8 frames
            if (state_manager_initialized && (g_frame_counter % save_interval) == 0) {
                SaveStateToBuffer(g_frame_counter);
            }
            
            // Auto-save to slot 0 if enabled (simplified logic - no manual sync mode)
            if (shared_memory_data && state_manager_initialized) {
                SharedInputData* shared_data = static_cast<SharedInputData*>(shared_memory_data);
                
                // Update configuration when config_version changes (forced re-read)
                static uint32_t prev_config_version = 0;
                static bool prev_minimal_testing = false;
                
                if (shared_data->config_version != prev_config_version) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Configuration updated (version %u -> %u), re-reading settings...", 
                               prev_config_version, shared_data->config_version);
                    prev_config_version = shared_data->config_version;
                    
                    // Force re-read all configuration
                    use_minimal_gamestate_testing = shared_data->use_minimal_gamestate_testing;
                    production_mode = shared_data->production_mode;
                    input_recording_enabled = shared_data->enable_input_recording;
                    
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Configuration applied: MinimalGameState=%s, Production=%s, InputRecording=%s", 
                               use_minimal_gamestate_testing ? "ENABLED" : "DISABLED",
                               production_mode ? "ENABLED" : "DISABLED", 
                               input_recording_enabled ? "ENABLED" : "DISABLED");
                } else {
                    // Check for config changes every frame during desync debugging
                    static uint32_t last_logged_frame = 0;
                    if (g_frame_counter <= 10 || g_frame_counter - last_logged_frame >= 60) {
                        bool current_minimal = shared_data->use_minimal_gamestate_testing;
                        if (current_minimal != use_minimal_gamestate_testing || g_frame_counter <= 10) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Frame %u: MinimalGameState check - shared_memory=%s, local_var=%s, config_version=%u", 
                                       g_frame_counter, current_minimal ? "TRUE" : "FALSE", 
                                       use_minimal_gamestate_testing ? "TRUE" : "FALSE", shared_data->config_version);
                            use_minimal_gamestate_testing = current_minimal;  // Sync the variable
                        }
                        last_logged_frame = g_frame_counter;
                    }
                    // Normal update (only log changes)
                    if (shared_data->use_minimal_gamestate_testing != prev_minimal_testing) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "MinimalGameState testing: %s", 
                                   shared_data->use_minimal_gamestate_testing ? "ENABLED" : "DISABLED");
                        prev_minimal_testing = shared_data->use_minimal_gamestate_testing;
                    }
                    use_minimal_gamestate_testing = shared_data->use_minimal_gamestate_testing;
                }
                
                if (shared_data->auto_save_enabled) {
                    // Only auto-save if enough frames have passed since last auto-save
                    if ((g_frame_counter - last_auto_save_frame) >= shared_data->auto_save_interval_frames) {
                        // Auto-save triggered (minimal logging)
                        SaveStateToSlot(0, g_frame_counter);  // Auto-save always goes to slot 0
                        last_auto_save_frame = g_frame_counter;
                    }
                }
            }
            
            // BSNES PATTERN: Check AllPlayersValid() but don't freeze the window
            if (!AllPlayersValid()) {
                // Network handshake in progress - allow Windows message processing
                // Handshake in progress (minimal logging)
                
                // CRITICAL: Process Windows messages to keep window responsive
                MSG msg;
                while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                
                // CRITICAL: Continue polling GekkoNet for network handshake
                gekko_network_poll(gekko_session);
                
                // CRITICAL: Also need to call update_session during handshake (like OnlineSession)
                int handshake_update_count = 0;
                auto handshake_updates = gekko_update_session(gekko_session, &handshake_update_count);
                // Handshake updates (minimal logging)
                
                // Let FM2K continue running basic systems but block game logic advancement
                return original_process_inputs ? original_process_inputs() : 0;
            }
            
            // Session handshake complete - synchronized gameplay
            
            // First: Process session events (connections, disconnections) like bsnes
            int session_event_count = 0;
            auto session_events = gekko_session_events(gekko_session, &session_event_count);
            for (int i = 0; i < session_event_count; i++) {
                auto event = session_events[i];
                switch (event->type) {
                    case PlayerConnected:
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player Connected - Handle: %d", event->data.connected.handle);
                        break;
                    case PlayerDisconnected: 
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player Disconnected - Handle: %d", event->data.disconnected.handle);
                        break;
                    case SessionStarted:
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session Started");
                        break;
                    case DesyncDetected: {
                        auto desync = event->data.desynced;
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "🚨 DESYNC DETECTED! Frame: %d, Handle: %d", desync.frame, desync.remote_handle);
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Local checksum: 0x%08X, Remote checksum: 0x%08X", desync.local_checksum, desync.remote_checksum);
                        
                        // Enhanced MinimalGameState desync analysis
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Desync analysis: use_minimal_gamestate_testing = %s", 
                                   use_minimal_gamestate_testing ? "TRUE" : "FALSE");
                        if (use_minimal_gamestate_testing) {
                            LogMinimalGameStateDesync(desync.frame, desync.local_checksum, desync.remote_checksum);
                        } else {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Skipping MinimalGameState analysis - feature disabled");
                        }
                        
                        // Generate detailed desync report
                        GenerateDesyncReport(desync.frame, desync.local_checksum, desync.remote_checksum);
                        break;
                    }
                    case PlayerSyncing:
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player Syncing - Handle: %d", event->data.syncing.handle);
                        break;
                    case SpectatorPaused:
                    case SpectatorUnpaused:
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Spectator %s", 
                                   (event->type == SpectatorPaused) ? "Paused" : "Unpaused");
                        break;
                    default:
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Unknown session event type: %d", event->type);
                        break;
                }
            }

            // Second: Process game updates (Save/Load/Advance) like bsnes  
            int update_count = 0;
            auto updates = gekko_update_session(gekko_session, &update_count);
            
            // Handle GekkoNet events (AdvanceEvent, SaveEvent, LoadEvent)
            if (updates && update_count > 0) {
                for (int i = 0; i < update_count; i++) {
                    auto* update = updates[i];
                    if (!update) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Null update at index %d", i);
                        continue;
                    }
                    
                    switch (update->type) {
                        case AdvanceEvent: {
                            // Game should advance one frame with predicted inputs
                            uint32_t target_frame = update->data.adv.frame;
                            uint32_t input_length = update->data.adv.input_len;
                            uint8_t* inputs = update->data.adv.inputs;
                            
                            // Reduced logging: Only log occasionally or on non-zero inputs
                            bool should_log = (target_frame % 30 == 1);
                            if (should_log) {
                                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent to frame %u (inputs: %u bytes)", 
                                            target_frame, input_length);
                            }
                            
                            // Like bsnes: Store inputs directly for the hooked input function to return
                            if (inputs && input_length >= 2) {
                                // Store raw network inputs (GekkoNet handles the prediction/rollback logic)
                                networked_p1_input = inputs[0];  // Direct storage - no conversion
                                networked_p2_input = inputs[1]; 
                                use_networked_inputs = true;     // Always use network inputs during AdvanceEvent
                                
                                // Minimal logging for network debugging  
                                if ((inputs[0] | inputs[1]) != 0) {
                                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Frame %u inputs P1=0x%02X, P2=0x%02X", 
                                               target_frame, inputs[0], inputs[1]);
                                }
                                
                                // GekkoNet will call the game's run loop after this AdvanceEvent
                            }
                            break;
                        }
                        
                        case SaveEvent: {
                            // GekkoNet wants us to save the current state
                            uint32_t save_frame = update->data.save.frame;
                            uint32_t* checksum_ptr = update->data.save.checksum;
                            uint32_t* state_len_ptr = update->data.save.state_len;
                            uint8_t* state_ptr = update->data.save.state;
                            
                            // SaveEvent processing (minimal logging)
                            
                            if (checksum_ptr && state_len_ptr && state_ptr) {
                                if (use_minimal_gamestate_testing) {
                                    // MinimalGameState testing mode: Use 48-byte state
                                    FM2K::MinimalGameState minimal_state;
                                    if (SaveMinimalState(&minimal_state, save_frame)) {
                                        // Save to local ring buffer for rollback
                                        SaveMinimalStateToBuffer(save_frame);
                                        
                                        // Pass minimal state directly to GekkoNet (48 bytes)
                                        *state_len_ptr = sizeof(FM2K::MinimalGameState);
                                        *checksum_ptr = minimal_state.CalculateChecksum();
                                        memcpy(state_ptr, &minimal_state, sizeof(FM2K::MinimalGameState));
                                        
                                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Saved MinimalState frame %u (48 bytes, checksum: 0x%08X)", 
                                                   save_frame, *checksum_ptr);
                                    } else {
                                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Failed to save MinimalState for frame %u", save_frame);
                                    }
                                } else if (state_manager_initialized) {
                                    // Full state mode: Use FastGameState system
                                    FM2K::State::GameState local_state;
                                    if (SaveStateFast(&local_state, save_frame)) {
                                        // Store full state in our local ring buffer (like bsnes netplay.states)
                                        // TODO: Implement local state ring buffer for rollback
                                        
                                        // Pass minimal frame identifier to GekkoNet (like bsnes)
                                        *state_len_ptr = sizeof(uint32_t);
                                        *checksum_ptr = local_state.checksum;  // Use our state checksum
                                        memcpy(state_ptr, &save_frame, sizeof(uint32_t));  // Just frame number
                                        
                                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Saved state for frame %u (checksum: 0x%08X)", 
                                                   save_frame, local_state.checksum);
                                    } else {
                                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Failed to save state for frame %u", save_frame);
                                    }
                                }
                            }
                            break;
                        }
                        
                        case LoadEvent: {
                            // Rollback to specific frame
                            uint32_t target_frame = update->data.load.frame;
                            uint32_t state_length = update->data.load.state_len;
                            uint8_t* state_data = update->data.load.state;
                            
                            // Track rollback performance metrics
                            uint32_t frames_rolled_back = (g_frame_counter > target_frame) ? (g_frame_counter - target_frame) : 0;
                            rollback_count++;
                            total_rollback_frames += frames_rolled_back;
                            if (frames_rolled_back > max_rollback_frames) {
                                max_rollback_frames = frames_rolled_back;
                            }
                            last_rollback_time_us = get_microseconds();
                            
                            // Update shared memory statistics for launcher UI
                            UpdateRollbackStats(frames_rolled_back);
                            
                            // Enhanced rollback logging (production mode: only show significant rollbacks)
                            bool should_log = !production_mode || frames_rolled_back >= 3;
                            if (should_log) {
                                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ROLLBACK: Frame %u → %u (%u frames back) [Count: %u, Max: %u, Avg: %.1f]", 
                                           g_frame_counter, target_frame, frames_rolled_back, rollback_count, max_rollback_frames,
                                           rollback_count > 0 ? (float)total_rollback_frames / rollback_count : 0.0f);
                            }
                            
                            if (use_minimal_gamestate_testing && state_data && state_length == sizeof(FM2K::MinimalGameState)) {
                                // MinimalGameState testing mode: Load 48-byte state directly
                                FM2K::MinimalGameState* minimal_state = reinterpret_cast<FM2K::MinimalGameState*>(state_data);
                                
                                if (LoadMinimalState(minimal_state)) {
                                    g_frame_counter = target_frame;
                                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: MinimalState rollback to frame %u (48 bytes, checksum: 0x%08X)", 
                                               target_frame, minimal_state->CalculateChecksum());
                                } else {
                                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Failed to load MinimalState for frame %u", target_frame);
                                }
                                
                            } else if (state_manager_initialized && state_data && state_length == sizeof(uint32_t)) {
                                // Full state mode: Get frame number from GekkoNet, load our own full state
                                uint32_t saved_frame_number = *reinterpret_cast<uint32_t*>(state_data);
                                
                                // TODO: Load full state from our local ring buffer using saved_frame_number
                                // For now, just reset frame counter and log the rollback attempt
                                g_frame_counter = target_frame;
                                
                                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Rollback to frame %u (saved frame: %u)", 
                                           target_frame, saved_frame_number);
                                
                            } else {
                                const char* expected_size = use_minimal_gamestate_testing ? "48 (MinimalGameState)" : "4 (frame ID)";
                                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Invalid rollback data for frame %u (state_len: %u, expected: %s)", 
                                           target_frame, state_length, expected_size);
                            }
                            break;
                        }
                        
                        default:
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Unknown event type: %d", update->type);
                            break;
                    }
                }
                
                // BSNES-style rift sync: Halt frames when clients drift apart
            float frames_ahead = gekko_frames_ahead(gekko_session);
            if (frames_ahead >= 2.0f && !rift_sync_active && (rift_sync_counter % 180 == 0)) {
                // Rift sync like bsnes: halt frame advancement when clients drift
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "RIFT_SYNC: Halting frame, frames ahead: %.2f", frames_ahead);
                rift_sync_active = true;
                
                // Implement rift sync: save state, run one frame, then restore
                if (use_minimal_gamestate_testing) {
                    FM2K::MinimalGameState rift_state;
                    if (SaveMinimalState(&rift_state, g_frame_counter)) {
                        // Call original function to advance one frame
                        if (original_process_inputs) {
                            original_process_inputs();
                        }
                        // Restore to halt the frame
                        LoadMinimalState(&rift_state);
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "RIFT_SYNC: Frame halted using MinimalGameState");
                    }
                } else {
                    // Fallback: just skip this frame
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "RIFT_SYNC: Frame skipped (no state save available)");
                }
                
                rift_sync_active = false;
                return original_process_inputs ? original_process_inputs() : 0;
            }
            
            rift_sync_counter++;
            
            // GekkoNet Desync Detection: Check for state mismatches
            static uint32_t last_desync_check_frame = 0;
            if (g_frame_counter > last_desync_check_frame + 60) {  // Check every 1 second at 60 FPS
                    // Get current game state checksum for comparison
                    uint32_t current_checksum = 0;
                    if (state_manager_initialized) {
                        FM2K::State::GameState temp_state;
                        if (SaveStateFast(&temp_state, g_frame_counter)) {
                            current_checksum = temp_state.checksum;
                        }
                    }
                    
                    // GekkoNet automatically handles desync detection when config.desync_detection = true
                    // We just log our own state for additional validation
                    static uint32_t consecutive_desync_frames = 0;
                    static uint32_t last_logged_checksum = 0;
                    
                    if (current_checksum != 0 && current_checksum != last_logged_checksum) {
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "DESYNC_CHECK: Frame %u, Checksum: 0x%08X", 
                                    g_frame_counter, current_checksum);
                        last_logged_checksum = current_checksum;
                        consecutive_desync_frames = 0;
                    } else if (current_checksum == last_logged_checksum) {
                        consecutive_desync_frames++;
                        if (consecutive_desync_frames > 300) {  // 5 seconds of identical checksums = potential freeze
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "POTENTIAL_FREEZE: Checksum unchanged for %u frames", 
                                       consecutive_desync_frames);
                            consecutive_desync_frames = 0;  // Reset to avoid spam
                        }
                    }
                    
                    last_desync_check_frame = g_frame_counter;
                }
            }
            
            // Manual sync mode logic removed - trust GekkoNet completely
            
            // Minimal logging like OnlineSession (only occasionally)
            if (g_frame_counter % 600 == 0) {  // Every 10 seconds
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Frame %u - Updates: %d", g_frame_counter, update_count);
            }
        }  // End of input processing block
    } else {
        // GekkoNet not initialized - log occasionally
        if (g_frame_counter % 300 == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session not initialized at frame %u", g_frame_counter);
        }
    }
    
    // Call original function
    int result = 0;
    if (original_process_inputs) {
        result = original_process_inputs();
    }
    
    return result;
}

int __cdecl Hook_UpdateGameState() {
    // CRITICAL: Block update_game_state during GekkoNet handshake (completing the BSNES pattern)
    if (gekko_initialized && !gekko_session_started) {
        // Don't call original function - this prevents FM2K from advancing game state
        // while network handshake is in progress (like BSNES blocking)
        return 0;
    }
    
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: update_game_state called!");
    
    // Call original function only when session is ready or GekkoNet not initialized
    int result = 0;
    if (original_update_game) {
        result = original_update_game();
    }
    
    return result;
}

// BSNES PATTERN: Hook run_game_loop for main control and blocking
BOOL __cdecl Hook_RunGameLoop() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** RUN_GAME_LOOP INTERCEPTED - BSNES-LEVEL CONTROL! ***");
    
    // STEP 1: Initialize GekkoNet immediately (after all FM2K systems ready)
    if (!gekko_initialized) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing GekkoNet at BSNES level!");
        bool gekko_result = InitializeGekkoNet();
        if (gekko_result) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: ✓ GekkoNet initialized at main loop level!");
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: ✗ GekkoNet initialization failed!");
            // Continue without netplay
            return original_run_game_loop ? original_run_game_loop() : FALSE;
        }
    }
    
    // STEP 2: Set up for blocking in game loop (not here - don't freeze main thread!)
    if (gekko_initialized && gekko_session) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet ready - synchronization will happen in game loop to preserve message handling");
        gekko_session_started = false; // Ensure we start in blocking mode
    }
    
    // STEP 3: Both clients ready (or timeout) - start FM2K main loop
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Calling original run_game_loop...");
    return original_run_game_loop ? original_run_game_loop() : FALSE;
}

// Simple initialization function
bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing MinHook...");
    
    // Initialize MinHook
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: MH_Initialize failed: %d", mh_init);
        return false;
    }
    
    // Validate target addresses before hooking
    if (IsBadCodePtr((FARPROC)PROCESS_INPUTS_ADDR) || IsBadCodePtr((FARPROC)GET_PLAYER_INPUT_ADDR) || 
        IsBadCodePtr((FARPROC)UPDATE_GAME_ADDR) || IsBadCodePtr((FARPROC)RUN_GAME_LOOP_ADDR)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Target addresses are invalid or not yet mapped");
        return false;
    }
    
    // Install hook for process_game_inputs function
    void* inputFuncAddr = (void*)PROCESS_INPUTS_ADDR;
    MH_STATUS status1 = MH_CreateHook(inputFuncAddr, (void*)Hook_ProcessGameInputs, (void**)&original_process_inputs);
    if (status1 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create input hook: %d", status1);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable1 = MH_EnableHook(inputFuncAddr);
    if (enable1 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable input hook: %d", enable1);
        MH_Uninitialize();
        return false;
    }
    
    // Install hook for get_player_input function (source-level input interception)
    void* getInputFuncAddr = (void*)GET_PLAYER_INPUT_ADDR;
    MH_STATUS status_getinput = MH_CreateHook(getInputFuncAddr, (void*)Hook_GetPlayerInput, (void**)&original_get_player_input);
    if (status_getinput != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create get_player_input hook: %d", status_getinput);
        MH_Uninitialize();
        return false;
    }

    MH_STATUS enable_getinput = MH_EnableHook(getInputFuncAddr);
    if (enable_getinput != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable get_player_input hook: %d", enable_getinput);
        MH_Uninitialize();
        return false;
    }
    
    // Install hook for update_game_state function  
    void* updateFuncAddr = (void*)UPDATE_GAME_ADDR;
    MH_STATUS status2 = MH_CreateHook(updateFuncAddr, (void*)Hook_UpdateGameState, (void**)&original_update_game);
    if (status2 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create update hook: %d", status2);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable2 = MH_EnableHook(updateFuncAddr);
    if (enable2 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable update hook: %d", enable2);
        MH_Uninitialize();
        return false;
    }
    
    // Install hook for run_game_loop function (BSNES-level control)
    void* runGameLoopFuncAddr = (void*)RUN_GAME_LOOP_ADDR;
    MH_STATUS status3 = MH_CreateHook(runGameLoopFuncAddr, (void*)Hook_RunGameLoop, (void**)&original_run_game_loop);
    if (status3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to create run_game_loop hook: %d", status3);
        MH_Uninitialize();
        return false;
    }
    
    MH_STATUS enable3 = MH_EnableHook(runGameLoopFuncAddr);
    if (enable3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to enable run_game_loop hook: %d", enable3);
        MH_Uninitialize();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: BSNES-level architecture installed successfully!");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   - run_game_loop hook at 0x%08X (BSNES main control + blocking)", RUN_GAME_LOOP_ADDR);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   - Input processing hook at 0x%08X", PROCESS_INPUTS_ADDR);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   - Get player input hook at 0x%08X (source-level interception)", GET_PLAYER_INPUT_ADDR);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   - Game state update hook at 0x%08X", UPDATE_GAME_ADDR);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "   - GekkoNet will initialize at main loop level with proper blocking");
    return true;
}

// Simple shutdown function
void ShutdownHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Shutting down hooks...");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hooks shut down");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        {
            // Disable thread library calls
            DisableThreadLibraryCalls(hModule);
            
            // Allocate a console window for debugging purposes
            AllocConsole();
            FILE* fDummy;
            freopen_s(&fDummy, "CONOUT$", "w", stdout);
            freopen_s(&fDummy, "CONOUT$", "w", stderr);
            freopen_s(&fDummy, "CONIN$", "r", stdin);
            std::cout.clear();
            std::cerr.clear();
            std::cin.clear();
            
            // Initialize SDL's logging system
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
            SDL_SetLogOutputFunction([](void* userdata, int category, SDL_LogPriority priority, const char* message) {
                printf("%s\n", message);
            }, nullptr);
            
            // The console is now available
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Console window opened for debugging.");

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: DLL attached to process!");
            
            // Write initial log entry first
            std::string log_path = GetLogFilePath();
            FILE* log = fopen(log_path.c_str(), "w");
            if (log) {
                fprintf(log, "FM2K HOOK: DLL attached to process at %lu\n", GetTickCount());
                fprintf(log, "FM2K HOOK: About to initialize GekkoNet...\n");
                fflush(log);
                fclose(log);
            }
            
            // Initialize shared memory for configuration
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing shared memory...");
            if (!InitializeSharedMemory()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to initialize shared memory");
            }
            
            // CRITICAL: Force identical RNG seed for deterministic execution
            char* forced_seed = getenv("FM2K_FORCE_RNG_SEED");
            if (forced_seed) {
                uint32_t seed = (uint32_t)atoi(forced_seed);
                uint32_t* rng_addr = (uint32_t*)0x41FB1C;  // FM2K RNG seed address
                if (rng_addr) {
                    *rng_addr = seed;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Forced RNG seed to %u at address 0x41FB1C", seed);
                }
            }
            
            // === COMPREHENSIVE INITIALIZATION LOGGING ===
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== INITIALIZATION STATE ANALYSIS ===");
            
            // Log environment variables
            char* player_index = getenv("FM2K_PLAYER_INDEX");
            char* local_port = getenv("FM2K_LOCAL_PORT");  
            char* remote_addr = getenv("FM2K_REMOTE_ADDR");
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ENV: PLAYER_INDEX=%s, LOCAL_PORT=%s, REMOTE_ADDR=%s", 
                       player_index ? player_index : "NULL",
                       local_port ? local_port : "NULL", 
                       remote_addr ? remote_addr : "NULL");
            
            // Log system timing
            DWORD tick_count = GetTickCount();
            LARGE_INTEGER perf_counter;
            QueryPerformanceCounter(&perf_counter);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "TIMING: TickCount=%lu, PerfCounter=%lld", tick_count, perf_counter.QuadPart);
            
            // Log memory layout  
            HMODULE base_module = GetModuleHandle(NULL);
            void* heap = GetProcessHeap();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "MEMORY: BaseModule=0x%p, ProcessHeap=0x%p", base_module, heap);
            
            // Log critical FM2K memory addresses
            uint32_t* rng_ptr = (uint32_t*)0x41FB1C;
            uint32_t* p1_hp_ptr = (uint32_t*)0x47010C;
            uint32_t* p2_hp_ptr = (uint32_t*)0x47030C;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K_STATE: RNG=0x%08X, P1_HP=0x%08X, P2_HP=0x%08X", 
                       rng_ptr ? *rng_ptr : 0, 
                       p1_hp_ptr ? *p1_hp_ptr : 0,
                       p2_hp_ptr ? *p2_hp_ptr : 0);
            
            // CRITICAL: Force deterministic memory initialization
            // Zero out critical memory regions to ensure identical state
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DETERMINISM: Forcing memory initialization...");
            
            // Zero out FM2K game state regions
            memset((void*)0x470000, 0, 0x1000);  // Clear input/state area
            memset((void*)0x4A0000, 0, 0x10000); // Clear game object area
            memset((void*)0x4E0000, 0, 0x10000); // Clear player data area
            
            // Force identical RNG and timer values
            if (rng_ptr) *rng_ptr = 12345678;  // Force identical RNG
            uint32_t* timer_ptr = (uint32_t*)0x470044;
            if (timer_ptr) *timer_ptr = 0;     // Force identical timer
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DETERMINISM: Memory initialization complete");
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== END INITIALIZATION ANALYSIS ===");
            
            // Initialize state manager for rollback
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing state manager...");
            if (!InitializeStateManager()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to initialize state manager");
            }
            
            // Default to local mode (offline) - can be changed later via configuration
            ConfigureNetworkMode(false, false);
            
            // Wait for game to stabilize before starting GekkoNet (critical for deterministic startup)
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Waiting for game stabilization before GekkoNet init...");
            
            // GekkoNet will be initialized immediately when first hook is called  
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialization will happen immediately at game start");
            bool gekko_result = true;  // Success since initialization is deferred to first hook call
            
            if (!gekko_result) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to initialize GekkoNet!");
                FILE* error_log = fopen(GetLogFilePath().c_str(), "a");
                if (error_log) {
                    fprintf(error_log, "ERROR FM2K HOOK: Failed to initialize GekkoNet!\n");
                    fflush(error_log);
                    fclose(error_log);
                }
                // Continue anyway - we can still hook without rollback
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Deferred GekkoNet initialization scheduled!");
            }

            // Wait a bit for the game to initialize before installing hooks
            Sleep(100);
            
            // Initialize hooks after game has had time to start
            if (!InitializeHooks()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to initialize hooks!");
                return FALSE;
            }
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: DLL initialization complete!");
            break;
        }
        
    case DLL_PROCESS_DETACH:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: DLL detaching from process");
        
        // Cleanup GekkoNet session
        if (gekko_session) {
            gekko_destroy(gekko_session);
            gekko_session = nullptr;
            gekko_initialized = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session closed");
        }
        
        // Cleanup file logging and input recording
        CleanupFileLogging();
        CleanupInputRecording();
        
        
        // Cleanup shared memory
        if (shared_memory_data) {
            UnmapViewOfFile(shared_memory_data);
            shared_memory_data = nullptr;
        }
        if (shared_memory_handle) {
            CloseHandle(shared_memory_handle);
            shared_memory_handle = nullptr;
        }
        
        ShutdownHooks();
        break;
    }
    return TRUE;
}