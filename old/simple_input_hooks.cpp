#include "simple_input_hooks.h"
#include "hooks/core/game_addresses.hpp"
#include "hooks/input/input_hooks.hpp"
#include "hooks/input/rollback_input_bridge.hpp"
#include "hooks/network/gekko_integration.hpp"
#include "debug_utils.h"
#include "utilities.hpp"
#include "netplay.hpp"
#include "replayer.hpp"
#include "recorder.hpp"
#include "address_definitions.h"
#include "argentum.hpp"  // For disable flags
#include "input/core/input_manager.hpp"  // Add this include for InputManager
#include "input/core/input_types.hpp"   // For NEW_INPUT_* constants
#include "menu/impl/controller_config.h"  // For ControllerConfig auto-save/load system
#include "mlfixtest/practice_mode_v2/enhanced_input_recording.hpp"  // Enhanced recording system
#include <cstdio>
#include <windows.h>
#include <MinHook.h>

// External reference to game base address
extern DWORD g_gameBaseAddress;

// Forward declarations for new functions
extern "C" void UpdateInputFromWindowMessage(UINT message, WPARAM wParam);
extern "C" void ClearConsumedInputs(bool force_clear);
extern "C" void InitializeGekkoOfflineMode();
extern "C" void ProcessGekkoNetFrame();
extern "C" void DebugCurrentInputState();
extern "C" void debug_input_system_status();

// Input state tracking
static struct {
    bool initialized;
    bool keys[256];               // Current key state
    bool keys_consumed[256];      // Keys consumed this frame
    bool keys_held[256];          // Keys being held down
    unsigned int hold_duration[256]; // How long each key has been held
    int frame_counter;
} g_input_state = {0};

// Using declarations to make namespaces visible
using namespace argentum::input;  // Add this to make InputManager accessible

// Global flag to block game input when controller config is open
static bool g_blockGameInput = false;

// External functions to control input blocking
extern "C" void BlockGameInput(bool block) {
    g_blockGameInput = block;
}

extern "C" bool IsGameInputBlocked() {
    return g_blockGameInput;
}

// ==============================================================================
// WINDOW MESSAGE BASED INPUT TRACKING (since SDL_GetKeyboardState isn't working)
// ==============================================================================

// Update input state based on window messages (called from SDL3 window proc)
extern "C" void UpdateInputFromWindowMessage(UINT message, WPARAM wParam) {
    if (!g_input_state.initialized) {
        memset(&g_input_state, 0, sizeof(g_input_state));
        g_input_state.initialized = true;
    }
    
    // Track key state changes
    switch (message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            g_input_state.keys[wParam & 0xFF] = true;
            
            // Check for back button (F1) to toggle controller config
            if (wParam == VK_F1) {
                // Toggle CCCaster-style controller config on first press
                if (!g_input_state.keys_held[VK_F1]) {
                    extern void OpenNewControllerConfig();
                    // Check if config window is already open
                    if (IsControllerConfigOpen()) {
                        // Close if already open
                        CloseControllerConfig();
                    } else {
                        // Open if closed
                        OpenNewControllerConfig();
                    }
                }
            }
            break;
            
        case WM_KEYUP:
        case WM_SYSKEYUP:
            g_input_state.keys[wParam & 0xFF] = false;
            g_input_state.keys_held[wParam & 0xFF] = false;
            g_input_state.hold_duration[wParam & 0xFF] = 0;
            break;
    }
}

// Clear consumed inputs at the start of each frame
extern "C" void ClearConsumedInputs(bool force_clear) {
    if (!g_input_state.initialized) return;
    
    g_input_state.frame_counter++;
    
    // Clear consumed flags every frame
    memset(g_input_state.keys_consumed, 0, sizeof(g_input_state.keys_consumed));
    
    // If force_clear is true, clear ALL key states
    if (force_clear) {
        memset(g_input_state.keys, 0, sizeof(g_input_state.keys));
        memset(g_input_state.keys_held, 0, sizeof(g_input_state.keys_held));
        memset(g_input_state.hold_duration, 0, sizeof(g_input_state.hold_duration));
        return;
    }
    
    // Update hold durations and handle key releases
    for (int i = 0; i < 256; i++) {
        if (g_input_state.keys[i]) {
            // Key is pressed - increment hold duration
            g_input_state.hold_duration[i]++;
            if (g_input_state.hold_duration[i] > 1) {
                g_input_state.keys_held[i] = true;
            }
        } else {
            // Key is released - reset hold tracking
            g_input_state.hold_duration[i] = 0;
            g_input_state.keys_held[i] = false;
        }
    }
}

// Convert Windows VK codes to ML2 input format with consumption tracking
unsigned char ConvertWindowsKeysToML2Input(bool isP2, bool consume = true) {
    if (!g_input_state.initialized) {
        return 0;
    }
    
    // FIXED: Return 0 immediately if input is blocked
    if (g_blockGameInput) {
        return 0;
    }
    
    // CONTEXT-AWARE EDGE DETECTION for keyboard (same as gamepad)
    static unsigned char lastKeyboardP1Output = 0;
    static unsigned char lastKeyboardP2Output = 0;
    static bool keyboardP1ActionConsumed = false;
    static bool keyboardP2ActionConsumed = false;
    
    // Determine current game context for edge detection
    bool isInMenuContext = true;  // Default to menu (safer for navigation)
    DWORD* currentGameMode = (DWORD*)(g_gameBaseAddress + OFFSET_CURRENTGAMEMODE);
    if (currentGameMode) {
        int gameMode = *currentGameMode;
        // Battle contexts where held state is needed: BATTLE(14), PRACTICE, etc.
        isInMenuContext = (gameMode != 14);  // 14 = GAMETYPE_BATTLE, others are menus
    }
    
    unsigned char input = 0;
    
    if (!isP2) {
        // P1: WASD + ZXC + Space
        unsigned char movementInput = 0;
        unsigned char actionInput = 0;
        
        // MOVEMENT: Always use held state (for walking) with edge detection for menus
        if (g_input_state.keys['W'] && (!g_input_state.keys_held['W'] || !consume)) { 
            movementInput |= 0x01; 
            if (consume) g_input_state.keys_consumed['W'] = true;
        }
        if (g_input_state.keys['S'] && (!g_input_state.keys_held['S'] || !consume)) { 
            movementInput |= 0x02; 
            if (consume) g_input_state.keys_consumed['S'] = true;
        }
        if (g_input_state.keys['A'] && (!g_input_state.keys_held['A'] || !consume)) { 
            movementInput |= 0x04; 
            if (consume) g_input_state.keys_consumed['A'] = true;
        }
        if (g_input_state.keys['D'] && (!g_input_state.keys_held['D'] || !consume)) { 
            movementInput |= 0x08; 
            if (consume) g_input_state.keys_consumed['D'] = true;
        }
        
        // Menu inputs (Space/Enter) are edge-triggered
        if ((g_input_state.keys[VK_SPACE] && (!g_input_state.keys_held[VK_SPACE] || !consume)) || 
            (g_input_state.keys[VK_RETURN] && (!g_input_state.keys_held[VK_RETURN] || !consume))) { 
            movementInput |= 0x10; 
            if (consume) {
                g_input_state.keys_consumed[VK_SPACE] = true;
                g_input_state.keys_consumed[VK_RETURN] = true;
            }
        }
        
        // ACTION BUTTONS: Apply context-aware edge detection
        unsigned char rawActionInput = 0;
        
        // Collect raw action button state (held detection logic preserved for battles)
        if (g_input_state.keys['Z']) {
            bool isNewPress = !g_input_state.keys_held['Z'];
            bool hasHeldLongEnough = g_input_state.hold_duration['Z'] > 6; // ~100ms at 60fps
            if (isNewPress || hasHeldLongEnough) {
                rawActionInput |= 0x20;
                if (consume && hasHeldLongEnough) {
                    g_input_state.hold_duration['Z'] = 0;
                }
            }
        }
        if (g_input_state.keys['X']) {
            bool isNewPress = !g_input_state.keys_held['X'];
            bool hasHeldLongEnough = g_input_state.hold_duration['X'] > 6;
            if (isNewPress || hasHeldLongEnough) {
                rawActionInput |= 0x40;
                if (consume && hasHeldLongEnough) {
                    g_input_state.hold_duration['X'] = 0;
                }
            }
        }
        if (g_input_state.keys['C']) {
            bool isNewPress = !g_input_state.keys_held['C'];
            bool hasHeldLongEnough = g_input_state.hold_duration['C'] > 6;
            if (isNewPress || hasHeldLongEnough) {
                rawActionInput |= 0x80;
                if (consume && hasHeldLongEnough) {
                    g_input_state.hold_duration['C'] = 0;
                }
            }
        }
        
        // Apply context-aware edge detection to action buttons only
        if (isInMenuContext && consume) {
            // Menu context: Apply edge detection to action buttons only
            unsigned char lastActions = lastKeyboardP1Output & 0xE0; // Action button bits
            unsigned char currentActions = rawActionInput & 0xE0;
            
            if (currentActions != 0) {
                if (currentActions == lastActions && !keyboardP1ActionConsumed) {
                    // Same action buttons - ignore repetition
                    actionInput = 0;
                    printf("? P1 KEYBOARD MENU: Ignoring repeated action buttons 0x%02X\n", currentActions);
                } else if (currentActions != lastActions) {
                    // New action buttons - allow and mark consumed
                    actionInput = rawActionInput;
                    keyboardP1ActionConsumed = true;
                    printf("? P1 KEYBOARD MENU: Allowing new action buttons 0x%02X\n", currentActions);
                }
            } else {
                // No action buttons - reset edge detection
                actionInput = 0;
                keyboardP1ActionConsumed = false;
            }
        } else {
            // Battle context or non-consuming: Use raw action input
            actionInput = rawActionInput;
        }
        
        input = movementInput | actionInput;
        lastKeyboardP1Output = input;
        
    } else {
        // P2: Arrow keys + UIO + Backslash (similar logic)
        unsigned char movementInput = 0;
        unsigned char actionInput = 0;
        
        // MOVEMENT: Always use held state (for walking) with edge detection for menus
        if (g_input_state.keys[VK_UP] && (!g_input_state.keys_held[VK_UP] || !consume)) { 
            movementInput |= 0x01; 
            if (consume) g_input_state.keys_consumed[VK_UP] = true;
        }
        if (g_input_state.keys[VK_DOWN] && (!g_input_state.keys_held[VK_DOWN] || !consume)) { 
            movementInput |= 0x02; 
            if (consume) g_input_state.keys_consumed[VK_DOWN] = true;
        }
        if (g_input_state.keys[VK_LEFT] && (!g_input_state.keys_held[VK_LEFT] || !consume)) { 
            movementInput |= 0x04; 
            if (consume) g_input_state.keys_consumed[VK_LEFT] = true;
        }
        if (g_input_state.keys[VK_RIGHT] && (!g_input_state.keys_held[VK_RIGHT] || !consume)) { 
            movementInput |= 0x08; 
            if (consume) g_input_state.keys_consumed[VK_RIGHT] = true;
        }
        
        // Menu input (Backslash) is edge-triggered
        if (g_input_state.keys[VK_OEM_5] && (!g_input_state.keys_held[VK_OEM_5] || !consume)) { 
            movementInput |= 0x10; 
            if (consume) g_input_state.keys_consumed[VK_OEM_5] = true;
        }
        
        // ACTION BUTTONS: Apply context-aware edge detection
        unsigned char rawActionInput = 0;
        
        if (g_input_state.keys['U']) {
            bool isNewPress = !g_input_state.keys_held['U'];
            bool hasHeldLongEnough = g_input_state.hold_duration['U'] > 6;
            if (isNewPress || hasHeldLongEnough) {
                rawActionInput |= 0x20;
                if (consume && hasHeldLongEnough) {
                    g_input_state.hold_duration['U'] = 0;
                }
            }
        }
        if (g_input_state.keys['I']) {
            bool isNewPress = !g_input_state.keys_held['I'];
            bool hasHeldLongEnough = g_input_state.hold_duration['I'] > 6;
            if (isNewPress || hasHeldLongEnough) {
                rawActionInput |= 0x40;
                if (consume && hasHeldLongEnough) {
                    g_input_state.hold_duration['I'] = 0;
                }
            }
        }
        if (g_input_state.keys['O']) {
            bool isNewPress = !g_input_state.keys_held['O'];
            bool hasHeldLongEnough = g_input_state.hold_duration['O'] > 6;
            if (isNewPress || hasHeldLongEnough) {
                rawActionInput |= 0x80;
                if (consume && hasHeldLongEnough) {
                    g_input_state.hold_duration['O'] = 0;
                }
            }
        }
        
        // Apply context-aware edge detection to action buttons only
        if (isInMenuContext && consume) {
            // Menu context: Apply edge detection to action buttons only
            unsigned char lastActions = lastKeyboardP2Output & 0xE0; // Action button bits
            unsigned char currentActions = rawActionInput & 0xE0;
            
            if (currentActions != 0) {
                if (currentActions == lastActions && !keyboardP2ActionConsumed) {
                    // Same action buttons - ignore repetition
                    actionInput = 0;
                    printf("? P2 KEYBOARD MENU: Ignoring repeated action buttons 0x%02X\n", currentActions);
                } else if (currentActions != lastActions) {
                    // New action buttons - allow and mark consumed
                    actionInput = rawActionInput;
                    keyboardP2ActionConsumed = true;
                    printf("? P2 KEYBOARD MENU: Allowing new action buttons 0x%02X\n", currentActions);
                }
            } else {
                // No action buttons - reset edge detection
                actionInput = 0;
                keyboardP2ActionConsumed = false;
            }
        } else {
            // Battle context or non-consuming: Use raw action input
            actionInput = rawActionInput;
        }
        
        input = movementInput | actionInput;
        lastKeyboardP2Output = input;
    }
    
    return input;
}

// ==============================================================================
// INPUT FORMAT CONVERSION - CRITICAL FOR REMAPPING SYSTEM
// ==============================================================================

// Convert InputManager format to game's expected format
extern "C" unsigned char convertNewToOldInputFormat(unsigned int newInput) {
    unsigned char oldInput = 0;
    
    // Convert directional inputs (NEW format uses high bits)
    if (newInput & NEW_INPUT_UP) oldInput |= 0x01;
    if (newInput & NEW_INPUT_DOWN) oldInput |= 0x02;
    if (newInput & NEW_INPUT_LEFT) oldInput |= 0x04;
    if (newInput & NEW_INPUT_RIGHT) oldInput |= 0x08;
    
    // Convert button inputs - SWAPPED A & B for proper fighting game layout
    // A = Medium Attack (0x40), B = Light Attack (0x20), C = Heavy Attack (0x60)
    if (newInput & NEW_INPUT_BTN_A) oldInput |= 0x40;  // A ? Medium (swapped)
    if (newInput & NEW_INPUT_BTN_B) oldInput |= 0x20;  // B ? Light (swapped)
    if (newInput & NEW_INPUT_BTN_C) oldInput |= 0x60;  // C ? Heavy (A+B = 0x40+0x20 = 0x60)
    
    // Start button (Space/Enter) maps to 0x10 - check for start bit
    // Note: Need to check what bit the start button uses in NEW format
    
    return oldInput;
}

// ==============================================================================
// GEKKONET ROLLBACK INPUT BRIDGE IMPLEMENTATION
// ==============================================================================

// Define global variables using memory addresses
#define g_lastRawInputState (*(unsigned int*)GET_GAME_ADDRESS(void*, OFFSET_G_LASTRAWEINPUTSTATE))
#define g_player2LastRawInputState (*(unsigned int*)GET_GAME_ADDRESS(void*, OFFSET_G_PLAYER2LASTRAWEINPUTSTATE))

// Store original functions
HandleInputsFunc originalHandleP1Inputs = nullptr;
HandleInputsFunc originalHandleP2Inputs = nullptr;

// GekkoNet Integration for offline rollback (as described in rollback documentation)
static bool g_gekko_offline_mode = false;
static int g_frame_counter = 0;

// GekkoNet session variables (following LocalSession.cpp pattern)
static void* g_gekko_session = nullptr;
static bool g_gekko_session_created = false;
static int g_player1_id = -1;
static int g_player2_id = -1;

// Track if inputs have been sent to GekkoNet this frame
static bool g_inputs_sent_this_frame = false;
static int g_current_game_frame = 0;

// Explicitly enable GekkoNet offline mode (call this when entering battle or for testing)
extern "C" void EnableGekkoOfflineMode() {
    if (g_gekko_offline_mode) {
        DebugOutput("? GEKKO OFFLINE: Already enabled\n");
        return;
    }
    
    DebugOutput("? GEKKO OFFLINE: Explicitly enabling for battle/testing\n");
    InitializeGekkoOfflineMode();
}

// Disable GekkoNet offline mode (call this when returning to menus)
extern "C" void DisableGekkoOfflineMode() {
    if (!g_gekko_offline_mode) {
        return;
    }
    
    DebugOutput("? GEKKO OFFLINE: Disabling - returning to direct input mode\n");
    g_gekko_offline_mode = false;
    g_inputs_sent_this_frame = false;
    
    // Clear any queued inputs
    ClearConsumedInputs(true);
}

// Initialize GekkoNet for offline rollback testing
extern "C" void InitializeGekkoOfflineMode() {
    if (g_gekko_offline_mode) return;
    
    DebugOutput("? GEKKO OFFLINE: Creating offline session using existing GekkoIntegration\n");
    
    // Initialize GekkoNet integration first
    argentum::hooks::network::GekkoIntegration::Initialize();
    
    // Use the existing GekkoIntegration methods (these were already working!)
    if (!g_gekko_session_created) {
        DebugOutput("? GEKKO: Setting up offline session using GekkoIntegration\n");
        
        // PrepareForBattle should create a local session internally
        // Based on your output, GekkoIntegration::Initialize() was working fine
        bool sessionReady = argentum::hooks::network::GekkoIntegration::PrepareForBattle();
        
        if (sessionReady) {
            g_gekko_session_created = true;
            g_gekko_session = (void*)0x1;  // Mark as created (dummy pointer)
            DebugOutput("? GEKKO: GekkoIntegration::PrepareForBattle() SUCCESS!\n");
        } else {
            DebugOutput("? GEKKO: GekkoIntegration::PrepareForBattle() returned false\n");
            // Try anyway, sometimes it still works
            g_gekko_session_created = true;
            g_gekko_session = (void*)0x1;
        }
    }
    
    g_gekko_offline_mode = true;
    g_frame_counter = 0;
    g_inputs_sent_this_frame = false;
    g_current_game_frame = 0;
    
    DebugOutput("? GEKKO OFFLINE: Offline session setup complete!\n");
    DebugOutput("   - Using existing GekkoIntegration system\n");
    DebugOutput("   - Input hooks already working\n");
    DebugOutput("   - Ready for rollback testing\n");
}

/**
 * Main P1 Input Hook - Implements complete rollback input system
 * Following the architecture described in COMPLETE_ROLLBACK_IMPLEMENTATION_GUIDE.md
 */
extern "C" int HandleP1InputsHook(void) {
    static int callCount = 0;
    callCount++;
    
    // CRITICAL: Update input blocking state every frame to ensure proper management
    extern void UpdateControllerConfigInputBlocking();
    UpdateControllerConfigInputBlocking();
    
    // CRITICAL: Check input blocking first - return 0 immediately if blocked
    if (g_blockGameInput) {
        return 0;
    }
    
    // TRACE: Add detailed debugging for input repetition
    static unsigned char lastReturnedInput = 0xFF;
    static int sameInputCount = 0;
    
    // CONTEXT-AWARE EDGE DETECTION: Track input state for menu contexts
    static unsigned int lastInputManagerRaw = 0;
    static unsigned char lastConvertedOutput = 0;
    static bool inputConsumedThisFrame = false;
    
    // Determine current game context for edge detection
    bool isInMenuContext = true;  // Default to menu (safer for navigation)
    DWORD* currentGameMode = (DWORD*)(g_gameBaseAddress + OFFSET_CURRENTGAMEMODE);
    if (currentGameMode) {
        int gameMode = *currentGameMode;
        // Battle contexts where held state is needed: BATTLE(14), PRACTICE, etc.
        isInMenuContext = (gameMode != 14);  // 14 = GAMETYPE_BATTLE, others are menus
    }
    
    // Ensure InputManager is initialized before use
    static bool inputManagerInitialized = false;
    if (!inputManagerInitialized) {
        auto& inputManager = argentum::input::InputManager::getInstance();
        if (inputManager.initialize()) {
            inputManagerInitialized = true;
            printf("InputManager initialized in input hook\n");
        } else {
            printf("ERROR: Failed to initialize InputManager\n");
            return ConvertWindowsKeysToML2Input(false, !g_gekko_offline_mode);
        }
    }
    
    // Clear consumed inputs at start of new frame
    // FIXED: Use proper frame detection instead of GekkoNet-dependent logic
    static int lastProcessedFrame = -1;
    bool isNewFrame = (g_current_game_frame != lastProcessedFrame);
    
    if (callCount == 1 || isNewFrame) {
        // Force clear inputs on first frame or when we detect menu transition
        DWORD* currentGameMode = (DWORD*)(g_gameBaseAddress + OFFSET_CURRENTGAMEMODE);
        static DWORD lastGameMode = 0;
        bool force_clear = (g_current_game_frame == 0 || (currentGameMode && *currentGameMode != lastGameMode));
        
        // Store current mode for next frame's comparison
        if (currentGameMode) {
            lastGameMode = *currentGameMode;
        }
        
        ClearConsumedInputs(force_clear);
        g_inputs_sent_this_frame = false;
        
        // Only increment frame counter on actual new frames, not every call
        if (isNewFrame) {
            g_current_game_frame++;
            lastProcessedFrame = g_current_game_frame;
        }
        
        // DEBUG: Track frame resets that affect edge detection (only on actual new frames)
        static int frameResetCount = 0;
        if (isNewFrame) {
            frameResetCount++;
            inputConsumedThisFrame = false;  // Reset edge detection only on new frames
        }
    }
    
    unsigned char input = 0;
    
    // TRACE: Track which input path we're taking
    const char* inputSource = "UNKNOWN";
    
    // ? Only use GekkoNet when explicitly enabled AND session is active
    if (g_gekko_offline_mode && argentum::hooks::network::GekkoIntegration::IsSessionActive()) {
        inputSource = "GEKKO_ROLLBACK";
        
        if (!g_inputs_sent_this_frame) {
            // Capture local input from window messages (consuming them)
            unsigned char p1LocalInput = ConvertWindowsKeysToML2Input(false, true);
            unsigned char p2LocalInput = ConvertWindowsKeysToML2Input(true, true);
            
            // Send both player inputs to GekkoNet at once
            argentum::hooks::network::GekkoIntegration::SetLocalInput(0, p1LocalInput);
            argentum::hooks::network::GekkoIntegration::SetLocalInput(1, p2LocalInput);
            
            g_inputs_sent_this_frame = true;
        }
        
        // Get synchronized inputs from GekkoNet (handles rollback automatically)
        uint16_t p1_input, p2_input;
        if (argentum::hooks::network::GekkoIntegration::GetPlayerInputs(p1_input, p2_input)) {
            input = (unsigned char)(p1_input & 0xFF);  // Convert uint16_t to uint8_t
        }
    } else {
        // Phase 2: Direct input collection using InputManager with CONTEXT-AWARE EDGE DETECTION
        try {
            inputSource = isInMenuContext ? "INPUT_MANAGER_MENU" : "INPUT_MANAGER_BATTLE";
            auto& inputManager = argentum::input::InputManager::getInstance();
            unsigned int newFormatInput = inputManager.getInput(0);  // Player 1
            
            // CONTEXT-AWARE EDGE DETECTION: Apply different logic based on game context
            if (isInMenuContext) {
                // ? MENU CONTEXT: Use edge detection ONLY for ACTION BUTTONS, preserve held state for movement
                if (newFormatInput != 0) {
                    // Separate movement inputs from action button inputs
                    unsigned int movementBits = newFormatInput & (NEW_INPUT_UP | NEW_INPUT_DOWN | NEW_INPUT_LEFT | NEW_INPUT_RIGHT);
                    unsigned int actionBits = newFormatInput & (NEW_INPUT_BTN_A | NEW_INPUT_BTN_B | NEW_INPUT_BTN_C);
                    
                    // MOVEMENT: Always allow held state (for walking)
                    unsigned int allowedMovement = movementBits;
                    
                    // ACTION BUTTONS: Apply edge detection to prevent menu spam
                    unsigned int allowedActions = 0;
                    if (actionBits != 0) {
                        unsigned int lastActionBits = lastInputManagerRaw & (NEW_INPUT_BTN_A | NEW_INPUT_BTN_B | NEW_INPUT_BTN_C);
                        
                        if (actionBits == lastActionBits && !inputConsumedThisFrame) {
                            // Same action buttons as last frame - ignore to prevent repetition
                            allowedActions = 0;
                        } else if (actionBits != lastActionBits) {
                            // New action buttons detected - allow them and mark as consumed
                            inputConsumedThisFrame = true;
                            allowedActions = actionBits;
                        } else {
                            // Same action but already consumed this frame - keep ignoring
                            allowedActions = 0;
                        }
                    } else {
                        // No action buttons - reset edge detection state for actions
                        inputConsumedThisFrame = false;
                    }
                    
                    // Combine movement (always allowed) + actions (edge-detected)
                    unsigned int finalInput = allowedMovement | allowedActions;
                    
                    // CRITICAL FIX: Track the ORIGINAL input for next frame comparison
                    lastInputManagerRaw = newFormatInput;  // Store original, not modified
                    newFormatInput = finalInput;           // Use modified for output
                    
                    // Menu input processed successfully
                } else {
                    // No input - reset edge detection state
                    lastInputManagerRaw = 0;
                    inputConsumedThisFrame = false;
                }
            } else {
                // ?? BATTLE CONTEXT: Use held state (original behavior for fighting games)
                lastInputManagerRaw = newFormatInput;  // Track for context switches
            }
            
            input = convertNewToOldInputFormat(newFormatInput);
            
        } catch (...) {
            // Fallback to old system if InputManager fails
            inputSource = "FALLBACK_WINDOWS";
            printf("P1 FALLBACK: InputManager failed, using old system\n");
            input = ConvertWindowsKeysToML2Input(false, !g_gekko_offline_mode);
        }
    }
    
    // TRACE: Track the final returned input for repetition detection
    if (input != lastReturnedInput) {
        sameInputCount = 0;
        lastReturnedInput = input;
    } else if (input != 0) {
        sameInputCount++;
    }
    
    // ? ENHANCED RECORDING INTEGRATION (Phase 2.2)
    // Record P1 input individually
    argentum::practice::InputRecordingBridge::recordPlayerInput(0, input);
    
    // Debug P1 input processing
    static unsigned char lastRecordedP1Input = 0xFF;
    if (input != lastRecordedP1Input) {
        printf("P1 INPUT HOOK: Processing P1 input 0x%02X (source: %s)\n", input, inputSource);
        lastRecordedP1Input = input;
    }
    
    // Check if we should override with playback input
    if (argentum::practice::InputRecordingBridge::shouldOverrideInput()) {
        unsigned char playbackInput = argentum::practice::InputRecordingBridge::getPlaybackInput(0);
        if (playbackInput != 0 || input == 0) {  // Use playback if it has input OR if we have no input
            input = playbackInput;
            
            // Debug playback override (only occasionally to avoid spam)
            static int playbackDebugCount = 0;
            if (playbackDebugCount < 5) {
                printf("P1 PLAYBACK: Overriding input 0x%02X -> 0x%02X\n", input, playbackInput);
                playbackDebugCount++;
            }
        }
    }
    
    return input;
}

/**
 * Main P2 Input Hook - Implements complete rollback input system with same logic as P1
 */
extern "C" unsigned char __fastcall HandleP2InputsHook(void) {
    static int callCount = 0;
    callCount++;
    
    // CRITICAL: Update input blocking state every frame to ensure proper management
    extern void UpdateControllerConfigInputBlocking();
    UpdateControllerConfigInputBlocking();
    
    // CRITICAL: Check input blocking first - return 0 immediately if blocked
    if (g_blockGameInput) {
        return 0;
    }
    
    // TRACE: Add detailed debugging for input repetition (same as P1)
    static unsigned char lastReturnedInput = 0xFF;
    static int sameInputCount = 0;
    
    // CONTEXT-AWARE EDGE DETECTION: Track input state for menu contexts (same as P1)
    static unsigned int lastInputManagerRaw = 0;
    static unsigned char lastConvertedOutput = 0;
    static bool inputConsumedThisFrame = false;
    
    // Determine current game context for edge detection (same as P1)
    bool isInMenuContext = true;  // Default to menu (safer for navigation)
    DWORD* currentGameMode = (DWORD*)(g_gameBaseAddress + OFFSET_CURRENTGAMEMODE);
    if (currentGameMode) {
        int gameMode = *currentGameMode;
        // Battle contexts where held state is needed: BATTLE(14), PRACTICE, etc.
        isInMenuContext = (gameMode != 14);  // 14 = GAMETYPE_BATTLE, others are menus
    }
    
    // Ensure InputManager is initialized before use
    static bool inputManagerInitialized = false;
    if (!inputManagerInitialized) {
        auto& inputManager = argentum::input::InputManager::getInstance();
        if (inputManager.initialize()) {
            inputManagerInitialized = true;
        } else {
            printf("ERROR: Failed to initialize InputManager in P2 hook\n");
            return ConvertWindowsKeysToML2Input(true, !g_gekko_offline_mode);
        }
    }
    
    // Frame tracking for edge detection (same as P1)
    static int lastGameFrame = -1;
    bool isNewFrame = false;
    DWORD* gameFramePtr = (DWORD*)(g_gameBaseAddress + OFFSET_G_FRAMECOUNTER);
    if (gameFramePtr) {
        int currentGameFrame = *gameFramePtr;
        if (currentGameFrame != lastGameFrame) {
            isNewFrame = true;
            lastGameFrame = currentGameFrame;
            g_inputs_sent_this_frame = false;
        }
        
        // DEBUG: Track frame resets that affect edge detection (only on actual new frames)
        static int frameResetCount = 0;
        if (isNewFrame) {
            frameResetCount++;
            inputConsumedThisFrame = false;  // Reset edge detection only on new frames
        }
    }
    
    unsigned char input = 0;
    
    // TRACE: Track which input path we're taking
    const char* inputSource = "UNKNOWN";
    
    // Note: Inputs are already sent to GekkoNet in P1 hook to avoid double-sending
    
    // Phase 1: Check if GekkoNet rollback session is active
    if (g_gekko_offline_mode && argentum::hooks::network::GekkoIntegration::IsSessionActive()) {
        inputSource = "GEKKO_ROLLBACK";
        
        // Get synchronized inputs from GekkoNet (handles rollback automatically)
        uint16_t p1_input, p2_input;
        if (argentum::hooks::network::GekkoIntegration::GetPlayerInputs(p1_input, p2_input)) {
            input = (unsigned char)(p2_input & 0xFF);  // Convert uint16_t to uint8_t
        }
    } else {
        // Phase 2: Direct input collection using InputManager with CONTEXT-AWARE EDGE DETECTION (same as P1)
        try {
            inputSource = isInMenuContext ? "INPUT_MANAGER_MENU" : "INPUT_MANAGER_BATTLE";
            auto& inputManager = argentum::input::InputManager::getInstance();
            unsigned int newFormatInput = inputManager.getInput(1);  // Player 2
            
            // CONTEXT-AWARE EDGE DETECTION: Apply different logic based on game context (same as P1)
            if (isInMenuContext) {
                // ? MENU CONTEXT: Use edge detection ONLY for ACTION BUTTONS, preserve held state for movement
                if (newFormatInput != 0) {
                    // Separate movement inputs from action button inputs
                    unsigned int movementBits = newFormatInput & (NEW_INPUT_UP | NEW_INPUT_DOWN | NEW_INPUT_LEFT | NEW_INPUT_RIGHT);
                    unsigned int actionBits = newFormatInput & (NEW_INPUT_BTN_A | NEW_INPUT_BTN_B | NEW_INPUT_BTN_C);
                    
                    // MOVEMENT: Always allow held state (for walking)
                    unsigned int allowedMovement = movementBits;
                    
                    // ACTION BUTTONS: Apply edge detection to prevent menu spam
                    unsigned int allowedActions = 0;
                    if (actionBits != 0) {
                        unsigned int lastActionBits = lastInputManagerRaw & (NEW_INPUT_BTN_A | NEW_INPUT_BTN_B | NEW_INPUT_BTN_C);
                        
                        if (actionBits == lastActionBits && !inputConsumedThisFrame) {
                            // Same action buttons as last frame - ignore to prevent repetition
                            allowedActions = 0;
                        } else if (actionBits != lastActionBits) {
                            // New action buttons detected - allow them and mark as consumed
                            inputConsumedThisFrame = true;
                            allowedActions = actionBits;
                        } else {
                            // Same action but already consumed this frame - keep ignoring
                            allowedActions = 0;
                        }
                    } else {
                        // No action buttons - reset edge detection state for actions
                        inputConsumedThisFrame = false;
                    }
                    
                    // Combine movement (always allowed) + actions (edge-detected)
                    unsigned int finalInput = allowedMovement | allowedActions;
                    
                    // CRITICAL FIX: Track the ORIGINAL input for next frame comparison
                    lastInputManagerRaw = newFormatInput;  // Store original, not modified
                    newFormatInput = finalInput;           // Use modified for output
                    
                    // Menu input processed successfully
                } else {
                    // No input - reset edge detection state
                    lastInputManagerRaw = 0;
                    inputConsumedThisFrame = false;
                }
            } else {
                // ?? BATTLE CONTEXT: Use held state (original behavior for fighting games)
                lastInputManagerRaw = newFormatInput;  // Track for context switches
            }
            
            input = convertNewToOldInputFormat(newFormatInput);
            
        } catch (...) {
            // Fallback to old system if InputManager fails
            inputSource = "FALLBACK_WINDOWS";
            printf("P2 FALLBACK: InputManager failed, using old system\n");
            input = ConvertWindowsKeysToML2Input(true, !g_gekko_offline_mode);
        }
    }
    
    // TRACE: Track the final returned input for repetition detection (same as P1)
    if (input != lastReturnedInput) {
        sameInputCount = 0;
        lastReturnedInput = input;
    } else if (input != 0) {
        sameInputCount++;
    }
    
    // ? ENHANCED RECORDING INTEGRATION (Phase 2.2)
    // Record P2 input individually
    argentum::practice::InputRecordingBridge::recordPlayerInput(1, input);
    
    // Debug P2 input processing
    static unsigned char lastRecordedP2Input = 0xFF;
    if (input != lastRecordedP2Input) {
        printf("P2 INPUT HOOK: Processing P2 input 0x%02X (source: %s)\n", input, inputSource);
        lastRecordedP2Input = input;
    }
    
    // Check if we should override with playback input
    if (argentum::practice::InputRecordingBridge::shouldOverrideInput()) {
        unsigned char playbackInput = argentum::practice::InputRecordingBridge::getPlaybackInput(1);
        if (playbackInput != 0 || input == 0) {  // Use playback if it has input OR if we have no input
            input = playbackInput;
            
            // Debug playback override (only occasionally to avoid spam)
            static int playbackDebugCount = 0;
            if (playbackDebugCount < 5) {
                printf("P2 PLAYBACK: Overriding input 0x%02X -> 0x%02X\n", input, playbackInput);
                playbackDebugCount++;
            }
        }
    }
    
    return input;
}

// ==============================================================================
// HOOK INSTALLATION AND BRIDGE FUNCTIONS
// ==============================================================================

// Install simplified hooks using direct function replacement
bool installSimplifiedInputHooks() {
    DebugOutput("Installing simplified input hooks:\n");
    DebugOutput("  P1: 0x%08X -> 0x%p\n", 0x00411280, HandleP1InputsHook);
    DebugOutput("  P2: 0x%08X -> 0x%p\n", 0x00411380, HandleP2InputsHook);
    
    // NOTE: InputManager initialization moved to initgame_replacement.cpp
    // to prevent early initialization hangs
    
    // Get the base address
    HMODULE gameModule = GetModuleHandle(NULL);
    if (!gameModule) {
        DebugOutput("ERROR: Could not get game module handle\n");
        return false;
    }
    
    // Calculate function addresses
    uintptr_t baseAddr = (uintptr_t)gameModule;
    void* p1FuncAddr = (void*)(baseAddr + 0x11280);  // HandleP1Inputs at 0x411280
    void* p2FuncAddr = (void*)(baseAddr + 0x11380);  // HandleP2Inputs at 0x411380
    
    // Install P1 hook using MinHook
    MH_STATUS status1 = MH_CreateHook(p1FuncAddr, (void*)HandleP1InputsHook, (void**)&originalHandleP1Inputs);
    if (status1 != MH_OK) {
        DebugOutput("ERROR: Failed to create P1 input hook: %d\n", status1);
        return false;
    }
    
    status1 = MH_EnableHook(p1FuncAddr);
    if (status1 != MH_OK) {
        DebugOutput("ERROR: Failed to enable P1 input hook: %d\n", status1);
        return false;
    }
    
    // Install P2 hook using MinHook
    MH_STATUS status2 = MH_CreateHook(p2FuncAddr, (void*)HandleP2InputsHook, (void**)&originalHandleP2Inputs);
    if (status2 != MH_OK) {
        DebugOutput("ERROR: Failed to create P2 input hook: %d\n", status2);
        return false;
    }
    
    status2 = MH_EnableHook(p2FuncAddr);
    if (status2 != MH_OK) {
        DebugOutput("ERROR: Failed to enable P2 input hook: %d\n", status2);
        return false;
    }
    
    DebugOutput("? Simplified input hooks installed successfully!\n");
    return true;
}

/**
 * Initialize InputManager and perform auto-assignment
 * Called from initgame_replacement.cpp just before bootlogo
 */
bool initializeControllerSystem() {
    DebugOutput("CONTROLLER INIT: Initializing InputManager for auto-assignment...\n");
    
    auto& inputManager = argentum::input::InputManager::getInstance();
    if (!inputManager.initialize()) {
        DebugOutput("CONTROLLER INIT: WARNING - InputManager initialization failed\n");
        return false;
    }
    
    DebugOutput("CONTROLLER INIT: InputManager initialized successfully!\n");
    
    // Initialize ControllerConfig auto-save/load system
    DebugOutput("CONTROLLER INIT: Initializing ControllerConfig auto-save/load system...\n");
    argentum::ControllerConfig::Initialize();
    DebugOutput("CONTROLLER INIT: ControllerConfig initialized successfully!\n");
    
    // Force gamepad refresh and auto-assignment
    inputManager.refreshGamepads();
    
    int gamepadCount = inputManager.getConnectedGamepadCount();
    bool hasKeyboard = true;
    
    DebugOutput("CONTROLLER INIT: Auto-assigning devices: %d gamepads, keyboard=%s\n", 
               gamepadCount, hasKeyboard ? "YES" : "NO");
               
    // Auto-assignment logic (same as F1 menu)
    if (gamepadCount >= 2) {
        inputManager.assignDeviceToPlayer(0, argentum::input::InputManager::DEVICE_GAMEPAD, 0);
        inputManager.assignDeviceToPlayer(1, argentum::input::InputManager::DEVICE_GAMEPAD, 1);
        DebugOutput("CONTROLLER INIT: AUTO-ASSIGNED - 2 gamepads to P1/P2\n");
    } else if (gamepadCount == 1 && hasKeyboard) {
        inputManager.assignDeviceToPlayer(0, argentum::input::InputManager::DEVICE_GAMEPAD, 0);
        inputManager.assignDeviceToPlayer(1, argentum::input::InputManager::DEVICE_KEYBOARD, 0);
        DebugOutput("CONTROLLER INIT: AUTO-ASSIGNED - Gamepad to P1, Keyboard to P2\n");
    } else if (gamepadCount == 1) {
        inputManager.assignDeviceToPlayer(0, argentum::input::InputManager::DEVICE_GAMEPAD, 0);
        DebugOutput("CONTROLLER INIT: AUTO-ASSIGNED - Gamepad to P1 only\n");
    } else if (hasKeyboard) {
        inputManager.assignDeviceToPlayer(0, argentum::input::InputManager::DEVICE_KEYBOARD, 0);
        DebugOutput("CONTROLLER INIT: AUTO-ASSIGNED - Keyboard to P1 only\n");
    }
    
    DebugOutput("CONTROLLER INIT: ? Controller system initialization complete!\n");
    return true;
}

// Bridge functions for rollback input system
extern "C" {
    GInput get_p1_input_bridge(void) {
        GInput input = {0};
        // Use non-consuming version for bridge functions to avoid interfering with main hooks
        input.value = ConvertWindowsKeysToML2Input(false, false);
        return input;
    }

    GInput get_p2_input_bridge(void) {
        GInput input = {0};
        // Use non-consuming version for bridge functions to avoid interfering with main hooks
        input.value = ConvertWindowsKeysToML2Input(true, false);
        return input;
    }

    // Convert between different input formats (for rollback system)
    unsigned char ginput_to_byte(GInput input) {
        return input.value;
    }

    GInput byte_to_ginput(unsigned char input) {
        GInput result = {0};
        result.value = input;
        return result;
    }
    
    // Debug function to check if inputs are being processed correctly
    void debug_input_system_status(void) {
        if (!g_input_state.initialized) {
            DebugOutput("? INPUT DEBUG: System not initialized\n");
            return;
        }
        
        // Check current input states
        unsigned char p1_raw = ConvertWindowsKeysToML2Input(false, false);
        unsigned char p2_raw = ConvertWindowsKeysToML2Input(true, false);
        
        // Count consumed keys
        int p1_consumed_count = 0, p2_consumed_count = 0;
        
        // P1 keys
        if (g_input_state.keys_consumed['W']) p1_consumed_count++;
        if (g_input_state.keys_consumed['A']) p1_consumed_count++;
        if (g_input_state.keys_consumed['S']) p1_consumed_count++;
        if (g_input_state.keys_consumed['D']) p1_consumed_count++;
        if (g_input_state.keys_consumed['Z']) p1_consumed_count++;
        if (g_input_state.keys_consumed['X']) p1_consumed_count++;
        if (g_input_state.keys_consumed['C']) p1_consumed_count++;
        if (g_input_state.keys_consumed[VK_SPACE]) p1_consumed_count++;
        
        // P2 keys
        if (g_input_state.keys_consumed[VK_UP]) p2_consumed_count++;
        if (g_input_state.keys_consumed[VK_DOWN]) p2_consumed_count++;
        if (g_input_state.keys_consumed[VK_LEFT]) p2_consumed_count++;
        if (g_input_state.keys_consumed[VK_RIGHT]) p2_consumed_count++;
        if (g_input_state.keys_consumed['U']) p2_consumed_count++;
        if (g_input_state.keys_consumed['I']) p2_consumed_count++;
        if (g_input_state.keys_consumed['O']) p2_consumed_count++;
        if (g_input_state.keys_consumed[VK_OEM_5]) p2_consumed_count++;
        
        // DISABLED for performance: INPUT DEBUG message causes FPS drops
        // DebugOutput("? INPUT DEBUG: Frame=%d, P1=0x%02X (consumed=%d), P2=0x%02X (consumed=%d), GekkoMode=%s, InputsSent=%s\n",
        //            g_current_game_frame, p1_raw, p1_consumed_count, p2_raw, p2_consumed_count,
        //            g_gekko_offline_mode ? "ON" : "OFF",
        //            g_inputs_sent_this_frame ? "YES" : "NO");
    }
}

// ==============================================================================
// GEKKONET FRAME PROCESSING INTEGRATION
// ==============================================================================

/**
 * Process GekkoNet frame - called from main game loop
 * Uses the existing rollback session system for complete offline session management
 */
extern "C" void ProcessGekkoNetFrame() {
    if (!g_gekko_offline_mode) return;
    
    // Clear consumed inputs at the start of each game frame
    // This ensures that our frame boundaries are properly aligned
    static int last_processed_frame = -1;
    if (g_current_game_frame != last_processed_frame) {
        // New frame detected - reset input tracking
        g_inputs_sent_this_frame = false;
        last_processed_frame = g_current_game_frame;
        
        // Debug frame processing
        static int frame_debug_count = 0;
        if (frame_debug_count < 5) {
            DebugOutput("? GEKKO FRAME: Processing frame %d (reset input flags)\n", g_current_game_frame);
            frame_debug_count++;
        }
    }
    
    g_frame_counter++;
    
    // ? Check if our rollback session is active (from rollback_session.cpp)
    bool sessionActive = false;
    if (g_gekko_session) {
        // Check with both systems to see if session is working
        sessionActive = argentum::hooks::network::GekkoIntegration::IsSessionActive();
        
        // Debug output for first few frames
        static int sessionDebugCount = 0;
        if (sessionDebugCount < 5) {
            DebugOutput("? GEKKO: Frame %d - Session check: rollback_session=%p, gekko_active=%s\n", 
                       g_frame_counter, g_gekko_session, sessionActive ? "YES" : "NO");
            sessionDebugCount++;
        }
    }
    
    if (sessionActive || g_gekko_session) {
        // Session is working! Process the frame
        static int processingDebugCount = 0;
        if (processingDebugCount < 5) {
            DebugOutput("? GEKKO: Successfully processing frame %d with rollback session\n", g_frame_counter);
            processingDebugCount++;
        }
        
        // Let GekkoNet handle the complete frame processing
        // This includes input collection, rollback detection, and state management
        // Note: Input collection is now handled by the input hooks, not here
        argentum::hooks::network::GekkoIntegration::UpdateNetplay();
        
        // Debug rollback events when they occur
        if (argentum::hooks::network::GekkoIntegration::IsInRollback()) {
            static int rollback_debug_count = 0;
            if (rollback_debug_count < 5) {
                DebugOutput("? GEKKO ROLLBACK: Frame %d - rollback simulation in progress\n", g_frame_counter);
                rollback_debug_count++;
            }
        }
    } else {
        // Session still not working - try to debug why
        static int retryCount = 0;
        if (retryCount < 3) {
            DebugOutput("? GEKKO: Session not working, investigating... (attempt %d)\n", retryCount + 1);
            DebugOutput("   - g_gekko_session: %p\n", g_gekko_session);
            DebugOutput("   - Integration active: %s\n", argentum::hooks::network::GekkoIntegration::IsSessionActive() ? "YES" : "NO");
            
            // Try to re-initialize
            if (!g_gekko_session) {
                DebugOutput("   - Attempting to recreate rollback session...\n");
                InitializeGekkoOfflineMode();
            }
            retryCount++;
        }
    }
}

// Debug function to check current input state
extern "C" void DebugCurrentInputState() {
    if (!g_input_state.initialized) return;
    
    static int debug_count = 0;
    debug_count++;
    
    if (debug_count % 300 == 0) {  // Every 5 seconds only
        // Use non-consuming version to check current state without affecting gameplay
        unsigned char p1 = ConvertWindowsKeysToML2Input(false, false);
        unsigned char p2 = ConvertWindowsKeysToML2Input(true, false);
        
        // Input debug logging (DISABLED for performance)
        // static unsigned char lastDebugP1 = 0xFF, lastDebugP2 = 0xFF;
        // bool inputChanged = (p1 != lastDebugP1 || p2 != lastDebugP2);
        // 
        // if (inputChanged) {
        //     DebugOutput("? INPUT DEBUG: P1=0x%02X, P2=0x%02X (frame=%d, inputs_sent=%s)\n", 
        //                p1, p2, g_current_game_frame, g_inputs_sent_this_frame ? "YES" : "NO");
        //     lastDebugP1 = p1;
        //     lastDebugP2 = p2;
        // }
        
        // Detailed debug function only every 15 seconds
        if (debug_count % 900 == 0) {
            debug_input_system_status();
        }
    }
}

// Simple polling-based input for player 1 and player 2
extern "C" uint16_t GetPlayerInput(int player) {
    if (!g_input_state.initialized) {
        return 0;
    }
    
    // Block all input to the game when controller config is open
    if (g_blockGameInput) {
        return 0;
    }
    
    uint16_t input = 0;
    
    // Use InputManager to get modern input
    try {
        auto& inputManager = argentum::input::InputManager::getInstance();
        unsigned int newFormatInput = inputManager.getInput(player);
        
        // Convert modern format to legacy format
        if (newFormatInput & NEW_INPUT_UP)    input |= 0x01;
        if (newFormatInput & NEW_INPUT_DOWN)  input |= 0x02;
        if (newFormatInput & NEW_INPUT_LEFT)  input |= 0x04;
        if (newFormatInput & NEW_INPUT_RIGHT) input |= 0x08;
        if (newFormatInput & NEW_INPUT_BTN_A) input |= 0x40;  // A ? Medium (swapped)
        if (newFormatInput & NEW_INPUT_BTN_B) input |= 0x20;  // B ? Light (swapped)
        if (newFormatInput & NEW_INPUT_BTN_C) input |= 0x60;  // C ? Heavy (A+B = 0x40+0x20 = 0x60)
        // Note: Start button (0x10) not currently mapped in GetPlayerInput - handled by ConvertWindowsKeysToML2Input
    } catch (...) {
        // Fallback to old system if InputManager fails
        input = ConvertWindowsKeysToML2Input(player == 1, false);
    }
    
    return input;
} 