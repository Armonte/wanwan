# CSS Input History for Character Spawning

## Overview
This document explains how FM2K uses input history to manage character confirmation on Character Select Screen (CSS) and subsequent character object spawning.

## Input History Buffer System

### Memory Layout
- **P1 Input History**: 1024-frame buffer at `0x4280E0` (2048 bytes)
- **P2 Input History**: 1024-frame buffer at `0x4290E0` (2048 bytes)
- **Input Buffer Index**: Frame counter at `0x447EE0` (used as ring buffer index)
- **Input Changes Array**: Just-pressed detection at `0x447f60` (8 uint32_t values)

### Buffer Structure
```cpp
// From savestate.cpp:51-54
uint16_t* p1_input_history_ptr = (uint16_t*)0x4280E0;  // P1 input history (1024 frames)
uint16_t* p2_input_history_ptr = (uint16_t*)0x4290E0;  // P2 input history (1024 frames)
uint32_t* input_buffer_index_ptr = (uint32_t*)0x447EE0;  // Frame counter as buffer index
const size_t input_history_size = 1024 * sizeof(uint16_t);  // 2048 bytes each
```

## Character Confirmation Process

### Confirmation Status Addresses
- **P1 Confirmation**: `0x47019C` (set to 1 when character locked in)
- **P2 Confirmation**: `0x4701A0` (set to 1 when character locked in)

### CSS Input State Tracking
```cpp
// From savestate.cpp:56-57
uint32_t* player_input_changes_ptr = (uint32_t*)0x447f60;  // g_player_input_changes[8] array
```

This array tracks just-pressed button states for detecting confirmation inputs.

## Input History Role in Character Spawning

### 1. Motion Input Support
The 1024-frame input history enables:
- Complex motion inputs (quarter-circle forward, dragon punch motions)
- Character-specific special moves that require input history
- Proper move execution after character spawning

### 2. Rollback State Preservation
From `savestate.cpp:97-99`:
```cpp
memcpy(save_data->p1_input_history, p1_input_history_ptr, input_history_size);
memcpy(save_data->p2_input_history, p2_input_history_ptr, input_history_size);
save_data->input_buffer_index = *input_buffer_index_ptr;
```

This ensures complete input history is preserved during rollback operations.

### 3. CSS Handler Integration
From `hooks.cpp:Hook_CSS_Handler()`:
- Monitors `g_combined_raw_input` at `0x4cfa04` for current frame inputs
- Tracks `g_player_input_changes` at `0x447f60` for button press detection
- Logs CSS handler calls when button inputs are detected

## Key Mechanisms

### Input History Buffer Management
- **Ring Buffer**: Uses `g_input_buffer_index & 0x3FF` for 1024-frame wrap-around
- **Frame-Accurate**: Each frame's input is stored at specific buffer position
- **Motion Detection**: Enables complex input sequence recognition

### Character Object Spawning Flow
1. Player presses confirm button on CSS
2. Input change detected in `g_player_input_changes` array
3. Confirmation flag set at `0x47019C`/`0x4701A0`
4. Character objects spawned with access to full input history
5. Motion inputs immediately available for character moves

### Critical for Rollback
The input history preservation is essential because:
- Characters need immediate access to input history upon spawning
- Rollback operations must maintain input sequence integrity
- Motion inputs require historical context to function properly

## Implementation Notes

### Current State Analysis
- Confirmation addresses currently show `0xff` values (uninitialized)
- CSS handler actively monitoring input changes
- Input history buffers properly preserved in save states
- System ready for character confirmation detection

### Integration Points
- `savestate.cpp`: Input history preservation for rollback
- `hooks.cpp`: CSS handler monitoring and input detection
- `globals.h`: Memory address definitions for CSS confirmation