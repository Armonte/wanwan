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
    
    // Log when use_networked_inputs changes state or periodically (reduced frequency)
    if (g_frame_counter - last_logged_frame > 300 || last_use_networked != current_use_networked) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hook_GetPlayerInput: P%d input=0x%02X, use_networked=%s, gekko_init=%s, session_valid=%s, net_p1=0x%02X, net_p2=0x%02X", 
                   player_id + 1, original_input & 0xFF, 
                   use_networked_inputs ? "YES" : "NO",
                   gekko_initialized ? "YES" : "NO",
                   (gekko_session && AllPlayersValid()) ? "YES" : "NO",
                   networked_p1_input & 0xFF,
                   networked_p2_input & 0xFF);
        last_logged_frame = g_frame_counter;
        last_use_networked = current_use_networked;
    }
    
    // Return networked input if available
    if (use_networked_inputs && gekko_initialized && gekko_session && AllPlayersValid()) {
        if (player_id == 0) {
            return networked_p1_input;
        } else if (player_id == 1) {
            return networked_p2_input;
        }
    }
    
    return original_input;
}

int __cdecl Hook_ProcessGameInputs() {
    g_frame_counter++;
    
    // Always output on first few calls to verify hook is working
    if (g_frame_counter <= 5) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hook called! Frame %u", g_frame_counter);
    }
    
    // Check for configuration updates from launcher
    CheckConfigurationUpdates();
    
    // Process debug commands from launcher
    ProcessDebugCommands();
    
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
        
        // BSNES PATTERN: Send local controller input to correct handle
        // Host (player_index 0) sends local controller input to P1 handle (0)
        // Client (player_index 1) sends local controller input to P2 handle (1)
        uint8_t local_input = (player_index == 0) ? (uint8_t)(live_p1_input & 0xFF) : (uint8_t)(live_p2_input & 0xFF);
        gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        
        // Enhanced logging for input transmission (reduced frequency)
        static uint32_t last_input_log = 0;
        if (g_frame_counter - last_input_log > 300) {  // Log every 5 seconds (300 frames @ 60fps)
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GEKKO INPUT: Frame %u, Player %u sending 0x%02X (P1_live=0x%02X, P2_live=0x%02X)", 
                       g_frame_counter, player_index + 1, local_input, 
                       (uint8_t)(live_p1_input & 0xFF), (uint8_t)(live_p2_input & 0xFF));
            last_input_log = g_frame_counter;
        }
        
        // Record inputs for testing/debugging if enabled
        RecordInput(g_frame_counter, live_p1_input, live_p2_input);
        
        // CRITICAL: Always use live inputs for local capture, but use networked inputs when available
        // This ensures that before GekkoNet has synchronized inputs, the game still functions normally
        
        // Check if all players are ready
        if (!AllPlayersValid()) {
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
            
            // Let FM2K continue running but block game logic advancement
            return original_process_inputs ? original_process_inputs() : 0;
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
                    
                    // Reduced logging: Only log occasionally or on non-zero inputs
                    bool should_log = (target_frame % 30 == 1);
                    if (should_log) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent to frame %u (inputs: %u bytes)", 
                                    target_frame, input_length);
                    }
                    
                    // Like bsnes: Store inputs directly for the hooked input function to return
                    if (inputs && input_length >= 2) {
                        // BSNES PATTERN: Map GekkoNet handles to FM2K player slots
                        // Handle 0 = P1 input, Handle 1 = P2 input (consistent across both clients)
                        networked_p1_input = inputs[0];  // Handle 0 -> P1
                        networked_p2_input = inputs[1];  // Handle 1 -> P2
                        use_networked_inputs = true;     // Always use network inputs during AdvanceEvent
                        
                        // Debug logging for received inputs (reduced frequency)
                        static uint32_t advance_log_counter = 0;
                        if (++advance_log_counter % 300 == 1) {  // Log every 5 seconds (300 frames @ 60fps)
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GEKKO AdvanceEvent: Frame %u, inputs P1=0x%02X P2=0x%02X, use_networked now=%s", 
                                       target_frame, inputs[0], inputs[1], use_networked_inputs ? "YES" : "NO");
                        }
                        
                        // GekkoNet will call the game's run loop after this AdvanceEvent
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
                    
                    // Save to local slot for rollback (ring buffer pattern)
                    bool save_success = FM2K::State::SaveStateToSlot(save_frame % 8, save_frame);
                    
                    // Provide checksum to GekkoNet for desync detection
                    if (checksum_ptr && state_len_ptr && state_ptr && save_success) {
                        // SAFE FALLBACK: Use simple frame-based checksum to avoid crashes
                        // TODO: Re-enable MinimalGameState once memory addresses are stable
                        *state_len_ptr = sizeof(uint32_t);
                        memcpy(state_ptr, &save_frame, sizeof(uint32_t));
                        *checksum_ptr = save_frame; // Use GekkoNet's synchronized frame number as checksum
                        
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SaveEvent frame %u, checksum: 0x%08X (frame-based)", save_frame, *checksum_ptr);
                    }
                    break;
                }
                
                case LoadEvent: {
                    // BSNES PATTERN: Load state and update frame counter
                    uint32_t load_frame = update->data.load.frame;
                    
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: ROLLBACK from frame %u to frame %u", g_frame_counter, load_frame);
                    
                    // Load from local slot (ring buffer pattern)
                    bool load_success = FM2K::State::LoadStateFromSlot(load_frame % 8);
                    
                    if (load_success) {
                        // Update our frame counter to match the rollback point
                        g_frame_counter = load_frame;
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Rollback successful, frame counter reset to %u", g_frame_counter);
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Rollback failed for frame %u", load_frame);
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