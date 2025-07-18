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
    // Call original function to advance FM2K
    int original_result = original_process_inputs ? original_process_inputs() : 0;
    g_frame_counter++;
    
    // Early logging to verify hook works
    if (g_frame_counter <= 3) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "MINIMAL ROLLBACK: Frame %u", g_frame_counter);
    }
    
    // GekkoNet rollback control (only if session is active)
    if (gekko_initialized && gekko_session && gekko_session_started) {
        // TEMPORARILY DISABLED: FM2K::ObjectTracking::g_object_tracker.UpdateTracking(g_frame_counter);
        
        // Send local input
        uint8_t local_input = (original_player_index == 0) ? 
            (uint8_t)(live_p1_input & 0xFF) : (uint8_t)(live_p2_input & 0xFF);
        gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        
        // INPUT TIMING LOGGING: Log input changes with frame numbers
        static uint8_t last_local_input = 0;
        if (local_input != last_local_input) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT TIMING: Player %d frame %u - input changed 0x%02X → 0x%02X", 
                       original_player_index, g_frame_counter, last_local_input, local_input);
            last_local_input = local_input;
        }
        
        // TEMPORARILY DISABLED: Object tracking analysis (debugging crashes)
        /*
        static uint32_t last_analysis_frame = 0;
        if (g_frame_counter - last_analysis_frame > 600) {
            auto stats = FM2K::ObjectTracking::g_object_tracker.GetStatistics();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "OBJECT ANALYSIS: Frame %u - Active: %u, Peak: %u, Avg: %u, Create/Del rates: %u/%u per 100f", 
                       g_frame_counter, stats.current_active, stats.peak_active, stats.avg_active, 
                       stats.creation_rate, stats.deletion_rate);
            
            if (g_frame_counter - last_analysis_frame > 3000) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== DETAILED OBJECT STRUCTURE ANALYSIS ===");
                FM2K::ObjectAnalysis::DumpDetailedObjectAnalysis();
                
                auto character_slots = FM2K::ObjectAnalysis::GetCharacterObjectSlots();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CHARACTER SLOTS IDENTIFIED: %zu characters found", character_slots.size());
                for (size_t i = 0; i < character_slots.size(); i++) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Character %zu: Slot %d", i + 1, character_slots[i]);
                }
                
                last_analysis_frame = g_frame_counter;
            } else {
                last_analysis_frame = g_frame_counter;
            }
        }
        */
        
        // Process GekkoNet events with MINIMAL SaveEvent/LoadEvent
        gekko_network_poll(gekko_session);
        int update_count = 0;
        auto updates = gekko_update_session(gekko_session, &update_count);
        
        for (int i = 0; i < update_count; i++) {
            auto update = updates[i];
            
            switch (update->type) {
                case AdvanceEvent: {
                    // Update networked inputs (this was working)
                    uint8_t new_net_p1 = update->data.adv.inputs[0];
                    uint8_t new_net_p2 = update->data.adv.inputs[1];
                    networked_p1_input = new_net_p1;
                    networked_p2_input = new_net_p2;
                    use_networked_inputs = true;
                    break;
                }
                    
                case SaveEvent: {
                    // PRODUCTION ROLLBACK: Object-aware state saving
                    
                    // Scan active objects in the pool
                    auto active_objects = FM2K::ObjectPool::Scanner::ScanActiveObjects();
                    
                    // Create rollback state
                    FM2K::ObjectPool::ObjectPoolState pool_state;
                    pool_state.frame_number = g_frame_counter;
                    pool_state.active_object_count = static_cast<uint32_t>(active_objects.size());
                    pool_state.objects = std::move(active_objects);
                    
                    // Calculate serialization size
                    uint32_t data_size = pool_state.GetSerializedSize();
                    
                    // Verify we don't exceed GekkoNet buffer limits
                    const uint32_t MAX_GEKKO_BUFFER = 8192; // Conservative limit
                    if (data_size > MAX_GEKKO_BUFFER) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                   "ROLLBACK SAVE ERROR: State too large (%u bytes > %u limit)", 
                                   data_size, MAX_GEKKO_BUFFER);
                        // Fall back to essential objects only (first 10 objects)
                        if (pool_state.objects.size() > 10) {
                            pool_state.objects.resize(10);
                            pool_state.active_object_count = 10;
                            data_size = pool_state.GetSerializedSize();
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                                      "ROLLBACK SAVE: Reduced to %u objects (%u bytes)", 
                                      pool_state.active_object_count, data_size);
                        }
                    }
                    
                    // Set GekkoNet save data
                    if (update->data.save.state_len) *update->data.save.state_len = data_size;
                    if (update->data.save.checksum) *update->data.save.checksum = FM2K::State::Fletcher32((const uint8_t*)&pool_state, sizeof(pool_state));
                    
                    if (update->data.save.state) {
                        // Serialize object pool state to GekkoNet buffer
                        if (pool_state.SerializeTo(update->data.save.state, data_size)) {
                            // Log every 100 frames for monitoring
                            if (update->data.save.frame % 100 == 0) {
                                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                           "🔄 ROLLBACK SAVE frame %u: %u objects, %u bytes (%.1f%% reduction)", 
                                           update->data.save.frame, pool_state.active_object_count, data_size,
                                           100.0f * (1.0f - (float)data_size / (pool_state.active_object_count * 382.0f)));
                            }
                        } else {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                       "ROLLBACK SAVE ERROR: Serialization failed for frame %u", 
                                       update->data.save.frame);
                        }
                    }
                    break;
                }
                    
                case LoadEvent: {
                    // PRODUCTION ROLLBACK: Object-aware state restoration
                    if (update->data.load.state && update->data.load.state_len > 0) {
                        
                        // Deserialize object pool state from GekkoNet buffer
                        FM2K::ObjectPool::ObjectPoolState pool_state;
                        if (pool_state.DeserializeFrom(update->data.load.state, update->data.load.state_len)) {
                            
                            // Clear entire object pool before restoration
                            FM2K::ObjectPool::Scanner::ClearObjectPool();
                            
                            // Restore frame counter
                            g_frame_counter = pool_state.frame_number;
                            
                            // Restore all objects to their exact slots
                            uint32_t restored_count = 0;
                            for (const auto& obj : pool_state.objects) {
                                if (FM2K::ObjectPool::Scanner::RestoreObjectToSlot(obj)) {
                                    restored_count++;
                                } else {
                                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                                              "ROLLBACK WARNING: Failed to restore object slot %u", 
                                              obj.slot_index);
                                }
                            }
                            
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                       "🔄 ROLLBACK to frame %u: %u/%u objects restored", 
                                       pool_state.frame_number, restored_count, pool_state.active_object_count);
                            
                            // Validate restoration
                            uint32_t current_count = FM2K::ObjectPool::Scanner::GetActiveObjectCount();
                            if (current_count != restored_count) {
                                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                                          "ROLLBACK WARNING: Object count mismatch (expected %u, got %u)", 
                                          restored_count, current_count);
                            }
                            
                        } else {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                       "ROLLBACK ERROR: Failed to deserialize state for frame %u (%u bytes)", 
                                       update->data.load.frame, update->data.load.state_len);
                        }
                        
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                                   "ROLLBACK ERROR: frame %u - no state data provided", 
                                   update->data.load.frame);
                    }
                    break;
                }
            }
        }
    }
    
    return original_result;
}

int __cdecl Hook_UpdateGameState() {
    // Monitor game state transitions for rollback management
    MonitorGameStateTransitions();
    
    // Track boot sequence objects during early frames
    static uint32_t update_count = 0;
    update_count++;
    if (update_count <= 10 || (update_count % 100 == 0 && update_count <= 1000)) {
        FM2K::BootAnalysis::AnalyzeBootSequenceObject();
    }
    
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