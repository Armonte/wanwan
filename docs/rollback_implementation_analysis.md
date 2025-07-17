# FM2K Rollback Implementation Analysis

## Current Problem: Frame Blocking Failure

### What We Think Is Happening
- Hook `process_game_inputs` at 0x4146D0
- When `can_advance_frame = false`, return early without calling original
- This should pause FM2K like Thorn's framestep tool

### What's Actually Happening
- We return early from our hook
- FM2K continues running and reaches title screen
- Game becomes unresponsive (can't input)
- "Blocking" logs appear but game isn't blocked

## Root Cause Analysis

### Thorn's Approach (WORKING)
```rust
// Creates process with DEBUG_ONLY_THIS_PROCESS
CreateProcessW(..., DEBUG_ONLY_THIS_PROCESS, ...)

// Replaces first instruction with INT3 breakpoint
handle.put_address(0x004146D0, &[0xcc]); // Replace PUSH EBX with INT3

// When function is called:
// 1. INT3 immediately triggers debug exception
// 2. ENTIRE PROCESS pauses waiting for debugger
// 3. External controller input resumes process
```

### Our Approach (BROKEN)
```cpp
// Hook function with MinHook
MH_CreateHook(0x4146D0, Hook_ProcessGameInputs, &original_process_inputs)

// In our hook:
if (!can_advance_frame) {
    return 0; // Return early, but process keeps running
}
```

## The Fundamental Difference

**Thorn's tool**: External debugger **pauses entire process**
**Our hook**: Returns from one function while **process continues**

## Why Our Blocking Fails

Looking at the decompiled function at 0x4146D0:
```cpp
int process_game_inputs_FRAMESTEP_HOOK(void) {
    GetKeyboardState(KeyState);                   // Line 16 - Gets input
    g_input_history_frame_index = (g_input_history_frame_index + 1) & 0x3FF; // Line 17 - INCREMENTS FRAME
    // ... rest of input processing
}
```

**The problem**: This function is called by FM2K's main game loop, but it's NOT the main game loop itself.

## FM2K's Actual Game Loop Structure

Based on our hooks and observations:

```
FM2K Main Loop (somewhere else)
├── Hook_RunGameLoop() - Called once at startup
├── Main Game Loop (unknown location)
│   ├── Hook_UpdateGameState() - Game logic updates  
│   ├── Hook_ProcessGameInputs() - Input processing <- WE HOOK THIS
│   ├── Rendering
│   ├── Audio
│   └── Window message handling
```

**Key insight**: We're only hooking INPUT PROCESSING, not the main loop that drives everything.

## What Actually Needs To Be Blocked

To implement true rollback, we need to control:

1. **Frame Advancement** - The core loop that drives everything
2. **State Updates** - Game logic progression  
3. **Rendering** - Visual updates
4. **Input Processing** - What we currently hook

## Possible Solutions

### Option 1: Find The Real Main Loop
- Use IDA to find the actual game loop that calls process_game_inputs
- Hook THAT function instead of just input processing
- This would give us true frame control

### Option 2: Message Pump Blocking  
- Hook Windows message processing (GetMessage/PeekMessage)
- Block the entire Windows message pump when rollback needs control
- More intrusive but might work

### Option 3: Sleep Injection
- Instead of returning early, inject delays/sleeps in our hook
- Make the input processing so slow that effectively pauses the game
- Crude but might work

### Option 4: Multiple Hook Points
- Hook ALL the functions that advance game state
- Block ALL of them simultaneously when rollback is active
- More complex but comprehensive

## Investigation Needed

1. **Find the main game loop** that calls our hooked functions
2. **Understand FM2K's threading model** - is it single-threaded?
3. **Identify all state-advancing functions** beyond just input processing
4. **Test if other hook points can achieve true blocking**

## ROOT CAUSE DISCOVERED

**The Problem**: We're hooking individual functions INSIDE the main loop, not the loop itself.

```cpp
// FM2K's ACTUAL main loop at 0x405AD0:
do {
    process_game_inputs_FRAMESTEP_HOOK();  // ← We hook this and return 0
    update_game_state();                   // ← We hook this and return 0  
    process_input_history(...);
    render_game();                         // ← This still runs!
    // ... message pump continues
} while (frame_loop_counter);
```

**Result**: Input and logic are blocked, but rendering and Windows messages continue.
**That's why**: We see title screen (rendering works) but can't input (input blocked).

## THE REAL SOLUTION

Hook `run_game_loop` at 0x405AD0 instead of individual functions:

```cpp
BOOL __cdecl Hook_RunGameLoop() {
    while (gekko_session_active) {
        if (!can_advance_frame) {
            // Sleep or yield - DON'T call original function
            Sleep(1);
            continue;
        }
        
        // Call original to advance exactly one frame
        return original_run_game_loop();
    }
}
```

This gives us **complete control** over the entire game loop, just like Thorn's debugger approach.