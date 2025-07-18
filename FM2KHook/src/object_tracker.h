#pragma once

#include <cstdint>
#include <cstring>

namespace FM2K {
namespace ObjectTracking {

// Object tracking system that adapts to different FM2K games
// Handles 31 objects (test game) to 169+ objects (wanwan) efficiently

constexpr uint32_t MAX_OBJECTS = 1024;
constexpr uint32_t OBJECT_SIZE = 382;
constexpr uint32_t MAX_OBJECT_LISTS = 16;
constexpr uint32_t EVENT_BUFFER_SIZE = 256;

enum class ObjectEventType : uint8_t {
    CREATED = 1,
    DELETED = 2,
    MODIFIED = 3,
    TYPE_CHANGED = 4
};

// Compact representation of an object event
struct ObjectEvent {
    uint32_t frame;
    uint16_t object_index;
    uint8_t  event_type;    // ObjectEventType
    uint8_t  object_type;   // FM2K object type (4=fighter, 12=effect, etc)
    uint32_t checksum;      // Quick checksum of object data
};

// Snapshot of a linked list state
struct ListSnapshot {
    uint32_t head_ptr;      // Pointer to first object
    uint32_t tail_ptr;      // Pointer to last object  
    uint16_t object_count;  // Number of objects in list
    uint8_t  list_type;     // Object type for this list
    uint8_t  _padding;
};

// Active object information
struct ActiveObject {
    uint16_t index;         // Object pool index (0-1023)
    uint16_t type;          // FM2K object type
    uint32_t position_x;    // Current X position
    uint32_t position_y;    // Current Y position
    uint32_t checksum;      // Object state checksum
};

// Main object tracking state
class ObjectTracker {
public:
    // Initialize tracker
    void Initialize();
    
    // Update tracking state - call every frame
    void UpdateTracking(uint32_t frame);
    
    // Get current active object count
    uint32_t GetActiveObjectCount() const;
    
    // Get active objects for state saving
    uint32_t GetActiveObjects(ActiveObject* buffer, uint32_t max_objects) const;
    
    // Check if specific object is active
    bool IsObjectActive(uint16_t index) const;
    
    // Get object creation/deletion events since frame
    uint32_t GetEventsSinceFrame(uint32_t frame, ObjectEvent* buffer, uint32_t max_events) const;
    
    // Save minimal state (just active indices + checksums)
    uint32_t SaveMinimalState(uint8_t* buffer, uint32_t buffer_size) const;
    
    // Restore from minimal state
    bool RestoreMinimalState(const uint8_t* buffer, uint32_t buffer_size);
    
    // Get statistics for profiling
    struct Statistics {
        uint16_t current_active;
        uint16_t peak_active;
        uint16_t avg_active;
        uint16_t creation_rate;  // Objects created per 100 frames
        uint16_t deletion_rate;  // Objects deleted per 100 frames
        uint32_t total_created;
        uint32_t total_deleted;
    };
    Statistics GetStatistics() const { return stats; }
    
private:
    // Active object bitmap (1 bit per object, 1024 bits = 128 bytes)
    uint32_t active_bitmap[32];
    
    // Previous frame's bitmap for change detection
    uint32_t prev_bitmap[32];
    
    // Linked list snapshots
    ListSnapshot list_snapshots[MAX_OBJECT_LISTS];
    uint8_t active_list_count;
    
    // Circular buffer of object events
    ObjectEvent event_buffer[EVENT_BUFFER_SIZE];
    uint8_t event_write_idx;
    uint32_t total_events;
    
    // Statistics tracking
    Statistics stats;
    uint32_t frame_counter;
    uint32_t active_accumulator;  // For averaging
    
    // Helper methods
    void ScanObjectPool();
    void ScanLinkedLists();
    void DetectChanges(uint32_t frame);
    void UpdateStatistics();
    uint32_t CalculateObjectChecksum(uint16_t index) const;
    void AddEvent(uint32_t frame, uint16_t index, ObjectEventType type, uint8_t obj_type);
};

// Global tracker instance
extern ObjectTracker g_object_tracker;

// Rollback state formats for object tracking
struct MinimalObjectState {
    uint32_t frame;
    uint16_t active_count;
    uint16_t _padding;
    
    // Followed by array of:
    struct Entry {
        uint16_t index;
        uint16_t type;
        uint32_t checksum;
        uint32_t position_x;
        uint32_t position_y;
    } entries[];
    
    uint32_t GetSizeBytes() const {
        return sizeof(MinimalObjectState) + (active_count * sizeof(Entry));
    }
};

// Smart save/restore functions
uint32_t SaveObjectsAdaptive(uint8_t* buffer, uint32_t buffer_size, uint32_t frame);
bool RestoreObjectsAdaptive(const uint8_t* buffer, uint32_t buffer_size);

// Object pool analysis functions
uint32_t AnalyzeObjectTypes(uint16_t* type_counts, uint32_t max_types);
void DumpObjectPoolState(const char* filename = nullptr);

} // namespace ObjectTracking
} // namespace FM2K