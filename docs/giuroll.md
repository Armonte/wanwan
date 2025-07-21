# Giuroll - Rollback Netcode Implementation

## Overview

Giuroll is a network rollback mod for “Œ•û”ñ‘z“V‘¥ / Touhou 12.3 Hisoutensoku that significantly improves netplay responsiveness through advanced rollback netcode implementation. This document details the technical implementation of giuroll's state management and rollback system.

## Architecture

### Core Components

- **Rollbacker**: Manages frame state storage and rollback logic
- **Netcoder**: Handles network packet processing and input synchronization
- **Sound Manager**: Manages sound effects during rollback
- **Memory Manager**: Tracks allocations/deallocations for proper cleanup

### State Management Strategy

Giuroll uses a **memory snapshot approach** rather than traditional diffing for state management. This provides fast, accurate state restoration while maintaining high performance.

## State Saving Implementation

### Frame Dumping Process

The `dump_frame()` function captures the complete game state by reading specific memory regions:

```rust
pub unsafe fn dump_frame() -> Frame {
    // Save FPU state
    let w = unsafe {
        asm!(
            "FSAVE {fpst}",
            "FRSTOR {fpst}",
            fpst = sym FPST
        );
        FPST
    };

    let mut m = vec![];
    
    // Capture memory regions containing game state
    // Character data, bullets, effects, input buffers, etc.
}
```

### Memory Regions Captured

1. **Character Data** (0x8985e4)
   - Player positions, health, states
   - Character-specific data structures
   - Bullet patterns and projectiles

2. **Battle System** (0x8985ec)
   - Battle state and phase information
   - Effect systems and visual elements
   - Linked list structures for dynamic objects

3. **Input Buffers** (0x8986a0)
   - Network input buffers
   - Local input state
   - Input processing queues

4. **Sound System** (0x8985f0)
   - Sound effect states
   - Audio playback information

5. **Network State** (0x8985dc)
   - Connection state
   - Packet buffers
   - Network timing data

### State Structure

```rust
pub struct Frame {
    pub number: usize,                    // Frame number
    pub adresses: Box<[ReadAddr]>,       // Memory regions
    pub fp: [u8; 108],                  // FPU state
    pub frees: Vec<usize>,              // Deallocations
    pub allocs: Vec<usize>,             // Allocations
    pub weather_sync_check: u8,         // Sync validation
}
```

## Rollback System

### Rollbacker Implementation

```rust
pub struct Rollbacker {
    pub guessed: Vec<RollFrame>,        // Stored frame states
    pub current: usize,                 // Current frame
    pub rolling_back: bool,             // Rollback state
    pub enemy_inputs: EnemyInputHolder, // Opponent input tracking
    pub self_inputs: Vec<RInput>,      // Local input history
    pub weathers: HashMap<usize, u8>,  // Weather sync data
}
```

### Input Prediction System

Giuroll uses input prediction to reduce rollback frequency:

```rust
impl EnemyInputHolder {
    fn get_result(&self, frame: usize) -> Result<RInput, RInput> {
        match self.i.get(frame) {
            Some(Some(x)) => Ok(*x),           // Known input
            None if frame == 0 => Err([false; 10]), // Default
            Some(None) | None => {
                Err(self.get(frame - 1))        // Use previous frame
            }
        }
    }
}
```

### Rollback Detection

```rust
pub fn step(&mut self, iteration_number: usize) -> Option<()> {
    // Check if predicted input matches actual input
    if fr.enemy_input != self.enemy_inputs.get(fr.prev_state.number) {
        // ROLLBACK NEEDED
        self.rolling_back = true;
        fr.prev_state.clone().restore();
        // Apply correct input and continue
    }
}
```

## Network Implementation

### Packet Structure

```rust
pub struct NetworkPacket {
    id: usize,                    // Frame ID
    desyncdetect: u8,            // Desync detection
    delay: u8,                   // Network delay
    max_rollback: u8,            // Maximum rollback frames
    inputs: Vec<u16>,            // Input history
    last_confirm: usize,         // Last confirmed frame
    sync: Option<i32>,           // Timing sync data
}
```

### Network Processing

The `Netcoder` handles:
- Input synchronization between players
- Delay compensation
- Desync detection
- Timing synchronization
- Auto-delay adjustment

## Sound Management

### Rollback Sound Manager

```rust
pub struct RollbackSoundManager {
    sounds_that_did_happen: HashMap<usize, Vec<usize>>,
    sounds_that_maybe_happened: HashMap<usize, Vec<usize>>,
    pub current_rollback: Option<usize>,
}
```

### Sound Handling During Rollback

1. **Sound Tracking**: Records sounds that occur during predicted frames
2. **Rollback Cleanup**: Removes sounds that shouldn't have played
3. **Sound Skipping**: Forces cancellation of incorrect sounds
4. **Replay Protection**: Prevents duplicate sound playback

## Memory Management

### Allocation Tracking

```rust
unsafe extern "stdcall" fn heap_alloc_override(heap: isize, flags: u32, s: usize) -> *mut c_void {
    let ret = HeapAlloc(heap, flags, s);
    
    if GetCurrentThreadId() == REQUESTED_THREAD_ID.load(Relaxed) {
        store_alloc(ret as usize);
    }
    return ret;
}
```

### Memory Cleanup

- Tracks allocations during rollback frames
- Cleans up memory that shouldn't exist after rollback
- Prevents memory leaks during state restoration

## Performance Optimizations

### 1. Selective Memory Capture
Only captures memory regions known to contain game state, avoiding unnecessary data.

### 2. Lazy State Restoration
Only restores state when rollback is actually needed, reducing overhead.

### 3. Input Prediction
Reduces rollback frequency by predicting opponent inputs based on previous frames.

### 4. Efficient Memory Operations
Direct memory reading/writing for fast state capture and restoration.

## Configuration

### Key Settings

```ini
[Netplay]
default_delay=2
enable_auto_delay=yes
auto_delay_rollback=2
frame_one_freeze_mitigation=yes

[FramerateFix]
spin_amount=1500
enable_f62=no
```

### Auto-Delay System

- Automatically adjusts network delay based on connection quality
- Targets specific rollback frequency for optimal performance
- Monitors packet loss and latency

## Technical Details

### Hook System

Giuroll uses the `ilhook` library to intercept game functions:
- Input processing hooks
- Sound system hooks
- Network packet hooks
- Memory allocation hooks

### Thread Safety

- Uses atomic operations for shared state
- Mutex protection for critical sections
- Thread-specific memory tracking

### Error Handling

- Desync detection and recovery
- Network timeout handling
- Memory allocation failure recovery
- Sound system error handling

## Advantages Over Traditional Netcode

1. **Reduced Latency**: Rollback eliminates input delay
2. **Better Responsiveness**: Immediate input processing
3. **Robust Error Recovery**: Automatic desync detection and correction
4. **Adaptive Performance**: Auto-delay system for optimal experience
5. **Sound Accuracy**: Proper sound handling during rollback

## Limitations and Considerations

1. **Memory Usage**: Stores multiple frame states
2. **CPU Overhead**: State capture and restoration costs
3. **Complexity**: More complex than traditional netcode
4. **Sound Challenges**: Managing sound effects during rollback
5. **Memory Management**: Tracking allocations/deallocations

## Future Improvements

1. **Enhanced Prediction**: Better input prediction algorithms
2. **Optimized Memory Usage**: More efficient state storage
3. **Improved Sound System**: Better sound rollback handling
4. **Advanced Desync Detection**: More sophisticated desync detection
5. **Performance Monitoring**: Better performance metrics and optimization

## Conclusion

Giuroll's rollback implementation provides a significant improvement in netplay experience for Touhou Hisoutensoku. Through careful state management, input prediction, and efficient memory operations, it achieves low-latency netplay while maintaining game accuracy and performance. 