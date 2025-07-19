#include "hooks.h"
#include "globals.h"
#include "gekkonet_hooks.h"
#include "state_manager.h"
#include "logging.h"
#include "shared_mem.h"
#include "game_state_machine.h"
#include "css_sync.h"
#include "object_tracker.h"
#include "object_analysis.h"
#include "object_pool_scanner.h"
#include "boot_object_analyzer.cpp"
#include <windows.h>
#include <mmsystem.h>

// Use global function pointers from globals.h

// New hook for boot-to-character-select hack
int __cdecl Hook_InitializeGameMode() {
    // Set character select mode flag to 1 (vs player mode)
    uint32_t* char_select_mode_ptr = (uint32_t*)FM2K::State::Memory::CHARACTER_SELECT_MODE_ADDR;
    if (!IsBadReadPtr(char_select_mode_ptr, sizeof(uint32_t))) {
        *char_select_mode_ptr = 1;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Set character select mode flag to 1 (vs player)");
    }
    
    // Change initialization object from 0x11 to 0x0A to boot to character select
    uint8_t* init_object_ptr = (uint8_t*)0x409CDA;
    if (!IsBadReadPtr(init_object_ptr, sizeof(uint8_t))) {
        *init_object_ptr = 0x0A;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Modified initialization object from 0x11 to 0x0A");
    }
    
    // Call original function if it exists
    if (original_initialize_game_mode) {
        return original_initialize_game_mode();
    }
    
    return 0;
}

// Hook implementations
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    int original_input = original_get_player_input ? original_get_player_input(player_id, input_type) : 0;
    
    // Always capture live inputs for networking
    if (player_id == 0) {
        live_p1_input = original_input;
    } else if (player_id == 1) {
        live_p2_input = original_input;
        // DEBUG: Log when P2 input is captured
        static uint32_t p2_input_count = 0;
        p2_input_count++;
        if (p2_input_count % 100 == 0 || original_input != 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG P2 INPUT CAPTURE: original_input=0x%02X, live_p2_input=0x%02X", 
                       original_input, live_p2_input);
        }
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
    // In lockstep/rollback mode, the game's frame advancement is handled inside the AdvanceEvent.
    // We do nothing here to allow GekkoNet to control the frame pacing.
    if (!waiting_for_gekko_advance) {
        // If not waiting for GekkoNet, we're in a non-networked or pre-session state.
        // Call the original function to let the game run normally.
        if (original_process_inputs) {
            original_process_inputs();
        }
        g_frame_counter++;
    }
    
    // Early logging to verify hook works
    if (g_frame_counter <= 3) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Frame %u", g_frame_counter);
    }
    
    // GekkoNet rollback control (only if session is active)
    if (gekko_initialized && gekko_session && gekko_session_started) {
        // Update CSS synchronization
        FM2K::CSS::g_css_sync.Update();
        
        // Send local input based on our GekkoNet handle (correct GekkoNet model)
        // Handle 0 (Host) sends P1 input, Handle 1 (Client) sends P2 input
        uint8_t local_input;
        if (local_player_handle == 0) {
            local_input = (uint8_t)(live_p1_input & 0xFF);
        } else {
            local_input = (uint8_t)(live_p2_input & 0xFF);
            // CRITICAL DEBUG: Why is Player 2 input always 0?
            if (live_p2_input != 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CRITICAL: Player 2 has input 0x%02X but local_input=0x%02X", 
                           live_p2_input, local_input);
            }
        }
        
        // DEBUG: Log input values every 100 frames to debug Player 2 issue
        static uint32_t debug_frame_count = 0;
        debug_frame_count++;
        if (debug_frame_count % 100 == 0 || (local_player_handle == 1 && local_input != 0)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DEBUG INPUT: Handle %d, live_p1=0x%02X, live_p2=0x%02X, local_input=0x%02X, calculation=(handle==0)?p1:p2", 
                       local_player_handle, live_p1_input & 0xFF, live_p2_input & 0xFF, local_input);
        }
        
        gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        
        // INPUT TIMING LOGGING: Log input changes with frame numbers
        static uint8_t last_local_input = 0;
        if (local_input != last_local_input) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT TIMING: Handle %d frame %u - input changed 0x%02X → 0x%02X", 
                       local_player_handle, g_frame_counter, last_local_input, local_input);
            last_local_input = local_input;
        }
        
        // Process GekkoNet events following the example pattern
        gekko_network_poll(gekko_session);
        
        // First handle session events (disconnects, desyncs)
        int session_event_count = 0;
        auto session_events = gekko_session_events(gekko_session, &session_event_count);
        for (int i = 0; i < session_event_count; i++) {
            auto event = session_events[i];
            if (event->type == DesyncDetected) {
                auto desync = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DESYNC: frame %d, remote handle %d, local checksum %u, remote checksum %u", 
                           desync.frame, desync.remote_handle, desync.local_checksum, desync.remote_checksum);
            } else if (event->type == PlayerDisconnected) {
                auto disco = event->data.disconnected;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "DISCONNECT: player handle %d", disco.handle);
            }
        }
        
        // Then handle game updates  
        int update_count = 0;
        auto updates = gekko_update_session(gekko_session, &update_count);
        
        for (int i = 0; i < update_count; i++) {
            auto update = updates[i];
            
            switch (update->type) {
                case AdvanceEvent: {
                    // Always apply the synchronized inputs first.
                    networked_p1_input = update->data.adv.inputs[0];
                    networked_p2_input = update->data.adv.inputs[1];
                    use_networked_inputs = true;

                    // Check if the remote player sent a confirmation signal.
                    uint8_t remote_input = is_host ? networked_p2_input : networked_p1_input;
                    if (remote_input == 0xFF) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ADVANCE EVENT: Remote player sent 0xFF confirmation signal");
                        FM2K::CSS::g_css_sync.ReceiveRemoteConfirmation();
                    }

                    // DEBUG: Log received inputs to see if Player 2's inputs are coming through
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ADVANCE EVENT: Frame %u - Received P1=0x%02X, P2=0x%02X", 
                               update->data.adv.frame, networked_p1_input, networked_p2_input);

                    // Now, let the original game code run with the synchronized inputs.
                    if (original_process_inputs) {
                        original_process_inputs();
                    }
                    g_frame_counter++;
                    break;
                }
                    
                case SaveEvent: {
                    // Query the state machine to determine the current strategy
                    auto strategy = FM2K::State::g_game_state_machine.GetSyncStrategy();

                    if (strategy == FM2K::State::SyncStrategy::ROLLBACK) {
                        // We are in active, stable battle. Perform a full state save.
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: Full Rollback Save at frame %u", update->data.save.frame);
                        
                        try {
                            // Scan active objects in the pool with error checking
                            auto active_objects = FM2K::ObjectPool::Scanner::ScanActiveObjects();
                            
                            // Create rollback state
                            FM2K::ObjectPool::ObjectPoolState pool_state;
                            pool_state.frame_number = g_frame_counter;
                            pool_state.active_object_count = static_cast<uint32_t>(active_objects.size());
                            pool_state.objects = std::move(active_objects);
                            
                            // Calculate serialization size
                            uint32_t data_size = pool_state.GetSerializedSize();
                            
                            // Verify we don't exceed GekkoNet buffer limits
                            const uint32_t MAX_GEKKO_BUFFER = 4096;
                            if (data_size > MAX_GEKKO_BUFFER) {
                                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                                           "SaveEvent: State size %u > %u, reducing objects", 
                                           data_size, MAX_GEKKO_BUFFER);
                                
                                // Reduce to fit in buffer
                                size_t max_objects = (MAX_GEKKO_BUFFER - 8) / sizeof(FM2K::ObjectPool::CompactObject);
                                if (pool_state.objects.size() > max_objects) {
                                    pool_state.objects.resize(max_objects);
                                    pool_state.active_object_count = static_cast<uint32_t>(max_objects);
                                    data_size = pool_state.GetSerializedSize();
                                }
                            }
                            
                            // Set GekkoNet save data
                            if (update->data.save.state_len) *update->data.save.state_len = data_size;
                            if (update->data.save.checksum) *update->data.save.checksum = g_frame_counter;
                            
                            if (update->data.save.state) {
                                if (pool_state.SerializeTo(update->data.save.state, data_size)) {
                                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                               "Battle SaveEvent frame %u: %u objects, %u bytes", 
                                               update->data.save.frame, pool_state.active_object_count, data_size);
                                } else {
                                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                               "SaveEvent ERROR: Serialization failed for frame %u", 
                                               update->data.save.frame);
                                }
                            }
                            
                        } catch (...) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                       "SaveEvent CRASH: Exception caught in frame %u", 
                                       update->data.save.frame);
                            
                            // Emergency fallback
                            if (update->data.save.state_len) *update->data.save.state_len = 8;
                            if (update->data.save.checksum) *update->data.save.checksum = 0xFFFFFFFF;
                            if (update->data.save.state) {
                                memset(update->data.save.state, 0xFF, 8);
                            }
                        }
                    } else {
                        // We are in lockstep (menus, CSS, or transition). Perform a minimal "dummy" save.
                        // GekkoNet requires a state buffer, but its contents don't matter for lockstep.
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: Lockstep (Minimal) Save at frame %u", update->data.save.frame);
                        if (update->data.save.state_len) *update->data.save.state_len = 8; // A small, non-zero size.
                        if (update->data.save.checksum) *update->data.save.checksum = 0xDEADBEEF + update->data.save.frame;
                        if (update->data.save.state) {
                            // Fill with a placeholder value for clarity in debugging.
                            memset(update->data.save.state, 0xAA, 8);
                        }
                    }
                    break;
                }
                    
                case LoadEvent: {
                    auto strategy = FM2K::State::g_game_state_machine.GetSyncStrategy();

                    if (strategy == FM2K::State::SyncStrategy::ROLLBACK) {
                        // Only load state if we are in a rollback-enabled phase.
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LoadEvent: Full Rollback Load to frame %u", update->data.load.frame);
                        
                        try {
                            // Validate load data
                            if (!update->data.load.state || update->data.load.state_len < 8) {
                                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                                           "LoadEvent: Invalid state data for frame %u", 
                                           update->data.load.frame);
                                break;
                            }
                            
                            // Deserialize object pool state from GekkoNet buffer
                            FM2K::ObjectPool::ObjectPoolState pool_state;
                            if (pool_state.DeserializeFrom(update->data.load.state, update->data.load.state_len)) {
                                
                                // Restore frame counter
                                g_frame_counter = pool_state.frame_number;
                                
                                // CRITICAL: Clear the entire pool before restoring to prevent stale objects
                                FM2K::ObjectPool::Scanner::ClearObjectPool();
                                
                                // Restore all objects to their exact slots
                                uint32_t restored_count = 0;
                                for (const auto& obj : pool_state.objects) {
                                    if (FM2K::ObjectPool::Scanner::RestoreObjectToSlot(obj)) {
                                        restored_count++;
                                    }
                                }
                                
                                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                           "Battle LoadEvent to frame %u: %u/%u objects restored", 
                                           pool_state.frame_number, restored_count, pool_state.active_object_count);
                                
                            } else {
                                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                           "LoadEvent ERROR: Failed to deserialize state for frame %u", 
                                           update->data.load.frame);
                            }
                            
                        } catch (...) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                       "LoadEvent CRASH: Exception caught in frame %u", 
                                       update->data.load.frame);
                        }
                    } else {
                        // In lockstep mode, we NEVER load state. The game progresses naturally.
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LoadEvent: Ignored during Lockstep frame %u", update->data.load.frame);
                    }
                    break;
                }
            }
        }
    }
    
    return 0; // Return 0 as the game's frame advancement is handled by GekkoNet
}

int __cdecl Hook_UpdateGameState() {
    // Monitor game state transitions for rollback management
    MonitorGameStateTransitions();
    
    // TEMPORARILY DISABLED: Boot analysis (causing console spam and crashes)
    // Track boot sequence objects during early frames
    // static uint32_t update_count = 0;
    // update_count++;
    // if (update_count <= 10 || (update_count % 100 == 0 && update_count <= 1000)) {
    //     FM2K::BootAnalysis::AnalyzeBootSequenceObject();
    // }
    
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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** REIMPLEMENTING FM2K MAIN LOOP WITH GEKKONET CONTROL ***");
    
    if (!gekko_initialized) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing GekkoNet...");
        if (!InitializeGekkoNet()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet failed, using original loop");
            return original_run_game_loop ? original_run_game_loop() : FALSE;
        }
        
        // TEMPORARILY DISABLED: Object tracker initialization (debugging crashes)
        // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing adaptive object tracker...");
        // FM2K::ObjectTracking::g_object_tracker.Initialize();
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet and object tracking initialized!");
    }
    
    // FM2K timing variables (from IDA analysis of run_game_loop at 0x405AD0)
    uint32_t* g_frame_time_ms = (uint32_t*)0x447EE4;        // Fixed 10ms (100 FPS)
    uint32_t* g_last_frame_time = (uint32_t*)0x447EE8;      // Last frame timestamp
    uint32_t* g_frame_sync_flag = (uint32_t*)0x447EEC;      // Frame sync state
    uint32_t* g_frame_time_delta = (uint32_t*)0x447EF0;     // Frame timing delta
    uint32_t* g_frame_skip_count = (uint32_t*)0x447EF4;     // Frame skip counter
    
    // EXACT FM2K INITIALIZATION (from decompiled run_game_loop)
    *g_frame_time_ms = 10;                                  // Initialize game loop timing - Fixed 100 FPS (10ms per frame)
    *g_last_frame_time = timeGetTime();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Running initial 8 warmup frames...");
    int init_loop_count = 8;                                // Initial game state warmup - Run 8 frames of game logic
    do {
        if (original_update_game) {
            original_update_game();                         // update_game_state() call
        }
        --init_loop_count;
    } while (init_loop_count);
    
    *g_last_frame_time = timeGetTime();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Warmup complete, starting GekkoNet-controlled main loop...");
    
    // Wait for GekkoNet connection before starting main game loop
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Waiting for GekkoNet connection...");
    uint32_t connection_attempts = 0;
    while (!AllPlayersValid() && connection_attempts < 1500) {  // 15 second timeout
        gekko_network_poll(gekko_session);
        int temp_count = 0;
        gekko_update_session(gekko_session, &temp_count);
        
        // Process Windows messages during connection
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return TRUE;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        Sleep(10);
        connection_attempts++;
        
        if (connection_attempts % 100 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Connection attempt %u/1500...", connection_attempts);
        }
    }
    
    if (!AllPlayersValid()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Connection timeout! Falling back to original loop.");
        return original_run_game_loop ? original_run_game_loop() : FALSE;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet connected! Calling original FM2K loop...");
    gekko_session_started = true;
    
    // SIMPLE APPROACH: Just call the original run_game_loop
    // Our Hook_ProcessGameInputs will handle the rollback logic
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Delegating to original FM2K main loop...");
    
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
    
    // Update the game state machine with current mode
    if (new_game_mode != 0xFFFFFFFF) {
        FM2K::State::g_game_state_machine.Update(new_game_mode);
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
    // SIMPLIFIED: Use state machine to determine rollback activation
    bool should_activate_rollback = FM2K::State::g_game_state_machine.ShouldEnableRollback();
    bool should_use_lockstep = FM2K::State::g_game_state_machine.ShouldUseLockstep();
    bool in_stabilization = FM2K::State::g_game_state_machine.IsInTransitionStabilization();
    
    // Determine if we need to be waiting for GekkoNet to advance the frame
    bool needs_frame_sync = (should_activate_rollback || should_use_lockstep) && !in_stabilization;

    // CRITICAL: Disable rollback during transition stabilization to prevent desyncs
    if (in_stabilization && waiting_for_gekko_advance) {
        waiting_for_gekko_advance = false;
        rollback_active = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "FM2K STATE: Disabling frame sync for stabilization (phase: %d, frames: %d)", 
            static_cast<int>(FM2K::State::g_game_state_machine.GetCurrentPhase()),
            FM2K::State::g_game_state_machine.GetFramesInCurrentPhase());
    }
    
    // Activate frame synchronization if we need either rollback or lockstep
    if (needs_frame_sync && !waiting_for_gekko_advance) {
        waiting_for_gekko_advance = true;
        rollback_active = should_activate_rollback; // Only set rollback_active if we're actually in battle
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "FM2K STATE: Activating %s sync (game_mode=0x%X)", 
            rollback_active ? "ROLLBACK" : "LOCKSTEP",
            game_mode);
        
    } else if (!needs_frame_sync && waiting_for_gekko_advance) {
        // Deactivate frame synchronization (returning to menu, etc.)
        waiting_for_gekko_advance = false;
        rollback_active = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K STATE: Deactivating frame sync (game_mode=0x%X)", game_mode);
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