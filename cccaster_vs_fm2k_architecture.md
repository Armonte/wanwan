# CCCaster vs FM2K Architecture Analysis

## Overview
This document analyzes how CCCaster interfaces with Melty Blood (MBAA) binary and compares it to our FM2K implementation approach.

## CCCaster Binary Interface Architecture

### 1. **DLL Injection & Hook System**
CCCaster operates as an injected DLL that hooks into the game process:

#### Hook Installation (`DllHacks.cpp`)
```cpp
// Pre-load hooks (before game fully initializes)
void initializePreLoad() {
    WRITE_ASM_HACK(hookMainLoop);      // Hook the main game loop
    WRITE_ASM_HACK(hijackControls);    // Hijack input system
    WRITE_ASM_HACK(detectRoundStart);  // Detect round transitions
    WRITE_ASM_HACK(filterRepeatedSfx); // Sound effect filtering
}

// Post-load hooks (after game loads)
void initializePostLoad() {
    // Hook Windows message processing
    MH_CREATE_HOOK(WindowProc);
    
    // Hook DirectX for frame rate control
    InitDirectX(windowHandle);
    HookDirectX();
}
```

### 2. **Frame Synchronization**
CCCaster uses a world timer monitoring approach:

#### World Timer Monitor (`DllMain.cpp`)
```cpp
// Monitor world timer changes to detect frames
RefChangeMonitor<Variable, uint32_t> worldTimerMoniter(
    this, Variable::WorldTime, *CC_WORLD_TIMER_ADDR
);

// Called when world timer changes
void changedValue(Variable var, uint32_t previous, uint32_t current) {
    if (var == Variable::WorldTime) {
        frameStep(); // Process the frame
    }
}
```

### 3. **Input Handling Architecture**
CCCaster has a sophisticated multi-layer input system:

#### Input Flow
1. **Windows Message Capture** (`WindowProc` hook)
   - Captures WM_KEYDOWN/WM_KEYUP messages
   - Injects into KeyboardManager

2. **Controller Updates** (`frameStepNormal`)
   ```cpp
   // Update controller state once per frame
   KeyboardState::update();
   updateControls(&localInputs[0]);
   ```

3. **Direct Memory Write**
   - Writes inputs directly to game memory
   - Bypasses game's input processing initially

### 4. **Rollback State Management**
CCCaster's rollback system (`DllRollbackManager.cpp`):

#### State Structure
```cpp
struct GameState {
    NetplayState netplayState;
    uint64_t startWorldTime;
    IndexedFrame indexedFrame;
    std::fenv_t fp_env;        // Floating point environment
    char* rawBytes;            // Raw memory dump
};
```

#### Save/Load Process
```cpp
void saveState(const NetplayManager& netMan) {
    // Save floating point state
    fegetenv(&fp_env);
    
    // Dump memory regions
    for (const MemDump& mem : allAddrs.addrs)
        mem.saveDump(dump);
}

void loadState(IndexedFrame indexedFrame, NetplayManager& netMan) {
    // Restore floating point state
    fesetenv(&fp_env);
    
    // Restore memory regions
    for (const MemDump& mem : allAddrs.addrs)
        mem.loadDump(dump);
}
```

### 5. **ASM Hacks & Direct Binary Modification**
CCCaster uses extensive ASM patches (`DllAsmHacks.cpp`):

#### Memory Write Function
```cpp
static int memwrite(void *dst, const void *src, size_t len) {
    DWORD old, tmp;
    VirtualProtect(dst, len, PAGE_READWRITE, &old);
    memcpy(dst, src, len);
    VirtualProtect(dst, len, old, &tmp);
    return 0;
}
```

#### Example ASM Hook
```cpp
extern "C" void callback() {
    // ASM callback injected into game loop
    if (appState == AppState::Polling) {
        mainApp->callback();
    }
}
```

## How CCCaster Works

```
1. FRAME LOOP (DllMain.cpp frameStep())
   |
   ├─> worldTimerMoniter.check() - Monitor game's internal timer
   |   └─> When timer changes = new frame
   |
   ├─> UPDATE CONTROLS (line 257-259)
   |   ├─> KeyboardState::update() - Poll keyboard state
   |   └─> updateControls(&localInputs[0]) - Convert to game format
   |
   ├─> NETPLAY MANAGER (line 474)
   |   └─> netMan.setInput(localPlayer, localInputs[0])
   |       └─> Stores input in internal buffer
   |
   ├─> NETWORK SEND (line 504)
   |   └─> dataSocket->send(netMan.getInputs(localPlayer))
   |
   └─> GAME ADVANCES NATURALLY
       └─> Game reads inputs from its normal memory locations
           └─> CCCaster writes to these locations OUTSIDE the input hook
```

**KEY POINTS:**
- CCCaster does NOT hook the game's input processing function
- It monitors the world timer to detect frame changes
- It captures keyboard input independently 
- It writes inputs to game memory before the game reads them
- The game's native input processing (with anti-repeat) happens naturally

## How Our FM2K Code Currently Works

```
1. HOOK ProcessGameInputs (FM2K_ProcessGameInputs_GekkoNet)
   |
   ├─> Call original_process_inputs() FIRST
   |   └─> Game applies anti-repeat logic
   |
   ├─> Read processed inputs from memory (0x447f40)
   |   └─> Capture what game actually processed
   |
   └─> Store for network transmission
       ├─> Host stores P1 input
       └─> Client stores P2 input

2. HOOK UpdateGameState (every frame)
   |
   └─> ProcessGekkoNetFrame()
       ├─> CheckFrameAdvanced() - Monitor frame counter
       └─> If frame advanced:
           └─> gekko_add_local_input() - Send to network
```

**PROBLEMS:**
1. We're hooking ProcessGameInputs which interferes with the natural flow
2. We're trying to capture inputs AFTER processing but still in the hook
3. The timing is wrong - we're in the middle of input processing

## How It SHOULD Work (CCCaster Style)

```
1. HOOK UpdateGameState (runs every frame BEFORE input processing)
   |
   ├─> CheckFrameAdvanced() - Monitor frame counter change
   |
   ├─> If new frame:
   |   ├─> Capture keyboard input directly (GetAsyncKeyState)
   |   └─> Send to GekkoNet
   |
   └─> When GekkoNet has synchronized inputs:
       └─> Write directly to game memory BEFORE ProcessGameInputs runs

2. Let ProcessGameInputs run NATURALLY (no hook)
   |
   └─> Game reads our written inputs
       └─> Applies anti-repeat logic naturally
```

## Key Differences

### CCCaster Approach:
1. **No input hook** - Let game process inputs naturally
2. **Frame monitoring** - Detect when game advances
3. **Direct memory writes** - Write inputs before game reads them
4. **Timing** - Everything happens BEFORE input processing

### Our Current Approach (WRONG):
1. **Hooking input processing** - Interfering with natural flow
2. **Capturing after processing** - Too late in the pipeline
3. **Complex timing** - Trying to work during input processing

### Correct Approach:
1. **Remove ProcessGameInputs hook** - Let it run naturally
2. **Write inputs in UpdateGameState** - Before input processing
3. **Direct keyboard capture** - Like CCCaster's updateControls()
4. **Simple timing** - Write inputs, then let game process them

## Key Binary Interface Techniques

### 1. **Frame Rate Control (`DllFrameRate.cpp`)**
```cpp
void PresentFrameEnd(IDirect3DDevice9 *device) {
    // Precise frame timing using millisecond resolution
    if (counter % 30 == 0) {
        while (now - last30f < (30 * 1000) / desiredFps)
            now = TimerManager::get().getNow(true);
    }
    
    // Update FPS counter in game memory
    *CC_FPS_COUNTER_ADDR = uint32_t(actualFps + 0.5);
}
```

### 2. **Direct Memory Writes (`DllMain.cpp:987-991`)**
```cpp
// Write game inputs directly to memory
procMan.writeGameInput(localPlayer, netMan.getInput(localPlayer));
procMan.writeGameInput(remotePlayer, netMan.getInput(remotePlayer));
procMan.writeGameInput(3, netMan.getInput(3));
procMan.writeGameInput(4, netMan.getInput(4));
```

### 3. **Rollback Sound Management**
```cpp
// Filter array to prevent repeated sounds during rollback
uint8_t sfxFilterArray[CC_SFX_ARRAY_LEN] = { 0 };
uint8_t sfxMuteArray[CC_SFX_ARRAY_LEN] = { 0 };

// Cancel unplayed sounds after rollback
if (AsmHacks::sfxFilterArray[j] == 0x80) {
    CC_SFX_ARRAY_ADDR[j] = 1;      // Play the SFX
    AsmHacks::sfxMuteArray[j] = 1;  // But mute it
}
```

## Key Differences: CCCaster vs FM2K

### 1. **Timing Architecture**
- **CCCaster**: Monitors world timer for frame detection
- **FM2K**: Hooks UpdateGameState directly at 100 FPS

### 2. **Input System**
- **CCCaster**: 
  - Captures Windows messages
  - Writes to game memory before input processing
  - Game applies its own anti-repeat logic
- **FM2K**: 
  - Hooks GetPlayerInput function
  - Maintains 1024-frame input history
  - Direct integration with game's input system

### 3. **State Management**
- **CCCaster**: 
  - Memory dumps of specific regions
  - Floating point state preservation
  - Sound effect filtering for rollback
- **FM2K**: 
  - Structured save state (SaveStateData)
  - Object pool preservation
  - Input history maintenance

### 4. **Network Architecture**
- **CCCaster**: Custom protocol with spectator support
- **FM2K**: GekkoNet integration (BSNES-style)

## Lessons for FM2K Implementation

### 1. **Frame Control**
CCCaster's approach:
```cpp
// Block rendering during certain states
*CC_SKIP_FRAMES_ADDR = 1;

// Control frame advancement
if (!ready) {
    return; // Block frame processing
}
```

### 2. **Input Timing**
CCCaster demonstrates that inputs must be written before the game's processing:
- It monitors the world timer to detect frame changes
- It captures keyboard input independently 
- It writes inputs to game memory before the game reads them
- The game's native input processing (with anti-repeat) happens

### 3. **State Integrity**
CCCaster shows the importance of:
- Preserving floating point state
- Managing sound effects during rollback
- Careful memory region selection

### 4. **Hook Architecture**
CCCaster's two-phase initialization:
1. **Pre-load**: Core game loop hooks
2. **Post-load**: Window handling, DirectX hooks

## Implementation Recommendations for FM2K

1. **Adopt World Timer Monitoring**: Consider monitoring FM2K's frame counter similarly to CCCaster

2. **Two-Phase Hook System**: Implement pre-load and post-load hook phases

3. **Direct Memory Management**: Use VirtualProtect for safe memory modifications

4. **Sound Effect Handling**: Implement rollback-aware sound filtering

5. **Floating Point State**: Preserve FPU state during save/load operations

## Implementation Changes Needed

1. **Remove the ProcessGameInputs hook entirely**
2. **In Hook_UpdateGameState:**
   - Capture keyboard input directly
   - Send to GekkoNet
   - When receiving networked inputs, write them to memory
3. **Let the game's ProcessGameInputs run naturally**
   - It will read our pre-written inputs
   - Apply anti-repeat logic
   - Everything works as designed

## Conclusion

CCCaster provides a proven architecture for fighting game netplay through:
- Precise frame synchronization via world timer monitoring
- Multi-layered input handling with proper timing
- Comprehensive state management for rollback
- Extensive binary modification capabilities

These patterns can be adapted for FM2K's rollback implementation while maintaining the game's 100 FPS timing and input history system. The key insight is that CCCaster doesn't fight the game's architecture - it works WITH it by writing inputs at the right time and letting the game process them naturally.