# FM2K Input Processing Analysis: `process_game_inputs_FRAMESTEP_HOOK`

**Function Address:** `0x4146d0`  
**Function Size:** `0x170` bytes  
**Analysis Date:** 2024-12-19

## Overview

The `process_game_inputs_FRAMESTEP_HOOK` function is the core input processing system for FM2K. It handles raw input capture, input buffering, repeat logic, and input filtering for both versus and story modes. This function is called every frame and processes inputs for up to 8 input devices/players.

## Function Structure

### Input Capture Phase (Lines 16-30)
1. **Keyboard State Capture** (`0x4146d9`)
   - Calls `GetKeyboardState(KeyState)` to capture current keyboard state
   - This provides the raw input data for processing

2. **Frame Counter Management** (`0x4146f4`)
   - Increments `g_frame_counter` with circular buffer logic: `(g_frame_counter + 1) & 0x3FF`
   - Provides 1024-frame input history buffer

3. **Input Buffer Initialization** (`0x4146fb`)
   - Clears P1 input buffer: `memset(g_p1_input, 0, 0x20u)`
   - Resets all input states to zero

4. **Game Mode Input Handling** (`0x414710`)
   - **Versus Mode** (`g_game_mode < 3000` or `g_character_select_mode_flag`):
     - P1 input: `get_player_input(0, 0)` ¨ `g_p1_input[0]`
     - P2 input: `get_player_input(1, 1)` ¨ `g_p2_input`
     - Both inputs stored in history buffers
   - **Story Mode** (else):
     - P1 input: `get_player_input(g_current_player_side, 0)` ¨ `g_p1_input[0]`
     - Input stored in history buffer

### Input Processing Phase (Lines 31-72)
Processes all 8 input devices with sophisticated repeat logic:

1. **Input State Management**
   - `current_raw_input` = `g_p1_input[device_index]`
   - `previous_raw_input` = `g_prev_input_state[device_index]`
   - Updates previous state for next frame
   - Detects input changes: `current_raw_input & (previous_raw_input ^ current_raw_input)`

2. **Repeat Logic System**
   - **Held Input Detection**: Checks if `current_raw_input == g_input_repeat_state[device_index]`
   - **Timer Management**: Uses `g_input_repeat_timer[device_index]` for timing
   - **Initial Delay**: New inputs get `g_input_initial_delay` timer
   - **Repeat Delay**: Held inputs get `g_input_repeat_delay` timer
   - **Input Suppression**: Timer not expired ¨ `g_player_input_processed[device_index] = 0`

3. **Input Filtering**
   - **Bit Filtering**: Filters out specific bits if previously held
   - Bits 0-1: `& 0xFFFFFFFC` if `(previous_repeat_state & 3) != 0`
   - Bits 2-3: `& 0xFFFFFFF3` if `(previous_repeat_state & 0xC) != 0`

4. **Input Accumulation**
   - `accumulated_raw_input` |= `current_raw_input`
   - `accumulated_just_pressed` |= `current_input_changes`
   - `accumulated_processed_input` |= `current_processed_input`

### Output Phase (Lines 73-76)
Stores final processed results:
- `g_combined_processed_input` = `accumulated_processed_input`
- `g_player_input_flags` = `accumulated_just_pressed`
- `g_combined_raw_input` = `accumulated_raw_input`
- Returns `device_index * 4` (32 bytes processed)

## Key Global Variables

### Input Buffers
- `g_p1_input[8]` (`0x4259c0`) - Raw P1 input for each device
- `g_p2_input` (`0x4259c4`) - Raw P2 input
- `g_player_input_history[1024]` (`0x4280e0`) - Input history buffer
- `g_p2_input_history[1024]` (`0x4290e0`) - P2 input history buffer

### State Management
- `g_frame_counter` (`0x447ee0`) - Circular buffer index (0-1023)
- `g_prev_input_state[8]` (`0x447f00`) - Previous frame input states
- `g_input_repeat_state[8]` - Current repeat states for filtering
- `g_input_repeat_timer[8]` - Timers for repeat logic

### Processed Outputs
- `g_player_input_processed[8]` (`0x447f40`) - Processed inputs after repeat logic
- `g_player_input_changes[8]` - Input change detection (just-pressed)
- `g_combined_processed_input` - Final combined processed input
- `g_player_input_flags` - Combined input change flags
- `g_combined_raw_input` - Final combined raw input

### Configuration
- `g_input_initial_delay` - Initial delay for new inputs
- `g_input_repeat_delay` - Repeat delay for held inputs

## Input Processing Algorithm

### 1. Raw Input Capture
```
GetKeyboardState() ¨ Raw keyboard state
Frame counter increment (circular 1024-frame buffer)
Clear input buffers
Game mode detection ¨ Capture P1/P2 inputs
Store in history buffers
```

### 2. Repeat Logic Processing
```
For each device (0-7):
  current_raw = g_p1_input[device]
  previous_raw = g_prev_input_state[device]
  input_changes = current_raw & (previous_raw ^ current_raw)
  
  if (current_raw == g_input_repeat_state[device]):
    // Held input - use repeat logic
    timer = g_input_repeat_timer[device] - 1
    if (timer > 0):
      processed_input = 0  // Suppress input
    else:
      processed_input = current_raw
      timer = g_input_repeat_delay
  else:
    // New input - use initial delay
    processed_input = current_raw
    timer = g_input_initial_delay
    
    // Filter out bits if previously held
    if (previous_repeat_state & 3): processed_input &= 0xFFFFFFFC
    if (previous_repeat_state & 0xC): processed_input &= 0xFFFFFFF3
    
  g_input_repeat_state[device] = current_raw
  g_input_repeat_timer[device] = timer
```

### 3. Input Accumulation
```
accumulated_raw |= current_raw
accumulated_just_pressed |= input_changes
accumulated_processed |= processed_input
```

## Key Features

### Circular Buffer System
- 1024-frame input history using `g_frame_counter & 0x3FF`
- Enables input replay and rollback functionality
- Critical for netplay synchronization

### Sophisticated Repeat Logic
- **Initial Delay**: New inputs are allowed immediately but get initial delay timer
- **Repeat Delay**: Held inputs are suppressed until repeat timer expires
- **Bit Filtering**: Specific input bits are filtered if previously held
- **State Persistence**: Repeat states persist across frames

### Multi-Device Support
- Processes up to 8 input devices simultaneously
- Accumulates inputs from all devices into combined outputs
- Maintains separate state for each device

### Game Mode Awareness
- **Versus Mode**: Processes both P1 and P2 inputs
- **Story Mode**: Processes only P1 input with side awareness
- **Character Select**: Treated as versus mode

## Implementation Notes

### Memory Layout
- Input buffers are 32 bytes (`0x20u`) per device
- History buffers are 1024 entries deep
- All arrays use device index (0-7) for addressing

### Performance Characteristics
- Processes 8 devices per frame
- Uses bitwise operations for efficient input processing
- Circular buffer prevents memory allocation overhead

### Netplay Considerations
- Input history buffer enables frame-accurate replay
- Repeat logic must be deterministic across clients
- Input filtering prevents input desyncs

## Future Reimplementation Strategy

### Phase 1: Core Structure
1. Implement circular buffer system (1024 frames)
2. Implement raw input capture (SDL3 keyboard state)
3. Implement basic input processing loop (8 devices)

### Phase 2: Repeat Logic
1. Implement initial delay system
2. Implement repeat delay system
3. Implement bit filtering logic
4. Implement state persistence

### Phase 3: Game Mode Integration
1. Implement versus mode (P1/P2)
2. Implement story mode (P1 only)
3. Implement character select mode

### Phase 4: Netplay Integration
1. Integrate with GekkoNet input system
2. Implement input prediction
3. Implement rollback support
4. Implement desync detection

This analysis provides the foundation for creating a clean, modern reimplementation of FM2K's input processing system using SDL3 and GekkoNet. 