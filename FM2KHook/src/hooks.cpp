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

// ARCHITECTURE FIX: Real input capture following CCCaster/GekkoNet pattern
static void CaptureRealInputs() {
    // Following the pattern from GekkoNet SDL2 example and CCCaster
    // This captures actual keyboard/controller input before the game processes it
    
    // For now, we'll read the inputs directly from the game's memory addresses
    // In the future, this could be enhanced to use direct keyboard/controller APIs
    
    // BACK TO WORKING APPROACH: Use Hook_GetPlayerInput to capture inputs at source
    // Just like dllmain_orig.cpp - inputs are captured when FM2K calls Hook_GetPlayerInput
    // Don't override them here - let the input hooks do their job
    
    // Simplified input capture without excessive logging
}

// Use global function pointers from globals.h

// Simplified input conversion without debug logging
static uint32_t ConvertNetworkInputToGameFormat(uint32_t network_input) {
    uint32_t game_input = 0;
    
    if (network_input & 0x01) game_input |= 0x001;  // LEFT
    if (network_input & 0x02) game_input |= 0x002;  // RIGHT
    if (network_input & 0x04) game_input |= 0x004;  // UP
    if (network_input & 0x08) game_input |= 0x008;  // DOWN
    if (network_input & 0x10) game_input |= 0x010;  // BUTTON1 (START)
    if (network_input & 0x20) game_input |= 0x020;  // BUTTON2
    if (network_input & 0x40) game_input |= 0x040;  // BUTTON3
    if (network_input & 0x80) game_input |= 0x080;  // BUTTON4
    
    return game_input;
}

// New hook for boot-to-character-select hack
// This hook modifies the game's initialization to boot directly to character select screen
// instead of showing the title screen and splash screens. It does this by:
// 1. Setting the character select mode flag to 1 (vs player mode instead of vs CPU)
// 2. Changing the initialization object byte from 0x11 to 0x0A to skip to character select
void ApplyBootToCharacterSelectPatches() {
    // Change initialization object from 0x11 to 0x0A to boot to character select
    uint8_t* init_object_ptr = (uint8_t*)0x409CD9;
    if (!IsBadReadPtr(init_object_ptr, sizeof(uint16_t))) {
        // Make the memory writable
        DWORD old_protect;
        if (VirtualProtect(init_object_ptr, 2, PAGE_EXECUTE_READWRITE, &old_protect)) {
            // Write the instruction: 6A 0A (push 0x0A)
            init_object_ptr[0] = 0x6A;  // push instruction
            init_object_ptr[1] = 0x0A;  // immediate value
            
            // Restore original protection
            VirtualProtect(init_object_ptr, 2, old_protect, &old_protect);
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Wrote instruction 6A 0A at 0x409CD9");
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to make memory writable at 0x409CD9");
        }
    }
}

// Hook implementations
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    // CRITICAL FIX: Both clients read P1 controls locally, but map to their network slot
    // This matches fighting game conventions - everyone uses same local controls (WASD/arrow keys)
    
    int original_input = 0;
    
    if (player_id == 0) {
        // P1 slot
        if (::is_host) {
            // Host: Read keyboard for P1, store in live_p1_input
            original_input = original_get_player_input ? original_get_player_input(0, input_type) : 0;
            live_p1_input = original_input;
        } else {
            // Client: P1 slot gets 0 (remote player), keyboard goes to live_p2_input
            original_input = original_get_player_input ? original_get_player_input(0, input_type) : 0;
            live_p2_input = original_input;  // Store for network transmission
            original_input = 0;  // FM2K P1 slot gets 0 (remote player)
        }
    } else if (player_id == 1) {
        // P2 slot
        if (::is_host) {
            // Host: P2 slot gets 0 (remote player input comes from network)
            original_input = 0;
        } else {
            // Client: P2 slot gets local keyboard input (already captured above)
            original_input = live_p2_input;  // Use already captured input
        }
    }
    
    // Simplified hook without excessive logging
    
    // Simplified CSS: Just pass synchronized inputs without cursor manipulation
    if (FM2K::State::g_game_state_machine.GetCurrentPhase() == FM2K::State::GamePhase::CHARACTER_SELECT) {
        if (use_networked_inputs && gekko_initialized && gekko_session) {
            if (player_id == 0) {
                return ConvertNetworkInputToGameFormat(networked_p1_input);
            } else if (player_id == 1) {
                return ConvertNetworkInputToGameFormat(networked_p2_input);
            }
        }
        return original_input;
    }
    
    // During battle: Use synchronized networked inputs if available
    if (use_networked_inputs && gekko_initialized && gekko_session) {
        if (player_id == 0) {
            return ConvertNetworkInputToGameFormat(networked_p1_input);
        } else if (player_id == 1) {
            return ConvertNetworkInputToGameFormat(networked_p2_input);
        }
    }
    
    // Fall back to original input (like dllmain_orig.cpp)
    return original_input;
}

int __cdecl Hook_ProcessGameInputs() {
    // In lockstep/rollback mode, the game's frame advancement is handled inside the AdvanceEvent.
    // We do nothing here to allow GekkoNet to control the frame pacing.
    if (!waiting_for_gekko_advance) {
        // If not waiting for GekkoNet, we're in a non-networked or pre-session state.
        // But still add timing consideration for CSS to prevent rapid cursor movements
        bool in_css = (FM2K::State::g_game_state_machine.GetCurrentPhase() == FM2K::State::GamePhase::CHARACTER_SELECT);
        
        if (in_css && gekko_session_started) {
            // Even in non-networked mode, add slight delay during CSS for consistency
            Sleep(1);
        }
        
        // Call the original function to let the game run normally.
        if (original_process_inputs) {
            original_process_inputs();
        }
        g_frame_counter++;
    }
    
    // Early logging to verify hook works (only first few frames)
    if (g_frame_counter <= 3) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Frame %u", g_frame_counter);
    }
    
    // GekkoNet rollback control (only if session is active)
    if (gekko_initialized && gekko_session && gekko_session_started) {
        // ARCHITECTURE FIX: Proper input capture following CCCaster/GekkoNet pattern
        // 1. CAPTURE: Read actual controller/keyboard inputs (like CCCaster's updateControls)
        CaptureRealInputs();
        
        // Update CSS synchronization only during character select phase (reduced frequency)
        auto current_phase = FM2K::State::g_game_state_machine.GetCurrentPhase();
        if (current_phase == FM2K::State::GamePhase::CHARACTER_SELECT) {
            // Only update CSS sync every 5 frames to reduce lag
            static uint32_t css_update_counter = 0;
            if (++css_update_counter % 5 == 0) {
                FM2K::CSS::g_css_sync.Update();
            }
        }
        
        // 2. SEND: Send local input to GekkoNet (like GekkoNet example's gekko_add_local_input)
        // GEKKONET-STYLE: Only send OUR keyboard input, regardless of player slot
        uint8_t local_input;
        const char* input_source;
        if (::is_host) {
            local_input = (uint8_t)(live_p1_input & 0xFF); // Host's keyboard input
            input_source = "HOST_KEYBOARD";
        } else {
            local_input = (uint8_t)(live_p2_input & 0xFF); // Client's keyboard input  
            input_source = "CLIENT_KEYBOARD";
        }
        
        // CRITICAL: Apply CSS input filtering during character select to prevent desyncs
        if (FM2K::State::g_game_state_machine.GetCurrentPhase() == FM2K::State::GamePhase::CHARACTER_SELECT) {
            uint8_t player_num = ::is_host ? 1 : 2;
            uint32_t filtered_input = FM2K::CSS::g_css_sync.ValidateAndFilterCSSInput(local_input, player_num, g_frame_counter);
            local_input = (uint8_t)(filtered_input & 0xFF);
        }
        
        // Simplified input sending without excessive logging
        
        gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        
        // Simplified input timing without logging
        
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
                    
                    // CRITICAL DEBUG: Log the exact inputs received from GekkoNet
                    uint8_t received_p1 = update->data.adv.inputs[0];
                    uint8_t received_p2 = update->data.adv.inputs[1];
                    
                    // Only log ADVANCE_EVENT_RAW for non-zero inputs or occasionally
                    static uint32_t advance_log_count = 0;
                    if ((received_p1 != 0 || received_p2 != 0) && (++advance_log_count % 200 == 0)) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                                   "ADVANCE_EVENT_RAW: Frame %u - GekkoNet delivered P1=0x%02X, P2=0x%02X (orig_player=%d, is_host=%s)", 
                                   update->data.adv.frame, received_p1, received_p2, original_player_index, is_host ? "YES" : "NO");
                    }
                    
                    // Always apply the synchronized inputs first.
                    networked_p1_input = received_p1;
                    networked_p2_input = received_p2;
                    use_networked_inputs = true;
                    
                    // Simplified synchronization without excessive logging

                    // Check if the remote player sent a confirmation signal.
                    uint8_t remote_input = is_host ? networked_p2_input : networked_p1_input;
                    if (remote_input == 0xFF) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ADVANCE EVENT: Remote player sent 0xFF confirmation signal");
                        FM2K::CSS::g_css_sync.ReceiveRemoteConfirmation();
                        
                        // IMPORTANT: Filter out 0xFF from normal gameplay inputs
                        // Replace 0xFF with 0x00 to prevent invalid game inputs
                        if (is_host) {
                            networked_p2_input = 0x00;
                        } else {
                            networked_p1_input = 0x00;
                        }
                        
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ADVANCE EVENT: Filtered out 0xFF confirmation signal for gameplay");
                    }

                    // Simplified advance event processing

                    // Simplified direction tracking without logging

                    // Simplified movement detection without logging
                    
                    // Normal frame advancement (allow rollback to work)
                    if (original_process_inputs) {
                        original_process_inputs();
                    }
                    g_frame_counter++;
                    
                    // Simplified post-processing without cursor tracking
                    
                    // Simplified post-processing without cursor tracking
                    break;
                }
                    
                case SaveEvent: {
                    // CCCaster Hybrid Approach: Only save rollback states during battle, not CSS
                    auto current_phase = FM2K::State::g_game_state_machine.GetCurrentPhase();
                    auto strategy = FM2K::State::g_game_state_machine.GetSyncStrategy();

                    if (current_phase == FM2K::State::GamePhase::CHARACTER_SELECT) {
                        // CCCaster approach: No state saves during CSS - use minimal dummy save
                        // Only log occasionally to reduce spam
                        if (update->data.save.frame % 100 == 0) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: CSS Skip (no rollback saves during character select) at frame %u", update->data.save.frame);
                        }
                        if (update->data.save.state_len) *update->data.save.state_len = 8;
                        if (update->data.save.checksum) *update->data.save.checksum = 0xC5500000 + update->data.save.frame;
                        if (update->data.save.state) {
                            memset(update->data.save.state, 0xCC, 8);  // CSS marker
                        }
                    } else if (strategy == FM2K::State::SyncStrategy::ROLLBACK) {
                        // Additional safety checks before object scanning
                        auto& state_machine = FM2K::State::g_game_state_machine;
                        bool safe_to_scan = true;
                        
                        // Validate we're truly in stable battle state
                        if (state_machine.IsInBattleStabilization()) {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: Still in battle stabilization, deferring object scanning");
                            safe_to_scan = false;
                        }
                        
                        // Ensure enough frames have passed since battle start
                        uint32_t frames_in_battle = state_machine.GetFramesInBattle();
                        if (frames_in_battle < 10) {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: Too early in battle (%u frames), deferring object scanning", frames_in_battle);
                            safe_to_scan = false;
                        }
                        
                        // TEMPORARY: Disable all object scanning during battle to prevent crashes
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: Battle minimal save (object scanning disabled) at frame %u (%u frames in battle)", update->data.save.frame, frames_in_battle);
                        if (update->data.save.state_len) *update->data.save.state_len = 8;
                        if (update->data.save.checksum) *update->data.save.checksum = 0xBABE0000 + update->data.save.frame;
                        if (update->data.save.state) {
                            memset(update->data.save.state, 0xBB, 8);  // Battle minimal marker
                        }
                        break; // Skip object scanning entirely
                    } else {
                        // We are in lockstep (menus, CSS, or transition). Perform a minimal "dummy" save.
                        // GekkoNet requires a state buffer, but its contents don't matter for lockstep.
                        // Only log lockstep saves periodically to reduce spam
                        static uint32_t last_lockstep_log_frame = 0;
                        if (update->data.save.frame - last_lockstep_log_frame >= 300) { // Log every 300 frames (5 seconds at 60fps)
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SaveEvent: Lockstep (Minimal) Save at frame %u", update->data.save.frame);
                            last_lockstep_log_frame = update->data.save.frame;
                        }
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
                    // CCCaster Hybrid Approach: Only load rollback states during battle, not CSS
                    auto current_phase = FM2K::State::g_game_state_machine.GetCurrentPhase();
                    auto strategy = FM2K::State::g_game_state_machine.GetSyncStrategy();

                    if (current_phase == FM2K::State::GamePhase::CHARACTER_SELECT) {
                        // CCCaster approach: No state loads during CSS - ignore rollback loads
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LoadEvent: CSS Skip (no rollback loads during character select) to frame %u", update->data.load.frame);
                    } else if (strategy == FM2K::State::SyncStrategy::ROLLBACK) {
                        // Additional safety checks before object pool restoration
                        auto& state_machine = FM2K::State::g_game_state_machine;
                        bool safe_to_load = true;
                        
                        // Validate we're truly in stable battle state  
                        if (state_machine.IsInBattleStabilization()) {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "LoadEvent: Still in battle stabilization, deferring object restoration");
                            safe_to_load = false;
                        }
                        
                        // Ensure enough frames have passed since battle start
                        uint32_t frames_in_battle = state_machine.GetFramesInBattle();
                        if (frames_in_battle < 10) {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "LoadEvent: Too early in battle (%u frames), deferring object restoration", frames_in_battle);
                            safe_to_load = false;
                        }
                        
                        // TEMPORARY: Disable all object restoration during battle to prevent crashes
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LoadEvent: Skipping rollback load (object restoration disabled) to frame %u (%u frames in battle)", update->data.load.frame, frames_in_battle);
                        break; // Skip object restoration entirely
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
    
    // Set character select mode flag after memory clearing
    uint8_t* char_select_mode_ptr = (uint8_t*)FM2K::State::Memory::CHARACTER_SELECT_MODE_ADDR;
    if (!IsBadReadPtr(char_select_mode_ptr, sizeof(uint8_t))) {
        DWORD old_protect;
        if (VirtualProtect(char_select_mode_ptr, sizeof(uint8_t), PAGE_READWRITE, &old_protect)) {
            *char_select_mode_ptr = 1;
            VirtualProtect(char_select_mode_ptr, sizeof(uint8_t), old_protect, &old_protect);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Set character select mode flag to 1 after memory clearing");
        }
    }
    
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
    
    // Apply boot-to-character-select patches directly
    ApplyBootToCharacterSelectPatches();
    
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
        
        // Simplified CSS mode detection without state logging
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