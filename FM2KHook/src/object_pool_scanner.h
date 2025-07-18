#pragma once

#include <cstdint>
#include <vector>

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

// Object pool scanner class
class Scanner {
public:
    // Scan the entire object pool and return active objects
    static std::vector<CompactObject> ScanActiveObjects();
    
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
    
private:
    // Read raw object data from memory
    static bool ReadRawObjectData(uint16_t slot, uint32_t* type, uint32_t* id, 
                                  uint32_t* x_coord, uint32_t* y_coord, 
                                  uint32_t* anim_state);
    
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