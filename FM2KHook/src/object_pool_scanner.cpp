#include "object_pool_scanner.h"
#include "logging.h"
#include <windows.h>
#include <cstring>

namespace FM2K {
namespace ObjectPool {

std::vector<CompactObject> Scanner::ScanActiveObjects() {
    std::vector<CompactObject> active_objects;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Starting object pool scan...");
    
    // SAFETY: Check if object pool base address is valid
    if (IsBadReadPtr((void*)OBJECT_POOL_BASE_ADDR, OBJECT_SIZE_BYTES)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CRASH DEBUG: Object pool base address 0x%08X invalid", OBJECT_POOL_BASE_ADDR);
        return active_objects;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Object pool base address 0x%08X is valid, starting scan...", OBJECT_POOL_BASE_ADDR);
    
    // Scan all slots to find active objects
    uint16_t max_slots_to_scan = MAX_OBJECT_SLOTS; // Full scan of 1024 slots
    
    for (uint16_t slot = 0; slot < max_slots_to_scan; slot++) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Scanning slot %d...", slot);
        
        uint32_t type, id, x_coord, y_coord, anim_state;
        
        if (ReadRawObjectData(slot, &type, &id, &x_coord, &y_coord, &anim_state)) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Slot %d: type=%d, id=%d", slot, type, id);
            
            // Only include active objects (type != 0)
            if (type != 0) {
                try {
                    active_objects.emplace_back(slot, type, id, x_coord, y_coord, anim_state);
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Added active object at slot %d", slot);
                } catch (...) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CRASH DEBUG: Exception adding object at slot %d", slot);
                    break;
                }
            }
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Failed to read slot %d", slot);
        }
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Object scan completed: %zu active objects found", active_objects.size());
    return active_objects;
}

std::vector<DetailedObject> Scanner::ScanDetailedObjects() {
    std::vector<DetailedObject> detailed_objects;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Starting DETAILED object pool scan...");
    
    // SAFETY: Check if object pool base address is valid
    if (IsBadReadPtr((void*)OBJECT_POOL_BASE_ADDR, OBJECT_SIZE_BYTES)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DETAILED SCAN: Object pool base address 0x%08X invalid", OBJECT_POOL_BASE_ADDR);
        return detailed_objects;
    }
    
    // Scan all slots for detailed analysis
    for (uint16_t slot = 0; slot < MAX_OBJECT_SLOTS; slot++) {
        DetailedObject obj;
        if (ReadDetailedObjectData(slot, &obj)) {
            if (obj.IsActive()) {
                detailed_objects.push_back(obj);
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "DETAILED: Found active object at slot %d", slot);
            }
        }
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DETAILED scan completed: %zu active objects found", detailed_objects.size());
    return detailed_objects;
}

DetailedObject Scanner::ReadDetailedObjectFromSlot(uint16_t slot) {
    DetailedObject obj;
    if (ReadDetailedObjectData(slot, &obj)) {
        return obj;
    }
    return DetailedObject();  // Return empty object if read failed
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
    uintptr_t pool_start = OBJECT_POOL_BASE_ADDR;
    uintptr_t pool_end = pool_start + (MAX_OBJECT_SLOTS * OBJECT_SIZE_BYTES);
    
    // SAFETY: Basic validation
    if (IsBadWritePtr((void*)pool_start, (UINT_PTR)pool_end - (UINT_PTR)pool_start)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ClearObjectPool: Invalid object pool address range!");
        return;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clearing object pool from 0x%08X to 0x%08X", pool_start, pool_end);
    
    // Zero out the entire memory region
    memset((void*)pool_start, 0, MAX_OBJECT_SLOTS * OBJECT_SIZE_BYTES);
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

bool Scanner::ReadDetailedObjectData(uint16_t slot, DetailedObject* obj) {
    if (slot >= MAX_OBJECT_SLOTS || !obj) return false;
    
    uintptr_t slot_addr = GetSlotAddress(slot);
    
    // Initialize object
    obj->slot_index = slot;
    
    // Read structured fields from known offsets
    uint32_t* type_ptr = (uint32_t*)(slot_addr + 0x00);
    uint32_t* id_ptr = (uint32_t*)(slot_addr + 0x04);
    uint32_t* pos_x_ptr = (uint32_t*)(slot_addr + 0x08);
    uint32_t* pos_y_ptr = (uint32_t*)(slot_addr + 0x0C);
    uint32_t* vel_x_ptr = (uint32_t*)(slot_addr + 0x10);
    uint32_t* vel_y_ptr = (uint32_t*)(slot_addr + 0x14);
    uint32_t* unknown_18_ptr = (uint32_t*)(slot_addr + 0x18);
    uint32_t* unknown_1C_ptr = (uint32_t*)(slot_addr + 0x1C);
    uint32_t* unknown_20_ptr = (uint32_t*)(slot_addr + 0x20);
    uint32_t* unknown_24_ptr = (uint32_t*)(slot_addr + 0x24);
    uint32_t* unknown_28_ptr = (uint32_t*)(slot_addr + 0x28);
    uint32_t* anim_state_ptr = (uint32_t*)(slot_addr + 0x2C);
    uint32_t* health_ptr = (uint32_t*)(slot_addr + 0x30);
    uint32_t* state_flags_ptr = (uint32_t*)(slot_addr + 0x34);
    uint32_t* timer_ptr = (uint32_t*)(slot_addr + 0x38);
    uint32_t* unknown_3C_ptr = (uint32_t*)(slot_addr + 0x3C);
    
    // Validate critical pointers
    if (IsBadReadPtr(type_ptr, sizeof(uint32_t)) ||
        IsBadReadPtr(id_ptr, sizeof(uint32_t)) ||
        IsBadReadPtr((void*)slot_addr, OBJECT_SIZE_BYTES)) {
        return false;
    }
    
    // Read structured fields
    obj->type = *type_ptr;
    obj->id = *id_ptr;
    obj->position_x = *pos_x_ptr;
    obj->position_y = *pos_y_ptr;
    obj->velocity_x = *vel_x_ptr;
    obj->velocity_y = *vel_y_ptr;
    obj->unknown_18 = *unknown_18_ptr;
    obj->unknown_1C = *unknown_1C_ptr;
    obj->unknown_20 = *unknown_20_ptr;
    obj->unknown_24 = *unknown_24_ptr;
    obj->unknown_28 = *unknown_28_ptr;
    obj->animation_state = *anim_state_ptr;
    obj->health_damage = *health_ptr;
    obj->state_flags = *state_flags_ptr;
    obj->timer_counter = *timer_ptr;
    obj->unknown_3C = *unknown_3C_ptr;
    
    // Copy complete raw data for analysis
    memcpy(obj->raw_data, (void*)slot_addr, OBJECT_SIZE_BYTES);
    
    return true;
}

void Scanner::LogDetailedObjectInfo(uint16_t slot) {
    DetailedObject obj = ReadDetailedObjectFromSlot(slot);
    if (!obj.IsActive()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SLOT %u: INACTIVE", slot);
        return;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== DETAILED OBJECT ANALYSIS: SLOT %u ===", slot);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Type: %u (%s)", obj.type, obj.GetTypeDescription().c_str());
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ID: %u", obj.id);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Position: (%u, %u)", obj.position_x, obj.position_y);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Velocity: (%u, %u)", obj.velocity_x, obj.velocity_y);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Animation State: %u", obj.animation_state);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Health/Damage: %u", obj.health_damage);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "State Flags: 0x%08X", obj.state_flags);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Timer/Counter: %u", obj.timer_counter);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Unknown Fields: 0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X", 
               obj.unknown_18, obj.unknown_1C, obj.unknown_20, obj.unknown_24, obj.unknown_28, obj.unknown_3C);
    
    // Show first 64 bytes of raw data as hex dump
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Raw Data (first 64 bytes):");
    std::string hex_dump = "";
    for (int i = 0; i < 64 && i < 382; i++) {
        char hex[4];
        sprintf(hex, "%02X ", obj.raw_data[i]);
        hex_dump += hex;
        if ((i + 1) % 16 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  %04X: %s", i - 15, hex_dump.c_str());
            hex_dump = "";
        }
    }
    if (!hex_dump.empty()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  %04X: %s", (64 / 16) * 16, hex_dump.c_str());
    }
}

void Scanner::LogAllActiveObjects() {
    auto detailed_objects = ScanDetailedObjects();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== COMPLETE ACTIVE OBJECT BREAKDOWN ===");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Found %zu active objects in pool", detailed_objects.size());
    
    for (const auto& obj : detailed_objects) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Slot %u: Type %u (%s), ID %u, Pos(%u,%u), Vel(%u,%u), Anim %u, Health %u, Flags 0x%08X", 
                   obj.slot_index, obj.type, obj.GetTypeDescription().c_str(), obj.id, 
                   obj.position_x, obj.position_y, obj.velocity_x, obj.velocity_y,
                   obj.animation_state, obj.health_damage, obj.state_flags);
    }
}

// DetailedObject method implementations (inside namespace)
std::string DetailedObject::GetTypeDescription() const {
    switch (type) {
        case 0: return "INACTIVE";
        case 1: return "SYSTEM";
        case 2: return "MENU";
        case 3: return "BACKGROUND";
        case 4: return "CHARACTER";
        case 5: return "PROJECTILE";
        case 6: return "EFFECT";
        case 7: return "UI_ELEMENT";
        case 8: return "SOUND";
        case 9: return "COLLISION";
        case 10: return "TRIGGER";
        default: return "UNKNOWN_TYPE_" + std::to_string(type);
    }
}

std::string DetailedObject::GetDetailedDescription() const {
    std::string desc = GetTypeDescription();
    
    if (HasPosition()) {
        desc += " at (" + std::to_string(position_x) + "," + std::to_string(position_y) + ")";
    }
    
    if (HasVelocity()) {
        desc += " moving (" + std::to_string(velocity_x) + "," + std::to_string(velocity_y) + ")";
    }
    
    if (animation_state != 0) {
        desc += " anim:" + std::to_string(animation_state);
    }
    
    if (health_damage != 0) {
        desc += " hp:" + std::to_string(health_damage);
    }
    
    if (state_flags != 0) {
        desc += " flags:0x" + std::to_string(state_flags);
    }
    
    return desc;
}

} // namespace ObjectPool
} // namespace FM2K