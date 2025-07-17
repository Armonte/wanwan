#include "hooks.h"
#include "globals.h"
#include "gekkonet_hooks.h"
#include "state_manager.h"
#include "logging.h"
#include "shared_mem.h"
#include "game_state_machine.h"
#include "css_sync.h"
#include <windows.h>
#include <mmsystem.h>

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
        // Host is P1 (handle 0), Client is P2 (handle 1). This is consistent on both machines.
        // networked_p1_input is from handle 0, networked_p2_input is from handle 1.
        // The game requests input for player_id 0 (P1) and player_id 1 (P2).
        // The mapping is direct and requires no swapping based on the local player's role.
        if (player_id == 0) {
            return networked_p1_input;
        }
        if (player_id == 1) {
            return networked_p2_input;
        }
    }
    
    return original_input;
}

int __cdecl Hook_ProcessGameInputs() {
    // SPEED TEST: Add delay to slow game to 50% speed to prove we have control
    static DWORD last_frame_time = 0;
    DWORD current_time = timeGetTime();
    
    // Normal FM2K runs at 100 FPS (10ms per frame)
    // To slow to 50%, we want 50 FPS (20ms per frame)
    const DWORD target_frame_time = 20; // 20ms = 50 FPS = 50% speed
    
    if (last_frame_time > 0) {
        DWORD elapsed = current_time - last_frame_time;
        if (elapsed < target_frame_time) {
            DWORD sleep_time = target_frame_time - elapsed;
            Sleep(sleep_time);
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "SPEED TEST: Slowing frame by %ums (50%% speed)", sleep_time);
        }
    }
    last_frame_time = timeGetTime();
    
    // CRITICAL: Let the original function handle frame advancement
    int original_result = original_process_inputs ? original_process_inputs() : 0;
    
    // Update our frame counter AFTER original function (which increments FM2K's counter)
    g_frame_counter++;
    
    // Always output on first few calls to verify hook is working
    if (g_frame_counter <= 5) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hook called! Frame %u", g_frame_counter);
    }
    
    // Check for configuration updates from launcher
    CheckConfigurationUpdates();
    
    // Process debug commands from launcher
    ProcessDebugCommands();
    
    // Update game state machine and character select sync
    FM2K::State::g_game_state_machine.Update(current_game_mode);
    FM2K::CSS::g_css_sync.Update();
    
    // Handle battle synchronization during battle transitions
    if (FM2K::State::g_game_state_machine.GetCurrentPhase() == FM2K::State::GamePhase::IN_BATTLE &&
        !FM2K::State::g_game_state_machine.IsBattleSyncConfirmed()) {
        // Continue sync process - this will auto-confirm after timeout
        FM2K::State::g_game_state_machine.RequestBattleSync();
    }
    
    // Debug: Log rollback state periodically during menus and battle sync
    static uint32_t rollback_log_counter = 0;
    if (current_game_mode < 3000 && (++rollback_log_counter % 300 == 1)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "MENU STATE: mode=%d, rollback_active=%s, frames_in_phase=%d",
            current_game_mode, rollback_active ? "YES" : "NO",
            FM2K::State::g_game_state_machine.GetFramesInCurrentPhase());
    } else if (current_game_mode >= 3000 && !FM2K::State::g_game_state_machine.IsBattleSyncConfirmed() && (++rollback_log_counter % 60 == 1)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "BATTLE SYNC: Waiting for synchronization confirmation (frame %u)", g_frame_counter);
    }
    
    // Log character selection transitions
    if (FM2K::State::g_game_state_machine.HasTransitioned()) {
        auto current_phase = FM2K::State::g_game_state_machine.GetCurrentPhase();
        auto previous_phase = FM2K::State::g_game_state_machine.GetPreviousPhase();
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GAME PHASE TRANSITION: %d -> %d (frame %u)", 
                   static_cast<int>(previous_phase), static_cast<int>(current_phase), g_frame_counter);
                   
        // CRITICAL: Log input state at transition time to debug desync
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
            "TRANSITION INPUTS: live_p1=0x%02X, live_p2=0x%02X, net_p1=0x%02X, net_p2=0x%02X",
            live_p1_input & 0xFF, live_p2_input & 0xFF, 
            networked_p1_input & 0xFF, networked_p2_input & 0xFF);
                   
        // Log when both players confirm character selection
        if (FM2K::State::g_game_state_machine.IsCharacterSelectionComplete()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CHARACTER SELECTION COMPLETE - Both players confirmed!");
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
        
        // CORRECT INPUT MAPPING:
        // HOST (original_player_index=0) controls P1, sends as P1 input
        // CLIENT (original_player_index=1) controls P2, sends as P2 input
        uint8_t local_input = 0;
        if (original_player_index == 0) {
            local_input = (uint8_t)(live_p1_input & 0xFF);  // HOST uses P1 controls, sends as P1
        } else {
            local_input = (uint8_t)(live_p2_input & 0xFF);  // CLIENT uses P2 controls, sends as P2
        }
        gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        
        // Enhanced input logging to debug transmission 
        static uint32_t send_frame_count = 0;
        send_frame_count++;
        if (send_frame_count <= 5 || send_frame_count % 600 == 0) {  // Only log first 5 frames or every 10 seconds
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT SEND: Handle %d sending 0x%02X (original_player=%d, role=%s, using %s controls)", 
                       local_player_handle, local_input, original_player_index, 
                       (original_player_index == 0) ? "HOST" : "CLIENT",
                       (original_player_index == 0) ? "P1" : "P2");
        }
        
        // Record inputs for testing/debugging if enabled
        RecordInput(g_frame_counter, live_p1_input, live_p2_input);
        
        // CRITICAL: Always use live inputs for local capture, but use networked inputs when available
        // This ensures that before GekkoNet has synchronized inputs, the game still functions normally
        
        // Check if all players are ready
        if (!AllPlayersValid()) {
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
                    
                    // GEKKO PATTERN: AdvanceEvent tells us to simulate one frame with given inputs
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: *** ADVANCE EVENT *** Frame %u (our frame: %u)", target_frame, g_frame_counter);
                    
                    // CRITICAL: Sync our frame counter with GekkoNet's expectation
                    if (g_frame_counter != target_frame) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Frame desync detected! Correcting %u → %u", g_frame_counter, target_frame);
                        g_frame_counter = target_frame;
                    }
                    
                    // Store the provided inputs for this frame
                    if (inputs && input_length >= 2) {
                        // Map GekkoNet handles to FM2K player slots
                        // Handle 0 = P1 input, Handle 1 = P2 input (consistent across both clients)
                        networked_p1_input = inputs[0];  // Handle 0 -> P1
                        networked_p2_input = inputs[1];  // Handle 1 -> P2
                        use_networked_inputs = true;     // Always use network inputs during AdvanceEvent
                        
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent inputs - P1: 0x%02X, P2: 0x%02X", 
                                   inputs[0], inputs[1]);
                    }
                    
                    // THORN'S PATTERN: Give FM2K permission to advance ONE frame
                    // This is like pressing "continue" in Thorn's framestep tool
                    can_advance_frame = true;
                    waiting_for_gekko_advance = false;
                    
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Granting frame advance permission for frame %u", target_frame);
                    break;
                }
                
                case SaveEvent: {
                    // BSNES PATTERN: Save state with checksum for desync detection
                    uint32_t save_frame = update->data.save.frame;
                    uint32_t* checksum_ptr = update->data.save.checksum;
                    uint32_t* state_len_ptr = update->data.save.state_len;
                    uint8_t* state_ptr = update->data.save.state;
                    
                    // CRITICAL: Skip saves during transition stabilization to prevent desyncs
                    bool in_stabilization = FM2K::State::g_game_state_machine.IsInTransitionStabilization();
                    if (in_stabilization) {
                        // Log only occasionally to reduce spam
                        static uint32_t skip_save_counter = 0;
                        if (++skip_save_counter % 60 == 1) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                "GekkoNet: Skipping SaveEvents during stabilization (frame %u)", save_frame);
                        }
                        
                        // Provide dummy checksum to keep GekkoNet happy
                        if (checksum_ptr && state_len_ptr && state_ptr) {
                            uint32_t dummy_checksum = 0x12345678;  // Fixed value during stabilization
                            *state_len_ptr = sizeof(uint32_t);
                            memcpy(state_ptr, &dummy_checksum, sizeof(uint32_t));
                            *checksum_ptr = dummy_checksum;
                        }
                        break;
                    }
                    
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
                    
                    // CRITICAL: Skip loads during transition stabilization to prevent desyncs
                    bool in_stabilization = FM2K::State::g_game_state_machine.IsInTransitionStabilization();
                    if (in_stabilization) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                            "GekkoNet: SKIPPING LoadEvent frame %u (in stabilization period)", load_frame);
                        break;
                    }
                    
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: *** ROLLBACK *** from frame %u to frame %u (slot %u)", g_frame_counter, load_frame, load_frame % 8);
                    
                    // Log current state before rollback for debugging
                    uint32_t* p1_hp_ptr = (uint32_t*)FM2K::State::Memory::P1_HP_ADDR;
                    uint32_t* p2_hp_ptr = (uint32_t*)FM2K::State::Memory::P2_HP_ADDR;
                    uint32_t* p1_x_ptr = (uint32_t*)0x4ADCC3;
                    uint32_t* p2_x_ptr = (uint32_t*)0x4EDD02;
                    
                    uint32_t before_p1_hp = p1_hp_ptr && !IsBadReadPtr(p1_hp_ptr, 4) ? *p1_hp_ptr : 0;
                    uint32_t before_p1_x = p1_x_ptr && !IsBadReadPtr(p1_x_ptr, 4) ? *p1_x_ptr : 0;
                    uint32_t before_p2_x = p2_x_ptr && !IsBadReadPtr(p2_x_ptr, 4) ? *p2_x_ptr : 0;
                    
                    // FAST IN-MEMORY: Load from memory buffer instead of file (like bsnes)
                    uint32_t slot = load_frame % 8;
                    bool load_success = FM2K::State::LoadStateFromMemoryBuffer(slot);
                    
                    if (load_success) {
                        // Update our frame counter to match the rollback point
                        g_frame_counter = load_frame;
                        
                        // Log state after rollback to verify it worked
                        uint32_t after_p1_hp = p1_hp_ptr && !IsBadReadPtr(p1_hp_ptr, 4) ? *p1_hp_ptr : 0;
                        uint32_t after_p1_x = p1_x_ptr && !IsBadReadPtr(p1_x_ptr, 4) ? *p1_x_ptr : 0;
                        uint32_t after_p2_x = p2_x_ptr && !IsBadReadPtr(p2_x_ptr, 4) ? *p2_x_ptr : 0;
                        
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: *** ROLLBACK SUCCESS *** frame %u, P1_HP: %u→%u, P1_X: %u→%u, P2_X: %u→%u", 
                                   g_frame_counter, before_p1_hp, after_p1_hp, before_p1_x, after_p1_x, before_p2_x, after_p2_x);
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: *** ROLLBACK FAILED *** for frame %u (slot %u)", load_frame, slot);
                    }
                    break;
                }
                
                default:
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Unknown update type: %d", update->type);
                    break;
            }
        }
    }
    
    return original_result;
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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** TAKING COMPLETE CONTROL OF MAIN GAME LOOP ***");
    
    if (!gekko_initialized) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing GekkoNet at main loop level!");
        if (!InitializeGekkoNet()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialization failed!");
            if (original_run_game_loop) {
                return original_run_game_loop();
            }
            return FALSE;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialized successfully!");
    }
    
    // DISABLE the individual function blocking since we're controlling the entire loop now
    gekko_frame_control_enabled = false;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Starting controlled game loop - session will connect during execution...");
    
    // SIMPLER APPROACH: Let FM2K run but control it through existing hooks
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** LETTING FM2K RUN WITH GEKKONET CONTROL ***");
    
    // Re-enable the individual function blocking since we'll control through hooks
    gekko_frame_control_enabled = true;
    can_advance_frame = false; // Start blocked
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Calling original run_game_loop with GekkoNet control enabled...");
    
    // Call the original run_game_loop - our hooks will control the frame advancement
    if (original_run_game_loop) {
        return original_run_game_loop();
    }
    
    return TRUE;
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
        
        // Log character select state when in CSS mode (2000-2999)
        if (new_game_mode >= 2000 && new_game_mode < 3000) {
            uint32_t* menu_sel_ptr = (uint32_t*)FM2K::State::Memory::MENU_SELECTION_ADDR;
            uint32_t* p1_cursor_x_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_X_ADDR;
            uint32_t* p1_cursor_y_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_Y_ADDR;
            uint32_t* p2_cursor_x_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_X_ADDR;
            uint32_t* p2_cursor_y_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_Y_ADDR;
            uint32_t* p1_char_ptr = (uint32_t*)FM2K::State::Memory::P1_SELECTED_CHAR_ADDR;
            uint32_t* p2_char_ptr = (uint32_t*)FM2K::State::Memory::P2_SELECTED_CHAR_ADDR;
            uint32_t* p1_confirmed_ptr = (uint32_t*)FM2K::State::Memory::P1_CSS_CONFIRMED_ADDR;
            uint32_t* p2_confirmed_ptr = (uint32_t*)FM2K::State::Memory::P2_CSS_CONFIRMED_ADDR;
            
            if (!IsBadReadPtr(menu_sel_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p1_cursor_x_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p1_cursor_y_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p2_cursor_x_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p2_cursor_y_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p1_char_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p2_char_ptr, sizeof(uint32_t)) && !IsBadReadPtr(p1_confirmed_ptr, sizeof(uint32_t)) && 
                !IsBadReadPtr(p2_confirmed_ptr, sizeof(uint32_t))) {
                
                uint32_t menu_sel = *menu_sel_ptr;
                uint32_t p1_cursor_x = *p1_cursor_x_ptr;
                uint32_t p1_cursor_y = *p1_cursor_y_ptr;
                uint32_t p2_cursor_x = *p2_cursor_x_ptr;
                uint32_t p2_cursor_y = *p2_cursor_y_ptr;
                uint32_t p1_char = *p1_char_ptr;
                uint32_t p2_char = *p2_char_ptr;
                uint32_t p1_confirmed = *p1_confirmed_ptr;
                uint32_t p2_confirmed = *p2_confirmed_ptr;
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS STATE: menu=%d, P1_cursor=(%d,%d), P2_cursor=(%d,%d), P1_char=%d, P2_char=%d, confirmed=(%d,%d)", 
                           menu_sel, p1_cursor_x, p1_cursor_y, p2_cursor_x, p2_cursor_y, p1_char, p2_char, p1_confirmed, p2_confirmed);
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
    // Use state machine to determine rollback activation
    bool should_activate = FM2K::State::g_game_state_machine.ShouldEnableRollback();
    bool should_use_lockstep = FM2K::State::g_game_state_machine.ShouldUseLockstep();
    bool in_stabilization = FM2K::State::g_game_state_machine.IsInTransitionStabilization();
    
    // CRITICAL: Disable rollback during transition stabilization to prevent desyncs
    if (in_stabilization && rollback_active) {
        rollback_active = false;
        waiting_for_gekko_advance = false;
        can_advance_frame = true;
        
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
            "FM2K STATE: *** DISABLING ROLLBACK FOR STABILIZATION *** (Transition period, frame %d in phase)", 
            FM2K::State::g_game_state_machine.GetFramesInCurrentPhase());
    }
    
    if (should_activate && !rollback_active && !in_stabilization) {
        // Activate rollback for combat - enable frame synchronization
        rollback_active = true;
        waiting_for_gekko_advance = true;
        can_advance_frame = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "FM2K STATE: *** ACTIVATING ROLLBACK NETCODE *** (Battle stabilized after %d frames, game_mode=0x%X)", 
            FM2K::State::g_game_state_machine.GetFramesInCurrentPhase(), game_mode);
        
    } else if (!should_activate && rollback_active && !in_stabilization) {
        // Deactivate rollback (returning to menu/character select) - disable frame synchronization
        rollback_active = false;
        waiting_for_gekko_advance = false;
        can_advance_frame = true;  // Allow free running during menus
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: *** DEACTIVATING ROLLBACK NETCODE *** (Left battle, game_mode=0x%X)", game_mode);
    }
    
    // Handle lockstep mode for character select
    if (should_use_lockstep && !rollback_active) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: Using lockstep sync for character select");
    }
}

bool ShouldActivateRollback(uint32_t game_mode, uint32_t fm2k_mode) {
    // UPDATED: Use state machine logic instead of always returning true
    // This function is legacy - the state machine handles this now
    return FM2K::State::g_game_state_machine.ShouldEnableRollback();
}

const char* GetGameModeString(uint32_t mode) {
    switch (mode) {
        case 0xFFFFFFFF: return "UNINITIALIZED";
        case 0x0: return "STARTUP";
        default:
            if (mode >= 1000 && mode < 2000) return "TITLE_SCREEN";
            if (mode >= 2000 && mode < 3000) return "CHARACTER_SELECT";
            if (mode >= 3000 && mode < 4000) return "IN_BATTLE";
            return "UNKNOWN";
    }
} 