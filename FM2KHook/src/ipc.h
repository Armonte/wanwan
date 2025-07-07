#pragma once

#include <windows.h>
#include <cstdint>

namespace FM2K {
namespace IPC {

// Event types that can be sent from hook DLL to launcher
enum class EventType : uint8_t {
    FRAME_ADVANCED = 1,    // Game frame completed
    STATE_SAVED = 2,       // State was saved to buffer
    STATE_LOADED = 3,      // State was loaded from buffer
    HIT_TABLES_INIT = 4,   // Hit judge tables were initialized
    VISUAL_STATE_CHANGED = 5, // Visual effects changed
    ERROR = 255            // Error occurred
};

// Visual effect state tracking
struct VisualState {
    uint32_t active_effects;      // Bitfield of active effects
    uint32_t effect_timers[8];    // Array of effect durations
    uint32_t color_values[8][3];  // RGB values for each effect
    uint32_t target_ids[8];       // Effect target identifiers
};

// Complete game state for serialization
struct GameState {
    // Core game state (from previous implementation)
    uint32_t frame_number;
    uint32_t random_seed;
    uint32_t input_buffer_index;
    
    // Hit detection state (~1KB)
    uint8_t hit_judge_tables[0x430120 - 0x42470C];
    
    // Visual state
    VisualState visual_state;
    
    // Round system state
    uint32_t round_timer;      // 0x470060
    uint32_t round_limit;      // 0x470048
    uint32_t round_state;      // 0x47004C
    uint32_t game_mode;        // 0x470040
};

// Fixed-size event structure
struct Event {
    EventType type;
    uint8_t player_index;     // Which player triggered the event
    uint16_t frame_number;    // Current game frame
    uint32_t timestamp_ms;    // GetTickCount() when event occurred
    
    // Additional event-specific data
    union {
        struct {
            uint32_t effect_id;    // For VISUAL_STATE_CHANGED
            uint32_t duration;
        } visual;
        
        struct {
            uint32_t table_size;   // For HIT_TABLES_INIT
            uint32_t checksum;
        } hit_tables;
    } data;
};

// Ring buffer for events, mapped into shared memory
struct EventBuffer {
    static constexpr size_t BUFFER_SIZE = 1024;
    
    volatile LONG write_index;      // Next slot to write
    volatile LONG read_index;       // Next slot to read
    Event events[BUFFER_SIZE];     // Circular buffer of events
    
    // Helper to check if buffer is full
    bool IsFull() const {
        return ((write_index + 1) % BUFFER_SIZE) == read_index;
    }
    
    // Helper to check if buffer is empty
    bool IsEmpty() const {
        return write_index == read_index;
    }
};

// Initialize IPC system
bool Init();

// Shutdown IPC system
void Shutdown();

// Post an event to the buffer
bool PostEvent(EventType type, const Event* data = nullptr);

// Read next event from buffer (returns false if no events)
bool ReadEvent(EventBuffer* buffer, Event* out);

} // namespace IPC
} // namespace FM2K 