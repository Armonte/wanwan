#include "game_state_detector.h"
#include "globals.h"

namespace FM2K {

    GameState DetectGameStateAdvanced(const ActiveFunctionAnalysis& functions, const GameStateContext& context, const ObjectChangeTracker& tracker) {
        if (context.game_mode >= 3000) {
            if (functions.has_character_state_machine && IsActiveCombat(context, tracker)) {
                return GameState::IN_GAME;
            }
        } else if (context.game_mode >= 2000) {
            if (functions.has_character_state_machine && !IsActiveCombat(context, tracker)) {
                return GameState::CHARACTER_SELECT;
            }
        } else if (context.game_mode >= 1000) {
            if (functions.has_main_menu || functions.has_character_select) {
                return GameState::TITLE_SCREEN;
            }
            if (functions.total_objects <= 5) {
                return GameState::BOOT_SPLASH;
            }
            return GameState::MAIN_MENU;
        }
        
        if (functions.has_character_state_machine && IsActiveCombat(context, tracker)) {
            return GameState::IN_GAME;
        }
        
        if (functions.has_intro_sequence) {
            return GameState::INTRO_LOADING;  
        }
        
        if (tracker.creation_rate > 10 || tracker.destruction_rate > 10 || functions.has_transition_effects) {
            return GameState::TRANSITION;  
        }
        
        if (functions.total_objects <= 5) {
            return GameState::BOOT_SPLASH;  
        }
        
        return GameState::UNKNOWN;
    }

    bool AnalyzeActiveObjectFunctions(ActiveFunctionAnalysis* analysis) {
        if (!analysis) return false;
        memset(analysis, 0, sizeof(ActiveFunctionAnalysis));
        uint8_t* object_pool = (uint8_t*)FM2K::State::Memory::GAME_OBJECT_POOL_ADDR;
        if (IsBadReadPtr(object_pool, FM2K::State::Memory::GAME_OBJECT_POOL_SIZE)) return false;

        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t* object_header = (uint32_t*)(object_pool + (i * 382));
            if (!IsBadReadPtr(object_header, 4)) {
                uint32_t object_type = *object_header;
                if (object_type != 0 && object_type != 0xFFFFFFFF) {
                    analysis->total_objects++;
                    if (object_type < 32) {
                        analysis->function_counts[object_type]++;
                        
                        // Check for specific game state indicators (using verified function indices)
                        switch (static_cast<ObjectFunctionIndex>(object_type)) {
                            case ObjectFunctionIndex::CHARACTER_STATE_MACHINE:
                                analysis->has_character_state_machine = true;
                                break;
                            case ObjectFunctionIndex::HANDLE_MAIN_MENU_AND_CHARACTER_SELECT:
                                analysis->has_main_menu = true;
                                analysis->has_character_select = true;  // This function handles both
                                break;
                            case ObjectFunctionIndex::UPDATE_MAIN_MENU:
                                analysis->has_main_menu = true;
                                break;
                            case ObjectFunctionIndex::GAME_INITIALIZE:
                                analysis->has_intro_sequence = true;
                                break;
                            case ObjectFunctionIndex::VS_ROUND_FUNCTION:
                                analysis->has_character_state_machine = true;  // VS rounds = in-game
                                break;
                            case ObjectFunctionIndex::UI_STATE_MANAGER:
                                // UI state manager could be menus or in-game UI
                                break;
                            case ObjectFunctionIndex::CAMERA_MANAGER:
                                // Camera manager typically active during gameplay
                                break;
                            case ObjectFunctionIndex::RESET_SPRITE_EFFECT:
                            case ObjectFunctionIndex::UPDATE_TRANSITION_EFFECT:
                            case ObjectFunctionIndex::INITIALIZE_SCREEN_TRANSITION:
                            case ObjectFunctionIndex::INITIALIZE_SCREEN_TRANSITION_ALT:
                            case ObjectFunctionIndex::UPDATE_SCREEN_FADE:
                                analysis->has_transition_effects = true;
                                break;
                            case ObjectFunctionIndex::SCORE_DISPLAY_SYSTEM:
                            case ObjectFunctionIndex::DISPLAY_SCORE:
                                // Score display typically during/after matches
                                break;
                            case ObjectFunctionIndex::GAME_STATE_MANAGER:
                                // General game state management
                                break;
                        }
                    }
                }
            }
        }
        return true;
    }

    void UpdateObjectChangeTracking(ObjectChangeTracker* tracker, const uint32_t* current_mask, uint16_t active_count) {
        if (!tracker || !current_mask) return;
        
        tracker->frame_count++;
        
        // Copy previous to current
        memcpy(tracker->previous_active_mask, tracker->current_active_mask, sizeof(tracker->current_active_mask));
        memcpy(tracker->current_active_mask, current_mask, sizeof(tracker->current_active_mask));
        
        // Calculate created and destroyed objects
        bool any_changes = false;
        for (int i = 0; i < 32; i++) {
            // Created: current & !previous
            tracker->created_objects[i] = tracker->current_active_mask[i] & ~tracker->previous_active_mask[i];
            
            // Destroyed: previous & !current
            tracker->destroyed_objects[i] = tracker->previous_active_mask[i] & ~tracker->current_active_mask[i];
            
            // Stable: current & previous
            tracker->stable_objects[i] = tracker->current_active_mask[i] & tracker->previous_active_mask[i];
            
            if (tracker->created_objects[i] || tracker->destroyed_objects[i]) {
                any_changes = true;
            }
        }
        
        // Update stability tracking
        if (any_changes) {
            tracker->frames_since_last_change = 0;
            tracker->objects_stable = false;
        } else {
            tracker->frames_since_last_change++;
            tracker->objects_stable = (tracker->frames_since_last_change >= 60);
        }
        
        // Calculate creation/destruction rates (objects per second, assuming 60 FPS)
        if (tracker->frame_count > 0) {
            uint32_t created_count = 0, destroyed_count = 0;
            for (int i = 0; i < 32; i++) {
                created_count += __builtin_popcount(tracker->created_objects[i]);
                destroyed_count += __builtin_popcount(tracker->destroyed_objects[i]);
            }
            
            // Simple moving average over last 60 frames
            float time_window = std::min(tracker->frame_count, 60u) / 60.0f;  // seconds
            tracker->creation_rate = static_cast<uint32_t>(created_count / time_window);
            tracker->destruction_rate = static_cast<uint32_t>(destroyed_count / time_window);
        }
    }

    void AnalyzeCharacterObjectStability(ObjectChangeTracker* tracker, const ActiveFunctionAnalysis& functions) {
        if (!tracker) return;
        
        tracker->stable_character_objects = 0;
        tracker->volatile_character_objects = 0;
        
        // Count CHARACTER_STATE_MACHINE objects by stability
        uint32_t char_state_type = static_cast<uint32_t>(ObjectFunctionIndex::CHARACTER_STATE_MACHINE);
        
        uint8_t* object_pool = (uint8_t*)FM2K::State::Memory::GAME_OBJECT_POOL_ADDR;
        if (IsBadReadPtr(object_pool, FM2K::State::Memory::GAME_OBJECT_POOL_SIZE)) return;
        
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t* object_header = (uint32_t*)(object_pool + (i * 382));
            if (!IsBadReadPtr(object_header, 4)) {
                uint32_t object_type = *object_header;
                
                if (object_type == char_state_type) {
                    uint32_t mask_index = i >> 5;
                    uint32_t bit_index = i & 31;
                    uint32_t bit_mask = 1U << bit_index;
                    
                    // Check if this character object is stable
                    if (tracker->stable_objects[mask_index] & bit_mask) {
                        tracker->stable_character_objects++;
                    } else if (tracker->current_active_mask[mask_index] & bit_mask) {
                        tracker->volatile_character_objects++;
                    }
                }
            }
        }
    }

    bool IsActiveCombat(const GameStateContext& context, const ObjectChangeTracker& tracker) {
        bool game_mode_combat = (context.game_mode >= 3000);
        bool timer_active = context.timer_running;
        bool health_changing = context.in_combat;
        bool objects_volatile = (tracker.creation_rate > 5 || tracker.destruction_rate > 5);
        bool characters_active = (tracker.volatile_character_objects > 0);
        bool objects_unstable = !tracker.objects_stable;
        return (game_mode_combat || (timer_active && (health_changing || objects_volatile)) || (characters_active && objects_unstable));
    }
} 