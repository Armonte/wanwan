#include "logging.h"
#include "globals.h"
#include "state_manager.h" // For CoreGameState

// File logging
static std::ofstream log_file;
static std::mutex log_mutex;
static bool file_logging_enabled = false;

// Input recording
static std::ofstream input_record_file;
static std::mutex input_record_mutex;
static bool input_recording_enabled = false;

void CustomLogOutput(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    std::lock_guard<std::mutex> lock(log_mutex);
    
    if (production_mode && priority > SDL_LOG_PRIORITY_WARN) {
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    struct tm* tm_info = localtime(&time_t);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
    
    char formatted_message[2048];
    snprintf(formatted_message, sizeof(formatted_message), "[%s.%03d] [Player %d] %s\n", 
             timestamp, (int)ms.count(), player_index + 1, message);
    
    printf("%s", formatted_message);
    
    if (file_logging_enabled && log_file.is_open()) {
        log_file << formatted_message;
        log_file.flush();
    }
}

void InitializeFileLogging() {
    std::lock_guard<std::mutex> lock(log_mutex);
    
    if (file_logging_enabled) return;
    
    char log_filename[128];
    snprintf(log_filename, sizeof(log_filename), "FM2K_Client%d_Debug.log", player_index + 1);
    
    log_file.open(log_filename, std::ios::out | std::ios::trunc);
    if (log_file.is_open()) {
        file_logging_enabled = true;
        SDL_SetLogOutputFunction(CustomLogOutput, nullptr);
        
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

void CleanupFileLogging() {
    std::lock_guard<std::mutex> lock(log_mutex);
    
    if (file_logging_enabled && log_file.is_open()) {
        log_file << "=== Session ended ===" << std::endl;
        log_file.close();
        file_logging_enabled = false;
        SDL_SetLogOutputFunction(nullptr, nullptr);
    }
}

void InitializeInputRecording() {
    std::lock_guard<std::mutex> lock(input_record_mutex);
    
    if (input_recording_enabled) return;
    
    char record_filename[128];
    snprintf(record_filename, sizeof(record_filename), "FM2K_InputRecord_Client%d.dat", player_index + 1);
    
    input_record_file.open(record_filename, std::ios::out | std::ios::binary | std::ios::trunc);
    if (input_record_file.is_open()) {
        input_recording_enabled = true;
        
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

void CleanupInputRecording() {
    std::lock_guard<std::mutex> lock(input_record_mutex);
    
    if (input_recording_enabled && input_record_file.is_open()) {
        input_record_file.close();
        input_recording_enabled = false;
    }
}

void RecordInput(uint32_t frame, uint32_t p1_input, uint32_t p2_input) {
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
    
    static uint32_t flush_counter = 0;
    if (++flush_counter % 100 == 0) {
        input_record_file.flush();
    }
}

void GenerateDesyncReport(uint32_t desync_frame, uint32_t local_checksum, uint32_t remote_checksum) {
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
    
    report_file << "=== GAME STATE AT DESYNC ===" << std::endl;
    
    // ... (rest of the report generation)
    
    report_file.close();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Desync report generated: %s", report_filename);
}

void LogMinimalGameStateDesync(uint32_t desync_frame, uint32_t local_checksum, uint32_t remote_checksum) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "=== CHECKSUM STATE DESYNC ANALYSIS ===");
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Desync Frame: %u", desync_frame);
    
    // Get current checksum state for comparison using CoreGameState  
    FM2K::State::CoreGameState current_state = {};
    
    // Read the current state manually for debugging
    uint32_t* frame_ptr = (uint32_t*)FM2K::State::Memory::FRAME_COUNTER_ADDR;
    uint16_t* p1_input_ptr = (uint16_t*)FM2K::State::Memory::P1_INPUT_ADDR;
    uint16_t* p2_input_ptr = (uint16_t*)FM2K::State::Memory::P2_INPUT_ADDR;
    uint32_t* mainmenu_cursor_ptr = (uint32_t*)FM2K::State::Memory::MENU_SELECTION_ADDR;
    uint32_t* p1_css_cursor_x_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_X_ADDR;
    uint32_t* p1_css_cursor_y_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_Y_ADDR;
    uint32_t* p2_css_cursor_x_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_X_ADDR;
    uint32_t* p2_css_cursor_y_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_Y_ADDR;
    uint32_t* p1_selected_char_ptr = (uint32_t*)FM2K::State::Memory::P1_SELECTED_CHAR_ADDR;
    uint32_t* p2_selected_char_ptr = (uint32_t*)FM2K::State::Memory::P2_SELECTED_CHAR_ADDR;
    
    if (!IsBadReadPtr(p1_input_ptr, sizeof(uint16_t))) current_state.p1_input_current = *p1_input_ptr;
    if (!IsBadReadPtr(p2_input_ptr, sizeof(uint16_t))) current_state.p2_input_current = *p2_input_ptr;
    if (!IsBadReadPtr(mainmenu_cursor_ptr, sizeof(uint32_t))) current_state.menu_selection = *mainmenu_cursor_ptr;
    if (!IsBadReadPtr(p1_css_cursor_x_ptr, sizeof(uint32_t))) current_state.p1_css_cursor_x = *p1_css_cursor_x_ptr;
    if (!IsBadReadPtr(p1_css_cursor_y_ptr, sizeof(uint32_t))) current_state.p1_css_cursor_y = *p1_css_cursor_y_ptr;
    if (!IsBadReadPtr(p2_css_cursor_x_ptr, sizeof(uint32_t))) current_state.p2_css_cursor_x = *p2_css_cursor_x_ptr;
    if (!IsBadReadPtr(p2_css_cursor_y_ptr, sizeof(uint32_t))) current_state.p2_css_cursor_y = *p2_css_cursor_y_ptr;
    // DISABLED FIELDS - commenting out to match minimized state structure
    // if (!IsBadReadPtr(p1_selected_char_ptr, sizeof(uint32_t))) current_state.p1_selected_char = *p1_selected_char_ptr;
    // if (!IsBadReadPtr(p2_selected_char_ptr, sizeof(uint32_t))) current_state.p2_selected_char = *p2_selected_char_ptr;
    
    uint32_t calculated_checksum = current_state.CalculateChecksum();
    
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Current Checksum State:");
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  Inputs: P1=%u P2=%u", current_state.p1_input_current, current_state.p2_input_current);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  Main Menu Cursor: %u", current_state.menu_selection);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  P1 CSS Cursor: (%u, %u)", current_state.p1_css_cursor_x, current_state.p1_css_cursor_y);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  P2 CSS Cursor: (%u, %u)", current_state.p2_css_cursor_x, current_state.p2_css_cursor_y);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  Calculated Checksum: 0x%08X (expected: 0x%08X)", calculated_checksum, local_checksum);
    
    // Check if our calculated checksum matches the reported local checksum
    if (calculated_checksum != local_checksum) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "⚠️  WARNING: Calculated checksum doesn't match reported local checksum!");
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "⚠️  This suggests memory corruption or race condition in state capture!");
    }
    
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Checksum Comparison:");
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  Local:  0x%08X", local_checksum);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  Remote: 0x%08X", remote_checksum);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "  Diff:   0x%08X", local_checksum ^ remote_checksum);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "==============================================");
} 