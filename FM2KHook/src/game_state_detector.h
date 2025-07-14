#pragma once

#include "common.h"

namespace FM2K {
    enum class GameState : uint32_t {
        BOOT_SPLASH,
        TITLE_SCREEN,
        MAIN_MENU,
        CHARACTER_SELECT,
        INTRO_LOADING,
        IN_GAME,
        TRANSITION,
        UNKNOWN
    };

    enum class ObjectFunctionIndex : uint32_t {
        NULLSUB_1 = 0,
        RESET_SPRITE_EFFECT = 1,
        GAME_INITIALIZE = 2,
        CAMERA_MANAGER = 3,
        CHARACTER_STATE_MACHINE = 4,
        UPDATE_SCREEN_FADE = 5,
        SCORE_DISPLAY_SYSTEM = 6,
        DISPLAY_SCORE = 7,
        UPDATE_TRANSITION_EFFECT = 8,
        INITIALIZE_SCREEN_TRANSITION = 9,
        GAME_STATE_MANAGER = 10,
        INITIALIZE_SCREEN_TRANSITION_ALT = 11,
        HANDLE_MAIN_MENU_AND_CHARACTER_SELECT = 12,
        UPDATE_MAIN_MENU = 13,
        VS_ROUND_FUNCTION = 14,
        UI_STATE_MANAGER = 15,
        MAX_FUNCTION_INDEX = 32
    };

    struct ActiveFunctionAnalysis {
        uint32_t function_counts[32] = {0};
        bool has_character_state_machine = false;
        bool has_main_menu = false;
        bool has_character_select = false;
        bool has_intro_sequence = false;
        bool has_transition_effects = false;
        uint32_t total_objects = 0;
    };

    struct GameStateContext {
        uint32_t game_mode = 0;
        bool timer_running = false;
        bool in_combat = false;
    };

    struct ObjectChangeTracker {
        uint32_t current_active_mask[32] = {0};
        uint32_t previous_active_mask[32] = {0};
        uint32_t created_objects[32] = {0};
        uint32_t destroyed_objects[32] = {0};
        uint32_t stable_objects[32] = {0};
        uint32_t frames_since_last_change = 0;
        bool objects_stable = false;
        uint32_t frame_count = 0;
        uint32_t creation_rate = 0;
        uint32_t destruction_rate = 0;
        uint32_t stable_character_objects = 0;
        uint32_t volatile_character_objects = 0;
    };

    GameState DetectGameStateAdvanced(const ActiveFunctionAnalysis& functions, const GameStateContext& context, const ObjectChangeTracker& tracker);
    bool AnalyzeActiveObjectFunctions(ActiveFunctionAnalysis* analysis);
    void UpdateObjectChangeTracking(ObjectChangeTracker* tracker, const uint32_t* current_mask, uint16_t active_count);
    void AnalyzeCharacterObjectStability(ObjectChangeTracker* tracker, const ActiveFunctionAnalysis& functions);
    bool IsActiveCombat(const GameStateContext& context, const ObjectChangeTracker& tracker);
} 