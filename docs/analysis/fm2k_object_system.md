# FM2K Object System Analysis

## Overview

FM2K uses a fixed-size object pool system for managing all game entities (characters, projectiles, effects, etc.). This document details the object structure and management system based on IDA Pro analysis.

## Object Pool Structure

### Memory Layout
- **Base Address**: `0x4701E0` (`g_game_object_pool`)
- **Total Objects**: 1024
- **Object Size**: 382 bytes (0x17E)
- **Total Pool Size**: 391,168 bytes (0x5F800)

### Object Structure (382 bytes)
```
Offset  | Size | Field Description
--------|------|------------------------------------------
0x000   | 4    | Object Type/ID (0 = inactive/empty)
0x004   | 4    | Owner/Category ID
0x008   | 4    | Unknown (likely position X)
0x00C   | 4    | Unknown (likely position Y)
...     | ...  | Game-specific data
0x17A   | 4    | Linked list pointer (set by engine)
0x17E   | -    | End of object
```

### Key Fields
- **Object Type (offset 0x0)**: Primary indicator of active/inactive state
  - Value of 0 means the object slot is empty/inactive
  - Non-zero values indicate active objects with specific types
- **Owner ID (offset 0x4)**: Links objects to players or systems
- **Linked List Pointer (offset 0x17A)**: Used by engine for object iteration

## Object Management System

### Linked List Tracking
FM2K uses linked lists to efficiently iterate active objects:
- **`g_object_list_heads`** (0x430240): Array of list head pointers by category
- **`g_object_list_tails`** (0x430244): Array of list tail pointers by category

### Object Lifecycle

1. **Creation** (`create_game_object` at 0x406574):
   - Searches for first object with type == 0
   - Calls `memset(object, 0, 0x17C)` to clear
   - Sets type, owner, and initial data
   - Returns pointer to initialized object

2. **Update** (`update_game_state` at 0x404CD0):
   - Rebuilds linked lists each frame
   - Iterates all 1024 objects
   - Adds objects with type != 0 to appropriate linked lists
   - Calls `update_game_object` on each active object

3. **Deactivation**:
   - Sets object type (offset 0x0) to 0
   - Object automatically excluded from next frame's linked lists

## Empty vs Active Detection

### Correct Detection Method
```cpp
bool IsObjectActive(uint8_t* object_ptr) {
    uint32_t object_type = *(uint32_t*)object_ptr;
    return object_type != 0;
}
```

### Why 0xFF Detection Failed
- Uninitialized memory is filled with 0xFF
- But FM2K clears objects with `memset(object, 0, 0x17C)`
- Empty objects have all fields set to 0, not 0xFF
- Only see 0xFF pattern before game initialization

## Typical Object Counts

Based on game analysis:
- **Menu/Idle**: 0-5 active objects
- **Character Select**: 5-15 active objects  
- **During Match**: 10-50 active objects (players, projectiles, effects)
- **Complex Scenes**: Up to 100-200 active objects

## Performance Considerations

### Scanning Optimization
Instead of checking all 1024 objects:
1. Use linked lists when available
2. Early exit when finding empty objects in sequence
3. Cache active object indices between frames

### Memory Access Patterns
- Objects are 382 bytes (not cache-aligned)
- Sequential scanning can be slow
- Consider checking just first DWORD for activity

## Implementation Recommendations

### For Rollback Netcode
1. **Fast Path**: Save only confirmed active objects (type != 0)
2. **Fallback**: If >100 active objects detected, assume initialization issue
3. **Optimization**: Track object activity between frames for differential saves

### Buffer Sizing
- **Minimum**: 20 objects × 384 bytes = ~7.5KB
- **Typical**: 50 objects × 384 bytes = ~19KB  
- **Maximum**: 200 objects × 384 bytes = ~75KB
- **Full Pool**: 1024 objects × 384 bytes = 384KB

## Related Functions

| Function | Address | Purpose |
|----------|---------|---------|
| `update_game_state` | 0x404CD0 | Main object update loop |
| `create_game_object` | 0x406574 | Object allocation |
| `update_game_object` | 0x40C130 | Individual object update |
| `DeactivateObjectsByOwner` | 0x4064F4 | Bulk object cleanup |
| `init_game_objects` | Unknown | Called at frame start |
| `finalize_game_objects` | Unknown | Called at frame end |

## Comparison with Giuroll

Giuroll (Touhou 12.3) uses a different approach:
- Tracks dynamic heap allocations
- Hooks HeapAlloc/HeapFree to monitor memory
- More complex but handles variable memory layouts

FM2K's fixed pool is simpler but requires proper empty detection.

## Game State Transitions and Object Pool Management

### Critical Implementation Need: Game State Tracking

For proper rollback netcode implementation, we need to track when the game transitions between different states, as the object pool behavior changes significantly:

#### State Transition Flow
1. **Main Menu** → **Character Select** → **In-Game** → **Results** → **Back to Menu**

#### Object Pool Behavior by Game State

| Game State | Object Count | Object Types | Save Strategy |
|------------|--------------|--------------|---------------|
| **Main Menu** | 0-2 | UI elements only | Core-only save (32 bytes) |
| **Character Select** | 5-15 | UI, preview models | Light object save (~5KB) |
| **Loading/Transition** | Variable | Cleanup + initialization | Fallback to core-only |
| **In-Game (Active)** | 10-50 | Players, projectiles, effects | Full object save (10-50KB) |
| **Pause Menu** | 10-50 | Game objects frozen | Full object save |
| **Results Screen** | 5-20 | UI + cleanup | Light object save |

#### Implementation Requirements

**1. Game State Detection**
We need to implement game state tracking to optimize save state strategy:
```cpp
enum class GameState {
    MAIN_MENU,
    CHARACTER_SELECT, 
    LOADING,
    IN_GAME,
    PAUSED,
    RESULTS,
    UNKNOWN
};

GameState DetectCurrentGameState() {
    // Check game mode variables, screen state, etc.
    // Use addresses from savestate_memory_map.md
}
```

**2. Adaptive Save Strategy**
```cpp
bool SaveStateFast(GameState current_state) {
    switch (current_state) {
        case GameState::MAIN_MENU:
        case GameState::LOADING:
            return SaveCoreOnly();  // 32 bytes
            
        case GameState::CHARACTER_SELECT:
        case GameState::RESULTS:
            return SaveLightObjects();  // Core + UI objects
            
        case GameState::IN_GAME:
        case GameState::PAUSED:
            return SaveFullObjects();  // Core + all active objects
            
        default:
            return SaveCoreOnly();  // Safe fallback
    }
}
```

**3. Transition Handling**
During state transitions, object pools may be in inconsistent states:
- **Entering match**: Objects being created/initialized
- **Exiting match**: Objects being cleaned up
- **Loading screens**: Mixed state during transition

**4. Memory Addresses for State Detection**
From `savestate_memory_map.md`, we can use:
- `g_game_mode` (0x470054): Current game mode
- `g_game_state_flag` (0xDFC6D): Central game state flag
- Player slot status to detect if characters are loaded

**5. Future Implementation Plan**
1. Add game state detection functions
2. Hook state transition points (character select, match start, etc.)
3. Implement adaptive save strategy based on current state
4. Add state transition logging for debugging
5. Optimize object detection per game state

This approach will prevent issues like:
- Trying to save 1024 "active" objects during menu (all 0xFF)
- Saving unnecessary data during transitions
- Buffer overflows during inconsistent states
- Poor performance during menu navigation

**Priority**: High - This is essential for production rollback stability.

## ✅ IMPLEMENTED: Adaptive Save Strategy (December 2024)

### Object Function Table Analysis

The implementation now includes intelligent game state detection using the object function table at `0x41ED58`. This enables automatic save strategy selection:

#### Function-Based Game State Detection (IDA Verified)
```cpp
// Complete object function table @ 0x41ED58 (verified with IDA MCP)
enum class ObjectFunctionIndex : uint32_t {
    NULLSUB_1 = 0,                              // 0x406990 - nullsub_1
    RESET_SPRITE_EFFECT = 1,                    // 0x4069A0 - ResetSpriteEffect
    GAME_INITIALIZE = 2,                        // 0x409A60 - Game_Initialize  
    CAMERA_MANAGER = 3,                         // 0x40AF30 - camera_manager
    CHARACTER_STATE_MACHINE = 4,                // 0x411BF0 - character_state_machine_ScriptMainLoop
    UPDATE_SCREEN_FADE = 5,                     // 0x40AC60 - UpdateScreenFade
    SCORE_DISPLAY_SYSTEM = 6,                   // 0x40A620 - score_display_system
    DISPLAY_SCORE = 7,                          // 0x40AB10 - DisplayScore
    UPDATE_TRANSITION_EFFECT = 8,               // 0x406CF0 - UpdateTransitionEffect
    INITIALIZE_SCREEN_TRANSITION = 9,           // 0x406D90 - InitializeScreenTransition
    GAME_STATE_MANAGER = 10,                    // 0x406FC0 - game_state_manager
    INITIALIZE_SCREEN_TRANSITION_ALT = 11,      // 0x406E50 - InitializeScreenTransition_Alt
    HANDLE_MAIN_MENU_AND_CHARACTER_SELECT = 12, // 0x408080 - handle_main_menu_and_character_select
    UPDATE_MAIN_MENU = 13,                      // 0x4084F0 - UpdateMainMenu
    VS_ROUND_FUNCTION = 14,                     // 0x4086A0 - vs_round_function
    UI_STATE_MANAGER = 15,                      // 0x409D00 - ui_state_manager
};

enum class GameState : uint32_t {
    BOOT_SPLASH,        // 1-3 objects, minimal functions
    TITLE_SCREEN,       // title_screen_manager active
    MAIN_MENU,          // update_main_menu active
    CHARACTER_SELECT,   // update_post_char_select active
    INTRO_LOADING,      // update_intro_sequence active
    IN_GAME,            // character_state_machine active
    TRANSITION,         // transition effects active
    UNKNOWN
};
```

#### Adaptive Save Strategy Implementation
```cpp
switch (current_game_state) {
    case GameState::IN_GAME:
        use_full_objects = true;        // Full object save (~10-50KB)
        save_strategy = "full-objects";
        break;
    case GameState::CHARACTER_SELECT:
        use_full_objects = (total_objects <= 100);  // Light object save (~5KB)
        save_strategy = use_full_objects ? "light-objects" : "core-only";
        break;
    case GameState::BOOT_SPLASH:
    case GameState::TITLE_SCREEN:
    case GameState::MAIN_MENU:
    case GameState::TRANSITION:
    case GameState::INTRO_LOADING:
    default:
        use_full_objects = false;       // Core-only save (~150 bytes)
        save_strategy = "core-only";
        break;
}
```

#### Performance Benefits
- **Menu Navigation**: 150 bytes vs 850KB (99.98% reduction)
- **Character Select**: ~5KB vs 850KB (99.4% reduction)  
- **In-Game**: ~10-50KB vs 850KB (94-98% reduction)
- **Automatic Fallback**: Falls back to core-only if object detection fails

#### Implementation Status
✅ **Object function analysis** - `AnalyzeActiveObjectFunctions()`  
✅ **Game state detection** - `DetectGameStateFromFunctions()`  
✅ **Adaptive save strategy** - Integrated into `SaveStateFast()`  
✅ **Automatic fallback** - Core-only save when detection fails  
✅ **Performance logging** - Real-time save strategy and timing metrics

#### IDA-Verified Function Categories

| Function Index | Function Name | Category | Game State Indicator |
|----------------|---------------|----------|---------------------|
| 0 | nullsub_1 | Null/Empty | - |
| 1 | ResetSpriteEffect | Visual Effects | Transition |
| 2 | Game_Initialize | Initialization | Loading/Intro |
| 3 | camera_manager | Gameplay Systems | In-Game |
| 4 | character_state_machine_ScriptMainLoop | **Core Gameplay** | **In-Game** |
| 5 | UpdateScreenFade | Visual Effects | Transition |
| 6 | score_display_system | UI/Display | Results/In-Game |
| 7 | DisplayScore | UI/Display | Results |
| 8 | UpdateTransitionEffect | Visual Effects | Transition |
| 9 | InitializeScreenTransition | Visual Effects | Transition |
| 10 | game_state_manager | Core Systems | General |
| 11 | InitializeScreenTransition_Alt | Visual Effects | Transition |
| 12 | handle_main_menu_and_character_select | **UI Navigation** | **Menu/Select** |
| 13 | UpdateMainMenu | UI Navigation | Main Menu |
| 14 | vs_round_function | **Gameplay Systems** | **In-Game** |
| 15 | ui_state_manager | UI Systems | General |

#### Key Insights from Analysis
- **Primary In-Game Indicators**: `CHARACTER_STATE_MACHINE` (4), `VS_ROUND_FUNCTION` (14)
- **Menu/Navigation Indicators**: `HANDLE_MAIN_MENU_AND_CHARACTER_SELECT` (12), `UPDATE_MAIN_MENU` (13)  
- **Transition Indicators**: Multiple transition/fade functions (1, 5, 8, 9, 11)
- **Loading/Init Indicators**: `GAME_INITIALIZE` (2)

This approach eliminates the performance issues caused by always saving the full 850KB state, while maintaining full rollback capability when actually needed during gameplay.