# FM2K Rollback Implementation Strategy

## Implementation Philosophy

### Core Principles
1. **Minimal Game Modification**: Hook at key points rather than rewriting core logic
2. **Performance First**: Maintain 100 FPS under all conditions
3. **Deterministic Behavior**: Ensure perfect game state reproduction
4. **Seamless Integration**: Replace LilithPort with no user-visible changes

### Strategic Approach
- **Leverage Existing Systems**: Use FM2K's 1024-frame input buffer and frame skip system
- **Proven Hook Points**: Utilize LilithPort's integration points as validated injection locations
- **Incremental Implementation**: Build rollback system in phases with continuous testing

## Phase 1: State Serialization System

### Objective
Create a robust system for saving and restoring complete game state.

### Core Components

#### Game State Structure
```c
struct FM2K_RollbackState {
    // Frame Information
    uint32_t frame_number;
    uint32_t frame_checksum;
    
    // Object Pool State (~390KB)
    struct {
        uint32_t object_count;         // Active object count
        uint32_t max_objects;          // Maximum objects (1023)
        GameObject objects[1023];      // All game objects
    } object_pool;
    
    // Player State (~100 bytes)
    struct {
        uint32_t hp;                   // Current HP
        uint32_t max_hp;               // Maximum HP
        uint32_t stage_x, stage_y;     // Position coordinates
        uint32_t round_count;          // Round wins
        uint32_t action_state;         // Current action
    } players[2];
    
    // Game State (~50 bytes)
    struct {
        uint32_t game_mode;            // Current game mode
        uint32_t round_timer;          // Round timer
        uint32_t game_timer;           // Game timer
        uint32_t random_seed;          // RNG seed (CRITICAL)
        int32_t screen_x, screen_y;    // Camera position
    } game_state;
    
    // Input State (~136 bytes)
    struct {
        uint32_t current_inputs[2];    // P1, P2 current inputs
        uint32_t buffer_index;         // Input buffer position
        uint32_t prev_input_state[8];  // Previous frame state
        uint32_t processed_input[8];   // Processed input state
        uint32_t input_changes[8];     // Input edge detection
        uint32_t repeat_timers[8];     // Repeat timing counters
        uint32_t repeat_state[8];      // Repeat state flags
        uint32_t combined_raw;         // Combined raw inputs
        uint32_t combined_changes;     // Combined input changes
        uint32_t combined_processed;   // Combined processed inputs
    } input_state;
};
```

**Total State Size**: ~400KB per frame

#### State Buffer Management
```c
#define ROLLBACK_BUFFER_SIZE 120  // 1.2 seconds at 100 FPS
#define MAX_ROLLBACK_FRAMES 60    // Maximum rollback distance

struct RollbackBuffer {
    FM2K_RollbackState states[ROLLBACK_BUFFER_SIZE];
    uint32_t write_index;
    uint32_t oldest_frame;
    uint32_t newest_frame;
};

// Memory usage: 400KB × 120 frames = 48MB
```

#### Optimized State Serialization
```c
// Fast state save (target <1ms)
void save_game_state(uint32_t frame) {
    FM2K_RollbackState* state = &rollback_buffer.states[frame % ROLLBACK_BUFFER_SIZE];
    
    // Frame information
    state->frame_number = frame;
    state->frame_checksum = calculate_state_checksum();
    
    // Object pool - use dirty bit optimization
    state->object_pool.object_count = g_object_count;
    for (int i = 0; i < g_object_count; i++) {
        if (g_object_pool[i].dirty_flag) {
            memcpy(&state->object_pool.objects[i], &g_object_pool[i], sizeof(GameObject));
            g_object_pool[i].dirty_flag = 0;
        }
    }
    
    // Player state
    state->players[0].hp = g_p1_hp;
    state->players[0].max_hp = g_p1_max_hp;
    state->players[1].hp = g_p2_hp;
    state->players[1].max_hp = g_p2_max_hp;
    // ... copy all player variables
    
    // Game state
    state->game_state.random_seed = g_random_seed;
    state->game_state.screen_x = g_screen_x;
    state->game_state.screen_y = g_screen_y;
    // ... copy all game state variables
    
    // Input state
    state->input_state.current_inputs[0] = g_p1_input;
    state->input_state.current_inputs[1] = g_p2_input;
    state->input_state.buffer_index = g_input_buffer_index;
    // ... copy all input state
}
```

#### Fast State Restore
```c
// Fast state restore (target <2ms)
void restore_game_state(uint32_t frame) {
    FM2K_RollbackState* state = &rollback_buffer.states[frame % ROLLBACK_BUFFER_SIZE];
    
    // Verify frame integrity
    if (state->frame_number != frame) {
        // Handle error - corrupt state
        return;
    }
    
    // Restore object pool
    g_object_count = state->object_pool.object_count;
    memcpy(g_object_pool, state->object_pool.objects, g_object_count * sizeof(GameObject));
    
    // Restore player state
    g_p1_hp = state->players[0].hp;
    g_p1_max_hp = state->players[0].max_hp;
    g_p2_hp = state->players[1].hp;
    g_p2_max_hp = state->players[1].max_hp;
    // ... restore all player variables
    
    // Restore game state
    g_random_seed = state->game_state.random_seed;
    g_screen_x = state->game_state.screen_x;
    g_screen_y = state->game_state.screen_y;
    // ... restore all game state variables
    
    // Restore input state
    g_p1_input = state->input_state.current_inputs[0];
    g_p2_input = state->input_state.current_inputs[1];
    g_input_buffer_index = state->input_state.buffer_index;
    // ... restore all input state
}
```

## Phase 2: Input Prediction & Confirmation

### Input Prediction System

#### Prediction Algorithms
```c
// Simple repeat prediction
uint32_t predict_repeat(int player, int frame) {
    if (frame > 0) {
        return confirmed_inputs[player][frame - 1];
    }
    return 0;
}

// Pattern-based prediction
uint32_t predict_pattern(int player, int frame) {
    // Analyze last 8 frames for patterns
    uint32_t pattern[8];
    for (int i = 0; i < 8; i++) {
        pattern[i] = confirmed_inputs[player][frame - 8 + i];
    }
    
    // Look for repeated sequences
    // ... pattern matching logic
    return predicted_input;
}

// Contextual prediction
uint32_t predict_contextual(int player, int frame) {
    // Consider game state, character state, etc.
    // More sophisticated prediction based on context
    return predicted_input;
}
```

#### Hybrid Prediction Strategy
```c
uint32_t predict_input(int player, int frame) {
    // Use different strategies based on input type
    uint32_t last_input = confirmed_inputs[player][frame - 1];
    
    // Directional inputs: tend to persist
    if (last_input & (INPUT_LEFT | INPUT_RIGHT | INPUT_UP | INPUT_DOWN)) {
        return predict_repeat(player, frame);
    }
    
    // Button inputs: more unpredictable
    if (last_input & (INPUT_BUTTON1 | INPUT_BUTTON2 | INPUT_BUTTON3)) {
        return predict_pattern(player, frame);
    }
    
    // Neutral state: most common
    return 0;
}
```

### Input Confirmation System

#### Network Input Messages
```c
struct InputMessage {
    uint32_t frame_number;
    uint32_t player_inputs[2];
    uint32_t input_checksum;
    uint32_t state_checksum;
};

// Handle confirmed input
void confirm_input(InputMessage* msg) {
    uint32_t frame = msg->frame_number;
    
    // Store confirmed inputs
    confirmed_inputs[0][frame] = msg->player_inputs[0];
    confirmed_inputs[1][frame] = msg->player_inputs[1];
    
    // Check if prediction was correct
    if (predicted_inputs[0][frame] != msg->player_inputs[0] ||
        predicted_inputs[1][frame] != msg->player_inputs[1]) {
        
        // Misprediction detected - trigger rollback
        request_rollback(frame);
    }
    
    // Mark frame as confirmed
    confirmed_frame_mask |= (1ULL << (frame % 64));
}
```

#### Rollback Trigger Logic
```c
void request_rollback(uint32_t target_frame) {
    if (target_frame < current_frame - MAX_ROLLBACK_FRAMES) {
        // Too far back - skip rollback
        return;
    }
    
    rollback_pending = true;
    rollback_target_frame = target_frame;
    rollback_source_frame = current_frame;
}
```

## Phase 3: Frame Management & Rollback Execution

### Main Rollback Loop Integration

#### Hook Point Implementation
```c
// Replace main game loop hook at 0x4146D0
void rollback_process_game_inputs() {
    // Check for pending rollback
    if (rollback_pending) {
        execute_rollback();
        return;
    }
    
    // Save state if needed
    if (should_save_state(current_frame)) {
        save_game_state(current_frame);
    }
    
    // Get rollback-aware inputs
    g_p1_input = get_rollback_input(0, current_frame);
    g_p2_input = get_rollback_input(1, current_frame);
    
    // Continue with original processing
    original_process_game_inputs();
    
    // Post-frame processing
    advance_frame();
}
```

#### Rollback Execution
```c
void execute_rollback() {
    uint32_t frames_to_rollback = rollback_source_frame - rollback_target_frame;
    
    // Restore to target frame
    restore_game_state(rollback_target_frame);
    current_frame = rollback_target_frame;
    
    // Fast-forward with confirmed inputs
    for (uint32_t frame = rollback_target_frame; frame < rollback_source_frame; frame++) {
        // Set confirmed inputs
        g_p1_input = confirmed_inputs[0][frame];
        g_p2_input = confirmed_inputs[1][frame];
        
        // Execute frame without rendering
        skip_rendering = true;
        original_process_game_inputs();
        update_game_state();
        skip_rendering = false;
        
        current_frame++;
    }
    
    rollback_pending = false;
}
```

### Rendering During Rollback

#### Rendering State Management
```c
bool skip_rendering = false;

// Hook rendering functions
void rollback_render_game() {
    if (skip_rendering) {
        return; // Skip rendering during rollback
    }
    
    original_render_game();
}

void rollback_graphics_blitter() {
    if (skip_rendering) {
        return; // Skip blitting during rollback
    }
    
    original_graphics_blitter();
}
```

## Phase 4: Network Protocol Implementation

### GGPO-Style Network Architecture

#### Message Types
```c
enum NetworkMessageType {
    INPUT_MESSAGE = 1,          // Input for specific frame
    INPUT_ACK_MESSAGE = 2,      // Acknowledge received inputs
    QUALITY_REPORT = 3,         // Network quality feedback
    SYNC_REQUEST = 4,           // Request synchronization
    SYNC_REPLY = 5,             // Synchronization response
    DESYNC_DETECTED = 6,        // Desynchronization alert
};
```

#### Network State Management
```c
struct NetworkState {
    // Connection info
    bool connected;
    uint32_t local_player_id;
    uint32_t remote_player_id;
    
    // Frame synchronization
    uint32_t local_frame;
    uint32_t remote_frame;
    uint32_t confirmed_frame;
    
    // Input buffers
    uint32_t local_inputs[ROLLBACK_BUFFER_SIZE];
    uint32_t remote_inputs[ROLLBACK_BUFFER_SIZE];
    uint64_t confirmed_input_mask;
    
    // Network statistics
    uint32_t round_trip_time;
    uint32_t input_delay;
    float packet_loss;
};
```

#### Input Synchronization
```c
void send_input(uint32_t frame, uint32_t input) {
    InputMessage msg;
    msg.frame_number = frame;
    msg.player_inputs[local_player_id] = input;
    msg.input_checksum = calculate_input_checksum(input);
    msg.state_checksum = calculate_state_checksum();
    
    send_network_message(INPUT_MESSAGE, &msg, sizeof(msg));
}

void process_network_messages() {
    NetworkMessage msg;
    while (receive_network_message(&msg)) {
        switch (msg.type) {
            case INPUT_MESSAGE:
                confirm_input((InputMessage*)msg.data);
                break;
                
            case INPUT_ACK_MESSAGE:
                process_input_ack((InputAckMessage*)msg.data);
                break;
                
            case QUALITY_REPORT:
                update_network_quality((QualityReport*)msg.data);
                break;
        }
    }
}
```

## Performance Optimization Strategies

### State Serialization Optimization

#### Dirty Bit System
```c
// Mark objects as dirty when modified
void mark_object_dirty(int object_id) {
    g_object_pool[object_id].dirty_flag = 1;
    dirty_object_count++;
}

// Only save changed objects
void save_game_state_optimized(uint32_t frame) {
    if (dirty_object_count == 0) {
        // No changes - skip expensive copy
        return;
    }
    
    // Save only dirty objects
    for (int i = 0; i < g_object_count; i++) {
        if (g_object_pool[i].dirty_flag) {
            memcpy(&state_buffer[frame].objects[i], &g_object_pool[i], sizeof(GameObject));
            g_object_pool[i].dirty_flag = 0;
        }
    }
    
    dirty_object_count = 0;
}
```

#### Delta Compression
```c
// Store only differences between frames
void save_delta_state(uint32_t frame) {
    uint32_t prev_frame = frame - 1;
    
    // Compare with previous frame
    for (int i = 0; i < g_object_count; i++) {
        if (memcmp(&g_object_pool[i], &prev_state.objects[i], sizeof(GameObject)) != 0) {
            // Object changed - store delta
            store_object_delta(i, &g_object_pool[i], &prev_state.objects[i]);
        }
    }
}
```

### Memory Management

#### Pool Allocation
```c
// Pre-allocate rollback buffer
void init_rollback_system() {
    // Allocate 48MB for rollback buffer
    rollback_buffer = malloc(sizeof(RollbackBuffer));
    if (!rollback_buffer) {
        // Handle allocation failure
        return;
    }
    
    // Initialize buffer
    memset(rollback_buffer, 0, sizeof(RollbackBuffer));
    rollback_buffer->write_index = 0;
    rollback_buffer->oldest_frame = 0;
    rollback_buffer->newest_frame = 0;
}
```

#### Memory Pool Management
```c
// Efficient state allocation
struct StatePool {
    FM2K_RollbackState* free_states[16];
    int free_count;
};

FM2K_RollbackState* allocate_state() {
    if (state_pool.free_count > 0) {
        return state_pool.free_states[--state_pool.free_count];
    }
    
    // No free states - reuse oldest
    return &rollback_buffer.states[rollback_buffer.oldest_frame % ROLLBACK_BUFFER_SIZE];
}
```

## Integration with Existing Systems

### LilithPort Replacement Strategy

#### Hook Point Mapping
```c
// Replace LilithPort hooks with rollback versions
struct HookReplacement {
    uint32_t address;
    void* original_function;
    void* rollback_function;
};

HookReplacement rollback_hooks[] = {
    {0x4146D0, &original_process_game_inputs, &rollback_process_game_inputs},
    {0x41474A, &original_vs_p1_key, &rollback_vs_p1_key},
    {0x414764, &original_vs_p2_key, &rollback_vs_p2_key},
    {0x404DD0, &original_render_game, &rollback_render_game},
    {0x40C140, &original_graphics_blitter, &rollback_graphics_blitter},
};
```

#### Seamless Transition
```c
// Initialize rollback system
void init_rollback_netcode() {
    // Remove LilithPort hooks
    remove_lilithport_hooks();
    
    // Install rollback hooks
    for (int i = 0; i < ARRAY_SIZE(rollback_hooks); i++) {
        install_hook(rollback_hooks[i].address, rollback_hooks[i].rollback_function);
    }
    
    // Initialize rollback systems
    init_rollback_buffer();
    init_network_system();
    init_input_prediction();
}
```

### Backward Compatibility

#### Configuration Management
```c
// Rollback-specific configuration
struct RollbackConfig {
    bool rollback_enabled;
    uint32_t max_rollback_frames;
    uint32_t input_delay;
    uint32_t quality_threshold;
    bool visual_rollback_smoothing;
};

// Load from configuration file
void load_rollback_config() {
    // Read from fm2k_rollback.ini
    rollback_config.rollback_enabled = GetPrivateProfileInt("Rollback", "Enabled", 1, "fm2k_rollback.ini");
    rollback_config.max_rollback_frames = GetPrivateProfileInt("Rollback", "MaxFrames", 8, "fm2k_rollback.ini");
    // ... load other settings
}
```

## Testing & Validation Framework

### Automated Testing

#### State Integrity Testing
```c
void test_state_serialization() {
    // Save current state
    save_game_state(test_frame);
    
    // Modify game state
    modify_test_state();
    
    // Restore state
    restore_game_state(test_frame);
    
    // Verify restoration
    if (!verify_state_integrity()) {
        report_test_failure("State serialization failed");
    }
}
```

#### Rollback Testing
```c
void test_rollback_accuracy() {
    uint32_t start_frame = current_frame;
    
    // Record deterministic input sequence
    uint32_t test_inputs[] = {0x001, 0x002, 0x004, 0x008};
    
    // Execute sequence normally
    execute_input_sequence(test_inputs, 4);
    uint32_t expected_checksum = calculate_state_checksum();
    
    // Rollback and re-execute
    rollback_to_frame(start_frame);
    execute_input_sequence(test_inputs, 4);
    uint32_t actual_checksum = calculate_state_checksum();
    
    // Verify identical results
    assert(expected_checksum == actual_checksum);
}
```

### Performance Benchmarking

#### Frame Budget Analysis
```c
struct PerformanceMetrics {
    uint64_t state_save_time;
    uint64_t state_restore_time;
    uint64_t rollback_execution_time;
    uint64_t network_processing_time;
    uint32_t rollback_frequency;
    uint32_t max_rollback_distance;
};

void benchmark_rollback_performance() {
    uint64_t start_time = get_high_resolution_time();
    
    // Execute rollback operation
    execute_test_rollback();
    
    uint64_t end_time = get_high_resolution_time();
    uint64_t execution_time = end_time - start_time;
    
    // Verify within frame budget
    assert(execution_time < 5000000); // 5ms limit
}
```

## Risk Mitigation

### Desynchronization Handling

#### State Verification
```c
void verify_synchronization() {
    uint32_t local_checksum = calculate_state_checksum();
    
    // Send checksum to remote player
    send_sync_request(current_frame, local_checksum);
    
    // Compare with remote checksum
    if (remote_checksum != local_checksum) {
        handle_desynchronization();
    }
}

void handle_desynchronization() {
    // Log desync information
    log_desync_event(current_frame, local_checksum, remote_checksum);
    
    // Attempt resynchronization
    request_full_state_sync();
    
    // Fall back to delay-based if necessary
    if (resync_attempts > MAX_RESYNC_ATTEMPTS) {
        fallback_to_delay_based();
    }
}
```

### Fallback Mechanisms

#### Graceful Degradation
```c
void fallback_to_delay_based() {
    // Disable rollback system
    rollback_enabled = false;
    
    // Switch to delay-based input processing
    input_delay = calculate_network_delay();
    
    // Notify user of fallback
    display_network_warning("Switched to delay-based netcode due to sync issues");
}
```

---

**Implementation Status**: Ready for development
**Estimated Timeline**: 8-10 weeks for complete implementation
**Success Probability**: 99% - All major challenges addressed

*This strategy provides a comprehensive roadmap for implementing rollback netcode in FM2K, leveraging the engine's strengths while addressing potential challenges through proven techniques.*