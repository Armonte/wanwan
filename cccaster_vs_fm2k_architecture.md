# CCCaster vs FM2K Architecture Comparison

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

This is why CCCaster works - it doesn't fight the game's architecture, it works WITH it by writing inputs at the right time and letting the game process them naturally.