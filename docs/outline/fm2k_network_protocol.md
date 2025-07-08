# FM2K Network Protocol Design

## Overview

GGPO-style networking protocol designed specifically for FM2K's 100 FPS architecture, leveraging the existing input buffer system and deterministic engine for optimal rollback netcode performance.

## Network Architecture Philosophy

### Core Principles
1. **Frame-Synchronous Protocol**: Align with FM2K's 100 FPS fixed timestep
2. **Input Prediction & Confirmation**: Minimize input latency while ensuring accuracy
3. **Lightweight Messages**: Optimize for bandwidth and latency
4. **Graceful Degradation**: Fall back to delay-based when necessary
5. **Deterministic Validation**: Continuous state verification

### GGPO-Style Approach
- **Optimistic Execution**: Execute frames with predicted inputs
- **Rollback on Misprediction**: Roll back when predictions are wrong
- **Input Confirmation**: Confirm inputs as they arrive from network
- **Frame Advantage Management**: Handle network delay and jitter

## Message Types & Structures

### Core Message Types
```c
enum NetworkMessageType {
    INPUT_MESSAGE = 0x01,          // Input for specific frame
    INPUT_ACK_MESSAGE = 0x02,      // Acknowledge received inputs
    QUALITY_REPORT = 0x03,         // Network quality feedback
    SYNC_REQUEST = 0x04,           // Request synchronization
    SYNC_REPLY = 0x05,             // Synchronization response
    DESYNC_DETECTED = 0x06,        // Desynchronization alert
    KEEP_ALIVE = 0x07,             // Connection heartbeat
    DISCONNECT = 0x08,             // Clean disconnection
};
```

### Message Header
```c
struct NetworkMessageHeader {
    uint8_t message_type;          // Message type from enum
    uint8_t protocol_version;      // Protocol version (current: 1)
    uint16_t sequence_number;      // Message sequence for ordering
    uint32_t timestamp;            // Local timestamp (timeGetTime())
    uint32_t frame_number;         // Game frame number
    uint16_t payload_size;         // Size of message payload
    uint16_t checksum;             // Message integrity checksum
};
```

## Input Synchronization Protocol

### Input Message Structure
```c
struct InputMessage {
    NetworkMessageHeader header;
    
    uint32_t frame_number;         // Frame this input applies to
    uint32_t input_data[2];        // P1, P2 inputs (11-bit each)
    uint32_t input_checksum;       // Input data verification
    uint32_t state_checksum;       // Game state verification
    uint8_t prediction_distance;   // How far ahead this input is predicted
    uint8_t input_delay;           // Current input delay setting
    uint16_t reserved;             // Future expansion
};
```

### Input Acknowledgment
```c
struct InputAckMessage {
    NetworkMessageHeader header;
    
    uint32_t acknowledged_frame;   // Frame being acknowledged
    uint32_t received_checksum;    // Checksum of received input
    uint32_t local_state_checksum; // Local state at that frame
    uint8_t rollback_distance;     // How far back we had to rollback
    uint8_t reserved[3];           // Padding
};
```

## Connection Management

### Connection Establishment
```c
struct ConnectionRequest {
    NetworkMessageHeader header;
    
    char player_name[32];          // Player identifier
    uint32_t game_version;         // FM2K version
    uint32_t protocol_version;     // Network protocol version
    uint32_t features_supported;   // Supported features bitmask
    uint32_t preferred_delay;      // Preferred input delay
};

struct ConnectionResponse {
    NetworkMessageHeader header;
    
    uint8_t connection_accepted;   // 1=accepted, 0=rejected
    uint8_t assigned_player_id;    // Player ID (0 or 1)
    uint8_t negotiated_delay;      // Negotiated input delay
    uint8_t reserved;              // Padding
    uint32_t session_id;           // Unique session identifier
};
```

### Network Quality Monitoring
```c
struct QualityReport {
    NetworkMessageHeader header;
    
    uint32_t round_trip_time;      // Current RTT in milliseconds
    uint32_t packet_loss_rate;     // Packet loss (fixed point: /1000)
    uint32_t jitter;               // Network jitter in milliseconds
    uint32_t bandwidth_used;       // Current bandwidth usage
    uint8_t recommended_delay;     // Recommended input delay
    uint8_t quality_score;         // Overall connection quality (0-100)
    uint16_t rollback_frequency;   // Rollbacks per second
};
```

## Frame Synchronization

### Frame Advantage System
```c
struct FrameAdvantage {
    int32_t local_frame;           // Our current frame
    int32_t remote_frame;          // Remote player's last known frame
    int32_t confirmed_frame;       // Last mutually confirmed frame
    int32_t prediction_frame;      // Furthest predicted frame
    
    uint8_t input_delay;           // Current input delay
    uint8_t max_prediction;        // Maximum prediction distance
    uint8_t rollback_limit;        // Maximum rollback distance
    uint8_t sync_state;            // Synchronization state
};
```

### Synchronization States
```c
enum SyncState {
    SYNC_OK = 0,                   // Normal operation
    SYNC_SLIGHT_AHEAD = 1,         // Slightly ahead, normal
    SYNC_MODERATE_AHEAD = 2,       // Moderately ahead, slow down
    SYNC_FAR_AHEAD = 3,            // Too far ahead, wait for remote
    SYNC_BEHIND = 4,               // Behind, need to catch up
    SYNC_DESYNCHRONIZED = 5,       // Major desync, need full resync
};
```

## Input Prediction & Confirmation

### Prediction Algorithm Framework
```c
enum PredictionStrategy {
    PREDICT_REPEAT_LAST = 0,       // Repeat last input (default)
    PREDICT_PATTERN_MATCH = 1,     // Pattern-based prediction
    PREDICT_CONTEXTUAL = 2,        // Game state-based prediction
    PREDICT_HYBRID = 3,            // Combination approach
};

struct InputPredictor {
    PredictionStrategy strategy;    // Current prediction method
    uint32_t prediction_history[64]; // Recent prediction accuracy
    uint32_t pattern_buffer[16];    // Input pattern buffer
    float accuracy_rates[4];        // Accuracy per strategy
    uint8_t current_confidence;     // Prediction confidence (0-100)
};
```

### Input Confirmation System
```c
struct InputConfirmation {
    uint32_t frame_number;         // Frame being confirmed
    uint32_t confirmed_inputs[2];  // Actual P1, P2 inputs
    uint32_t predicted_inputs[2];  // What we predicted
    uint8_t prediction_correct;    // 1=correct, 0=misprediction
    uint8_t rollback_required;     // 1=rollback needed
    uint16_t misprediction_distance; // How far back to rollback
};
```

## Bandwidth Optimization

### Message Compression
```c
// Compact input representation for frequently sent messages
struct CompactInputMessage {
    uint8_t message_type;          // INPUT_MESSAGE
    uint8_t frame_delta;           // Frame offset from last (0-255)
    uint16_t input_data;           // P1 input (11 bits) + P2 input (11 bits)
    uint8_t checksum;              // Simple 8-bit checksum
};
// Total: 5 bytes vs 28 bytes for full InputMessage
```

### Adaptive Message Frequency
```c
struct AdaptiveMessaging {
    uint32_t base_send_rate;       // Base messages per second
    uint32_t current_send_rate;    // Current adaptive rate
    uint32_t network_quality;      // Network quality score
    uint32_t prediction_accuracy;  // Recent prediction accuracy
    
    // Adaptive thresholds
    uint32_t high_quality_threshold;   // Reduce message rate above this
    uint32_t low_quality_threshold;    // Increase message rate below this
};
```

## Protocol State Machine

### Connection States
```c
enum ConnectionState {
    STATE_DISCONNECTED = 0,
    STATE_CONNECTING = 1,
    STATE_CONNECTED = 2,
    STATE_SYNCING = 3,
    STATE_PLAYING = 4,
    STATE_RECONNECTING = 5,
    STATE_DISCONNECTING = 6,
};

struct ProtocolState {
    ConnectionState current_state;
    uint32_t state_timer;          // Time in current state
    uint32_t retry_count;          // Connection retry attempts
    uint32_t last_received_time;   // Last message received timestamp
    uint32_t keepalive_timer;      // Keepalive message timing
};
```

### State Transitions
```c
// Connection establishment flow
DISCONNECTED → CONNECTING → CONNECTED → SYNCING → PLAYING

// Error handling flows  
PLAYING → RECONNECTING → PLAYING
PLAYING → DISCONNECTING → DISCONNECTED
Any State → DISCONNECTED (on fatal error)
```

## Network Transport Layer

### UDP Socket Configuration
```c
struct NetworkTransport {
    SOCKET udp_socket;             // UDP socket handle
    struct sockaddr_in local_addr; // Local endpoint
    struct sockaddr_in remote_addr; // Remote endpoint
    
    uint16_t local_port;           // Local port number
    uint16_t remote_port;          // Remote port number
    uint32_t socket_timeout;       // Socket timeout in ms
    
    uint8_t enable_nagle;          // Nagle algorithm (usually disabled)
    uint8_t enable_keepalive;      // Socket keepalive
    uint16_t buffer_size;          // Socket buffer size
};
```

### Message Queue Management
```c
#define MAX_OUTBOUND_QUEUE 256
#define MAX_INBOUND_QUEUE 256

struct MessageQueue {
    // Outbound message queue
    NetworkMessage outbound[MAX_OUTBOUND_QUEUE];
    uint16_t outbound_head;
    uint16_t outbound_tail;
    uint16_t outbound_count;
    
    // Inbound message queue
    NetworkMessage inbound[MAX_INBOUND_QUEUE];
    uint16_t inbound_head;
    uint16_t inbound_tail;
    uint16_t inbound_count;
    
    // Statistics
    uint32_t messages_sent;
    uint32_t messages_received;
    uint32_t messages_dropped;
};
```

## Error Handling & Recovery

### Desynchronization Detection
```c
struct DesyncDetection {
    uint32_t checksum_interval;    // Frames between checksum comparisons
    uint32_t max_checksum_diff;    // Maximum allowed checksum differences
    uint32_t desync_count;         // Recent desync occurrences
    uint32_t resync_threshold;     // Desync count before full resync
    
    uint32_t state_checksums[64];  // Recent state checksums
    uint8_t checksum_index;        // Current checksum buffer index
};
```

### Recovery Mechanisms
```c
enum RecoveryAction {
    RECOVERY_NONE = 0,             // No action needed
    RECOVERY_ROLLBACK = 1,         // Simple rollback
    RECOVERY_RESYNC = 2,           // Full state resynchronization  
    RECOVERY_DISCONNECT = 3,       // Give up and disconnect
};

struct RecoverySystem {
    RecoveryAction current_action;
    uint32_t recovery_attempts;
    uint32_t max_recovery_attempts;
    uint32_t recovery_timeout;
    uint32_t last_recovery_time;
};
```

## Performance Optimization

### Message Batching
```c
// Batch multiple inputs into single message for efficiency
struct BatchedInputMessage {
    NetworkMessageHeader header;
    
    uint8_t input_count;           // Number of inputs in batch (1-8)
    uint8_t reserved[3];           // Padding
    
    struct {
        uint32_t frame_number;     // Frame number
        uint16_t input_data;       // Compact P1+P2 inputs
        uint8_t checksum;          // Input checksum
        uint8_t reserved;          // Padding
    } inputs[8];                   // Up to 8 frames per message
};
```

### Priority Queuing
```c
enum MessagePriority {
    PRIORITY_CRITICAL = 0,         // INPUT_MESSAGE, SYNC_REQUEST
    PRIORITY_HIGH = 1,             // INPUT_ACK_MESSAGE, DESYNC_DETECTED
    PRIORITY_NORMAL = 2,           // QUALITY_REPORT
    PRIORITY_LOW = 3,              // KEEP_ALIVE
};

// Send critical messages immediately, batch lower priority
void send_message_with_priority(NetworkMessage* msg, MessagePriority priority);
```

## Integration with FM2K

### Network Thread Architecture
```c
// Separate network thread for non-blocking I/O
DWORD WINAPI NetworkThread(LPVOID param) {
    while (network_active) {
        // Process incoming messages
        process_inbound_messages();
        
        // Send outbound messages
        process_outbound_queue();
        
        // Update network statistics
        update_network_stats();
        
        // Adaptive delay based on performance
        Sleep(1); // 1ms sleep for ~1000 Hz network loop
    }
    return 0;
}
```

### Game Thread Integration
```c
// Called from main game loop at 100 FPS
void update_network_system() {
    // Send current frame input
    send_input_message(current_frame, g_p1_input, g_p2_input);
    
    // Process confirmed inputs
    process_input_confirmations();
    
    // Check for rollback conditions
    if (should_rollback()) {
        execute_rollback();
    }
    
    // Update frame advantage
    update_frame_advantage();
}
```

## Protocol Timing

### 100 FPS Integration
```c
// Timing aligned with FM2K's 10ms frames
#define FRAME_TIME_MS 10
#define NETWORK_UPDATE_MS 1          // Update network 10x per frame
#define KEEPALIVE_INTERVAL_MS 1000   // Keepalive every 1 second
#define CONNECTION_TIMEOUT_MS 5000   // Timeout after 5 seconds

struct NetworkTiming {
    uint32_t frame_start_time;     // Frame start timestamp
    uint32_t network_update_time;  // Last network update
    uint32_t last_keepalive_time;  // Last keepalive sent
    uint32_t last_received_time;   // Last message received
};
```

### Adaptive Input Delay
```c
// Automatically adjust input delay based on network conditions
uint8_t calculate_optimal_delay(uint32_t rtt, uint32_t jitter, float packet_loss) {
    uint8_t base_delay = (rtt / FRAME_TIME_MS) / 2;  // Half RTT in frames
    uint8_t jitter_compensation = (jitter / FRAME_TIME_MS) + 1;
    uint8_t loss_compensation = (packet_loss > 0.05f) ? 2 : 0;
    
    uint8_t total_delay = base_delay + jitter_compensation + loss_compensation;
    
    // Clamp to reasonable range
    return max(0, min(8, total_delay));
}
```

---

**Status**: ✅ Complete network protocol design
**Integration Ready**: Aligned with FM2K's 100 FPS architecture
**Bandwidth Optimized**: Lightweight messages with adaptive compression

*This protocol design provides a robust foundation for GGPO-style rollback networking, optimized specifically for FM2K's engine characteristics and performance requirements.*