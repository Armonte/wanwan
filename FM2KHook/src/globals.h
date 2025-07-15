#pragma once

#include "common.h"
#include "state_manager.h" // For GameState and CoreGameState
#include "gekkonet.h"

// Key FM2K addresses
namespace FM2K::State::Memory {
    constexpr uintptr_t PROCESS_INPUTS_ADDR = 0x4146D0;
    constexpr uintptr_t GET_PLAYER_INPUT_ADDR = 0x414340;
    constexpr uintptr_t UPDATE_GAME_ADDR = 0x404CD0;
    constexpr uintptr_t RUN_GAME_LOOP_ADDR = 0x405AD0;
    constexpr uintptr_t FRAME_COUNTER_ADDR = 0x447EE0;
    constexpr uintptr_t P1_INPUT_ADDR = 0x4259C0;
    constexpr uintptr_t P2_INPUT_ADDR = 0x4259C4;
    constexpr uintptr_t P1_HP_ADDR = 0x47010C;
    constexpr uintptr_t P2_HP_ADDR = 0x47030C;
    constexpr uintptr_t ROUND_TIMER_ADDR = 0x470060;
    constexpr uintptr_t GAME_TIMER_ADDR = 0x470044;
    constexpr uintptr_t RANDOM_SEED_ADDR = 0x41FB1C;
    constexpr uintptr_t PLAYER_DATA_SLOTS_ADDR = 0x4D1D80;
    constexpr size_t PLAYER_DATA_SLOTS_SIZE = 0x701F8;
    constexpr uintptr_t GAME_OBJECT_POOL_ADDR = 0x4701E0;
    constexpr size_t GAME_OBJECT_POOL_SIZE = 0x5F800;
    constexpr uintptr_t GAME_MODE_ADDR = 0x470054;
    constexpr uintptr_t FM2K_GAME_MODE_ADDR = 0x470040;        // g_fm2k_game_mode  
    constexpr uintptr_t CHARACTER_SELECT_MODE_ADDR = 0x470058; // g_character_select_mode_flag
    constexpr uintptr_t REPLAY_MODE_ADDR = 0x4701C0;           // g_replay_mode
    constexpr uintptr_t ROUND_SETTING_ADDR = 0x470068;
    constexpr uintptr_t P1_ROUND_COUNT_ADDR = 0x4700EC;
    constexpr uintptr_t P1_ROUND_STATE_ADDR = 0x4700F0;
    constexpr uintptr_t P1_ACTION_STATE_ADDR = 0x47019C;
    constexpr uintptr_t P2_ACTION_STATE_ADDR = 0x4701A0;
    constexpr uintptr_t CAMERA_X_ADDR = 0x447F2C;
    constexpr uintptr_t CAMERA_Y_ADDR = 0x447F30;
    constexpr uintptr_t TIMER_COUNTDOWN1_ADDR = 0x4456E4;
    constexpr uintptr_t TIMER_COUNTDOWN2_ADDR = 0x447D91;
    constexpr uintptr_t OBJECT_LIST_HEADS_ADDR = 0x430240;
    constexpr uintptr_t OBJECT_LIST_TAILS_ADDR = 0x430244;
    constexpr uintptr_t ROUND_TIMER_COUNTER_ADDR = 0x424F00;
    
    // Character Select Menu State (discovered via Cheat Engine)
    constexpr uintptr_t MENU_SELECTION_ADDR = 0x424780;        // g_menu_selection (main menu cursor)
    constexpr uintptr_t P1_CSS_CURSOR_X_ADDR = 0x424E50;       // p1Cursor X (column) - 4 bytes
    constexpr uintptr_t P1_CSS_CURSOR_Y_ADDR = 0x424E54;       // p1Cursor Y (row) - 4 bytes
    constexpr uintptr_t P2_CSS_CURSOR_X_ADDR = 0x424E58;       // p2Cursor X (column) - 4 bytes  
    constexpr uintptr_t P2_CSS_CURSOR_Y_ADDR = 0x424E5C;       // p2Cursor Y (row) - 4 bytes
    constexpr uintptr_t P1_SELECTED_CHAR_ADDR = 0x470020;      // p1CharToDisplayAndLoad
    constexpr uintptr_t P2_SELECTED_CHAR_ADDR = 0x470024;      // p2CharToDisplayAndLoad
    constexpr uintptr_t P1_CHAR_RELATED_ADDR = 0x4CF960;       // u_p1_related
    constexpr uintptr_t P2_CHAR_RELATED_ADDR = 0x4CF964;       // u_p2_related
}

// Global variables
extern GekkoSession* gekko_session;
extern bool gekko_initialized;
extern bool gekko_session_started;
extern bool gekko_session_active;  // BSNES PATTERN: Single unified session state
extern uint32_t synchronized_frame;  // BSNES PATTERN: GekkoNet-synchronized frame counter
extern bool is_online_mode;
extern bool is_host;
extern uint8_t player_index;
extern uint8_t original_player_index;  // Store original before reassignment
extern int local_player_handle;
extern bool use_minimal_gamestate_testing;
extern bool production_mode;

// Hook-related globals
extern uint32_t g_frame_counter;
extern uint32_t networked_p1_input;
extern uint32_t networked_p2_input;
extern bool use_networked_inputs;
extern uint32_t live_p1_input;
extern uint32_t live_p2_input;

// Frame advance control (GekkoNet synchronization)
extern bool can_advance_frame;        // Flag to control FM2K frame advancement
extern bool waiting_for_gekko_advance; // True when waiting for GekkoNet AdvanceEvent

// Timeout mechanisms to prevent deadlocks
extern uint32_t handshake_timeout_frames;    // Timeout counter for network handshake
extern uint32_t advance_timeout_frames;      // Timeout counter for frame advance waits
extern uint32_t last_valid_players_frame;    // Last frame when AllPlayersValid() was true

// BSNES PATTERN: Frame drift correction system
struct FrameAdvantageHistory {
    static const int HISTORY_SIZE = 26;   // ~26 frame rolling average like BSNES
    float local_advantage[HISTORY_SIZE];  // Local frame advantage history
    float remote_advantage[HISTORY_SIZE]; // Remote frame advantage history  
    int history_index = 0;                // Current index in circular buffer
    uint32_t drift_check_counter = 0;     // Counter for periodic drift checks (every 180 frames)
    bool drift_correction_active = false; // Flag to prevent multiple corrections
    
    float GetAverageAdvantage() const;     // Calculate rolling average of frame advantage
    void AddAdvantage(float local_adv, float remote_adv); // Add new advantage reading
};
extern FrameAdvantageHistory frame_advantage_history;

// Function pointers for original functions
typedef int(__cdecl* ProcessGameInputsFunc)();
typedef int(__cdecl* GetPlayerInputFunc)(int playerIndex, int inputType);
typedef int(__cdecl* UpdateGameStateFunc)();
typedef BOOL(__cdecl* RunGameLoopFunc)();

extern ProcessGameInputsFunc original_process_inputs;
extern GetPlayerInputFunc original_get_player_input;
extern UpdateGameStateFunc original_update_game_state;
extern RunGameLoopFunc original_run_game_loop;

// Binary patch restoration removed - we no longer hook update_game_state

// State manager variables
extern uint32_t last_auto_save_frame;
extern bool state_manager_initialized;

// Game state monitoring variables
extern uint32_t current_game_mode;
extern uint32_t current_fm2k_mode;
extern uint32_t current_char_select_mode;
extern bool rollback_active;
extern bool game_state_initialized;

// Function declarations for debugging and reporting
void GenerateDesyncReport(uint32_t desync_frame, uint32_t local_checksum, uint32_t remote_checksum);
void LogMinimalGameStateDesync(uint32_t desync_frame, uint32_t local_checksum, uint32_t remote_checksum);

// Game state management functions
void MonitorGameStateTransitions();
void ManageRollbackActivation(uint32_t game_mode, uint32_t fm2k_mode, uint32_t char_select_mode);
const char* GetGameModeString(uint32_t mode);

// BSNES PATTERN: Frame drift correction functions
void CheckFrameDrift();
void ApplyRiftSyncCorrection(float avg_advantage); 