# fm2k fighter maker 2nd - rollback netcode implementation

## project status: functional but buggy

this document describes our current rollback netcode implementation for fm2k. we have a working system using gekkonet library with comprehensive save states and dual-client testing, but we are not production ready - still have a bunch of bugs, need to implement more debug features and do a ton of testing.

**what's working so far:**
- game runs at 100 fps (10ms per frame)
- gekkonet rollback networking integration  
- comprehensive save state system (50kb-850kb profiles)
- dual-client local testing infrastructure
- real-time rollback performance monitoring
- input recording and desync detection
- production mode toggle to reduce debug spam

**known issues:**
- still debugging random desyncs during gameplay
- save state optimization needs more work (currently 850kb, target 50-200kb)
- network stability issues under poor connections  
- need more comprehensive testing with different character combinations
- performance profiling shows occasional frame drops during heavy rollbacks

---

## architecture overview

### launcher system (like lilithport)
our implementation follows a proven launcher-based architecture:

1. user runs `FM2K_RollbackLauncher.exe`
2. launcher displays game selection ui  
3. user selects fm2k game and network settings
4. launcher creates fm2k process in suspended state
5. launcher injects `FM2KHook.dll` into the game process
6. game resumes with rollback netcode active

### core components

**launcher (`FM2K_RollbackLauncher.exe`):**
- imgui-based game selection interface
- dual-client local testing support
- real-time rollback performance monitoring
- gekkonet session management
- shared memory ipc with hook dll

**hook dll (`FM2KHook.dll`):**
- injected into fm2k game process
- hooks critical game functions for rollback
- gekkonet integration for networking
- save state system with multiple profiles
- input recording and desync detection

---

## technical implementation

### memory addresses (verified in ida)

**input system:**
```
0x4259C0: g_p1_input               // current p1 input state
0x4259C4: g_p2_input               // current p2 input state  
0x4280E0: g_p1_input_history[1024] // p1 input history (4096 bytes)
0x4290E0: g_p2_input_history[1024] // p2 input history (4096 bytes)
0x447EE0: g_input_buffer_index     // circular buffer index
```

**game state:**
```
0x470104: g_p1_stage_x             // player 1 x position
0x470108: g_p1_stage_y             // player 1 y position
0x47010C: g_p1_hp                  // player 1 health
0x470110: g_p1_max_hp              // player 1 max health
0x47030C: g_p2_hp                  // player 2 health
0x470310: g_p2_max_hp              // player 2 max health
0x470060: g_round_timer            // round timer
0x470044: g_game_timer             // game timer
0x41FB1C: g_random_seed            // rng seed (critical for determinism)
```

**timing system:**
```
0x447EE0: g_frame_counter          // current frame number
0x41E2F0: g_frame_time_ms          // frame duration (always 10ms)
0x447DD4: g_last_frame_time        // last frame timestamp
```

### hook points

we hook these critical addresses for rollback functionality:

**main hook:**
- `0x4146D0`: `process_game_inputs` - perfect insertion point for rollback logic

**input hooks (from lilithport analysis):**
- `0x41474A`: vs_p1_key - player 1 input in versus mode
- `0x414764`: vs_p2_key - player 2 input in versus mode
- `0x414729`: story_key - story mode input processing

**game logic hooks:**
- `0x404CD0`: update_game_state - main game state update
- `0x417A22`: rand_func - random number generation

---

## save state system

### profiles

we implemented three save state profiles for different use cases:

**minimal profile (~50kb):**
- core game state + active objects only
- optimized for performance
- smart object detection saves only active game entities

**standard profile (~200kb):**
- essential runtime state
- balanced size vs completeness
- recommended for production

**complete profile (~850kb):**
- everything captured
- used for debugging and research
- includes all 1023 game objects

### state structure

```c
struct CoreGameState {
    // input system
    uint32_t input_buffer_index;
    uint32_t p1_input_current;
    uint32_t p2_input_current;
    uint32_t p1_input_history[1024];
    uint32_t p2_input_history[1024];
    
    // player state
    uint32_t p1_stage_x, p1_stage_y;
    uint32_t p1_hp, p1_max_hp;
    uint32_t p2_hp, p2_max_hp;
    
    // game state
    uint32_t round_timer;
    uint32_t game_timer;
    uint32_t random_seed;
    
    // visual effects
    uint32_t effect_active_flags;
    uint32_t effect_timers[8];
    uint32_t effect_colors[8][3];
    uint32_t effect_targets[8];
};
```

---

## gekkonet integration

### session management

we use gekkonet for ggpo-style rollback networking:

```c
// gekko configuration
GekkoConfig config = {
    .num_players = 2,
    .input_size = sizeof(uint8_t),
    .state_size = sizeof(uint32_t) * 8,  // deterministic state
    .max_spectators = 0,
    .input_prediction_window = 10,
    .desync_detection = true
};
```

### event handling

gekkonet provides three main events:

**savevent:** triggered when we need to save game state
**loadevent:** triggered when we need to rollback to previous state  
**advanceevent:** triggered to advance frame with confirmed inputs

### input encoding

fm2k uses 11-bit input encoding:
```c
#define INPUT_LEFT     0x001
#define INPUT_RIGHT    0x002  
#define INPUT_UP       0x004
#define INPUT_DOWN     0x008
#define INPUT_BUTTON1  0x010
#define INPUT_BUTTON2  0x020
#define INPUT_BUTTON3  0x040
#define INPUT_BUTTON4  0x080
#define INPUT_BUTTON5  0x100
#define INPUT_BUTTON6  0x200
#define INPUT_BUTTON7  0x400
```

---

## production features

### dual-client testing

our launcher supports local dual-client testing:

- launches two fm2k instances (`wanwan` and `wanwan2` directories)
- configures gekkonet networking between clients
- provides separate debug logging per client
- real-time rollback statistics monitoring

### performance monitoring

we implemented comprehensive rollback performance tracking:

**rollback statistics:**
- rollbacks per second
- maximum rollback distance  
- average rollback frames
- total rollback count
- performance counters

**shared memory ipc:**
```c
struct SharedPerformanceStats {
    uint32_t rollback_count;
    uint32_t max_rollback_frames;
    uint32_t total_rollback_frames;
    uint32_t avg_rollback_frames;
    uint64_t last_rollback_time_us;
    uint32_t rollbacks_this_second;
    uint64_t current_second_start;
};
```

### input recording

for testing and desync analysis:

- binary input recording to `.dat` files
- timestamped input capture for both players
- structured desync reports with game state dumps
- replay functionality for debugging

### production mode

optimized settings for stable gameplay:

- reduced debug logging (error/warn only)
- optimized save frequency (32 frames vs 8 frames)
- production-grade error handling
- performance optimizations

---

## testing infrastructure

### multi-client testing ui

launcher provides comprehensive testing tools:

- game selection with automatic detection
- dual client launch with automatic configuration
- real-time log monitoring with syntax highlighting
- rollback performance visualization
- input recording management
- desync detection and reporting

### debug tools

- manual save/load state testing
- force rollback testing (configurable frame count)
- save state profile switching
- performance profiling and analysis

### file-based logging

- separate log files per client (`FM2K_Client1_Debug.log`, `FM2K_Client2_Debug.log`)
- timestamped entries with player identification
- color-coded log viewing in launcher ui
- automatic log rotation and management

---

## performance analysis

### frame budget (10ms per frame @ 100fps)

```
total available: 10ms
- input processing: ~0.5ms
- game logic: ~3-4ms  
- rendering: ~2-3ms
- rollback overhead: ~2-3ms
- buffer remaining: ~1ms
```

### memory usage

```
per-frame state: 50kb-850kb (profile dependent)
rollback buffer: ~24-48mb (120 frames)
input buffers: 8kb (existing)
network buffers: ~1mb
total additional: ~50mb
```

### optimization targets achieved

- state serialization: < 1ms per save/restore
- rollback execution: < 5ms for 10-frame rollback  
- memory footprint: < 50mb additional usage
- network latency: 2-3 frame input delay typical

---

## build system

### prerequisites
- mingw-w64 cross-compiler (i686-w64-mingw32-gcc/g++)
- cmake 3.20+
- ninja build system

### build commands
```bash
./make_build.sh  # configure cmake
./go.sh          # build and copy outputs
```

### outputs
- `FM2K_RollbackLauncher.exe` - main application
- `FM2KHook.dll` - hook dll for injection

---

## network protocol

### gekkonet messages

our implementation uses gekkonet's built-in message types:

**input message:** player inputs for specific frame
**input ack:** confirmation of received inputs  
**quality report:** network quality feedback
**synchronization:** frame synchronization

### desync detection

automatic desync detection using:
- fletcher32 checksum validation
- frame-by-frame state verification
- structured desync reports with full game state
- automatic recovery mechanisms

---

## deployment

### requirements

- windows (32-bit fm2k compatibility)
- administrator privileges (for process injection)
- fm2k game files in accessible directory
- network connectivity for online play

### installation

1. place built executables in fm2k game directory
2. run `FM2K_RollbackLauncher.exe`
3. select game and configure network settings
4. launch clients for local testing or online play

### compatibility

- tested with wonderfulworld and other fm2k games
- works with existing fm2k character and stage files
- compatible with standard fm2k configurations
- no modifications to original game files required

---

## future improvements

### optimization opportunities

- differential state saving (only changed objects)
- compression for network traffic
- adaptive rollback window based on network conditions
- visual rollback smoothing

### feature additions

- spectator mode support
- replay system with rewind/fast-forward
- tournament bracket integration
- matchmaking system

### platform expansion

- potential linux support via wine
- streaming integration for tournaments
- mobile companion app for match organization

---

## technical notes

### security considerations

- process injection requires administrator privileges
- hook dll must be 32-bit to match fm2k architecture  
- windows defender may flag injection tools
- network traffic is unencrypted (gekkonet limitation)

### cross-platform limitations

- windows-only due to process injection requirements
- uses windows-specific apis (createprocess, dll injection)
- could potentially be ported using different integration methods

### research documentation

extensive reverse engineering documentation available:
- complete engine analysis and function mapping
- memory layout documentation with verified addresses
- hook point analysis and lilithport comparison
- performance benchmarking and optimization guides

---

this document represents our current rollback implementation for fm2k. we have the core functionality working but still debugging issues and need extensive testing before considering it production ready. the dual-client testing infrastructure helps us identify and fix problems but there's still work to do.