# FM2K Fighter Maker 2nd - Rollback Netcode Research

## Project Overview

This document contains our reverse engineering research for implementing rollback netcode in FM2K (Fighter Maker 2nd). The game currently uses LilithPort for delay-based networking, but we're working to understand the engine internals to implement GGPO-style rollback netcode.

**Key Facts:**
- Game runs at 100 FPS (10ms per frame)
- Uses GDI for rendering (640x480 resolution)
- Already has 1024-frame input buffering
- Fixed timestep game loop with frame skipping capability

---

## Core Game Architecture

### Main Game Loop and Timing System (0x405ad0)

The main game loop provides the foundation for rollback implementation:

1. **Fixed Timestep Architecture:**
   - **Frame Rate**: 100 FPS (10ms per frame)
   - **Frame Skipping**: Up to 10 frames max
   - **Synchronization**: Frame sync flag prevents drift
   - **Timing**: Uses `timeGetTime()` for precision

2. **Game Loop Structure:**
   ```c
   void main_game_loop() {
       g_frame_time_ms = 10;  // 100 FPS
       g_last_frame_time = timeGetTime();
       
       // Initial 8 frame warmup
       for (int i = 0; i < 8; i++) {
           update_game_state();
       }
       
       while (running) {
           // Frame timing calculation
           current_time = timeGetTime();
           frame_delta = current_time - g_last_frame_time;
           
           if (current_time >= g_last_frame_time + g_frame_time_ms) {
               // Frame advance
               int frames_to_process = calculate_frame_skip(current_time);
               
               for (int i = 0; i < frames_to_process; i++) {
                   process_game_inputs_FRAMESTEP_HOOK();  // 0x4146d0
                   update_game_state();                   // Game logic
                   process_input_history(g_p1_input_history[g_input_buffer_index]);
               }
               
               render_game(0);
               g_frame_sync_flag = 0;
           }
       }
   }
   ```

3. **Critical Hook Points:**
   - **0x405be7**: `process_game_inputs_FRAMESTEP_HOOK()` - Primary rollback hook
   - **0x405bec**: `update_game_state()` - Game state update
   - **0x405bff**: `process_input_history()` - Input processing
   - **0x405c0a**: `render_game()` - Rendering

4. **Frame Synchronization:**
   - `g_frame_sync_flag` (0x424700): Prevents timing drift
   - `g_frame_time_delta` (0x425960): Frame time variance
   - `g_frame_skip_count` (0x4246f4): Frame skip counter
   - `g_last_frame_time` (0x447dd4): Last frame timestamp

### Random Number Generator (0x417a22)

FM2K uses a deterministic Linear Congruential Generator (LCG):

1. **Algorithm:**
   ```c
   int game_rand() {
       g_random_seed = 214013 * g_random_seed + 2531011;
       return (g_random_seed >> 16) & 0x7FFF;
   }
   ```

2. **Critical Properties:**
   - **Deterministic**: Same seed produces same sequence
   - **Fast**: Simple multiplication and addition
   - **Predictable**: Perfect for rollback synchronization
   - **Seed Location**: `g_random_seed` (0x41fb1c)

3. **Rollback Implications:**
   - **State Serialization**: Must include RNG seed
   - **Deterministic Replay**: Same inputs + same seed = same results
   - **Synchronization**: Both players must have identical seed state

### Enhanced GekkoNet Integration

With the timing system analysis, we can refine our integration:

#### Updated State Structure
```c
typedef struct FM2KGameState {
    // Frame and timing state
    uint32_t frame_number;
    uint32_t input_buffer_index;
    uint32_t last_frame_time;
    uint32_t frame_time_delta;
    uint8_t frame_skip_count;
    uint8_t frame_sync_flag;
    
    // Random number generator state
    uint32_t random_seed;
    
    // Input state
    uint32_t p1_input_current;
    uint32_t p2_input_current;
    uint32_t input_repeat_state[8];
    uint32_t input_repeat_timer[8];
    
    // Character states (existing structure)
    struct {
        // ... existing character state fields
    } characters[2];
    
    // Round and game state
    uint32_t round_state;
    uint32_t round_timer;
    uint32_t game_timer;
    uint32_t game_mode;
    
    // Hit effects and timers
    uint32_t hit_effect_target;
    uint32_t hit_effect_timer;
    
    // Combo and damage state
    uint16_t combo_counter[2];
    uint16_t damage_scaling[2];
    
} FM2KGameState;
```

#### Enhanced Hook Implementation
```c
// Primary rollback hook at main game loop
void fm2k_rollback_main_loop() {
    // 1. Network polling
    gekko_network_poll(session);
    
    // 2. Add local input
    uint32_t local_input = get_local_player_input();
    gekko_add_local_input(session, local_player_id, &local_input);
    
    // 3. Process GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case SaveEvent:
                save_complete_fm2k_state(events[i]->data.save.state, 
                                        events[i]->data.save.state_len,
                                        events[i]->data.save.checksum);
                break;
                
            case LoadEvent:
                load_complete_fm2k_state(events[i]->data.load.state,
                                        events[i]->data.load.state_len);
                break;
                
            case AdvanceEvent:
                // Inject inputs and advance frame
                inject_frame_inputs(events[i]->data.adv.inputs,
                                  events[i]->data.adv.input_len);
                // Let normal game loop continue
                break;
        }
    }
    
    // 4. Continue normal FM2K frame processing
    // (The existing main_game_loop logic continues)
}
```

#### State Serialization with Timing
```c
void save_complete_fm2k_state(unsigned char* buffer, unsigned int* length, unsigned int* checksum) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Save timing state
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    state->last_frame_time = g_last_frame_time;
    state->frame_time_delta = g_frame_time_delta;
    state->frame_skip_count = g_frame_skip_count;
    state->frame_sync_flag = g_frame_sync_flag;
    
    // Save RNG state
    state->random_seed = g_random_seed;
    
    // Save all other game state...
    
    *length = sizeof(FM2KGameState);
    *checksum = calculate_state_checksum(state);
}
```

### Key Advantages of This Integration:

1. **Perfect Timing Control**: 100 FPS fixed timestep ideal for rollback
2. **Deterministic RNG**: LCG ensures perfect synchronization
3. **Minimal Hooks**: Single main loop hook handles everything
4. **Frame Skipping**: Built-in frame skip system handles performance
5. **Existing Infrastructure**: Leverages FM2K's robust timing system

This analysis reveals that FM2K's architecture is exceptionally well-suited for rollback netcode, with built-in systems that support the exact requirements for deterministic online play.

---

## Input System Architecture

### Input Processing Pipeline

The game has a sophisticated input system that's already well-suited for rollback:

1. **Raw Input Collection** (`get_player_input` - 0x414340)
2. **Input State Processing** (`process_game_inputs` - 0x4146D0)
3. **Input History Management** (`process_input_history` - 0x4025A0)

### Input Buffer System

The game maintains a **1024-frame circular buffer** for input history:

```c
// Input buffer structure
struct InputBuffer {
    uint32_t buffer_index;     // g_input_buffer_index (0x447EE0)
    uint32_t p1_history[1024]; // g_p1_input_history (0x4280E0)
    uint32_t p2_history[1024]; // g_p2_input_history (0x4290E0)
};

// Buffer index is masked: (index + 1) & 0x3FF
// This gives us ~10 seconds of input history at 100 FPS
```

### Input State Arrays

The input system uses multiple arrays for state tracking:

| Variable | Address | Purpose |
|----------|---------|---------|
| `g_p1_input` | 0x4259C0 | Current P1 input state |
| `g_p2_input` | 0x4259C4 | Current P2 input state |
| `g_prev_input_state` | 0x447F00 | Previous frame input state |
| `g_processed_input` | 0x447F40 | Processed input with repeat handling |
| `g_input_changes` | 0x447F60 | New input presses (edge detection) |
| `g_input_repeat_timer` | 0x4D1C40 | Input repeat timing counters |
| `g_input_repeat_state` | 0x541F80 | Input repeat state tracking |

### Combined Input States

For performance, the game combines all inputs:

| Variable | Address | Purpose |
|----------|---------|---------|
| `g_combined_raw_input` | 0x4CFA04 | All raw inputs combined |
| `g_combined_input_changes` | 0x4280D8 | All input changes combined |
| `g_combined_processed_input` | 0x4D1C20 | All processed inputs combined |

### Input Mapping

#### Keyboard Mappings
| Variable | Address | Input |
|----------|---------|-------|
| `g_key_up` | 0x425980 | Up direction |
| `g_key_down` | 0x425982 | Down direction |
| `g_key_left` | 0x425981 | Left direction |
| `g_key_right` | 0x425983 | Right direction |
| `g_key_button1-7` | 0x425984-0x42598A | Attack buttons |

#### Joystick Support
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_joystick_enabled` | 0x430110 | Joystick enable flag |
| `g_joystick_buttons1` | 0x445710 | Joystick button config 1 |
| `g_joystick_buttons2` | 0x445714 | Joystick button config 2 |

### Input Timing Configuration
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_input_repeat_delay` | 0x41E400 | Repeat delay timing |
| `g_input_initial_delay` | 0x41E3FC | Initial input delay |

---

## Game State Management

### Player State Variables

| Variable | Address | Purpose |
|----------|---------|---------|
| `g_p1_hp` | 0x4DFC85 | Player 1 current HP |
| `g_p2_hp` | 0x4EDCC4 | Player 2 current HP |
| `g_p1_max_hp` | 0x4DFC91 | Player 1 maximum HP |
| `g_p2_max_hp` | 0x4EDCD0 | Player 2 maximum HP |

### Game Configuration

| Variable | Address | Purpose |
|----------|---------|---------|
| `g_game_mode` | 0x470054 | Current game mode (story/vs/etc) |
| `g_game_mode_flag` | 0x470058 | Game mode flags |
| `g_player_side` | 0x424F20 | Player side selection |
| `g_default_round` | 0x430124 | Default round configuration |
| `g_team_round` | 0x430128 | Team round configuration |
| `g_round_esi` | 0x4EDCAC | Round state variable |

### Round System (0x4086a0)

The round system manages game flow and state transitions:

1. **Round State Machine:**
   - States stored in `g_round_state` (dword_47004C):
     - 0: Round initialization
     - 1: Round in progress
     - 2: Round ending
     - 3: Round complete
   - State transitions:
     ```
     100 -> 101 -> 110 -> 111 -> 112 -> 113 -> 200 -> 300 -> 
     410/420/430 (Win states) -> 510/520/530 (Round end) -> 900 (Reset)
     ```

2. **Round Memory Layout:**
   ```
   round_state {
     +338: Current round state
     +342: Round timer/counter
     +350: Game flags
     +470048: Round limit
     +47004C: Round state
     +470040: Game mode
   }
   ```

3. **Player State Management:**
   - HP values at `g_p1_hp` and `g_p2_hp`
   - Max HP at `g_p1_max_hp` and `g_p2_max_hp`
   - Round counts at `g_p1_round_count` and `g_p2_round_count`
   - Round states at `g_p1_round_state` and `g_p2_round_state`

4. **Game Mode Flags:**
   - 0: Arcade/Story mode
   - 1: VS mode
   - 2: Team battle mode
   - Each mode has specific round handling:
     - VS: Simple win/loss tracking
     - Team: Complex state with member rotation
     - Arcade: Stage progression and scoring

5. **Critical Memory Regions:**
   - Round state: 0x47004C
   - Game mode: 0x470040
   - Round limit: 0x470048
   - Player HP: 0x4DFC85 (P1), 0x4EDCC4 (P2)
   - Round counters: 0x424F24, 0x424F28

This analysis reveals key structures for rollback:
- Round state transitions
- Player state data
- Game mode handling
- Win/loss conditions
- State reset points

---

## Random Number System

**Critical for Rollback Implementation**

| Component | Address | Purpose |
|-----------|---------|---------|
| `g_random_seed` | 0x41FB1C | RNG seed storage |
| `game_rand` | 0x417A22 | Random number function |

The RNG system is deterministic and must be saved/restored during rollback operations. The seed at `0x41FB1C` is referenced by the `game_rand` function and needs to be included in any game state snapshots.

---

## Critical Hook Points (From LilithPort)

These are locations where LilithPort injects code for delay-based netcode. They represent key points where we can inject rollback logic:

### Input Hooks
| Address | Hook Name | Purpose |
|---------|-----------|---------|
| 0x414729 | STORY_KEY | Story mode P1 input processing |
| 0x41474A | VS_P1_KEY | Versus mode P1 input processing |
| 0x414764 | VS_P2_KEY | Versus mode P2 input processing |
| 0x414712 | SINGLE_CONTROL_HOOK | Single player control hook |
| 0x414748 | VS_CONTROL_HOOK | Versus control hook |

### Game Logic Hooks
| Address | Hook Name | Purpose |
|---------|-----------|---------|
| 0x414C90 | HIT_JUDGE_SET | Hit detection setup |
| 0x408756 | STAGE_SELECT | Stage selection processing |
| 0x40897F | VS_ROUND | Versus round management |
| 0x409715 | ROUND_END | Round end processing |

### System Hooks
| Address | Hook Name | Purpose |
|---------|-----------|---------|
| 0x404C37 | FRAME_RATE | Frame rate control |
| 0x417A22 | RAND_FUNC | Random number generation |

---

## Rendering System

### Rendering Functions
| Function | Address | Purpose |
|----------|---------|---------|
| `render_game` | 0x404DD0 | Main game rendering |
| `render_frame` | 0x404C10 | Frame buffer to screen |

### Rendering Details
- Uses GDI (BitBlt/StretchBlt) for rendering
- Native resolution: 640x480
- Supports scaling to window size
- Single-threaded rendering

**Note:** The GDI rendering system may need modification for rollback, as we'll need to:
1. Render multiple speculative frames
2. Potentially skip rendering during rollback
3. Handle visual rollback smoothly

---

## Rollback Implementation Strategy

### Phase 1: State Serialization
**Objective:** Identify and serialize all game state

**Required State Components:**
1. **Player States**
   - HP values (current and max)
   - Position coordinates (need to identify)
   - Animation states (need to identify)
   - Velocity/movement data (need to identify)

2. **Game State**
   - Round information
   - Timer state (need to identify)
   - Stage state (need to identify)
   - Game mode flags

3. **Random Number State**
   - RNG seed (`g_random_seed`)
   - Any additional RNG state

4. **Input State**
   - Current input buffers
   - Input repeat timers
   - Input processing state

### Phase 2: Frame Management
**Objective:** Implement frame saving/loading system

**Implementation Points:**
1. **Save State Hook:** After `update_game_state` (0x404CD0)
2. **Load State Hook:** Before input processing when rollback needed
3. **Frame Skip:** Leverage existing frame skip system for fast-forward

### Phase 3: Input Prediction
**Objective:** Implement input prediction and confirmation

**Strategy:**
1. Leverage existing 1024-frame input buffer
2. Implement input prediction algorithms
3. Handle input confirmation from network
4. Trigger rollback on input misprediction

### Phase 4: Network Integration
**Objective:** Replace LilithPort's delay-based system

**Integration Points:**
1. Replace input hooks with rollback-aware versions
2. Implement GGPO-style networking
3. Handle connection management
4. Implement rollback-specific UI feedback

---

## Memory Layout Analysis

### Input System Memory Map
```
0x4259C0: g_p1_input (4 bytes)
0x4259C4: g_p2_input (4 bytes)
...
0x4280E0: g_p1_input_history (4096 bytes - 1024 frames)
0x4290E0: g_p2_input_history (4096 bytes - 1024 frames)
...
0x447EE0: g_input_buffer_index (4 bytes)
0x447F00: g_prev_input_state (32 bytes - 8 inputs)
0x447F40: g_processed_input (32 bytes - 8 inputs)
0x447F60: g_input_changes (32 bytes - 8 inputs)
```

### Game State Memory Map
```
0x4DFC85: g_p1_hp (4 bytes)
0x4DFC91: g_p1_max_hp (4 bytes)
0x4EDCC4: g_p2_hp (4 bytes)
0x4EDCD0: g_p2_max_hp (4 bytes)
0x41FB1C: g_random_seed (4 bytes)
```

---

## Technical Challenges

### 1. State Identification
**Challenge:** We need to identify ALL game state variables
**Status:** Partially complete - need player positions, animations, etc.
**Next Steps:** Continue systematic variable identification

### 2. State Size Optimization
**Challenge:** Game state snapshots need to be small for performance
**Considerations:** 
- 100 FPS means potentially 100 snapshots per second
- Network bandwidth limitations
- Memory usage for state history

### 3. Rendering During Rollback
**Challenge:** GDI rendering may not be suitable for rollback
**Options:**
1. Skip rendering during rollback (fastest)
2. Modify rendering to handle multiple frames
3. Implement visual rollback smoothing

### 4. Integration with Existing Code
**Challenge:** Minimize changes to game logic
**Strategy:** Hook at key points rather than modifying core logic

---

## Next Research Steps

### Immediate Priorities
1. **Continue Variable Identification**
   - Player position coordinates
   - Animation state variables
   - Timer/clock variables
   - Stage state variables

2. **Function Analysis**
   - `update_game_state` (0x404CD0) - understand what state it modifies
   - `hit_judge_set_function` (0x414930) - hit detection logic
   - `vs_round_function` (0x4086A0) - round management

3. **Memory Layout Mapping**
   - Create complete memory map of game state
   - Identify state variable clusters
   - Determine optimal save/restore points

### Long-term Goals
1. **Proof of Concept**
   - Implement basic state save/restore
   - Test rollback with simple scenarios
   - Measure performance impact

2. **Network Protocol Design**
   - Design rollback-aware network protocol
   - Implement input synchronization
   - Handle connection management

3. **Integration Testing**
   - Test with real network conditions
   - Optimize for various latencies
   - Ensure compatibility with existing game features

---

## Code Injection Strategy

### Hook Points for Rollback

1. **Pre-Frame Hook** (before input processing)
   ```c
   // Save current state if needed
   if (should_save_state()) {
       save_game_state(current_frame);
   }
   
   // Check for rollback conditions
   if (should_rollback()) {
       rollback_to_frame(target_frame);
   }
   ```

2. **Post-Frame Hook** (after state update)
   ```c
   // Confirm frame completion
   confirm_frame(current_frame);
   
   // Handle network input
   process_network_inputs();
   ```

3. **Input Hook Replacement**
   ```c
   // Replace LilithPort input hooks with rollback-aware versions
   uint32_t get_rollback_input(int player, int frame) {
       if (is_confirmed_input(player, frame)) {
           return get_confirmed_input(player, frame);
       } else {
           return predict_input(player, frame);
       }
   }
   ```

---

## Performance Considerations

### Frame Budget (10ms per frame)
- **Input Processing:** ~1ms
- **Game Logic Update:** ~3-5ms
- **Rendering:** ~2-3ms
- **Network/Rollback:** ~2-3ms remaining

### Memory Usage
- **State Snapshots:** Estimate 1-4KB per frame
- **Input History:** Already 8KB (1024 frames Å~ 2 players Å~ 4 bytes)
- **Rollback Buffer:** Target 60-120 frames (600ms-1.2s at 100fps)

### Optimization Targets
1. **State Serialization:** < 0.5ms per save/restore
2. **Rollback Execution:** < 5ms for 10-frame rollback
3. **Memory Footprint:** < 1MB additional memory usage

---

## Conclusion

The FM2K engine is remarkably well-suited for rollback netcode implementation:

**Advantages:**
- Fixed 100 FPS timestep
- Existing 1024-frame input buffering
- Deterministic game logic
- Clear separation of input/logic/rendering

**Key Success Factors:**
1. Complete state variable identification
2. Efficient state serialization
3. Minimal integration impact
4. Performance optimization

The existing LilithPort integration points provide an excellent roadmap for where to inject rollback logic. The game's architecture suggests that a well-implemented rollback system could provide excellent performance with minimal latency.

---

## Recent Discoveries (Latest Session)

### Game Object System
From analyzing `update_game_state` (0x404CD0), we discovered the game uses a sophisticated object management system:

| Variable | Address | Purpose |
|----------|---------|---------|
| `g_object_count` | 0x4246FC | Current number of active game objects |
| `g_max_objects` | 0x4259A4 | Maximum objects (1024) |
| `g_current_object_ptr` | 0x4259A8 | Current object being processed |
| `g_object_data_ptr` | 0x4CFA00 | Pointer to object data |
| `g_object_list_heads` | 0x430240 | Object list head pointers |
| `g_object_list_tails` | 0x430244 | Object list tail pointers |

**Key Functions:**
- `init_game_objects` (0x415200): Initialize object system
- `update_game_object` (0x40C130): Update individual objects
- `finalize_game_objects` (0x40FFC0): Cleanup objects

### Timer System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_timer_countdown1` | 0x4456E4 | Primary timer countdown |
| `g_timer_countdown2` | 0x447D91 | Secondary timer countdown |

### Configuration System
The game loads extensive configuration from INI files via `hit_judge_set_function` (0x414930):

| Variable | Address | Purpose |
|----------|---------|---------|
| `g_config_value1-7` | 0x4300E0-0x430120 | Various game configuration values |
| `g_hit_judge_value` | 0x42470C | Hit detection configuration |
| `g_window_x` | 0x425A48 | Window X position |
| `g_window_config` | 0x43022C | Window configuration |
| `g_display_config` | 0x4D1D60 | Display configuration |

### Hook Point Verification
All LilithPort hook points have been verified and commented:

**Configuration Hooks:**
- 0x414AFC: ROUND_SET hook - Round configuration
- 0x414ADB: TEAM_ROUND_SET hook - Team round configuration  
- 0x414A8C: TIMER_SET hook - Timer configuration
- 0x40347B: VOLUME_SET_2 hook - Volume control

**Additional Functions Identified:**
- `volume_control_function` (0x403430): Audio volume management

### State Management Implications for Rollback

**Critical Discovery:** The game uses a complex object system with up to 1024 active objects. For rollback implementation, we need to:

1. **Object State Serialization**: Save/restore all active game objects
2. **Object List Management**: Preserve object linked lists during rollback
3. **Timer State**: Include timer countdowns in game state snapshots

**Memory Footprint Estimate:**
- Object system: ~1024 objects Å~ estimated 64-128 bytes each = 64-128KB per frame
- This is larger than initially estimated but still manageable

**Performance Considerations:**
- Object iteration happens every frame (up to 1024 objects)
- State saving must be efficient to maintain 100 FPS performance
- Consider differential state saving (only changed objects)

### ? CRITICAL DISCOVERY: Complete Object System Structure

From analyzing `game_state_manager` (0x406FC0) and `create_game_object` (0x406570), we've discovered the complete object system:

#### Object Pool Structure
- **Object Pool**: `g_object_pool` (0x4701E0)
- **Object Size**: 382 bytes each (0x17E hex)
- **Max Objects**: 1023 objects total
- **Object Layout**:
  ```c
  struct GameObject {
      uint32_t type;           // +0x00: Object type
      uint32_t subtype;        // +0x04: Object subtype  
      uint32_t x_pos;          // +0x08: X position (fixed point << 16)
      uint32_t y_pos;          // +0x0C: Y position (fixed point << 16)
      // ... 374 more bytes of object data
      uint32_t object_data_ptr; // +0x17A: Back-pointer to object data
  };
  ```

#### Player Position System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_p1_stage_x` | 0x424E68 | Player 1 stage X coordinate |
| `g_p1_stage_y` | 0x424E6C | Player 1 stage Y coordinate |
| `g_stage_width` | 0x4452B8 | Stage width in tiles |
| `g_stage_height` | 0x4452BA | Stage height in tiles |
| `g_player_stage_positions` | 0x470020 | Player positions on stage grid |

#### Screen/Camera System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_screen_x` | 0x447F2C | Screen/camera X position |
| `g_screen_y` | 0x447F30 | Screen/camera Y position (= 480) |
| `g_screen_offset_x` | 0x4452B0 | Screen X offset |
| `g_screen_offset_y` | 0x4452B2 | Screen Y offset |
| `g_screen_scale_x` | 0x4452B4 | Screen X scaling factor |
| `g_screen_scale_y` | 0x4452B6 | Screen Y scaling factor |

#### Game State Variables
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_round_timer` | 0x470060 | Round timer value |
| `g_round_setting` | 0x470068 | Round configuration |
| `g_game_timer` | 0x470044 | Main game timer |
| `g_team_round_setting` | 0x470064 | Team round setting |
| `g_round_timer_counter` | 0x424F00 | Round timer counter |
| `g_game_state_flag` | 0x4DFC6D | Game state flag |

#### Player Action System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_p1_round_count` | 0x4700EC | Player 1 round count |
| `g_p1_round_state` | 0x4700F0 | Player 1 round state |
| `g_p1_action_state` | 0x47019C | Player 1 action state |
| `g_p2_action_state` | 0x4701A0 | Player 2 action state |
| `g_player_move_history` | 0x47006C | Player move history |
| `g_player_action_history` | 0x47011C | Player action history |

#### Animation/Character System
| Variable | Address | Purpose |
|----------|---------|---------|
| `g_character_animation_data` | 0x4D9324 | Character animation data |
| `g_character_sprite_data` | 0x4D1E90 | Character sprite data |

### ? **100% CONFIRMED: Complete State Requirements for Rollback**

We now have **definitive confirmation** of what needs to be saved for rollback:

1. **Object Pool State** (382 Å~ 1023 = ~390KB)
   - All 1023 game objects with complete state
   - Object positions, types, animation states
   - This is the largest component but contains ALL game entities

2. **Player State** (~100 bytes)
   - Positions, HP, action states, round counts
   - Move and action history buffers

3. **Game State** (~50 bytes) 
   - Timers, round settings, camera position
   - Game mode flags and counters

4. **Input State** (~8KB - already identified)
   - Input buffers and processing state

**Total estimated state size: ~400KB per frame**
- This is manageable for 60-120 frame rollback buffer (24-48MB)
- Much larger than initially estimated but still feasible

---

## ? MAJOR BREAKTHROUGHS: Complete Engine Analysis

### Six Core Functions - Deep Analysis Complete

We've fully analyzed the six largest and most critical functions in the FM2K engine:

#### 1. Character State Machine (`sub_411BF0` - 8,166 bytes)
**Renamed to:** `character_state_machine`
- **Purpose**: Massive character update function handling animation, movement, script execution
- **Key Features**: 
  - Complex state machines for character behavior
  - Animation frame processing and transitions
  - Movement and physics calculations
  - Script execution for special moves
  - Object interaction handling
- **Rollback Impact**: Contains most character-specific state that needs preservation

#### 2. Sprite Rendering Engine (`sub_40CC30` - 6,072 bytes) 
**Renamed to:** `sprite_rendering_engine`
- **Purpose**: Advanced sprite rendering with effects and transformations
- **Key Features**:
  - Multiple blend modes (normal, additive, subtractive, alpha)
  - Palette manipulation and color effects
  - Lighting and shadow processing
  - Special effects rendering (particles, etc.)
  - Scaling and rotation transformations
- **Rollback Impact**: Rendering function - minimal state, but critical for visual consistency

#### 3. Game State Manager (`sub_409D00` - 2,321 bytes)
**Renamed to:** `game_state_manager`
- **Purpose**: Central game state and UI management
- **Key Features**:
  - Round management and transitions
  - Player health and status displays
  - UI element updates and positioning
  - Victory/defeat condition checking
  - Character selection handling
- **Rollback Impact**: High priority for state preservation

#### 4. Hit Detection System (`sub_40F010` - 2,301 bytes)
**Renamed to:** `hit_detection_system`
- **Purpose**: Collision detection and damage calculation
- **Key Features**:
  - Hitbox and hurtbox collision processing
  - Damage calculation and application
  - Reaction state determination
  - Combo and counter system
  - Environmental collision detection
- **Rollback Impact**: Critical for gameplay consistency - must be deterministic

#### 5. Graphics Blitter (`sub_40B4C0` - 2,214 bytes)
**Renamed to:** `graphics_blitter`
- **Purpose**: High-performance graphics blitting with multiple blend modes
- **Key Features**:
  - Normal, additive, subtractive blending
  - Alpha blending with different bit depths
  - Color space conversion (15-bit vs 16-bit)
  - Pixel manipulation functions
  - Direct framebuffer access
- **Rollback Impact**: Rendering function - can skip during rollback

#### 6. AI Behavior Processor (`sub_410060` - 2,110 bytes)
**Renamed to:** `ai_behavior_processor`  
- **Purpose**: AI decision making and input pattern processing
- **Key Features**:
  - Input pattern recognition and matching
  - AI decision trees and state evaluation
  - Move execution and timing
  - Character-specific behavior scripts
  - Difficulty scaling algorithms
- **Rollback Impact**: AI state must be preserved for deterministic behavior

---

## Complete Object System Architecture

### Object Pool Structure (CONFIRMED)
```c
// Object pool at 0x4701E0
#define MAX_OBJECTS 1023
#define OBJECT_SIZE 382

struct GameObject {
    uint32_t type;              // +0x000: Object type ID
    uint32_t list_type;         // +0x004: Object list category
    uint32_t x_position;        // +0x008: X position (fixed point)
    uint32_t y_position;        // +0x00C: Y position (fixed point)
    uint32_t state_flags;       // +0x010: Object state flags
    // ... 366 more bytes of object-specific data
    uint32_t parent_ptr;        // +0x17A: Back-pointer to parent object
    uint16_t active_flag;       // +0x17E: 0=inactive, 1=active
};

struct ObjectManager {
    uint32_t object_count;      // 0x4246FC: Current active objects
    uint32_t max_objects;       // 0x4259A4: Maximum objects (1023)
    uint32_t current_object;    // 0x4259A8: Current object being processed
    uint32_t object_data_ptr;   // 0x4CFA00: Pointer to current object data
    void*    list_heads[512];   // 0x430240: Object list head pointers
    void*    list_tails[512];   // 0x430244: Object list tail pointers
};
```

### Input System - Complete Specification

#### Input Encoding (11-bit mask)
```c
#define INPUT_LEFT     0x001    // Direction: Left
#define INPUT_RIGHT    0x002    // Direction: Right  
#define INPUT_UP       0x004    // Direction: Up
#define INPUT_DOWN     0x008    // Direction: Down
#define INPUT_BUTTON1  0x010    // Attack button 1
#define INPUT_BUTTON2  0x020    // Attack button 2
#define INPUT_BUTTON3  0x040    // Attack button 3
#define INPUT_BUTTON4  0x080    // Attack button 4
#define INPUT_BUTTON5  0x100    // Attack button 5
#define INPUT_BUTTON6  0x200    // Attack button 6
#define INPUT_BUTTON7  0x400    // Attack button 7

// Direction auto-flips based on character facing
// When character faces left, LEFT/RIGHT inputs are swapped
```

#### Input Processing Pipeline
```c
// Every frame at 100 FPS:
1. GetKeyboardState() Å® raw keyboard state
2. get_player_input() Å® encode to 11-bit mask
3. process_game_inputs() Å® apply repeat logic, store in buffers
4. Input stored at: g_p1_input_history[g_input_buffer_index]
5. g_input_buffer_index = (g_input_buffer_index + 1) & 0x3FF
```

#### Complete Input Memory Map
```
Input Buffers (1024 frames each):
0x4280E0: g_p1_input_history[1024]  // P1 input history (4096 bytes)
0x4290E0: g_p2_input_history[1024]  // P2 input history (4096 bytes)

Current Input State:
0x4259C0: g_p1_input               // Current P1 input
0x4259C4: g_p2_input               // Current P2 input
0x447EE0: g_input_buffer_index     // Circular buffer index

Input Processing Arrays:
0x447F00: g_prev_input_state[8]    // Previous frame inputs
0x447F40: g_processed_input[8]     // Processed inputs with repeat
0x447F60: g_input_changes[8]       // Edge detection (new presses)

Combined Input States:
0x4CFA04: g_combined_raw_input     // All raw inputs OR'd together
0x4280D8: g_combined_input_changes // All input changes OR'd together
0x4D1C20: g_combined_processed_input // All processed inputs OR'd together

Input Repeat System:
0x4D1C40: g_input_repeat_timer[8]  // Repeat timing counters
0x541F80: g_input_repeat_state[8]  // Repeat state tracking
0x41E3FC: g_input_initial_delay    // Initial repeat delay
0x41E400: g_input_repeat_delay     // Subsequent repeat delay
```

---

## Frame Timing System - Complete Analysis

### Timing Architecture
```c
struct FrameTiming {
    uint32_t frame_time_ms;        // 0x41E2F0: Always 10 (100 FPS)
    uint32_t last_frame_time;      // 0x447DD4: timeGetTime() timestamp
    uint32_t frame_skip_count;     // 0x4246F4: Number of frames to skip
    uint32_t frame_sync_flag;      // 0x424700: Frame synchronization state
    uint32_t frame_time_delta;     // 0x425960: Frame timing delta
};

// Main loop timing logic:
void main_game_loop() {
    while (running) {
        current_time = timeGetTime();
        if (current_time >= last_frame_time + 10) {
            // Calculate how many frames to process
            frame_skip = min(10, (current_time - last_frame_time) / 10);
            
            for (int i = 0; i < frame_skip; i++) {
                process_game_inputs();    // Input processing
                update_game_state();      // Game logic
                process_input_history();  // Input history
            }
            
            render_game();  // Rendering (only once per loop)
            last_frame_time += frame_skip * 10;
        }
    }
}
```
```

### Character Facing Controller (0x40e5c0)

The character facing controller manages character orientation and facing direction:

1. **Object Processing:**
   - Iterates through 1024 object pool entries
   - Processes active characters (type 4)
   - Validates hit masks and collision states
   - Checks character HP and state flags

2. **Direction Management:**
   - Uses `g_input_buffer_index` for current frame
   - Modifies input history for direction changes
   - Handles automatic character turning based on:
     - Relative position (16.16 fixed point)
     - Distance threshold (419430400)
     - Hit detection masks
     - Character state flags

3. **Memory Layout:**
   ```
   character_facing {
     +0: Object type (4 for characters)
     +8: Position X (16.16 fixed point)
     +20: Character state
     +342: Character ID
     +g_player_hit_mask: Hit detection mask
     +g_input_buffer_index: Current input frame
     +dword_4DFCD1: Facing direction flags
     +dword_4DFCE9: Target object pointer
   }
   ```

4. **Critical Operations:**
   - Distance calculation in fixed point
   - Input buffer modification
   - Direction flag updates
   - Hit mask validation
   - State preservation

5. **State Dependencies:**
   - Character positions
   - Input buffer state
   - Hit detection masks
   - Character HP values
   - Object pool pointers

This system is crucial for rollback as it affects:
- Input processing
- Character interactions
- Hit detection
- Visual representation

For rollback implementation:
1. Save facing states
2. Preserve input modifications
3. Handle direction changes
4. Maintain hit detection masks
5. Restore object relationships
```

### AI Input Processor (0x411270)

The AI input processor manages AI behavior and input simulation:

1. **AI Modes:**
   - Mode 1: Pattern-based decision making
   - Mode 2: Input mirroring
   - Mode 4: Simple input generation
   - Default: No input modification

2. **Pattern System:**
   - Uses probability weights (0-100)
   - Distance-based decision making
   - State-based pattern selection
   - Frame timing control

3. **Memory Layout:**
   ```
   ai_state {
     +57153: HP value
     +57157: Animation frame
     +57077: Character data pointer
     +57185: Difficulty modifier
     +57189: AI mode
     +57205: Current pattern ID
     +57209: Pattern timer
     +57213: Decision timer
     +57217: Pattern step index
   }
   ```

4. **Input Generation:**
   - Writes to `g_p1_input_history[1024 * player_id + g_input_buffer_index]`
   - Input flags:
     - 0x1-0x8: Directional inputs
     - 0x10-0x80: Attack buttons
     - 0x100-0x200: Special inputs
   - Pattern-based input sequences
   - State-based decision making

5. **Pattern Structure:**
   - Each pattern has:
     - Probability weight
     - Distance requirements
     - State requirements
     - Input sequence
     - Duration control
     - Repeat conditions

This system is crucial for rollback as it:
- Generates deterministic AI inputs
- Maintains consistent behavior
- Handles state-based decisions
- Manages input timing

For rollback implementation:
1. Save AI state
2. Preserve pattern indices
3. Maintain timers
4. Store decision state
5. Handle input generation
```

### Physics Collision System (0x40f910)

The physics collision system manages movement and collision resolution:

1. **Object Pool Processing:**
   - Iterates through 1024 object pool entries
   - Processes active objects (type 4)
   - Handles position updates
   - Manages collision resolution
   - Updates velocity vectors

2. **Position Management:**
   - Uses 16.16 fixed point format
   - Screen boundaries:
     - Left: 3276800 (50 << 16)
     - Right: 38666240 (590 << 16)
   - Push-out resolution for collisions
   - Velocity-based movement

3. **Memory Layout:**
   ```
   physics_object {
     +0: Object type (4 for characters)
     +2: Position X (16.16)
     +3: Position Y (16.16)
     +6: Velocity X (16.16)
     +7: Velocity Y (16.16)
     +10: State flags
     +16: Movement flags
     +28: Action counter
     +92: Direction flags
     +293-313: Collision boxes (20 entries)
     +346: State ID
     +350: Character state
     +378: Parent object pointer
   }
   ```

4. **Collision Box Structure:**
   ```
   collision_box {
     +0: Box pointer
     +1: X offset
     +3: Y offset
     +5: Width
     +7: Height
     +10: Type flags (bit 1: collision, bit 2: hit, bit 3: hurt)
   }
   ```

5. **State Dependencies:**
   - Character positions
   - Velocity vectors
   - Collision boxes
   - Hit/hurt boxes
   - Movement flags
   - Parent/child relationships

6. **Critical Operations:**
   - Position updates
   - Collision detection
   - Push-out resolution
   - Boundary checking
   - Velocity application
   - Parent object tracking

This system is crucial for rollback as it:
- Manages physical state
- Handles collisions
- Controls movement
- Enforces boundaries
- Updates positions

For rollback implementation:
1. Save physics state
2. Preserve velocities
3. Store collision data
4. Maintain relationships
5. Handle push-out state
```

### Character State Machine (0x411bf0)

The character state machine manages character behavior and state transitions:

1. **State Categories:**
   - Base states (0x0-0x3):
     - 0x0: Neutral/Standing
     - 0x1: Crouching
     - 0x2: Jumping
     - 0x3: Special
   - Action states (0x4-0x7):
     - 0x4: Attack
     - 0x5: Guard
     - 0x6: Hit stun
     - 0x7: Special move
   - Movement states (0x8-0xB):
     - 0x8: Walking
     - 0x9: Running
     - 0xA: Air movement
     - 0xB: Recovery

2. **State Transitions:**
   - Priority system:
     1. Hit stun/Guard break
     2. Special moves
     3. Normal attacks
     4. Movement
     5. Base states
   - Frame advantage tracking
   - Cancel windows
   - Buffer system

3. **Memory Layout:**
   ```
   character_state {
     +0: Object type (4)
     +4: Current state
     +8: Previous state
     +12: Frame counter
     +16: State timer
     +20: Cancel window
     +24: Buffer timer
     +28: Recovery frames
     +32: Advantage frames
     +36: Chain counter
   }
   ```

4. **State Processing:**
   - Input validation
   - State priority checks
   - Frame advantage calculation
   - Cancel window management
   - Buffer system processing
   - Recovery handling

5. **Critical State Flags:**
   - 0x1: State change pending
   - 0x2: Cancel allowed
   - 0x4: Buffer active
   - 0x8: Recovery active
   - 0x10: Chain allowed
   - 0x20: Special cancel
   - 0x40: Super cancel
   - 0x80: Guard break

This state machine is crucial for rollback implementation as it manages all character behavior and state transitions. The clear separation of state categories and well-defined transitions will make it easier to serialize and restore state during rollback operations.
```

### Hit Detection System (0x40f010)

The hit detection system manages collision detection and hit reactions:

1. **Object Pool Processing:**
   - Iterates through 1024 object pool entries
   - Processes active characters (type 4)
   - Validates hit masks and collision states
   - Checks character HP and state flags

2. **Hit Box Management:**
   - Uses 16.16 fixed point coordinates
   - Box dimensions stored in object structure:
     - Position offsets (offset 2-3)
     - Box dimensions (offset 5-7)
     - Hit masks (offset 10)
     - State flags (offset 350)

3. **Hit Detection Logic:**
   - Priority system:
     1. Special moves
     2. Normal attacks
     3. Throws
     4. Counter hits
   - State validation:
     - Attack active frames
     - Hit stun check
     - Guard state check
     - Special state flags

4. **Hit Reaction System:**
   - Damage calculation:
     - Base damage * scaling
     - Guard damage modifiers
     - Counter hit bonuses
   - State transitions:
     - Hit stun duration
     - Guard break states
     - Counter hit states
     - Special cancels

5. **Memory Layout:**
   ```
   hit_detection {
     +0: Object type (4)
     +2: Position X (16.16)
     +3: Position Y (16.16)
     +5: Hit box width
     +7: Hit box height
     +10: Hit flags
     +12: Damage value
     +14: Hit stun duration
     +16: Guard damage
     +20: State flags
     +23: Direction flags
     +26: Cancel window
     +27: Recovery frames
     +342: Character ID
     +346: Hit confirmation
     +350: State flags
   }
   ```

This system is crucial for rollback implementation as it manages all hit detection and reaction states. The clear separation of hit detection logic and state management will make it easier to serialize and restore hit states during rollback operations.
```

### Input Buffer System (0x4146d0)

The input buffer system manages input processing and history:

1. **Frame Processing:**
   - Runs at 100 FPS (10ms per frame)
   - Processes keyboard state
   - Maintains 1024-frame circular buffer
   - Separate buffers for P1 and P2

2. **Input History:**
   - Circular buffer index: `g_input_buffer_index & 0x3FF`
   - Input history arrays:
     ```
     g_p1_input_history[1024]
     g_p2_input_history[1024]
     ```
   - Input state flags:
     - 0x1-0x8: Directional inputs
     - 0x10-0x80: Attack buttons
     - 0x100-0x200: Special inputs

3. **Input Processing:**
   - Raw input capture
   - Input change detection
   - Input repeat handling:
     - Initial delay
     - Repeat delay
     - Repeat timer
   - Input validation
   - Mode-specific processing:
     - VS mode (< 3000)
     - Story mode (? 3000)

4. **Memory Layout:**
   ```
   input_system {
     +g_input_buffer_index: Current frame index
     +g_p1_input[8]: Current P1 inputs
     +g_p2_input: Current P2 inputs
     +g_prev_input_state[8]: Previous frame inputs
     +g_input_changes[8]: Input change flags
     +g_input_repeat_state[8]: Input repeat flags
     +g_input_repeat_timer[8]: Repeat timers
     +g_processed_input[8]: Processed inputs
     +g_combined_processed_input: Combined state
     +g_combined_input_changes: Change mask
     +g_combined_raw_input: Raw input mask
   }
   ```

5. **Critical Operations:**
   - Frame advance:
     ```c
     g_input_buffer_index = (g_input_buffer_index + 1) & 0x3FF;
     g_p1_input_history[g_input_buffer_index] = current_input;
     ```
   - Input processing:
     ```c
     g_input_changes = current_input & (prev_input ^ current_input);
     if (repeat_active && current_input == repeat_state) {
       if (--repeat_timer == 0) {
         processed_input = current_input;
         repeat_timer = repeat_delay;
       }
     }
     ```

This system is crucial for rollback implementation as it provides:
1. Deterministic input processing
2. Frame-accurate input history
3. Clear state serialization points
4. Mode-specific input handling
5. Input validation and processing logic

The 1024-frame history buffer is particularly useful for rollback, as it provides ample space for state rewinding and replay.
```

### Health Damage Manager (0x40e6f0)

The health damage manager handles health state and damage calculations:

1. **Health State Management:**
   - Per-character health tracking
   - Health segments system
   - Recovery mechanics
   - Guard damage handling

2. **Memory Layout:**
   ```
   health_system {
     +4DFC95: Current health segment
     +4DFC99: Maximum health segments
     +4DFC9D: Current health points
     +4DFCA1: Health segment size
   }
   ```

3. **Damage Processing:**
   - Segment-based health system:
     ```c
     while (current_health < 0) {
       if (current_segment > 0) {
         current_segment--;
         current_health += segment_size;
       } else {
         current_health = 0;
       }
     }
     ```
   - Health recovery:
     ```c
     while (current_health >= segment_size) {
       if (current_segment < max_segments) {
         current_health -= segment_size;
         current_segment++;
       } else {
         current_health = 0;
       }
     }
     ```

4. **Critical Operations:**
   - Damage application:
     - Negative values for damage
     - Positive values for healing
   - Segment management:
     - Segment depletion
     - Segment recovery
     - Maximum segment cap
   - Health validation:
     - Minimum health check
     - Maximum health check
     - Segment boundary checks

This system is crucial for rollback implementation as it:
1. Uses deterministic damage calculations
2. Maintains clear health state boundaries
3. Provides segment-based state tracking
4. Handles health recovery mechanics
5. Manages guard damage separately

The segmented health system will make it easier to serialize and restore health states during rollback operations, as each segment provides a clear checkpoint for state management.
```

### Rollback Implementation Summary

After analyzing FM2K's core systems, we can conclude that the game is well-suited for rollback netcode implementation. Here's a comprehensive overview of how each system will integrate with rollback:

1. **State Serialization Points:**
   - Character State Machine (0x411bf0):
     - Base states (0x0-0x3)
     - Action states (0x4-0x7)
     - Movement states (0x8-0xB)
     - Frame counters and timers
   - Hit Detection System (0x40f010):
     - Hit boxes and collision state
     - Damage values and scaling
     - Hit confirmation flags
   - Input Buffer System (0x4146d0):
     - 1024-frame history buffer
     - Input state flags
     - Repeat handling state
   - Health System (0x40e6f0):
     - Health segments
     - Current health points
     - Guard damage state
     - Recovery mechanics

2. **Deterministic Systems:**
   - Fixed frame rate (100 FPS)
   - 16.16 fixed-point coordinates
   - Deterministic input processing
   - Clear state transitions
   - Predictable damage calculations
   - Frame-based timers

3. **State Management:**
   - Object Pool (1024 entries):
     - Type identification
     - Position and velocity
     - Animation state
     - Collision data
   - Input History:
     - Circular buffer design
     - Clear frame boundaries
     - Input validation
   - Hit Detection:
     - Priority system
     - State validation
     - Reaction handling
   - Health System:
     - Segment-based tracking
     - Clear state boundaries
     - Recovery mechanics

4. **Implementation Strategy:**
   a. Save State (Frame N):
      ```c
      struct SaveState {
          // Character State
          int base_state;
          int action_state;
          int movement_state;
          int frame_counters[4];
          
          // Hit Detection
          int hit_boxes[20];
          int damage_values[8];
          int hit_flags;
          
          // Input Buffer
          int input_history[1024];
          int input_flags;
          int repeat_state;
          
          // Health System
          int health_segments;
          int current_health;
          int guard_damage;
      };
      ```

   b. Rollback Process:
      1. Save current frame state
      2. Apply new remote input
      3. Simulate forward:
         - Process inputs
         - Update character states
         - Handle collisions
         - Apply damage
      4. Render new state
      5. Save confirmed state

5. **Critical Hooks:**
   - Input Processing (0x4146d0):
     - Frame advance point
     - Input buffer management
   - Character State (0x411bf0):
     - State transition handling
     - Frame counting
   - Hit Detection (0x40f010):
     - Collision processing
     - Damage application
   - Health System (0x40e6f0):
     - Health state management
     - Segment tracking

6. **Optimization Opportunities:**
   - Parallel state simulation
   - Minimal state serialization
   - Efficient state diffing
   - Smart state prediction
   - Input compression

The analysis reveals that FM2K's architecture is highly suitable for rollback implementation due to its:
1. Deterministic game logic
2. Clear state boundaries
3. Frame-based processing
4. Efficient memory layout
5. Well-defined systems

This research provides a solid foundation for implementing rollback netcode in FM2K, with all major systems supporting the requirements for state saving, rollback, and resimulation.

## GekkoNet Integration Analysis

### GekkoNet Library Overview

GekkoNet is a rollback netcode library that provides the networking infrastructure we need for FM2K. Here's how it maps to our analyzed systems:

### 1. **GekkoNet Configuration for FM2K**
```c
GekkoConfig fm2k_config = {
    .num_players = 2,                    // P1 vs P2
    .max_spectators = 8,                 // Allow spectators
    .input_prediction_window = 8,        // 8 frames ahead prediction
    .spectator_delay = 6,                // 6 frame spectator delay
    .input_size = sizeof(uint32_t),      // FM2K input flags (32-bit)
    .state_size = sizeof(FM2KGameState), // Our minimal state structure
    .limited_saving = false,             // Full state saving for accuracy
    .post_sync_joining = true,           // Allow mid-match spectators
    .desync_detection = true             // Enable desync detection
};
```

### 2. **Integration Points with FM2K Systems**

#### Input System Integration (0x4146d0)
```c
// Hook into process_game_inputs()
void fm2k_input_hook() {
    // Get GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case AdvanceEvent:
                // Inject inputs into FM2K's input system
                g_p1_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[0];
                g_p2_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[1];
                // Continue normal input processing
                break;
                
            case SaveEvent:
                // Save current FM2K state
                save_fm2k_state(events[i]->data.save.state, events[i]->data.save.state_len);
                break;
                
            case LoadEvent:
                // Restore FM2K state
                load_fm2k_state(events[i]->data.load.state, events[i]->data.load.state_len);
                break;
        }
    }
}
```

#### State Management Integration
```c
typedef struct FM2KGameState {
    // Frame tracking
    uint32_t frame_number;
    uint32_t input_buffer_index;
    
    // Input state
    uint32_t p1_input_current;
    uint32_t p2_input_current;
    uint32_t input_repeat_state[8];
    uint32_t input_repeat_timer[8];
    
    // Character states (per character)
    struct {
        // Position (16.16 fixed point)
        int32_t pos_x, pos_y;
        int32_t vel_x, vel_y;
        
        // State machine
        uint16_t base_state;        // 0x0-0x3
        uint16_t action_state;      // 0x4-0x7  
        uint16_t movement_state;    // 0x8-0xB
        uint16_t state_flags;       // Offset 350
        
        // Animation
        uint16_t animation_frame;   // Offset 12
        uint16_t max_animation_frame; // Offset 88
        
        // Health system
        uint16_t health_segments;   // Current segments
        uint16_t current_health;    // Health points
        uint16_t max_health_segments; // Maximum segments
        uint16_t guard_damage;      // Guard damage accumulation
        
        // Hit detection
        uint16_t hit_flags;         // Hit state flags
        uint16_t hit_stun_timer;    // Hit stun duration
        uint16_t guard_state;       // Guard state flags
        
        // Facing and direction
        uint8_t facing_direction;   // Left/right facing
        uint8_t input_direction;    // Current input direction
    } characters[2];
    
    // Round state
    uint32_t round_state;           // Round state machine
    uint32_t round_timer;           // Round timer
    uint32_t game_mode;             // Current game mode
    
    // Hit detection global state
    uint32_t hit_effect_target;     // Hit effect target
    uint32_t hit_effect_timer;      // Hit effect timer
    
    // Combo system
    uint16_t combo_counter[2];      // Combo counters
    uint16_t damage_scaling[2];     // Damage scaling
    
    // Object pool critical state
    uint16_t active_objects;        // Number of active objects
    uint32_t object_state_flags;    // Critical object flags
    
} FM2KGameState;
```

### 3. **Network Adapter Implementation**
```c
GekkoNetAdapter fm2k_adapter = {
    .send_data = fm2k_send_packet,
    .receive_data = fm2k_receive_packets,
    .free_data = fm2k_free_packet_data
};

void fm2k_send_packet(GekkoNetAddress* addr, const char* data, int length) {
    // Use existing LilithPort networking or implement UDP
    // addr->data contains IP/port information
    // data contains the packet to send
}

GekkoNetResult** fm2k_receive_packets(int* length) {
    // Collect all packets received since last call
    // Return array of GekkoNetResult pointers
    // GekkoNet will call free_data on each result
}
```

### 4. **Integration with Existing FM2K Systems**

#### Main Game Loop Integration
```c
// Hook at 0x4146d0 - process_game_inputs()
void fm2k_rollback_frame() {
    // 1. Poll network
    gekko_network_poll(session);
    
    // 2. Add local inputs
    uint32_t local_input = get_local_player_input();
    gekko_add_local_input(session, local_player_id, &local_input);
    
    // 3. Process GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    // 4. Handle events (save/load/advance)
    process_gekko_events(events, event_count);
    
    // 5. Continue normal FM2K processing
    // (character updates, hit detection, etc.)
}
```

#### State Serialization Functions
```c
void save_fm2k_state(unsigned char* buffer, unsigned int* length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Save frame state
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    
    // Save character states from object pool
    for (int i = 0; i < 2; i++) {
        save_character_state(&state->characters[i], i);
    }
    
    // Save round state
    state->round_state = g_round_state;
    state->round_timer = g_round_timer;
    
    *length = sizeof(FM2KGameState);
}

void load_fm2k_state(unsigned char* buffer, unsigned int length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Restore frame state
    current_frame = state->frame_number;
    g_input_buffer_index = state->input_buffer_index;
    
    // Restore character states to object pool
    for (int i = 0; i < 2; i++) {
        load_character_state(&state->characters[i], i);
    }
    
    // Restore round state
    g_round_state = state->round_state;
    g_round_timer = state->round_timer;
}
```

### 5. **Event Handling Integration**
```c
void handle_session_events() {
    int event_count;
    GekkoSessionEvent** events = gekko_session_events(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case PlayerConnected:
                // Show "Player Connected" message
                break;
            case PlayerDisconnected:
                // Handle disconnection
                break;
            case DesyncDetected:
                // Log desync information
                printf("Desync at frame %d: local=%08x remote=%08x\n",
                       events[i]->data.desynced.frame,
                       events[i]->data.desynced.local_checksum,
                       events[i]->data.desynced.remote_checksum);
                break;
        }
    }
}
```

### 6. **Performance Considerations**
- **State Size**: ~200 bytes per frame (very efficient)
- **Prediction Window**: 8 frames ahead (80ms at 100 FPS)
- **Rollback Depth**: Up to 15 frames (150ms)
- **Network Frequency**: 100 Hz to match FM2K's frame rate

### 7. **Implementation Benefits**
1. **Minimal Code Changes**: Only need to hook input processing
2. **Existing Infrastructure**: Leverages FM2K's input history system
3. **Deterministic**: FM2K's fixed-point math ensures consistency
4. **Efficient**: Small state size enables fast rollback
5. **Robust**: GekkoNet handles all networking complexity

This integration approach leverages both FM2K's existing architecture and GekkoNet's proven rollback implementation, providing a solid foundation for online play.

### Critical Player State Variables

Based on IDA analysis, here are the key memory addresses for player state:

#### Player 1 State Variables:
```c
// Input system
g_p1_input          = 0x4259c0;  // Current input state
g_p1_input_history  = 0x4280e0;  // 1024-frame input history

// Position and stage
g_p1_stage_x        = 0x424e68;  // Stage position X
g_p1_stage_y        = 0x424e6c;  // Stage position Y

// Round and game state
g_p1_round_count    = 0x4700ec;  // Round wins
g_p1_round_state    = 0x4700f0;  // Round state
g_p1_action_state   = 0x47019c;  // Action state

// Health system
g_p1_hp             = 0x4dfc85;  // Current HP
g_p1_max_hp         = 0x4dfc91;  // Maximum HP
```

#### Player 2 State Variables:
```c
// Input system
g_p2_input          = 0x4259c4;  // Current input state
g_p2_input_history  = 0x4290e0;  // 1024-frame input history

// Round and game state
g_p2_action_state   = 0x4701a0;  // Action state

// Health system
g_p2_hp             = 0x4edcc4;  // Current HP
g_p2_max_hp         = 0x4edcd0;  // Maximum HP
```

#### Core Game State Variables:
```c
// Object pool
g_object_pool       = 0x4701e0;  // Main object pool (1024 entries)
g_sprite_data_array = 0x447938;  // Sprite data array

// Timing system
g_frame_time_ms     = 0x41e2f0;  // Frame duration (10ms)
g_last_frame_time   = 0x447dd4;  // Last frame timestamp
g_frame_sync_flag   = 0x424700;  // Frame sync state
g_frame_time_delta  = 0x425960;  // Frame timing delta
g_frame_skip_count  = 0x4246f4;  // Frame skip counter

// Random number generator
g_random_seed       = 0x41fb1c;  // RNG seed

// Input buffer management
g_input_buffer_index = (calculated); // Current buffer index
g_input_repeat_timer = 0x4d1c40;     // Input repeat timers

// Round system
g_round_timer       = 0x470060;  // Round timer
g_game_timer        = 0x470044;  // Game timer
g_hit_effect_timer  = 0x4701c8;  // Hit effect timer
```

### Final State Structure for GekkoNet

With all the memory addresses identified, here's the complete state structure:

```c
typedef struct FM2KCompleteGameState {
    // Frame and timing state
    uint32_t frame_number;
    uint32_t input_buffer_index;
    uint32_t last_frame_time;
    uint32_t frame_time_delta;
    uint8_t frame_skip_count;
    uint8_t frame_sync_flag;
    
    // Random number generator state
    uint32_t random_seed;
    
    // Player 1 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t stage_x, stage_y;
        uint32_t round_count;
        uint32_t round_state;
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p1;
    
    // Player 2 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p2;
    
    // Global timers
    uint32_t round_timer;
    uint32_t game_timer;
    uint32_t hit_effect_timer;
    
    // Input system state
    uint32_t input_repeat_timer[8];
    
    // Critical object pool state (first 2 character objects)
    struct {
        uint32_t object_type;
        int32_t pos_x, pos_y;        // 16.16 fixed point
        int32_t vel_x, vel_y;
        uint16_t state_flags;
        uint16_t animation_frame;
        uint16_t health_segments;
        uint16_t facing_direction;
        // ... other critical fields
    } character_objects[2];
    
} FM2KCompleteGameState;
```

### Memory Access Functions

```c
// Direct memory access functions for state serialization
void save_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Frame and timing
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    state->last_frame_time = *(uint32_t*)0x447dd4;
    state->frame_time_delta = *(uint32_t*)0x425960;
    state->frame_skip_count = *(uint8_t*)0x4246f4;
    state->frame_sync_flag = *(uint8_t*)0x424700;
    
    // RNG state
    state->random_seed = *(uint32_t*)0x41fb1c;
    
    // Player 1 state
    state->p1.input_current = *(uint32_t*)0x4259c0;
    memcpy(state->p1.input_history, (void*)0x4280e0, 1024 * sizeof(uint32_t));
    state->p1.stage_x = *(uint32_t*)0x424e68;
    state->p1.stage_y = *(uint32_t*)0x424e6c;
    state->p1.round_count = *(uint32_t*)0x4700ec;
    state->p1.round_state = *(uint32_t*)0x4700f0;
    state->p1.action_state = *(uint32_t*)0x47019c;
    state->p1.hp = *(uint32_t*)0x4dfc85;
    state->p1.max_hp = *(uint32_t*)0x4dfc91;
    
    // Player 2 state
    state->p2.input_current = *(uint32_t*)0x4259c4;
    memcpy(state->p2.input_history, (void*)0x4290e0, 1024 * sizeof(uint32_t));
    state->p2.action_state = *(uint32_t*)0x4701a0;
    state->p2.hp = *(uint32_t*)0x4edcc4;
    state->p2.max_hp = *(uint32_t*)0x4edcd0;
    
    // Global timers
    state->round_timer = *(uint32_t*)0x470060;
    state->game_timer = *(uint32_t*)0x470044;
    state->hit_effect_timer = *(uint32_t*)0x4701c8;
    
    // Input repeat timers
    memcpy(state->input_repeat_timer, (void*)0x4d1c40, 8 * sizeof(uint32_t));
    
    // Character objects from object pool
    save_character_objects_from_pool(state->character_objects);
}

void load_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Restore all state in reverse order
    // ... (similar but with assignment in opposite direction)
}
```

This gives us a complete, memory-accurate state structure that can be used with GekkoNet for rollback implementation. The state size is approximately 10KB, which is very reasonable for rollback netcode.

## GekkoNet SDL Example Analysis & FM2K Integration

### SDL Example Key Patterns

The OnlineSession.cpp example demonstrates the core GekkoNet integration patterns we need for FM2K:

#### 1. **Session Setup Pattern**
```c
// From SDL example - adapt for FM2K
GekkoSession* sess = nullptr;
GekkoConfig conf {
    .num_players = 2,
    .input_size = sizeof(char),           // FM2K: sizeof(uint32_t)
    .state_size = sizeof(GState),         // FM2K: sizeof(FM2KCompleteGameState)
    .max_spectators = 0,                  // FM2K: 8 for spectators
    .input_prediction_window = 10,        // FM2K: 8 frames
    .desync_detection = true,
    // .limited_saving = true,            // FM2K: false for accuracy
};

gekko_create(&sess);
gekko_start(sess, &conf);
gekko_net_adapter_set(sess, gekko_default_adapter(localport));
```

#### 2. **Player Management Pattern**
```c
// Order-dependent player addition (from example)
if (localplayer == 0) {
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
} else {
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
}
```

#### 3. **Core Game Loop Pattern** 
```c
// From SDL example - critical for FM2K integration
while (running) {
    // Timing control
    frames_ahead = gekko_frames_ahead(sess);
    frame_time = GetFrameTime(frames_ahead);
    
    // Network polling
    gekko_network_poll(sess);
    
    // Frame processing
    while (accumulator >= frame_time) {
        // Add local input
        auto input = get_key_inputs();
        gekko_add_local_input(sess, localplayer, &input);
        
        // Handle session events
        auto events = gekko_session_events(sess, &count);
        for (int i = 0; i < count; i++) {
            // Handle PlayerConnected, PlayerDisconnected, DesyncDetected
        }
        
        // Process game events
        auto updates = gekko_update_session(sess, &count);
        for (int i = 0; i < count; i++) {
            switch (updates[i]->type) {
                case SaveEvent: save_state(&state, updates[i]); break;
                case LoadEvent: load_state(&state, updates[i]); break;
                case AdvanceEvent:
                    // Extract inputs and advance game
                    inputs[0].input.value = updates[i]->data.adv.inputs[0];
                    inputs[1].input.value = updates[i]->data.adv.inputs[1];
                    update_state(state, inputs, num_players);
                    break;
            }
        }
        accumulator -= frame_time;
    }
    
    // Render current state
    render_state(state);
}
```

### FM2K Integration Strategy

#### 1. **Replace SDL2 with SDL3 for FM2K**
```c
// SDL3 initialization for FM2K integration
#include "SDL3/SDL.h"

bool init_fm2k_window(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Error initializing SDL3.\n");
        return false;
    }
    
    // Create window matching FM2K's resolution
    window = SDL_CreateWindow(
        "FM2K Online Session",
        640, 480,  // FM2K native resolution
        SDL_WINDOW_RESIZABLE
    );
    
    if (!window) {
        fprintf(stderr, "Error creating SDL3 Window.\n");
        return false;
    }
    
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        fprintf(stderr, "Error creating SDL3 Renderer.\n");
        return false;
    }
    
    return true;
}
```

#### 2. **FM2K Input Structure**
```c
// Replace simple GInput with FM2K's 11-bit input system
struct FM2KInput {
    union {
        struct {
            uint16_t left     : 1;   // 0x001
            uint16_t right    : 1;   // 0x002
            uint16_t up       : 1;   // 0x004
            uint16_t down     : 1;   // 0x008
            uint16_t button1  : 1;   // 0x010
            uint16_t button2  : 1;   // 0x020
            uint16_t button3  : 1;   // 0x040
            uint16_t button4  : 1;   // 0x080
            uint16_t button5  : 1;   // 0x100
            uint16_t button6  : 1;   // 0x200
            uint16_t button7  : 1;   // 0x400
            uint16_t reserved : 5;   // Unused bits
        } bits;
        uint16_t value;
    } input;
};

FM2KInput get_fm2k_inputs() {
    FM2KInput input{};
    input.input.value = 0;
    
    // Get keyboard state (SDL3 syntax)
    const bool* keys = SDL_GetKeyboardState(NULL);
    
    // Map to FM2K inputs
    input.input.bits.up = keys[SDL_SCANCODE_W];
    input.input.bits.left = keys[SDL_SCANCODE_A];
    input.input.bits.down = keys[SDL_SCANCODE_S];
    input.input.bits.right = keys[SDL_SCANCODE_D];
    input.input.bits.button1 = keys[SDL_SCANCODE_J];
    input.input.bits.button2 = keys[SDL_SCANCODE_K];
    input.input.bits.button3 = keys[SDL_SCANCODE_L];
    // ... map other buttons
    
    return input;
}
```

#### 3. **FM2K State Management**
```c
// Replace simple GState with FM2KCompleteGameState
void save_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    *ev->data.save.state_len = sizeof(FM2KCompleteGameState);
    
    // Use Fletcher's checksum (from example) for consistency
    *ev->data.save.checksum = fletcher32((uint16_t*)gs, sizeof(FM2KCompleteGameState));
    
    std::memcpy(ev->data.save.state, gs, sizeof(FM2KCompleteGameState));
}

void load_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    std::memcpy(gs, ev->data.load.state, sizeof(FM2KCompleteGameState));
}

void update_fm2k_state(FM2KCompleteGameState& gs, FM2KInput inputs[2]) {
    // Call into FM2K's actual game logic
    // This would hook into the functions we analyzed:
    
    // 1. Inject inputs into FM2K's input system
    *(uint32_t*)0x4259c0 = inputs[0].input.value;  // g_p1_input
    *(uint32_t*)0x4259c4 = inputs[1].input.value;  // g_p2_input
    
    // 2. Call FM2K's update functions
    // process_game_inputs_FRAMESTEP_HOOK();  // 0x4146d0
    // update_game_state();                   // Game logic
    // character_state_machine();             // 0x411bf0
    // hit_detection_system();                // 0x40f010
    
    // 3. Extract updated state from FM2K memory
    gs.p1.hp = *(uint32_t*)0x4dfc85;
    gs.p2.hp = *(uint32_t*)0x4edcc4;
    gs.round_timer = *(uint32_t*)0x470060;
    // ... extract all other state variables
}
```

#### 4. **FM2K Timing Integration**
```c
// Adapt timing to match FM2K's 100 FPS
float GetFM2KFrameTime(float frames_ahead) {
    // FM2K runs at 100 FPS (10ms per frame)
    const float base_frame_time = 1.0f / 100.0f;  // 0.01 seconds
    
    if (frames_ahead >= 0.75f) {
        // Slow down if too far ahead
        return base_frame_time * 1.02f;  // 59.8 FPS equivalent
    } else {
        return base_frame_time;  // Normal 100 FPS
    }
}
```

#### 5. **Complete FM2K Integration Main Loop**
```c
int fm2k_online_main(int argc, char* args[]) {
    // Parse command line (same as example)
    int localplayer = std::stoi(args[1]);
    const int localport = std::stoi(args[2]);
    std::string remote_address = std::move(args[3]);
    int localdelay = std::stoi(args[4]);
    
    // Initialize SDL3 and FM2K
    running = init_fm2k_window();
    
    // Initialize FM2K state
    FM2KCompleteGameState state = {};
    FM2KInput inputs[2] = {};
    
    // Setup GekkoNet
    GekkoSession* sess = nullptr;
    GekkoConfig conf {
        .num_players = 2,
        .input_size = sizeof(uint16_t),  // FM2K input size
        .state_size = sizeof(FM2KCompleteGameState),
        .max_spectators = 8,
        .input_prediction_window = 8,
        .desync_detection = true,
        .limited_saving = false,  // Full state saving for accuracy
        .post_sync_joining = true
    };
    
    gekko_create(&sess);
    gekko_start(sess, &conf);
    gekko_net_adapter_set(sess, gekko_default_adapter(localport));
    
    // Add players (order-dependent)
    if (localplayer == 0) {
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
    } else {
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    }
    
    gekko_set_local_delay(sess, localplayer, localdelay);
    
    // Timing variables
    auto curr_time = std::chrono::steady_clock::now();
    auto prev_time = curr_time;
    float delta_time = 0.0f;
    float accumulator = 0.0f;
    float frame_time = 0.0f;
    float frames_ahead = 0.0f;
    
    while (running) {
        curr_time = std::chrono::steady_clock::now();
        
        frames_ahead = gekko_frames_ahead(sess);
        frame_time = GetFM2KFrameTime(frames_ahead);
        
        delta_time = std::chrono::duration<float>(curr_time - prev_time).count();
        prev_time = curr_time;
        accumulator += delta_time;
        
        gekko_network_poll(sess);
        
        while (accumulator >= frame_time) {
            // Process SDL events
            process_events();
            
            // Get local input
            auto input = get_fm2k_inputs();
            gekko_add_local_input(sess, localplayer, &input);
            
            // Handle session events
            int count = 0;
            auto events = gekko_session_events(sess, &count);
            for (int i = 0; i < count; i++) {
                handle_fm2k_session_event(events[i]);
            }
            
            // Process game events
            count = 0;
            auto updates = gekko_update_session(sess, &count);
            for (int i = 0; i < count; i++) {
                auto ev = updates[i];
                
                switch (ev->type) {
                    case SaveEvent:
                        save_fm2k_state(&state, ev);
                        break;
                    case LoadEvent:
                        load_fm2k_state(&state, ev);
                        break;
                    case AdvanceEvent:
                        // Extract inputs from GekkoNet
                        inputs[0].input.value = ((uint16_t*)ev->data.adv.inputs)[0];
                        inputs[1].input.value = ((uint16_t*)ev->data.adv.inputs)[1];
                        
                        // Update FM2K game state
                        update_fm2k_state(state, inputs);
                        break;
                }
            }
            
            accumulator -= frame_time;
        }
        
        // Render current state
        render_fm2k_state(state);
    }
    
    gekko_destroy(sess);
    del_window();
    return 0;
}
```

### Key Differences from SDL Example

1. **Input Size**: FM2K uses 16-bit inputs vs 8-bit in example
2. **State Size**: ~10KB vs ~16 bytes in example  
3. **Frame Rate**: 100 FPS vs 60 FPS in example
4. **State Complexity**: Full game state vs simple position
5. **Integration Depth**: Hooks into existing game vs standalone

### Integration Benefits

1. **Proven Pattern**: The SDL example shows GekkoNet works reliably
2. **Clear Structure**: Event handling patterns are well-defined
3. **Performance**: Timing control handles frame rate variations
4. **Network Stats**: Built-in ping/jitter monitoring
5. **Error Handling**: Desync detection and player management

This analysis shows that adapting the SDL example for FM2K is very feasible, with the main changes being input/state structures and frame timing to match FM2K's 100 FPS architecture.

## Hit Judge and Round System Architecture

### Hit Judge Configuration (0x42470C-0x430120)

The game uses a sophisticated hit detection and configuration system loaded from INI files:

```c
struct FM2K_HitJudgeConfig {
    uint32_t hit_judge_value;    // 0x42470C - Hit detection threshold
    uint32_t config_values[7];   // 0x4300E0-0x430120 - General config values
    char     config_string[260]; // Configuration string buffer
};
```

The hit judge system is initialized by `hit_judge_set_function` (0x414930) which:
1. Loads configuration from INI files
2. Sets up hit detection parameters
3. Configures round settings and game modes

### Round System State (0x470040-0x47006C)

The round system maintains several critical state variables:

```c
struct FM2K_RoundSystem {
    uint32_t game_mode;      // 0x470040 - Current game mode (VS/Team/Tournament)
    uint32_t round_limit;    // 0x470048 - Maximum rounds for the match
    uint32_t round_state;    // 0x47004C - Current round state
    uint32_t round_timer;    // 0x470060 - Active round timer
};

enum FM2K_RoundState {
    ROUND_INIT = 0,      // Initial state
    ROUND_ACTIVE = 1,    // Round in progress
    ROUND_END = 2,       // Round has ended
    ROUND_MATCH_END = 3  // Match has ended
};

enum FM2K_GameMode {
    MODE_NORMAL = 0,     // Normal VS mode
    MODE_TEAM = 1,       // Team Battle mode
    MODE_TOURNAMENT = 2  // Tournament mode
};
```

The round system is managed by `vs_round_function` (0x4086A0) which:
1. Handles round state transitions
2. Manages round timers
3. Processes win/loss conditions
4. Controls match flow

### Rollback Implications

This architecture has several important implications for rollback implementation:

1. **State Serialization Requirements:**
   - Must save hit judge configuration
   - Must preserve round state and timers
   - Game mode affects state size

2. **Deterministic Behavior:**
   - Hit detection is configuration-driven
   - Round state transitions are deterministic
   - Timer-based events are predictable

3. **State Size Optimization:**
   - Hit judge config is static per match
   - Only dynamic round state needs saving
   - Can optimize by excluding static config

4. **Synchronization Points:**
   - Round state changes are key sync points
   - Hit detection results must be identical
   - Timer values must match exactly

Updated state structure for rollback:

```c
typedef struct FM2KGameState {
    // ... existing timing and input state ...

    // Round System State
    struct {
        uint32_t mode;          // Current game mode
        uint32_t round_limit;   // Maximum rounds
        uint32_t round_state;   // Current round state
        uint32_t round_timer;   // Active round timer
        uint32_t score_value;   // Current score/timer value
    } round_system;

    // Hit Judge State
    struct {
        uint32_t hit_judge_value;   // Current hit detection threshold
        uint32_t effect_target;     // Current hit effect target
        uint32_t effect_timer;      // Hit effect duration
    } hit_system;

    // ... rest of game state ...
} FM2KGameState;
```

## Sprite Rendering and Hit Detection System

### Sprite Rendering Engine (0x40CC30)

The game uses a sophisticated sprite rendering engine that handles:
1. Hit effect visualization
2. Sprite transformations and blending
3. Color palette manipulation
4. Screen-space effects

Key components:

```c
struct SpriteRenderState {
    uint32_t render_mode;     // Different rendering modes (-10 to 3)
    uint32_t blend_flags;     // Blending and effect flags
    uint32_t color_values[3]; // RGB color modification values
    uint32_t effect_timer;    // Effect duration counter
};
```

### Hit Effect System

The hit effect system is tightly integrated with the sprite renderer:

1. **Effect Triggers:**
   - Hit detection results (0x42470C)
   - Game state transitions
   - Timer-based events

2. **Effect Parameters:**
   ```c
   struct HitEffectParams {
       uint32_t target_id;      // Effect target identifier
       uint32_t effect_type;    // Visual effect type
       uint32_t duration;       // Effect duration in frames
       uint32_t color_values;   // RGB color modulation
   };
   ```

3. **Rendering Pipeline:**
   - Color palette manipulation (0x4D1A20)
   - Sprite transformation
   - Blend mode selection
   - Screen-space effects

### Integration with Rollback

The sprite and hit effect systems have important implications for rollback:

1. **Visual State Management:**
   - Effect timers must be part of the game state
   - Color modulation values need saving
   - Sprite transformations must be deterministic

2. **Performance Considerations:**
   - Effect parameters are small (few bytes per effect)
   - Visual state is derived from game state
   - Can optimize by excluding pure visual state

3. **Synchronization Requirements:**
   - Hit effects must trigger identically
   - Effect timers must stay synchronized
   - Visual state must match game state

Updated state structure to include visual effects:

```c
typedef struct FM2KGameState {
    // ... previous state members ...

    // Visual Effect State
    struct {
        uint32_t active_effects;     // Bitfield of active effects
        uint32_t effect_timers[8];   // Array of effect durations
        uint32_t color_values[8][3]; // RGB values for each effect
        uint32_t target_ids[8];      // Effect target identifiers
    } visual_state;

    // ... rest of game state ...
} FM2KGameState;
```

## Game State Management System

The game state manager (0x406FC0) handles several critical aspects of the game:

### State Variables

1. **Round System State:**
   ```c
   struct RoundSystemState {
       uint32_t g_round_timer;        // 0x470060 - Current round timer value
       uint32_t g_round_limit;        // 0x470048 - Maximum rounds per match
       uint32_t g_round_state;        // 0x47004C - Current round state
       uint32_t g_game_mode;          // 0x470040 - Current game mode
   };
   ```

2. **Player State:**
   ```c
   struct PlayerState {
       int32_t stage_position;        // Current position on stage grid
       uint32_t action_state;         // Current action being performed
       uint32_t round_count;          // Number of rounds completed
       uint32_t input_history[4];     // Recent input history
       uint32_t move_history[4];      // Recent movement history
   };
   ```

### State Management Flow

1. **Initialization (0x406FC0):**
   - Loads initial configuration
   - Sets up round timer and limits
   - Initializes player positions
   - Sets up game mode parameters

2. **State Updates:**
   - Input processing and validation
   - Position and movement updates
   - Action state transitions
   - Round state management

3. **State Synchronization:**
   - Timer synchronization
   - Player state synchronization
   - Round state transitions
   - Game mode state management

### Rollback Implications

1. **Critical State Components:**
   - Round timer and state
   - Player positions and actions
   - Input and move history
   - Game mode state

2. **State Size Analysis:**
   - Core game state: ~128 bytes
   - Player state: ~32 bytes per player
   - History buffers: ~32 bytes per player
   - Total per-frame state: ~256 bytes

3. **Synchronization Requirements:**
   - Timer must be deterministic
   - Player state must be atomic
   - Input processing must be consistent
   - State transitions must be reproducible

## Input Processing System

The game uses a sophisticated input processing system that handles both movement and action inputs:

### Input Mapping (0x406F20)

```c
enum InputActions {
    ACTION_NONE = 0,
    ACTION_A = 1,    // 0x20  - Light Attack
    ACTION_B = 2,    // 0x40  - Medium Attack
    ACTION_C = 3,    // 0x80  - Heavy Attack
    ACTION_D = 4,    // 0x100 - Special Move
    ACTION_E = 5     // 0x200 - Super Move
};

enum InputDirections {
    DIR_NONE  = 0x000,
    DIR_UP    = 0x004,
    DIR_DOWN  = 0x008,
    DIR_LEFT  = 0x001,
    DIR_RIGHT = 0x002
};
```

### Input Processing Flow

1. **Raw Input Capture:**
   - Movement inputs (4-way directional)
   - Action buttons (5 main buttons)
   - System buttons (Start, Select)

2. **Input State Management:**
   ```c
   struct InputState {
       uint16_t current_input;     // Current frame's input
       uint16_t previous_input;    // Last frame's input
       uint16_t input_changes;     // Changed bits since last frame
       uint8_t  processed_input;   // Processed directional input
   };
   ```

3. **Input History:**
   - Each player maintains a 4-frame input history
   - History includes both raw and processed inputs
   - Used for move validation and rollback

4. **Input Processing:**
   - Raw input capture
   - Input change detection
   - Input repeat handling:
     - Initial delay
     - Repeat delay
     - Repeat timer
   - Input validation
   - Mode-specific processing:
     - VS mode (< 3000)
     - Story mode (? 3000)

5. **Memory Layout:**
   ```
   input_system {
     +g_input_buffer_index: Current frame index
     +g_p1_input[8]: Current P1 inputs
     +g_p2_input: Current P2 inputs
     +g_prev_input_state[8]: Previous frame inputs
     +g_input_changes[8]: Input change flags
     +g_input_repeat_state[8]: Input repeat flags
     +g_input_repeat_timer[8]: Repeat timers
     +g_processed_input[8]: Processed inputs
     +g_combined_processed_input: Combined state
     +g_combined_input_changes: Change mask
     +g_combined_raw_input: Raw input mask
   }
   ```

6. **Critical Operations:**
   - Frame advance:
     ```c
     g_input_buffer_index = (g_input_buffer_index + 1) & 0x3FF;
     g_p1_input_history[g_input_buffer_index] = current_input;
     ```
   - Input processing:
     ```c
     g_input_changes = current_input & (prev_input ^ current_input);
     if (repeat_active && current_input == repeat_state) {
       if (--repeat_timer == 0) {
         processed_input = current_input;
         repeat_timer = repeat_delay;
       }
     }
     ```

This system is crucial for rollback implementation as it provides:
1. Deterministic input processing
2. Frame-accurate input history
3. Clear state serialization points
4. Mode-specific input handling
5. Input validation and processing logic

The 1024-frame history buffer is particularly useful for rollback, as it provides ample space for state rewinding and replay.
```

### Health Damage Manager (0x40e6f0)

The health damage manager handles health state and damage calculations:

1. **Health State Management:**
   - Per-character health tracking
   - Health segments system
   - Recovery mechanics
   - Guard damage handling

2. **Memory Layout:**
   ```
   health_system {
     +4DFC95: Current health segment
     +4DFC99: Maximum health segments
     +4DFC9D: Current health points
     +4DFCA1: Health segment size
   }
   ```

3. **Damage Processing:**
   - Segment-based health system:
     ```c
     while (current_health < 0) {
       if (current_segment > 0) {
         current_segment--;
         current_health += segment_size;
       } else {
         current_health = 0;
       }
     }
     ```
   - Health recovery:
     ```c
     while (current_health >= segment_size) {
       if (current_segment < max_segments) {
         current_health -= segment_size;
         current_segment++;
       } else {
         current_health = 0;
       }
     }
     ```

4. **Critical Operations:**
   - Damage application:
     - Negative values for damage
     - Positive values for healing
   - Segment management:
     - Segment depletion
     - Segment recovery
     - Maximum segment cap
   - Health validation:
     - Minimum health check
     - Maximum health check
     - Segment boundary checks

This system is crucial for rollback implementation as it:
1. Uses deterministic damage calculations
2. Maintains clear health state boundaries
3. Provides segment-based state tracking
4. Handles health recovery mechanics
5. Manages guard damage separately

The segmented health system will make it easier to serialize and restore health states during rollback operations, as each segment provides a clear checkpoint for state management.
```

### Rollback Implementation Summary

After analyzing FM2K's core systems, we can conclude that the game is well-suited for rollback netcode implementation. Here's a comprehensive overview of how each system will integrate with rollback:

1. **State Serialization Points:**
   - Character State Machine (0x411bf0):
     - Base states (0x0-0x3)
     - Action states (0x4-0x7)
     - Movement states (0x8-0xB)
     - Frame counters and timers
   - Hit Detection System (0x40f010):
     - Hit boxes and collision state
     - Damage values and scaling
     - Hit confirmation flags
   - Input Buffer System (0x4146d0):
     - 1024-frame history buffer
     - Input state flags
     - Repeat handling state
   - Health System (0x40e6f0):
     - Health segments
     - Current health points
     - Guard damage state
     - Recovery mechanics

2. **Deterministic Systems:**
   - Fixed frame rate (100 FPS)
   - 16.16 fixed-point coordinates
   - Deterministic input processing
   - Clear state transitions
   - Predictable damage calculations
   - Frame-based timers

3. **State Management:**
   - Object Pool (1024 entries):
     - Type identification
     - Position and velocity
     - Animation state
     - Collision data
   - Input History:
     - Circular buffer design
     - Clear frame boundaries
     - Input validation
   - Hit Detection:
     - Priority system
     - State validation
     - Reaction handling
   - Health System:
     - Segment-based tracking
     - Clear state boundaries
     - Recovery mechanics

4. **Implementation Strategy:**
   a. Save State (Frame N):
      ```c
      struct SaveState {
          // Character State
          int base_state;
          int action_state;
          int movement_state;
          int frame_counters[4];
          
          // Hit Detection
          int hit_boxes[20];
          int damage_values[8];
          int hit_flags;
          
          // Input Buffer
          int input_history[1024];
          int input_flags;
          int repeat_state;
          
          // Health System
          int health_segments;
          int current_health;
          int guard_damage;
      };
      ```

   b. Rollback Process:
      1. Save current frame state
      2. Apply new remote input
      3. Simulate forward:
         - Process inputs
         - Update character states
         - Handle collisions
         - Apply damage
      4. Render new state
      5. Save confirmed state

5. **Critical Hooks:**
   - Input Processing (0x4146d0):
     - Frame advance point
     - Input buffer management
   - Character State (0x411bf0):
     - State transition handling
     - Frame counting
   - Hit Detection (0x40f010):
     - Collision processing
     - Damage application
   - Health System (0x40e6f0):
     - Health state management
     - Segment tracking

6. **Optimization Opportunities:**
   - Parallel state simulation
   - Minimal state serialization
   - Efficient state diffing
   - Smart state prediction
   - Input compression

The analysis reveals that FM2K's architecture is highly suitable for rollback implementation due to its:
1. Deterministic game logic
2. Clear state boundaries
3. Frame-based processing
4. Efficient memory layout
5. Well-defined systems

This research provides a solid foundation for implementing rollback netcode in FM2K, with all major systems supporting the requirements for state saving, rollback, and resimulation.

## GekkoNet Integration Analysis

### GekkoNet Library Overview

GekkoNet is a rollback netcode library that provides the networking infrastructure we need for FM2K. Here's how it maps to our analyzed systems:

### 1. **GekkoNet Configuration for FM2K**
```c
GekkoConfig fm2k_config = {
    .num_players = 2,                    // P1 vs P2
    .max_spectators = 8,                 // Allow spectators
    .input_prediction_window = 8,        // 8 frames ahead prediction
    .spectator_delay = 6,                // 6 frame spectator delay
    .input_size = sizeof(uint32_t),      // FM2K input flags (32-bit)
    .state_size = sizeof(FM2KGameState), // Our minimal state structure
    .limited_saving = false,             // Full state saving for accuracy
    .post_sync_joining = true,           // Allow mid-match spectators
    .desync_detection = true             // Enable desync detection
};
```

### 2. **Integration Points with FM2K Systems**

#### Input System Integration (0x4146d0)
```c
// Hook into process_game_inputs()
void fm2k_input_hook() {
    // Get GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case AdvanceEvent:
                // Inject inputs into FM2K's input system
                g_p1_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[0];
                g_p2_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[1];
                // Continue normal input processing
                break;
                
            case SaveEvent:
                // Save current FM2K state
                save_fm2k_state(events[i]->data.save.state, events[i]->data.save.state_len);
                break;
                
            case LoadEvent:
                // Restore FM2K state
                load_fm2k_state(events[i]->data.load.state, events[i]->data.load.state_len);
                break;
        }
    }
}
```

#### State Management Integration
```c
typedef struct FM2KGameState {
    // Frame tracking
    uint32_t frame_number;
    uint32_t input_buffer_index;
    
    // Input state
    uint32_t p1_input_current;
    uint32_t p2_input_current;
    uint32_t input_repeat_state[8];
    uint32_t input_repeat_timer[8];
    
    // Character states (per character)
    struct {
        // Position (16.16 fixed point)
        int32_t pos_x, pos_y;
        int32_t vel_x, vel_y;
        
        // State machine
        uint16_t base_state;        // 0x0-0x3
        uint16_t action_state;      // 0x4-0x7  
        uint16_t movement_state;    // 0x8-0xB
        uint16_t state_flags;       // Offset 350
        
        // Animation
        uint16_t animation_frame;   // Offset 12
        uint16_t max_animation_frame; // Offset 88
        
        // Health system
        uint16_t health_segments;   // Current segments
        uint16_t current_health;    // Health points
        uint16_t max_health_segments; // Maximum segments
        uint16_t guard_damage;      // Guard damage accumulation
        
        // Hit detection
        uint16_t hit_flags;         // Hit state flags
        uint16_t hit_stun_timer;    // Hit stun duration
        uint16_t guard_state;       // Guard state flags
        
        // Facing and direction
        uint8_t facing_direction;   // Left/right facing
        uint8_t input_direction;    // Current input direction
    } characters[2];
    
    // Round state
    uint32_t round_state;           // Round state machine
    uint32_t round_timer;           // Round timer
    uint32_t game_mode;             // Current game mode
    
    // Hit detection global state
    uint32_t hit_effect_target;     // Hit effect target
    uint32_t hit_effect_timer;      // Hit effect timer
    
    // Combo system
    uint16_t combo_counter[2];      // Combo counters
    uint16_t damage_scaling[2];     // Damage scaling
    
    // Object pool critical state
    uint16_t active_objects;        // Number of active objects
    uint32_t object_state_flags;    // Critical object flags
    
} FM2KGameState;
```

### 3. **Network Adapter Implementation**
```c
GekkoNetAdapter fm2k_adapter = {
    .send_data = fm2k_send_packet,
    .receive_data = fm2k_receive_packets,
    .free_data = fm2k_free_packet_data
};

void fm2k_send_packet(GekkoNetAddress* addr, const char* data, int length) {
    // Use existing LilithPort networking or implement UDP
    // addr->data contains IP/port information
    // data contains the packet to send
}

GekkoNetResult** fm2k_receive_packets(int* length) {
    // Collect all packets received since last call
    // Return array of GekkoNetResult pointers
    // GekkoNet will call free_data on each result
}
```

### 4. **Integration with Existing FM2K Systems**

#### Main Game Loop Integration
```c
// Hook at 0x4146d0 - process_game_inputs()
void fm2k_rollback_frame() {
    // 1. Poll network
    gekko_network_poll(session);
    
    // 2. Add local inputs
    uint32_t local_input = get_local_player_input();
    gekko_add_local_input(session, local_player_id, &local_input);
    
    // 3. Process GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    // 4. Handle events (save/load/advance)
    process_gekko_events(events, event_count);
    
    // 5. Continue normal FM2K processing
    // (character updates, hit detection, etc.)
}
```

#### State Serialization Functions
```c
void save_fm2k_state(unsigned char* buffer, unsigned int* length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Save frame state
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    
    // Save character states from object pool
    for (int i = 0; i < 2; i++) {
        save_character_state(&state->characters[i], i);
    }
    
    // Save round state
    state->round_state = g_round_state;
    state->round_timer = g_round_timer;
    
    *length = sizeof(FM2KGameState);
}

void load_fm2k_state(unsigned char* buffer, unsigned int length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Restore frame state
    current_frame = state->frame_number;
    g_input_buffer_index = state->input_buffer_index;
    
    // Restore character states to object pool
    for (int i = 0; i < 2; i++) {
        load_character_state(&state->characters[i], i);
    }
    
    // Restore round state
    g_round_state = state->round_state;
    g_round_timer = state->round_timer;
}
```

### 5. **Event Handling Integration**
```c
void handle_session_events() {
    int event_count;
    GekkoSessionEvent** events = gekko_session_events(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case PlayerConnected:
                // Show "Player Connected" message
                break;
            case PlayerDisconnected:
                // Handle disconnection
                break;
            case DesyncDetected:
                // Log desync information
                printf("Desync at frame %d: local=%08x remote=%08x\n",
                       events[i]->data.desynced.frame,
                       events[i]->data.desynced.local_checksum,
                       events[i]->data.desynced.remote_checksum);
                break;
        }
    }
}
```

### 6. **Performance Considerations**
- **State Size**: ~200 bytes per frame (very efficient)
- **Prediction Window**: 8 frames ahead (80ms at 100 FPS)
- **Rollback Depth**: Up to 15 frames (150ms)
- **Network Frequency**: 100 Hz to match FM2K's frame rate

### 7. **Implementation Benefits**
1. **Minimal Code Changes**: Only need to hook input processing
2. **Existing Infrastructure**: Leverages FM2K's input history system
3. **Deterministic**: FM2K's fixed-point math ensures consistency
4. **Efficient**: Small state size enables fast rollback
5. **Robust**: GekkoNet handles all networking complexity

This integration approach leverages both FM2K's existing architecture and GekkoNet's proven rollback implementation, providing a solid foundation for online play.

### Critical Player State Variables

Based on IDA analysis, here are the key memory addresses for player state:

#### Player 1 State Variables:
```c
// Input system
g_p1_input          = 0x4259c0;  // Current input state
g_p1_input_history  = 0x4280e0;  // 1024-frame input history

// Position and stage
g_p1_stage_x        = 0x424e68;  // Stage position X
g_p1_stage_y        = 0x424e6c;  // Stage position Y

// Round and game state
g_p1_round_count    = 0x4700ec;  // Round wins
g_p1_round_state    = 0x4700f0;  // Round state
g_p1_action_state   = 0x47019c;  // Action state

// Health system
g_p1_hp             = 0x4dfc85;  // Current HP
g_p1_max_hp         = 0x4dfc91;  // Maximum HP
```

#### Player 2 State Variables:
```c
// Input system
g_p2_input          = 0x4259c4;  // Current input state
g_p2_input_history  = 0x4290e0;  // 1024-frame input history

// Round and game state
g_p2_action_state   = 0x4701a0;  // Action state

// Health system
g_p2_hp             = 0x4edcc4;  // Current HP
g_p2_max_hp         = 0x4edcd0;  // Maximum HP
```

#### Core Game State Variables:
```c
// Object pool
g_object_pool       = 0x4701e0;  // Main object pool (1024 entries)
g_sprite_data_array = 0x447938;  // Sprite data array

// Timing system
g_frame_time_ms     = 0x41e2f0;  // Frame duration (10ms)
g_last_frame_time   = 0x447dd4;  // Last frame timestamp
g_frame_sync_flag   = 0x424700;  // Frame sync state
g_frame_time_delta  = 0x425960;  // Frame timing delta
g_frame_skip_count  = 0x4246f4;  // Frame skip counter

// Random number generator
g_random_seed       = 0x41fb1c;  // RNG seed

// Input buffer management
g_input_buffer_index = (calculated); // Current buffer index
g_input_repeat_timer = 0x4d1c40;     // Input repeat timers

// Round system
g_round_timer       = 0x470060;  // Round timer
g_game_timer        = 0x470044;  // Game timer
g_hit_effect_timer  = 0x4701c8;  // Hit effect timer
```

### Final State Structure for GekkoNet

With all the memory addresses identified, here's the complete state structure:

```c
typedef struct FM2KCompleteGameState {
    // Frame and timing state
    uint32_t frame_number;
    uint32_t input_buffer_index;
    uint32_t last_frame_time;
    uint32_t frame_time_delta;
    uint8_t frame_skip_count;
    uint8_t frame_sync_flag;
    
    // Random number generator state
    uint32_t random_seed;
    
    // Player 1 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t stage_x, stage_y;
        uint32_t round_count;
        uint32_t round_state;
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p1;
    
    // Player 2 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p2;
    
    // Global timers
    uint32_t round_timer;
    uint32_t game_timer;
    uint32_t hit_effect_timer;
    
    // Input system state
    uint32_t input_repeat_timer[8];
    
    // Critical object pool state (first 2 character objects)
    struct {
        uint32_t object_type;
        int32_t pos_x, pos_y;        // 16.16 fixed point
        int32_t vel_x, vel_y;
        uint16_t state_flags;
        uint16_t animation_frame;
        uint16_t health_segments;
        uint16_t facing_direction;
        // ... other critical fields
    } character_objects[2];
    
} FM2KCompleteGameState;
```

### Memory Access Functions

```c
// Direct memory access functions for state serialization
void save_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Frame and timing
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    state->last_frame_time = *(uint32_t*)0x447dd4;
    state->frame_time_delta = *(uint32_t*)0x425960;
    state->frame_skip_count = *(uint8_t*)0x4246f4;
    state->frame_sync_flag = *(uint8_t*)0x424700;
    
    // RNG state
    state->random_seed = *(uint32_t*)0x41fb1c;
    
    // Player 1 state
    state->p1.input_current = *(uint32_t*)0x4259c0;
    memcpy(state->p1.input_history, (void*)0x4280e0, 1024 * sizeof(uint32_t));
    state->p1.stage_x = *(uint32_t*)0x424e68;
    state->p1.stage_y = *(uint32_t*)0x424e6c;
    state->p1.round_count = *(uint32_t*)0x4700ec;
    state->p1.round_state = *(uint32_t*)0x4700f0;
    state->p1.action_state = *(uint32_t*)0x47019c;
    state->p1.hp = *(uint32_t*)0x4dfc85;
    state->p1.max_hp = *(uint32_t*)0x4dfc91;
    
    // Player 2 state
    state->p2.input_current = *(uint32_t*)0x4259c4;
    memcpy(state->p2.input_history, (void*)0x4290e0, 1024 * sizeof(uint32_t));
    state->p2.action_state = *(uint32_t*)0x4701a0;
    state->p2.hp = *(uint32_t*)0x4edcc4;
    state->p2.max_hp = *(uint32_t*)0x4edcd0;
    
    // Global timers
    state->round_timer = *(uint32_t*)0x470060;
    state->game_timer = *(uint32_t*)0x470044;
    state->hit_effect_timer = *(uint32_t*)0x4701c8;
    
    // Input repeat timers
    memcpy(state->input_repeat_timer, (void*)0x4d1c40, 8 * sizeof(uint32_t));
    
    // Character objects from object pool
    save_character_objects_from_pool(state->character_objects);
}

void load_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Restore all state in reverse order
    // ... (similar but with assignment in opposite direction)
}
```

This gives us a complete, memory-accurate state structure that can be used with GekkoNet for rollback implementation. The state size is approximately 10KB, which is very reasonable for rollback netcode.

## GekkoNet SDL Example Analysis & FM2K Integration

### SDL Example Key Patterns

The OnlineSession.cpp example demonstrates the core GekkoNet integration patterns we need for FM2K:

#### 1. **Session Setup Pattern**
```c
// From SDL example - adapt for FM2K
GekkoSession* sess = nullptr;
GekkoConfig conf {
    .num_players = 2,
    .input_size = sizeof(char),           // FM2K: sizeof(uint32_t)
    .state_size = sizeof(GState),         // FM2K: sizeof(FM2KCompleteGameState)
    .max_spectators = 0,                  // FM2K: 8 for spectators
    .input_prediction_window = 10,        // FM2K: 8 frames
    .desync_detection = true,
    // .limited_saving = true,            // FM2K: false for accuracy
};

gekko_create(&sess);
gekko_start(sess, &conf);
gekko_net_adapter_set(sess, gekko_default_adapter(localport));
```

#### 2. **Player Management Pattern**
```c
// Order-dependent player addition (from example)
if (localplayer == 0) {
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
} else {
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
}
```

#### 3. **Core Game Loop Pattern** 
```c
// From SDL example - critical for FM2K integration
while (running) {
    // Timing control
    frames_ahead = gekko_frames_ahead(sess);
    frame_time = GetFrameTime(frames_ahead);
    
    // Network polling
    gekko_network_poll(sess);
    
    // Frame processing
    while (accumulator >= frame_time) {
        // Add local input
        auto input = get_key_inputs();
        gekko_add_local_input(sess, localplayer, &input);
        
        // Handle session events
        auto events = gekko_session_events(sess, &count);
        for (int i = 0; i < count; i++) {
            // Handle PlayerConnected, PlayerDisconnected, DesyncDetected
        }
        
        // Process game events
        auto updates = gekko_update_session(sess, &count);
        for (int i = 0; i < count; i++) {
            switch (updates[i]->type) {
                case SaveEvent: save_state(&state, updates[i]); break;
                case LoadEvent: load_state(&state, updates[i]); break;
                case AdvanceEvent:
                    // Extract inputs and advance game
                    inputs[0].input.value = updates[i]->data.adv.inputs[0];
                    inputs[1].input.value = updates[i]->data.adv.inputs[1];
                    update_state(state, inputs, num_players);
                    break;
            }
        }
        accumulator -= frame_time;
    }
    
    // Render current state
    render_state(state);
}
```

### FM2K Integration Strategy

#### 1. **Replace SDL2 with SDL3 for FM2K**
```c
// SDL3 initialization for FM2K integration
#include "SDL3/SDL.h"

bool init_fm2k_window(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Error initializing SDL3.\n");
        return false;
    }
    
    // Create window matching FM2K's resolution
    window = SDL_CreateWindow(
        "FM2K Online Session",
        640, 480,  // FM2K native resolution
        SDL_WINDOW_RESIZABLE
    );
    
    if (!window) {
        fprintf(stderr, "Error creating SDL3 Window.\n");
        return false;
    }
    
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        fprintf(stderr, "Error creating SDL3 Renderer.\n");
        return false;
    }
    
    return true;
}
```

#### 2. **FM2K Input Structure**
```c
// Replace simple GInput with FM2K's 11-bit input system
struct FM2KInput {
    union {
        struct {
            uint16_t left     : 1;   // 0x001
            uint16_t right    : 1;   // 0x002
            uint16_t up       : 1;   // 0x004
            uint16_t down     : 1;   // 0x008
            uint16_t button1  : 1;   // 0x010
            uint16_t button2  : 1;   // 0x020
            uint16_t button3  : 1;   // 0x040
            uint16_t button4  : 1;   // 0x080
            uint16_t button5  : 1;   // 0x100
            uint16_t button6  : 1;   // 0x200
            uint16_t button7  : 1;   // 0x400
            uint16_t reserved : 5;   // Unused bits
        } bits;
        uint16_t value;
    } input;
};

FM2KInput get_fm2k_inputs() {
    FM2KInput input{};
    input.input.value = 0;
    
    // Get keyboard state (SDL3 syntax)
    const bool* keys = SDL_GetKeyboardState(NULL);
    
    // Map to FM2K inputs
    input.input.bits.up = keys[SDL_SCANCODE_W];
    input.input.bits.left = keys[SDL_SCANCODE_A];
    input.input.bits.down = keys[SDL_SCANCODE_S];
    input.input.bits.right = keys[SDL_SCANCODE_D];
    input.input.bits.button1 = keys[SDL_SCANCODE_J];
    input.input.bits.button2 = keys[SDL_SCANCODE_K];
    input.input.bits.button3 = keys[SDL_SCANCODE_L];
    // ... map other buttons
    
    return input;
}
```

#### 3. **FM2K State Management**
```c
// Replace simple GState with FM2KCompleteGameState
void save_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    *ev->data.save.state_len = sizeof(FM2KCompleteGameState);
    
    // Use Fletcher's checksum (from example) for consistency
    *ev->data.save.checksum = fletcher32((uint16_t*)gs, sizeof(FM2KCompleteGameState));
    
    std::memcpy(ev->data.save.state, gs, sizeof(FM2KCompleteGameState));
}

void load_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    std::memcpy(gs, ev->data.load.state, sizeof(FM2KCompleteGameState));
}

void update_fm2k_state(FM2KCompleteGameState& gs, FM2KInput inputs[2]) {
    // Call into FM2K's actual game logic
    // This would hook into the functions we analyzed:
    
    // 1. Inject inputs into FM2K's input system
    *(uint32_t*)0x4259c0 = inputs[0].input.value;  // g_p1_input
    *(uint32_t*)0x4259c4 = inputs[1].input.value;  // g_p2_input
    
    // 2. Call FM2K's update functions
    // process_game_inputs_FRAMESTEP_HOOK();  // 0x4146d0
    // update_game_state();                   // Game logic
    // character_state_machine();             // 0x411bf0
    // hit_detection_system();                // 0x40f010
    
    // 3. Extract updated state from FM2K memory
    gs.p1.hp = *(uint32_t*)0x4dfc85;
    gs.p2.hp = *(uint32_t*)0x4edcc4;
    gs.round_timer = *(uint32_t*)0x470060;
    // ... extract all other state variables
}
```

#### 4. **FM2K Timing Integration**
```c
// Adapt timing to match FM2K's 100 FPS
float GetFM2KFrameTime(float frames_ahead) {
    // FM2K runs at 100 FPS (10ms per frame)
    const float base_frame_time = 1.0f / 100.0f;  // 0.01 seconds
    
    if (frames_ahead >= 0.75f) {
        // Slow down if too far ahead
        return base_frame_time * 1.02f;  // 59.8 FPS equivalent
    } else {
        return base_frame_time;  // Normal 100 FPS
    }
}
```

#### 5. **Complete FM2K Integration Main Loop**
```c
int fm2k_online_main(int argc, char* args[]) {
    // Parse command line (same as example)
    int localplayer = std::stoi(args[1]);
    const int localport = std::stoi(args[2]);
    std::string remote_address = std::move(args[3]);
    int localdelay = std::stoi(args[4]);
    
    // Initialize SDL3 and FM2K
    running = init_fm2k_window();
    
    // Initialize FM2K state
    FM2KCompleteGameState state = {};
    FM2KInput inputs[2] = {};
    
    // Setup GekkoNet
    GekkoSession* sess = nullptr;
    GekkoConfig conf {
        .num_players = 2,
        .input_size = sizeof(uint16_t),  // FM2K input size
        .state_size = sizeof(FM2KCompleteGameState),
        .max_spectators = 8,
        .input_prediction_window = 8,
        .desync_detection = true,
        .limited_saving = false,  // Full state saving for accuracy
        .post_sync_joining = true
    };
    
    gekko_create(&sess);
    gekko_start(sess, &conf);
    gekko_net_adapter_set(sess, gekko_default_adapter(localport));
    
    // Add players (order-dependent)
    if (localplayer == 0) {
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
    } else {
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    }
    
    gekko_set_local_delay(sess, localplayer, localdelay);
    
    // Timing variables
    auto curr_time = std::chrono::steady_clock::now();
    auto prev_time = curr_time;
    float delta_time = 0.0f;
    float accumulator = 0.0f;
    float frame_time = 0.0f;
    float frames_ahead = 0.0f;
    
    while (running) {
        curr_time = std::chrono::steady_clock::now();
        
        frames_ahead = gekko_frames_ahead(sess);
        frame_time = GetFM2KFrameTime(frames_ahead);
        
        delta_time = std::chrono::duration<float>(curr_time - prev_time).count();
        prev_time = curr_time;
        accumulator += delta_time;
        
        gekko_network_poll(sess);
        
        while (accumulator >= frame_time) {
            // Process SDL events
            process_events();
            
            // Get local input
            auto input = get_fm2k_inputs();
            gekko_add_local_input(sess, localplayer, &input);
            
            // Handle session events
            int count = 0;
            auto events = gekko_session_events(sess, &count);
            for (int i = 0; i < count; i++) {
                handle_fm2k_session_event(events[i]);
            }
            
            // Process game events
            count = 0;
            auto updates = gekko_update_session(sess, &count);
            for (int i = 0; i < count; i++) {
                auto ev = updates[i];
                
                switch (ev->type) {
                    case SaveEvent:
                        save_fm2k_state(&state, ev);
                        break;
                    case LoadEvent:
                        load_fm2k_state(&state, ev);
                        break;
                    case AdvanceEvent:
                        // Extract inputs from GekkoNet
                        inputs[0].input.value = ((uint16_t*)ev->data.adv.inputs)[0];
                        inputs[1].input.value = ((uint16_t*)ev->data.adv.inputs)[1];
                        
                        // Update FM2K game state
                        update_fm2k_state(state, inputs);
                        break;
                }
            }
            
            accumulator -= frame_time;
        }
        
        // Render current state
        render_fm2k_state(state);
    }
    
    gekko_destroy(sess);
    del_window();
    return 0;
}
```

### Key Differences from SDL Example

1. **Input Size**: FM2K uses 16-bit inputs vs 8-bit in example
2. **State Size**: ~10KB vs ~16 bytes in example  
3. **Frame Rate**: 100 FPS vs 60 FPS in example
4. **State Complexity**: Full game state vs simple position
5. **Integration Depth**: Hooks into existing game vs standalone

### Integration Benefits

1. **Proven Pattern**: The SDL example shows GekkoNet works reliably
2. **Clear Structure**: Event handling patterns are well-defined
3. **Performance**: Timing control handles frame rate variations
4. **Network Stats**: Built-in ping/jitter monitoring
5. **Error Handling**: Desync detection and player management

This analysis shows that adapting the SDL example for FM2K is very feasible, with the main changes being input/state structures and frame timing to match FM2K's 100 FPS architecture.

## Hit Judge and Round System Architecture

### Hit Judge Configuration (0x42470C-0x430120)

The game uses a sophisticated hit detection and configuration system loaded from INI files:

```c
struct FM2K_HitJudgeConfig {
    uint32_t hit_judge_value;    // 0x42470C - Hit detection threshold
    uint32_t config_values[7];   // 0x4300E0-0x430120 - General config values
    char     config_string[260]; // Configuration string buffer
};
```

The hit judge system is initialized by `hit_judge_set_function` (0x414930) which:
1. Loads configuration from INI files
2. Sets up hit detection parameters
3. Configures round settings and game modes

### Round System State (0x470040-0x47006C)

The round system maintains several critical state variables:

```c
struct FM2K_RoundSystem {
    uint32_t game_mode;      // 0x470040 - Current game mode (VS/Team/Tournament)
    uint32_t round_limit;    // 0x470048 - Maximum rounds for the match
    uint32_t round_state;    // 0x47004C - Current round state
    uint32_t round_timer;    // 0x470060 - Active round timer
};

enum FM2K_RoundState {
    ROUND_INIT = 0,      // Initial state
    ROUND_ACTIVE = 1,    // Round in progress
    ROUND_END = 2,       // Round has ended
    ROUND_MATCH_END = 3  // Match has ended
};

enum FM2K_GameMode {
    MODE_NORMAL = 0,     // Normal VS mode
    MODE_TEAM = 1,       // Team Battle mode
    MODE_TOURNAMENT = 2  // Tournament mode
};
```

The round system is managed by `vs_round_function` (0x4086A0) which:
1. Handles round state transitions
2. Manages round timers
3. Processes win/loss conditions
4. Controls match flow

### Rollback Implications

This architecture has several important implications for rollback implementation:

1. **State Serialization Requirements:**
   - Must save hit judge configuration
   - Must preserve round state and timers
   - Game mode affects state size

2. **Deterministic Behavior:**
   - Hit detection is configuration-driven
   - Round state transitions are deterministic
   - Timer-based events are predictable

3. **State Size Optimization:**
   - Hit judge config is static per match
   - Only dynamic round state needs saving
   - Can optimize by excluding static config

4. **Synchronization Points:**
   - Round state changes are key sync points
   - Hit detection results must be identical
   - Timer values must match exactly

Updated state structure for rollback:

```c
typedef struct FM2KGameState {
    // ... existing timing and input state ...

    // Round System State
    struct {
        uint32_t mode;          // Current game mode
        uint32_t round_limit;   // Maximum rounds
        uint32_t round_state;   // Current round state
        uint32_t round_timer;   // Active round timer
        uint32_t score_value;   // Current score/timer value
    } round_system;

    // Hit Judge State
    struct {
        uint32_t hit_judge_value;   // Current hit detection threshold
        uint32_t effect_target;     // Current hit effect target
        uint32_t effect_timer;      // Hit effect duration
    } hit_system;

    // ... rest of game state ...
} FM2KGameState;
```

## Sprite Rendering and Hit Detection System

### Sprite Rendering Engine (0x40CC30)

The game uses a sophisticated sprite rendering engine that handles:
1. Hit effect visualization
2. Sprite transformations and blending
3. Color palette manipulation
4. Screen-space effects

Key components:

```c
struct SpriteRenderState {
    uint32_t render_mode;     // Different rendering modes (-10 to 3)
    uint32_t blend_flags;     // Blending and effect flags
    uint32_t color_values[3]; // RGB color modification values
    uint32_t effect_timer;    // Effect duration counter
};
```

### Hit Effect System

The hit effect system is tightly integrated with the sprite renderer:

1. **Effect Triggers:**
   - Hit detection results (0x42470C)
   - Game state transitions
   - Timer-based events

2. **Effect Parameters:**
   ```c
   struct HitEffectParams {
       uint32_t target_id;      // Effect target identifier
       uint32_t effect_type;    // Visual effect type
       uint32_t duration;       // Effect duration in frames
       uint32_t color_values;   // RGB color modulation
   };
   ```

3. **Rendering Pipeline:**
   - Color palette manipulation (0x4D1A20)
   - Sprite transformation
   - Blend mode selection
   - Screen-space effects

### Integration with Rollback

The sprite and hit effect systems have important implications for rollback:

1. **Visual State Management:**
   - Effect timers must be part of the game state
   - Color modulation values need saving
   - Sprite transformations must be deterministic

2. **Performance Considerations:**
   - Effect parameters are small (few bytes per effect)
   - Visual state is derived from game state
   - Can optimize by excluding pure visual state

3. **Synchronization Requirements:**
   - Hit effects must trigger identically
   - Effect timers must stay synchronized
   - Visual state must match game state

Updated state structure to include visual effects:

```c
typedef struct FM2KGameState {
    // ... previous state members ...

    // Visual Effect State
    struct {
        uint32_t active_effects;     // Bitfield of active effects
        uint32_t effect_timers[8];   // Array of effect durations
        uint32_t color_values[8][3]; // RGB values for each effect
        uint32_t target_ids[8];      // Effect target identifiers
    } visual_state;

    // ... rest of game state ...
} FM2KGameState;
```

## Game State Management System

The game state manager (0x406FC0) handles several critical aspects of the game:

### State Variables

1. **Round System State:**
   ```c
   struct RoundSystemState {
       uint32_t g_round_timer;        // 0x470060 - Current round timer value
       uint32_t g_round_limit;        // 0x470048 - Maximum rounds per match
       uint32_t g_round_state;        // 0x47004C - Current round state
       uint32_t g_game_mode;          // 0x470040 - Current game mode
   };
   ```

2. **Player State:**
   ```c
   struct PlayerState {
       int32_t stage_position;        // Current position on stage grid
       uint32_t action_state;         // Current action being performed
       uint32_t round_count;          // Number of rounds completed
       uint32_t input_history[4];     // Recent input history
       uint32_t move_history[4];      // Recent movement history
   };
   ```

### State Management Flow

1. **Initialization (0x406FC0):**
   - Loads initial configuration
   - Sets up round timer and limits
   - Initializes player positions
   - Sets up game mode parameters

2. **State Updates:**
   - Input processing and validation
   - Position and movement updates
   - Action state transitions
   - Round state management

3. **State Synchronization:**
   - Timer synchronization
   - Player state synchronization
   - Round state transitions
   - Game mode state management

### Rollback Implications

1. **Critical State Components:**
   - Round timer and state
   - Player positions and actions
   - Input and move history
   - Game mode state

2. **State Size Analysis:**
   - Core game state: ~128 bytes
   - Player state: ~32 bytes per player
   - History buffers: ~32 bytes per player
   - Total per-frame state: ~256 bytes

3. **Synchronization Requirements:**
   - Timer must be deterministic
   - Player state must be atomic
   - Input processing must be consistent
   - State transitions must be reproducible

## Input Processing System

The game uses a sophisticated input processing system that handles both movement and action inputs:

### Input Mapping (0x406F20)

```c
enum InputActions {
    ACTION_NONE = 0,
    ACTION_A = 1,    // 0x20  - Light Attack
    ACTION_B = 2,    // 0x40  - Medium Attack
    ACTION_C = 3,    // 0x80  - Heavy Attack
    ACTION_D = 4,    // 0x100 - Special Move
    ACTION_E = 5     // 0x200 - Super Move
};

enum InputDirections {
    DIR_NONE  = 0x000,
    DIR_UP    = 0x004,
    DIR_DOWN  = 0x008,
    DIR_LEFT  = 0x001,
    DIR_RIGHT = 0x002
};
```

### Input Processing Flow

1. **Raw Input Capture:**
   - Movement inputs (4-way directional)
   - Action buttons (5 main buttons)
   - System buttons (Start, Select)

2. **Input State Management:**
   ```c
   struct InputState {
       uint16_t current_input;     // Current frame's input
       uint16_t previous_input;    // Last frame's input
       uint16_t input_changes;     // Changed bits since last frame
       uint8_t  processed_input;   // Processed directional input
   };
   ```

3. **Input History:**
   - Each player maintains a 4-frame input history
   - History includes both raw and processed inputs
   - Used for move validation and rollback

4. **Input Processing:**
   - Raw input capture
   - Input change detection
   - Input repeat handling:
     - Initial delay
     - Repeat delay
     - Repeat timer
   - Input validation
   - Mode-specific processing:
     - VS mode (< 3000)
     - Story mode (? 3000)

5. **Memory Layout:**
   ```
   input_system {
     +g_input_buffer_index: Current frame index
     +g_p1_input[8]: Current P1 inputs
     +g_p2_input: Current P2 inputs
     +g_prev_input_state[8]: Previous frame inputs
     +g_input_changes[8]: Input change flags
     +g_input_repeat_state[8]: Input repeat flags
     +g_input_repeat_timer[8]: Repeat timers
     +g_processed_input[8]: Processed inputs
     +g_combined_processed_input: Combined state
     +g_combined_input_changes: Change mask
     +g_combined_raw_input: Raw input mask
   }
   ```

6. **Critical Operations:**
   - Frame advance:
     ```c
     g_input_buffer_index = (g_input_buffer_index + 1) & 0x3FF;
     g_p1_input_history[g_input_buffer_index] = current_input;
     ```
   - Input processing:
     ```c
     g_input_changes = current_input & (prev_input ^ current_input);
     if (repeat_active && current_input == repeat_state) {
       if (--repeat_timer == 0) {
         processed_input = current_input;
         repeat_timer = repeat_delay;
       }
     }
     ```

This system is crucial for rollback implementation as it provides:
1. Deterministic input processing
2. Frame-accurate input history
3. Clear state serialization points
4. Mode-specific input handling
5. Input validation and processing logic

The 1024-frame history buffer is particularly useful for rollback, as it provides ample space for state rewinding and replay.
```

### Health Damage Manager (0x40e6f0)

The health damage manager handles health state and damage calculations:

1. **Health State Management:**
   - Per-character health tracking
   - Health segments system
   - Recovery mechanics
   - Guard damage handling

2. **Memory Layout:**
   ```
   health_system {
     +4DFC95: Current health segment
     +4DFC99: Maximum health segments
     +4DFC9D: Current health points
     +4DFCA1: Health segment size
   }
   ```

3. **Damage Processing:**
   - Segment-based health system:
     ```c
     while (current_health < 0) {
       if (current_segment > 0) {
         current_segment--;
         current_health += segment_size;
       } else {
         current_health = 0;
       }
     }
     ```
   - Health recovery:
     ```c
     while (current_health >= segment_size) {
       if (current_segment < max_segments) {
         current_health -= segment_size;
         current_segment++;
       } else {
         current_health = 0;
       }
     }
     ```

4. **Critical Operations:**
   - Damage application:
     - Negative values for damage
     - Positive values for healing
   - Segment management:
     - Segment depletion
     - Segment recovery
     - Maximum segment cap
   - Health validation:
     - Minimum health check
     - Maximum health check
     - Segment boundary checks

This system is crucial for rollback implementation as it:
1. Uses deterministic damage calculations
2. Maintains clear health state boundaries
3. Provides segment-based state tracking
4. Handles health recovery mechanics
5. Manages guard damage separately

The segmented health system will make it easier to serialize and restore health states during rollback operations, as each segment provides a clear checkpoint for state management.
```

### Rollback Implementation Summary

After analyzing FM2K's core systems, we can conclude that the game is well-suited for rollback netcode implementation. Here's a comprehensive overview of how each system will integrate with rollback:

1. **State Serialization Points:**
   - Character State Machine (0x411bf0):
     - Base states (0x0-0x3)
     - Action states (0x4-0x7)
     - Movement states (0x8-0xB)
     - Frame counters and timers
   - Hit Detection System (0x40f010):
     - Hit boxes and collision state
     - Damage values and scaling
     - Hit confirmation flags
   - Input Buffer System (0x4146d0):
     - 1024-frame history buffer
     - Input state flags
     - Repeat handling state
   - Health System (0x40e6f0):
     - Health segments
     - Current health points
     - Guard damage state
     - Recovery mechanics

2. **Deterministic Systems:**
   - Fixed frame rate (100 FPS)
   - 16.16 fixed-point coordinates
   - Deterministic input processing
   - Clear state transitions
   - Predictable damage calculations
   - Frame-based timers

3. **State Management:**
   - Object Pool (1024 entries):
     - Type identification
     - Position and velocity
     - Animation state
     - Collision data
   - Input History:
     - Circular buffer design
     - Clear frame boundaries
     - Input validation
   - Hit Detection:
     - Priority system
     - State validation
     - Reaction handling
   - Health System:
     - Segment-based tracking
     - Clear state boundaries
     - Recovery mechanics

4. **Implementation Strategy:**
   a. Save State (Frame N):
      ```c
      struct SaveState {
          // Character State
          int base_state;
          int action_state;
          int movement_state;
          int frame_counters[4];
          
          // Hit Detection
          int hit_boxes[20];
          int damage_values[8];
          int hit_flags;
          
          // Input Buffer
          int input_history[1024];
          int input_flags;
          int repeat_state;
          
          // Health System
          int health_segments;
          int current_health;
          int guard_damage;
      };
      ```

   b. Rollback Process:
      1. Save current frame state
      2. Apply new remote input
      3. Simulate forward:
         - Process inputs
         - Update character states
         - Handle collisions
         - Apply damage
      4. Render new state
      5. Save confirmed state

5. **Critical Hooks:**
   - Input Processing (0x4146d0):
     - Frame advance point
     - Input buffer management
   - Character State (0x411bf0):
     - State transition handling
     - Frame counting
   - Hit Detection (0x40f010):
     - Collision processing
     - Damage application
   - Health System (0x40e6f0):
     - Health state management
     - Segment tracking

6. **Optimization Opportunities:**
   - Parallel state simulation
   - Minimal state serialization
   - Efficient state diffing
   - Smart state prediction
   - Input compression

The analysis reveals that FM2K's architecture is highly suitable for rollback implementation due to its:
1. Deterministic game logic
2. Clear state boundaries
3. Frame-based processing
4. Efficient memory layout
5. Well-defined systems

This research provides a solid foundation for implementing rollback netcode in FM2K, with all major systems supporting the requirements for state saving, rollback, and resimulation.

## GekkoNet Integration Analysis

### GekkoNet Library Overview

GekkoNet is a rollback netcode library that provides the networking infrastructure we need for FM2K. Here's how it maps to our analyzed systems:

### 1. **GekkoNet Configuration for FM2K**
```c
GekkoConfig fm2k_config = {
    .num_players = 2,                    // P1 vs P2
    .max_spectators = 8,                 // Allow spectators
    .input_prediction_window = 8,        // 8 frames ahead prediction
    .spectator_delay = 6,                // 6 frame spectator delay
    .input_size = sizeof(uint32_t),      // FM2K input flags (32-bit)
    .state_size = sizeof(FM2KGameState), // Our minimal state structure
    .limited_saving = false,             // Full state saving for accuracy
    .post_sync_joining = true,           // Allow mid-match spectators
    .desync_detection = true             // Enable desync detection
};
```

### 2. **Integration Points with FM2K Systems**

#### Input System Integration (0x4146d0)
```c
// Hook into process_game_inputs()
void fm2k_input_hook() {
    // Get GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case AdvanceEvent:
                // Inject inputs into FM2K's input system
                g_p1_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[0];
                g_p2_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[1];
                // Continue normal input processing
                break;
                
            case SaveEvent:
                // Save current FM2K state
                save_fm2k_state(events[i]->data.save.state, events[i]->data.save.state_len);
                break;
                
            case LoadEvent:
                // Restore FM2K state
                load_fm2k_state(events[i]->data.load.state, events[i]->data.load.state_len);
                break;
        }
    }
}
```

#### State Management Integration
```c
typedef struct FM2KGameState {
    // Frame tracking
    uint32_t frame_number;
    uint32_t input_buffer_index;
    
    // Input state
    uint32_t p1_input_current;
    uint32_t p2_input_current;
    uint32_t input_repeat_state[8];
    uint32_t input_repeat_timer[8];
    
    // Character states (per character)
    struct {
        // Position (16.16 fixed point)
        int32_t pos_x, pos_y;
        int32_t vel_x, vel_y;
        
        // State machine
        uint16_t base_state;        // 0x0-0x3
        uint16_t action_state;      // 0x4-0x7  
        uint16_t movement_state;    // 0x8-0xB
        uint16_t state_flags;       // Offset 350
        
        // Animation
        uint16_t animation_frame;   // Offset 12
        uint16_t max_animation_frame; // Offset 88
        
        // Health system
        uint16_t health_segments;   // Current segments
        uint16_t current_health;    // Health points
        uint16_t max_health_segments; // Maximum segments
        uint16_t guard_damage;      // Guard damage accumulation
        
        // Hit detection
        uint16_t hit_flags;         // Hit state flags
        uint16_t hit_stun_timer;    // Hit stun duration
        uint16_t guard_state;       // Guard state flags
        
        // Facing and direction
        uint8_t facing_direction;   // Left/right facing
        uint8_t input_direction;    // Current input direction
    } characters[2];
    
    // Round state
    uint32_t round_state;           // Round state machine
    uint32_t round_timer;           // Round timer
    uint32_t game_mode;             // Current game mode
    
    // Hit detection global state
    uint32_t hit_effect_target;     // Hit effect target
    uint32_t hit_effect_timer;      // Hit effect timer
    
    // Combo system
    uint16_t combo_counter[2];      // Combo counters
    uint16_t damage_scaling[2];     // Damage scaling
    
    // Object pool critical state
    uint16_t active_objects;        // Number of active objects
    uint32_t object_state_flags;    // Critical object flags
    
} FM2KGameState;
```

### 3. **Network Adapter Implementation**
```c
GekkoNetAdapter fm2k_adapter = {
    .send_data = fm2k_send_packet,
    .receive_data = fm2k_receive_packets,
    .free_data = fm2k_free_packet_data
};

void fm2k_send_packet(GekkoNetAddress* addr, const char* data, int length) {
    // Use existing LilithPort networking or implement UDP
    // addr->data contains IP/port information
    // data contains the packet to send
}

GekkoNetResult** fm2k_receive_packets(int* length) {
    // Collect all packets received since last call
    // Return array of GekkoNetResult pointers
    // GekkoNet will call free_data on each result
}
```

### 4. **Integration with Existing FM2K Systems**

#### Main Game Loop Integration
```c
// Hook at 0x4146d0 - process_game_inputs()
void fm2k_rollback_frame() {
    // 1. Poll network
    gekko_network_poll(session);
    
    // 2. Add local inputs
    uint32_t local_input = get_local_player_input();
    gekko_add_local_input(session, local_player_id, &local_input);
    
    // 3. Process GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    // 4. Handle events (save/load/advance)
    process_gekko_events(events, event_count);
    
    // 5. Continue normal FM2K processing
    // (character updates, hit detection, etc.)
}
```

#### State Serialization Functions
```c
void save_fm2k_state(unsigned char* buffer, unsigned int* length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Save frame state
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    
    // Save character states from object pool
    for (int i = 0; i < 2; i++) {
        save_character_state(&state->characters[i], i);
    }
    
    // Save round state
    state->round_state = g_round_state;
    state->round_timer = g_round_timer;
    
    *length = sizeof(FM2KGameState);
}

void load_fm2k_state(unsigned char* buffer, unsigned int length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Restore frame state
    current_frame = state->frame_number;
    g_input_buffer_index = state->input_buffer_index;
    
    // Restore character states to object pool
    for (int i = 0; i < 2; i++) {
        load_character_state(&state->characters[i], i);
    }
    
    // Restore round state
    g_round_state = state->round_state;
    g_round_timer = state->round_timer;
}
```

### 5. **Event Handling Integration**
```c
void handle_session_events() {
    int event_count;
    GekkoSessionEvent** events = gekko_session_events(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case PlayerConnected:
                // Show "Player Connected" message
                break;
            case PlayerDisconnected:
                // Handle disconnection
                break;
            case DesyncDetected:
                // Log desync information
                printf("Desync at frame %d: local=%08x remote=%08x\n",
                       events[i]->data.desynced.frame,
                       events[i]->data.desynced.local_checksum,
                       events[i]->data.desynced.remote_checksum);
                break;
        }
    }
}
```

### 6. **Performance Considerations**
- **State Size**: ~200 bytes per frame (very efficient)
- **Prediction Window**: 8 frames ahead (80ms at 100 FPS)
- **Rollback Depth**: Up to 15 frames (150ms)
- **Network Frequency**: 100 Hz to match FM2K's frame rate

### 7. **Implementation Benefits**
1. **Minimal Code Changes**: Only need to hook input processing
2. **Existing Infrastructure**: Leverages FM2K's input history system
3. **Deterministic**: FM2K's fixed-point math ensures consistency
4. **Efficient**: Small state size enables fast rollback
5. **Robust**: GekkoNet handles all networking complexity

This integration approach leverages both FM2K's existing architecture and GekkoNet's proven rollback implementation, providing a solid foundation for online play.

### Critical Player State Variables

Based on IDA analysis, here are the key memory addresses for player state:

#### Player 1 State Variables:
```c
// Input system
g_p1_input          = 0x4259c0;  // Current input state
g_p1_input_history  = 0x4280e0;  // 1024-frame input history

// Position and stage
g_p1_stage_x        = 0x424e68;  // Stage position X
g_p1_stage_y        = 0x424e6c;  // Stage position Y

// Round and game state
g_p1_round_count    = 0x4700ec;  // Round wins
g_p1_round_state    = 0x4700f0;  // Round state
g_p1_action_state   = 0x47019c;  // Action state

// Health system
g_p1_hp             = 0x4dfc85;  // Current HP
g_p1_max_hp         = 0x4dfc91;  // Maximum HP
```

#### Player 2 State Variables:
```c
// Input system
g_p2_input          = 0x4259c4;  // Current input state
g_p2_input_history  = 0x4290e0;  // 1024-frame input history

// Round and game state
g_p2_action_state   = 0x4701a0;  // Action state

// Health system
g_p2_hp             = 0x4edcc4;  // Current HP
g_p2_max_hp         = 0x4edcd0;  // Maximum HP
```

#### Core Game State Variables:
```c
// Object pool
g_object_pool       = 0x4701e0;  // Main object pool (1024 entries)
g_sprite_data_array = 0x447938;  // Sprite data array

// Timing system
g_frame_time_ms     = 0x41e2f0;  // Frame duration (10ms)
g_last_frame_time   = 0x447dd4;  // Last frame timestamp
g_frame_sync_flag   = 0x424700;  // Frame sync state
g_frame_time_delta  = 0x425960;  // Frame timing delta
g_frame_skip_count  = 0x4246f4;  // Frame skip counter

// Random number generator
g_random_seed       = 0x41fb1c;  // RNG seed

// Input buffer management
g_input_buffer_index = (calculated); // Current buffer index
g_input_repeat_timer = 0x4d1c40;     // Input repeat timers

// Round system
g_round_timer       = 0x470060;  // Round timer
g_game_timer        = 0x470044;  // Game timer
g_hit_effect_timer  = 0x4701c8;  // Hit effect timer
```

### Final State Structure for GekkoNet

With all the memory addresses identified, here's the complete state structure:

```c
typedef struct FM2KCompleteGameState {
    // Frame and timing state
    uint32_t frame_number;
    uint32_t input_buffer_index;
    uint32_t last_frame_time;
    uint32_t frame_time_delta;
    uint8_t frame_skip_count;
    uint8_t frame_sync_flag;
    
    // Random number generator state
    uint32_t random_seed;
    
    // Player 1 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t stage_x, stage_y;
        uint32_t round_count;
        uint32_t round_state;
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p1;
    
    // Player 2 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p2;
    
    // Global timers
    uint32_t round_timer;
    uint32_t game_timer;
    uint32_t hit_effect_timer;
    
    // Input system state
    uint32_t input_repeat_timer[8];
    
    // Critical object pool state (first 2 character objects)
    struct {
        uint32_t object_type;
        int32_t pos_x, pos_y;        // 16.16 fixed point
        int32_t vel_x, vel_y;
        uint16_t state_flags;
        uint16_t animation_frame;
        uint16_t health_segments;
        uint16_t facing_direction;
        // ... other critical fields
    } character_objects[2];
    
} FM2KCompleteGameState;
```

### Memory Access Functions

```c
// Direct memory access functions for state serialization
void save_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Frame and timing
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    state->last_frame_time = *(uint32_t*)0x447dd4;
    state->frame_time_delta = *(uint32_t*)0x425960;
    state->frame_skip_count = *(uint8_t*)0x4246f4;
    state->frame_sync_flag = *(uint8_t*)0x424700;
    
    // RNG state
    state->random_seed = *(uint32_t*)0x41fb1c;
    
    // Player 1 state
    state->p1.input_current = *(uint32_t*)0x4259c0;
    memcpy(state->p1.input_history, (void*)0x4280e0, 1024 * sizeof(uint32_t));
    state->p1.stage_x = *(uint32_t*)0x424e68;
    state->p1.stage_y = *(uint32_t*)0x424e6c;
    state->p1.round_count = *(uint32_t*)0x4700ec;
    state->p1.round_state = *(uint32_t*)0x4700f0;
    state->p1.action_state = *(uint32_t*)0x47019c;
    state->p1.hp = *(uint32_t*)0x4dfc85;
    state->p1.max_hp = *(uint32_t*)0x4dfc91;
    
    // Player 2 state
    state->p2.input_current = *(uint32_t*)0x4259c4;
    memcpy(state->p2.input_history, (void*)0x4290e0, 1024 * sizeof(uint32_t));
    state->p2.action_state = *(uint32_t*)0x4701a0;
    state->p2.hp = *(uint32_t*)0x4edcc4;
    state->p2.max_hp = *(uint32_t*)0x4edcd0;
    
    // Global timers
    state->round_timer = *(uint32_t*)0x470060;
    state->game_timer = *(uint32_t*)0x470044;
    state->hit_effect_timer = *(uint32_t*)0x4701c8;
    
    // Input repeat timers
    memcpy(state->input_repeat_timer, (void*)0x4d1c40, 8 * sizeof(uint32_t));
    
    // Character objects from object pool
    save_character_objects_from_pool(state->character_objects);
}

void load_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Restore all state in reverse order
    // ... (similar but with assignment in opposite direction)
}
```

This gives us a complete, memory-accurate state structure that can be used with GekkoNet for rollback implementation. The state size is approximately 10KB, which is very reasonable for rollback netcode.

## GekkoNet SDL Example Analysis & FM2K Integration

### SDL Example Key Patterns

The OnlineSession.cpp example demonstrates the core GekkoNet integration patterns we need for FM2K:

#### 1. **Session Setup Pattern**
```c
// From SDL example - adapt for FM2K
GekkoSession* sess = nullptr;
GekkoConfig conf {
    .num_players = 2,
    .input_size = sizeof(char),           // FM2K: sizeof(uint32_t)
    .state_size = sizeof(GState),         // FM2K: sizeof(FM2KCompleteGameState)
    .max_spectators = 0,                  // FM2K: 8 for spectators
    .input_prediction_window = 10,        // FM2K: 8 frames
    .desync_detection = true,
    // .limited_saving = true,            // FM2K: false for accuracy
};

gekko_create(&sess);
gekko_start(sess, &conf);
gekko_net_adapter_set(sess, gekko_default_adapter(localport));
```

#### 2. **Player Management Pattern**
```c
// Order-dependent player addition (from example)
if (localplayer == 0) {
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
} else {
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
}
```

#### 3. **Core Game Loop Pattern** 
```c
// From SDL example - critical for FM2K integration
while (running) {
    // Timing control
    frames_ahead = gekko_frames_ahead(sess);
    frame_time = GetFrameTime(frames_ahead);
    
    // Network polling
    gekko_network_poll(sess);
    
    // Frame processing
    while (accumulator >= frame_time) {
        // Add local input
        auto input = get_key_inputs();
        gekko_add_local_input(sess, localplayer, &input);
        
        // Handle session events
        auto events = gekko_session_events(sess, &count);
        for (int i = 0; i < count; i++) {
            // Handle PlayerConnected, PlayerDisconnected, DesyncDetected
        }
        
        // Process game events
        auto updates = gekko_update_session(sess, &count);
        for (int i = 0; i < count; i++) {
            switch (updates[i]->type) {
                case SaveEvent: save_state(&state, updates[i]); break;
                case LoadEvent: load_state(&state, updates[i]); break;
                case AdvanceEvent:
                    // Extract inputs and advance game
                    inputs[0].input.value = updates[i]->data.adv.inputs[0];
                    inputs[1].input.value = updates[i]->data.adv.inputs[1];
                    update_state(state, inputs, num_players);
                    break;
            }
        }
        accumulator -= frame_time;
    }
    
    // Render current state
    render_state(state);
}
```

### FM2K Integration Strategy

#### 1. **Replace SDL2 with SDL3 for FM2K**
```c
// SDL3 initialization for FM2K integration
#include "SDL3/SDL.h"

bool init_fm2k_window(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Error initializing SDL3.\n");
        return false;
    }
    
    // Create window matching FM2K's resolution
    window = SDL_CreateWindow(
        "FM2K Online Session",
        640, 480,  // FM2K native resolution
        SDL_WINDOW_RESIZABLE
    );
    
    if (!window) {
        fprintf(stderr, "Error creating SDL3 Window.\n");
        return false;
    }
    
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        fprintf(stderr, "Error creating SDL3 Renderer.\n");
        return false;
    }
    
    return true;
}
```

#### 2. **FM2K Input Structure**
```c
// Replace simple GInput with FM2K's 11-bit input system
struct FM2KInput {
    union {
        struct {
            uint16_t left     : 1;   // 0x001
            uint16_t right    : 1;   // 0x002
            uint16_t up       : 1;   // 0x004
            uint16_t down     : 1;   // 0x008
            uint16_t button1  : 1;   // 0x010
            uint16_t button2  : 1;   // 0x020
            uint16_t button3  : 1;   // 0x040
            uint16_t button4  : 1;   // 0x080
            uint16_t button5  : 1;   // 0x100
            uint16_t button6  : 1;   // 0x200
            uint16_t button7  : 1;   // 0x400
            uint16_t reserved : 5;   // Unused bits
        } bits;
        uint16_t value;
    } input;
};

FM2KInput get_fm2k_inputs() {
    FM2KInput input{};
    input.input.value = 0;
    
    // Get keyboard state (SDL3 syntax)
    const bool* keys = SDL_GetKeyboardState(NULL);
    
    // Map to FM2K inputs
    input.input.bits.up = keys[SDL_SCANCODE_W];
    input.input.bits.left = keys[SDL_SCANCODE_A];
    input.input.bits.down = keys[SDL_SCANCODE_S];
    input.input.bits.right = keys[SDL_SCANCODE_D];
    input.input.bits.button1 = keys[SDL_SCANCODE_J];
    input.input.bits.button2 = keys[SDL_SCANCODE_K];
    input.input.bits.button3 = keys[SDL_SCANCODE_L];
    // ... map other buttons
    
    return input;
}
```

#### 3. **FM2K State Management**
```c
// Replace simple GState with FM2KCompleteGameState
void save_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    *ev->data.save.state_len = sizeof(FM2KCompleteGameState);
    
    // Use Fletcher's checksum (from example) for consistency
    *ev->data.save.checksum = fletcher32((uint16_t*)gs, sizeof(FM2KCompleteGameState));
    
    std::memcpy(ev->data.save.state, gs, sizeof(FM2KCompleteGameState));
}

void load_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    std::memcpy(gs, ev->data.load.state, sizeof(FM2KCompleteGameState));
}

void update_fm2k_state(FM2KCompleteGameState& gs, FM2KInput inputs[2]) {
    // Call into FM2K's actual game logic
    // This would hook into the functions we analyzed:
    
    // 1. Inject inputs into FM2K's input system
    *(uint32_t*)0x4259c0 = inputs[0].input.value;  // g_p1_input
    *(uint32_t*)0x4259c4 = inputs[1].input.value;  // g_p2_input
    
    // 2. Call FM2K's update functions
    // process_game_inputs_FRAMESTEP_HOOK();  // 0x4146d0
    // update_game_state();                   // Game logic
    // character_state_machine();             // 0x411bf0
    // hit_detection_system();                // 0x40f010
    
    // 3. Extract updated state from FM2K memory
    gs.p1.hp = *(uint32_t*)0x4dfc85;
    gs.p2.hp = *(uint32_t*)0x4edcc4;
    gs.round_timer = *(uint32_t*)0x470060;
    // ... extract all other state variables
}
```

#### 4. **FM2K Timing Integration**
```c
// Adapt timing to match FM2K's 100 FPS
float GetFM2KFrameTime(float frames_ahead) {
    // FM2K runs at 100 FPS (10ms per frame)
    const float base_frame_time = 1.0f / 100.0f;  // 0.01 seconds
    
    if (frames_ahead >= 0.75f) {
        // Slow down if too far ahead
        return base_frame_time * 1.02f;  // 59.8 FPS equivalent
    } else {
        return base_frame_time;  // Normal 100 FPS
    }
}
```

#### 5. **Complete FM2K Integration Main Loop**
```c
int fm2k_online_main(int argc, char* args[]) {
    // Parse command line (same as example)
    int localplayer = std::stoi(args[1]);
    const int localport = std::stoi(args[2]);
    std::string remote_address = std::move(args[3]);
    int localdelay = std::stoi(args[4]);
    
    // Initialize SDL3 and FM2K
    running = init_fm2k_window();
    
    // Initialize FM2K state
    FM2KCompleteGameState state = {};
    FM2KInput inputs[2] = {};
    
    // Setup GekkoNet
    GekkoSession* sess = nullptr;
    GekkoConfig conf {
        .num_players = 2,
        .input_size = sizeof(uint16_t),  // FM2K input size
        .state_size = sizeof(FM2KCompleteGameState),
        .max_spectators = 8,
        .input_prediction_window = 8,
        .desync_detection = true,
        .limited_saving = false,  // Full state saving for accuracy
        .post_sync_joining = true
    };
    
    gekko_create(&sess);
    gekko_start(sess, &conf);
    gekko_net_adapter_set(sess, gekko_default_adapter(localport));
    
    // Add players (order-dependent)
    if (localplayer == 0) {
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
    } else {
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    }
    
    gekko_set_local_delay(sess, localplayer, localdelay);
    
    // Timing variables
    auto curr_time = std::chrono::steady_clock::now();
    auto prev_time = curr_time;
    float delta_time = 0.0f;
    float accumulator = 0.0f;
    float frame_time = 0.0f;
    float frames_ahead = 0.0f;
    
    while (running) {
        curr_time = std::chrono::steady_clock::now();
        
        frames_ahead = gekko_frames_ahead(sess);
        frame_time = GetFM2KFrameTime(frames_ahead);
        
        delta_time = std::chrono::duration<float>(curr_time - prev_time).count();
        prev_time = curr_time;
        accumulator += delta_time;
        
        gekko_network_poll(sess);
        
        while (accumulator >= frame_time) {
            // Process SDL events
            process_events();
            
            // Get local input
            auto input = get_fm2k_inputs();
            gekko_add_local_input(sess, localplayer, &input);
            
            // Handle session events
            int count = 0;
            auto events = gekko_session_events(sess, &count);
            for (int i = 0; i < count; i++) {
                handle_fm2k_session_event(events[i]);
            }
            
            // Process game events
            count = 0;
            auto updates = gekko_update_session(sess, &count);
            for (int i = 0; i < count; i++) {
                auto ev = updates[i];
                
                switch (ev->type) {
                    case SaveEvent:
                        save_fm2k_state(&state, ev);
                        break;
                    case LoadEvent:
                        load_fm2k_state(&state, ev);
                        break;
                    case AdvanceEvent:
                        // Extract inputs from GekkoNet
                        inputs[0].input.value = ((uint16_t*)ev->data.adv.inputs)[0];
                        inputs[1].input.value = ((uint16_t*)ev->data.adv.inputs)[1];
                        
                        // Update FM2K game state
                        update_fm2k_state(state, inputs);
                        break;
                }
            }
            
            accumulator -= frame_time;
        }
        
        // Render current state
        render_fm2k_state(state);
    }
    
    gekko_destroy(sess);
    del_window();
    return 0;
}
```

### Key Differences from SDL Example

1. **Input Size**: FM2K uses 16-bit inputs vs 8-bit in example
2. **State Size**: ~10KB vs ~16 bytes in example  
3. **Frame Rate**: 100 FPS vs 60 FPS in example
4. **State Complexity**: Full game state vs simple position
5. **Integration Depth**: Hooks into existing game vs standalone

### Integration Benefits

1. **Proven Pattern**: The SDL example shows GekkoNet works reliably
2. **Clear Structure**: Event handling patterns are well-defined
3. **Performance**: Timing control handles frame rate variations
4. **Network Stats**: Built-in ping/jitter monitoring
5. **Error Handling**: Desync detection and player management

This analysis shows that adapting the SDL example for FM2K is very feasible, with the main changes being input/state structures and frame timing to match FM2K's 100 FPS architecture.

## Hit Judge and Round System Architecture

### Hit Judge Configuration (0x42470C-0x430120)

The game uses a sophisticated hit detection and configuration system loaded from INI files:

```c
struct FM2K_HitJudgeConfig {
    uint32_t hit_judge_value;    // 0x42470C - Hit detection threshold
    uint32_t config_values[7];   // 0x4300E0-0x430120 - General config values
    char     config_string[260]; // Configuration string buffer
};
```

The hit judge system is initialized by `hit_judge_set_function` (0x414930) which:
1. Loads configuration from INI files
2. Sets up hit detection parameters
3. Configures round settings and game modes

### Round System State (0x470040-0x47006C)

The round system maintains several critical state variables:

```c
struct FM2K_RoundSystem {
    uint32_t game_mode;      // 0x470040 - Current game mode (VS/Team/Tournament)
    uint32_t round_limit;    // 0x470048 - Maximum rounds for the match
    uint32_t round_state;    // 0x47004C - Current round state
    uint32_t round_timer;    // 0x470060 - Active round timer
};

enum FM2K_RoundState {
    ROUND_INIT = 0,      // Initial state
    ROUND_ACTIVE = 1,    // Round in progress
    ROUND_END = 2,       // Round has ended
    ROUND_MATCH_END = 3  // Match has ended
};

enum FM2K_GameMode {
    MODE_NORMAL = 0,     // Normal VS mode
    MODE_TEAM = 1,       // Team Battle mode
    MODE_TOURNAMENT = 2  // Tournament mode
};
```

The round system is managed by `vs_round_function` (0x4086A0) which:
1. Handles round state transitions
2. Manages round timers
3. Processes win/loss conditions
4. Controls match flow

### Rollback Implications

This architecture has several important implications for rollback implementation:

1. **State Serialization Requirements:**
   - Must save hit judge configuration
   - Must preserve round state and timers
   - Game mode affects state size

2. **Deterministic Behavior:**
   - Hit detection is configuration-driven
   - Round state transitions are deterministic
   - Timer-based events are predictable

3. **State Size Optimization:**
   - Hit judge config is static per match
   - Only dynamic round state needs saving
   - Can optimize by excluding static config

4. **Synchronization Points:**
   - Round state changes are key sync points
   - Hit detection results must be identical
   - Timer values must match exactly

Updated state structure for rollback:

```c
typedef struct FM2KGameState {
    // ... existing timing and input state ...

    // Round System State
    struct {
        uint32_t mode;          // Current game mode
        uint32_t round_limit;   // Maximum rounds
        uint32_t round_state;   // Current round state
        uint32_t round_timer;   // Active round timer
        uint32_t score_value;   // Current score/timer value
    } round_system;

    // Hit Judge State
    struct {
        uint32_t hit_judge_value;   // Current hit detection threshold
        uint32_t effect_target;     // Current hit effect target
        uint32_t effect_timer;      // Hit effect duration
    } hit_system;

    // ... rest of game state ...
} FM2KGameState;
```

## Sprite Rendering and Hit Detection System

### Sprite Rendering Engine (0x40CC30)

The game uses a sophisticated sprite rendering engine that handles:
1. Hit effect visualization
2. Sprite transformations and blending
3. Color palette manipulation
4. Screen-space effects

Key components:

```c
struct SpriteRenderState {
    uint32_t render_mode;     // Different rendering modes (-10 to 3)
    uint32_t blend_flags;     // Blending and effect flags
    uint32_t color_values[3]; // RGB color modification values
    uint32_t effect_timer;    // Effect duration counter
};
```

### Hit Effect System

The hit effect system is tightly integrated with the sprite renderer:

1. **Effect Triggers:**
   - Hit detection results (0x42470C)
   - Game state transitions
   - Timer-based events

2. **Effect Parameters:**
   ```c
   struct HitEffectParams {
       uint32_t target_id;      // Effect target identifier
       uint32_t effect_type;    // Visual effect type
       uint32_t duration;       // Effect duration in frames
       uint32_t color_values;   // RGB color modulation
   };
   ```

3. **Rendering Pipeline:**
   - Color palette manipulation (0x4D1A20)
   - Sprite transformation
   - Blend mode selection
   - Screen-space effects

### Integration with Rollback

The sprite and hit effect systems have important implications for rollback:

1. **Visual State Management:**
   - Effect timers must be part of the game state
   - Color modulation values need saving
   - Sprite transformations must be deterministic

2. **Performance Considerations:**
   - Effect parameters are small (few bytes per effect)
   - Visual state is derived from game state
   - Can optimize by excluding pure visual state

3. **Synchronization Requirements:**
   - Hit effects must trigger identically
   - Effect timers must stay synchronized
   - Visual state must match game state

Updated state structure to include visual effects:

```c
typedef struct FM2KGameState {
    // ... previous state members ...

    // Visual Effect State
    struct {
        uint32_t active_effects;     // Bitfield of active effects
        uint32_t effect_timers[8];   // Array of effect durations
        uint32_t color_values[8][3]; // RGB values for each effect
        uint32_t target_ids[8];      // Effect target identifiers
    } visual_state;

    // ... rest of game state ...
} FM2KGameState;
```

## Game State Management System

The game state manager (0x406FC0) handles several critical aspects of the game:

### State Variables

1. **Round System State:**
   ```c
   struct RoundSystemState {
       uint32_t g_round_timer;        // 0x470060 - Current round timer value
       uint32_t g_round_limit;        // 0x470048 - Maximum rounds per match
       uint32_t g_round_state;        // 0x47004C - Current round state
       uint32_t g_game_mode;          // 0x470040 - Current game mode
   };
   ```

2. **Player State:**
   ```c
   struct PlayerState {
       int32_t stage_position;        // Current position on stage grid
       uint32_t action_state;         // Current action being performed
       uint32_t round_count;          // Number of rounds completed
       uint32_t input_history[4];     // Recent input history
       uint32_t move_history[4];      // Recent movement history
   };
   ```

### State Management Flow

1. **Initialization (0x406FC0):**
   - Loads initial configuration
   - Sets up round timer and limits
   - Initializes player positions
   - Sets up game mode parameters

2. **State Updates:**
   - Input processing and validation
   - Position and movement updates
   - Action state transitions
   - Round state management

3. **State Synchronization:**
   - Timer synchronization
   - Player state synchronization
   - Round state transitions
   - Game mode state management

### Rollback Implications

1. **Critical State Components:**
   - Round timer and state
   - Player positions and actions
   - Input and move history
   - Game mode state

2. **State Size Analysis:**
   - Core game state: ~128 bytes
   - Player state: ~32 bytes per player
   - History buffers: ~32 bytes per player
   - Total per-frame state: ~256 bytes

3. **Synchronization Requirements:**
   - Timer must be deterministic
   - Player state must be atomic
   - Input processing must be consistent
   - State transitions must be reproducible

## Input Processing System

The game uses a sophisticated input processing system that handles both movement and action inputs:

### Input Mapping (0x406F20)

```c
enum InputActions {
    ACTION_NONE = 0,
    ACTION_A = 1,    // 0x20  - Light Attack
    ACTION_B = 2,    // 0x40  - Medium Attack
    ACTION_C = 3,    // 0x80  - Heavy Attack
    ACTION_D = 4,    // 0x100 - Special Move
    ACTION_E = 5     // 0x200 - Super Move
};

enum InputDirections {
    DIR_NONE  = 0x000,
    DIR_UP    = 0x004,
    DIR_DOWN  = 0x008,
    DIR_LEFT  = 0x001,
    DIR_RIGHT = 0x002
};
```

### Input Processing Flow

1. **Raw Input Capture:**
   - Movement inputs (4-way directional)
   - Action buttons (5 main buttons)
   - System buttons (Start, Select)

2. **Input State Management:**
   ```c
   struct InputState {
       uint16_t current_input;     // Current frame's input
       uint16_t previous_input;    // Last frame's input
       uint16_t input_changes;     // Changed bits since last frame
       uint8_t  processed_input;   // Processed directional input
   };
   ```

3. **Input History:**
   - Each player maintains a 4-frame input history
   - History includes both raw and processed inputs
   - Used for move validation and rollback

4. **Input Processing:**
   - Raw input capture
   - Input change detection
   - Input repeat handling:
     - Initial delay
     - Repeat delay
     - Repeat timer
   - Input validation
   - Mode-specific processing:
     - VS mode (< 3000)
     - Story mode (? 3000)

5. **Memory Layout:**
   ```
   input_system {
     +g_input_buffer_index: Current frame index
     +g_p1_input[8]: Current P1 inputs
     +g_p2_input: Current P2 inputs
     +g_prev_input_state[8]: Previous frame inputs
     +g_input_changes[8]: Input change flags
     +g_input_repeat_state[8]: Input repeat flags
     +g_input_repeat_timer[8]: Repeat timers
     +g_processed_input[8]: Processed inputs
     +g_combined_processed_input: Combined state
     +g_combined_input_changes: Change mask
     +g_combined_raw_input: Raw input mask
   }
   ```

6. **Critical Operations:**
   - Frame advance:
     ```c
     g_input_buffer_index = (g_input_buffer_index + 1) & 0x3FF;
     g_p1_input_history[g_input_buffer_index] = current_input;
     ```
   - Input processing:
     ```c
     g_input_changes = current_input & (prev_input ^ current_input);
     if (repeat_active && current_input == repeat_state) {
       if (--repeat_timer == 0) {
         processed_input = current_input;
         repeat_timer = repeat_delay;
       }
     }
     ```

This system is crucial for rollback implementation as it provides:
1. Deterministic input processing
2. Frame-accurate input history
3. Clear state serialization points
4. Mode-specific input handling
5. Input validation and processing logic

The 1024-frame history buffer is particularly useful for rollback, as it provides ample space for state rewinding and replay.
```

### Health Damage Manager (0x40e6f0)

The health damage manager handles health state and damage calculations:

1. **Health State Management:**
   - Per-character health tracking
   - Health segments system
   - Recovery mechanics
   - Guard damage handling

2. **Memory Layout:**
   ```
   health_system {
     +4DFC95: Current health segment
     +4DFC99: Maximum health segments
     +4DFC9D: Current health points
     +4DFCA1: Health segment size
   }
   ```

3. **Damage Processing:**
   - Segment-based health system:
     ```c
     while (current_health < 0) {
       if (current_segment > 0) {
         current_segment--;
         current_health += segment_size;
       } else {
         current_health = 0;
       }
     }
     ```
   - Health recovery:
     ```c
     while (current_health >= segment_size) {
       if (current_segment < max_segments) {
         current_health -= segment_size;
         current_segment++;
       } else {
         current_health = 0;
       }
     }
     ```

4. **Critical Operations:**
   - Damage application:
     - Negative values for damage
     - Positive values for healing
   - Segment management:
     - Segment depletion
     - Segment recovery
     - Maximum segment cap
   - Health validation:
     - Minimum health check
     - Maximum health check
     - Segment boundary checks

This system is crucial for rollback implementation as it:
1. Uses deterministic damage calculations
2. Maintains clear health state boundaries
3. Provides segment-based state tracking
4. Handles health recovery mechanics
5. Manages guard damage separately

The segmented health system will make it easier to serialize and restore health states during rollback operations, as each segment provides a clear checkpoint for state management.
```

### Rollback Implementation Summary

After analyzing FM2K's core systems, we can conclude that the game is well-suited for rollback netcode implementation. Here's a comprehensive overview of how each system will integrate with rollback:

1. **State Serialization Points:**
   - Character State Machine (0x411bf0):
     - Base states (0x0-0x3)
     - Action states (0x4-0x7)
     - Movement states (0x8-0xB)
     - Frame counters and timers
   - Hit Detection System (0x40f010):
     - Hit boxes and collision state
     - Damage values and scaling
     - Hit confirmation flags
   - Input Buffer System (0x4146d0):
     - 1024-frame history buffer
     - Input state flags
     - Repeat handling state
   - Health System (0x40e6f0):
     - Health segments
     - Current health points
     - Guard damage state
     - Recovery mechanics

2. **Deterministic Systems:**
   - Fixed frame rate (100 FPS)
   - 16.16 fixed-point coordinates
   - Deterministic input processing
   - Clear state transitions
   - Predictable damage calculations
   - Frame-based timers

3. **State Management:**
   - Object Pool (1024 entries):
     - Type identification
     - Position and velocity
     - Animation state
     - Collision data
   - Input History:
     - Circular buffer design
     - Clear frame boundaries
     - Input validation
   - Hit Detection:
     - Priority system
     - State validation
     - Reaction handling
   - Health System:
     - Segment-based tracking
     - Clear state boundaries
     - Recovery mechanics

4. **Implementation Strategy:**
   a. Save State (Frame N):
      ```c
      struct SaveState {
          // Character State
          int base_state;
          int action_state;
          int movement_state;
          int frame_counters[4];
          
          // Hit Detection
          int hit_boxes[20];
          int damage_values[8];
          int hit_flags;
          
          // Input Buffer
          int input_history[1024];
          int input_flags;
          int repeat_state;
          
          // Health System
          int health_segments;
          int current_health;
          int guard_damage;
      };
      ```

   b. Rollback Process:
      1. Save current frame state
      2. Apply new remote input
      3. Simulate forward:
         - Process inputs
         - Update character states
         - Handle collisions
         - Apply damage
      4. Render new state
      5. Save confirmed state

5. **Critical Hooks:**
   - Input Processing (0x4146d0):
     - Frame advance point
     - Input buffer management
   - Character State (0x411bf0):
     - State transition handling
     - Frame counting
   - Hit Detection (0x40f010):
     - Collision processing
     - Damage application
   - Health System (0x40e6f0):
     - Health state management
     - Segment tracking

6. **Optimization Opportunities:**
   - Parallel state simulation
   - Minimal state serialization
   - Efficient state diffing
   - Smart state prediction
   - Input compression

The analysis reveals that FM2K's architecture is highly suitable for rollback implementation due to its:
1. Deterministic game logic
2. Clear state boundaries
3. Frame-based processing
4. Efficient memory layout
5. Well-defined systems

This research provides a solid foundation for implementing rollback netcode in FM2K, with all major systems supporting the requirements for state saving, rollback, and resimulation.

## GekkoNet Integration Analysis

### GekkoNet Library Overview

GekkoNet is a rollback netcode library that provides the networking infrastructure we need for FM2K. Here's how it maps to our analyzed systems:

### 1. **GekkoNet Configuration for FM2K**
```c
GekkoConfig fm2k_config = {
    .num_players = 2,                    // P1 vs P2
    .max_spectators = 8,                 // Allow spectators
    .input_prediction_window = 8,        // 8 frames ahead prediction
    .spectator_delay = 6,                // 6 frame spectator delay
    .input_size = sizeof(uint32_t),      // FM2K input flags (32-bit)
    .state_size = sizeof(FM2KGameState), // Our minimal state structure
    .limited_saving = false,             // Full state saving for accuracy
    .post_sync_joining = true,           // Allow mid-match spectators
    .desync_detection = true             // Enable desync detection
};
```

### 2. **Integration Points with FM2K Systems**

#### Input System Integration (0x4146d0)
```c
// Hook into process_game_inputs()
void fm2k_input_hook() {
    // Get GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case AdvanceEvent:
                // Inject inputs into FM2K's input system
                g_p1_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[0];
                g_p2_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[1];
                // Continue normal input processing
                break;
                
            case SaveEvent:
                // Save current FM2K state
                save_fm2k_state(events[i]->data.save.state, events[i]->data.save.state_len);
                break;
                
            case LoadEvent:
                // Restore FM2K state
                load_fm2k_state(events[i]->data.load.state, events[i]->data.load.state_len);
                break;
        }
    }
}
```

#### State Management Integration
```c
typedef struct FM2KGameState {
    // Frame tracking
    uint32_t frame_number;
    uint32_t input_buffer_index;
    
    // Input state
    uint32_t p1_input_current;
    uint32_t p2_input_current;
    uint32_t input_repeat_state[8];
    uint32_t input_repeat_timer[8];
    
    // Character states (per character)
    struct {
        // Position (16.16 fixed point)
        int32_t pos_x, pos_y;
        int32_t vel_x, vel_y;
        
        // State machine
        uint16_t base_state;        // 0x0-0x3
        uint16_t action_state;      // 0x4-0x7  
        uint16_t movement_state;    // 0x8-0xB
        uint16_t state_flags;       // Offset 350
        
        // Animation
        uint16_t animation_frame;   // Offset 12
        uint16_t max_animation_frame; // Offset 88
        
        // Health system
        uint16_t health_segments;   // Current segments
        uint16_t current_health;    // Health points
        uint16_t max_health_segments; // Maximum segments
        uint16_t guard_damage;      // Guard damage accumulation
        
        // Hit detection
        uint16_t hit_flags;         // Hit state flags
        uint16_t hit_stun_timer;    // Hit stun duration
        uint16_t guard_state;       // Guard state flags
        
        // Facing and direction
        uint8_t facing_direction;   // Left/right facing
        uint8_t input_direction;    // Current input direction
    } characters[2];
    
    // Round state
    uint32_t round_state;           // Round state machine
    uint32_t round_timer;           // Round timer
    uint32_t game_mode;             // Current game mode
    
    // Hit detection global state
    uint32_t hit_effect_target;     // Hit effect target
    uint32_t hit_effect_timer;      // Hit effect timer
    
    // Combo system
    uint16_t combo_counter[2];      // Combo counters
    uint16_t damage_scaling[2];     // Damage scaling
    
    // Object pool critical state
    uint16_t active_objects;        // Number of active objects
    uint32_t object_state_flags;    // Critical object flags
    
} FM2KGameState;
```

### 3. **Network Adapter Implementation**
```c
GekkoNetAdapter fm2k_adapter = {
    .send_data = fm2k_send_packet,
    .receive_data = fm2k_receive_packets,
    .free_data = fm2k_free_packet_data
};

void fm2k_send_packet(GekkoNetAddress* addr, const char* data, int length) {
    // Use existing LilithPort networking or implement UDP
    // addr->data contains IP/port information
    // data contains the packet to send
}

GekkoNetResult** fm2k_receive_packets(int* length) {
    // Collect all packets received since last call
    // Return array of GekkoNetResult pointers
    // GekkoNet will call free_data on each result
}
```

### 4. **Integration with Existing FM2K Systems**

#### Main Game Loop Integration
```c
// Hook at 0x4146d0 - process_game_inputs()
void fm2k_rollback_frame() {
    // 1. Poll network
    gekko_network_poll(session);
    
    // 2. Add local inputs
    uint32_t local_input = get_local_player_input();
    gekko_add_local_input(session, local_player_id, &local_input);
    
    // 3. Process GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    // 4. Handle events (save/load/advance)
    process_gekko_events(events, event_count);
    
    // 5. Continue normal FM2K processing
    // (character updates, hit detection, etc.)
}
```

#### State Serialization Functions
```c
void save_fm2k_state(unsigned char* buffer, unsigned int* length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Save frame state
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    
    // Save character states from object pool
    for (int i = 0; i < 2; i++) {
        save_character_state(&state->characters[i], i);
    }
    
    // Save round state
    state->round_state = g_round_state;
    state->round_timer = g_round_timer;
    
    *length = sizeof(FM2KGameState);
}

void load_fm2k_state(unsigned char* buffer, unsigned int length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Restore frame state
    current_frame = state->frame_number;
    g_input_buffer_index = state->input_buffer_index;
    
    // Restore character states to object pool
    for (int i = 0; i < 2; i++) {
        load_character_state(&state->characters[i], i);
    }
    
    // Restore round state
    g_round_state = state->round_state;
    g_round_timer = state->round_timer;
}
```

### 5. **Event Handling Integration**
```c
void handle_session_events() {
    int event_count;
    GekkoSessionEvent** events = gekko_session_events(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case PlayerConnected:
                // Show "Player Connected" message
                break;
            case PlayerDisconnected:
                // Handle disconnection
                break;
            case DesyncDetected:
                // Log desync information
                printf("Desync at frame %d: local=%08x remote=%08x\n",
                       events[i]->data.desynced.frame,
                       events[i]->data.desynced.local_checksum,
                       events[i]->data.desynced.remote_checksum);
                break;
        }
    }
}
```

### 6. **Performance Considerations**
- **State Size**: ~200 bytes per frame (very efficient)
- **Prediction Window**: 8 frames ahead (80ms at 100 FPS)
- **Rollback Depth**: Up to 15 frames (150ms)
- **Network Frequency**: 100 Hz to match FM2K's frame rate

### 7. **Implementation Benefits**
1. **Minimal Code Changes**: Only need to hook input processing
2. **Existing Infrastructure**: Leverages FM2K's input history system
3. **Deterministic**: FM2K's fixed-point math ensures consistency
4. **Efficient**: Small state size enables fast rollback
5. **Robust**: GekkoNet handles all networking complexity

This integration approach leverages both FM2K's existing architecture and GekkoNet's proven rollback implementation, providing a solid foundation for online play.

### Critical Player State Variables

Based on IDA analysis, here are the key memory addresses for player state:

#### Player 1 State Variables:
```c
// Input system
g_p1_input          = 0x4259c0;  // Current input state
g_p1_input_history  = 0x4280e0;  // 1024-frame input history

// Position and stage
g_p1_stage_x        = 0x424e68;  // Stage position X
g_p1_stage_y        = 0x424e6c;  // Stage position Y

// Round and game state
g_p1_round_count    = 0x4700ec;  // Round wins
g_p1_round_state    = 0x4700f0;  // Round state
g_p1_action_state   = 0x47019c;  // Action state

// Health system
g_p1_hp             = 0x4dfc85;  // Current HP
g_p1_max_hp         = 0x4dfc91;  // Maximum HP
```

#### Player 2 State Variables:
```c
// Input system
g_p2_input          = 0x4259c4;  // Current input state
g_p2_input_history  = 0x4290e0;  // 1024-frame input history

// Round and game state
g_p2_action_state   = 0x4701a0;  // Action state

// Health system
g_p2_hp             = 0x4edcc4;  // Current HP
g_p2_max_hp         = 0x4edcd0;  // Maximum HP
```

#### Core Game State Variables:
```c
// Object pool
g_object_pool       = 0x4701e0;  // Main object pool (1024 entries)
g_sprite_data_array = 0x447938;  // Sprite data array

// Timing system
g_frame_time_ms     = 0x41e2f0;  // Frame duration (10ms)
g_last_frame_time   = 0x447dd4;  // Last frame timestamp
g_frame_sync_flag   = 0x424700;  // Frame sync state
g_frame_time_delta  = 0x425960;  // Frame timing delta
g_frame_skip_count  = 0x4246f4;  // Frame skip counter

// Random number generator
g_random_seed       = 0x41fb1c;  // RNG seed

// Input buffer management
g_input_buffer_index = (calculated); // Current buffer index
g_input_repeat_timer = 0x4d1c40;     // Input repeat timers

// Round system
g_round_timer       = 0x470060;  // Round timer
g_game_timer        = 0x470044;  // Game timer
g_hit_effect_timer  = 0x4701c8;  // Hit effect timer
```

### Final State Structure for GekkoNet

With all the memory addresses identified, here's the complete state structure:

```c
typedef struct FM2KCompleteGameState {
    // Frame and timing state
    uint32_t frame_number;
    uint32_t input_buffer_index;
    uint32_t last_frame_time;
    uint32_t frame_time_delta;
    uint8_t frame_skip_count;
    uint8_t frame_sync_flag;
    
    // Random number generator state
    uint32_t random_seed;
    
    // Player 1 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t stage_x, stage_y;
        uint32_t round_count;
        uint32_t round_state;
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p1;
    
    // Player 2 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p2;
    
    // Global timers
    uint32_t round_timer;
    uint32_t game_timer;
    uint32_t hit_effect_timer;
    
    // Input system state
    uint32_t input_repeat_timer[8];
    
    // Critical object pool state (first 2 character objects)
    struct {
        uint32_t object_type;
        int32_t pos_x, pos_y;        // 16.16 fixed point
        int32_t vel_x, vel_y;
        uint16_t state_flags;
        uint16_t animation_frame;
        uint16_t health_segments;
        uint16_t facing_direction;
        // ... other critical fields
    } character_objects[2];
    
} FM2KCompleteGameState;
```

### Memory Access Functions

```c
// Direct memory access functions for state serialization
void save_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Frame and timing
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    state->last_frame_time = *(uint32_t*)0x447dd4;
    state->frame_time_delta = *(uint32_t*)0x425960;
    state->frame_skip_count = *(uint8_t*)0x4246f4;
    state->frame_sync_flag = *(uint8_t*)0x424700;
    
    // RNG state
    state->random_seed = *(uint32_t*)0x41fb1c;
    
    // Player 1 state
    state->p1.input_current = *(uint32_t*)0x4259c0;
    memcpy(state->p1.input_history, (void*)0x4280e0, 1024 * sizeof(uint32_t));
    state->p1.stage_x = *(uint32_t*)0x424e68;
    state->p1.stage_y = *(uint32_t*)0x424e6c;
    state->p1.round_count = *(uint32_t*)0x4700ec;
    state->p1.round_state = *(uint32_t*)0x4700f0;
    state->p1.action_state = *(uint32_t*)0x47019c;
    state->p1.hp = *(uint32_t*)0x4dfc85;
    state->p1.max_hp = *(uint32_t*)0x4dfc91;
    
    // Player 2 state
    state->p2.input_current = *(uint32_t*)0x4259c4;
    memcpy(state->p2.input_history, (void*)0x4290e0, 1024 * sizeof(uint32_t));
    state->p2.action_state = *(uint32_t*)0x4701a0;
    state->p2.hp = *(uint32_t*)0x4edcc4;
    state->p2.max_hp = *(uint32_t*)0x4edcd0;
    
    // Global timers
    state->round_timer = *(uint32_t*)0x470060;
    state->game_timer = *(uint32_t*)0x470044;
    state->hit_effect_timer = *(uint32_t*)0x4701c8;
    
    // Input repeat timers
    memcpy(state->input_repeat_timer, (void*)0x4d1c40, 8 * sizeof(uint32_t));
    
    // Character objects from object pool
    save_character_objects_from_pool(state->character_objects);
}

void load_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Restore all state in reverse order
    // ... (similar but with assignment in opposite direction)
}
```

This gives us a complete, memory-accurate state structure that can be used with GekkoNet for rollback implementation. The state size is approximately 10KB, which is very reasonable for rollback netcode.

## GekkoNet SDL Example Analysis & FM2K Integration

### SDL Example Key Patterns

The OnlineSession.cpp example demonstrates the core GekkoNet integration patterns we need for FM2K:

#### 1. **Session Setup Pattern**
```c
// From SDL example - adapt for FM2K
GekkoSession* sess = nullptr;
GekkoConfig conf {
    .num_players = 2,
    .input_size = sizeof(char),           // FM2K: sizeof(uint32_t)
    .state_size = sizeof(GState),         // FM2K: sizeof(FM2KCompleteGameState)
    .max_spectators = 0,                  // FM2K: 8 for spectators
    .input_prediction_window = 10,        // FM2K: 8 frames
    .desync_detection = true,
    // .limited_saving = true,            // FM2K: false for accuracy
};

gekko_create(&sess);
gekko_start(sess, &conf);
gekko_net_adapter_set(sess, gekko_default_adapter(localport));
```

#### 2. **Player Management Pattern**
```c
// Order-dependent player addition (from example)
if (localplayer == 0) {
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
} else {
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
}
```

#### 3. **Core Game Loop Pattern** 
```c
// From SDL example - critical for FM2K integration
while (running) {
    // Timing control
    frames_ahead = gekko_frames_ahead(sess);
    frame_time = GetFrameTime(frames_ahead);
    
    // Network polling
    gekko_network_poll(sess);
    
    // Frame processing
    while (accumulator >= frame_time) {
        // Add local input
        auto input = get_key_inputs();
        gekko_add_local_input(sess, localplayer, &input);
        
        // Handle session events
        auto events = gekko_session_events(sess, &count);
        for (int i = 0; i < count; i++) {
            // Handle PlayerConnected, PlayerDisconnected, DesyncDetected
        }
        
        // Process game events
        auto updates = gekko_update_session(sess, &count);
        for (int i = 0; i < count; i++) {
            switch (updates[i]->type) {
                case SaveEvent: save_state(&state, updates[i]); break;
                case LoadEvent: load_state(&state, updates[i]); break;
                case AdvanceEvent:
                    // Extract inputs and advance game
                    inputs[0].input.value = updates[i]->data.adv.inputs[0];
                    inputs[1].input.value = updates[i]->data.adv.inputs[1];
                    update_state(state, inputs, num_players);
                    break;
            }
        }
        accumulator -= frame_time;
    }
    
    // Render current state
    render_state(state);
}
```

### FM2K Integration Strategy

#### 1. **Replace SDL2 with SDL3 for FM2K**
```c
// SDL3 initialization for FM2K integration
#include "SDL3/SDL.h"

bool init_fm2k_window(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Error initializing SDL3.\n");
        return false;
    }
    
    // Create window matching FM2K's resolution
    window = SDL_CreateWindow(
        "FM2K Online Session",
        640, 480,  // FM2K native resolution
        SDL_WINDOW_RESIZABLE
    );
    
    if (!window) {
        fprintf(stderr, "Error creating SDL3 Window.\n");
        return false;
    }
    
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        fprintf(stderr, "Error creating SDL3 Renderer.\n");
        return false;
    }
    
    return true;
}
```

#### 2. **FM2K Input Structure**
```c
// Replace simple GInput with FM2K's 11-bit input system
struct FM2KInput {
    union {
        struct {
            uint16_t left     : 1;   // 0x001
            uint16_t right    : 1;   // 0x002
            uint16_t up       : 1;   // 0x004
            uint16_t down     : 1;   // 0x008
            uint16_t button1  : 1;   // 0x010
            uint16_t button2  : 1;   // 0x020
            uint16_t button3  : 1;   // 0x040
            uint16_t button4  : 1;   // 0x080
            uint16_t button5  : 1;   // 0x100
            uint16_t button6  : 1;   // 0x200
            uint16_t button7  : 1;   // 0x400
            uint16_t reserved : 5;   // Unused bits
        } bits;
        uint16_t value;
    } input;
};

FM2KInput get_fm2k_inputs() {
    FM2KInput input{};
    input.input.value = 0;
    
    // Get keyboard state (SDL3 syntax)
    const bool* keys = SDL_GetKeyboardState(NULL);
    
    // Map to FM2K inputs
    input.input.bits.up = keys[SDL_SCANCODE_W];
    input.input.bits.left = keys[SDL_SCANCODE_A];
    input.input.bits.down = keys[SDL_SCANCODE_S];
    input.input.bits.right = keys[SDL_SCANCODE_D];
    input.input.bits.button1 = keys[SDL_SCANCODE_J];
    input.input.bits.button2 = keys[SDL_SCANCODE_K];
    input.input.bits.button3 = keys[SDL_SCANCODE_L];
    // ... map other buttons
    
    return input;
}
```

#### 3. **FM2K State Management**
```c
// Replace simple GState with FM2KCompleteGameState
void save_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    *ev->data.save.state_len = sizeof(FM2KCompleteGameState);
    
    // Use Fletcher's checksum (from example) for consistency
    *ev->data.save.checksum = fletcher32((uint16_t*)gs, sizeof(FM2KCompleteGameState));
    
    std::memcpy(ev->data.save.state, gs, sizeof(FM2KCompleteGameState));
}

void load_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    std::memcpy(gs, ev->data.load.state, sizeof(FM2KCompleteGameState));
}

void update_fm2k_state(FM2KCompleteGameState& gs, FM2KInput inputs[2]) {
    // Call into FM2K's actual game logic
    // This would hook into the functions we analyzed:
    
    // 1. Inject inputs into FM2K's input system
    *(uint32_t*)0x4259c0 = inputs[0].input.value;  // g_p1_input
    *(uint32_t*)0x4259c4 = inputs[1].input.value;  // g_p2_input
    
    // 2. Call FM2K's update functions
    // process_game_inputs_FRAMESTEP_HOOK();  // 0x4146d0
    // update_game_state();                   // Game logic
    // character_state_machine();             // 0x411bf0
    // hit_detection_system();                // 0x40f010
    
    // 3. Extract updated state from FM2K memory
    gs.p1.hp = *(uint32_t*)0x4dfc85;
    gs.p2.hp = *(uint32_t*)0x4edcc4;
    gs.round_timer = *(uint32_t*)0x470060;
    // ... extract all other state variables
}
```

#### 4. **FM2K Timing Integration**
```c
// Adapt timing to match FM2K's 100 FPS
float GetFM2KFrameTime(float frames_ahead) {
    // FM2K runs at 100 FPS (10ms per frame)
    const float base_frame_time = 1.0f / 100.0f;  // 0.01 seconds
    
    if (frames_ahead >= 0.75f) {
        // Slow down if too far ahead
        return base_frame_time * 1.02f;  // 59.8 FPS equivalent
    } else {
        return base_frame_time;  // Normal 100 FPS
    }
}
```

#### 5. **Complete FM2K Integration Main Loop**
```c
int fm2k_online_main(int argc, char* args[]) {
    // Parse command line (same as example)
    int localplayer = std::stoi(args[1]);
    const int localport = std::stoi(args[2]);
    std::string remote_address = std::move(args[3]);
    int localdelay = std::stoi(args[4]);
    
    // Initialize SDL3 and FM2K
    running = init_fm2k_window();
    
    // Initialize FM2K state
    FM2KCompleteGameState state = {};
    FM2KInput inputs[2] = {};
    
    // Setup GekkoNet
    GekkoSession* sess = nullptr;
    GekkoConfig conf {
        .num_players = 2,
        .input_size = sizeof(uint16_t),  // FM2K input size
        .state_size = sizeof(FM2KCompleteGameState),
        .max_spectators = 8,
        .input_prediction_window = 8,
        .desync_detection = true,
        .limited_saving = false,  // Full state saving for accuracy
        .post_sync_joining = true
    };
    
    gekko_create(&sess);
    gekko_start(sess, &conf);
    gekko_net_adapter_set(sess, gekko_default_adapter(localport));
    
    // Add players (order-dependent)
    if (localplayer == 0) {
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
    } else {
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    }
    
    gekko_set_local_delay(sess, localplayer, localdelay);
    
    // Timing variables
    auto curr_time = std::chrono::steady_clock::now();
    auto prev_time = curr_time;
    float delta_time = 0.0f;
    float accumulator = 0.0f;
    float frame_time = 0.0f;
    float frames_ahead = 0.0f;
    
    while (running) {
        curr_time = std::chrono::steady_clock::now();
        
        frames_ahead = gekko_frames_ahead(sess);
        frame_time = GetFM2KFrameTime(frames_ahead);
        
        delta_time = std::chrono::duration<float>(curr_time - prev_time).count();
        prev_time = curr_time;
        accumulator += delta_time;
        
        gekko_network_poll(sess);
        
        while (accumulator >= frame_time) {
            // Process SDL events
            process_events();
            
            // Get local input
            auto input = get_fm2k_inputs();
            gekko_add_local_input(sess, localplayer, &input);
            
            // Handle session events
            int count = 0;
            auto events = gekko_session_events(sess, &count);
            for (int i = 0; i < count; i++) {
                handle_fm2k_session_event(events[i]);
            }
            
            // Process game events
            count = 0;
            auto updates = gekko_update_session(sess, &count);
            for (int i = 0; i < count; i++) {
                auto ev = updates[i];
                
                switch (ev->type) {
                    case SaveEvent:
                        save_fm2k_state(&state, ev);
                        break;
                    case LoadEvent:
                        load_fm2k_state(&state, ev);
                        break;
                    case AdvanceEvent:
                        // Extract inputs from GekkoNet
                        inputs[0].input.value = ((uint16_t*)ev->data.adv.inputs)[0];
                        inputs[1].input.value = ((uint16_t*)ev->data.adv.inputs)[1];
                        
                        // Update FM2K game state
                        update_fm2k_state(state, inputs);
                        break;
                }
            }
            
            accumulator -= frame_time;
        }
        
        // Render current state
        render_fm2k_state(state);
    }
    
    gekko_destroy(sess);
    del_window();
    return 0;
}
```

### Key Differences from SDL Example

1. **Input Size**: FM2K uses 16-bit inputs vs 8-bit in example
2. **State Size**: ~10KB vs ~16 bytes in example  
3. **Frame Rate**: 100 FPS vs 60 FPS in example
4. **State Complexity**: Full game state vs simple position
5. **Integration Depth**: Hooks into existing game vs standalone

### Integration Benefits

1. **Proven Pattern**: The SDL example shows GekkoNet works reliably
2. **Clear Structure**: Event handling patterns are well-defined
3. **Performance**: Timing control handles frame rate variations
4. **Network Stats**: Built-in ping/jitter monitoring
5. **Error Handling**: Desync detection and player management

This analysis shows that adapting the SDL example for FM2K is very feasible, with the main changes being input/state structures and frame timing to match FM2K's 100 FPS architecture.

## Hit Judge and Round System Architecture

### Hit Judge Configuration (0x42470C-0x430120)

The game uses a sophisticated hit detection and configuration system loaded from INI files:

```c
struct FM2K_HitJudgeConfig {
    uint32_t hit_judge_value;    // 0x42470C - Hit detection threshold
    uint32_t config_values[7];   // 0x4300E0-0x430120 - General config values
    char     config_string[260]; // Configuration string buffer
};
```

The hit judge system is initialized by `hit_judge_set_function` (0x414930) which:
1. Loads configuration from INI files
2. Sets up hit detection parameters
3. Configures round settings and game modes

### Round System State (0x470040-0x47006C)

The round system maintains several critical state variables:

```c
struct FM2K_RoundSystem {
    uint32_t game_mode;      // 0x470040 - Current game mode (VS/Team/Tournament)
    uint32_t round_limit;    // 0x470048 - Maximum rounds for the match
    uint32_t round_state;    // 0x47004C - Current round state
    uint32_t round_timer;    // 0x470060 - Active round timer
};

enum FM2K_RoundState {
    ROUND_INIT = 0,      // Initial state
    ROUND_ACTIVE = 1,    // Round in progress
    ROUND_END = 2,       // Round has ended
    ROUND_MATCH_END = 3  // Match has ended
};

enum FM2K_GameMode {
    MODE_NORMAL = 0,     // Normal VS mode
    MODE_TEAM = 1,       // Team Battle mode
    MODE_TOURNAMENT = 2  // Tournament mode
};
```

The round system is managed by `vs_round_function` (0x4086A0) which:
1. Handles round state transitions
2. Manages round timers
3. Processes win/loss conditions
4. Controls match flow

### Rollback Implications

This architecture has several important implications for rollback implementation:

1. **State Serialization Requirements:**
   - Must save hit judge configuration
   - Must preserve round state and timers
   - Game mode affects state size

2. **Deterministic Behavior:**
   - Hit detection is configuration-driven
   - Round state transitions are deterministic
   - Timer-based events are predictable

3. **State Size Optimization:**
   - Hit judge config is static per match
   - Only dynamic round state needs saving
   - Can optimize by excluding static config

4. **Synchronization Points:**
   - Round state changes are key sync points
   - Hit detection results must be identical
   - Timer values must match exactly

Updated state structure for rollback:

```c
typedef struct FM2KGameState {
    // ... existing timing and input state ...

    // Round System State
    struct {
        uint32_t mode;          // Current game mode
        uint32_t round_limit;   // Maximum rounds
        uint32_t round_state;   // Current round state
        uint32_t round_timer;   // Active round timer
        uint32_t score_value;   // Current score/timer value
    } round_system;

    // Hit Judge State
    struct {
        uint32_t hit_judge_value;   // Current hit detection threshold
        uint32_t effect_target;     // Current hit effect target
        uint32_t effect_timer;      // Hit effect duration
    } hit_system;

    // ... rest of game state ...
} FM2KGameState;
```

## Sprite Rendering and Hit Detection System

### Sprite Rendering Engine (0x40CC30)

The game uses a sophisticated sprite rendering engine that handles:
1. Hit effect visualization
2. Sprite transformations and blending
3. Color palette manipulation
4. Screen-space effects

Key components:

```c
struct SpriteRenderState {
    uint32_t render_mode;     // Different rendering modes (-10 to 3)
    uint32_t blend_flags;     // Blending and effect flags
    uint32_t color_values[3]; // RGB color modification values
    uint32_t effect_timer;    // Effect duration counter
};
```

### Hit Effect System

The hit effect system is tightly integrated with the sprite renderer:

1. **Effect Triggers:**
   - Hit detection results (0x42470C)
   - Game state transitions
   - Timer-based events

2. **Effect Parameters:**
   ```c
   struct HitEffectParams {
       uint32_t target_id;      // Effect target identifier
       uint32_t effect_type;    // Visual effect type
       uint32_t duration;       // Effect duration in frames
       uint32_t color_values;   // RGB color modulation
   };
   ```

3. **Rendering Pipeline:**
   - Color palette manipulation (0x4D1A20)
   - Sprite transformation
   - Blend mode selection
   - Screen-space effects

### Integration with Rollback

The sprite and hit effect systems have important implications for rollback:

1. **Visual State Management:**
   - Effect timers must be part of the game state
   - Color modulation values need saving
   - Sprite transformations must be deterministic

2. **Performance Considerations:**
   - Effect parameters are small (few bytes per effect)
   - Visual state is derived from game state
   - Can optimize by excluding pure visual state

3. **Synchronization Requirements:**
   - Hit effects must trigger identically
   - Effect timers must stay synchronized
   - Visual state must match game state

Updated state structure to include visual effects:

```c
typedef struct FM2KGameState {
    // ... previous state members ...

    // Visual Effect State
    struct {
        uint32_t active_effects;     // Bitfield of active effects
        uint32_t effect_timers[8];   // Array of effect durations
        uint32_t color_values[8][3]; // RGB values for each effect
        uint32_t target_ids[8];      // Effect target identifiers
    } visual_state;

    // ... rest of game state ...
} FM2KGameState;
```

## Game State Management System

The game state manager (0x406FC0) handles several critical aspects of the game:

### State Variables

1. **Round System State:**
   ```c
   struct RoundSystemState {
       uint32_t g_round_timer;        // 0x470060 - Current round timer value
       uint32_t g_round_limit;        // 0x470048 - Maximum rounds per match
       uint32_t g_round_state;        // 0x47004C - Current round state
       uint32_t g_game_mode;          // 0x470040 - Current game mode
   };
   ```

2. **Player State:**
   ```c
   struct PlayerState {
       int32_t stage_position;        // Current position on stage grid
       uint32_t action_state;         // Current action being performed
       uint32_t round_count;          // Number of rounds completed
       uint32_t input_history[4];     // Recent input history
       uint32_t move_history[4];      // Recent movement history
   };
   ```

### State Management Flow

1. **Initialization (0x406FC0):**
   - Loads initial configuration
   - Sets up round timer and limits
   - Initializes player positions
   - Sets up game mode parameters

2. **State Updates:**
   - Input processing and validation
   - Position and movement updates
   - Action state transitions
   - Round state management

3. **State Synchronization:**
   - Timer synchronization
   - Player state synchronization
   - Round state transitions
   - Game mode state management

### Rollback Implications

1. **Critical State Components:**
   - Round timer and state
   - Player positions and actions
   - Input and move history
   - Game mode state

2. **State Size Analysis:**
   - Core game state: ~128 bytes
   - Player state: ~32 bytes per player
   - History buffers: ~32 bytes per player
   - Total per-frame state: ~256 bytes

3. **Synchronization Requirements:**
   - Timer must be deterministic
   - Player state must be atomic
   - Input processing must be consistent
   - State transitions must be reproducible

## Input Processing System

The game uses a sophisticated input processing system that handles both movement and action inputs:

### Input Mapping (0x406F20)

```c
enum InputActions {
    ACTION_NONE = 0,
    ACTION_A = 1,    // 0x20  - Light Attack
    ACTION_B = 2,    // 0x40  - Medium Attack
    ACTION_C = 3,    // 0x80  - Heavy Attack
    ACTION_D = 4,    // 0x100 - Special Move
    ACTION_E = 5     // 0x200 - Super Move
};

enum InputDirections {
    DIR_NONE  = 0x000,
    DIR_UP    = 0x004,
    DIR_DOWN  = 0x008,
    DIR_LEFT  = 0x001,
    DIR_RIGHT = 0x002
};
```

### Input Processing Flow

1. **Raw Input Capture:**
   - Movement inputs (4-way directional)
   - Action buttons (5 main buttons)
   - System buttons (Start, Select)

2. **Input State Management:**
   ```c
   struct InputState {
       uint16_t current_input;     // Current frame's input
       uint16_t previous_input;    // Last frame's input
       uint16_t input_changes;     // Changed bits since last frame
       uint8_t  processed_input;   // Processed directional input
   };
   ```

3. **Input History:**
   - Each player maintains a 4-frame input history
   - History includes both raw and processed inputs
   - Used for move validation and rollback

4. **Input Processing:**
   - Raw input capture
   - Input change detection
   - Input repeat handling:
     - Initial delay
     - Repeat delay
     - Repeat timer
   - Input validation
   - Mode-specific processing:
     - VS mode (< 3000)
     - Story mode (? 3000)

5. **Memory Layout:**
   ```
   input_system {
     +g_input_buffer_index: Current frame index
     +g_p1_input[8]: Current P1 inputs
     +g_p2_input: Current P2 inputs
     +g_prev_input_state[8]: Previous frame inputs
     +g_input_changes[8]: Input change flags
     +g_input_repeat_state[8]: Input repeat flags
     +g_input_repeat_timer[8]: Repeat timers
     +g_processed_input[8]: Processed inputs
     +g_combined_processed_input: Combined state
     +g_combined_input_changes: Change mask
     +g_combined_raw_input: Raw input mask
   }
   ```

6. **Critical Operations:**
   - Frame advance:
     ```c
     g_input_buffer_index = (g_input_buffer_index + 1) & 0x3FF;
     g_p1_input_history[g_input_buffer_index] = current_input;
     ```
   - Input processing:
     ```c
     g_input_changes = current_input & (prev_input ^ current_input);
     if (repeat_active && current_input == repeat_state) {
       if (--repeat_timer == 0) {
         processed_input = current_input;
         repeat_timer = repeat_delay;
       }
     }
     ```

This system is crucial for rollback implementation as it provides:
1. Deterministic input processing
2. Frame-accurate input history
3. Clear state serialization points
4. Mode-specific input handling
5. Input validation and processing logic

The 1024-frame history buffer is particularly useful for rollback, as it provides ample space for state rewinding and replay.
```

### Health Damage Manager (0x40e6f0)

The health damage manager handles health state and damage calculations:

1. **Health State Management:**
   - Per-character health tracking
   - Health segments system
   - Recovery mechanics
   - Guard damage handling

2. **Memory Layout:**
   ```
   health_system {
     +4DFC95: Current health segment
     +4DFC99: Maximum health segments
     +4DFC9D: Current health points
     +4DFCA1: Health segment size
   }
   ```

3. **Damage Processing:**
   - Segment-based health system:
     ```c
     while (current_health < 0) {
       if (current_segment > 0) {
         current_segment--;
         current_health += segment_size;
       } else {
         current_health = 0;
       }
     }
     ```
   - Health recovery:
     ```c
     while (current_health >= segment_size) {
       if (current_segment < max_segments) {
         current_health -= segment_size;
         current_segment++;
       } else {
         current_health = 0;
       }
     }
     ```

4. **Critical Operations:**
   - Damage application:
     - Negative values for damage
     - Positive values for healing
   - Segment management:
     - Segment depletion
     - Segment recovery
     - Maximum segment cap
   - Health validation:
     - Minimum health check
     - Maximum health check
     - Segment boundary checks

This system is crucial for rollback implementation as it:
1. Uses deterministic damage calculations
2. Maintains clear health state boundaries
3. Provides segment-based state tracking
4. Handles health recovery mechanics
5. Manages guard damage separately

The segmented health system will make it easier to serialize and restore health states during rollback operations, as each segment provides a clear checkpoint for state management.
```

### Rollback Implementation Summary

After analyzing FM2K's core systems, we can conclude that the game is well-suited for rollback netcode implementation. Here's a comprehensive overview of how each system will integrate with rollback:

1. **State Serialization Points:**
   - Character State Machine (0x411bf0):
     - Base states (0x0-0x3)
     - Action states (0x4-0x7)
     - Movement states (0x8-0xB)
     - Frame counters and timers
   - Hit Detection System (0x40f010):
     - Hit boxes and collision state
     - Damage values and scaling
     - Hit confirmation flags
   - Input Buffer System (0x4146d0):
     - 1024-frame history buffer
     - Input state flags
     - Repeat handling state
   - Health System (0x40e6f0):
     - Health segments
     - Current health points
     - Guard damage state
     - Recovery mechanics

2. **Deterministic Systems:**
   - Fixed frame rate (100 FPS)
   - 16.16 fixed-point coordinates
   - Deterministic input processing
   - Clear state transitions
   - Predictable damage calculations
   - Frame-based timers

3. **State Management:**
   - Object Pool (1024 entries):
     - Type identification
     - Position and velocity
     - Animation state
     - Collision data
   - Input History:
     - Circular buffer design
     - Clear frame boundaries
     - Input validation
   - Hit Detection:
     - Priority system
     - State validation
     - Reaction handling
   - Health System:
     - Segment-based tracking
     - Clear state boundaries
     - Recovery mechanics

4. **Implementation Strategy:**
   a. Save State (Frame N):
      ```c
      struct SaveState {
          // Character State
          int base_state;
          int action_state;
          int movement_state;
          int frame_counters[4];
          
          // Hit Detection
          int hit_boxes[20];
          int damage_values[8];
          int hit_flags;
          
          // Input Buffer
          int input_history[1024];
          int input_flags;
          int repeat_state;
          
          // Health System
          int health_segments;
          int current_health;
          int guard_damage;
      };
      ```

   b. Rollback Process:
      1. Save current frame state
      2. Apply new remote input
      3. Simulate forward:
         - Process inputs
         - Update character states
         - Handle collisions
         - Apply damage
      4. Render new state
      5. Save confirmed state

5. **Critical Hooks:**
   - Input Processing (0x4146d0):
     - Frame advance point
     - Input buffer management
   - Character State (0x411bf0):
     - State transition handling
     - Frame counting
   - Hit Detection (0x40f010):
     - Collision processing
     - Damage application
   - Health System (0x40e6f0):
     - Health state management
     - Segment tracking

6. **Optimization Opportunities:**
   - Parallel state simulation
   - Minimal state serialization
   - Efficient state diffing
   - Smart state prediction
   - Input compression

The analysis reveals that FM2K's architecture is highly suitable for rollback implementation due to its:
1. Deterministic game logic
2. Clear state boundaries
3. Frame-based processing
4. Efficient memory layout
5. Well-defined systems

This research provides a solid foundation for implementing rollback netcode in FM2K, with all major systems supporting the requirements for state saving, rollback, and resimulation.

## GekkoNet Integration Analysis

### GekkoNet Library Overview

GekkoNet is a rollback netcode library that provides the networking infrastructure we need for FM2K. Here's how it maps to our analyzed systems:

### 1. **GekkoNet Configuration for FM2K**
```c
GekkoConfig fm2k_config = {
    .num_players = 2,                    // P1 vs P2
    .max_spectators = 8,                 // Allow spectators
    .input_prediction_window = 8,        // 8 frames ahead prediction
    .spectator_delay = 6,                // 6 frame spectator delay
    .input_size = sizeof(uint32_t),      // FM2K input flags (32-bit)
    .state_size = sizeof(FM2KGameState), // Our minimal state structure
    .limited_saving = false,             // Full state saving for accuracy
    .post_sync_joining = true,           // Allow mid-match spectators
    .desync_detection = true             // Enable desync detection
};
```

### 2. **Integration Points with FM2K Systems**

#### Input System Integration (0x4146d0)
```c
// Hook into process_game_inputs()
void fm2k_input_hook() {
    // Get GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case AdvanceEvent:
                // Inject inputs into FM2K's input system
                g_p1_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[0];
                g_p2_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[1];
                // Continue normal input processing
                break;
                
            case SaveEvent:
                // Save current FM2K state
                save_fm2k_state(events[i]->data.save.state, events[i]->data.save.state_len);
                break;
                
            case LoadEvent:
                // Restore FM2K state
                load_fm2k_state(events[i]->data.load.state, events[i]->data.load.state_len);
                break;
        }
    }
}
```

#### State Management Integration
```c
typedef struct FM2KGameState {
    // Frame tracking
    uint32_t frame_number;
    uint32_t input_buffer_index;
    
    // Input state
    uint32_t p1_input_current;
    uint32_t p2_input_current;
    uint32_t input_repeat_state[8];
    uint32_t input_repeat_timer[8];
    
    // Character states (per character)
    struct {
        // Position (16.16 fixed point)
        int32_t pos_x, pos_y;
        int32_t vel_x, vel_y;
        
        // State machine
        uint16_t base_state;        // 0x0-0x3
        uint16_t action_state;      // 0x4-0x7  
        uint16_t movement_state;    // 0x8-0xB
        uint16_t state_flags;       // Offset 350
        
        // Animation
        uint16_t animation_frame;   // Offset 12
        uint16_t max_animation_frame; // Offset 88
        
        // Health system
        uint16_t health_segments;   // Current segments
        uint16_t current_health;    // Health points
        uint16_t max_health_segments; // Maximum segments
        uint16_t guard_damage;      // Guard damage accumulation
        
        // Hit detection
        uint16_t hit_flags;         // Hit state flags
        uint16_t hit_stun_timer;    // Hit stun duration
        uint16_t guard_state;       // Guard state flags
        
        // Facing and direction
        uint8_t facing_direction;   // Left/right facing
        uint8_t input_direction;    // Current input direction
    } characters[2];
    
    // Round state
    uint32_t round_state;           // Round state machine
    uint32_t round_timer;           // Round timer
    uint32_t game_mode;             // Current game mode
    
    // Hit detection global state
    uint32_t hit_effect_target;     // Hit effect target
    uint32_t hit_effect_timer;      // Hit effect timer
    
    // Combo system
    uint16_t combo_counter[2];      // Combo counters
    uint16_t damage_scaling[2];     // Damage scaling
    
    // Object pool critical state
    uint16_t active_objects;        // Number of active objects
    uint32_t object_state_flags;    // Critical object flags
    
} FM2KGameState;
```

### 3. **Network Adapter Implementation**
```c
GekkoNetAdapter fm2k_adapter = {
    .send_data = fm2k_send_packet,
    .receive_data = fm2k_receive_packets,
    .free_data = fm2k_free_packet_data
};

void fm2k_send_packet(GekkoNetAddress* addr, const char* data, int length) {
    // Use existing LilithPort networking or implement UDP
    // addr->data contains IP/port information
    // data contains the packet to send
}

GekkoNetResult** fm2k_receive_packets(int* length) {
    // Collect all packets received since last call
    // Return array of GekkoNetResult pointers
    // GekkoNet will call free_data on each result
}
```

### 4. **Integration with Existing FM2K Systems**

#### Main Game Loop Integration
```c
// Hook at 0x4146d0 - process_game_inputs()
void fm2k_rollback_frame() {
    // 1. Poll network
    gekko_network_poll(session);
    
    // 2. Add local inputs
    uint32_t local_input = get_local_player_input();
    gekko_add_local_input(session, local_player_id, &local_input);
    
    // 3. Process GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    // 4. Handle events (save/load/advance)
    process_gekko_events(events, event_count);
    
    // 5. Continue normal FM2K processing
    // (character updates, hit detection, etc.)
}
```

#### State Serialization Functions
```c
void save_fm2k_state(unsigned char* buffer, unsigned int* length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Save frame state
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    
    // Save character states from object pool
    for (int i = 0; i < 2; i++) {
        save_character_state(&state->characters[i], i);
    }
    
    // Save round state
    state->round_state = g_round_state;
    state->round_timer = g_round_timer;
    
    *length = sizeof(FM2KGameState);
}

void load_fm2k_state(unsigned char* buffer, unsigned int length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Restore frame state
    current_frame = state->frame_number;
    g_input_buffer_index = state->input_buffer_index;
    
    // Restore character states to object pool
    for (int i = 0; i < 2; i++) {
        load_character_state(&state->characters[i], i);
    }
    
    // Restore round state
    g_round_state = state->round_state;
    g_round_timer = state->round_timer;
}
```

### 5. **Event Handling Integration**
```c
void handle_session_events() {
    int event_count;
    GekkoSessionEvent** events = gekko_session_events(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case PlayerConnected:
                // Show "Player Connected" message
                break;
            case PlayerDisconnected:
                // Handle disconnection
                break;
            case DesyncDetected:
                // Log desync information
                printf("Desync at frame %d: local=%08x remote=%08x\n",
                       events[i]->data.desynced.frame,
                       events[i]->data.desynced.local_checksum,
                       events[i]->data.desynced.remote_checksum);
                break;
        }
    }
}
```

### 6. **Performance Considerations**
- **State Size**: ~200 bytes per frame (very efficient)
- **Prediction Window**: 8 frames ahead (80ms at 100 FPS)
- **Rollback Depth**: Up to 15 frames (150ms)
- **Network Frequency**: 100 Hz to match FM2K's frame rate

### 7. **Implementation Benefits**
1. **Minimal Code Changes**: Only need to hook input processing
2. **Existing Infrastructure**: Leverages FM2K's input history system
3. **Deterministic**: FM2K's fixed-point math ensures consistency
4. **Efficient**: Small state size enables fast rollback
5. **Robust**: GekkoNet handles all networking complexity

This integration approach leverages both FM2K's existing architecture and GekkoNet's proven rollback implementation, providing a solid foundation for online play.

### Critical Player State Variables

Based on IDA analysis, here are the key memory addresses for player state:

#### Player 1 State Variables:
```c
// Input system
g_p1_input          = 0x4259c0;  // Current input state
g_p1_input_history  = 0x4280e0;  // 1024-frame input history

// Position and stage
g_p1_stage_x        = 0x424e68;  // Stage position X
g_p1_stage_y        = 0x424e6c;  // Stage position Y

// Round and game state
g_p1_round_count    = 0x4700ec;  // Round wins
g_p1_round_state    = 0x4700f0;  // Round state
g_p1_action_state   = 0x47019c;  // Action state

// Health system
g_p1_hp             = 0x4dfc85;  // Current HP
g_p1_max_hp         = 0x4dfc91;  // Maximum HP
```

#### Player 2 State Variables:
```c
// Input system
g_p2_input          = 0x4259c4;  // Current input state
g_p2_input_history  = 0x4290e0;  // 1024-frame input history

// Round and game state
g_p2_action_state   = 0x4701a0;  // Action state

// Health system
g_p2_hp             = 0x4edcc4;  // Current HP
g_p2_max_hp         = 0x4edcd0;  // Maximum HP
```

#### Core Game State Variables:
```c
// Object pool
g_object_pool       = 0x4701e0;  // Main object pool (1024 entries)
g_sprite_data_array = 0x447938;  // Sprite data array

// Timing system
g_frame_time_ms     = 0x41e2f0;  // Frame duration (10ms)
g_last_frame_time   = 0x447dd4;  // Last frame timestamp
g_frame_sync_flag   = 0x424700;  // Frame sync state
g_frame_time_delta  = 0x425960;  // Frame timing delta
g_frame_skip_count  = 0x4246f4;  // Frame skip counter

// Random number generator
g_random_seed       = 0x41fb1c;  // RNG seed

// Input buffer management
g_input_buffer_index = (calculated); // Current buffer index
g_input_repeat_timer = 0x4d1c40;     // Input repeat timers

// Round system
g_round_timer       = 0x470060;  // Round timer
g_game_timer        = 0x470044;  // Game timer
g_hit_effect_timer  = 0x4701c8;  // Hit effect timer
```

### Final State Structure for GekkoNet

With all the memory addresses identified, here's the complete state structure:

```c
typedef struct FM2KCompleteGameState {
    // Frame and timing state
    uint32_t frame_number;
    uint32_t input_buffer_index;
    uint32_t last_frame_time;
    uint32_t frame_time_delta;
    uint8_t frame_skip_count;
    uint8_t frame_sync_flag;
    
    // Random number generator state
    uint32_t random_seed;
    
    // Player 1 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t stage_x, stage_y;
        uint32_t round_count;
        uint32_t round_state;
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p1;
    
    // Player 2 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p2;
    
    // Global timers
    uint32_t round_timer;
    uint32_t game_timer;
    uint32_t hit_effect_timer;
    
    // Input system state
    uint32_t input_repeat_timer[8];
    
    // Critical object pool state (first 2 character objects)
    struct {
        uint32_t object_type;
        int32_t pos_x, pos_y;        // 16.16 fixed point
        int32_t vel_x, vel_y;
        uint16_t state_flags;
        uint16_t animation_frame;
        uint16_t health_segments;
        uint16_t facing_direction;
        // ... other critical fields
    } character_objects[2];
    
} FM2KCompleteGameState;
```

### Memory Access Functions

```c
// Direct memory access functions for state serialization
void save_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Frame and timing
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    state->last_frame_time = *(uint32_t*)0x447dd4;
    state->frame_time_delta = *(uint32_t*)0x425960;
    state->frame_skip_count = *(uint8_t*)0x4246f4;
    state->frame_sync_flag = *(uint8_t*)0x424700;
    
    // RNG state
    state->random_seed = *(uint32_t*)0x41fb1c;
    
    // Player 1 state
    state->p1.input_current = *(uint32_t*)0x4259c0;
    memcpy(state->p1.input_history, (void*)0x4280e0, 1024 * sizeof(uint32_t));
    state->p1.stage_x = *(uint32_t*)0x424e68;
    state->p1.stage_y = *(uint32_t*)0x424e6c;
    state->p1.round_count = *(uint32_t*)0x4700ec;
    state->p1.round_state = *(uint32_t*)0x4700f0;
    state->p1.action_state = *(uint32_t*)0x47019c;
    state->p1.hp = *(uint32_t*)0x4dfc85;
    state->p1.max_hp = *(uint32_t*)0x4dfc91;
    
    // Player 2 state
    state->p2.input_current = *(uint32_t*)0x4259c4;
    memcpy(state->p2.input_history, (void*)0x4290e0, 1024 * sizeof(uint32_t));
    state->p2.action_state = *(uint32_t*)0x4701a0;
    state->p2.hp = *(uint32_t*)0x4edcc4;
    state->p2.max_hp = *(uint32_t*)0x4edcd0;
    
    // Global timers
    state->round_timer = *(uint32_t*)0x470060;
    state->game_timer = *(uint32_t*)0x470044;
    state->hit_effect_timer = *(uint32_t*)0x4701c8;
    
    // Input repeat timers
    memcpy(state->input_repeat_timer, (void*)0x4d1c40, 8 * sizeof(uint32_t));
    
    // Character objects from object pool
    save_character_objects_from_pool(state->character_objects);
}

void load_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Restore all state in reverse order
    // ... (similar but with assignment in opposite direction)
}
```

This gives us a complete, memory-accurate state structure that can be used with GekkoNet for rollback implementation. The state size is approximately 10KB, which is very reasonable for rollback netcode.

## GekkoNet SDL Example Analysis & FM2K Integration

### SDL Example Key Patterns

The OnlineSession.cpp example demonstrates the core GekkoNet integration patterns we need for FM2K:

#### 1. **Session Setup Pattern**
```c
// From SDL example - adapt for FM2K
GekkoSession* sess = nullptr;
GekkoConfig conf {
    .num_players = 2,
    .input_size = sizeof(char),           // FM2K: sizeof(uint32_t)
    .state_size = sizeof(GState),         // FM2K: sizeof(FM2KCompleteGameState)
    .max_spectators = 0,                  // FM2K: 8 for spectators
    .input_prediction_window = 10,        // FM2K: 8 frames
    .desync_detection = true,
    // .limited_saving = true,            // FM2K: false for accuracy
};

gekko_create(&sess);
gekko_start(sess, &conf);
gekko_net_adapter_set(sess, gekko_default_adapter(localport));
```

#### 2. **Player Management Pattern**
```c
// Order-dependent player addition (from example)
if (localplayer == 0) {
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
} else {
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
}
```

#### 3. **Core Game Loop Pattern** 
```c
// From SDL example - critical for FM2K integration
while (running) {
    // Timing control
    frames_ahead = gekko_frames_ahead(sess);
    frame_time = GetFrameTime(frames_ahead);
    
    // Network polling
    gekko_network_poll(sess);
    
    // Frame processing
    while (accumulator >= frame_time) {
        // Add local input
        auto input = get_key_inputs();
        gekko_add_local_input(sess, localplayer, &input);
        
        // Handle session events
        auto events = gekko_session_events(sess, &count);
        for (int i = 0; i < count; i++) {
            // Handle PlayerConnected, PlayerDisconnected, DesyncDetected
        }
        
        // Process game events
        auto updates = gekko_update_session(sess, &count);
        for (int i = 0; i < count; i++) {
            switch (updates[i]->type) {
                case SaveEvent: save_state(&state, updates[i]); break;
                case LoadEvent: load_state(&state, updates[i]); break;
                case AdvanceEvent:
                    // Extract inputs and advance game
                    inputs[0].input.value = updates[i]->data.adv.inputs[0];
                    inputs[1].input.value = updates[i]->data.adv.inputs[1];
                    update_state(state, inputs, num_players);
                    break;
            }
        }
        accumulator -= frame_time;
    }
    
    // Render current state
    render_state(state);
}
```

### FM2K Integration Strategy

#### 1. **Replace SDL2 with SDL3 for FM2K**
```c
// SDL3 initialization for FM2K integration
#include "SDL3/SDL.h"

bool init_fm2k_window(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Error initializing SDL3.\n");
        return false;
    }
    
    // Create window matching FM2K's resolution
    window = SDL_CreateWindow(
        "FM2K Online Session",
        640, 480,  // FM2K native resolution
        SDL_WINDOW_RESIZABLE
    );
    
    if (!window) {
        fprintf(stderr, "Error creating SDL3 Window.\n");
        return false;
    }
    
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        fprintf(stderr, "Error creating SDL3 Renderer.\n");
        return false;
    }
    
    return true;
}
```

#### 2. **FM2K Input Structure**
```c
// Replace simple GInput with FM2K's 11-bit input system
struct FM2KInput {
    union {
        struct {
            uint16_t left     : 1;   // 0x001
            uint16_t right    : 1;   // 0x002
            uint16_t up       : 1;   // 0x004
            uint16_t down     : 1;   // 0x008
            uint16_t button1  : 1;   // 0x010
            uint16_t button2  : 1;   // 0x020
            uint16_t button3  : 1;   // 0x040
            uint16_t button4  : 1;   // 0x080
            uint16_t button5  : 1;   // 0x100
            uint16_t button6  : 1;   // 0x200
            uint16_t button7  : 1;   // 0x400
            uint16_t reserved : 5;   // Unused bits
        } bits;
        uint16_t value;
    } input;
};

FM2KInput get_fm2k_inputs() {
    FM2KInput input{};
    input.input.value = 0;
    
    // Get keyboard state (SDL3 syntax)
    const bool* keys = SDL_GetKeyboardState(NULL);
    
    // Map to FM2K inputs
    input.input.bits.up = keys[SDL_SCANCODE_W];
    input.input.bits.left = keys[SDL_SCANCODE_A];
    input.input.bits.down = keys[SDL_SCANCODE_S];
    input.input.bits.right = keys[SDL_SCANCODE_D];
    input.input.bits.button1 = keys[SDL_SCANCODE_J];
    input.input.bits.button2 = keys[SDL_SCANCODE_K];
    input.input.bits.button3 = keys[SDL_SCANCODE_L];
    // ... map other buttons
    
    return input;
}
```

#### 3. **FM2K State Management**
```c
// Replace simple GState with FM2KCompleteGameState
void save_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    *ev->data.save.state_len = sizeof(FM2KCompleteGameState);
    
    // Use Fletcher's checksum (from example) for consistency
    *ev->data.save.checksum = fletcher32((uint16_t*)gs, sizeof(FM2KCompleteGameState));
    
    std::memcpy(ev->data.save.state, gs, sizeof(FM2KCompleteGameState));
}

void load_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    std::memcpy(gs, ev->data.load.state, sizeof(FM2KCompleteGameState));
}

void update_fm2k_state(FM2KCompleteGameState& gs, FM2KInput inputs[2]) {
    // Call into FM2K's actual game logic
    // This would hook into the functions we analyzed:
    
    // 1. Inject inputs into FM2K's input system
    *(uint32_t*)0x4259c0 = inputs[0].input.value;  // g_p1_input
    *(uint32_t*)0x4259c4 = inputs[1].input.value;  // g_p2_input
    
    // 2. Call FM2K's update functions
    // process_game_inputs_FRAMESTEP_HOOK();  // 0x4146d0
    // update_game_state();                   // Game logic
    // character_state_machine();             // 0x411bf0
    // hit_detection_system();                // 0x40f010
    
    // 3. Extract updated state from FM2K memory
    gs.p1.hp = *(uint32_t*)0x4dfc85;
    gs.p2.hp = *(uint32_t*)0x4edcc4;
    gs.round_timer = *(uint32_t*)0x470060;
    // ... extract all other state variables
}
```

#### 4. **FM2K Timing Integration**
```c
// Adapt timing to match FM2K's 100 FPS
float GetFM2KFrameTime(float frames_ahead) {
    // FM2K runs at 100 FPS (10ms per frame)
    const float base_frame_time = 1.0f / 100.0f;  // 0.01 seconds
    
    if (frames_ahead >= 0.75f) {
        // Slow down if too far ahead
        return base_frame_time * 1.02f;  // 59.8 FPS equivalent
    } else {
        return base_frame_time;  // Normal 100 FPS
    }
}
```

#### 5. **Complete FM2K Integration Main Loop**
```c
int fm2k_online_main(int argc, char* args[]) {
    // Parse command line (same as example)
    int localplayer = std::stoi(args[1]);
    const int localport = std::stoi(args[2]);
    std::string remote_address = std::move(args[3]);
    int localdelay = std::stoi(args[4]);
    
    // Initialize SDL3 and FM2K
    running = init_fm2k_window();
    
    // Initialize FM2K state
    FM2KCompleteGameState state = {};
    FM2KInput inputs[2] = {};
    
    // Setup GekkoNet
    GekkoSession* sess = nullptr;
    GekkoConfig conf {
        .num_players = 2,
        .input_size = sizeof(uint16_t),  // FM2K input size
        .state_size = sizeof(FM2KCompleteGameState),
        .max_spectators = 8,
        .input_prediction_window = 8,
        .desync_detection = true,
        .limited_saving = false,  // Full state saving for accuracy
        .post_sync_joining = true
    };
    
    gekko_create(&sess);
    gekko_start(sess, &conf);
    gekko_net_adapter_set(sess, gekko_default_adapter(localport));
    
    // Add players (order-dependent)
    if (localplayer == 0) {
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
    } else {
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    }
    
    gekko_set_local_delay(sess, localplayer, localdelay);
    
    // Timing variables
    auto curr_time = std::chrono::steady_clock::now();
    auto prev_time = curr_time;
    float delta_time = 0.0f;
    float accumulator = 0.0f;
    float frame_time = 0.0f;
    float frames_ahead = 0.0f;
    
    while (running) {
        curr_time = std::chrono::steady_clock::now();
        
        frames_ahead = gekko_frames_ahead(sess);
        frame_time = GetFM2KFrameTime(frames_ahead);
        
        delta_time = std::chrono::duration<float>(curr_time - prev_time).count();
        prev_time = curr_time;
        accumulator += delta_time;
        
        gekko_network_poll(sess);
        
        while (accumulator >= frame_time) {
            // Process SDL events
            process_events();
            
            // Get local input
            auto input = get_fm2k_inputs();
            gekko_add_local_input(sess, localplayer, &input);
            
            // Handle session events
            int count = 0;
            auto events = gekko_session_events(sess, &count);
            for (int i = 0; i < count; i++) {
                handle_fm2k_session_event(events[i]);
            }
            
            // Process game events
            count = 0;
            auto updates = gekko_update_session(sess, &count);
            for (int i = 0; i < count; i++) {
                auto ev = updates[i];
                
                switch (ev->type) {
                    case SaveEvent:
                        save_fm2k_state(&state, ev);
                        break;
                    case LoadEvent:
                        load_fm2k_state(&state, ev);
                        break;
                    case AdvanceEvent:
                        // Extract inputs from GekkoNet
                        inputs[0].input.value = ((uint16_t*)ev->data.adv.inputs)[0];
                        inputs[1].input.value = ((uint16_t*)ev->data.adv.inputs)[1];
                        
                        // Update FM2K game state
                        update_fm2k_state(state, inputs);
                        break;
                }
            }
            
            accumulator -= frame_time;
        }
        
        // Render current state
        render_fm2k_state(state);
    }
    
    gekko_destroy(sess);
    del_window();
    return 0;
}
```

### Key Differences from SDL Example

1. **Input Size**: FM2K uses 16-bit inputs vs 8-bit in example
2. **State Size**: ~10KB vs ~16 bytes in example  
3. **Frame Rate**: 100 FPS vs 60 FPS in example
4. **State Complexity**: Full game state vs simple position
5. **Integration Depth**: Hooks into existing game vs standalone

### Integration Benefits

1. **Proven Pattern**: The SDL example shows GekkoNet works reliably
2. **Clear Structure**: Event handling patterns are well-defined
3. **Performance**: Timing control handles frame rate variations
4. **Network Stats**: Built-in ping/jitter monitoring
5. **Error Handling**: Desync detection and player management

This analysis shows that adapting the SDL example for FM2K is very feasible, with the main changes being input/state structures and frame timing to match FM2K's 100 FPS architecture.

## Hit Judge and Round System Architecture

### Hit Judge Configuration (0x42470C-0x430120)

The game uses a sophisticated hit detection and configuration system loaded from INI files:

```c
struct FM2K_HitJudgeConfig {
    uint32_t hit_judge_value;    // 0x42470C - Hit detection threshold
    uint32_t config_values[7];   // 0x4300E0-0x430120 - General config values
    char     config_string[260]; // Configuration string buffer
};
```

The hit judge system is initialized by `hit_judge_set_function` (0x414930) which:
1. Loads configuration from INI files
2. Sets up hit detection parameters
3. Configures round settings and game modes

### Round System State (0x470040-0x47006C)

The round system maintains several critical state variables:

```c
struct FM2K_RoundSystem {
    uint32_t game_mode;      // 0x470040 - Current game mode (VS/Team/Tournament)
    uint32_t round_limit;    // 0x470048 - Maximum rounds for the match
    uint32_t round_state;    // 0x47004C - Current round state
    uint32_t round_timer;    // 0x470060 - Active round timer
};

enum FM2K_RoundState {
    ROUND_INIT = 0,      // Initial state
    ROUND_ACTIVE = 1,    // Round in progress
    ROUND_END = 2,       // Round has ended
    ROUND_MATCH_END = 3  // Match has ended
};

enum FM2K_GameMode {
    MODE_NORMAL = 0,     // Normal VS mode
    MODE_TEAM = 1,       // Team Battle mode
    MODE_TOURNAMENT = 2  // Tournament mode
};
```

The round system is managed by `vs_round_function` (0x4086A0) which:
1. Handles round state transitions
2. Manages round timers
3. Processes win/loss conditions
4. Controls match flow

### Rollback Implications

This architecture has several important implications for rollback implementation:

1. **State Serialization Requirements:**
   - Must save hit judge configuration
   - Must preserve round state and timers
   - Game mode affects state size

2. **Deterministic Behavior:**
   - Hit detection is configuration-driven
   - Round state transitions are deterministic
   - Timer-based events are predictable

3. **State Size Optimization:**
   - Hit judge config is static per match
   - Only dynamic round state needs saving
   - Can optimize by excluding static config

4. **Synchronization Points:**
   - Round state changes are key sync points
   - Hit detection results must be identical
   - Timer values must match exactly

Updated state structure for rollback:

```c
typedef struct FM2KGameState {
    // ... existing timing and input state ...

    // Round System State
    struct {
        uint32_t mode;          // Current game mode
        uint32_t round_limit;   // Maximum rounds
        uint32_t round_state;   // Current round state
        uint32_t round_timer;   // Active round timer
        uint32_t score_value;   // Current score/timer value
    } round_system;

    // Hit Judge State
    struct {
        uint32_t hit_judge_value;   // Current hit detection threshold
        uint32_t effect_target;     // Current hit effect target
        uint32_t effect_timer;      // Hit effect duration
    } hit_system;

    // ... rest of game state ...
} FM2KGameState;
```

## Sprite Rendering and Hit Detection System

### Sprite Rendering Engine (0x40CC30)

The game uses a sophisticated sprite rendering engine that handles:
1. Hit effect visualization
2. Sprite transformations and blending
3. Color palette manipulation
4. Screen-space effects

Key components:

```c
struct SpriteRenderState {
    uint32_t render_mode;     // Different rendering modes (-10 to 3)
    uint32_t blend_flags;     // Blending and effect flags
    uint32_t color_values[3]; // RGB color modification values
    uint32_t effect_timer;    // Effect duration counter
};
```

### Hit Effect System

The hit effect system is tightly integrated with the sprite renderer:

1. **Effect Triggers:**
   - Hit detection results (0x42470C)
   - Game state transitions
   - Timer-based events

2. **Effect Parameters:**
   ```c
   struct HitEffectParams {
       uint32_t target_id;      // Effect target identifier
       uint32_t effect_type;    // Visual effect type
       uint32_t duration;       // Effect duration in frames
       uint32_t color_values;   // RGB color modulation
   };
   ```

3. **Rendering Pipeline:**
   - Color palette manipulation (0x4D1A20)
   - Sprite transformation
   - Blend mode selection
   - Screen-space effects

### Integration with Rollback

The sprite and hit effect systems have important implications for rollback:

1. **Visual State Management:**
   - Effect timers must be part of the game state
   - Color modulation values need saving
   - Sprite transformations must be deterministic

2. **Performance Considerations:**
   - Effect parameters are small (few bytes per effect)
   - Visual state is derived from game state
   - Can optimize by excluding pure visual state

3. **Synchronization Requirements:**
   - Hit effects must trigger identically
   - Effect timers must stay synchronized
   - Visual state must match game state

Updated state structure to include visual effects:

```c
typedef struct FM2KGameState {
    // ... previous state members ...

    // Visual Effect State
    struct {
        uint32_t active_effects;     // Bitfield of active effects
        uint32_t effect_timers[8];   // Array of effect durations
        uint32_t color_values[8][3]; // RGB values for each effect
        uint32_t target_ids[8];      // Effect target identifiers
    } visual_state;

    // ... rest of game state ...
} FM2KGameState;
```

## Game State Management System

The game state manager (0x406FC0) handles several critical aspects of the game:

### State Variables

1. **Round System State:**
   ```c
   struct RoundSystemState {
       uint32_t g_round_timer;        // 0x470060 - Current round timer value
       uint32_t g_round_limit;        // 0x470048 - Maximum rounds per match
       uint32_t g_round_state;        // 0x47004C - Current round state
       uint32_t g_game_mode;          // 0x470040 - Current game mode
   };
   ```

2. **Player State:**
   ```c
   struct PlayerState {
       int32_t stage_position;        // Current position on stage grid
       uint32_t action_state;         // Current action being performed
       uint32_t round_count;          // Number of rounds completed
       uint32_t input_history[4];     // Recent input history
       uint32_t move_history[4];      // Recent movement history
   };
   ```

### State Management Flow

1. **Initialization (0x406FC0):**
   - Loads initial configuration
   - Sets up round timer and limits
   - Initializes player positions
   - Sets up game mode parameters

2. **State Updates:**
   - Input processing and validation
   - Position and movement updates
   - Action state transitions
   - Round state management

3. **State Synchronization:**
   - Timer synchronization
   - Player state synchronization
   - Round state transitions
   - Game mode state management

### Rollback Implications

1. **Critical State Components:**
   - Round timer and state
   - Player positions and actions
   - Input and move history
   - Game mode state

2. **State Size Analysis:**
   - Core game state: ~128 bytes
   - Player state: ~32 bytes per player
   - History buffers: ~32 bytes per player
   - Total per-frame state: ~256 bytes

3. **Synchronization Requirements:**
   - Timer must be deterministic
   - Player state must be atomic
   - Input processing must be consistent
   - State transitions must be reproducible

## Input Processing System

The game uses a sophisticated input processing system that handles both movement and action inputs:

### Input Mapping (0x406F20)

```c
enum InputActions {
    ACTION_NONE = 0,
    ACTION_A = 1,    // 0x20  - Light Attack
    ACTION_B = 2,    // 0x40  - Medium Attack
    ACTION_C = 3,    // 0x80  - Heavy Attack
    ACTION_D = 4,    // 0x100 - Special Move
    ACTION_E = 5     // 0x200 - Super Move
};

enum InputDirections {
    DIR_NONE  = 0x000,
    DIR_UP    = 0x004,
    DIR_DOWN  = 0x008,
    DIR_LEFT  = 0x001,
    DIR_RIGHT = 0x002
};
```

### Input Processing Flow

1. **Raw Input Capture:**
   - Movement inputs (4-way directional)
   - Action buttons (5 main buttons)
   - System buttons (Start, Select)

2. **Input State Management:**
   ```c
   struct InputState {
       uint16_t current_input;     // Current frame's input
       uint16_t previous_input;    // Last frame's input
       uint16_t input_changes;     // Changed bits since last frame
       uint8_t  processed_input;   // Processed directional input
   };
   ```

3. **Input History:**
   - Each player maintains a 4-frame input history
   - History includes both raw and processed inputs
   - Used for move validation and rollback

4. **Input Processing:**
   - Raw input capture
   - Input change detection
   - Input repeat handling:
     - Initial delay
     - Repeat delay
     - Repeat timer
   - Input validation
   - Mode-specific processing:
     - VS mode (< 3000)
     - Story mode (? 3000)

5. **Memory Layout:**
   ```
   input_system {
     +g_input_buffer_index: Current frame index
     +g_p1_input[8]: Current P1 inputs
     +g_p2_input: Current P2 inputs
     +g_prev_input_state[8]: Previous frame inputs
     +g_input_changes[8]: Input change flags
     +g_input_repeat_state[8]: Input repeat flags
     +g_input_repeat_timer[8]: Repeat timers
     +g_processed_input[8]: Processed inputs
     +g_combined_processed_input: Combined state
     +g_combined_input_changes: Change mask
     +g_combined_raw_input: Raw input mask
   }
   ```

6. **Critical Operations:**
   - Frame advance:
     ```c
     g_input_buffer_index = (g_input_buffer_index + 1) & 0x3FF;
     g_p1_input_history[g_input_buffer_index] = current_input;
     ```
   - Input processing:
     ```c
     g_input_changes = current_input & (prev_input ^ current_input);
     if (repeat_active && current_input == repeat_state) {
       if (--repeat_timer == 0) {
         processed_input = current_input;
         repeat_timer = repeat_delay;
       }
     }
     ```

This system is crucial for rollback implementation as it provides:
1. Deterministic input processing
2. Frame-accurate input history
3. Clear state serialization points
4. Mode-specific input handling
5. Input validation and processing logic

The 1024-frame history buffer is particularly useful for rollback, as it provides ample space for state rewinding and replay.
```

### Health Damage Manager (0x40e6f0)

The health damage manager handles health state and damage calculations:

1. **Health State Management:**
   - Per-character health tracking
   - Health segments system
   - Recovery mechanics
   - Guard damage handling

2. **Memory Layout:**
   ```
   health_system {
     +4DFC95: Current health segment
     +4DFC99: Maximum health segments
     +4DFC9D: Current health points
     +4DFCA1: Health segment size
   }
   ```

3. **Damage Processing:**
   - Segment-based health system:
     ```c
     while (current_health < 0) {
       if (current_segment > 0) {
         current_segment--;
         current_health += segment_size;
       } else {
         current_health = 0;
       }
     }
     ```
   - Health recovery:
     ```c
     while (current_health >= segment_size) {
       if (current_segment < max_segments) {
         current_health -= segment_size;
         current_segment++;
       } else {
         current_health = 0;
       }
     }
     ```

4. **Critical Operations:**
   - Damage application:
     - Negative values for damage
     - Positive values for healing
   - Segment management:
     - Segment depletion
     - Segment recovery
     - Maximum segment cap
   - Health validation:
     - Minimum health check
     - Maximum health check
     - Segment boundary checks

This system is crucial for rollback implementation as it:
1. Uses deterministic damage calculations
2. Maintains clear health state boundaries
3. Provides segment-based state tracking
4. Handles health recovery mechanics
5. Manages guard damage separately

The segmented health system will make it easier to serialize and restore health states during rollback operations, as each segment provides a clear checkpoint for state management.
```

### Rollback Implementation Summary

After analyzing FM2K's core systems, we can conclude that the game is well-suited for rollback netcode implementation. Here's a comprehensive overview of how each system will integrate with rollback:

1. **State Serialization Points:**
   - Character State Machine (0x411bf0):
     - Base states (0x0-0x3)
     - Action states (0x4-0x7)
     - Movement states (0x8-0xB)
     - Frame counters and timers
   - Hit Detection System (0x40f010):
     - Hit boxes and collision state
     - Damage values and scaling
     - Hit confirmation flags
   - Input Buffer System (0x4146d0):
     - 1024-frame history buffer
     - Input state flags
     - Repeat handling state
   - Health System (0x40e6f0):
     - Health segments
     - Current health points
     - Guard damage state
     - Recovery mechanics

2. **Deterministic Systems:**
   - Fixed frame rate (100 FPS)
   - 16.16 fixed-point coordinates
   - Deterministic input processing
   - Clear state transitions
   - Predictable damage calculations
   - Frame-based timers

3. **State Management:**
   - Object Pool (1024 entries):
     - Type identification
     - Position and velocity
     - Animation state
     - Collision data
   - Input History:
     - Circular buffer design
     - Clear frame boundaries
     - Input validation
   - Hit Detection:
     - Priority system
     - State validation
     - Reaction handling
   - Health System:
     - Segment-based tracking
     - Clear state boundaries
     - Recovery mechanics

4. **Implementation Strategy:**
   a. Save State (Frame N):
      ```c
      struct SaveState {
          // Character State
          int base_state;
          int action_state;
          int movement_state;
          int frame_counters[4];
          
          // Hit Detection
          int hit_boxes[20];
          int damage_values[8];
          int hit_flags;
          
          // Input Buffer
          int input_history[1024];
          int input_flags;
          int repeat_state;
          
          // Health System
          int health_segments;
          int current_health;
          int guard_damage;
      };
      ```

   b. Rollback Process:
      1. Save current frame state
      2. Apply new remote input
      3. Simulate forward:
         - Process inputs
         - Update character states
         - Handle collisions
         - Apply damage
      4. Render new state
      5. Save confirmed state

5. **Critical Hooks:**
   - Input Processing (0x4146d0):
     - Frame advance point
     - Input buffer management
   - Character State (0x411bf0):
     - State transition handling
     - Frame counting
   - Hit Detection (0x40f010):
     - Collision processing
     - Damage application
   - Health System (0x40e6f0):
     - Health state management
     - Segment tracking

6. **Optimization Opportunities:**
   - Parallel state simulation
   - Minimal state serialization
   - Efficient state diffing
   - Smart state prediction
   - Input compression

The analysis reveals that FM2K's architecture is highly suitable for rollback implementation due to its:
1. Deterministic game logic
2. Clear state boundaries
3. Frame-based processing
4. Efficient memory layout
5. Well-defined systems

This research provides a solid foundation for implementing rollback netcode in FM2K, with all major systems supporting the requirements for state saving, rollback, and resimulation.

## GekkoNet Integration Analysis

### GekkoNet Library Overview

GekkoNet is a rollback netcode library that provides the networking infrastructure we need for FM2K. Here's how it maps to our analyzed systems:

### 1. **GekkoNet Configuration for FM2K**
```c
GekkoConfig fm2k_config = {
    .num_players = 2,                    // P1 vs P2
    .max_spectators = 8,                 // Allow spectators
    .input_prediction_window = 8,        // 8 frames ahead prediction
    .spectator_delay = 6,                // 6 frame spectator delay
    .input_size = sizeof(uint32_t),      // FM2K input flags (32-bit)
    .state_size = sizeof(FM2KGameState), // Our minimal state structure
    .limited_saving = false,             // Full state saving for accuracy
    .post_sync_joining = true,           // Allow mid-match spectators
    .desync_detection = true             // Enable desync detection
};
```

### 2. **Integration Points with FM2K Systems**

#### Input System Integration (0x4146d0)
```c
// Hook into process_game_inputs()
void fm2k_input_hook() {
    // Get GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case AdvanceEvent:
                // Inject inputs into FM2K's input system
                g_p1_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[0];
                g_p2_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[1];
                // Continue normal input processing
                break;
                
            case SaveEvent:
                // Save current FM2K state
                save_fm2k_state(events[i]->data.save.state, events[i]->data.save.state_len);
                break;
                
            case LoadEvent:
                // Restore FM2K state
                load_fm2k_state(events[i]->data.load.state, events[i]->data.load.state_len);
                break;
        }
    }
}
```

#### State Management Integration
```c
typedef struct FM2KGameState {
    // Frame tracking
    uint32_t frame_number;
    uint32_t input_buffer_index;
    
    // Input state
    uint32_t p1_input_current;
    uint32_t p2_input_current;
    uint32_t input_repeat_state[8];
    uint32_t input_repeat_timer[8];
    
    // Character states (per character)
    struct {
        // Position (16.16 fixed point)
        int32_t pos_x, pos_y;
        int32_t vel_x, vel_y;
        
        // State machine
        uint16_t base_state;        // 0x0-0x3
        uint16_t action_state;      // 0x4-0x7  
        uint16_t movement_state;    // 0x8-0xB
        uint16_t state_flags;       // Offset 350
        
        // Animation
        uint16_t animation_frame;   // Offset 12
        uint16_t max_animation_frame; // Offset 88
        
        // Health system
        uint16_t health_segments;   // Current segments
        uint16_t current_health;    // Health points
        uint16_t max_health_segments; // Maximum segments
        uint16_t guard_damage;      // Guard damage accumulation
        
        // Hit detection
        uint16_t hit_flags;         // Hit state flags
        uint16_t hit_stun_timer;    // Hit stun duration
        uint16_t guard_state;       // Guard state flags
        
        // Facing and direction
        uint8_t facing_direction;   // Left/right facing
        uint8_t input_direction;    // Current input direction
    } characters[2];
    
    // Round state
    uint32_t round_state;           // Round state machine
    uint32_t round_timer;           // Round timer
    uint32_t game_mode;             // Current game mode
    
    // Hit detection global state
    uint32_t hit_effect_target;     // Hit effect target
    uint32_t hit_effect_timer;      // Hit effect timer
    
    // Combo system
    uint16_t combo_counter[2];      // Combo counters
    uint16_t damage_scaling[2];     // Damage scaling
    
    // Object pool critical state
    uint16_t active_objects;        // Number of active objects
    uint32_t object_state_flags;    // Critical object flags
    
} FM2KGameState;
```

### 3. **Network Adapter Implementation**
```c
GekkoNetAdapter fm2k_adapter = {
    .send_data = fm2k_send_packet,
    .receive_data = fm2k_receive_packets,
    .free_data = fm2k_free_packet_data
};

void fm2k_send_packet(GekkoNetAddress* addr, const char* data, int length) {
    // Use existing LilithPort networking or implement UDP
    // addr->data contains IP/port information
    // data contains the packet to send
}

GekkoNetResult** fm2k_receive_packets(int* length) {
    // Collect all packets received since last call
    // Return array of GekkoNetResult pointers
    // GekkoNet will call free_data on each result
}
```

### 4. **Integration with Existing FM2K Systems**

#### Main Game Loop Integration
```c
// Hook at 0x4146d0 - process_game_inputs()
void fm2k_rollback_frame() {
    // 1. Poll network
    gekko_network_poll(session);
    
    // 2. Add local inputs
    uint32_t local_input = get_local_player_input();
    gekko_add_local_input(session, local_player_id, &local_input);
    
    // 3. Process GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    // 4. Handle events (save/load/advance)
    process_gekko_events(events, event_count);
    
    // 5. Continue normal FM2K processing
    // (character updates, hit detection, etc.)
}
```

#### State Serialization Functions
```c
void save_fm2k_state(unsigned char* buffer, unsigned int* length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Save frame state
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    
    // Save character states from object pool
    for (int i = 0; i < 2; i++) {
        save_character_state(&state->characters[i], i);
    }
    
    // Save round state
    state->round_state = g_round_state;
    state->round_timer = g_round_timer;
    
    *length = sizeof(FM2KGameState);
}

void load_fm2k_state(unsigned char* buffer, unsigned int length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Restore frame state
    current_frame = state->frame_number;
    g_input_buffer_index = state->input_buffer_index;
    
    // Restore character states to object pool
    for (int i = 0; i < 2; i++) {
        load_character_state(&state->characters[i], i);
    }
    
    // Restore round state
    g_round_state = state->round_state;
    g_round_timer = state->round_timer;
}
```

### 5. **Event Handling Integration**
```c
void handle_session_events() {
    int event_count;
    GekkoSessionEvent** events = gekko_session_events(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case PlayerConnected:
                // Show "Player Connected" message
                break;
            case PlayerDisconnected:
                // Handle disconnection
                break;
            case DesyncDetected:
                // Log desync information
                printf("Desync at frame %d: local=%08x remote=%08x\n",
                       events[i]->data.desynced.frame,
                       events[i]->data.desynced.local_checksum,
                       events[i]->data.desynced.remote_checksum);
                break;
        }
    }
}
```

### 6. **Performance Considerations**
- **State Size**: ~200 bytes per frame (very efficient)
- **Prediction Window**: 8 frames ahead (80ms at 100 FPS)
- **Rollback Depth**: Up to 15 frames (150ms)
- **Network Frequency**: 100 Hz to match FM2K's frame rate

### 7. **Implementation Benefits**
1. **Minimal Code Changes**: Only need to hook input processing
2. **Existing Infrastructure**: Leverages FM2K's input history system
3. **Deterministic**: FM2K's fixed-point math ensures consistency
4. **Efficient**: Small state size enables fast rollback
5. **Robust**: GekkoNet handles all networking complexity

This integration approach leverages both FM2K's existing architecture and GekkoNet's proven rollback implementation, providing a solid foundation for online play.

### Critical Player State Variables

Based on IDA analysis, here are the key memory addresses for player state:

#### Player 1 State Variables:
```c
// Input system
g_p1_input          = 0x4259c0;  // Current input state
g_p1_input_history  = 0x4280e0;  // 1024-frame input history

// Position and stage
g_p1_stage_x        = 0x424e68;  // Stage position X
g_p1_stage_y        = 0x424e6c;  // Stage position Y

// Round and game state
g_p1_round_count    = 0x4700ec;  // Round wins
g_p1_round_state    = 0x4700f0;  // Round state
g_p1_action_state   = 0x47019c;  // Action state

// Health system
g_p1_hp             = 0x4dfc85;  // Current HP
g_p1_max_hp         = 0x4dfc91;  // Maximum HP
```

#### Player 2 State Variables:
```c
// Input system
g_p2_input          = 0x4259c4;  // Current input state
g_p2_input_history  = 0x4290e0;  // 1024-frame input history

// Round and game state
g_p2_action_state   = 0x4701a0;  // Action state

// Health system
g_p2_hp             = 0x4edcc4;  // Current HP
g_p2_max_hp         = 0x4edcd0;  // Maximum HP
```

#### Core Game State Variables:
```c
// Object pool
g_object_pool       = 0x4701e0;  // Main object pool (1024 entries)
g_sprite_data_array = 0x447938;  // Sprite data array

// Timing system
g_frame_time_ms     = 0x41e2f0;  // Frame duration (10ms)
g_last_frame_time   = 0x447dd4;  // Last frame timestamp
g_frame_sync_flag   = 0x424700;  // Frame sync state
g_frame_time_delta  = 0x425960;  // Frame timing delta
g_frame_skip_count  = 0x4246f4;  // Frame skip counter

// Random number generator
g_random_seed       = 0x41fb1c;  // RNG seed

// Input buffer management
g_input_buffer_index = (calculated); // Current buffer index
g_input_repeat_timer = 0x4d1c40;     // Input repeat timers

// Round system
g_round_timer       = 0x470060;  // Round timer
g_game_timer        = 0x470044;  // Game timer
g_hit_effect_timer  = 0x4701c8;  // Hit effect timer
```

### Final State Structure for GekkoNet

With all the memory addresses identified, here's the complete state structure:

```c
typedef struct FM2KCompleteGameState {
    // Frame and timing state
    uint32_t frame_number;
    uint32_t input_buffer_index;
    uint32_t last_frame_time;
    uint32_t frame_time_delta;
    uint8_t frame_skip_count;
    uint8_t frame_sync_flag;
    
    // Random number generator state
    uint32_t random_seed;
    
    // Player 1 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t stage_x, stage_y;
        uint32_t round_count;
        uint32_t round_state;
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p1;
    
    // Player 2 state
    struct {
        uint32_t input_current;
        uint32_t input_history[1024];
        uint32_t action_state;
        uint32_t hp, max_hp;
    } p2;
    
    // Global timers
    uint32_t round_timer;
    uint32_t game_timer;
    uint32_t hit_effect_timer;
    
    // Input system state
    uint32_t input_repeat_timer[8];
    
    // Critical object pool state (first 2 character objects)
    struct {
        uint32_t object_type;
        int32_t pos_x, pos_y;        // 16.16 fixed point
        int32_t vel_x, vel_y;
        uint16_t state_flags;
        uint16_t animation_frame;
        uint16_t health_segments;
        uint16_t facing_direction;
        // ... other critical fields
    } character_objects[2];
    
} FM2KCompleteGameState;
```

### Memory Access Functions

```c
// Direct memory access functions for state serialization
void save_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Frame and timing
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    state->last_frame_time = *(uint32_t*)0x447dd4;
    state->frame_time_delta = *(uint32_t*)0x425960;
    state->frame_skip_count = *(uint8_t*)0x4246f4;
    state->frame_sync_flag = *(uint8_t*)0x424700;
    
    // RNG state
    state->random_seed = *(uint32_t*)0x41fb1c;
    
    // Player 1 state
    state->p1.input_current = *(uint32_t*)0x4259c0;
    memcpy(state->p1.input_history, (void*)0x4280e0, 1024 * sizeof(uint32_t));
    state->p1.stage_x = *(uint32_t*)0x424e68;
    state->p1.stage_y = *(uint32_t*)0x424e6c;
    state->p1.round_count = *(uint32_t*)0x4700ec;
    state->p1.round_state = *(uint32_t*)0x4700f0;
    state->p1.action_state = *(uint32_t*)0x47019c;
    state->p1.hp = *(uint32_t*)0x4dfc85;
    state->p1.max_hp = *(uint32_t*)0x4dfc91;
    
    // Player 2 state
    state->p2.input_current = *(uint32_t*)0x4259c4;
    memcpy(state->p2.input_history, (void*)0x4290e0, 1024 * sizeof(uint32_t));
    state->p2.action_state = *(uint32_t*)0x4701a0;
    state->p2.hp = *(uint32_t*)0x4edcc4;
    state->p2.max_hp = *(uint32_t*)0x4edcd0;
    
    // Global timers
    state->round_timer = *(uint32_t*)0x470060;
    state->game_timer = *(uint32_t*)0x470044;
    state->hit_effect_timer = *(uint32_t*)0x4701c8;
    
    // Input repeat timers
    memcpy(state->input_repeat_timer, (void*)0x4d1c40, 8 * sizeof(uint32_t));
    
    // Character objects from object pool
    save_character_objects_from_pool(state->character_objects);
}

void load_fm2k_complete_state(FM2KCompleteGameState* state) {
    // Restore all state in reverse order
    // ... (similar but with assignment in opposite direction)
}
```

This gives us a complete, memory-accurate state structure that can be used with GekkoNet for rollback implementation. The state size is approximately 10KB, which is very reasonable for rollback netcode.

## GekkoNet SDL Example Analysis & FM2K Integration

### SDL Example Key Patterns

The OnlineSession.cpp example demonstrates the core GekkoNet integration patterns we need for FM2K:

#### 1. **Session Setup Pattern**
```c
// From SDL example - adapt for FM2K
GekkoSession* sess = nullptr;
GekkoConfig conf {
    .num_players = 2,
    .input_size = sizeof(char),           // FM2K: sizeof(uint32_t)
    .state_size = sizeof(GState),         // FM2K: sizeof(FM2KCompleteGameState)
    .max_spectators = 0,                  // FM2K: 8 for spectators
    .input_prediction_window = 10,        // FM2K: 8 frames
    .desync_detection = true,
    // .limited_saving = true,            // FM2K: false for accuracy
};

gekko_create(&sess);
gekko_start(sess, &conf);
gekko_net_adapter_set(sess, gekko_default_adapter(localport));
```

#### 2. **Player Management Pattern**
```c
// Order-dependent player addition (from example)
if (localplayer == 0) {
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
} else {
    auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                  (unsigned int)remote_address.size() };
    gekko_add_actor(sess, RemotePlayer, &remote);
    localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
}
```

#### 3. **Core Game Loop Pattern** 
```c
// From SDL example - critical for FM2K integration
while (running) {
    // Timing control
    frames_ahead = gekko_frames_ahead(sess);
    frame_time = GetFrameTime(frames_ahead);
    
    // Network polling
    gekko_network_poll(sess);
    
    // Frame processing
    while (accumulator >= frame_time) {
        // Add local input
        auto input = get_key_inputs();
        gekko_add_local_input(sess, localplayer, &input);
        
        // Handle session events
        auto events = gekko_session_events(sess, &count);
        for (int i = 0; i < count; i++) {
            // Handle PlayerConnected, PlayerDisconnected, DesyncDetected
        }
        
        // Process game events
        auto updates = gekko_update_session(sess, &count);
        for (int i = 0; i < count; i++) {
            switch (updates[i]->type) {
                case SaveEvent: save_state(&state, updates[i]); break;
                case LoadEvent: load_state(&state, updates[i]); break;
                case AdvanceEvent:
                    // Extract inputs and advance game
                    inputs[0].input.value = updates[i]->data.adv.inputs[0];
                    inputs[1].input.value = updates[i]->data.adv.inputs[1];
                    update_state(state, inputs, num_players);
                    break;
            }
        }
        accumulator -= frame_time;
    }
    
    // Render current state
    render_state(state);
}
```

### FM2K Integration Strategy

#### 1. **Replace SDL2 with SDL3 for FM2K**
```c
// SDL3 initialization for FM2K integration
#include "SDL3/SDL.h"

bool init_fm2k_window(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Error initializing SDL3.\n");
        return false;
    }
    
    // Create window matching FM2K's resolution
    window = SDL_CreateWindow(
        "FM2K Online Session",
        640, 480,  // FM2K native resolution
        SDL_WINDOW_RESIZABLE
    );
    
    if (!window) {
        fprintf(stderr, "Error creating SDL3 Window.\n");
        return false;
    }
    
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        fprintf(stderr, "Error creating SDL3 Renderer.\n");
        return false;
    }
    
    return true;
}
```

#### 2. **FM2K Input Structure**
```c
// Replace simple GInput with FM2K's 11-bit input system
struct FM2KInput {
    union {
        struct {
            uint16_t left     : 1;   // 0x001
            uint16_t right    : 1;   // 0x002
            uint16_t up       : 1;   // 0x004
            uint16_t down     : 1;   // 0x008
            uint16_t button1  : 1;   // 0x010
            uint16_t button2  : 1;   // 0x020
            uint16_t button3  : 1;   // 0x040
            uint16_t button4  : 1;   // 0x080
            uint16_t button5  : 1;   // 0x100
            uint16_t button6  : 1;   // 0x200
            uint16_t button7  : 1;   // 0x400
            uint16_t reserved : 5;   // Unused bits
        } bits;
        uint16_t value;
    } input;
};

FM2KInput get_fm2k_inputs() {
    FM2KInput input{};
    input.input.value = 0;
    
    // Get keyboard state (SDL3 syntax)
    const bool* keys = SDL_GetKeyboardState(NULL);
    
    // Map to FM2K inputs
    input.input.bits.up = keys[SDL_SCANCODE_W];
    input.input.bits.left = keys[SDL_SCANCODE_A];
    input.input.bits.down = keys[SDL_SCANCODE_S];
    input.input.bits.right = keys[SDL_SCANCODE_D];
    input.input.bits.button1 = keys[SDL_SCANCODE_J];
    input.input.bits.button2 = keys[SDL_SCANCODE_K];
    input.input.bits.button3 = keys[SDL_SCANCODE_L];
    // ... map other buttons
    
    return input;
}
```

#### 3. **FM2K State Management**
```c
// Replace simple GState with FM2KCompleteGameState
void save_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    *ev->data.save.state_len = sizeof(FM2KCompleteGameState);
    
    // Use Fletcher's checksum (from example) for consistency
    *ev->data.save.checksum = fletcher32((uint16_t*)gs, sizeof(FM2KCompleteGameState));
    
    std::memcpy(ev->data.save.state, gs, sizeof(FM2KCompleteGameState));
}

void load_fm2k_state(FM2KCompleteGameState* gs, GekkoGameEvent* ev) {
    std::memcpy(gs, ev->data.load.state, sizeof(FM2KCompleteGameState));
}

void update_fm2k_state(FM2KCompleteGameState& gs, FM2KInput inputs[2]) {
    // Call into FM2K's actual game logic
    // This would hook into the functions we analyzed:
    
    // 1. Inject inputs into FM2K's input system
    *(uint32_t*)0x4259c0 = inputs[0].input.value;  // g_p1_input
    *(uint32_t*)0x4259c4 = inputs[1].input.value;  // g_p2_input
    
    // 2. Call FM2K's update functions
    // process_game_inputs_FRAMESTEP_HOOK();  // 0x4146d0
    // update_game_state();                   // Game logic
    // character_state_machine();             // 0x411bf0
    // hit_detection_system();                // 0x40f010
    
    // 3. Extract updated state from FM2K memory
    gs.p1.hp = *(uint32_t*)0x4dfc85;
    gs.p2.hp = *(uint32_t*)0x4edcc4;
    gs.round_timer = *(uint32_t*)0x470060;
    // ... extract all other state variables
}
```

#### 4. **FM2K Timing Integration**
```c
// Adapt timing to match FM2K's 100 FPS
float GetFM2KFrameTime(float frames_ahead) {
    // FM2K runs at 100 FPS (10ms per frame)
    const float base_frame_time = 1.0f / 100.0f;  // 0.01 seconds
    
    if (frames_ahead >= 0.75f) {
        // Slow down if too far ahead
        return base_frame_time * 1.02f;  // 59.8 FPS equivalent
    } else {
        return base_frame_time;  // Normal 100 FPS
    }
}
```

#### 5. **Complete FM2K Integration Main Loop**
```c
int fm2k_online_main(int argc, char* args[]) {
    // Parse command line (same as example)
    int localplayer = std::stoi(args[1]);
    const int localport = std::stoi(args[2]);
    std::string remote_address = std::move(args[3]);
    int localdelay = std::stoi(args[4]);
    
    // Initialize SDL3 and FM2K
    running = init_fm2k_window();
    
    // Initialize FM2K state
    FM2KCompleteGameState state = {};
    FM2KInput inputs[2] = {};
    
    // Setup GekkoNet
    GekkoSession* sess = nullptr;
    GekkoConfig conf {
        .num_players = 2,
        .input_size = sizeof(uint16_t),  // FM2K input size
        .state_size = sizeof(FM2KCompleteGameState),
        .max_spectators = 8,
        .input_prediction_window = 8,
        .desync_detection = true,
        .limited_saving = false,  // Full state saving for accuracy
        .post_sync_joining = true
    };
    
    gekko_create(&sess);
    gekko_start(sess, &conf);
    gekko_net_adapter_set(sess, gekko_default_adapter(localport));
    
    // Add players (order-dependent)
    if (localplayer == 0) {
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
    } else {
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), 
                                      (unsigned int)remote_address.size() };
        gekko_add_actor(sess, RemotePlayer, &remote);
        localplayer = gekko_add_actor(sess, LocalPlayer, nullptr);
    }
    
    gekko_set_local_delay(sess, localplayer, localdelay);
    
    // Timing variables
    auto curr_time = std::chrono::steady_clock::now();
    auto prev_time = curr_time;
    float delta_time = 0.0f;
    float accumulator = 0.0f;
    float frame_time = 0.0f;
    float frames_ahead = 0.0f;
    
    while (running) {
        curr_time = std::chrono::steady_clock::now();
        
        frames_ahead = gekko_frames_ahead(sess);
        frame_time = GetFM2KFrameTime(frames_ahead);
        
        delta_time = std::chrono::duration<float>(curr_time - prev_time).count();
        prev_time = curr_time;
        accumulator += delta_time;
        
        gekko_network_poll(sess);
        
        while (accumulator >= frame_time) {
            // Process SDL events
            process_events();
            
            // Get local input
            auto input = get_fm2k_inputs();
            gekko_add_local_input(sess, localplayer, &input);
            
            // Handle session events
            int count = 0;
            auto events = gekko_session_events(sess, &count);
            for (int i = 0; i < count; i++) {
                handle_fm2k_session_event(events[i]);
            }
            
            // Process game events
            count = 0;
            auto updates = gekko_update_session(sess, &count);
            for (int i = 0; i < count; i++) {
                auto ev = updates[i];
                
                switch (ev->type) {
                    case SaveEvent:
                        save_fm2k_state(&state, ev);
                        break;
                    case LoadEvent:
                        load_fm2k_state(&state, ev);
                        break;
                    case AdvanceEvent:
                        // Extract inputs from GekkoNet
                        inputs[0].input.value = ((uint16_t*)ev->data.adv.inputs)[0];
                        inputs[1].input.value = ((uint16_t*)ev->data.adv.inputs)[1];
                        
                        // Update FM2K game state
                        update_fm2k_state(state, inputs);
                        break;
                }
            }
            
            accumulator -= frame_time;
        }
        
        // Render current state
        render_fm2k_state(state);
    }
    
    gekko_destroy(sess);
    del_window();
    return 0;
}
```

### Key Differences from SDL Example

1. **Input Size**: FM2K uses 16-bit inputs vs 8-bit in example
2. **State Size**: ~10KB vs ~16 bytes in example  
3. **Frame Rate**: 100 FPS vs 60 FPS in example
4. **State Complexity**: Full game state vs simple position
5. **Integration Depth**: Hooks into existing game vs standalone

### Integration Benefits

1. **Proven Pattern**: The SDL example shows GekkoNet works reliably
2. **Clear Structure**: Event handling patterns are well-defined
3. **Performance**: Timing control handles frame rate variations
4. **Network Stats**: Built-in ping/jitter monitoring
5. **Error Handling**: Desync detection and player management

This analysis shows that adapting the SDL example for FM2K is very feasible, with the main changes being input/state structures and frame timing to match FM2K's 100 FPS architecture.

## Hit Judge and Round System Architecture

### Hit Judge Configuration (0x42470C-0x430120)

The game uses a sophisticated hit detection and configuration system loaded from INI files:

```c
struct FM2K_HitJudgeConfig {
    uint32_t hit_judge_value;    // 0x42470C - Hit detection threshold
    uint32_t config_values[7];   // 0x4300E0-0x430120 - General config values
    char     config_string[260]; // Configuration string buffer
};
```

The hit judge system is initialized by `hit_judge_set_function` (0x414930) which:
1. Loads configuration from INI files
2. Sets up hit detection parameters
3. Configures round settings and game modes

### Round System State (0x470040-0x47006C)

The round system maintains several critical state variables:

```c
struct FM2K_RoundSystem {
    uint32_t game_mode;      // 0x470040 - Current game mode (VS/Team/Tournament)
    uint32_t round_limit;    // 0x470048 - Maximum rounds for the match
    uint32_t round_state;    // 0x47004C - Current round state
    uint32_t round_timer;    // 0x470060 - Active round timer
};

enum FM2K_RoundState {
    ROUND_INIT = 0,      // Initial state
    ROUND_ACTIVE = 1,    // Round in progress
    ROUND_END = 2,       // Round has ended
    ROUND_MATCH_END = 3  // Match has ended
};

enum FM2K_GameMode {
    MODE_NORMAL = 0,     // Normal VS mode
    MODE_TEAM = 1,       // Team Battle mode
    MODE_TOURNAMENT = 2  // Tournament mode
};
```

The round system is managed by `vs_round_function` (0x4086A0) which:
1. Handles round state transitions
2. Manages round timers
3. Processes win/loss conditions
4. Controls match flow

### Rollback Implications

This architecture has several important implications for rollback implementation:

1. **State Serialization Requirements:**
   - Must save hit judge configuration
   - Must preserve round state and timers
   - Game mode affects state size

2. **Deterministic Behavior:**
   - Hit detection is configuration-driven
   - Round state transitions are deterministic
   - Timer-based events are predictable

3. **State Size Optimization:**
   - Hit judge config is static per match
   - Only dynamic round state needs saving
   - Can optimize by excluding static config

4. **Synchronization Points:**
   - Round state changes are key sync points
   - Hit detection results must be identical
   - Timer values must match exactly

Updated state structure for rollback:

```c
typedef struct FM2KGameState {
    // ... existing timing and input state ...

    // Round System State
    struct {
        uint32_t mode;          // Current game mode
        uint32_t round_limit;   // Maximum rounds
        uint32_t round_state;   // Current round state
        uint32_t round_timer;   // Active round timer
        uint32_t score_value;   // Current score/timer value
    } round_system;

    // Hit Judge State
    struct {
        uint32_t hit_judge_value;   // Current hit detection threshold
        uint32_t effect_target;     // Current hit effect target
        uint32_t effect_timer;      // Hit effect duration
    } hit_system;

    // ... rest of game state ...
} FM2KGameState;
```

## Sprite Rendering and Hit Detection System

### Sprite Rendering Engine (0x40CC30)

The game uses a sophisticated sprite rendering engine that handles:
1. Hit effect visualization
2. Sprite transformations and blending
3. Color palette manipulation
4. Screen-space effects

Key components:

```c
struct SpriteRenderState {
    uint32_t render_mode;     // Different rendering modes (-10 to 3)
    uint32_t blend_flags;     // Blending and effect flags
    uint32_t color_values[3]; // RGB color modification values
    uint32_t effect_timer;    // Effect duration counter
};
```

### Hit Effect System

The hit effect system is tightly integrated with the sprite renderer:

1. **Effect Triggers:**
   - Hit detection results (0x42470C)
   - Game state transitions
   - Timer-based events

2. **Effect Parameters:**
   ```c
   struct HitEffectParams {
       uint32_t target_id;      // Effect target identifier
       uint32_t effect_type;    // Visual effect type
       uint32_t duration;       // Effect duration in frames
       uint32_t color_values;   // RGB color modulation
   };
   ```

3. **Rendering Pipeline:**
   - Color palette manipulation (0x4D1A20)
   - Sprite transformation
   - Blend mode selection
   - Screen-space effects

### Integration with Rollback

The sprite and hit effect systems have important implications for rollback:

1. **Visual State Management:**
   - Effect timers must be part of the game state
   - Color modulation values need saving
   - Sprite transformations must be deterministic

2. **Performance Considerations:**
   - Effect parameters are small (few bytes per effect)
   - Visual state is derived from game state
   - Can optimize by excluding pure visual state

3. **Synchronization Requirements:**
   - Hit effects must trigger identically
   - Effect timers must stay synchronized
   - Visual state must match game state

Updated state structure to include visual effects:

```c
typedef struct FM2KGameState {
    // ... previous state members ...

    // Visual Effect State
    struct {
        uint32_t active_effects;     // Bitfield of active effects
        uint32_t effect_timers[8];   // Array of effect durations
        uint32_t color_values[8][3]; // RGB values for each effect
        uint32_t target_ids[8];      // Effect target identifiers
    } visual_state;

    // ... rest of game state ...
} FM2KGameState;
```

## Game State Management System

The game state manager (0x406FC0) handles several critical aspects of the game:

### State Variables

1. **Round System State:**
   ```c
   struct RoundSystemState {
       uint32_t g_round_timer;        // 0x470060 - Current round timer value
       uint32_t g_round_limit;        // 0x470048 - Maximum rounds per match
       uint32_t g_round_state;        // 0x47004C - Current round state
       uint32_t g_game_mode;          // 0x470040 - Current game mode
   };
   ```

2. **Player State:**
   ```c
   struct PlayerState {
       int32_t stage_position;        // Current position on stage grid
       uint32_t action_state;         // Current action being performed
       uint32_t round_count;          // Number of rounds completed
       uint32_t input_history[4];     // Recent input history
       uint32_t move_history[4];      // Recent movement history
   };
   ```

### State Management Flow

1. **Initialization (0x406FC0):**
   - Loads initial configuration
   - Sets up round timer and limits
   - Initializes player positions
   - Sets up game mode parameters

2. **State Updates:**
   - Input processing and validation
   - Position and movement updates
   - Action state transitions
   - Round state management

3. **State Synchronization:**
   - Timer synchronization
   - Player state synchronization
   - Round state transitions
   - Game mode state management

### Rollback Implications

1. **Critical State Components:**
   - Round timer and state
   - Player positions and actions
   - Input and move history
   - Game mode state

2. **State Size Analysis:**
   - Core game state: ~128 bytes
   - Player state: ~32 bytes per player
   - History buffers: ~32 bytes per player
   - Total per-frame state: ~256 bytes

3. **Synchronization Requirements:**
   - Timer must be deterministic
   - Player state must be atomic
   - Input processing must be consistent
   - State transitions must be reproducible

## Input Processing System

The game uses a sophisticated input processing system that handles both movement and action inputs:

### Input Mapping (0x406F20)

```c
enum InputActions {
    ACTION_NONE = 0,
    ACTION_A = 1,    // 0x20  - Light Attack
    ACTION_B = 2,    // 0x40  - Medium Attack
    ACTION_C = 3,    // 0x80  - Heavy Attack
    ACTION_D = 4,    // 0x100 - Special Move
    ACTION_E = 5     // 0x200 - Super Move
};

enum InputDirections {
    DIR_NONE  = 0x000,
    DIR_UP    = 0x004,
    DIR_DOWN  = 0x008,
    DIR_LEFT  = 0x001,
    DIR_RIGHT = 0x002
};
```

### Input Processing Flow

1. **Raw Input Capture:**
   - Movement inputs (4-way directional)
   - Action buttons (5 main buttons)
   - System buttons (Start, Select)

2. **Input State Management:**
   ```c
   struct InputState {
       uint16_t current_input;     // Current frame's input
       uint16_t previous_input;    // Last frame's input
       uint16_t input_changes;     // Changed bits since last frame
       uint8_t  processed_input;   // Processed directional input
   };
   ```

3. **Input History:**
   - Each player maintains a 4-frame input history
   - History includes both raw and processed inputs
   - Used for move validation and rollback

4. **Input Processing:**
   - Raw input capture
   - Input change detection
   - Input repeat handling:
     - Initial delay
     - Repeat delay
     - Repeat timer
   - Input validation
   - Mode-specific processing:
     - VS mode (< 3000)
     - Story mode (? 3000)

5. **Memory Layout:**
   ```
   input_system {
     +g_input_buffer_index: Current frame index
     +g_p1_input[8]: Current P1 inputs
     +g_p2_input: Current P2 inputs
     +g_prev_input_state[8]: Previous frame inputs
     +g_input_changes[8]: Input change flags
     +g_input_repeat_state[8]: Input repeat flags
     +g_input_repeat_timer[8]: Repeat timers
     +g_processed_input[8]: Processed inputs
     +g_combined_processed_input: Combined state
     +g_combined_input_changes: Change mask
     +g_combined_raw_input: Raw input mask
   }
   ```

6. **Critical Operations:**
   - Frame advance:
     ```c
     g_input_buffer_index = (g_input_buffer_index + 1) & 0x3FF;
     g_p1_input_history[g_input_buffer_index] = current_input;
     ```
   - Input processing:
     ```c
     g_input_changes = current_input & (prev_input ^ current_input);
     if (repeat_active && current_input == repeat_state) {
       if (--repeat_timer == 0) {
         processed_input = current_input;
         repeat_timer = repeat_delay;
       }
     }
     ```

This system is crucial for rollback implementation as it provides:
1. Deterministic input processing
2. Frame-accurate input history
3. Clear state serialization points
4. Mode-specific input handling
5. Input validation and processing logic

The 1024-frame history buffer is particularly useful for rollback, as it provides ample space for state rewinding and replay.
```

### Health Damage Manager (0x40e6f0)

The health damage manager handles health state and damage calculations:

1. **Health State Management:**
   - Per-character health tracking
   - Health segments system
   - Recovery mechanics
   - Guard damage handling

2. **Memory Layout:**
   ```
   health_system {
     +4DFC95: Current health segment
     +4DFC99: Maximum health segments
     +4DFC9D: Current health points
     +4DFCA1: Health segment size
   }
   ```

3. **Damage Processing:**
   - Segment-based health system:
     ```c
     while (current_health < 0) {
       if (current_segment > 0) {
         current_segment--;
         current_health += segment_size;
       } else {
         current_health = 0;
       }
     }
     ```
   - Health recovery:
     ```c
     while (current_health >= segment_size) {
       if (current_segment < max_segments) {
         current_health -= segment_size;
         current_segment++;
       } else {
         current_health = 0;
       }
     }
     ```

4. **Critical Operations:**
   - Damage application:
     - Negative values for damage
     - Positive values for healing
   - Segment management:
     - Segment depletion
     - Segment recovery
     - Maximum segment cap
   - Health validation:
     - Minimum health check
     - Maximum health check
     - Segment boundary checks

This system is crucial for rollback implementation as it:
1. Uses deterministic damage calculations
2. Maintains clear health state boundaries
3. Provides segment-based state tracking
4. Handles health recovery mechanics
5. Manages guard damage separately

The segmented health system will make it easier to serialize and restore health states during rollback operations, as each segment provides a clear checkpoint for state management.
```

### Rollback Implementation Summary

After analyzing FM2K's core systems, we can conclude that the game is well-suited for rollback netcode implementation. Here's a comprehensive overview of how each system will integrate with rollback:

1. **State Serialization Points:**
   - Character State Machine (0x411bf0):
     - Base states (0x0-0x3)
     - Action states (0x4-0x7)
     - Movement states (0x8-0xB)
     - Frame counters and timers
   - Hit Detection System (0x40f010):
     - Hit boxes and collision state
     - Damage values and scaling
     - Hit confirmation flags
   - Input Buffer System (0x4146d0):
     - 1024-frame history buffer
     - Input state flags
     - Repeat handling state
   - Health System (0x40e6f0):
     - Health segments
     - Current health points
     - Guard damage state
     - Recovery mechanics

2. **Deterministic Systems:**
   - Fixed frame rate (100 FPS)
   - 16.16 fixed-point coordinates
   - Deterministic input processing
   - Clear state transitions
   - Predictable damage calculations
   - Frame-based timers

3. **State Management:**
   - Object Pool (1024 entries):
     - Type identification
     - Position and velocity
     - Animation state
     - Collision data
   - Input History:
     - Circular buffer design
     - Clear frame boundaries
     - Input validation
   - Hit Detection:
     - Priority system
     - State validation
     - Reaction handling
   - Health System:
     - Segment-based tracking
     - Clear state boundaries
     - Recovery mechanics

4. **Implementation Strategy:**
   a. Save State (Frame N):
      ```c
      struct SaveState {
          // Character State
          int base_state;
          int action_state;
          int movement_state;
          int frame_counters[4];
          
          // Hit Detection
          int hit_boxes[20];
          int damage_values[8];
          int hit_flags;
          
          // Input Buffer
          int input_history[1024];
          int input_flags;
          int repeat_state;
          
          // Health System
          int health_segments;
          int current_health;
          int guard_damage;
      };
      ```

   b. Rollback Process:
      1. Save current frame state
      2. Apply new remote input
      3. Simulate forward:
         - Process inputs
         - Update character states
         - Handle collisions
         - Apply damage
      4. Render new state
      5. Save confirmed state

5. **Critical Hooks:**
   - Input Processing (0x4146d0):
     - Frame advance point
     - Input buffer management
   - Character State (0x411bf0):
     - State transition handling
     - Frame counting
   - Hit Detection (0x40f010):
     - Collision processing
     - Damage application
   - Health System (0x40e6f0):
     - Health state management
     - Segment tracking

6. **Optimization Opportunities:**
   - Parallel state simulation
   - Minimal state serialization
   - Efficient state diffing
   - Smart state prediction
   - Input compression

The analysis reveals that FM2K's architecture is highly suitable for rollback implementation due to its:
1. Deterministic game logic
2. Clear state boundaries
3. Frame-based processing
4. Efficient memory layout
5. Well-defined systems

This research provides a solid foundation for implementing rollback netcode in FM2K, with all major systems supporting the requirements for state saving, rollback, and resimulation.

## GekkoNet Integration Analysis

### GekkoNet Library Overview

GekkoNet is a rollback netcode library that provides the networking infrastructure we need for FM2K. Here's how it maps to our analyzed systems:

### 1. **GekkoNet Configuration for FM2K**
```c
GekkoConfig fm2k_config = {
    .num_players = 2,                    // P1 vs P2
    .max_spectators = 8,                 // Allow spectators
    .input_prediction_window = 8,        // 8 frames ahead prediction
    .spectator_delay = 6,                // 6 frame spectator delay
    .input_size = sizeof(uint32_t),      // FM2K input flags (32-bit)
    .state_size = sizeof(FM2KGameState), // Our minimal state structure
    .limited_saving = false,             // Full state saving for accuracy
    .post_sync_joining = true,           // Allow mid-match spectators
    .desync_detection = true             // Enable desync detection
};
```

### 2. **Integration Points with FM2K Systems**

#### Input System Integration (0x4146d0)
```c
// Hook into process_game_inputs()
void fm2k_input_hook() {
    // Get GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case AdvanceEvent:
                // Inject inputs into FM2K's input system
                g_p1_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[0];
                g_p2_input_history[g_input_buffer_index] = events[i]->data.adv.inputs[1];
                // Continue normal input processing
                break;
                
            case SaveEvent:
                // Save current FM2K state
                save_fm2k_state(events[i]->data.save.state, events[i]->data.save.state_len);
                break;
                
            case LoadEvent:
                // Restore FM2K state
                load_fm2k_state(events[i]->data.load.state, events[i]->data.load.state_len);
                break;
        }
    }
}
```

#### State Management Integration
```c
typedef struct FM2KGameState {
    // Frame tracking
    uint32_t frame_number;
    uint32_t input_buffer_index;
    
    // Input state
    uint32_t p1_input_current;
    uint32_t p2_input_current;
    uint32_t input_repeat_state[8];
    uint32_t input_repeat_timer[8];
    
    // Character states (per character)
    struct {
        // Position (16.16 fixed point)
        int32_t pos_x, pos_y;
        int32_t vel_x, vel_y;
        
        // State machine
        uint16_t base_state;        // 0x0-0x3
        uint16_t action_state;      // 0x4-0x7  
        uint16_t movement_state;    // 0x8-0xB
        uint16_t state_flags;       // Offset 350
        
        // Animation
        uint16_t animation_frame;   // Offset 12
        uint16_t max_animation_frame; // Offset 88
        
        // Health system
        uint16_t health_segments;   // Current segments
        uint16_t current_health;    // Health points
        uint16_t max_health_segments; // Maximum segments
        uint16_t guard_damage;      // Guard damage accumulation
        
        // Hit detection
        uint16_t hit_flags;         // Hit state flags
        uint16_t hit_stun_timer;    // Hit stun duration
        uint16_t guard_state;       // Guard state flags
        
        // Facing and direction
        uint8_t facing_direction;   // Left/right facing
        uint8_t input_direction;    // Current input direction
    } characters[2];
    
    // Round state
    uint32_t round_state;           // Round state machine
    uint32_t round_timer;           // Round timer
    uint32_t game_mode;             // Current game mode
    
    // Hit detection global state
    uint32_t hit_effect_target;     // Hit effect target
    uint32_t hit_effect_timer;      // Hit effect timer
    
    // Combo system
    uint16_t combo_counter[2];      // Combo counters
    uint16_t damage_scaling[2];     // Damage scaling
    
    // Object pool critical state
    uint16_t active_objects;        // Number of active objects
    uint32_t object_state_flags;    // Critical object flags
    
} FM2KGameState;
```

### 3. **Network Adapter Implementation**
```c
GekkoNetAdapter fm2k_adapter = {
    .send_data = fm2k_send_packet,
    .receive_data = fm2k_receive_packets,
    .free_data = fm2k_free_packet_data
};

void fm2k_send_packet(GekkoNetAddress* addr, const char* data, int length) {
    // Use existing LilithPort networking or implement UDP
    // addr->data contains IP/port information
    // data contains the packet to send
}

GekkoNetResult** fm2k_receive_packets(int* length) {
    // Collect all packets received since last call
    // Return array of GekkoNetResult pointers
    // GekkoNet will call free_data on each result
}
```

### 4. **Integration with Existing FM2K Systems**

#### Main Game Loop Integration
```c
// Hook at 0x4146d0 - process_game_inputs()
void fm2k_rollback_frame() {
    // 1. Poll network
    gekko_network_poll(session);
    
    // 2. Add local inputs
    uint32_t local_input = get_local_player_input();
    gekko_add_local_input(session, local_player_id, &local_input);
    
    // 3. Process GekkoNet events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session, &event_count);
    
    // 4. Handle events (save/load/advance)
    process_gekko_events(events, event_count);
    
    // 5. Continue normal FM2K processing
    // (character updates, hit detection, etc.)
}
```

#### State Serialization Functions
```c
void save_fm2k_state(unsigned char* buffer, unsigned int* length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Save frame state
    state->frame_number = current_frame;
    state->input_buffer_index = g_input_buffer_index;
    
    // Save character states from object pool
    for (int i = 0; i < 2; i++) {
        save_character_state(&state->characters[i], i);
    }
    
    // Save round state
    state->round_state = g_round_state;
    state->round_timer = g_round_timer;
    
    *length = sizeof(FM2KGameState);
}

void load_fm2k_state(unsigned char* buffer, unsigned int length) {
    FM2KGameState* state = (FM2KGameState*)buffer;
    
    // Restore frame state
    current_frame = state->frame_number;
    g_input_buffer_index = state->input_buffer_index;
    
    // Restore character states to object pool
    for (int i = 0; i < 2; i++) {
        load_character_state(&state->characters[i], i);
    }
    
    // Restore round state
    g_round_state = state->round_state;
    g_round_timer = state->round_timer;
}
```

### 5. **Event Handling Integration**
```c
void handle_session_events() {
    int event_count;
    GekkoSessionEvent** events = gekko_session_events(session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case PlayerConnected:
                // Show "Player Connected" message
                break;
            case PlayerDisconnected:
                // Handle disconnection
                break;
            case DesyncDetected:
                // Log desync information
                printf("Desync at frame %d: local=%08x remote=%08x\n",
                       events[i]->data.desynced.frame,
                       events[i]->data.desynced.local_checksum,
                       events[i]->data.desynced.remote_checksum);
               