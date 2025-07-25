#include "input_handler.h"
#include "globals.h"
#include "gekkonet_hooks.h"
#include "logging.h"
#include "shared_mem.h"
#include "savestate.h"
#include "debug_features.h"
#include <windows.h>
#include <SDL3/SDL.h>

// Input buffer write patches for motion input preservation
static bool buffer_writes_patched = false;
static uint8_t original_bytes_1[7] = {0};
static uint8_t original_bytes_2[7] = {0};

void PatchInputBufferWrites(bool block) {
    // Addresses where process_game_inputs writes to input history buffer
    uint8_t* write_addr_1 = (uint8_t*)0x41472E;
    uint8_t* write_addr_2 = (uint8_t*)0x41474F;
    
    if (block && !buffer_writes_patched) {
        // Save original bytes
        memcpy(original_bytes_1, write_addr_1, 7);
        memcpy(original_bytes_2, write_addr_2, 7);
        
        // Make memory writable
        DWORD old_protect;
        VirtualProtect(write_addr_1, 7, PAGE_EXECUTE_READWRITE, &old_protect);
        VirtualProtect(write_addr_2, 7, PAGE_EXECUTE_READWRITE, &old_protect);
        
        // Patch to NOPs
        memset(write_addr_1, 0x90, 7); // NOP the mov instruction
        memset(write_addr_2, 0x90, 7); // NOP the mov instruction
        
        buffer_writes_patched = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FRAME STEP: Patched input buffer writes - motion inputs preserved");
    } else if (!block && buffer_writes_patched) {
        // Restore original bytes
        DWORD old_protect;
        VirtualProtect(write_addr_1, 7, PAGE_EXECUTE_READWRITE, &old_protect);
        VirtualProtect(write_addr_2, 7, PAGE_EXECUTE_READWRITE, &old_protect);
        
        memcpy(write_addr_1, original_bytes_1, 7);
        memcpy(write_addr_2, original_bytes_2, 7);
        
        buffer_writes_patched = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FRAME STEP: Restored input buffer writes - normal operation");
    }
}

// ARCHITECTURE FIX: Real input capture following CCCaster/GekkoNet pattern
void CaptureRealInputs() {
    // In online mode, we only read the input for the local player.
    // In true offline (local VS) mode, we read both.
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);

    if (original_get_player_input) {
        if (is_true_offline) {
            // TRUE OFFLINE: Read both players from local hardware.
            live_p1_input = original_get_player_input(0, 0);
            live_p2_input = original_get_player_input(1, 0);
        } else {
            // ONLINE: Both host and client read their local controls from the P1 slot.
            // The netcode layer (GekkoNet) will map this to the correct player in-game.
            uint32_t local_hardware_input = original_get_player_input(0, 0);

            if (::is_host) {
                live_p1_input = local_hardware_input;
                live_p2_input = 0;
                if (local_hardware_input != 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT CAPTURE: Host P1 input=0x%03X (is_host=%s, player_index=%d)", 
                               local_hardware_input, ::is_host ? "YES" : "NO", ::player_index);
                }
            } else {
                live_p1_input = 0;
                // The client's local input becomes P2's input in the session.
                live_p2_input = local_hardware_input;
                if (local_hardware_input != 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT CAPTURE: Client P2 input=0x%03X (is_host=%s, player_index=%d)", 
                               local_hardware_input, ::is_host ? "YES" : "NO", ::player_index);
                }
            }
        }

        // The P2 left/right bit swap is a hardware/engine quirk, apply it whenever P2 input is generated.
        // This needs to happen for the client's input as it will control the P2 character.

    } else {
        live_p1_input = 0;
        live_p2_input = 0;
    }

    // Debug logging for button issues
    static uint32_t debug_counter = 0;
    if (debug_counter++ % 60 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "2DFM INPUT: P1=0x%03X P2=0x%03X",
                   live_p1_input & 0x7FF, live_p2_input & 0x7FF);
    }
}

// NEW GEKKONET INTEGRATION: SDL keyboard polling for clean input source
uint16_t PollSDLKeyboard() {
    // Get current SDL keyboard state (SDL3 returns const bool*)
    const bool* keys = SDL_GetKeyboardState(NULL);
    uint16_t input = 0;
    
    // Debug: Check if SDL_GetKeyboardState is working
    static uint32_t debug_call_count = 0;
    debug_call_count++;
    
    if (!keys) {
        if (debug_call_count % 300 == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL ERROR: SDL_GetKeyboardState returned NULL!");
        }
        return 0;
    }
    
    // Debug: Log that SDL is working (first few calls)
    if (debug_call_count <= 3) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL DEBUG: PollSDLKeyboard() call #%d - SDL_GetKeyboardState working", debug_call_count);
    }
    
    // Map SDL scancodes to FM2K input bits
    // Using standard arrow keys + Z/X/C/A/S/D for 6 buttons
    if (keys[SDL_SCANCODE_LEFT])  input |= 0x001;  // LEFT
    if (keys[SDL_SCANCODE_RIGHT]) input |= 0x002;  // RIGHT  
    if (keys[SDL_SCANCODE_UP])    input |= 0x004;  // UP
    if (keys[SDL_SCANCODE_DOWN])  input |= 0x008;  // DOWN
    if (keys[SDL_SCANCODE_Z])     input |= 0x010;  // BUTTON1
    if (keys[SDL_SCANCODE_X])     input |= 0x020;  // BUTTON2
    if (keys[SDL_SCANCODE_C])     input |= 0x040;  // BUTTON3
    if (keys[SDL_SCANCODE_A])     input |= 0x080;  // BUTTON4
    if (keys[SDL_SCANCODE_S])     input |= 0x100;  // BUTTON5
    if (keys[SDL_SCANCODE_D])     input |= 0x200;  // BUTTON6
    if (keys[SDL_SCANCODE_Q])     input |= 0x400;  // BUTTON7
    
    // Debug: Log key state when any key is pressed OR every 300 calls
    static uint32_t last_input = 0;
    if (input != last_input || debug_call_count % 300 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                   "SDL KEYS: L=%d R=%d U=%d D=%d Z=%d X=%d -> input=0x%03X", 
                   keys[SDL_SCANCODE_LEFT] ? 1 : 0,
                   keys[SDL_SCANCODE_RIGHT] ? 1 : 0, 
                   keys[SDL_SCANCODE_UP] ? 1 : 0,
                   keys[SDL_SCANCODE_DOWN] ? 1 : 0,
                   keys[SDL_SCANCODE_Z] ? 1 : 0,
                   keys[SDL_SCANCODE_X] ? 1 : 0,
                   input);
        last_input = input;
    }
    
    return input;
}

// Our simplified get_player_input replacement - no joystick, just keyboard
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    static uint32_t call_count = 0;
    
    // Get our Windows keyboard input - replicate original function behavior
    int input_mask = 0;
    
    // Directional inputs (same bit pattern as original)
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)   input_mask |= 0x008;
    if (GetAsyncKeyState(VK_UP) & 0x8000)     input_mask |= 0x004;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)   input_mask |= 0x001;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000)  input_mask |= 0x002;
    
    // Button inputs (same bit pattern as original)
    if (GetAsyncKeyState('Z') & 0x8000)       input_mask |= 0x010;
    if (GetAsyncKeyState('X') & 0x8000)       input_mask |= 0x020;
    if (GetAsyncKeyState('C') & 0x8000)       input_mask |= 0x040;
    if (GetAsyncKeyState('A') & 0x8000)       input_mask |= 0x080;
    if (GetAsyncKeyState('S') & 0x8000)       input_mask |= 0x100;
    if (GetAsyncKeyState('D') & 0x8000)       input_mask |= 0x200;
    if (GetAsyncKeyState('Q') & 0x8000)       input_mask |= 0x400;
    
    // Debug logging
    if (++call_count % 100 == 0 || input_mask != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hook_GetPlayerInput: player=%d, type=%d, mask=0x%03X", 
                   player_id, input_type, input_mask);
    }
    
    // Just return the input mask like the original function
    // Let process_game_inputs handle writing to the arrays
    return input_mask;
}

// Proper input bit mapping
uint32_t ConvertNetworkInputToGameFormat(uint32_t network_input) {
    uint32_t game_input = 0;
    
    if (network_input & 0x001) game_input |= 0x001;  // LEFT
    if (network_input & 0x002) game_input |= 0x002;  // RIGHT
    if (network_input & 0x004) game_input |= 0x004;  // UP
    if (network_input & 0x008) game_input |= 0x008;  // DOWN
    if (network_input & 0x010) game_input |= 0x010;  // BUTTON1
    if (network_input & 0x020) game_input |= 0x020;  // BUTTON2
    if (network_input & 0x040) game_input |= 0x040;  // BUTTON3
    if (network_input & 0x080) game_input |= 0x080;  // BUTTON4
    if (network_input & 0x100) game_input |= 0x100;  // BUTTON5
    if (network_input & 0x200) game_input |= 0x200;  // BUTTON6
    if (network_input & 0x400) game_input |= 0x400;  // BUTTON7 (7th button at bit 1024)
    
    return game_input;
}

// NEW GEKKONET INTEGRATION: Complete replacement for process_game_inputs
// Follows BSNES-netplay pattern: GekkoNet for input sync + FM2K's processing logic
int __cdecl FM2K_ProcessGameInputs_GekkoNet() {
    // COMPLETE REIMPLEMENTATION: Following exact original algorithm from analysis
    static uint32_t call_count = 0;
    if (++call_count <= 3 || call_count % 100 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "COMPLETE REIMPL called #%d", call_count);
    }
    
    // BSNES-STYLE FRAME CONTROL: Process GekkoNet and block if no AdvanceEvent
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
    bool dual_client_mode = (::player_index == 0 || ::player_index == 1);
    bool use_gekko = !is_true_offline || dual_client_mode;
    
    if (use_gekko && gekko_initialized && gekko_session) {
        // Run the GekkoNet processing (from Hook_ProcessGameInputs)
        ProcessGekkoNetFrame();
        
        // CRITICAL: Block frame advancement if no AdvanceEvent
        if (!can_advance_frame) {
            static uint32_t block_counter = 0;
            if (++block_counter % 120 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: BLOCKING FRAME #%d - waiting for AdvanceEvent [session_started=%s]", 
                           block_counter, gekko_session_started ? "YES" : "NO");
            }
            return 0; // Don't process this frame - this is true frame control
        }
    }
    
    // ===== PHASE 1: Input Capture Phase (exactly like original) =====
    
    // 1. Keyboard State Capture (0x4146d9) - CRITICAL for graphics!
    static BYTE KeyState[256];
    GetKeyboardState(KeyState);
    
    // 2. Frame Counter Management (0x4146f4) - USE GAME'S REAL FRAME COUNTER!
    uint32_t* game_frame_counter_ptr = (uint32_t*)0x447ee0;
    uint32_t current_frame = (*game_frame_counter_ptr + 1) & 0x3FF; // Read game's counter and increment
    *game_frame_counter_ptr = current_frame; // Write back to game's counter
    g_frame_counter = current_frame; // Also update our local copy for logging
    
    // 3. Input Buffer Initialization (0x4146fb)
    uint32_t* g_p1_input = (uint32_t*)0x4259c0;      // g_p1_input[8] 
    uint32_t* g_p2_input_ptr = (uint32_t*)0x4259c4;   // g_p2_input
    uint32_t* g_player_input_history = (uint32_t*)0x4280e0; // Input history[1024]
    uint32_t* g_p2_input_history = (uint32_t*)0x4290e0;     // P2 history[1024]
    
    memset(g_p1_input, 0, 0x20u); // Clear P1 input buffer (32 bytes)
    
    // 4. Game Mode Input Handling (0x414710) - REPLACE get_player_input with Windows
    uint32_t g_game_mode = *(uint32_t*)0x447EDC; // Game mode address
    uint8_t g_character_select_mode_flag = *(uint8_t*)0x447EE8; // Character select flag
    
    // GEKKONET INTEGRATION: Use synchronized inputs from ProcessGekkoNetFrame
    uint32_t p1_final_input = 0;
    uint32_t p2_final_input = 0;
    
    if (use_networked_inputs && gekko_session_started) {
        // Use GekkoNet synchronized inputs (BSNES approach)
        p1_final_input = networked_p1_input;
        p2_final_input = networked_p2_input;
        
        static uint32_t sync_log_counter = 0;
        if (++sync_log_counter % 300 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using GekkoNet inputs - P1=0x%04X P2=0x%04X", 
                       p1_final_input, p2_final_input);
        }
    } else {
        // Use local Windows input (fallback/offline mode)
        if (GetAsyncKeyState(VK_LEFT) & 0x8000)   p1_final_input |= 0x001;  // LEFT
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000)  p1_final_input |= 0x002;  // RIGHT
        if (GetAsyncKeyState(VK_UP) & 0x8000)     p1_final_input |= 0x004;  // UP
        if (GetAsyncKeyState(VK_DOWN) & 0x8000)   p1_final_input |= 0x008;  // DOWN
        if (GetAsyncKeyState('Z') & 0x8000)       p1_final_input |= 0x010;  // BUTTON1
        if (GetAsyncKeyState('X') & 0x8000)       p1_final_input |= 0x020;  // BUTTON2
        if (GetAsyncKeyState('C') & 0x8000)       p1_final_input |= 0x040;  // BUTTON3
        if (GetAsyncKeyState('A') & 0x8000)       p1_final_input |= 0x080;  // BUTTON4
        if (GetAsyncKeyState('S') & 0x8000)       p1_final_input |= 0x100;  // BUTTON5
        if (GetAsyncKeyState('D') & 0x8000)       p1_final_input |= 0x200;  // BUTTON6
        
        // For now, P2 is controlled by P1 in offline mode
        p2_final_input = 0;
    }
    
    // ===== PHASE 2: Input Assignment to Game Memory =====
    
    if (g_game_mode < 3000 || g_character_select_mode_flag) {
        // Versus mode - assign both P1 and P2 inputs
        g_p1_input[0] = p1_final_input;
        g_player_input_history[g_frame_counter] = p1_final_input;
        *g_p2_input_ptr = p2_final_input;
        g_p2_input_history[g_frame_counter] = p2_final_input;
    } else {
        // Story mode - only P1 input
        g_p1_input[0] = p1_final_input;
        g_player_input_history[g_frame_counter] = p1_final_input;
    }
    
    // ===== PHASE 2: Input Processing Phase - FULL REPEAT LOGIC =====
    
    // Static local arrays for repeat logic state
    static uint32_t g_prev_input_state[8] = {0};         // Previous frame input states
    static uint32_t g_input_repeat_state[8] = {0};       // Current repeat states
    static uint32_t g_input_repeat_timer[8] = {0};       // Timers for repeat logic
    
    // CRITICAL: Use actual game memory addresses that CSS reads from!
    uint32_t* g_player_input_processed = (uint32_t*)0x447f40;  // g_player_input_processed[8] array
    uint32_t* g_player_input_changes = (uint32_t*)0x447f60;    // g_player_input_changes[8] array (estimated)
    
    // Configuration values (typical fighting game values)
    const uint32_t g_input_initial_delay = 15;  // Initial delay frames
    const uint32_t g_input_repeat_delay = 4;    // Repeat delay frames
    
    uint32_t accumulated_raw_input = 0;
    uint32_t accumulated_just_pressed = 0;
    uint32_t accumulated_processed_input = 0;
    
    // Process all 8 input devices with repeat logic (exactly like original)
    for (int device_index = 0; device_index < 8; device_index++) {
        uint32_t current_raw_input = g_p1_input[device_index];
        uint32_t previous_raw_input = g_prev_input_state[device_index];
        
        // Update previous input state for next frame
        g_prev_input_state[device_index] = current_raw_input;
        
        // Detect input changes (just-pressed buttons)
        uint32_t current_input_changes = current_raw_input & (previous_raw_input ^ current_raw_input);
        g_player_input_changes[device_index] = current_input_changes;
        
        uint32_t current_processed_input;
        
        // REPEAT LOGIC: Check if input is held (same as previous repeat state)
        if (current_raw_input && current_raw_input == g_input_repeat_state[device_index]) {
            // Held input - use repeat logic
            uint32_t repeat_timer_remaining = g_input_repeat_timer[device_index] - 1;
            g_input_repeat_timer[device_index] = repeat_timer_remaining;
            
            if (repeat_timer_remaining) {
                // Timer not expired - suppress input
                current_processed_input = 0;
            } else {
                // Timer expired - allow input and reset to repeat delay
                current_processed_input = current_raw_input;
                g_input_repeat_timer[device_index] = g_input_repeat_delay;
            }
        } else {
            // New input detected - set initial delay
            current_processed_input = current_raw_input; // Allow new input immediately
            g_input_repeat_timer[device_index] = g_input_initial_delay;
            
            // Bit filtering: Filter out specific bits if previously held
            uint32_t previous_repeat_state = g_input_repeat_state[device_index];
            if ((previous_repeat_state & 3) != 0) {
                // Filter out bits 0-1 if previously held
                current_processed_input &= 0xFFFFFFFC;
            }
            if ((previous_repeat_state & 0xC) != 0) {
                // Filter out bits 2-3 if previously held  
                current_processed_input &= 0xFFFFFFF3;
            }
            
            // Update repeat state for next frame
            g_input_repeat_state[device_index] = current_raw_input;
        }
        
        g_player_input_processed[device_index] = current_processed_input;
        
        // Accumulate inputs from all devices
        accumulated_raw_input |= current_raw_input;
        accumulated_just_pressed |= current_input_changes;
        accumulated_processed_input |= current_processed_input;
        
        // Debug: Log when we actually allow input through repeat logic
        if (device_index == 0 && current_processed_input != 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "REPEAT LOGIC: Device %d - raw=0x%03X processed=0x%03X", 
                       device_index, current_raw_input, current_processed_input);
        }
    }
    
    // ===== PHASE 3: Output Phase (using CORRECT global addresses found via MCP) =====
    
    // Store final results in the REAL global variables that the game actually reads!
    *(uint32_t*)0x4cfa04 = accumulated_raw_input;       // g_combined_raw_input (REAL ADDRESS!)
    *(uint32_t*)0x4d1c20 = accumulated_processed_input; // g_combined_processed_input (REAL ADDRESS!)
    
    // CRITICAL: Write to the ACTUAL global arrays the game reads
    // g_player_input_processed and g_player_input_changes are already pointing to the right addresses
    // No need to memcpy since we wrote directly to memory addresses
    
    // Debug: Log actual addresses we're writing to
    if (accumulated_processed_input != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "OUTPUT DEBUG: Writing 0x%03X to addresses 0x4cfa04, 0x4d1c20, 0x447f40[0]", 
                   accumulated_processed_input);
    }
    
    if (call_count % 300 == 0 || accumulated_processed_input != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "COMPLETE REIMPL: Frame %u - Raw=0x%03X Processed=0x%03X JustPressed=0x%03X", 
                   g_frame_counter, accumulated_raw_input, accumulated_processed_input, accumulated_just_pressed);
    }
    
    return 32; // device_index * 4 (8 devices * 4 bytes)
}

int __cdecl Hook_ProcessGameInputs() {
    // Check for true offline mode
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
    
    // Track if we advanced frame via AdvanceEvent (to avoid double calling original_process_inputs)
    bool frame_advanced = false;

    // CRITICAL FIX: Send inputs during handshake (like BSNES)
    // Must capture and send inputs BEFORE AllPlayersValid() check to establish connection
    bool dual_client_mode = (::player_index == 0 || ::player_index == 1);
    bool use_gekko = !is_true_offline || dual_client_mode;
    
    // BSNES-STYLE GEKKONET PROCESSING
    // This is the exact equivalent of BSNES's netplayRun() function for FM2K
    if (use_gekko && gekko_initialized && gekko_session) {
        
        // STEP 1: Always capture real inputs (equivalent to BSNES netplayPollLocalInput)
        CaptureRealInputs();
        
        // STEP 2: Always send inputs to GekkoNet (equivalent to BSNES gekko_add_local_input)
        // This must happen regardless of session state to establish the connection
        if (is_local_session) {
            // Local session: Send both players' inputs
            uint16_t p1_input = (uint16_t)(live_p1_input & 0x7FF);
            uint16_t p2_input = (uint16_t)(live_p2_input & 0x7FF);
            gekko_add_local_input(gekko_session, p1_player_handle, &p1_input);
            gekko_add_local_input(gekko_session, p2_player_handle, &p2_input);
        } else {
            // Online session: Each client sends only their player's input
            if (::player_index == 0) {
                uint16_t p1_input = (uint16_t)(live_p1_input & 0x7FF);
                gekko_add_local_input(gekko_session, local_player_handle, &p1_input);
            } else {
                uint16_t p2_input = (uint16_t)(live_p2_input & 0x7FF);
                gekko_add_local_input(gekko_session, local_player_handle, &p2_input);
            }
        }
        
        // STEP 3: Always process connection events (equivalent to BSNES gekko_session_events)
        int event_count = 0;
        auto events = gekko_session_events(gekko_session, &event_count);
        for (int i = 0; i < event_count; i++) {
            auto event = events[i];
            if (event->type == PlayerConnected) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player Connected - handle %d", event->data.connected.handle);
            } else if (event->type == PlayerDisconnected) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player Disconnected - handle %d", event->data.disconnected.handle);
            } else if (event->type == SessionStarted) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session Started!");
                gekko_session_started = true;
                gekko_frame_control_enabled = true;
            }
        }
        
        // STEP 4: Always process updates (SaveEvent, LoadEvent, AdvanceEvent)
        // This is the core of BSNES gekko_update_session processing
        gekko_network_poll(gekko_session);
        int update_count = 0;
        auto updates = gekko_update_session(gekko_session, &update_count);
        
        // Reset frame advance flag - will be set by AdvanceEvent if we should advance
        can_advance_frame = false;
        use_networked_inputs = false;
        
        for (int i = 0; i < update_count; i++) {
            auto update = updates[i];
            switch (update->type) {
                case SaveEvent:
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SaveEvent frame %d", update->data.save.frame);
                    // Minimal state like BSNES - just 4 bytes
                    *update->data.save.checksum = 0;
                    *update->data.save.state_len = sizeof(int32_t);
                    memcpy(update->data.save.state, &update->data.save.frame, sizeof(int32_t));
                    break;
                    
                case LoadEvent:
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: LoadEvent frame %d", update->data.load.frame);
                    // TODO: Implement rollback state loading
                    break;
                    
                case AdvanceEvent:
                    // This is the key - only advance when GekkoNet says so (like BSNES emulator->run())
                    can_advance_frame = true;
                    use_networked_inputs = true;
                    gekko_frame_control_enabled = true;
                    
                    // Copy networked inputs from GekkoNet (like BSNES memcpy)
                    if (update->data.adv.inputs && update->data.adv.input_len >= sizeof(uint16_t) * 2) {
                        uint16_t* networked_inputs = (uint16_t*)update->data.adv.inputs;
                        static uint16_t p1_networked_input = 0;
                        static uint16_t p2_networked_input = 0;
                        p1_networked_input = networked_inputs[0];
                        p2_networked_input = networked_inputs[1];
                        
                        static uint32_t advance_counter = 0;
                        if (++advance_counter % 300 == 0) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent #%d - P1=0x%04X P2=0x%04X", 
                                       advance_counter, p1_networked_input, p2_networked_input);
                        }
                    }
                    break;
            }
        }
        
        // BSNES-STYLE BLOCKING: Only advance frame if we got AdvanceEvent
        // This is the critical control point - like BSNES emulator->run()
        // Block regardless of session state - if GekkoNet is active, we wait for AdvanceEvent
        if (!can_advance_frame) {
            static uint32_t input_block_counter = 0;
            if (++input_block_counter % 120 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: BLOCKING FRAME - waiting for AdvanceEvent (#%d) [session_started=%s]", 
                           input_block_counter, gekko_session_started ? "YES" : "NO");
            }
            // This is the key - don't call original_process_inputs() when no AdvanceEvent
            return 0;
        }
    }

    // DEBUG: Log frame counters to understand the difference
    static uint32_t input_call_count = 0;
    if (++input_call_count % 100 == 0) { // Log every 100 calls to avoid spam
        uint32_t input_buffer_frame = *(uint32_t*)0x004EF1A4; // Input buffer circular index
        uint32_t render_frame = *(uint32_t*)0x4456FC; // Render frame counter
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "Hook_ProcessGameInputs() called #%d - input_buffer_frame=%u, render_frame=%u - gekko_frame_control_enabled=%s, gekko_session_started=%s, can_advance_frame=%s", 
            input_call_count, input_buffer_frame, render_frame,
            gekko_frame_control_enabled ? "YES" : "NO",
            gekko_session_started ? "YES" : "NO",
            can_advance_frame ? "YES" : "NO");
    }
    
    // CORRECT APPROACH: Following OnlineSession example - NO BLOCKING
    // Let the game run normally and just process synchronized inputs on AdvanceEvents
    
    // FRAME STEPPING: This is the main control point since it's called repeatedly in the game loop
    // Get shared memory for frame stepping control
    SharedInputData* shared_data = GetSharedMemory();
    
    // Initialize GekkoNet on first input hook call (safer than main loop hook)
    // DUAL CLIENT TESTING: Always initialize GekkoNet when player_index is set (dual client mode)
    // Only skip GekkoNet for true single-client offline mode
    bool skip_gekko = is_true_offline && !dual_client_mode;
    
    if (!gekko_initialized && !skip_gekko) {
        static bool initialization_attempted = false;
        if (!initialization_attempted) {
            initialization_attempted = true;
            if (dual_client_mode) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: DUAL CLIENT mode detected (player_index=%d) - initializing GekkoNet...", ::player_index);
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: First call - initializing GekkoNet...");
            }
            
            if (InitializeGekkoNet()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: GekkoNet initialized successfully from input hook");
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: GekkoNet initialization failed");
            }
        }
    } else if (skip_gekko) {
        static bool offline_log_shown = false;
        if (!offline_log_shown) {
            offline_log_shown = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: SINGLE CLIENT offline mode - skipping GekkoNet initialization completely");
        }
    }
    
    // Wait for GekkoNet connection (moved from main loop hook) - REMOVED, handled by AllPlayersValid() now
    
    // DEBUG: Log that input hook is being called
    static uint32_t input_hook_call_count = 0;
    input_hook_call_count++;
    // Disabled verbose input hook logging
    // if (input_hook_call_count % 100 == 0) { // Log every 100 calls to avoid spam
    //     SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Called %u times, frame %u", input_hook_call_count, g_frame_counter);
    // }
    
    // ARCHITECTURE FIX: Process debug commands (including save/load) BEFORE the pause check
    // This allows save/load to work even when the game is paused
    CheckForDebugCommands();
    CheckForHotkeys(); // Check for keyboard hotkeys for save/load and frame stepping
    ProcessManualSaveLoadRequests();
    
    // Check for frame stepping commands
    if (shared_data) {
        // ONE-TIME-FIX: Handle the initial state where memset sets remaining_frames to 0.
        // This state should mean "running indefinitely".
        static bool initial_state_fixed = false;
        if (!initial_state_fixed && !shared_data->frame_step_is_paused && shared_data->frame_step_remaining_frames == 0) {
            shared_data->frame_step_remaining_frames = UINT32_MAX;
            initial_state_fixed = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Corrected initial frame step state to RUNNING.");
        }

        // DEBUG: Log frame stepping state
        if (shared_data->frame_step_pause_requested || 
            shared_data->frame_step_resume_requested || 
            shared_data->frame_step_single_requested || 
            shared_data->frame_step_multi_count > 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame stepping command detected - pause=%d, resume=%d, single=%d, multi=%u", 
                       shared_data->frame_step_pause_requested,
                       shared_data->frame_step_resume_requested,
                       shared_data->frame_step_single_requested,
                       shared_data->frame_step_multi_count);
        }
        
        // Always log single step requests for debugging
        if (shared_data->frame_step_single_requested) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: SINGLE STEP REQUEST DETECTED at frame %u", g_frame_counter);
        }
        
        // Handle frame stepping commands
        if (shared_data->frame_step_pause_requested) {
            frame_step_paused_global = true;
            shared_data->frame_step_is_paused = true;
            shared_data->frame_step_pause_requested = false;
            shared_data->frame_step_remaining_frames = 0; // No stepping, just pause
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame stepping PAUSED at frame %u", g_frame_counter);
        }
        if (shared_data->frame_step_resume_requested) {
            frame_step_paused_global = false;
            shared_data->frame_step_is_paused = false;
            shared_data->frame_step_resume_requested = false;
            shared_data->frame_step_remaining_frames = UINT32_MAX; // Use sentinel for "running"
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame stepping RESUMED at frame %u", g_frame_counter);
        }
        if (shared_data->frame_step_single_requested) {
            // Single step: run one frame then pause
            shared_data->frame_step_single_requested = false;
            frame_step_paused_global = false; // Allow one frame
            shared_data->frame_step_is_paused = false;
            shared_data->frame_step_remaining_frames = 1; // Allow exactly 1 frame
            shared_data->frame_step_needs_input_refresh = true; // Capture fresh inputs right before execution
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: SINGLE STEP ENABLED - allowing 1 frame at frame %u", g_frame_counter);
        }
        // Multi-step disabled - focus on single step only
        if (shared_data->frame_step_multi_count > 0) {
            shared_data->frame_step_multi_count = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Multi-step disabled - use single step instead");
        }
        
        // If paused, keep input system alive but preserve motion input buffer SURGICALLY
        if (frame_step_paused_global && shared_data->frame_step_is_paused) {
            // SURGICAL APPROACH: Keep input system alive but prevent frame counter advancement
            
            // Save critical state that original_process_inputs modifies
            uint32_t* frame_counter_ptr = (uint32_t*)0x447EE0;  // g_frame_counter
            uint32_t* p1_history_ptr = (uint32_t*)0x4280E0;     // g_player_input_history  
            uint32_t* p2_history_ptr = (uint32_t*)0x4284E0;     // g_p2_input_history (assuming offset)
            
            uint32_t saved_frame_counter = 0;
            uint32_t saved_p1_history = 0;
            uint32_t saved_p2_history = 0;
            
            if (!IsBadReadPtr(frame_counter_ptr, sizeof(uint32_t))) {
                saved_frame_counter = *frame_counter_ptr;
                
                // Save the input history entries that would be overwritten
                uint32_t next_frame_index = (saved_frame_counter + 1) & 0x3FF;
                if (!IsBadReadPtr(p1_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    saved_p1_history = p1_history_ptr[next_frame_index];
                }
                if (!IsBadReadPtr(p2_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    saved_p2_history = p2_history_ptr[next_frame_index];
                }
            }
            
            // Update input system to keep it alive
            CaptureRealInputs();
            if (original_process_inputs) {
                original_process_inputs();
            }
            
            // Restore the critical state to preserve motion inputs
            if (!IsBadWritePtr(frame_counter_ptr, sizeof(uint32_t))) {
                *frame_counter_ptr = saved_frame_counter;  // Undo frame counter advance
                
                // Restore the input history entries
                uint32_t next_frame_index = (saved_frame_counter + 1) & 0x3FF;
                if (!IsBadWritePtr(p1_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    p1_history_ptr[next_frame_index] = saved_p1_history;
                }
                if (!IsBadWritePtr(p2_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    p2_history_ptr[next_frame_index] = saved_p2_history;
                }
            }
            
            return 0; // Block game advancement but keep inputs fresh
        }
        
        // CRITICAL: Inputs are now captured at the top of the function.
        
        // Handle frame stepping countdown AFTER processing the frame
        // This ensures the frame actually gets processed before we count it down
    }
    
    // Normal input capture - but skip if we're going to do fresh capture right before execution
    if (!shared_data || !shared_data->frame_step_needs_input_refresh) {
        static int capture_log = 0;
        if (++capture_log % 30 == 0) {  // More frequent logging
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Calling CaptureRealInputs() - shared_data=%p, frame_step_needs_input_refresh=%s", 
                       shared_data, shared_data ? (shared_data->frame_step_needs_input_refresh ? "YES" : "NO") : "N/A");
        }
        CaptureRealInputs();
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Skipping normal capture, will do fresh capture before execution at frame %u", g_frame_counter);
    }
    
    
    // TRUE OFFLINE MODE: Handle completely without GekkoNet
    if (is_true_offline) {
        
        // FIXED: Increment frame counter BEFORE processing to fix 1-frame input delay
        g_frame_counter++;
        
        // RADICAL SOLUTION: Call original_process_inputs TWICE on step frames to eliminate delay
        if (shared_data && shared_data->frame_step_needs_input_refresh) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: DOUBLE CALL to eliminate 1-frame delay at frame %u", g_frame_counter);
            
            // Capture fresh inputs with detailed logging
            uint32_t old_p1 = live_p1_input;
            uint32_t old_p2 = live_p2_input;
            CaptureRealInputs();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Step capture - P1: 0x%03X->0x%03X, P2: 0x%03X->0x%03X", 
                       old_p1, live_p1_input, old_p2, live_p2_input);
            
            // First call: This "primes" the input system with current inputs
            if (original_process_inputs) {
                original_process_inputs();
            }
            
            // Second call: This ensures the inputs are processed for THIS frame, not next
            if (original_process_inputs) {
                original_process_inputs();
            }
            
            shared_data->frame_step_needs_input_refresh = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Double call complete - inputs should be immediate");
        } else {
            // Normal single call
            if (original_process_inputs) {
                original_process_inputs();
            }
        }
        
        // UNIFIED LOGIC: Handle frame stepping countdown. Re-pausing is now in the render hook.
        if (shared_data && shared_data->frame_step_remaining_frames > 0 && shared_data->frame_step_remaining_frames != UINT32_MAX) {
            // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame ADVANCED to %u during stepping (normal path)", g_frame_counter);
            shared_data->frame_step_remaining_frames--;
            if (shared_data->frame_step_remaining_frames == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Step processing complete for frame %u, will pause in render hook", g_frame_counter);
            }
        }
        
        // TRUE OFFLINE: Early return to prevent double frame execution
        return 0;
    }
    
    
    // CORRECT GEKKONET PROCESSING: Following bsnes-netplay pattern
    // Game runs normally, GekkoNet processes events each frame and provides synchronized inputs
    if (use_gekko && gekko_initialized && gekko_session && gekko_session_started) {
        
        // Inputs already sent during handshake phase above
        
        // Process GekkoNet events and advance frame (like bsnes-netplay)
        gekko_network_poll(gekko_session);
        
        // Process session events (like bsnes-netplay)
        int session_event_count = 0;
        auto session_events = gekko_session_events(gekko_session, &session_event_count);
        for (int i = 0; i < session_event_count; i++) {
            auto event = session_events[i];
            if (event->type == SessionStarted) {
                // This is when the session actually starts (like bsnes-netplay)
                gekko_session_started = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session started!");
            } else if (event->type == DesyncDetected) {
                auto desync = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet DESYNC detected at frame %d", desync.frame);
            } else if (event->type == PlayerDisconnected) {
                auto disco = event->data.disconnected;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "DISCONNECT: handle %d", disco.handle);
            }
        }
        
        // Process updates (like bsnes-netplay) - ONLY if session is started
        if (gekko_session_started) {
            int update_count = 0;
            auto updates = gekko_update_session(gekko_session, &update_count);
            
            for (int i = 0; i < update_count; i++) {
                auto update = updates[i];
                
                switch (update->type) {
                    case AdvanceEvent: {
                        // This is the authoritative event that drives the game forward (like bsnes-netplay)
                        uint16_t received_p1 = ((uint16_t*)update->data.adv.inputs)[0];
                        uint16_t received_p2 = ((uint16_t*)update->data.adv.inputs)[1];
                        
                        static uint32_t debug_count = 0;
                        if (++debug_count % 300 == 0) { // Log every 5 seconds
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT DEBUG: AdvanceEvent received inputs[0]=0x%03X, inputs[1]=0x%03X", 
                                        received_p1, received_p2);
                        }
                        
                        // BSNES APPROACH: Simple direct mapping - inputs[0]=P1, inputs[1]=P2
                        // Set networked inputs (used by Hook_GetPlayerInput when use_networked_inputs=true)
                        networked_p1_input = received_p1;
                        networked_p2_input = received_p2;
                        use_networked_inputs = true;
                        
                        static uint32_t debug_networked_count = 0;
                        if (++debug_networked_count % 50 == 0) { // Log every 50 AdvanceEvents
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "NETWORKED INPUT SET: P1=0x%03X, P2=0x%03X, use_networked_inputs=true", 
                                       networked_p1_input, networked_p2_input);
                        }

                        // CRITICAL: Set frame_advanced=true to prevent calling original_process_inputs()
                        // which would overwrite the networked inputs with hardware inputs
                        frame_advanced = true;  // Prevent double call to original_process_inputs()
                        break;
                    }
                    case SaveEvent: {
                        // Save current game state for rollback (like bsnes-netplay)
                        // NO BATTLE MODE CHECK - SaveEvent should always be processed
                        static SaveStateData local_rollback_slots[16];
                        uint32_t rollback_slot = update->data.save.frame % 16;
                        SaveStateData* rollback_save_slot = &local_rollback_slots[rollback_slot];
                        
                        if (SaveCompleteGameState(rollback_save_slot, update->data.save.frame)) {
                            void* state_buffer = update->data.save.state;
                            size_t* state_len = update->data.save.state_len;
                            uint32_t* checksum = update->data.save.checksum;
                            
                            // The "state" for GekkoNet is just the checksum (like bsnes-netplay)
                            *(uint32_t*)state_buffer = rollback_save_slot->checksum;
                            *state_len = sizeof(uint32_t);
                            *checksum = rollback_save_slot->checksum;
                        }
                        break;
                    }
                    case LoadEvent: {
                        // Restore game state for rollback (like bsnes-netplay)
                        static SaveStateData local_rollback_slots[16];
                        uint32_t rollback_slot = update->data.load.frame % 16;
                        SaveStateData* rollback_save_slot = &local_rollback_slots[rollback_slot];
                        
                        if (LoadCompleteGameState(rollback_save_slot)) {
                            g_frame_counter = update->data.load.frame;
                        }
                        break;
                    }
                }
            }
        } else {
            // Session not started yet - just process events to keep connection alive
            static int waiting_counter = 0;
            if (++waiting_counter % 60 == 0) { // Log every second
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Waiting for all players to connect...");
            }
        }
        
        if (!frame_advanced) {
            return 0;
        }
    } else {
        // GekkoNet session not yet started/active OR in true offline mode
        // Process inputs locally to allow menus/CSS to function before connection
        g_frame_counter++;
        if (original_process_inputs) {
            original_process_inputs();
        }
    }
    
    // Handle frame stepping countdown for GekkoNet path
    if (shared_data && shared_data->frame_step_remaining_frames > 0 && shared_data->frame_step_remaining_frames != UINT32_MAX) {
        shared_data->frame_step_remaining_frames--;
        if (shared_data->frame_step_remaining_frames == 0) {
            frame_step_paused_global = true;
            shared_data->frame_step_is_paused = true;
        }
    }
    
    // CRITICAL FIX: Only call original function when we have networked inputs from AdvanceEvent
    // Like BSNES: AdvanceEvent triggers emulator->run() with synchronized inputs
    if (frame_advanced && original_process_inputs) {
        return original_process_inputs();
    }
    
    return 0; // Return 0 if no AdvanceEvent received or no original function
}
