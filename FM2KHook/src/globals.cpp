#include "globals.h"

// GekkoNet session state
GekkoSession* gekko_session = nullptr;
bool gekko_initialized = false;
bool gekko_session_started = false;
bool is_online_mode = false;
bool is_host = false;
uint8_t player_index = 0;
uint8_t original_player_index = 0;  // Store original before reassignment
int local_player_handle = -1;
int p1_player_handle = -1;  // Handle for P1 (local session)
int p2_player_handle = -1;  // Handle for P2 (local session)
bool is_local_session = false;  // True for offline mode
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
bool can_advance_frame = false;       // Block frame advancement until GekkoNet sends AdvanceEvent
bool waiting_for_gekko_advance = false; // Not waiting initially
bool gekko_frame_control_enabled = false; // Disabled by default, enable after GekkoNet starts

// Frame stepping control
bool frame_step_paused_global = false; // Global pause flag for frame stepping
bool block_input_buffer_update = false; // Block input history buffer updates during pause

// Timeout mechanisms to prevent deadlocks
uint32_t handshake_timeout_frames = 0;    // Timeout counter for network handshake
uint32_t advance_timeout_frames = 0;      // Timeout counter for frame advance waits
uint32_t last_valid_players_frame = 0;    // Last frame when AllPlayersValid() was true

// Function pointers for original functions
ProcessGameInputsFunc original_process_inputs = nullptr;
GetPlayerInputFunc original_get_player_input = nullptr;
UpdateGameStateFunc original_update_game = nullptr;
RunGameLoopFunc original_run_game_loop = nullptr;

// Additional function pointers for main loop implementation
RenderGameFunc original_render_game = nullptr;
GameRandFunc original_game_rand = nullptr;
ProcessInputHistoryFunc original_process_input_history = nullptr;
CheckGameContinueFunc original_check_game_continue = nullptr;

// Global variables for manual save/load requests
bool manual_save_requested = false;
bool manual_load_requested = false;
uint32_t target_save_slot = 0;
uint32_t target_load_slot = 0;

// Deterministic RNG state
uint32_t deterministic_rng_seed = 12345678;
bool use_deterministic_rng = false;

// CSS Input injection system
DelayedInput css_delayed_inputs[2] = {{0, 0, false}, {0, 0, false}};

// State manager variables
uint32_t last_auto_save_frame = 0;
bool state_manager_initialized = false;

// Game state monitoring variables
uint32_t current_game_mode = 0xFFFFFFFF;
uint32_t current_fm2k_mode = 0xFFFFFFFF;
uint32_t current_char_select_mode = 0xFFFFFFFF;
bool rollback_active = false;
bool game_state_initialized = false;

