# LilithPort SDL3 Migration Strategy

## Executive Summary

LilithPort is currently a C++/CLI application using .NET Framework for UI (Windows Forms), networking (UdpClient), and system integration. This document outlines strategies for migrating to a modern SDL3-based architecture while preserving the core netplay functionality.

**Key Finding**: This is a complete rewrite, not a migration. The .NET dependencies are too deeply integrated for incremental porting.

**? MAJOR UPDATE**: The presence of **GekkoNet** in the vendored directory fundamentally changes our approach - we can leverage modern rollback netcode instead of the legacy delay-based system.

**? GAME-CHANGER**: **Complete FM2K engine analysis** provides the exact knowledge needed to build the ultimate rollback netplay client tailored specifically for Fighter Maker 2nd!

## Current Architecture Analysis

### Technology Stack
- **Language**: C++/CLI (Managed C++)
- **UI Framework**: Windows Forms (.NET Framework)
- **Networking**: System::Net::Sockets::UdpClient (**Delay-based netcode**)
- **Audio**: System::Media::SoundPlayer
- **Threading**: System::Threading
- **Build System**: Visual Studio 2008 (.vcproj)
- **Platform**: Windows-only
- **Target Game**: **Fighter Maker 2nd (FM2K)** - 100 FPS fighting game engine

### Core Components
1. **MainForm** (3,622 lines) - Primary UI and networking logic
2. **OptionForm** (3,016 lines) - Configuration dialog
3. **Networking Protocol** - Custom UDP packet system for game synchronization
4. **Game Integration** - **Memory patching for FM2K games** (specific memory addresses)
5. **Replay System** - Input recording/playback
6. **UPnP Integration** - Automatic port forwarding

## FM2K Engine Knowledge Advantage

### Why This Changes Everything
From the comprehensive FM2K reverse engineering research, we now know:

**? Perfect Architecture for Rollback:**
- **100 FPS fixed timestep** - Ideal for frame-based rollback
- **1024-frame input buffering** - Already exists in the engine!
- **Deterministic game logic** - All state variables identified
- **~400KB state size** - Manageable for rollback buffer
- **Existing frame skip** - Can process multiple frames quickly

**? Exact Hook Points:**
```cpp
// Current LilithPort hooks (delay-based):
0x41474A: VS_P1_KEY    // Player 1 input injection
0x414764: VS_P2_KEY    // Player 2 input injection  
0x404CD0: update_game_state // Main game state update
0x417A22: RAND_FUNC    // Random number generation

// With complete engine knowledge, we can implement proper rollback!
```

**? Complete State Requirements:**
```cpp
struct FM2K_RollbackState {
    // Object Pool (390KB) - All game entities
    GameObject objects[1023];           // 382 bytes each
    
    // Player State (100 bytes)
    uint32_t p1_hp, p2_hp;             // 0x4DFC85, 0x4EDCC4
    uint32_t p1_stage_x, p1_stage_y;   // 0x424E68, 0x424E6C
    uint32_t round_timer;               // 0x470060
    
    // Critical System State
    uint32_t random_seed;               // 0x41FB1C - RNG state
    uint32_t screen_x, screen_y;        // 0x447F2C, 0x447F30
    
    // Input State (8KB) - Already buffered!
    uint32_t p1_input_history[1024];    // 0x4280E0
    uint32_t p2_input_history[1024];    // 0x4290E0
    uint32_t input_buffer_index;        // 0x447EE0
};
```

## GekkoNet Integration Opportunity

### What is GekkoNet?
GekkoNet is a modern **rollback netcode library** already present in the vendored directory. It provides:

- **Rollback Networking**: Superior to delay-based netcode
- **Input Prediction**: Reduces perceived latency
- **Spectator Support**: Built-in spectating functionality  
- **Desync Detection**: Automatic detection and recovery
- **Cross-platform**: C API works everywhere
- **Network Abstraction**: Pluggable network adapters

### Current vs. GekkoNet Networking

**Current LilithPort (Delay-based):**
```cpp
// Legacy approach - wait for input from remote player
if (!remote_input_received) {
    delay_frame(); // Creates 3-8 frames of input lag
}
process_game_frame(local_input, remote_input);
```

**GekkoNet + FM2K Knowledge (Rollback-based):**
```cpp
// Modern approach - predict and rollback if needed
gekko_add_local_input(session, player_id, &input);
GekkoGameEvent** events = gekko_update_session(session, &count);

for (int i = 0; i < count; i++) {
    switch (events[i]->type) {
        case AdvanceEvent:
            // Use exact FM2K addresses for input injection
            inject_input_at_addresses(events[i]->data.adv.inputs);
            advance_fm2k_frame();
            break;
        case SaveEvent:
            // Save complete 400KB FM2K state
            save_fm2k_state(&events[i]->data.save);
            break;
        case LoadEvent:
            // Restore FM2K state to exact frame
            restore_fm2k_state(&events[i]->data.load);
            break;
    }
}
```

## Migration Strategies

### Strategy 1: FM2K-Optimized Rollback Client (ULTIMATE RECOMMENDATION)

**Target Architecture:**
```
„¡„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„¢    „¡„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„¢    „¡„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„¢
„    Dear ImGui    „     „    SDL3 Core      „     „    GekkoNet      „ 
„    (Modern UI)   „ ?„Ÿ„Ÿ?„   (Audio/Window)  „ ?„Ÿ„Ÿ?„   (Rollback)     „ 
„¤„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„£    „¤„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„£    „¤„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„£
         „                        „                        „ 
         „¤„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„©„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„£
                                 ¥
              „¡„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„¢
              „        FM2K State Manager            „ 
              „   (Complete Engine Knowledge)        „ 
              „   ? 1024 Objects ~ 382 bytes        „ 
              „   ? Exact memory addresses          „ 
              „   ? Deterministic state save/load   „ 
              „¤„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„Ÿ„£
```

**Technology Stack:**
- **UI**: Dear ImGui + SDL3 (modern, responsive interface)
- **Networking**: **GekkoNet** (rollback netcode library)
- **Game Integration**: **Direct memory injection** (using FM2K research)
- **Audio**: SDL3 (cross-platform audio)
- **JSON**: nlohmann/json (configuration)
- **Build**: CMake + vcpkg (modern build system)

**Revolutionary Advantages:**
- **? Zero Input Lag**: Rollback eliminates delay-based lag
- **? FM2K-Optimized**: Tailored specifically to FM2K's 100 FPS architecture
- **? Perfect State Management**: Complete knowledge of all 400KB game state
- **? Exact Performance**: Leverages existing 1024-frame buffering
- **? Cross-platform**: Works on Windows, Linux, macOS
- **?? Robust**: Automatic desync detection and recovery

**FM2K-Specific Integration:**
```cpp
class FM2K_RollbackClient {
private:
    GekkoSession* session_;
    GekkoConfig config_;
    
    // FM2K-specific memory management
    HANDLE fm2k_process_;
    void* object_pool_ptr_;      // 0x4701E0
    void* input_history_ptr_;    // 0x4280E0
    void* game_state_ptr_;       // Various addresses
    
public:
    bool initialize() {
        if (!gekko_create(&session_)) return false;
        
        // Optimized for FM2K's 100 FPS
        config_.num_players = 2;
        config_.max_spectators = 8;
        config_.input_prediction_window = 3;  // 30ms prediction at 100 FPS
        config_.input_size = sizeof(uint32_t); // 11-bit input mask
        config_.state_size = sizeof(FM2K_RollbackState); // ~400KB
        config_.desync_detection = true;
        
        gekko_start(session_, &config_);
        return attach_to_fm2k_process();
    }
    
    void update_frame() {
        // Read current FM2K input
        uint32_t current_input = read_fm2k_input();
        gekko_add_local_input(session_, 0, &current_input);
        
        // Process rollback events
        int event_count;
        GekkoGameEvent** events = gekko_update_session(session_, &event_count);
        
        for (int i = 0; i < event_count; i++) {
            handle_fm2k_event(events[i]);
        }
    }
    
private:
    void handle_fm2k_event(GekkoGameEvent* event) {
        switch (event->type) {
            case AdvanceEvent:
                // Inject inputs at exact FM2K addresses
                inject_inputs_fm2k(event->data.adv.inputs);
                advance_fm2k_single_frame();
                break;
                
            case SaveEvent:
                // Save complete FM2K state (400KB)
                save_fm2k_complete_state(event->data.save);
                break;
                
            case LoadEvent:
                // Restore FM2K state to exact frame
                restore_fm2k_complete_state(event->data.load);
                break;
        }
    }
    
    void save_fm2k_complete_state(GekkoGameEvent::Save& save_event) {
        FM2K_RollbackState state;
        
        // Read complete object pool (390KB)
        ReadProcessMemory(fm2k_process_, 
                         (void*)0x4701E0,  // g_object_pool
                         &state.objects, 
                         sizeof(state.objects), nullptr);
        
        // Read player state
        ReadProcessMemory(fm2k_process_, (void*)0x4DFC85, &state.p1_hp, 4, nullptr);
        ReadProcessMemory(fm2k_process_, (void*)0x4EDCC4, &state.p2_hp, 4, nullptr);
        
        // Read critical system state
        ReadProcessMemory(fm2k_process_, (void*)0x41FB1C, &state.random_seed, 4, nullptr);
        
        // Input history is already managed by GekkoNet
        
        // Copy to GekkoNet save event
        memcpy(save_event.state, &state, sizeof(state));
        *save_event.state_len = sizeof(state);
        *save_event.checksum = calculate_fm2k_checksum(&state);
    }
};
```

### Strategy 2: Hybrid FM2K Integration

**Phase 1: Extract FM2K State Management**
- Implement FM2K state save/restore using research data
- Create deterministic FM2K simulation layer
- Test state consistency with exact memory addresses

**Phase 2: Integrate GekkoNet with FM2K**
- Replace LilithPort hooks with GekkoNet-aware versions
- Maintain existing lobby system
- Add rollback support using FM2K knowledge

**Phase 3: Modernize Complete Client**
- Replace Windows Forms with SDL3 + Dear ImGui
- Add advanced rollback diagnostics
- Implement cross-platform support

## Technical Design Recommendations

### FM2K-Specific State Management

**Optimized for FM2K Architecture:**
```cpp
class FM2K_StateManager {
public:
    // Based on complete engine analysis
    struct FM2K_GameState {
        // Object System (largest component)
        struct GameObject {
            uint32_t type;           // +0x00
            uint32_t list_type;      // +0x04
            uint32_t x_position;     // +0x08 (fixed point)
            uint32_t y_position;     // +0x0C (fixed point)
            uint32_t state_flags;    // +0x10
            uint8_t  object_data[366]; // +0x14 to +0x17A
            uint16_t active_flag;    // +0x17E
        } objects[1023];
        
        // Player State (exact addresses from research)
        uint32_t p1_hp;             // 0x4DFC85
        uint32_t p2_hp;             // 0x4EDCC4
        uint32_t p1_max_hp;         // 0x4DFC91
        uint32_t p2_max_hp;         // 0x4EDCD0
        uint32_t p1_stage_x;        // 0x424E68
        uint32_t p1_stage_y;        // 0x424E6C
        uint32_t round_timer;       // 0x470060
        uint32_t game_timer;        // 0x470044
        
        // Critical System State
        uint32_t random_seed;       // 0x41FB1C
        uint32_t screen_x;          // 0x447F2C
        uint32_t screen_y;          // 0x447F30
        uint32_t frame_number;
        
        // Input System (leverage existing buffers)
        uint32_t input_buffer_index; // 0x447EE0
        // Note: Input history managed by GekkoNet
        
        uint32_t calculate_checksum() const;
    };
    
    // Ultra-fast state operations (< 1ms target)
    FM2K_GameState save_state_direct();
    void load_state_direct(const FM2K_GameState& state);
    
    // Leverage FM2K's existing frame skip for fast-forward
    void advance_frame_with_inputs(const uint32_t inputs[2]);
};
```

### Input System Integration

**Leverage FM2K's Existing 1024-Frame Buffering:**
```cpp
class FM2K_InputManager {
private:
    // FM2K input addresses (from research)
    static constexpr uint32_t P1_INPUT_ADDR = 0x4259C0;
    static constexpr uint32_t P2_INPUT_ADDR = 0x4259C4;
    static constexpr uint32_t P1_HISTORY_ADDR = 0x4280E0;
    static constexpr uint32_t P2_HISTORY_ADDR = 0x4290E0;
    static constexpr uint32_t INPUT_INDEX_ADDR = 0x447EE0;
    
public:
    // FM2K uses 11-bit input encoding
    struct FM2K_Input {
        uint32_t directions : 4;    // Up, Down, Left, Right
        uint32_t buttons : 7;       // 7 attack buttons
        uint32_t padding : 21;      // Unused
    };
    
    void inject_inputs_to_fm2k(const FM2K_Input inputs[2]) {
        // Direct memory injection at exact addresses
        WriteProcessMemory(process_, (void*)P1_INPUT_ADDR, &inputs[0], 4, nullptr);
        WriteProcessMemory(process_, (void*)P2_INPUT_ADDR, &inputs[1], 4, nullptr);
        
        // Update input history (FM2K does this automatically)
        // The 1024-frame circular buffer is managed by FM2K itself
    }
    
    FM2K_Input read_current_input(int player) {
        FM2K_Input input;
        uint32_t addr = (player == 0) ? P1_INPUT_ADDR : P2_INPUT_ADDR;
        ReadProcessMemory(process_, (void*)addr, &input, 4, nullptr);
        return input;
    }
};
```

### Advanced UI with Rollback Diagnostics

**Real-time Rollback Visualization:**
```cpp
void render_rollback_diagnostics() {
    ImGui::Begin("FM2K Rollback Diagnostics");
    
    // Network statistics
    GekkoNetworkStats stats;
    gekko_network_stats(session, 0, &stats);
    ImGui::Text("Ping: %ums (%.1f avg)", stats.last_ping, stats.avg_ping);
    ImGui::Text("Jitter: %ums", stats.jitter);
    
    // Rollback information
    float frames_ahead = gekko_frames_ahead(session);
    ImGui::Text("Frames ahead: %.1f", frames_ahead);
    ImGui::Text("Rollbacks/sec: %u", rollback_counter);
    
    // FM2K-specific information
    ImGui::Text("FM2K State size: %zu KB", sizeof(FM2K_GameState) / 1024);
    ImGui::Text("Object count: %u/1023", active_object_count);
    
    // Visual rollback indicator
    if (ImGui::CollapsingHeader("Frame Timeline")) {
        // Show last 60 frames with rollback indicators
        for (int i = 0; i < 60; i++) {
            bool was_rollback = frame_rollback_history[i];
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, 
                                 was_rollback ? IM_COL32(255,100,100,255) 
                                              : IM_COL32(100,255,100,255));
            ImGui::Button("##frame", ImVec2(8, 20));
            ImGui::PopStyleColor();
        }
    }
    
    ImGui::End();
}
```

## Implementation Roadmap (FM2K-Optimized)

### Phase 1: FM2K Integration Foundation (Month 1-2)
- [ ] Set up CMake build system with GekkoNet
- [ ] Implement FM2K process attachment and memory reading
- [ ] Create complete FM2K state serialization (400KB)
- [ ] Test state save/restore with exact memory addresses
- [ ] Implement FM2K input injection system

### Phase 2: Rollback Core Implementation (Month 3-4)
- [ ] Integrate GekkoNet with FM2K state manager
- [ ] Implement rollback-aware input processing
- [ ] Create deterministic FM2K frame advancement
- [ ] Test rollback with simple 2-player scenarios
- [ ] Performance optimization for 100 FPS target

### Phase 3: Modern Client Interface (Month 5-6)
- [ ] Create SDL3 + Dear ImGui interface
- [ ] Implement lobby system (maintain compatibility)
- [ ] Add rollback diagnostics and visualization
- [ ] Audio system integration
- [ ] Advanced networking features (spectator mode)

### Phase 4: Production Polish (Month 7-8)
- [ ] Cross-platform testing and support
- [ ] Network optimization and tuning
- [ ] Comprehensive testing with all FM2K games
- [ ] Performance benchmarking and optimization
- [ ] Documentation and user guides

## Performance Analysis (FM2K-Specific)

### Frame Budget at 100 FPS (10ms per frame)
```
Total available: 10ms per frame
- FM2K game logic: 6ms (controlled by game)
- State serialization: <1ms (optimized direct memory copy)
- GekkoNet processing: <1ms (rollback decisions)  
- Network I/O: <1ms (async processing)
- UI rendering: <1ms (Dear ImGui)
- Buffer remaining: ~1ms (safety margin)
```

### Memory Usage Optimization
```
FM2K State per frame: 400KB
Rollback buffer (120 frames): 48MB
GekkoNet internal buffers: ~8MB  
UI and application overhead: ~4MB
Total memory footprint: ~60MB (acceptable)
```

### State Serialization Performance Target
```cpp
// Target performance for FM2K state operations:
void save_fm2k_state_optimized() {
    // Direct memory copy - should be <0.5ms
    auto start = high_resolution_clock::now();
    
    // Bulk copy object pool (390KB) 
    memcpy(&state.objects, (void*)0x4701E0, sizeof(state.objects));
    
    // Copy critical game state (~100 bytes)
    read_game_state_bulk(&state.game_state);
    
    auto end = high_resolution_clock::now();
    // Target: <500 microseconds for full state save
}
```

## Resource Requirements

### Development Team (FM2K-Specialized)
- **Senior C++ Developer**: Lead architect with game hacking experience (6-8 months)
- **Rollback Specialist**: GekkoNet integration and optimization (4-6 months)  
- **FM2K Expert**: Game engine specialist familiar with Fighter Maker (2-4 months)
- **UI/UX Developer**: Modern interface design (3-4 months)
- **QA Engineer**: Comprehensive testing across FM2K games (2-3 months)

### Dependencies
```cmake
# CMakeLists.txt for FM2K-optimized build
find_package(SDL3 REQUIRED)
find_package(nlohmann_json REQUIRED)

# GekkoNet is already vendored
add_subdirectory(vendored/GekkoNet/GekkoLib)

target_link_libraries(lilithport_fm2k
    SDL3::SDL3
    nlohmann_json::nlohmann_json
    GekkoLib
    # Windows-specific for process memory access
    psapi.lib
)
```

## Alternative Frameworks Comparison (Updated)

| Framework | Pros | Cons | Best For |
|-----------|------|------|----------|
| **SDL3 + ImGui + GekkoNet** | Perfect for FM2K, rollback, cross-platform | Learning curve, manual UI | **RECOMMENDED: FM2K-optimized client** |
| **Qt6 + GekkoNet** | Professional UI, rapid development | Large overhead, licensing | Desktop-focused alternative |
| **Native Win32 + GekkoNet** | Minimal overhead, Windows-optimized | Windows-only, complex UI | Performance-critical scenarios |

## Conclusion

**Recommendation**: Proceed with **Strategy 1 (FM2K-Optimized Rollback Client)** using SDL3 + Dear ImGui + **GekkoNet**.

**Revolutionary Advantages**:
1. **? Perfect Game Knowledge**: Complete FM2K engine analysis provides exact state requirements
2. **? Zero Input Lag**: Rollback netcode eliminates delay-based lag entirely  
3. **? Optimized Performance**: Tailored to FM2K's 100 FPS, 1024-frame buffer architecture
4. **?? Bulletproof Stability**: Automatic desync detection with known state variables
5. **? Future-Proof**: Cross-platform modern codebase
6. **? Advanced Diagnostics**: Real-time rollback visualization and network stats

**Game-Changing Benefits of FM2K Knowledge**:
- **Exact State Size**: 400KB per frame is perfectly manageable
- **Existing Input Buffers**: 1024-frame buffering already in the engine
- **Deterministic Logic**: All RNG and state variables identified
- **Performance Predictable**: 100 FPS fixed timestep with known frame budget
- **Hook Points Identified**: Exact memory addresses for integration

**Next Steps**:
1. Set up development environment with GekkoNet + SDL3
2. Implement FM2K process attachment and memory reading
3. Create proof-of-concept state save/restore using exact addresses
4. Test basic rollback functionality with simple scenarios
5. Begin full client implementation

**This represents the most advanced fighting game netcode client possible - leveraging complete engine knowledge + modern rollback technology!**

---

*This document serves as the definitive technical roadmap for creating the ultimate FM2K rollback netplay client, combining complete engine knowledge with modern SDL3 + GekkoNet architecture.* 