#include "input_handler.h"
#include "globals.h"
#include "gekkonet_hooks.h"
#include "logging.h"
#include "shared_mem.h"
#include "savestate.h"
#include "debug_features.h"
#include <windows.h>
#include <SDL3/SDL.h>

// CCCaster-style direct input capture
uint16_t CaptureDirectInput() {
    uint16_t input = 0;
    
    // Direction inputs (correct FM2K bit mapping from documentation)
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)  input |= 0x001; // Left
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) input |= 0x002; // Right  
    if (GetAsyncKeyState(VK_UP) & 0x8000)    input |= 0x004; // Up
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)  input |= 0x008; // Down
    
    // Button inputs (using common keyboard mapping)
    if (GetAsyncKeyState('Z') & 0x8000)      input |= 0x010; // Button 1
    if (GetAsyncKeyState('X') & 0x8000)      input |= 0x020; // Button 2  
    if (GetAsyncKeyState('C') & 0x8000)      input |= 0x040; // Button 3
    if (GetAsyncKeyState('A') & 0x8000)      input |= 0x080; // Button 4
    if (GetAsyncKeyState('S') & 0x8000)      input |= 0x100; // Button 5
    if (GetAsyncKeyState('D') & 0x8000)      input |= 0x200; // Button 6
    if (GetAsyncKeyState('Q') & 0x8000)      input |= 0x400; // Button 7
    
    return input;
}

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

// REMOVED: CaptureRealInputs - no longer needed with pure GekkoNet architecture

// NEW GEKKONET INTEGRATION: SDL keyboard polling for clean input source
uint16_t PollSDLKeyboard() {
    // Get current SDL keyboard state (SDL3 returns const bool*)
    const bool* keys = SDL_GetKeyboardState(NULL);
    uint16_t input = 0;
    
    // Debug: Check if SDL_GetKeyboardState is working
    static uint32_t debug_call_count = 0;
    debug_call_count++;
    
    if (!keys) {
        if (debug_call_count % 600 == 0) { // Reduced from 300 to 600
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
    
    // REDUCED LOGGING: Log key state when any key is pressed OR every 600 calls (reduced from 300)
    static uint32_t last_input = 0;
    if (input != last_input || debug_call_count % 600 == 0) { // Reduced from 300 to 600
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
bool IsWindowFocused() {
    // Get the currently focused window
    HWND focused_window = GetForegroundWindow();
    HWND our_window = GetActiveWindow();
    
    // Check if our process window has focus (like BSNES focused() check)
    return (focused_window == our_window) || (focused_window != NULL && our_window != NULL);
}

int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    // UNIFIED INPUT CONTROL: Handle both offline and online modes with proper side-flipping
    
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
    
    int raw_input = 0;
    
    // Step 1: Get raw input based on mode
    if (is_true_offline) {
        // OFFLINE MODE: Local keyboard input for both players
        if (IsWindowFocused()) {
            if (player_id == 0) {
                // P1: Arrow keys + ZXCASD
                if (GetAsyncKeyState(VK_LEFT) & 0x8000)  raw_input |= 0x001;
                if (GetAsyncKeyState(VK_RIGHT) & 0x8000) raw_input |= 0x002;
                if (GetAsyncKeyState(VK_UP) & 0x8000)    raw_input |= 0x004;
                if (GetAsyncKeyState(VK_DOWN) & 0x8000)  raw_input |= 0x008;
                if (GetAsyncKeyState('Z') & 0x8000)      raw_input |= 0x010;
                if (GetAsyncKeyState('X') & 0x8000)      raw_input |= 0x020;
                if (GetAsyncKeyState('C') & 0x8000)      raw_input |= 0x040;
                if (GetAsyncKeyState('A') & 0x8000)      raw_input |= 0x080;
                if (GetAsyncKeyState('S') & 0x8000)      raw_input |= 0x100;
                if (GetAsyncKeyState('D') & 0x8000)      raw_input |= 0x200;
            } else if (player_id == 1) {
                // P2: WASD + UIOPJK
                if (GetAsyncKeyState('A') & 0x8000)      raw_input |= 0x001;
                if (GetAsyncKeyState('D') & 0x8000)      raw_input |= 0x002;
                if (GetAsyncKeyState('W') & 0x8000)      raw_input |= 0x004;
                if (GetAsyncKeyState('S') & 0x8000)      raw_input |= 0x008;
                if (GetAsyncKeyState('U') & 0x8000)      raw_input |= 0x010;
                if (GetAsyncKeyState('I') & 0x8000)      raw_input |= 0x020;
                if (GetAsyncKeyState('O') & 0x8000)      raw_input |= 0x040;
                if (GetAsyncKeyState('P') & 0x8000)      raw_input |= 0x080;
                if (GetAsyncKeyState('J') & 0x8000)      raw_input |= 0x100;
                if (GetAsyncKeyState('K') & 0x8000)      raw_input |= 0x200;
            }
        }
    } else {
        // ONLINE MODE: Local + networked inputs
        if (::player_index == 0) {
            // HOST: Controls P1, receives P2 from network
            if (player_id == 0) {
                // P1 local keyboard
                if (IsWindowFocused()) {
                    if (GetAsyncKeyState(VK_LEFT) & 0x8000)  raw_input |= 0x001;
                    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) raw_input |= 0x002;
                    if (GetAsyncKeyState(VK_UP) & 0x8000)    raw_input |= 0x004;
                    if (GetAsyncKeyState(VK_DOWN) & 0x8000)  raw_input |= 0x008;
                    if (GetAsyncKeyState('Z') & 0x8000)      raw_input |= 0x010;
                    if (GetAsyncKeyState('X') & 0x8000)      raw_input |= 0x020;
                    if (GetAsyncKeyState('C') & 0x8000)      raw_input |= 0x040;
                    if (GetAsyncKeyState('A') & 0x8000)      raw_input |= 0x080;
                    if (GetAsyncKeyState('S') & 0x8000)      raw_input |= 0x100;
                    if (GetAsyncKeyState('D') & 0x8000)      raw_input |= 0x200;
                }
            } else if (player_id == 1) {
                // P2 networked
                raw_input = GetCurrentNetworkedP2Input();
            }
        } else if (::player_index == 1) {
            // CLIENT: Controls P2, receives P1 from network
            if (player_id == 0) {
                // P1 networked
                raw_input = GetCurrentNetworkedP1Input();
            } else if (player_id == 1) {
                // P2 local keyboard
                if (IsWindowFocused()) {
                    if (GetAsyncKeyState('A') & 0x8000)      raw_input |= 0x001;
                    if (GetAsyncKeyState('D') & 0x8000)      raw_input |= 0x002;
                    if (GetAsyncKeyState('W') & 0x8000)      raw_input |= 0x004;
                    if (GetAsyncKeyState('S') & 0x8000)      raw_input |= 0x008;
                    if (GetAsyncKeyState('U') & 0x8000)      raw_input |= 0x010;
                    if (GetAsyncKeyState('I') & 0x8000)      raw_input |= 0x020;
                    if (GetAsyncKeyState('O') & 0x8000)      raw_input |= 0x040;
                    if (GetAsyncKeyState('P') & 0x8000)      raw_input |= 0x080;
                    if (GetAsyncKeyState('J') & 0x8000)      raw_input |= 0x100;
                    if (GetAsyncKeyState('K') & 0x8000)      raw_input |= 0x200;
                }
            }
        }
    }
    
    // Step 2: Apply FM2K's side-flipping logic (replicated from original get_player_input)
    uint32_t* g_game_mode = (uint32_t*)0x470054;
    uint8_t* g_player_current_actions = (uint8_t*)0x4dfcd1; // Correct address from MCP
    uint32_t* g_player_action_flags = (uint32_t*)0x4d9a36;  // Correct address from MCP
    
    bool invert_directions = false;
    if (*g_game_mode < 3000 || *g_game_mode >= 4000 ||
        !g_player_current_actions[57407 * input_type] ||
        (g_player_action_flags[57407 * input_type] & 8) != 0) {
        invert_directions = true;
    }
    
    // Step 3: Apply direction inversion if needed
    int final_input = raw_input;
    if (!invert_directions) {
        // Swap left/right bits when NOT inverting (FM2K's logic)
        int left_bit = (raw_input & 0x001) ? 1 : 0;
        int right_bit = (raw_input & 0x002) ? 1 : 0;
        
        final_input = raw_input & ~0x003; // Clear left/right bits
        if (left_bit)  final_input |= 0x002; // Left key -> right bit
        if (right_bit) final_input |= 0x001; // Right key -> left bit
    }
    
    // Step 4: Debug logging
    static uint32_t input_log_counter = 0;
    if (++input_log_counter % 180 == 0 && (raw_input != 0 || final_input != 0)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                   "INPUT P%d: raw=0x%03X final=0x%03X invert=%s mode=%d", 
                   player_id, raw_input, final_input, 
                   invert_directions ? "true" : "false", *g_game_mode);
    }
    
    return final_input;
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

// REMOVED: CaptureKeyboardInput and WriteInputsToGameMemory
// Following Heat's advice - only override FINAL processed state in GekkoNet callbacks