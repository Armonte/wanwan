#include "hooks.h"
#include "globals.h"
#include "logging.h"
#include "input_handler.h"
#include "savestate.h"
#include "debug_features.h"
#include "game_patches.h"
#include "state_monitor.h"
#include "shared_mem.h"
// REMOVED: #include "css_handler.h" - CSS delayed input system removed
#include "gekkonet_hooks.h"
#include "imgui_overlay.h"
#include <MinHook.h>
#include <windows.h>
#include <cstdlib>
#include <cstring>

// Original function pointers

// FM2K internal functions we need to call directly
static ProcessGameInputsFunc process_game_inputs_FRAMESTEP_HOOK = (ProcessGameInputsFunc)0x4146d0;
static UpdateGameStateFunc update_game_state = (UpdateGameStateFunc)0x404d20;
static RenderGameFunc render_game = (RenderGameFunc)0x404dd0;


bool InitializeHooks() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing MinHook...");
    
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: MH_Initialize failed: %d", mh_init);
        return false;
    }
    
    // CCCASTER APPROACH: Don't hook ProcessGameInputs - let it run naturally
    // The game will read inputs we write to memory in UpdateGameState

    // Hook GetPlayerInput
    void* getInputFuncAddr = (void*)FM2K::State::Memory::GET_PLAYER_INPUT_ADDR;
    if (MH_CreateHook(getInputFuncAddr, (void*)Hook_GetPlayerInput, (void**)&original_get_player_input) != MH_OK ||
        MH_EnableHook(getInputFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook GetPlayerInput");
        MH_Uninitialize();
        return false;
    }

    // Hook UpdateGameState
    void* updateFuncAddr = (void*)FM2K::State::Memory::UPDATE_GAME_ADDR;
    if (MH_CreateHook(updateFuncAddr, (void*)Hook_UpdateGameState, (void**)&original_update_game) != MH_OK ||
        MH_EnableHook(updateFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook UpdateGameState");
        MH_Uninitialize();
        return false;
    }

    // Hook RunGameLoop
    void* runGameLoopFuncAddr = (void*)FM2K::State::Memory::RUN_GAME_LOOP_ADDR;
    if (MH_CreateHook(runGameLoopFuncAddr, (void*)Hook_RunGameLoop, (void**)&original_run_game_loop) != MH_OK ||
        MH_EnableHook(runGameLoopFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook RunGameLoop");
        MH_Uninitialize();
        return false;
    }

    // Hook RenderGame
    void* renderFuncAddr = (void*)0x404DD0;
    if (MH_CreateHook(renderFuncAddr, (void*)Hook_RenderGame, (void**)&original_render_game) != MH_OK ||
        MH_EnableHook(renderFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook RenderGame");
        MH_Uninitialize();
        return false;
    }

    // Hook game_rand
    void* gameRandFuncAddr = (void*)0x417A22;
    if (MH_CreateHook(gameRandFuncAddr, (void*)Hook_GameRand, (void**)&original_game_rand) != MH_OK ||
        MH_EnableHook(gameRandFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook game_rand");
        MH_Uninitialize();
        return false;
    }
    
    // Hook process_game_inputs for framestep input buffer control
    void* processInputsFuncAddr = (void*)0x4146d0;
    if (MH_CreateHook(processInputsFuncAddr, (void*)Hook_ProcessGameInputs, (void**)&original_process_game_inputs) != MH_OK ||
        MH_EnableHook(processInputsFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook process_game_inputs");
        MH_Uninitialize();
        return false;
    }
    
    // Hook characterSelect_state_manager for CSS debugging (main CSS function)
    void* cssHandlerFuncAddr = (void*)0x407000;
    if (MH_CreateHook(cssHandlerFuncAddr, (void*)Hook_CSS_Handler, (void**)&original_css_handler) != MH_OK ||
        MH_EnableHook(cssHandlerFuncAddr) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ERROR FM2K HOOK: Failed to hook CSS handler");
        MH_Uninitialize();
        return false;
    }
    
    
    // Apply boot-to-character-select patches directly
    ApplyBootToCharacterSelectPatches();
    
    // Apply character select mode patches
    ApplyCharacterSelectModePatches();
    
    // Disable cursor hiding for ImGui overlay
    DisableCursorHiding();
    
    // DO NOT disable input repeat delays - they're necessary for FM2K's architecture
    // DisableInputRepeatDelays(); // REMOVED - causes rapid fire desyncs
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SUCCESS FM2K HOOK: BSNES-level architecture installed successfully!");
    
    return true;
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Hooks shut down");
}

int __cdecl Hook_UpdateGameState() {
    // Environment check for offline mode
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
    bool dual_client_mode = (::player_index == 0 || ::player_index == 1);
    bool use_gekko = !is_true_offline && dual_client_mode;
    
    if (use_gekko && gekko_initialized && gekko_session) {
        // CRITICAL FIX: ALWAYS process GekkoNet for connection events, regardless of frame changes
        // STEP 1: Capture local input directly (CCCaster-style)
        uint16_t local_input = CaptureDirectInput();
        
        // STEP 2: Send to GekkoNet for synchronization
        gekko_add_local_input(gekko_session, player_index, &local_input);
        
        // STEP 3: ALWAYS process GekkoNet to handle connection events
        ProcessGekkoNetFrame();
        
        // STEP 4: Check synchronization status for both initialization and battle phases
        uint32_t* game_mode = (uint32_t*)0x470054;
        uint32_t current_frame_count = *(uint32_t*)FM2K::State::Memory::FRAME_COUNTER_ADDR;
        bool is_initialization_phase = (*game_mode < 3000);  // Before battle mode
        
        // CRITICAL FIX: Block game state updates, but NOT GekkoNet processing
        if (!gekko_session_started) {
            // STARTUP SYNCHRONIZATION: Block game updates until both clients connect
            static uint32_t startup_sync_counter = 0;
            if (++startup_sync_counter % 60 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                           "STARTUP SYNC: ProcessGekkoNetFrame running, blocking game updates - frame=%d mode=%d (#%d)", 
                           current_frame_count, *game_mode, startup_sync_counter);
            }
            // NOTE: We still ran ProcessGekkoNetFrame above, just blocking the original game function
            return 0; 
        }
        
        if (!can_advance_frame && gekko_frame_control_enabled) {
            // FRAME SYNCHRONIZATION: Block until GekkoNet sends AdvanceEvent
            const char* phase_name = is_initialization_phase ? "INIT" : "BATTLE";
            static uint32_t frame_sync_counter = 0;
            if (++frame_sync_counter % 120 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                           "%s SYNC: Waiting for AdvanceEvent - frame=%d mode=%d (#%d)", 
                           phase_name, current_frame_count, *game_mode, frame_sync_counter);
            }
            return 0; // Skip original function until AdvanceEvent
        }
        
        // INPUT INJECTION: Now handled via GetKeyboardState hook
        // This allows FM2K's natural input processing to work, including side-flipping logic
    } else {
        // OFFLINE MODE: FM2K reads inputs naturally from keyboard
        // No direct memory writes needed - GetKeyboardState hook is inactive in offline mode
    }
    
    // FRAME STEPPING: Block game state updates when paused
    SharedInputData* shared_data = GetSharedMemory();
    if (shared_data && frame_step_paused_global && shared_data->frame_step_is_paused) {
        return 0; // Block game state updates when truly paused
    }
    
    // BYPASS HOOK ENTIRELY during step frames - let original function run without interference
    if (shared_data && shared_data->frame_step_remaining_frames > 0 && shared_data->frame_step_remaining_frames != UINT32_MAX) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "UPDATE HOOK: BYPASSING hook during step frame %u", g_frame_counter);
        return original_update_game ? original_update_game() : 0;
    }
    
    // Normal per-frame logic
    MonitorGameStateTransitions();
    CheckForDebugCommands();
    CheckForHotkeys();
    CheckOverlayHotkey();
    ProcessManualSaveLoadRequests();
    
    // Call original UpdateGameState - game will read our pre-written inputs
    if (original_update_game) {
        return original_update_game();
    }
    return 0;
}

void __cdecl Hook_RenderGame() {
    SharedInputData* shared_data = GetSharedMemory();
    
    // FRAME STEPPING: Re-pause after a step has finished.
    // This is done in the render hook to ensure that the game state for the stepped frame
    // has been fully updated before the pause is re-engaged.
    if (shared_data) {
        if (!shared_data->frame_step_is_paused && shared_data->frame_step_remaining_frames == 0) {
            frame_step_paused_global = true;
            shared_data->frame_step_is_paused = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "RENDER HOOK: Step complete, PAUSING at frame %u", g_frame_counter);
        }
    }

    // SYNCHRONIZATION: Block rendering during both startup and frame sync as needed
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
    bool dual_client_mode = (::player_index == 0 || ::player_index == 1);
    bool use_gekko = !is_true_offline && dual_client_mode;
    
    if (use_gekko && gekko_initialized && gekko_session) {
        uint32_t* game_mode = (uint32_t*)0x470054;  // g_game_mode
        bool is_initialization_phase = (*game_mode < 3000);  // Before battle mode
        
        // STARTUP SYNCHRONIZATION: Block rendering until both clients connect
        if (!gekko_session_started) {
            static uint32_t startup_render_counter = 0;
            if (++startup_render_counter % 60 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                           "STARTUP RENDER BLOCK: Waiting for both clients to connect - mode=%d (#%d)", 
                           *game_mode, startup_render_counter);
            }
            return; // Skip rendering until session starts
        }
        
        // FRAME SYNCHRONIZATION: Block rendering when waiting for AdvanceEvent
        if (!can_advance_frame && gekko_frame_control_enabled) {
            const char* phase_name = is_initialization_phase ? "INIT" : "BATTLE";
            static uint32_t frame_render_counter = 0;
            if (++frame_render_counter % 120 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                           "%s RENDER BLOCK: Waiting for AdvanceEvent - mode=%d (#%d)", 
                           phase_name, *game_mode, frame_render_counter);
            }
            return; // Skip rendering until AdvanceEvent
        }
    }
    
    // We always render to give visual feedback, even when paused.
    if (original_render_game) {
        original_render_game();
    }
}

BOOL __cdecl Hook_RunGameLoop() {
    // Apply patches on startup
    ApplyCharacterSelectModePatches();
    
    // Always use original main loop - framestep is handled in other hooks
    return original_run_game_loop ? original_run_game_loop() : FALSE;
}


char __cdecl Hook_CSS_Handler() {
    // CSS DEBUG: Log when CSS handler is called and what input values it sees
    uint32_t* player_input_flags = (uint32_t*)0x4cfa04;  // g_combined_raw_input
    uint32_t* player_input_changes = (uint32_t*)0x447f60; // g_player_input_changes[8]
    uint32_t* game_mode = (uint32_t*)0x470054;  // g_game_mode
    
    static uint32_t css_call_counter = 0;
    css_call_counter++;
    
    // Always log CSS handler calls when there's potential button input
    bool has_button_input = (*player_input_flags & 0x3F0) || 
                           (player_input_changes[0] & 0x3F0) || 
                           (player_input_changes[1] & 0x3F0);
    
    if (css_call_counter <= 5 || has_button_input || css_call_counter % 300 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                   "CSS HANDLER #%d: game_mode=%d, input_flags=0x%03X, changes[0]=0x%03X, changes[1]=0x%03X", 
                   css_call_counter, *game_mode, *player_input_flags, 
                   player_input_changes[0], player_input_changes[1]);
    }
    
    // Call original CSS handler
    char result = 0;
    if (original_css_handler) {
        result = original_css_handler();
    }
    
    // Log result if there was button input
    if (has_button_input) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS HANDLER RESULT: %d", result);
    }
    
    return result;
}

int __cdecl Hook_ProcessGameInputs() {
    // Check for true offline mode
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);

    // FRAME STEPPING: This is the main control point since it's called repeatedly in the game loop
    // Get shared memory for frame stepping control
    SharedInputData* shared_data = GetSharedMemory();
    
    // Process debug commands (including save/load) BEFORE the pause check
    // This allows save/load to work even when the game is paused
    CheckForDebugCommands();
    CheckForHotkeys(); // Check for keyboard hotkeys for save/load and frame stepping
    ProcessManualSaveLoadRequests();
    
    // Check for frame stepping commands
    if (shared_data) {
        // ONE-TIME-FIX: Handle the initial state where memset sets remaining_frames to 0.
        // This state should mean "running indefinitely".
        static bool initial_state_fixed = false;
        if (!initial_state_fixed && !shared_data->frame_step_is_paused && shared_data->frame_step_remaining_frames == 0) {
            shared_data->frame_step_remaining_frames = UINT32_MAX;
            initial_state_fixed = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Corrected initial frame step state to RUNNING.");
        }

        // Handle frame stepping commands
        if (shared_data->frame_step_pause_requested) {
            frame_step_paused_global = true;
            shared_data->frame_step_is_paused = true;
            shared_data->frame_step_pause_requested = false;
            shared_data->frame_step_remaining_frames = 0; // No stepping, just pause
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame stepping PAUSED at frame %u", g_frame_counter);
        }
        if (shared_data->frame_step_resume_requested) {
            frame_step_paused_global = false;
            shared_data->frame_step_is_paused = false;
            shared_data->frame_step_resume_requested = false;
            shared_data->frame_step_remaining_frames = UINT32_MAX; // Use sentinel for "running"
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame stepping RESUMED at frame %u", g_frame_counter);
        }
        if (shared_data->frame_step_single_requested) {
            // Single step: run one frame then pause
            shared_data->frame_step_single_requested = false;
            frame_step_paused_global = false; // Allow one frame
            shared_data->frame_step_is_paused = false;
            shared_data->frame_step_remaining_frames = 1; // Allow exactly 1 frame
            shared_data->frame_step_needs_input_refresh = true; // Capture fresh inputs right before execution
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: SINGLE STEP ENABLED - allowing 1 frame at frame %u", g_frame_counter);
        }
        // Multi-step disabled - focus on single step only
        if (shared_data->frame_step_multi_count > 0) {
            shared_data->frame_step_multi_count = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Multi-step disabled - use single step instead");
        }
        
        // If paused, keep input system alive but preserve motion input buffer SURGICALLY
        if (frame_step_paused_global && shared_data->frame_step_is_paused) {
            // SURGICAL APPROACH: Keep input system alive but prevent frame counter advancement
            
            // Save critical state that original_process_inputs modifies
            uint32_t* frame_counter_ptr = (uint32_t*)0x447EE0;  // g_frame_counter
            uint32_t* p1_history_ptr = (uint32_t*)0x4280E0;     // g_player_input_history  
            uint32_t* p2_history_ptr = (uint32_t*)0x4284E0;     // g_p2_input_history (assuming offset)
            
            uint32_t saved_frame_counter = 0;
            uint32_t saved_p1_history = 0;
            uint32_t saved_p2_history = 0;
            
            if (!IsBadReadPtr(frame_counter_ptr, sizeof(uint32_t))) {
                saved_frame_counter = *frame_counter_ptr;
                
                // Save the input history entries that would be overwritten
                uint32_t next_frame_index = (saved_frame_counter + 1) & 0x3FF;
                if (!IsBadReadPtr(p1_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    saved_p1_history = p1_history_ptr[next_frame_index];
                }
                if (!IsBadReadPtr(p2_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    saved_p2_history = p2_history_ptr[next_frame_index];
                }
            }
            
            // Update input system to keep it alive - use game's own input mapping
            // This is what CaptureRealInputs() did in the working commit
            if (original_get_player_input) {
                if (is_true_offline) {
                    // TRUE OFFLINE: Read both players from local hardware using game's input mapping
                    original_get_player_input(0, 0);  // P1 input 
                    original_get_player_input(1, 0);  // P2 input
                } else {
                    // ONLINE: Read local controls using game's input mapping
                    original_get_player_input(0, 0);  // Local input
                }
            }
            
            if (original_process_game_inputs) {
                original_process_game_inputs();
            }
            
            // Restore the critical state to preserve motion inputs
            if (!IsBadWritePtr(frame_counter_ptr, sizeof(uint32_t))) {
                *frame_counter_ptr = saved_frame_counter;  // Undo frame counter advance
                
                // Restore the input history entries
                uint32_t next_frame_index = (saved_frame_counter + 1) & 0x3FF;
                if (!IsBadWritePtr(p1_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    p1_history_ptr[next_frame_index] = saved_p1_history;
                }
                if (!IsBadWritePtr(p2_history_ptr + next_frame_index, sizeof(uint32_t))) {
                    p2_history_ptr[next_frame_index] = saved_p2_history;
                }
            }
            
            return 0; // Block game advancement but keep inputs fresh
        }
    }
    
    // Normal input capture - use game's own input system
    if (!shared_data || !shared_data->frame_step_needs_input_refresh) {
        if (original_get_player_input) {
            if (is_true_offline) {
                original_get_player_input(0, 0);  // P1 input
                original_get_player_input(1, 0);  // P2 input  
            } else {
                original_get_player_input(0, 0);  // Local input
            }
        }
    }
    
    // FIXED: Increment frame counter BEFORE processing to fix 1-frame input delay
    g_frame_counter++;
    
    // RADICAL SOLUTION: Call original_process_inputs TWICE on step frames to eliminate delay
    if (shared_data && shared_data->frame_step_needs_input_refresh) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: DOUBLE CALL to eliminate 1-frame delay at frame %u", g_frame_counter);
        
        // Capture fresh inputs using game's input mapping
        if (original_get_player_input) {
            if (is_true_offline) {
                original_get_player_input(0, 0);  // P1 input
                original_get_player_input(1, 0);  // P2 input
            } else {
                original_get_player_input(0, 0);  // Local input
            }
        }
        
        // First call: This "primes" the input system with current inputs
        if (original_process_game_inputs) {
            original_process_game_inputs();
        }
        
        // Second call: This ensures the inputs are processed for THIS frame, not next
        if (original_process_game_inputs) {
            original_process_game_inputs();
        }
        
        shared_data->frame_step_needs_input_refresh = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Double call complete - inputs should be immediate");
    } else {
        // Normal single call
        if (original_process_game_inputs) {
            original_process_game_inputs();
        }
    }
    
    // Handle frame stepping countdown. Re-pausing is now in the render hook.
    if (shared_data && shared_data->frame_step_remaining_frames > 0 && shared_data->frame_step_remaining_frames != UINT32_MAX) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Frame ADVANCED to %u during stepping", g_frame_counter);
        shared_data->frame_step_remaining_frames--;
        if (shared_data->frame_step_remaining_frames == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "INPUT HOOK: Step processing complete for frame %u, will pause in render hook", g_frame_counter);
        }
    }
    
    // CRITICAL: Early return to prevent double frame execution
    return 0;
}