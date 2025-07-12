# Launcher Testing Infrastructure Design

## Overview

This document outlines the design for enhanced debug tools and multi-client testing infrastructure in the FM2K launcher. The goal is to provide comprehensive testing capabilities for rollback netcode development and validation.

## Current State

### Existing Debug Tools
- Save state management (manual save/load, auto-save configuration)
- Save state profile selection (MINIMAL/STANDARD/COMPLETE)
- Performance monitoring and statistics
- Slot-based save system with status display

### Current Limitations
- No multi-client testing capabilities
- No network simulation or debugging tools
- No automated testing frameworks
- Limited GekkoNet integration testing

## Proposed Enhancements

### 1. Multi-Client Testing System

#### Core Functionality
- **Local Dual Client Launch**: Single button to launch two FM2K instances locally
- **Automatic IP Configuration**: Auto-configure local networking (127.0.0.1:7000 and 127.0.0.1:7001)
- **Process Management**: Monitor and control both client processes
- **Synchronized Debugging**: Debug both clients simultaneously

#### Implementation Plan
```cpp
// New callback functions needed
std::function<bool(const std::string& game_path)> on_launch_local_client1;  // Launch first client as host
std::function<bool(const std::string& game_path)> on_launch_local_client2;  // Launch second client as guest  
std::function<bool()> on_terminate_all_clients;  // Kill all launched clients
std::function<bool(uint32_t, bool)> on_client_process_status;  // (client_id, alive) - Process monitoring
```

#### UI Design
```
┌─ Multi-Client Testing ─────────────────────────┐
│ ┌─ Local Testing ─────────────────────────────┐ │
│ │ Selected Game: [WonderfulWorld_ver_0946.exe]│ │
│ │ ┌───────────────────────────────────────────┐ │ │
│ │ │ [Launch Dual Clients]  [Stop All Clients]│ │ │
│ │ └───────────────────────────────────────────┘ │ │
│ │ Client 1: ● Online (Host - 127.0.0.1:7000) │ │
│ │ Client 2: ● Online (Guest - 127.0.0.1:7001)│ │
│ │ Network Status: ✓ Connected, 2ms latency   │ │
│ └─────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────┘
```

### 2. Network Simulation & Debugging Tools

#### Latency Simulation
- **Configurable Latency**: Simulate network delays (0-500ms)
- **Jitter Simulation**: Add random variance to latency
- **Packet Loss**: Simulate dropped packets (0-10%)

#### Real-time Monitoring
- **Connection Quality**: Ping, packet loss, bandwidth usage
- **Rollback Statistics**: Rollback frequency, frame advantage
- **Input Delay**: Dynamic input delay adjustment monitoring
- **Desync Detection**: Real-time desync detection and logging

#### Implementation
```cpp
// Network simulation callbacks
std::function<bool(uint32_t)> on_set_simulated_latency;     // Set artificial latency in ms
std::function<bool(float)> on_set_packet_loss_rate;        // Set packet loss percentage (0.0-1.0)
std::function<bool(uint32_t)> on_set_jitter_variance;      // Set latency jitter variance in ms

// Monitoring callbacks  
std::function<bool(NetworkStats&)> on_get_network_stats;   // Get real-time network statistics
std::function<bool(RollbackStats&)> on_get_rollback_stats; // Get rollback performance data
```

#### UI Design
```
┌─ Network Simulation ───────────────────────────┐
│ Latency:     [150] ms  ┌─ Presets ─────────┐   │
│ Jitter:      [20]  ms  │ [Perfect]         │   │  
│ Packet Loss: [2.5] %   │ [Good WiFi]       │   │
│                        │ [Poor Connection] │   │
│ ┌─ Live Stats ─────────┐└───────────────────┘   │
│ │ Ping: 152ms         │                        │
│ │ Rollbacks: 3/sec    │ [Apply Settings]       │
│ │ Frame Advantage: +1 │                        │
│ └─────────────────────┘                        │
└─────────────────────────────────────────────────┘
```

### 3. GekkoNet Integration & Session Management

#### Session Control
- **Session Lifecycle**: Create, start, stop, destroy GekkoNet sessions
- **Player Management**: Add/remove local and remote players
- **State Synchronization**: Monitor and control state sync
- **Input Prediction**: View and adjust input prediction settings

#### Session Monitoring
- **Player Status**: Connection state, input delay, rollback frequency
- **Frame Synchronization**: Frame count, confirmed frames, speculative frames
- **Performance Metrics**: Frame timing, rollback cost, network overhead

#### Implementation
```cpp
// GekkoNet session callbacks
std::function<bool(GekkoConfig&)> on_create_gekko_session;        // Create session with config
std::function<bool()> on_start_gekko_session;                     // Start session
std::function<bool()> on_stop_gekko_session;                      // Stop session  
std::function<bool(int, PlayerType)> on_add_gekko_player;         // Add player to session
std::function<bool(GekkoSessionStats&)> on_get_gekko_stats;       // Get session statistics
```

### 4. Automated Testing Framework

#### Test Scenarios
- **Connection Tests**: Auto-test different network conditions
- **Rollback Tests**: Stress test rollback with forced desyncs
- **Performance Tests**: Measure rollback overhead and timing
- **Compatibility Tests**: Test different save state profiles

#### Test Automation
- **Scripted Tests**: Run predefined test sequences
- **Regression Testing**: Automated testing of core functionality  
- **Performance Benchmarking**: Measure and compare performance metrics
- **Report Generation**: Generate test reports and logs

## Implementation Phases

### Phase 1: Basic Multi-Client Support
1. **Process Management**: Implement dual client launching and monitoring
2. **Basic UI**: Add multi-client testing panel to launcher
3. **Network Setup**: Auto-configure local networking for dual clients
4. **Status Monitoring**: Display client connection status

### Phase 2: Network Simulation  
1. **Simulation Engine**: Implement latency, jitter, and packet loss simulation
2. **Monitoring System**: Real-time network and performance statistics
3. **UI Controls**: Network simulation control panel
4. **Presets**: Common network condition presets

### Phase 3: GekkoNet Integration
1. **Session Management**: Full GekkoNet session lifecycle control
2. **Player Management**: Dynamic player add/remove functionality
3. **Advanced Monitoring**: Detailed rollback and sync statistics
4. **Performance Analysis**: Frame timing and rollback cost analysis

### Phase 4: Testing Automation
1. **Test Framework**: Scripted test execution system
2. **Scenario Library**: Common test scenarios and stress tests
3. **Reporting**: Automated test reporting and analysis
4. **Regression Suite**: Continuous testing integration

## Data Structures

### Network Statistics
```cpp
struct NetworkStats {
    uint32_t ping_ms;              // Current ping in milliseconds
    float packet_loss_rate;        // Packet loss rate (0.0-1.0)
    uint32_t bytes_sent;           // Total bytes sent
    uint32_t bytes_received;       // Total bytes received
    uint32_t packets_sent;         // Total packets sent
    uint32_t packets_received;     // Total packets received
    uint32_t connection_quality;   // Connection quality (0-100)
};
```

### Rollback Statistics  
```cpp
struct RollbackStats {
    uint32_t rollbacks_per_second; // Current rollback frequency
    uint32_t max_rollback_frames;  // Maximum rollback distance
    uint32_t avg_rollback_frames;  // Average rollback distance
    float frame_advantage;         // Current frame advantage
    uint32_t input_delay_frames;   // Current input delay
    uint32_t confirmed_frames;     // Number of confirmed frames
    uint32_t speculative_frames;   // Number of speculative frames
};
```

### GekkoNet Session Statistics
```cpp
struct GekkoSessionStats {
    bool session_active;           // Session is running
    uint32_t player_count;         // Number of connected players
    uint32_t frame_number;         // Current frame number
    uint32_t confirmed_frame;      // Last confirmed frame
    float frames_ahead;            // Dynamic input delay
    uint32_t state_size_bytes;     // Current state size
    uint32_t checksum_mismatches;  // Number of checksum failures
};
```

## UI Layout Enhancement

The debug tools will be reorganized into tabbed sections for better organization:

```
┌─ Debug & Testing Tools ────────────────────────┐
│ [Save States] [Multi-Client] [Network] [Stats] │
│ ┌─────────────────────────────────────────────┐ │
│ │          (Current tab content)              │ │
│ │                                             │ │
│ │                                             │ │
│ └─────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────┘
```

## Benefits

### For Development
- **Rapid Testing**: Quick setup of test scenarios
- **Real-time Debugging**: Monitor both clients simultaneously  
- **Network Simulation**: Test various network conditions locally
- **Performance Analysis**: Detailed rollback and networking metrics

### For Quality Assurance
- **Automated Testing**: Reduced manual testing effort
- **Regression Detection**: Early detection of performance regressions
- **Comprehensive Coverage**: Test multiple scenarios and edge cases
- **Reproducible Tests**: Consistent testing conditions

### For Future Development
- **Foundation for Production**: Testing infrastructure becomes basis for matchmaking
- **Performance Optimization**: Detailed metrics guide optimization efforts
- **User Testing**: Tools can be exposed for beta testing and feedback
- **Documentation**: Test scenarios serve as usage examples

## Next Steps

1. **Implement Phase 1**: Basic multi-client support with dual launching
2. **Create UI mockups**: Design the enhanced debug tool interface
3. **Plan process management**: Design client lifecycle management
4. **Network configuration**: Auto-setup for local dual client testing

This infrastructure will provide a solid foundation for rollback netcode development and testing while building toward production-ready networking capabilities.