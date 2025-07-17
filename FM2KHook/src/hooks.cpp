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
    // Call original function to advance FM2K
    int original_result = original_process_inputs ? original_process_inputs() : 0;
    g_frame_counter++;
    
    // Early logging to verify hook works
    if (g_frame_counter <= 3) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "MINIMAL ROLLBACK: Frame %u", g_frame_counter);
    }
    
    // GekkoNet rollback control (only if session is active)
    if (gekko_initialized && gekko_session && gekko_session_started) {
        // Send local input
        uint8_t local_input = (original_player_index == 0) ? 
            (uint8_t)(live_p1_input & 0xFF) : (uint8_t)(live_p2_input & 0xFF);
        gekko_add_local_input(gekko_session, local_player_handle, &local_input);
        
        // Process GekkoNet events
        gekko_network_poll(gekko_session);
        int update_count = 0;
        auto updates = gekko_update_session(gekko_session, &update_count);
        
        for (int i = 0; i < update_count; i++) {
            auto update = updates[i];
            
            switch (update->type) {
                case AdvanceEvent:
                    // Update networked inputs
                    networked_p1_input = update->data.adv.inputs[0];
                    networked_p2_input = update->data.adv.inputs[1];
                    use_networked_inputs = true;
                    break;
                    
                case SaveEvent:
                    FM2K::State::SaveStateToMemoryBuffer(update->data.save.frame % 8, update->data.save.frame);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SAVE frame %u", update->data.save.frame);
                    break;
                    
                case LoadEvent:
                    if (FM2K::State::LoadStateFromMemoryBuffer(update->data.load.frame % 8)) {
                        g_frame_counter = update->data.load.frame;
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "ROLLBACK to frame %u", update->data.load.frame);
                    }
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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** INITIALIZING GEKKONET AND CALLING ORIGINAL LOOP ***");
    
    if (!gekko_initialized) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing GekkoNet...");
        if (!InitializeGekkoNet()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet failed, using original loop");
            return original_run_game_loop ? original_run_game_loop() : FALSE;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialized!");
    }
    
    gekko_session_started = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Calling original run_game_loop - GekkoNet will control via hooks...");
    
    // Let FM2K handle everything - our hooks will control frame advancement
    return original_run_game_loop ? original_run_game_loop() : FALSE;
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