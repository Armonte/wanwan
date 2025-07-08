# FM2K Rollback Test Plan

## Pre-Test Setup

### Build Verification
1. Build FM2KHook DLL
   - [ ] Verify MinHook dependency linked
   - [ ] Verify SDL3 dependency linked
   - [ ] Check DLL exports
   - [ ] Verify debug symbols generated

2. Build Launcher
   - [ ] Verify SDL3 dependency linked
   - [ ] Verify ImGui integration
   - [ ] Check GekkoNet integration

### Environment Setup
1. Game Installation
   - [ ] Clean FM2K installation
   - [ ] Verify game version matches addresses
   - [ ] Test base game functionality

2. Test Tools
   - [ ] Setup logging capture
   - [ ] Prepare test save states
   - [ ] Setup memory viewer for verification

## Test Sequence

### Phase 1: Launch and Hook Installation
1. Process Creation
   - [ ] Game launches suspended
   - [ ] Process memory accessible
   - [ ] DLL injection succeeds

2. Hook Installation
   - [ ] MinHook initialization successful
   - [ ] All hooks installed (check logs)
   - [ ] State manager initialized
   - [ ] IPC buffer created

3. Basic Functionality
   - [ ] Game resumes normally
   - [ ] Input processing works
   - [ ] Frame advancement normal
   - [ ] No performance impact

### Phase 2: State Management

1. Basic State Operations
   ```cpp
   // Test sequence
   GameState state;
   uint32_t checksum;
   SaveState(&state, &checksum);  // Should succeed
   LoadState(&state);             // Should succeed and match checksum
   ```
   - [ ] Save state succeeds
   - [ ] Load state succeeds
   - [ ] Checksum matches
   - [ ] Game continues normally

2. Memory Region Tests
   - Hit Judge Tables
     - [ ] Read succeeds
     - [ ] Write succeeds
     - [ ] Content verified
   - Round State
     - [ ] Timer tracked
     - [ ] State changes captured
     - [ ] Mode preserved

3. Visual State Tests
   - [ ] Capture base state
   - [ ] Trigger effect (e.g., hit spark)
   - [ ] Save during effect
   - [ ] Load state
   - [ ] Effect restored correctly
   - [ ] Colors match
   - [ ] Timers accurate

### Phase 3: Rollback Integration

1. Basic Rollback
   ```cpp
   // Test sequence
   SaveState(t0);    // Initial state
   AdvanceFrame();   // t1 with local input
   SaveState(t1);    // Save t1
   LoadState(t0);    // Rollback to t0
   AdvanceFrame();   // Replay with new input
   VerifyState(t1);  // Should match if deterministic
   ```
   - [ ] Save at t0 works
   - [ ] Advance frame works
   - [ ] Rollback to t0 works
   - [ ] State matches after replay

2. Visual Effects During Rollback
   - [ ] Effect starts before rollback
   - [ ] Rollback during effect
   - [ ] Effect restored correctly
   - [ ] No visual artifacts

3. Edge Cases
   - [ ] Rollback during hit detection
   - [ ] Multiple effects active
   - [ ] State buffer boundaries
   - [ ] Maximum rollback distance

### Phase 4: Network Integration

1. Local Testing
   - [ ] GekkoNet callbacks fire
   - [ ] State save/load through callbacks
   - [ ] Input injection works

2. Network Testing
   - [ ] Connect two instances
   - [ ] Synchronize initial state
   - [ ] Input delay configured
   - [ ] Rollbacks occur properly

## Error Cases to Test

1. Memory Access
   - [ ] Invalid addresses
   - [ ] Partial reads/writes
   - [ ] Access violations handled

2. State Management
   - [ ] Invalid state data
   - [ ] Checksum mismatches
   - [ ] Buffer overflows prevented

3. Network Conditions
   - [ ] Packet loss handling
   - [ ] Input delay adjustment
   - [ ] Disconnection recovery

## Performance Metrics

1. Timing Requirements
   - [ ] Frame time under 10ms (100 FPS)
   - [ ] State save under 1ms
   - [ ] State load under 1ms
   - [ ] Rollback under 2ms

2. Memory Usage
   - [ ] State buffer size stable
   - [ ] No memory leaks
   - [ ] Peak usage acceptable

## Test Completion Criteria

- All critical tests pass
- No memory leaks
- Stable performance
- Visual consistency maintained
- Network play functional

## Issue Tracking

| ID | Description | Status | Priority |
|----|-------------|--------|----------|
| 1  | Initial test setup | Pending | High |
| 2  | Basic functionality | Pending | High |
| 3  | State management | Pending | High |
| 4  | Visual effects | Pending | High |
| 5  | Network integration | Pending | Medium | 