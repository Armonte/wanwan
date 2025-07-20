# FM2K Rollback Netcode: Current Architecture & Development Roadmap

## Table of Contents
1. [Current Stable State](#current-stable-state)
2. [Architecture Overview](#architecture-overview)
3. [CSS Synchronization](#css-synchronization)
4. [Battle Phase Implementation](#battle-phase-implementation)
5. [Problem Analysis](#problem-analysis)
6. [CCCaster vs GekkoNet](#cccaster-vs-gekkonet)
7. [Development Roadmap](#development-roadmap)
8. [ArtMoney Integration Plan](#artmoney-integration-plan)

---

## Current Stable State

### ✅ **Achievements (December 2024)**
- **Crash-free operation** through CSS → Battle transitions
- **Stable CSS synchronization** using lockstep mode
- **600-frame stabilization period** for battle transitions
- **Clean handshake system** for character confirmation
- **Reduced logging spam** and performance optimizations

### 🏗️ **Current Architecture**
```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   TITLE_SCREEN  │ →  │ CHARACTER_SELECT │ →  │   IN_BATTLE     │
│   (Lockstep)    │    │   (Lockstep)     │    │ (Lockstep/Safe) │
└─────────────────┘    └──────────────────┘    └─────────────────┘
```

### 📊 **Performance Status**
- **CSS**: Stable, no desyncs, minimal lag
- **Battle**: Stable, no crashes, but limited rollback capability
- **Transitions**: Smooth with 600-frame stabilization
- **Network**: Pure lockstep (`input_prediction_window = 0`)

---

## Architecture Overview

### **Core Components**

#### **1. GekkoNet Configuration**
```cpp
GekkoConfig config;
config.num_players = 2;
config.input_prediction_window = 0;  // LOCKSTEP mode
config.input_size = sizeof(uint8_t);  // 8-bit input
config.state_size = sizeof(uint32_t); // Minimal state
config.desync_detection = true;       // Checksum validation
```

#### **2. Game State Machine**
```cpp
enum class GamePhase {
    TITLE_SCREEN = 1000,
    CHARACTER_SELECT = 2000, 
    IN_BATTLE = 3000
};

enum class SyncStrategy {
    LOCKSTEP,   // Frame-perfect sync (current)
    ROLLBACK    // Prediction + rollback (future)
};
```

#### **3. Sync Event Flow**
```
Input Capture → GekkoNet Send → Network → GekkoNet Receive → Game Apply
     ↓              ↓                          ↓              ↓
Local Input    gekko_add_local_input()    AdvanceEvent   Process Inputs
```

### **Memory Layout**
- **FM2K Integration**: `FM2K_Integration.h` - Core memory addresses
- **ArtMoney Variables**: `artmoney_table_types.md` - 100+ game variables
- **Verified Addresses**: Live-tested memory locations for HP, coordinates, etc.

---

## CSS Synchronization

### **Current Implementation**
```cpp
// CSS Sync Strategy: Pure Lockstep + 0xFF Handshake
class CharSelectSync {
    bool confirmation_sent_;      // Local player confirmed
    bool confirmation_received_;  // Remote player confirmed  
    bool handshake_completed_;    // Both players ready
};
```

### **Handshake Protocol**
1. **Character Selection**: Players navigate using synchronized inputs
2. **Confirmation**: Local player confirms → Send `0xFF` signal
3. **Remote Receipt**: Receive `0xFF` → Mark remote confirmed
4. **Transition**: Both confirmed → Proceed to battle

### **Input Flow**
```
P1 Input → GekkoNet → P2 Receives P1 Input
P2 Input → GekkoNet → P1 Receives P2 Input
Both players process same inputs on same frame = Perfect sync
```

### **Why Lockstep Works for CSS**
- **Deterministic file loading**: Both clients load character previews simultaneously
- **No timing differences**: Frame-perfect synchronization prevents desyncs
- **Simple confirmation**: 0xFF handshake ensures both players ready

### **Performance Optimizations**
- **CSS updates**: Every 5 frames (reduced from every frame)
- **CSS logging**: Every 100 frames (reduced spam)
- **State resets**: Clean slate for each CSS session

---

## Battle Phase Implementation

### **Current State: "Safe Mode"**
```cpp
// Battle Phase: Lockstep with Minimal Saves
if (strategy == SyncStrategy::ROLLBACK) {
    // TEMPORARY: Disable object scanning to prevent crashes
    SDL_LogInfo("Battle minimal save (object scanning disabled)");
    // Use 8-byte minimal save with 0xBB marker
    break; // Skip all object operations
}
```

### **Stabilization System**
```cpp
const uint32_t STABILIZATION_FRAMES = 600; // 6 seconds at 100 FPS

if (frames_in_battle < STABILIZATION_FRAMES) {
    return SyncStrategy::LOCKSTEP;  // Stay safe during transition
} else {
    return SyncStrategy::ROLLBACK;  // Enable rollback (if implemented)
}
```

### **Event Handling**
- **SaveEvent**: Minimal 8-byte saves with markers
- **LoadEvent**: Skipped entirely (no restoration)
- **AdvanceEvent**: Standard input synchronization

### **Why Object Scanning Was Disabled**
1. **Crash source**: Complex object pool operations during transitions
2. **Timing sensitive**: Object scanning during state changes caused instability  
3. **Memory complexity**: 4KB+ states with intricate serialization
4. **Safer alternative**: ArtMoney variables provide deterministic state

---

## Problem Analysis

### **🚨 Primary Issues**

#### **1. CSS Lockstep Dependency**
```
Problem: CSS only works with input_prediction_window = 0
Root Cause: File loading during character preview causes timing differences
Impact: Cannot use rollback prediction during CSS
```

#### **2. Battle Desync Vulnerability**  
```
Problem: Players can desync in battle by taking different actions
Root Cause: No state synchronization (object scanning disabled)
Impact: Rollback netcode not functional during gameplay
```

#### **3. State Capture Gap**
```
Problem: Object pool scanning too complex and crash-prone
Root Cause: FM2K's object system not well-suited for rollback capture
Impact: Cannot implement proper save/load states
```

### **🎯 Required Solutions**

#### **CSS Liberation Strategy**
- Implement CCCaster-style input filtering
- Add timing validation and lockout periods
- Remove dependency on pure lockstep synchronization

#### **ArtMoney State Integration**  
- Replace object scanning with variable capture
- Use verified memory addresses for deterministic state
- Implement lightweight serialization (200 bytes vs 4KB)

---

## CCCaster vs GekkoNet

### **CCCaster's CSS Approach**
```cpp
// CCCaster Strategy: Rollback + Input Filtering
- Rollback netcode throughout (no mode switching)
- Input filtering with 150-frame lockout periods  
- Timing validation prevents rapid inputs
- State sharing via network messages
- Character selection desyncs prevented by input gates
```

### **Our Current Approach**
```cpp
// Our Strategy: Mode Switching + Lockstep
- Mode switching (lockstep → rollback)
- Pure lockstep for CSS synchronization  
- No input filtering (relies on lockstep timing)
- Minimal network state (8-byte saves)
- Character selection synced by frame-perfect timing
```

### **GekkoNet's Advantages**
```cpp
// GekkoNet Capabilities We Can Leverage
- Built-in rollback with prediction windows
- State serialization system ready for use
- Automatic desync detection with checksums  
- Frame advancement control for timing
- Network abstraction handles complexity
```

### **Hybrid Approach Benefits**
```
CCCaster Input Filtering + GekkoNet Rollback = Best of Both Worlds
- Robust CSS sync without lockstep dependency
- Proper rollback during battle gameplay
- Deterministic state via ArtMoney variables
- Performance optimization through selective sync
```

---

## Development Roadmap

### **Phase 1: CSS Input Filtering (Immediate Priority)**

#### **Objectives**
- Remove CSS dependency on lockstep mode
- Implement CCCaster-style input filtering
- Enable CSS with `input_prediction_window = 3`

#### **Implementation Plan**
```cpp
class CSSInputFilter {
    uint32_t css_frame_count_;
    uint32_t last_input_frame_;
    
    uint32_t FilterCSSInput(uint32_t raw_input, uint32_t css_frames) {
        // 150-frame lockout for confirmation inputs
        if (css_frames < 150) {
            raw_input &= ~(0x10 | 0x20); // Remove START/CONFIRM
        }
        
        // 3-frame lockout after any input
        if (css_frames - last_input_frame_ < 3) {
            raw_input &= ~(0x10 | 0x20 | 0x40); // Remove confirm/cancel
        }
        
        return raw_input;
    }
};
```

#### **Testing Strategy**
1. Implement input filtering in current lockstep system
2. Verify CSS still works with filtering enabled
3. Switch to `input_prediction_window = 3`  
4. Test CSS synchronization with rollback prediction
5. Validate no desyncs during character selection

### **Phase 2: ArtMoney State Integration**

#### **State Structure Design**
```cpp
struct FM2KGameState {
    // Core timing (8 bytes)
    uint32_t timer;           // 0x47DB94 - Game timer
    uint32_t frame_counter;   // Internal frame tracking
    
    // Player positions (16 bytes)  
    uint32_t p1_coord_x;      // 0x4ADCC3 - P1 X coordinate
    uint32_t p1_coord_y;      // Calculated offset - P1 Y coordinate
    uint32_t p2_coord_x;      // Calculated offset - P2 X coordinate
    uint32_t p2_coord_y;      // Calculated offset - P2 Y coordinate
    
    // Player health (8 bytes)
    uint32_t p1_hp;           // 0x47010C - P1 current HP
    uint32_t p2_hp;           // 0x47030C - P2 current HP
    
    // Character variables (48 bytes)
    uint8_t char_vars_p1[16]; // Character variables A-P for P1
    uint16_t char_vars_p2[16]; // Character variables A-P for P2  
    
    // System variables (32 bytes)
    uint8_t system_vars_core[16];  // Critical system variables
    uint16_t system_vars_ext[8];   // Extended system state
    
    // Input state (8 bytes)
    uint32_t p1_input_state;  // Current input state
    uint32_t p2_input_state;  // Current input state
    
    // Total: ~120 bytes (vs 4KB+ object scanning)
};
```

#### **Memory Address Integration**
```cpp
namespace FM2K::ArtMoney {
    // Verified addresses from artmoney_table_types.md
    constexpr uintptr_t TIMER_ADDR = 0x47DB94;
    constexpr uintptr_t P1_COORD_X_BASE = 0x4ADCC3;  // +6212F offset
    constexpr uintptr_t P1_HP_ADDR = 0x47010C;       // Verified
    constexpr uintptr_t P2_HP_ADDR = 0x47030C;       // Verified
    
    // Character variables (A-P mapping)
    constexpr uintptr_t CHAR_VAR_A_P1 = 0x4ADCC3 + 0x97DE7;
    // ... Additional mappings from ArtMoney table
}
```

#### **Implementation Strategy**
1. **State Capture**: Read ArtMoney variables during SaveEvent
2. **State Restoration**: Write ArtMoney variables during LoadEvent  
3. **Validation**: Compare state checksums for desync detection
4. **Optimization**: Profile capture/restore performance

### **Phase 3: Battle Rollback Enablement**

#### **Rollback State Implementation**
```cpp
class ArtMoneyStateManager {
    FM2KGameState CaptureState() {
        FM2KGameState state;
        
        // Direct memory reads from verified addresses
        state.timer = ReadMemory<uint32_t>(FM2K::ArtMoney::TIMER_ADDR);
        state.p1_hp = ReadMemory<uint32_t>(FM2K::ArtMoney::P1_HP_ADDR);
        state.p2_hp = ReadMemory<uint32_t>(FM2K::ArtMoney::P2_HP_ADDR);
        // ... Read all critical variables
        
        return state;
    }
    
    void RestoreState(const FM2KGameState& state) {
        // Direct memory writes to restore state
        WriteMemory(FM2K::ArtMoney::TIMER_ADDR, state.timer);
        WriteMemory(FM2K::ArtMoney::P1_HP_ADDR, state.p1_hp);
        WriteMemory(FM2K::ArtMoney::P2_HP_ADDR, state.p2_hp);
        // ... Restore all variables
    }
};
```

#### **Integration with GekkoNet**
```cpp
// Replace object scanning in SaveEvent
case SaveEvent: {
    if (strategy == SyncStrategy::ROLLBACK) {
        auto state = artmoney_manager.CaptureState();
        auto serialized = state.Serialize();
        
        *update->data.save.state_len = serialized.size();
        memcpy(update->data.save.state, serialized.data(), serialized.size());
        *update->data.save.checksum = state.CalculateChecksum();
    }
}

// Replace object restoration in LoadEvent  
case LoadEvent: {
    if (strategy == SyncStrategy::ROLLBACK) {
        FM2KGameState state;
        state.Deserialize(update->data.load.state, update->data.load.state_len);
        artmoney_manager.RestoreState(state);
    }
}
```

---

## ArtMoney Integration Plan

### **Variable Priority Classification**

#### **Tier 1: Critical Sync Variables (Must Have)**
```cpp
// Core gameplay state that affects synchronization
uint32_t timer;           // Game timer - affects all frame timing
uint32_t p1_hp;           // P1 health - affects win conditions  
uint32_t p2_hp;           // P2 health - affects win conditions
uint32_t p1_coord_x;      // P1 position - affects collision
uint32_t p1_coord_y;      // P1 position - affects collision
uint32_t p2_coord_x;      // P2 position - affects collision  
uint32_t p2_coord_y;      // P2 position - affects collision
```

#### **Tier 2: Character State Variables (Important)**
```cpp
// Character-specific variables that affect gameplay logic
uint8_t char_vars_p1[16]; // Character variables A-P for P1
uint16_t char_vars_p2[16]; // Character variables A-P for P2
uint32_t p1_super;        // Super meter state
uint32_t p2_super;        // Super meter state  
uint32_t p1_special_stock; // Special move availability
uint32_t p2_special_stock; // Special move availability
```

#### **Tier 3: System Variables (Optional)**
```cpp
// System state that may affect edge cases
uint8_t system_vars[16];  // System variables A-P
uint16_t round_number;    // Current round
uint32_t map_coord_x;     // Stage position
uint32_t map_coord_y;     // Stage position
```

### **Memory Address Verification Strategy**
```cpp
class AddressValidator {
    bool ValidateAddress(uintptr_t addr, size_t size) {
        // Check if address is readable/writable
        return !IsBadReadPtr((void*)addr, size) && 
               !IsBadWritePtr((void*)addr, size);
    }
    
    void ValidateAllAddresses() {
        // Validate all ArtMoney addresses on startup
        // Log any invalid addresses for debugging
        // Provide fallback for missing variables
    }
};
```

### **Performance Considerations**
```cpp
// State capture optimization
- Target: <1ms capture time
- Size: ~120 bytes total state
- Frequency: Every rollback frame (up to 100 FPS)
- Memory: Direct reads, no object traversal

// State restoration optimization  
- Target: <1ms restoration time
- Atomic: All-or-nothing restoration
- Validation: Checksum verification
- Error handling: Graceful failure modes
```

---

## Implementation Timeline

### **Week 1: CSS Input Filtering**
- [ ] Research CCCaster input filtering implementation
- [ ] Design input filter architecture for FM2K
- [ ] Implement 150-frame lockout system
- [ ] Add rapid input prevention logic
- [ ] Test CSS with current lockstep system

### **Week 2: CSS Rollback Testing** 
- [ ] Switch to `input_prediction_window = 3`
- [ ] Test CSS synchronization with prediction
- [ ] Debug any desyncs with input filtering
- [ ] Validate character selection stability
- [ ] Performance testing and optimization

### **Week 3: ArtMoney State Design**
- [ ] Validate all ArtMoney memory addresses
- [ ] Design FM2KGameState structure  
- [ ] Implement state capture/restore functions
- [ ] Add serialization/deserialization
- [ ] Unit test state operations

### **Week 4: Battle Rollback Integration**
- [ ] Replace object scanning with ArtMoney state
- [ ] Implement SaveEvent using variable capture
- [ ] Implement LoadEvent using variable restoration  
- [ ] Test basic rollback functionality
- [ ] Debug any state synchronization issues

### **Week 5: Validation & Optimization**
- [ ] Comprehensive rollback testing
- [ ] Performance profiling and optimization
- [ ] Edge case testing and bug fixes
- [ ] Documentation updates
- [ ] Production readiness assessment

---

## Success Criteria

### **Phase 1 Success: CSS Liberation**
- ✅ CSS works with `input_prediction_window = 3`
- ✅ No desyncs during character selection
- ✅ Smooth transitions from CSS to battle
- ✅ Input filtering prevents rapid selection issues

### **Phase 2 Success: ArtMoney Integration**  
- ✅ State capture <1ms execution time
- ✅ State restoration <1ms execution time
- ✅ State size <200 bytes per frame
- ✅ All critical variables properly captured/restored

### **Phase 3 Success: Battle Rollback**
- ✅ No desyncs during battle gameplay
- ✅ Smooth rollback with 3-frame prediction
- ✅ Stable 100 FPS performance
- ✅ Proper win condition synchronization

### **Overall Success: Production Rollback**
- ✅ Stable netplay from CSS through battle completion
- ✅ CCCaster-level functionality with GekkoNet reliability
- ✅ No crashes, no desyncs, optimal performance
- ✅ Ready for public release and tournament use

---

*Last Updated: December 2024*  
*Status: Phase 1 (CSS Input Filtering) - Ready to Begin*