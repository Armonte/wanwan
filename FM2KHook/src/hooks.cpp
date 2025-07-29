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
#include <cstdlib>
#include <cstring>

// Original function pointers

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
    
    // Normal per-frame logic
    MonitorGameStateTransitions();
    CheckForDebugCommands();
    CheckForHotkeys();
    CheckOverlayHotkey();
    ProcessManualSaveLoadRequests();
    
    // FRAMESTEP LOGIC: Handle pause/resume/single step functionality
    ProcessFrameStepLogic();
    
    // FRAMESTEP BLOCKING: Check if we should block game execution
    SharedInputData* shared_data = GetSharedMemory();
    if (shared_data && shared_data->frame_step_is_paused) {
        if (shared_data->frame_step_remaining_frames <= 0) {
            // PAUSED: Block game execution but continue processing our hooks
            // This maintains input buffer continuity for motion inputs and dashes
            static uint32_t pause_log_counter = 0;
            if (++pause_log_counter % 300 == 0) {  // Log every 3 seconds
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FRAMESTEP: Game execution blocked (paused)");
            }
            return 0; // Block original UpdateGameState
        } else {
            // STEPPING: Allow this frame and decrement counter
            shared_data->frame_step_remaining_frames--;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FRAMESTEP: Executing frame (%u remaining)", 
                       shared_data->frame_step_remaining_frames);
        }
    }
    
    // Call original UpdateGameState - game will read our pre-written inputs
    if (original_update_game) {
        return original_update_game();
    }
    return 0;
}

void __cdecl Hook_RenderGame() {
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
    
    if (original_render_game) {
        original_render_game();
    }
}

BOOL __cdecl Hook_RunGameLoop() {
    // STRATEGY CHANGE: Use original main loop but control through hooks
    // The original loop has sophisticated timing with timeGetTime() and frame skip logic
    // We let it run naturally and control frame processing through our hooks
    
    ApplyCharacterSelectModePatches();
    
    // Always use original main loop - our hooks will control the frame processing
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