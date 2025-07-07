#pragma once

#include <cstdint>
#include <windows.h>

namespace FM2K {
namespace IPC {

// Event types for IPC communication
enum class EventType : uint8_t {
    NONE = 0,
    FRAME_ADVANCED = 1,
    STATE_SAVED = 2,
    STATE_LOADED = 3,
    HIT_TABLES_INIT = 4,
    VISUAL_STATE_CHANGED = 5,
    HOOK_ERROR = 255      // Changed from ERROR to HOOK_ERROR to avoid Windows macro conflict
};

// Visual state data structure
struct VisualState {
    uint32_t effect_flags;
    uint32_t timer_values[16];
    uint32_t color_values[16];
    uint32_t target_ids[16];
};

// State data structure
struct StateData {
    uint32_t checksum;
    uint32_t frame_number;
};

// Error data structure
struct ErrorData {
    char message[256];
};

// Union of all possible event data types
union EventData {
    StateData state;
    VisualState visual;
    ErrorData error;
};

// Event structure
struct Event {
    EventType type;
    uint32_t frame_number;
    uint32_t timestamp_ms;
    EventData data;
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

// Ring buffer for events, mapped into shared memory
struct EventBuffer {
    static constexpr size_t BUFFER_SIZE = 1024;
    
    volatile LONG write_index;      // Next slot to write
    volatile LONG read_index;       // Next slot to read
    Event events[BUFFER_SIZE];     // Circular buffer of events
    
    // Helper to check if buffer is full
    bool IsFull() const {
        return (static_cast<size_t>((write_index + 1) % BUFFER_SIZE)) == static_cast<size_t>(read_index);
    }
    
    // Helper to check if buffer is empty
    bool IsEmpty() const {
        return write_index == read_index;
    }
};

// Initialize/cleanup IPC
bool Init();
void Shutdown();

// Event functions
bool PostEvent(const Event& event);
bool ReadEvent(Event& event);
bool WriteEvent(const Event& event);

} // namespace IPC
} // namespace FM2K 