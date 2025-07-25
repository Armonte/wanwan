#include "debug_features.h"
#include "globals.h"
#include "logging.h"
#include "shared_mem.h"
#include "object_analysis.h"
#include <windows.h>
#include <SDL3/SDL.h>

// Check for debug commands from launcher via shared memory
void CheckForDebugCommands() {
    SharedInputData* shared_data = GetSharedMemory();
    if (!shared_data) {
        return; // No shared memory available
    }
    
    // Check for slot-based save state request (launcher uses these fields)
    if (shared_data->debug_save_to_slot_requested && !manual_save_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher requested save to slot %u", shared_data->debug_target_slot);
        manual_save_requested = true;
        shared_data->debug_save_to_slot_requested = false;
    }
    
    // Check for slot-based load state request
    if (shared_data->debug_load_from_slot_requested && !manual_load_requested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher requested load from slot %u", shared_data->debug_target_slot);
        manual_load_requested = true;
        shared_data->debug_load_from_slot_requested = false;
    }
    
    // Process force rollback requests
    if (shared_data->debug_rollback_frames > 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher requested rollback of %u frames", 
                   shared_data->debug_rollback_frames);
        // TODO: Implement force rollback through GekkoNet
        shared_data->debug_rollback_frames = 0;
    }
    
    // Frame stepping is now handled in Hook_ProcessGameInputs()
    
    // Update enhanced action data for launcher (reduced frequency for performance)
    static uint32_t last_action_update_frame = 0;
    if (g_frame_counter - last_action_update_frame >= 60) {  // Update every 60 frames (0.6 seconds)
        last_action_update_frame = g_frame_counter;
        // Reduced logging frequency
        if (g_frame_counter % 300 == 0) {  // Log every 3 seconds
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "HOOK: Updating enhanced action data at frame %u", g_frame_counter);
        }
        UpdateEnhancedActionData();
    }
}

// Keyboard hotkey handler for save states and frame stepping
void CheckForHotkeys() {
    SharedInputData* shared_data = GetSharedMemory();
    if (!shared_data) {
        return; // No shared memory available
    }
    
    static bool keys_pressed[256] = {false}; // Track key press states to avoid repeats
    
    // Check for save state hotkeys: Shift+1-8
    bool shift_pressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (shift_pressed) {
        for (int i = 0; i < 8; i++) {
            int vk_key = '1' + i; // VK codes for 1-8
            bool key_currently_pressed = (GetAsyncKeyState(vk_key) & 0x8000) != 0;
            
            if (key_currently_pressed && !keys_pressed[vk_key]) {
                // Save to slot i
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey: Save to slot %d", i);
                if (!manual_save_requested) {
                    manual_save_requested = true;
                    target_save_slot = i;
                }
            }
            keys_pressed[vk_key] = key_currently_pressed;
        }
    }
    
    // Check for load state hotkeys: 1-8 (without Shift)
    if (!shift_pressed) {
        for (int i = 0; i < 8; i++) {
            int vk_key = '1' + i; // VK codes for 1-8
            bool key_currently_pressed = (GetAsyncKeyState(vk_key) & 0x8000) != 0;
            
            if (key_currently_pressed && !keys_pressed[vk_key]) {
                // Load from slot i
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey: Load from slot %d", i);
                if (!manual_load_requested) {
                    manual_load_requested = true;
                    target_load_slot = i;
                }
            }
            keys_pressed[vk_key] = key_currently_pressed;
        }
    }
    
    // Check for pause/resume hotkey: 0
    bool key_0_pressed = (GetAsyncKeyState('0') & 0x8000) != 0;
    if (key_0_pressed && !keys_pressed['0']) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey: Toggle pause/resume");
        if (shared_data->frame_step_is_paused) {
            shared_data->frame_step_resume_requested = true;
        } else {
            shared_data->frame_step_pause_requested = true;
        }
    }
    keys_pressed['0'] = key_0_pressed;
    
    // Check for single step hotkeys: - and +/=
    bool key_minus_pressed = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0; // - key
    bool key_plus_pressed = (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0;   // +/= key
    
    if (key_minus_pressed && !keys_pressed[VK_OEM_MINUS]) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey: Single step advance");
        shared_data->frame_step_single_requested = true;
    }
    keys_pressed[VK_OEM_MINUS] = key_minus_pressed;
    
    if (key_plus_pressed && !keys_pressed[VK_OEM_PLUS]) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey: Single step advance");  
        shared_data->frame_step_single_requested = true;
    }
    keys_pressed[VK_OEM_PLUS] = key_plus_pressed;
    
    // Check for F5 key to toggle hitjudge flag
    bool key_f5_pressed = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (key_f5_pressed && !keys_pressed[VK_F5]) {
        // Toggle hitjudge flag at 0x42470C
        uint8_t* hitjudge_flag = (uint8_t*)0x42470C;
        uint8_t current_value = *hitjudge_flag;
        uint8_t new_value = current_value ? 0 : 1;
        *hitjudge_flag = new_value;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hotkey F5: Toggled hitjudge flag from %d to %d", current_value, new_value);
    }
    keys_pressed[VK_F5] = key_f5_pressed;
}
