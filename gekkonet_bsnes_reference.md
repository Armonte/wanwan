# Gekkonet with BSNES-Netplay Reference

## Overview

Gekkonet is a rollback netcode library that provides deterministic networking for games. When integrated with bsnes-netplay, it demonstrates a clean separation between network synchronization and game state management.

## Key Architecture Principles

### 1. **State Size Optimization**
Gekkonet does NOT transmit the entire game state over the network. Instead:

- **Network State**: Only 4 bytes (`sizeof(int32)`) - just a frame counter
- **Local State**: Full SNES memory state stored locally in bsnes
- **Synchronization**: Frame-by-frame input synchronization with rollback capability

### 2. **Event-Driven Architecture**
The integration uses three main event types:

```cpp
enum Event : uint {
  None = 0,
  Input = 1,
  Save = 2,
  Load = 3,
};
```

## How BSNES-Netplay Works

### State Management
```cpp
// Only 4 bytes transmitted over network
netplay.config.state_size = sizeof(int32);

// Local state handling
case SaveEvent:
    // Save state locally in bsnes
    seria.save(wram, sizeof(wram));
    seria.save(cpu.regs, sizeof(cpu.regs));
    // ... other SNES components
    
case LoadEvent:
    // Load state locally from bsnes
    seria.load(wram, sizeof(wram));
    seria.load(cpu.regs, sizeof(cpu.regs));
    // ... other SNES components
```

### Input Handling
```cpp
case InputEvent:
    // Process input for current frame
    input.poll();
    // Apply input to game state
```

### Frame Synchronization
- Each frame, gekkonet sends only the input data
- The frame counter (4 bytes) serves as the "state" for network transmission
- Full game state is maintained locally in bsnes
- Rollback occurs by reloading local state and replaying inputs

## Lessons for FM2K Integration

### 1. **Keep Network State Minimal**
- Don't transmit full game state over network
- Use frame counters or minimal state identifiers
- Store complete state locally

### 2. **Separate Input from State**
- Input handling should be separate from state serialization
- Focus on capturing inputs correctly first
- State management can be optimized later

### 3. **Event-Driven Design**
- Use clear event types (Input, Save, Load)
- Handle each event type separately
- Maintain clean separation of concerns

### 4. **Local State Management**
- Serialize complete game state locally
- Use the local state for rollback operations
- Network only needs minimal synchronization data

## Current FM2K Issues

Based on the attached code, the current FM2K implementation appears to be over-engineered:

### Problems Identified:
1. **Complex State Serialization**: Trying to serialize too much data
2. **Over-Engineering**: Complex save/load mechanisms when simple input capture would suffice
3. **Missing Input Focus**: Not prioritizing input capture as the primary concern

### Recommended Approach:
1. **Start with Input Capture**: Focus on correctly capturing and transmitting inputs
2. **Minimal State**: Use frame counters or simple state identifiers
3. **Local State**: Keep full game state local, only sync inputs
4. **Simplify**: Remove complex serialization until basic input sync works

## Implementation Strategy for FM2K

### Phase 1: Input Capture
```cpp
// Focus on this first
static void CaptureRealInputs() {
    // Capture player inputs
    // Transmit via gekkonet
    // Handle input events
}
```

### Phase 2: Minimal State Sync
```cpp
// Use simple frame counter like bsnes
uint32_t frame_counter = 0;
// This becomes the "state" transmitted over network
```

### Phase 3: Local State Management
```cpp
// Only after input sync works
static bool SaveCompleteGameState() {
    // Save locally, don't transmit
}
```

## Key Takeaways

1. **Gekkonet is input-focused**: The network layer primarily handles input synchronization
2. **State is local**: Full game state stays on each machine
3. **Rollback is local**: When rollback occurs, reload local state and replay inputs
4. **Keep it simple**: Start with input capture, add complexity later
5. **4 bytes is enough**: The network state can be as small as a frame counter

This approach will significantly simplify the FM2K integration and avoid the over-engineering currently present in the codebase. 