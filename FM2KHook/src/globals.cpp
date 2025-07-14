#include "globals.h"

// GekkoNet session
GekkoSession* gekko_session = nullptr;
bool gekko_initialized = false;
bool gekko_session_started = false;
bool is_online_mode = false;
bool is_host = false;
uint8_t player_index = 0;
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

// Function pointers for original functions
ProcessGameInputsFunc original_process_inputs = nullptr;
GetPlayerInputFunc original_get_player_input = nullptr;
UpdateGameStateFunc original_update_game = nullptr;
RunGameLoopFunc original_run_game_loop = nullptr;

// State manager variables
uint32_t last_auto_save_frame = 0;
bool state_manager_initialized = false; 