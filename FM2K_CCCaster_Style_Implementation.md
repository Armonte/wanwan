# FM2K CCCaster-Style Implementation with GekkoNet

## Overview
This document outlines how to implement CCCaster-style input handling for FM2K using GekkoNet as the networking library.

## Key Principles from CCCaster

1. **Don't hook input processing** - Let the game process inputs naturally
2. **Monitor frame advancement** - Detect when the game advances to a new frame
3. **Write inputs before processing** - Place inputs in memory before the game reads them
4. **Work with the game's architecture** - Don't fight against built-in systems

## FM2K Memory Addresses We Need

```cpp
// Frame counter - increments every frame at 100 FPS (never resets)
constexpr uint32_t FRAME_COUNTER_ADDR = 0x4456FC;  // Continuous counter

// Raw input addresses (where game reads inputs)
constexpr uint32_t P1_RAW_INPUT_ADDR = 0x4cfa00;
constexpr uint32_t P2_RAW_INPUT_ADDR = 0x4cfa02;

// Input history buffers (1024 frames each)
constexpr uint32_t P1_INPUT_HISTORY_ADDR = 0x4280E0;
constexpr uint32_t P2_INPUT_HISTORY_ADDR = 0x4290E0;

// Game mode for state detection
constexpr uint32_t GAME_MODE_ADDR = 0x470054;
```

## Proposed Implementation

### 1. Remove ProcessGameInputs Hook
```cpp
// DELETE this entire hook - we don't need it!
// Let the game's input processing run naturally
```

### 2. Enhanced UpdateGameState Hook
```cpp
int __cdecl Hook_UpdateGameState() {
    // Monitor frame counter like CCCaster monitors world timer
    // Using 0x4456FC which continuously increments (never resets)
    static uint32_t last_frame_count = 0;
    uint32_t current_frame_count = *(uint32_t*)FRAME_COUNTER_ADDR;
    
    // Detect new frame
    if (current_frame_count != last_frame_count) {
        // STEP 1: Capture local input directly
        uint16_t local_input = CaptureDirectInput();
        
        // STEP 2: Send to GekkoNet
        if (gekko_initialized && gekko_session) {
            // Add our input for this frame
            gekko_add_local_input(gekko_session, player_index, &local_input);
            
            // Process GekkoNet synchronization
            ProcessGekkoNetFrame();
        }
        
        // STEP 3: Write synchronized inputs to memory
        if (can_advance_frame) {
            // Write inputs where the game will read them
            *(uint16_t*)P1_RAW_INPUT_ADDR = synchronized_inputs[0];
            *(uint16_t*)P2_RAW_INPUT_ADDR = synchronized_inputs[1];
        } else {
            // Block frame advancement during sync
            return 0;
        }
        
        last_frame_count = current_frame_count;
    }
    
    // Call original UpdateGameState
    return original_update_game ? original_update_game() : 0;
}
```

### 3. Direct Input Capture
```cpp
uint16_t CaptureDirectInput() {
    uint16_t input = 0;
    
    // Direction inputs (D-pad/stick)
    if (GetAsyncKeyState(VK_UP) & 0x8000)    input |= 0x001; // Up
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)  input |= 0x002; // Down
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)  input |= 0x004; // Left
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) input |= 0x008; // Right
    
    // Button inputs
    if (GetAsyncKeyState('Z') & 0x8000)      input |= 0x010; // A
    if (GetAsyncKeyState('X') & 0x8000)      input |= 0x020; // B
    if (GetAsyncKeyState('C') & 0x8000)      input |= 0x040; // C
    if (GetAsyncKeyState('A') & 0x8000)      input |= 0x080; // D
    if (GetAsyncKeyState('S') & 0x8000)      input |= 0x100; // E
    if (GetAsyncKeyState('D') & 0x8000)      input |= 0x200; // F
    
    return input;
}
```

### 4. GekkoNet Integration
```cpp
void ProcessGekkoNetFrame() {
    // Process GekkoNet events
    int event_count = 0;
    auto events = gekko_session_events(gekko_session, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        switch (events[i]->type) {
            case SessionStarted:
                gekko_session_started = true;
                can_advance_frame = true;
                break;
                
            case AdvanceEvent:
                // GekkoNet says we can advance
                can_advance_frame = true;
                
                // Get synchronized inputs
                auto advance = events[i]->data.advance;
                memcpy(synchronized_inputs, advance.inputs, 
                       sizeof(uint16_t) * 2);
                break;
        }
    }
}
```

## Key Differences from Current Implementation

### Current (Problematic) Approach:
1. Hooks ProcessGameInputs
2. Tries to capture inputs during processing
3. Fights with game's anti-repeat logic
4. Complex timing issues

### New CCCaster-Style Approach:
1. No ProcessGameInputs hook
2. Captures input before processing
3. Works with anti-repeat logic
4. Simple, clean timing

## Benefits

1. **Cleaner Architecture**: No interference with game's input pipeline
2. **Better Timing**: Inputs written at the correct time
3. **Natural Anti-Repeat**: Game's built-in logic works perfectly
4. **Simpler Code**: Less complex than trying to hook mid-processing
5. **GekkoNet Compatible**: Clean integration with rollback networking

## Implementation Checklist

- [ ] Remove ProcessGameInputs hook completely
- [ ] Implement frame counter monitoring in UpdateGameState
- [ ] Add direct keyboard input capture
- [ ] Integrate GekkoNet session management
- [ ] Write inputs to memory before game reads them
- [ ] Test with anti-repeat scenarios (held buttons)
- [ ] Verify CSS input behavior
- [ ] Test rollback scenarios

## Expected Results

By following CCCaster's proven approach:
- Input timing issues will be resolved
- Anti-repeat will work naturally
- CSS character selection will work properly
- Rollback will have correct input data
- Overall cleaner and more maintainable code

The key insight is: **Don't fight the game - work with it!**