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
    // HEAT'S ADVICE: Only poll for local player, let GekkoNet provide remote player
    // In TRUE_OFFLINE mode, we provide both players' inputs
    
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
    
    int input_mask = 0;
    
    // TRUE OFFLINE: Provide inputs for both players
    if (is_true_offline) {
        if (IsWindowFocused()) {
            if (player_id == 0) {
                // P1: Arrow keys + ZXC/ASD
                if (GetAsyncKeyState(VK_LEFT) & 0x8000)  input_mask |= 0x001;
                if (GetAsyncKeyState(VK_RIGHT) & 0x8000) input_mask |= 0x002;
                if (GetAsyncKeyState(VK_UP) & 0x8000)    input_mask |= 0x004;
                if (GetAsyncKeyState(VK_DOWN) & 0x8000)  input_mask |= 0x008;
                if (GetAsyncKeyState('Z') & 0x8000)      input_mask |= 0x010;
                if (GetAsyncKeyState('X') & 0x8000)      input_mask |= 0x020;
                if (GetAsyncKeyState('C') & 0x8000)      input_mask |= 0x040;
                if (GetAsyncKeyState('A') & 0x8000)      input_mask |= 0x080;
                if (GetAsyncKeyState('S') & 0x8000)      input_mask |= 0x100;
                if (GetAsyncKeyState('D') & 0x8000)      input_mask |= 0x200;
            } else if (player_id == 1) {
                // P2: WASD + UIO/JKL
                if (GetAsyncKeyState('A') & 0x8000)      input_mask |= 0x001;
                if (GetAsyncKeyState('D') & 0x8000)      input_mask |= 0x002;
                if (GetAsyncKeyState('W') & 0x8000)      input_mask |= 0x004;
                if (GetAsyncKeyState('S') & 0x8000)      input_mask |= 0x008;
                if (GetAsyncKeyState('U') & 0x8000)      input_mask |= 0x010;
                if (GetAsyncKeyState('I') & 0x8000)      input_mask |= 0x020;
                if (GetAsyncKeyState('O') & 0x8000)      input_mask |= 0x040;
                if (GetAsyncKeyState('P') & 0x8000)      input_mask |= 0x080;
                if (GetAsyncKeyState('J') & 0x8000)      input_mask |= 0x100;
                if (GetAsyncKeyState('K') & 0x8000)      input_mask |= 0x200;
            }
        }
        return input_mask;
    }
    
    // ONLINE MODE: Provide local player input + networked remote player input
    if (::player_index == 0) {
        // HOST controls P1, receives P2 from network
        if (player_id == 0) {
            // P1 input from local keyboard
            if (IsWindowFocused()) {
                if (GetAsyncKeyState(VK_LEFT) & 0x8000)  input_mask |= 0x001;
                if (GetAsyncKeyState(VK_RIGHT) & 0x8000) input_mask |= 0x002;
                if (GetAsyncKeyState(VK_UP) & 0x8000)    input_mask |= 0x004;
                if (GetAsyncKeyState(VK_DOWN) & 0x8000)  input_mask |= 0x008;
                if (GetAsyncKeyState('Z') & 0x8000)      input_mask |= 0x010;
                if (GetAsyncKeyState('X') & 0x8000)      input_mask |= 0x020;
                if (GetAsyncKeyState('C') & 0x8000)      input_mask |= 0x040;
                if (GetAsyncKeyState('A') & 0x8000)      input_mask |= 0x080;
                if (GetAsyncKeyState('S') & 0x8000)      input_mask |= 0x100;
                if (GetAsyncKeyState('D') & 0x8000)      input_mask |= 0x200;
            }
        } else if (player_id == 1) {
            // P2 input from network
            input_mask = GetCurrentNetworkedP2Input();
        }
    } else if (::player_index == 1) {
        // CLIENT controls P2, receives P1 from network
        if (player_id == 0) {
            // P1 input from network
            input_mask = GetCurrentNetworkedP1Input();
        } else if (player_id == 1) {
            // P2 input from local keyboard
            if (IsWindowFocused()) {
                if (GetAsyncKeyState('A') & 0x8000)      input_mask |= 0x001;
                if (GetAsyncKeyState('D') & 0x8000)      input_mask |= 0x002;
                if (GetAsyncKeyState('W') & 0x8000)      input_mask |= 0x004;
                if (GetAsyncKeyState('S') & 0x8000)      input_mask |= 0x008;
                if (GetAsyncKeyState('U') & 0x8000)      input_mask |= 0x010;
                if (GetAsyncKeyState('I') & 0x8000)      input_mask |= 0x020;
                if (GetAsyncKeyState('O') & 0x8000)      input_mask |= 0x040;
                if (GetAsyncKeyState('P') & 0x8000)      input_mask |= 0x080;
                if (GetAsyncKeyState('J') & 0x8000)      input_mask |= 0x100;
                if (GetAsyncKeyState('K') & 0x8000)      input_mask |= 0x200;
            }
        }
    }
    
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

// REMOVED: CaptureKeyboardInput and WriteInputsToGameMemory
// Following Heat's advice - only override FINAL processed state in GekkoNet callbacks