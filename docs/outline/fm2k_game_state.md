# FM2K Game State & Object Management

## Overview

FM2K uses a sophisticated object management system with a fixed pool of 1024 game objects, comprehensive player state tracking, and deterministic game state management. This system is exceptionally well-designed for rollback netcode implementation.

## Object Pool Architecture (1024 Objects)

### Core Object System
**Location**: `g_object_pool` (0x4701E0)
**Size**: 1024 objects × 382 bytes = ~390KB

#### Object Structure
```c
struct GameObject {
    uint32_t type;              // +0x000: Object type ID
    uint32_t list_type;         // +0x004: Object list category
    uint32_t x_position;        // +0x008: X position (fixed point)
    uint32_t y_position;        // +0x00C: Y position (fixed point)
    uint32_t state_flags;       // +0x010: Object state flags
    uint32_t velocity_x;        // +0x014: X velocity
    uint32_t velocity_y;        // +0x018: Y velocity
    uint32_t animation_frame;   // +0x01C: Current animation frame
    uint32_t animation_timer;   // +0x020: Animation timing
    uint32_t health_points;     // +0x024: Object HP (if applicable)
    
    // ... 352 more bytes of object-specific data
    
    uint32_t parent_ptr;        // +0x17A: Back-pointer to parent object
    uint16_t active_flag;       // +0x17E: 0=inactive, 1=active
};

#define MAX_OBJECTS 1023
#define OBJECT_SIZE 382
```

### Object Management Variables
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_object_count` | 0x4246FC | Current number of active objects |
| `g_max_objects` | 0x4259A4 | Maximum objects (1023) |
| `g_current_object_ptr` | 0x4259A8 | Current object being processed |
| `g_object_data_ptr` | 0x4CFA00 | Pointer to current object data |

### Object List Management
```c
struct ObjectManager {
    uint32_t object_count;      // 0x4246FC: Current active objects
    uint32_t max_objects;       // 0x4259A4: Maximum objects (1023)
    void*    list_heads[512];   // 0x430240: Object list head pointers
    void*    list_tails[512];   // 0x430244: Object list tail pointers
};
```

The object system organizes objects into **512 different lists** based on:
- Object type (characters, projectiles, effects, etc.)
- Processing priority
- Render order
- Collision groups

### ⭐ **Critical Object Functions**
| Function | Address | Purpose |
|----------|---------|---------|
| `init_game_objects` | 0x415200 | Initialize object system |
| `create_game_object` | 0x406570 | Create new object |
| `update_game_object` | 0x40C130 | Update individual objects |
| `finalize_game_objects` | 0x40FFC0 | Cleanup objects |

## Player State Variables

### Player Health System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_p1_hp` | 0x4DFC85 | Player 1 current HP |
| `g_p2_hp` | 0x4EDCC4 | Player 2 current HP |
| `g_p1_max_hp` | 0x4DFC91 | Player 1 maximum HP |
| `g_p2_max_hp` | 0x4EDCD0 | Player 2 maximum HP |

### Player Position System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_p1_stage_x` | 0x424E68 | Player 1 stage X coordinate |
| `g_p1_stage_y` | 0x424E6C | Player 1 stage Y coordinate |
| `g_stage_width` | 0x4452B8 | Stage width in tiles |
| `g_stage_height` | 0x4452BA | Stage height in tiles |
| `g_player_stage_positions` | 0x470020 | Player positions on stage grid |

### Player Action System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_p1_round_count` | 0x4700EC | Player 1 round count |
| `g_p1_round_state` | 0x4700F0 | Player 1 round state |
| `g_p1_action_state` | 0x47019C | Player 1 action state |
| `g_p2_action_state` | 0x4701A0 | Player 2 action state |
| `g_player_move_history` | 0x47006C | Player move history |
| `g_player_action_history` | 0x47011C | Player action history |

### Complete Player State Structure
```c
struct PlayerState {
    // Health System
    uint32_t current_hp;        // Current health points
    uint32_t maximum_hp;        // Maximum health points
    
    // Position System
    uint32_t stage_x;           // X coordinate on stage
    uint32_t stage_y;           // Y coordinate on stage
    uint32_t facing_direction;  // Character facing direction
    
    // Combat State
    uint32_t action_state;      // Current action/move
    uint32_t animation_frame;   // Current animation frame
    uint32_t combo_count;       // Current combo counter
    uint32_t hit_stun_timer;    // Hit stun remaining frames
    
    // Round Management
    uint32_t round_count;       // Rounds won
    uint32_t round_state;       // Current round state
    
    // Move History (for special moves)
    uint32_t move_history[16];  // Recent move inputs
    uint32_t action_history[8]; // Recent actions performed
};
```

## Game State Management

### Core Game State Variables
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_game_mode` | 0x470054 | Current game mode (story/vs/etc) |
| `g_game_mode_flag` | 0x470058 | Game mode flags |
| `g_game_state_flag` | 0x4DFC6D | Game state flag |
| `g_player_side` | 0x424F20 | Player side selection |

### Timing System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_game_timer` | 0x470044 | Main game timer |
| `g_round_timer` | 0x470060 | Round timer value |
| `g_round_timer_counter` | 0x424F00 | Round timer counter |
| `g_timer_countdown1` | 0x4456E4 | Primary timer countdown |
| `g_timer_countdown2` | 0x447D91 | Secondary timer countdown |

### Round System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_round_setting` | 0x470068 | Round configuration |
| `g_team_round_setting` | 0x470064 | Team round setting |
| `g_default_round` | 0x430124 | Default round configuration |
| `g_team_round` | 0x430128 | Team round configuration |
| `g_round_esi` | 0x4EDCAC | Round state variable |

### Screen/Camera System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_screen_x` | 0x447F2C | Screen/camera X position |
| `g_screen_y` | 0x447F30 | Screen/camera Y position (= 480) |
| `g_screen_offset_x` | 0x4452B0 | Screen X offset |
| `g_screen_offset_y` | 0x4452B2 | Screen Y offset |
| `g_screen_scale_x` | 0x4452B4 | Screen X scaling factor |
| `g_screen_scale_y` | 0x4452B6 | Screen Y scaling factor |

## Random Number System

### ⭐ **CRITICAL for Rollback Implementation**
| Component | Address | Purpose |
|-----------|---------|---------|
| `g_random_seed` | 0x41FB1C | RNG seed storage |
| `game_rand` | 0x417A22 | Random number function |

#### RNG System Details
```c
// Random number generation
uint32_t g_random_seed;  // Global seed at 0x41FB1C

uint32_t game_rand() {
    // Linear congruential generator
    g_random_seed = (g_random_seed * 1103515245 + 12345) & 0x7FFFFFFF;
    return g_random_seed;
}
```

**Rollback Requirement**: The RNG seed must be preserved in all game state snapshots to ensure deterministic behavior during rollback operations.

## Animation & Character System

### Character Animation Data
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_character_animation_data` | 0x4D9324 | Character animation data |
| `g_character_sprite_data` | 0x4D1E90 | Character sprite data |

### Animation State Management
Each character object contains:
- **Animation frame**: Current frame in animation sequence
- **Animation timer**: Timing for frame transitions  
- **Animation state**: Current animation being played
- **Frame data**: Hitboxes, hurtboxes, movement data per frame

## Hit Detection & Combat System

### Hit Effect Management
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_hit_effect_timer` | 0x4701C8 | Hit effect duration |
| `g_hit_effect_target` | 0x4701C4 | Hit effect target player |

### Combat State Variables
```c
struct CombatState {
    uint32_t hit_stun_timer;     // Frames of hit stun remaining
    uint32_t block_stun_timer;   // Frames of block stun remaining
    uint32_t invincibility_timer; // Invincibility frames
    uint32_t combo_counter;      // Current combo count
    uint32_t damage_scaling;     // Damage scaling factor
    uint32_t meter_value;        // Super meter value
};
```

## Configuration System

### Game Configuration
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_config_value1-7` | 0x4300E0-0x430120 | Various game configuration values |
| `g_hit_judge_value` | 0x42470C | Hit detection configuration |
| `g_window_x` | 0x425A48 | Window X position |
| `g_window_config` | 0x43022C | Window configuration |
| `g_display_config` | 0x4D1D60 | Display configuration |

### Configuration Loading
The game loads extensive configuration from INI files via `hit_judge_set_function` (0x414930), including:
- Hit detection parameters
- Round settings and timers
- Character-specific data
- Display and audio settings

## State Serialization for Rollback

### Complete Game State Structure
```c
struct FM2K_GameState {
    // Object Pool State (~390KB)
    struct {
        uint32_t object_count;
        GameObject objects[1023];
        uint32_t list_heads[512];
        uint32_t list_tails[512];
    } object_system;
    
    // Player State (~200 bytes)
    PlayerState players[2];
    
    // Game State (~100 bytes)
    struct {
        uint32_t game_mode;
        uint32_t game_mode_flag;
        uint32_t game_state_flag;
        uint32_t game_timer;
        uint32_t round_timer;
        uint32_t round_timer_counter;
        uint32_t random_seed;        // CRITICAL
        int32_t screen_x, screen_y;
        uint16_t screen_offsets[4];
    } game_state;
    
    // Combat State (~50 bytes)
    struct {
        uint32_t hit_effect_timer;
        uint32_t hit_effect_target;
        CombatState combat_states[2];
    } combat_system;
    
    // Configuration State (~50 bytes)
    struct {
        uint32_t config_values[7];
        uint32_t hit_judge_value;
        uint32_t display_config;
    } configuration;
};
```

**Total State Size**: ~400KB per frame

### State Management Functions

#### Optimized State Save
```c
void save_game_state_optimized(uint32_t frame) {
    FM2K_GameState* state = get_state_buffer(frame);
    
    // Object system - use dirty bit optimization
    state->object_system.object_count = g_object_count;
    for (uint32_t i = 0; i < g_object_count; i++) {
        if (g_object_pool[i].dirty_flag) {
            memcpy(&state->object_system.objects[i], &g_object_pool[i], OBJECT_SIZE);
            g_object_pool[i].dirty_flag = 0;
        }
    }
    
    // Player state
    save_player_state(&state->players[0], 0);
    save_player_state(&state->players[1], 1);
    
    // Game state
    state->game_state.random_seed = g_random_seed;  // CRITICAL
    state->game_state.game_timer = g_game_timer;
    state->game_state.round_timer = g_round_timer;
    state->game_state.screen_x = g_screen_x;
    state->game_state.screen_y = g_screen_y;
    
    // Combat state
    state->combat_system.hit_effect_timer = g_hit_effect_timer;
    state->combat_system.hit_effect_target = g_hit_effect_target;
}
```

#### Fast State Restore
```c
void restore_game_state(uint32_t frame) {
    FM2K_GameState* state = get_state_buffer(frame);
    
    // Restore object system
    g_object_count = state->object_system.object_count;
    memcpy(g_object_pool, state->object_system.objects, 
           g_object_count * OBJECT_SIZE);
    
    // Restore player state
    restore_player_state(&state->players[0], 0);
    restore_player_state(&state->players[1], 1);
    
    // Restore game state
    g_random_seed = state->game_state.random_seed;  // CRITICAL
    g_game_timer = state->game_state.game_timer;
    g_round_timer = state->game_state.round_timer;
    g_screen_x = state->game_state.screen_x;
    g_screen_y = state->game_state.screen_y;
    
    // Restore combat state
    g_hit_effect_timer = state->combat_system.hit_effect_timer;
    g_hit_effect_target = state->combat_system.hit_effect_target;
}
```

## Object System Optimization

### Dirty Bit System
```c
// Mark objects as modified for optimized saving
void mark_object_dirty(uint32_t object_id) {
    if (object_id < MAX_OBJECTS) {
        g_object_pool[object_id].dirty_flag = 1;
        g_dirty_object_count++;
    }
}

// Only save changed objects
bool has_dirty_objects() {
    return g_dirty_object_count > 0;
}
```

### Object Pool Management
```c
// Object allocation from pool
GameObject* allocate_object() {
    for (uint32_t i = 0; i < MAX_OBJECTS; i++) {
        if (!g_object_pool[i].active_flag) {
            g_object_pool[i].active_flag = 1;
            g_object_count++;
            mark_object_dirty(i);
            return &g_object_pool[i];
        }
    }
    return NULL; // Pool full
}

// Object deallocation
void deallocate_object(uint32_t object_id) {
    if (object_id < MAX_OBJECTS && g_object_pool[object_id].active_flag) {
        g_object_pool[object_id].active_flag = 0;
        g_object_count--;
        mark_object_dirty(object_id);
    }
}
```

## Performance Considerations

### Memory Usage Analysis
- **Object Pool**: 390KB (largest state component)
- **Player State**: ~200 bytes per player
- **Game State**: ~100 bytes of critical variables
- **Total per frame**: ~400KB

### Optimization Strategies
1. **Dirty Bit Tracking**: Only save modified objects
2. **Delta Compression**: Store differences between frames
3. **Object Pooling**: Reuse allocated objects
4. **Batch Operations**: Group state operations for efficiency

## Rollback Integration Points

### State Modification Tracking
All functions that modify game state should be hooked:
- **Object creation/destruction**
- **Position/velocity updates**
- **Health/timer modifications**
- **Animation state changes**

### Critical State Preservation
Priority order for state preservation during rollback:

1. **RNG Seed** (0x41FB1C) - HIGHEST PRIORITY
   - Must be preserved exactly for deterministic behavior
   - Single point of failure for game determinism

2. **Object Pool** (0x4701E0) - HIGH PRIORITY  
   - Contains all game entities and their states
   - Largest component but essential for gameplay

3. **Player State** - HIGH PRIORITY
   - Health, position, action states
   - Direct impact on gameplay and visuals

4. **Game Timers** - MEDIUM PRIORITY
   - Round timers, game timers
   - Affects game flow and win conditions

5. **Camera Position** - LOW PRIORITY
   - Screen positioning
   - Visual consistency but not gameplay critical

---

**Status**: ✅ Complete analysis of game state and object systems
**State Size**: ~400KB per frame (manageable for rollback)
**Implementation Ready**: All critical variables identified and documented

*The FM2K object management system provides excellent foundation for rollback implementation with its fixed pool allocation and deterministic state variables.*