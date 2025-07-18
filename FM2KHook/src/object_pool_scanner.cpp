#include "object_pool_scanner.h"
#include "logging.h"
#include <windows.h>
#include <cstring>

namespace FM2K {
namespace ObjectPool {

std::vector<CompactObject> Scanner::ScanActiveObjects() {
    std::vector<CompactObject> active_objects;
    
    for (uint16_t slot = 0; slot < MAX_OBJECT_SLOTS; slot++) {
        uint32_t type, id, x_coord, y_coord, anim_state;
        
        if (ReadRawObjectData(slot, &type, &id, &x_coord, &y_coord, &anim_state)) {
            // Only include active objects (type != 0)
            if (type != 0) {
                active_objects.emplace_back(slot, type, id, x_coord, y_coord, anim_state);
            }
        }
    }
    
    return active_objects;
}

uint32_t Scanner::GetActiveObjectCount() {
    uint32_t count = 0;
    
    for (uint16_t slot = 0; slot < MAX_OBJECT_SLOTS; slot++) {
        if (IsSlotActive(slot)) {
            count++;
        }
    }
    
    return count;
}

bool Scanner::IsSlotActive(uint16_t slot) {
    if (slot >= MAX_OBJECT_SLOTS) return false;
    
    uintptr_t slot_addr = GetSlotAddress(slot);
    uint32_t* type_ptr = (uint32_t*)slot_addr;
    
    if (IsBadReadPtr(type_ptr, sizeof(uint32_t))) {
        return false;
    }
    
    return *type_ptr != 0;
}

CompactObject Scanner::ReadObjectFromSlot(uint16_t slot) {
    uint32_t type, id, x_coord, y_coord, anim_state;
    
    if (ReadRawObjectData(slot, &type, &id, &x_coord, &y_coord, &anim_state)) {
        return CompactObject(slot, type, id, x_coord, y_coord, anim_state);
    }
    
    // Return inactive object if read failed
    return CompactObject(slot, 0, 0, 0, 0, 0);
}

bool Scanner::RestoreObjectToSlot(const CompactObject& obj) {
    return WriteRawObjectData(obj.slot_index, obj.type, obj.id, 
                              obj.x_coord, obj.y_coord, obj.animation_state);
}

void Scanner::ClearObjectPool() {
    for (uint16_t slot = 0; slot < MAX_OBJECT_SLOTS; slot++) {
        uintptr_t slot_addr = GetSlotAddress(slot);
        
        if (!IsBadWritePtr((void*)slot_addr, OBJECT_SIZE_BYTES)) {
            // Zero out the entire object slot
            memset((void*)slot_addr, 0, OBJECT_SIZE_BYTES);
        }
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Object pool cleared for rollback restoration");
}

bool Scanner::ReadRawObjectData(uint16_t slot, uint32_t* type, uint32_t* id,
                                uint32_t* x_coord, uint32_t* y_coord,
                                uint32_t* anim_state) {
    if (slot >= MAX_OBJECT_SLOTS) return false;
    
    uintptr_t slot_addr = GetSlotAddress(slot);
    
    // Read key fields from object structure
    uint32_t* type_ptr = (uint32_t*)(slot_addr + 0);   // Offset 0: type
    uint32_t* id_ptr = (uint32_t*)(slot_addr + 4);     // Offset 4: ID
    uint32_t* x_ptr = (uint32_t*)(slot_addr + 8);      // Offset 8: X coordinate
    uint32_t* y_ptr = (uint32_t*)(slot_addr + 12);     // Offset 12: Y coordinate
    uint32_t* anim_ptr = (uint32_t*)(slot_addr + 44);  // Offset 44: animation state
    
    // Validate all pointers before reading
    if (IsBadReadPtr(type_ptr, sizeof(uint32_t)) ||
        IsBadReadPtr(id_ptr, sizeof(uint32_t)) ||
        IsBadReadPtr(x_ptr, sizeof(uint32_t)) ||
        IsBadReadPtr(y_ptr, sizeof(uint32_t)) ||
        IsBadReadPtr(anim_ptr, sizeof(uint32_t))) {
        return false;
    }
    
    // Read values
    *type = *type_ptr;
    *id = *id_ptr;
    *x_coord = *x_ptr;
    *y_coord = *y_ptr;
    *anim_state = *anim_ptr;
    
    return true;
}

bool Scanner::WriteRawObjectData(uint16_t slot, uint32_t type, uint32_t id,
                                 uint32_t x_coord, uint32_t y_coord,
                                 uint32_t anim_state) {
    if (slot >= MAX_OBJECT_SLOTS) return false;
    
    uintptr_t slot_addr = GetSlotAddress(slot);
    
    // Get pointers to key fields
    uint32_t* type_ptr = (uint32_t*)(slot_addr + 0);
    uint32_t* id_ptr = (uint32_t*)(slot_addr + 4);
    uint32_t* x_ptr = (uint32_t*)(slot_addr + 8);
    uint32_t* y_ptr = (uint32_t*)(slot_addr + 12);
    uint32_t* anim_ptr = (uint32_t*)(slot_addr + 44);
    
    // Validate all pointers before writing
    if (IsBadWritePtr(type_ptr, sizeof(uint32_t)) ||
        IsBadWritePtr(id_ptr, sizeof(uint32_t)) ||
        IsBadWritePtr(x_ptr, sizeof(uint32_t)) ||
        IsBadWritePtr(y_ptr, sizeof(uint32_t)) ||
        IsBadWritePtr(anim_ptr, sizeof(uint32_t))) {
        return false;
    }
    
    // Write values
    *type_ptr = type;
    *id_ptr = id;
    *x_ptr = x_coord;
    *y_ptr = y_coord;
    *anim_ptr = anim_state;
    
    // Set standard fields for active objects
    if (type != 0) {
        // Set the standard 0xFFFFFFFF marker at offset 16 (from our research)
        uint32_t* marker_ptr = (uint32_t*)(slot_addr + 16);
        if (!IsBadWritePtr(marker_ptr, sizeof(uint32_t))) {
            *marker_ptr = 0xFFFFFFFF;
        }
    }
    
    return true;
}

uintptr_t Scanner::GetSlotAddress(uint16_t slot) {
    return OBJECT_POOL_BASE_ADDR + (slot * OBJECT_SIZE_BYTES);
}

bool ObjectPoolState::SerializeTo(uint8_t* buffer, uint32_t buffer_size) const {
    uint32_t required_size = GetSerializedSize();
    if (buffer_size < required_size) {
        return false;
    }
    
    uint8_t* write_ptr = buffer;
    
    // Write header
    memcpy(write_ptr, &frame_number, sizeof(uint32_t));
    write_ptr += sizeof(uint32_t);
    
    memcpy(write_ptr, &active_object_count, sizeof(uint32_t));
    write_ptr += sizeof(uint32_t);
    
    // Write objects
    for (const auto& obj : objects) {
        memcpy(write_ptr, &obj, sizeof(CompactObject));
        write_ptr += sizeof(CompactObject);
    }
    
    return true;
}

bool ObjectPoolState::DeserializeFrom(const uint8_t* buffer, uint32_t buffer_size) {
    if (buffer_size < sizeof(uint32_t) * 2) {
        return false; // Not enough data for header
    }
    
    const uint8_t* read_ptr = buffer;
    
    // Read header
    memcpy(&frame_number, read_ptr, sizeof(uint32_t));
    read_ptr += sizeof(uint32_t);
    
    memcpy(&active_object_count, read_ptr, sizeof(uint32_t));
    read_ptr += sizeof(uint32_t);
    
    // Validate object count
    uint32_t objects_data_size = active_object_count * sizeof(CompactObject);
    if (buffer_size < sizeof(uint32_t) * 2 + objects_data_size) {
        return false; // Not enough data for objects
    }
    
    // Read objects
    objects.clear();
    objects.reserve(active_object_count);
    
    for (uint32_t i = 0; i < active_object_count; i++) {
        CompactObject obj(0, 0, 0, 0, 0, 0); // Default constructor values
        memcpy(&obj, read_ptr, sizeof(CompactObject));
        read_ptr += sizeof(CompactObject);
        objects.push_back(obj);
    }
    
    return true;
}

} // namespace ObjectPool
} // namespace FM2K