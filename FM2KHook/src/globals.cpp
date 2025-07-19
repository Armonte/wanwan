#include "globals.h"

// GekkoNet session
GekkoSession* gekko_session = nullptr;
bool gekko_initialized = false;
bool gekko_session_started = false;
bool is_online_mode = false;
bool is_host = false;
uint8_t player_index = 0;
uint8_t original_player_index = 0;  // Store original before reassignment
int local_player_handle = -1;
bool use_minimal_gamestate_testing = false;
bool production_mode = false;

// Hook-related globals
uint32_t g_frame_counter = 0;
uint32_t networked_p1_input = 0;
uint32_t networked_p2_input = 0;
bool use_networked_inputs = false;
uint32_t live_p1_input = 0;
uint32_t live_p2_input = 0;
uint32_t backup_p1_input = 0;  // Raw inputs from game for debugging
uint32_t backup_p2_input = 0;

// Frame advance control (GekkoNet synchronization)
bool can_advance_frame = true;        // Allow frame advancement initially
bool waiting_for_gekko_advance = false; // Not waiting initially
bool gekko_frame_control_enabled = false; // Disabled by default, enable after GekkoNet starts

// Timeout mechanisms to prevent deadlocks
uint32_t handshake_timeout_frames = 0;    // Timeout counter for network handshake
uint32_t advance_timeout_frames = 0;      // Timeout counter for frame advance waits
uint32_t last_valid_players_frame = 0;    // Last frame when AllPlayersValid() was true

// Function pointers for original functions
ProcessGameInputsFunc original_process_inputs = nullptr;
GetPlayerInputFunc original_get_player_input = nullptr;
UpdateGameStateFunc original_update_game = nullptr;
RunGameLoopFunc original_run_game_loop = nullptr;

// State manager variables
uint32_t last_auto_save_frame = 0;
bool state_manager_initialized = false;

// Game state monitoring variables
uint32_t current_game_mode = 0xFFFFFFFF;
uint32_t current_fm2k_mode = 0xFFFFFFFF;
uint32_t current_char_select_mode = 0xFFFFFFFF;
bool rollback_active = false;
bool game_state_initialized = false; 