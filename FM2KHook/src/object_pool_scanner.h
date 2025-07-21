#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

namespace FM2K {
namespace ObjectPool {

// FM2K object pool constants
constexpr uintptr_t OBJECT_POOL_BASE_ADDR = 0x4701E0;
constexpr uint32_t OBJECT_SIZE_BYTES = 382;
constexpr uint32_t MAX_OBJECT_SLOTS = 1024;

// Compact object representation for rollback
struct CompactObject {
    uint16_t slot_index;        // Which slot this object occupies
    uint32_t type;              // Object type (0=inactive, 4=active, etc.)
    uint32_t id;                // Object ID (determines behavior: 12=menu, 50=char, etc.)
    uint32_t x_coord;           // X coordinate (offset 8)
    uint32_t y_coord;           // Y coordinate (offset 12) 
    uint32_t animation_state;   // Animation frame/state (offset 44)
    
    // Default constructor
    CompactObject() : slot_index(0), type(0), id(0), x_coord(0), y_coord(0), animation_state(0) {}
    
    // Constructor for easy creation
    CompactObject(uint16_t slot, uint32_t obj_type, uint32_t obj_id, 
                  uint32_t x, uint32_t y, uint32_t anim_state)
        : slot_index(slot), type(obj_type), id(obj_id), 
          x_coord(x), y_coord(y), animation_state(anim_state) {}
};

// Comprehensive object structure for detailed analysis
struct DetailedObject {
    uint16_t slot_index;        // Which slot this object occupies
    uint32_t type;              // +0x00: Object type
    uint32_t id;                // +0x04: Object ID
    uint32_t position_x;        // +0x08: X position
    uint32_t position_y;        // +0x0C: Y position
    uint32_t velocity_x;        // +0x10: X velocity
    uint32_t velocity_y;        // +0x14: Y velocity
    uint32_t unknown_18;        // +0x18: Unknown field
    uint32_t unknown_1C;        // +0x1C: Unknown field
    uint32_t unknown_20;        // +0x20: Unknown field
    uint32_t unknown_24;        // +0x24: Unknown field
    uint32_t unknown_28;        // +0x28: Unknown field
    uint32_t animation_state;   // +0x2C: Animation state/frame
    uint32_t health_damage;     // +0x30: Health/damage related
    uint32_t state_flags;       // +0x34: Object state flags
    uint32_t timer_counter;     // +0x38: Timer/counter
    uint32_t unknown_3C;        // +0x3C: Unknown field
    
    // Raw bytes for complete analysis
    uint8_t raw_data[382];      // Complete 382-byte object data
    
    // Default constructor
    DetailedObject() { memset(this, 0, sizeof(DetailedObject)); }
    
    // Analysis methods
    bool IsActive() const { return type != 0; }
    bool HasPosition() const { return position_x != 0 || position_y != 0; }
    bool HasVelocity() const { return velocity_x != 0 || velocity_y != 0; }
    std::string GetTypeDescription() const;
    std::string GetDetailedDescription() const;
};

// Object pool scanner class
class Scanner {
public:
    // Scan the entire object pool and return active objects
    static std::vector<CompactObject> ScanActiveObjects();
    
    // Enhanced detailed object scanning
    static std::vector<DetailedObject> ScanDetailedObjects();
    static DetailedObject ReadDetailedObjectFromSlot(uint16_t slot);
    
    // Get total count of active objects
    static uint32_t GetActiveObjectCount();
    
    // Check if specific slot is active
    static bool IsSlotActive(uint16_t slot);
    
    // Read specific object from slot
    static CompactObject ReadObjectFromSlot(uint16_t slot);
    
    // Restore object to specific slot
    static bool RestoreObjectToSlot(const CompactObject& obj);
    
    // Clear entire object pool (for rollback restoration)
    static void ClearObjectPool();
    
    // Detailed object analysis and logging
    static void LogDetailedObjectInfo(uint16_t slot);
    static void LogAllActiveObjects();
    
private:
    // Read raw object data from memory
    static bool ReadRawObjectData(uint16_t slot, uint32_t* type, uint32_t* id, 
                                  uint32_t* x_coord, uint32_t* y_coord, 
                                  uint32_t* anim_state);
    
    // Read comprehensive object data from memory
    static bool ReadDetailedObjectData(uint16_t slot, DetailedObject* obj);
    
    // Write raw object data to memory
    static bool WriteRawObjectData(uint16_t slot, uint32_t type, uint32_t id,
                                   uint32_t x_coord, uint32_t y_coord,
                                   uint32_t anim_state);
    
    // Calculate memory address for slot
    static uintptr_t GetSlotAddress(uint16_t slot);
};

// Rollback state structure
struct ObjectPoolState {
    uint32_t frame_number;
    uint32_t active_object_count;
    std::vector<CompactObject> objects;
    
    // Calculate total serialization size
    uint32_t GetSerializedSize() const {
        return sizeof(uint32_t) * 2 + // frame_number + active_object_count
               objects.size() * sizeof(CompactObject);
    }
    
    // Serialize to buffer for GekkoNet
    bool SerializeTo(uint8_t* buffer, uint32_t buffer_size) const;
    
    // Deserialize from buffer
    bool DeserializeFrom(const uint8_t* buffer, uint32_t buffer_size);
};

} // namespace ObjectPool
} // namespace FM2K