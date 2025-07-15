#include "hooks.h"
#include "globals.h"
#include "gekkonet_hooks.h"
#include "state_manager.h"
#include "logging.h"
#include "shared_mem.h"

// Use global function pointers from globals.h

// Hook implementations
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    int original_input = original_get_player_input ? original_get_player_input(player_id, input_type) : 0;
    
    // Always capture live inputs for networking
    if (player_id == 0) {
        live_p1_input = original_input;
    } else if (player_id == 1) {
        live_p2_input = original_input;
    }
    
    // Enhanced logging for debugging
    static uint32_t last_logged_frame = 0;
    static bool last_use_networked = false;
    bool current_use_networked = use_networked_inputs;
    
    // CSS cursor sync logging - log every movement during character select
    static uint32_t last_css_input[2] = {0, 0};
    bool is_css_mode = (current_game_mode > 0 && current_game_mode < 3000);
    
    // BSNES PATTERN: Always use networked inputs when session is active (simplified logic)
    int returned_input = original_input;
    if (gekko_session_active && use_networked_inputs) {
        // Host is P1 (handle 0), Client is P2 (handle 1). This is consistent on both machines.
        // networked_p1_input is from handle 0, networked_p2_input is from handle 1.
        // The game requests input for player_id 0 (P1) and player_id 1 (P2).
        // The mapping is direct and requires no swapping based on the local player's role.
        if (player_id == 0) {
            returned_input = networked_p1_input;
        } else if (player_id == 1) {
            returned_input = networked_p2_input;
        }
    }
    
    // Log CSS cursor movements or periodic status
    if (is_css_mode && player_id < 2 && (returned_input != last_css_input[player_id])) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS INPUT: P%d input changed 0x%02X -> 0x%02X (frame %u, use_net=%s)", 
                   player_id + 1, last_css_input[player_id] & 0xFF, returned_input & 0xFF, 
                   g_frame_counter, use_networked_inputs ? "YES" : "NO");
        last_css_input[player_id] = returned_input;
    } else if (g_frame_counter - last_logged_frame > 300 || last_use_networked != current_use_networked) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hook_GetPlayerInput: P%d input=0x%02X, session_active=%s, use_networked=%s, net_p1=0x%02X, net_p2=0x%02X", 
                   player_id + 1, original_input & 0xFF, 
                   gekko_session_active ? "YES" : "NO",
                   use_networked_inputs ? "YES" : "NO",
                   networked_p1_input & 0xFF,
                   networked_p2_input & 0xFF);
        last_logged_frame = g_frame_counter;
        last_use_networked = current_use_networked;
    }
    
    return returned_input;
}

int __cdecl Hook_ProcessGameInputs() {
    // SDL2 PATTERN: Let GekkoNet handle frame synchronization - don't block execution
    // The SDL2 example shows they NEVER block with return 0, they just adjust timing
    // GekkoNet is designed to handle the entire session from frame 1
    if (gekko_session_active) {
        // Check how far ahead we are from the remote client for logging only
        float frames_ahead = gekko_frames_ahead(gekko_session);
        
        // CRITICAL: Only block if EXTREMELY far ahead (emergency brake only)
        if (frames_ahead >= 10.0f) {
            static uint32_t emergency_brake_counter = 0;
            if (++emergency_brake_counter % 60 == 1) {  // Log every second
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GEKKO EMERGENCY: Pausing FM2K - %.1f frames ahead (emergency brake)", frames_ahead);
            }
            return 0;
        }
        
        // SDL2 PATTERN: Log sync status but continue execution
        static uint32_t sync_log_counter = 0;
        if (++sync_log_counter % 300 == 1 && frames_ahead > 0.5f) {  // Log every 5 seconds if slightly ahead
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GEKKO SYNC: %.1f frames ahead (normal - letting GekkoNet handle)", frames_ahead);
        }
    }
    
    g_frame_counter++;
    
    // CRITICAL: Post-warmup frame counter synchronization
    static bool post_warmup_sync_done = false;
    static uint32_t sync_attempt_counter = 0;
    static bool first_session_active_detected = false;
    
    // Try a different approach: sync on FIRST time we see gekko_session_active 
    if (gekko_session_active && !first_session_active_detected) {
        first_session_active_detected = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "🔄 FIRST SESSION ACTIVE DETECTED - Checking for post-warmup sync...");
        
        if (!post_warmup_sync_done) {
            uint32_t* frame_counter_ptr = (uint32_t*)FM2K::State::Memory::FRAME_COUNTER_ADDR;
            if (!IsBadWritePtr(frame_counter_ptr, sizeof(uint32_t))) {
                uint32_t old_frame_counter = *frame_counter_ptr;
                *frame_counter_ptr = 0;  // Reset both clients to frame 0 after warmup
                g_frame_counter = 0;     // Reset our internal counter too  
                synchronized_frame = 0;  // Reset GekkoNet sync counter
                post_warmup_sync_done = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "⚡ POST-WARMUP SYNC EXECUTED: Frame counters synchronized (FM2K: %u→0, internal: 0)", old_frame_counter);
            }
        }
    }
    
    // Legacy check for debugging
    if (gekko_session_started && !post_warmup_sync_done && sync_attempt_counter < 5) {
        sync_attempt_counter++;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "POST-WARMUP SYNC CHECK %u: session_started=%s, sync_done=%s, session_active=%s", 
                   sync_attempt_counter, gekko_session_started ? "YES" : "NO", 
                   post_warmup_sync_done ? "YES" : "NO", gekko_session_active ? "YES" : "NO");
    }
    
    // BSNES PATTERN: Frame drift correction system ("rift syncing")
    if (gekko_session_active) {
        float frames_ahead = gekko_frames_ahead(gekko_session);
        
        // Track frame advantage in rolling history
        frame_advantage_history.AddAdvantage(frames_ahead, 0.0f);  // Remote advantage is inverse
        frame_advantage_history.drift_check_counter++;
        
        // BSNES PATTERN: Check for drift every 180 frames (3 seconds at 60fps)
        if (frame_advantage_history.drift_check_counter % 180 == 0) {
            float avg_advantage = frame_advantage_history.GetAverageAdvantage();
            
            // If consistently ahead by 1+ frames and not already correcting
            if (avg_advantage >= 1.0f && !frame_advantage_history.drift_correction_active) {
                frame_advantage_history.drift_correction_active = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "BSNES RIFT SYNC: Correcting frame drift (avg advantage: %.2f)", avg_advantage);
                
                // BSNES "halt frame" pattern: effectively waste one frame to let remote catch up
                uint32_t* frame_counter_ptr = (uint32_t*)FM2K::State::Memory::FRAME_COUNTER_ADDR;
                if (!IsBadReadPtr(frame_counter_ptr, sizeof(uint32_t))) {
                    uint32_t current_fm2k_frame = *frame_counter_ptr;
                    // Don't increment FM2K frame counter this cycle to create a "halt"
                    if (!IsBadWritePtr(frame_counter_ptr, sizeof(uint32_t))) {
                        *frame_counter_ptr = current_fm2k_frame;  // Keep same frame number
                    }
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "BSNES RIFT SYNC: Halted FM2K frame at %u (sync_frame: %u)", current_fm2k_frame, synchronized_frame);
                }
                
                frame_advantage_history.drift_correction_active = false;
                return original_process_inputs ? original_process_inputs() : 0;  // Continue normally
            }
        }
    }
    
    // Always output on first few calls to verify hook is working
    if (g_frame_counter <= 5) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hook called! Frame %u", g_frame_counter);
    }
    
    // Check for configuration updates from launcher
    CheckConfigurationUpdates();
    
    // Process debug commands from launcher
    ProcessDebugCommands();
    
    // BSNES PATTERN: Continuous monitoring every frame (like combat) - no special case for CSS
    // All game states get identical frame-by-frame treatment
    
    if (current_game_mode > 0 && current_game_mode < 3000) {  // Monitor all frames during menus/CSS
        uint32_t* menu_sel_ptr = (uint32_t*)FM2K::State::Memory::MENU_SELECTION_ADDR;
        uint32_t* p1_cursor_x_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_X_ADDR;
        uint32_t* p1_cursor_y_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_Y_ADDR;
        uint32_t* p2_cursor_x_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_X_ADDR;
        uint32_t* p2_cursor_y_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_Y_ADDR;
        uint32_t* p1_char_ptr = (uint32_t*)FM2K::State::Memory::P1_SELECTED_CHAR_ADDR;
        uint32_t* p2_char_ptr = (uint32_t*)FM2K::State::Memory::P2_SELECTED_CHAR_ADDR;
        
        if (!IsBadReadPtr(menu_sel_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p1_cursor_x_ptr, sizeof(uint32_t))) {
            uint32_t menu_sel = *menu_sel_ptr;
            uint32_t p1_cursor_x = *p1_cursor_x_ptr;
            uint32_t p1_cursor_y = *p1_cursor_y_ptr;
            uint32_t p2_cursor_x = *p2_cursor_x_ptr;
            uint32_t p2_cursor_y = *p2_cursor_y_ptr;
            uint32_t p1_char = *p1_char_ptr;
            uint32_t p2_char = *p2_char_ptr;
            
            // BSNES PATTERN: Use synchronized frame for consistent logging across clients
            uint32_t display_frame = gekko_session_active ? synchronized_frame : g_frame_counter;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS MONITOR (Frame %u): menu=%d, P1=(%d,%d), P2=(%d,%d), chars=(%d,%d), game_mode=0x%X", 
                       display_frame, menu_sel, p1_cursor_x, p1_cursor_y, p2_cursor_x, p2_cursor_y, p1_char, p2_char, current_game_mode);
        }
    }
    
    // Capture current inputs from game memory
    uint32_t p1_input = 0;
    uint32_t p2_input = 0;
    
    uint32_t* p1_input_ptr = (uint32_t*)FM2K::State::Memory::P1_INPUT_ADDR;
    uint32_t* p2_input_ptr = (uint32_t*)FM2K::State::Memory::P2_INPUT_ADDR;
    
    if (p1_input_ptr && !IsBadReadPtr(p1_input_ptr, sizeof(uint32_t))) {
        p1_input = *p1_input_ptr;
    }
    if (p2_input_ptr && !IsBadReadPtr(p2_input_ptr, sizeof(uint32_t))) {
        p2_input = *p2_input_ptr;
    }
    
    // CRITICAL: Don't overwrite live inputs captured by Hook_GetPlayerInput
    // live_p1_input and live_p2_input are already captured in real-time by Hook_GetPlayerInput
    // Overwriting them here with potentially stale memory values breaks input transmission
    
    // Forward inputs directly to GekkoNet
    if (gekko_initialized && gekko_session) {
        // Call gekko_network_poll EVERY frame
        gekko_network_poll(gekko_session);
        
        // BSNES PATTERN: Process session events immediately after polling
        if (!gekko_session_started) {
            int session_event_count = 0;
            auto session_events = gekko_session_events(gekko_session, &session_event_count);
            
            for (int i = 0; i < session_event_count; i++) {
                auto event = session_events[i];
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session Event: %d", event->type);
                
                if (event->type == SessionStarted) {
                    gekko_session_started = true;
                    
                    // DEFER: Don't reset frame counter here - warmup frames still executing!
                    // We'll reset after warmup completes in the main process_inputs hook
                    uint32_t* frame_counter_ptr = (uint32_t*)FM2K::State::Memory::FRAME_COUNTER_ADDR;
                    if (!IsBadReadPtr(frame_counter_ptr, sizeof(uint32_t))) {
                        uint32_t current_frame = *frame_counter_ptr;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SESSION STARTED - Frame counter: %u (warmup in progress)", current_frame);
                    } else {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SESSION STARTED in main loop!");
                    }
                    break;
                }
            }
        }
        
        // FIXED: Both clients use P1 controls locally for better UX
        // HOST (original_player_index=0) controls P1, sends as P1 input
        // CLIENT (original_player_index=1) controls P1 locally, sends as P2 input
        uint8_t local_input = 0;
        if (original_player_index == 0) {
            local_input = (uint8_t)(live_p1_input & 0xFF);  // HOST uses P1 controls, sends as P1
        } else {
            local_input = (uint8_t)(live_p1_input & 0xFF);  // CLIENT uses P1 controls, sends as P2
        }
        gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        
        // Enhanced input logging to debug transmission 
        static uint32_t send_frame_count = 0;
        send_frame_count++;
        if (send_frame_count <= 5 || send_frame_count % 600 == 0) {  // Only log first 5 frames or every 10 seconds
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT SEND: Handle %d sending 0x%02X (original_player=%d, role=%s, using P1 controls)", 
                       local_player_handle, local_input, original_player_index, 
                       (original_player_index == 0) ? "HOST" : "CLIENT");
        }
        
        // Record inputs for testing/debugging if enabled
        RecordInput(g_frame_counter, live_p1_input, live_p2_input);
        
        // CRITICAL: Always use live inputs for local capture, but use networked inputs when available
        // This ensures that before GekkoNet has synchronized inputs, the game still functions normally
        
        // BSNES PATTERN: Check if session is not fully active (simplified logic)
        if (!gekko_session_active) {
            // CRITICAL: Track handshake duration to prevent infinite blocking
            static uint32_t handshake_wait_frames = 0;
            handshake_wait_frames++;
            
            // ESCAPE MECHANISM: After 15 seconds (1500 frames), force continuation
            if (handshake_wait_frames > 1500) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GEKKO HANDSHAKE: TIMEOUT - Continuing without full sync after %u frames to prevent deadlock", handshake_wait_frames);
                handshake_wait_frames = 0;
                // Continue execution instead of blocking
            } else {
                // Network handshake in progress - allow Windows message processing
                MSG msg;
                while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                
                // Continue polling GekkoNet for network handshake
                gekko_network_poll(gekko_session);
                
                // Also need to call update_session during handshake
                int handshake_update_count = 0;
                gekko_update_session(gekko_session, &handshake_update_count);
                
                // Log handshake progress periodically
                if (handshake_wait_frames % 300 == 1) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GEKKO HANDSHAKE: Waiting for network sync (%u frames, timeout in %u)", 
                               handshake_wait_frames, 1500 - handshake_wait_frames);
                }
                
                // Let FM2K continue running but block game logic advancement
                return original_process_inputs ? original_process_inputs() : 0;
            }
        }
        
        // Session handshake complete - process session events
        int session_event_count = 0;
        auto session_events = gekko_session_events(gekko_session, &session_event_count);
        for (int i = 0; i < session_event_count; i++) {
            auto event = session_events[i];
            
            if (event->type == DesyncDetected) {
                auto desync = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "=== DESYNC DETECTED ===");
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Frame: %u", desync.frame);
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Local Checksum: 0x%08X", desync.local_checksum);
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Remote Checksum: 0x%08X", desync.remote_checksum);
                
                GenerateDesyncReport(desync.frame, desync.local_checksum, desync.remote_checksum);
                
                if (use_minimal_gamestate_testing) {
                    LogMinimalGameStateDesync(desync.frame, desync.local_checksum, desync.remote_checksum);
                }
            } else if (event->type == PlayerDisconnected) {
                auto disco = event->data.disconnected;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player disconnected: %d", disco.handle);
            } else if (event->type == PlayerConnected) {
                auto connected = event->data.connected;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player connected: %d", connected.handle);
            }
        }
        
        // Get game updates
        int update_count = 0;
        auto updates = gekko_update_session(gekko_session, &update_count);
        
        // Enable networked inputs once we have GekkoNet updates
        static bool networked_inputs_enabled_logged = false;
        if (update_count > 0) {
            use_networked_inputs = true;
            if (!networked_inputs_enabled_logged) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GEKKO: Enabling networked inputs (received %d updates)", update_count);
                networked_inputs_enabled_logged = true;
            }
        }
        
        // BSNES PATTERN: Process all events synchronously without deferred flags
        for (int i = 0; i < update_count; i++) {
            auto update = updates[i];
            
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GEKKO UPDATE: Type %d", update->type);
            
            // Handle different update types based on GekkoNet events  
            switch (update->type) {
                case AdvanceEvent: {
                    // Game should advance one frame with predicted inputs
                    uint32_t target_frame = update->data.adv.frame;
                    uint32_t input_length = update->data.adv.input_len;
                    uint8_t* inputs = update->data.adv.inputs;
                    
                    // BSNES PATTERN: Update synchronized frame counter and process immediately
                    synchronized_frame = target_frame;
                    
                    // Reduced logging: Only log occasionally or on non-zero inputs
                    bool should_log = (target_frame % 30 == 1);
                    if (should_log) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent to frame %u (inputs: %u bytes) - SYNCHRONOUS PROCESSING", 
                                    target_frame, input_length);
                    }
                    
                    // Like bsnes: Store inputs directly for the hooked input function to return
                    if (inputs && input_length >= 2) {
                        // BSNES PATTERN: Map GekkoNet handles to FM2K player slots
                        // Handle 0 = P1 input, Handle 1 = P2 input (consistent across both clients)
                        networked_p1_input = inputs[0];  // Handle 0 -> P1
                        networked_p2_input = inputs[1];  // Handle 1 -> P2
                        use_networked_inputs = true;     // Always use network inputs during AdvanceEvent
                        
                        // Debug logging for received inputs (REDUCED frequency for performance)
                        static uint32_t advance_log_counter = 0;
                        if (++advance_log_counter % 1800 == 1) {  // Log every 30 seconds only
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GEKKO AdvanceEvent: Frame %u, inputs[0]=0x%02X inputs[1]=0x%02X → P1=0x%02X P2=0x%02X", 
                                       target_frame, inputs[0], inputs[1], networked_p1_input, networked_p2_input);
                        }
                        
                        // BSNES PATTERN: AdvanceEvent is processed immediately, no waiting flags
                    }
                    break;
                }
                
                case SaveEvent: {
                    // BSNES PATTERN: Save state with checksum for desync detection
                    uint32_t save_frame = update->data.save.frame;
                    uint32_t* checksum_ptr = update->data.save.checksum;
                    uint32_t* state_len_ptr = update->data.save.state_len;
                    uint8_t* state_ptr = update->data.save.state;
                    
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SaveEvent for frame %u", save_frame);
                    
                    // FAST IN-MEMORY: Save to memory buffer instead of file (like bsnes)
                    uint32_t slot = save_frame % 8;
                    bool save_success = FM2K::State::SaveStateToMemoryBuffer(slot, save_frame);
                    
                    // Provide checksum to GekkoNet for desync detection
                    if (checksum_ptr && state_len_ptr && state_ptr && save_success) {
                        // Use the fixed checksum from our memory buffer
                        uint32_t state_checksum = FM2K::State::GetStateChecksum(slot);
                        *state_len_ptr = sizeof(uint32_t);
                        memcpy(state_ptr, &state_checksum, sizeof(uint32_t));
                        *checksum_ptr = state_checksum;
                        
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SaveEvent frame %u, slot %u, checksum: 0x%08X (essential data only)", 
                                     save_frame, slot, *checksum_ptr);
                    }
                    break;
                }
                
                case LoadEvent: {
                    // BSNES PATTERN: Load state and update frame counter
                    uint32_t load_frame = update->data.load.frame;
                    
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: ROLLBACK from frame %u to frame %u", synchronized_frame, load_frame);
                    
                    // FAST IN-MEMORY: Load from memory buffer instead of file (like bsnes)
                    uint32_t slot = load_frame % 8;
                    bool load_success = FM2K::State::LoadStateFromMemoryBuffer(slot);
                    
                    if (load_success) {
                        // BSNES PATTERN: Update both frame counters to match the rollback point
                        g_frame_counter = load_frame;
                        synchronized_frame = load_frame;
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Rollback successful, frame counters reset to %u", load_frame);
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Rollback failed for frame %u (slot %u)", load_frame, slot);
                    }
                    break;
                }
                
                default:
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Unknown update type: %d", update->type);
                    break;
            }
        }
    }
    
    return original_process_inputs ? original_process_inputs() : 0;
}

int __cdecl Hook_UpdateGameState() {
    // Monitor game state transitions for rollback management
    MonitorGameStateTransitions();
    
    // Original logic for GekkoNet session management
    if (gekko_initialized && !gekko_session_started) {
        return 0;
    }
    
    if (original_update_game) {
        return original_update_game();
    }
    return 0;
}

BOOL __cdecl Hook_RunGameLoop() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** RUN_GAME_LOOP INTERCEPTED - BSNES-LEVEL CONTROL! ***");
    
    if (!gekko_initialized) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing GekkoNet at BSNES level!");
        if (!InitializeGekkoNet()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: ? GekkoNet initialization failed!");
            if (original_run_game_loop) {
                return original_run_game_loop();
            }
            return FALSE;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: ? GekkoNet initialized at main loop level!");
    }
    
    if (gekko_initialized && gekko_session) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet ready - synchronization will happen in game loop to preserve message handling");
        gekko_session_started = false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Calling original run_game_loop...");
    if (original_run_game_loop) {
        return original_run_game_loop();
    }
    return FALSE;
}

bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing MinHook...");
    
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: MH_Initialize failed: %d", mh_init);
        return false;
    }
    
    if (IsBadCodePtr((FARPROC)FM2K::State::Memory::PROCESS_INPUTS_ADDR) || IsBadCodePtr((FARPROC)FM2K::State::Memory::GET_PLAYER_INPUT_ADDR) || 
        IsBadCodePtr((FARPROC)FM2K::State::Memory::UPDATE_GAME_ADDR) || IsBadCodePtr((FARPROC)FM2K::State::Memory::RUN_GAME_LOOP_ADDR)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Target addresses are invalid or not yet mapped");
        return false;
    }
    
    void* inputFuncAddr = (void*)FM2K::State::Memory::PROCESS_INPUTS_ADDR;
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
    
    void* getInputFuncAddr = (void*)FM2K::State::Memory::GET_PLAYER_INPUT_ADDR;
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
    
    void* updateFuncAddr = (void*)FM2K::State::Memory::UPDATE_GAME_ADDR;
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
    
    void* runGameLoopFuncAddr = (void*)FM2K::State::Memory::RUN_GAME_LOOP_ADDR;
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
    return true;
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hooks shut down");
}

// Game state monitoring implementation
void MonitorGameStateTransitions() {
    // Read current game mode values from memory
    uint32_t* game_mode_ptr = (uint32_t*)FM2K::State::Memory::GAME_MODE_ADDR;
    uint32_t* fm2k_mode_ptr = (uint32_t*)FM2K::State::Memory::FM2K_GAME_MODE_ADDR;
    uint32_t* char_select_ptr = (uint32_t*)FM2K::State::Memory::CHARACTER_SELECT_MODE_ADDR;
    
    // Safely read values
    uint32_t new_game_mode = 0xFFFFFFFF;
    uint32_t new_fm2k_mode = 0xFFFFFFFF;
    uint32_t new_char_select = 0xFFFFFFFF;
    
    if (!IsBadReadPtr(game_mode_ptr, sizeof(uint32_t))) {
        new_game_mode = *game_mode_ptr;
    }
    if (!IsBadReadPtr(fm2k_mode_ptr, sizeof(uint32_t))) {
        new_fm2k_mode = *fm2k_mode_ptr;
    }
    if (!IsBadReadPtr(char_select_ptr, sizeof(uint32_t))) {
        new_char_select = *char_select_ptr;
    }
    
    // Check for state transitions and log them
    bool state_changed = false;
    if (new_game_mode != current_game_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: game_mode changed from %s (0x%08X) to %s (0x%08X)", 
                   GetGameModeString(current_game_mode), current_game_mode,
                   GetGameModeString(new_game_mode), new_game_mode);
        current_game_mode = new_game_mode;
        state_changed = true;
        
        // Log character select state when in CSS mode (< 3000)
        if (new_game_mode < 3000) {
            uint32_t* menu_sel_ptr = (uint32_t*)FM2K::State::Memory::MENU_SELECTION_ADDR;
            uint32_t* p1_cursor_x_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_X_ADDR;
            uint32_t* p1_cursor_y_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_Y_ADDR;
            uint32_t* p2_cursor_x_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_X_ADDR;
            uint32_t* p2_cursor_y_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_Y_ADDR;
            uint32_t* p1_char_ptr = (uint32_t*)FM2K::State::Memory::P1_SELECTED_CHAR_ADDR;
            uint32_t* p2_char_ptr = (uint32_t*)FM2K::State::Memory::P2_SELECTED_CHAR_ADDR;
            
            if (!IsBadReadPtr(menu_sel_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p1_cursor_x_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p1_cursor_y_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p2_cursor_x_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p2_cursor_y_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p1_char_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p2_char_ptr, sizeof(uint32_t))) {
                
                uint32_t menu_sel = *menu_sel_ptr;
                uint32_t p1_cursor_x = *p1_cursor_x_ptr;
                uint32_t p1_cursor_y = *p1_cursor_y_ptr;
                uint32_t p2_cursor_x = *p2_cursor_x_ptr;
                uint32_t p2_cursor_y = *p2_cursor_y_ptr;
                uint32_t p1_char = *p1_char_ptr;
                uint32_t p2_char = *p2_char_ptr;
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS STATE: menu=%d, P1_cursor=(%d,%d), P2_cursor=(%d,%d), P1_char=%d, P2_char=%d", 
                           menu_sel, p1_cursor_x, p1_cursor_y, p2_cursor_x, p2_cursor_y, p1_char, p2_char);
            }
        }
    }
    
    if (new_fm2k_mode != current_fm2k_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: fm2k_mode changed from 0x%08X to 0x%08X", 
                   current_fm2k_mode, new_fm2k_mode);
        current_fm2k_mode = new_fm2k_mode;
        state_changed = true;
    }
    
    if (new_char_select != current_char_select_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: char_select_mode changed from 0x%08X to 0x%08X", 
                   current_char_select_mode, new_char_select);
        current_char_select_mode = new_char_select;
        state_changed = true;
    }
    
    // Manage rollback activation based on state changes
    if (state_changed) {
        ManageRollbackActivation(new_game_mode, new_fm2k_mode, new_char_select);
    }
    
    // Mark as initialized after first read
    if (!game_state_initialized) {
        game_state_initialized = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: Initial state - game_mode=0x%08X, fm2k_mode=0x%08X, char_select=0x%08X", 
                   new_game_mode, new_fm2k_mode, new_char_select);
    }
}

void ManageRollbackActivation(uint32_t game_mode, uint32_t fm2k_mode, uint32_t char_select_mode) {
    // BSNES PATTERN: Update unified session state (session startup handled in main loop)
    gekko_session_active = (gekko_session && gekko_initialized && gekko_session_started);
    
    if (gekko_session_active) {
        // Once GekkoNet session is active, it stays active for entire session
        if (!rollback_active) {
            rollback_active = true;
            waiting_for_gekko_advance = true;
            can_advance_frame = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "BSNES PATTERN: GekkoNet session active - rollback enabled for entire session");
        }
    } else {
        // Only disable rollback if GekkoNet is not available at all
        if (rollback_active) {
            rollback_active = false;
            waiting_for_gekko_advance = false;
            can_advance_frame = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "BSNES PATTERN: GekkoNet session inactive - rollback disabled");
        }
    }
}

const char* GetGameModeString(uint32_t mode) {
    switch (mode) {
        case 0xFFFFFFFF: return "UNINITIALIZED";
        case 0x0: return "STARTUP";
        case 0x1: return "INTRO";
        case 0x2: return "MAIN_MENU";
        case 0x3: return "CHARACTER_SELECT";
        case 0x4: return "STAGE_SELECT";
        case 0x5: return "LOADING";
        case 0x1000: return "COMBAT_1000";
        case 0x2000: return "COMBAT_2000";
        case 0x3000: return "COMBAT_3000";
        default: return "UNKNOWN";
    }
} 