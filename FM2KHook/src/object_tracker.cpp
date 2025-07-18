#include "object_tracker.h"
#include <SDL3/SDL.h>
#include <windows.h>
#include <algorithm>

namespace FM2K {
namespace ObjectTracking {

// Global instance
ObjectTracker g_object_tracker;

// FM2K memory addresses from analysis
constexpr uintptr_t OBJECT_POOL_ADDR = 0x4701E0;
constexpr uintptr_t OBJECT_LIST_HEADS = 0x430240;
constexpr uintptr_t OBJECT_LIST_TAILS = 0x430244;

struct FM2KObject {
    uint32_t type;          // +0x00
    uint32_t id;            // +0x04
    uint32_t position_x;    // +0x08
    uint32_t position_y;    // +0x0C
    uint32_t velocity_x;    // +0x10
    uint32_t velocity_y;    // +0x14
    // ... rest of 382 byte structure
    uint8_t data[382 - 24]; // Remaining data
};

void ObjectTracker::Initialize() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K ObjectTracker: Initializing adaptive object tracking system");
    
    memset(this, 0, sizeof(ObjectTracker));
    
    // Initial scan to establish baseline
    ScanObjectPool();
    memcpy(prev_bitmap, active_bitmap, sizeof(active_bitmap));
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K ObjectTracker: Found %u active objects on initialization", 
                GetActiveObjectCount());
}

void ObjectTracker::UpdateTracking(uint32_t frame) {
    frame_counter++;
    
    // Scan current state
    ScanObjectPool();
    ScanLinkedLists();
    
    // Detect changes from previous frame
    DetectChanges(frame);
    
    // Update statistics
    UpdateStatistics();
    
    // Save current state as previous
    memcpy(prev_bitmap, active_bitmap, sizeof(active_bitmap));
}

void ObjectTracker::ScanObjectPool() {
    FM2KObject* pool = (FM2KObject*)OBJECT_POOL_ADDR;
    
    // Clear active bitmap
    memset(active_bitmap, 0, sizeof(active_bitmap));
    
    // Check if we can read the object pool
    if (IsBadReadPtr(pool, MAX_OBJECTS * OBJECT_SIZE)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FM2K ObjectTracker: Cannot read object pool memory");
        return;
    }
    
    // Scan all object slots
    for (uint32_t i = 0; i < MAX_OBJECTS; i++) {
        FM2KObject* obj = &pool[i];
        
        // Object is active if type is non-zero
        if (obj->type != 0) {
            // Set bit in bitmap
            uint32_t word_idx = i / 32;
            uint32_t bit_idx = i % 32;
            active_bitmap[word_idx] |= (1u << bit_idx);
        }
    }
}

void ObjectTracker::ScanLinkedLists() {
    // Read linked list head pointers
    uint32_t* heads = (uint32_t*)OBJECT_LIST_HEADS;
    
    if (IsBadReadPtr(heads, MAX_OBJECT_LISTS * sizeof(uint32_t))) {
        return;
    }
    
    active_list_count = 0;
    
    // Scan each potential list
    for (uint32_t i = 0; i < MAX_OBJECT_LISTS; i++) {
        if (heads[i] != 0) {
            ListSnapshot& snap = list_snapshots[active_list_count];
            snap.head_ptr = heads[i];
            snap.list_type = i;
            snap.object_count = 0;
            
            // Count objects in this list (simplified - real implementation would walk the list)
            // For now, we'll estimate based on the head pointer
            
            active_list_count++;
        }
    }
}

void ObjectTracker::DetectChanges(uint32_t frame) {
    FM2KObject* pool = (FM2KObject*)OBJECT_POOL_ADDR;
    
    // Compare current bitmap with previous to find changes
    for (uint32_t word = 0; word < 32; word++) {
        uint32_t created = active_bitmap[word] & ~prev_bitmap[word];  // Newly active
        uint32_t deleted = ~active_bitmap[word] & prev_bitmap[word];  // Newly inactive
        
        if (created || deleted) {
            // Process each bit
            for (uint32_t bit = 0; bit < 32; bit++) {
                uint32_t mask = 1u << bit;
                uint32_t index = word * 32 + bit;
                
                if (created & mask) {
                    // Object created
                    FM2KObject* obj = &pool[index];
                    AddEvent(frame, index, ObjectEventType::CREATED, obj->type);
                    stats.total_created++;
                    
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
                               "FM2K ObjectTracker: Object %u created (type=%u) at frame %u", 
                               index, obj->type, frame);
                }
                
                if (deleted & mask) {
                    // Object deleted
                    AddEvent(frame, index, ObjectEventType::DELETED, 0);
                    stats.total_deleted++;
                    
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
                               "FM2K ObjectTracker: Object %u deleted at frame %u", 
                               index, frame);
                }
            }
        }
    }
}

void ObjectTracker::UpdateStatistics() {
    uint32_t current = GetActiveObjectCount();
    
    stats.current_active = current;
    stats.peak_active = std::max(stats.peak_active, (uint16_t)current);
    
    // Update running average
    active_accumulator += current;
    if (frame_counter > 0) {
        stats.avg_active = active_accumulator / frame_counter;
    }
    
    // Calculate rates (per 100 frames)
    if (frame_counter >= 100) {
        uint32_t window_frames = std::min(frame_counter, 100u);
        
        // Count recent events
        uint32_t recent_creates = 0, recent_deletes = 0;
        for (uint32_t i = 0; i < EVENT_BUFFER_SIZE; i++) {
            const ObjectEvent& evt = event_buffer[i];
            if (evt.frame > frame_counter - window_frames) {
                if (evt.event_type == (uint8_t)ObjectEventType::CREATED) recent_creates++;
                if (evt.event_type == (uint8_t)ObjectEventType::DELETED) recent_deletes++;
            }
        }
        
        stats.creation_rate = recent_creates;
        stats.deletion_rate = recent_deletes;
    }
}

uint32_t ObjectTracker::GetActiveObjectCount() const {
    uint32_t count = 0;
    for (uint32_t i = 0; i < 32; i++) {
        count += __builtin_popcount(active_bitmap[i]);
    }
    return count;
}

bool ObjectTracker::IsObjectActive(uint16_t index) const {
    if (index >= MAX_OBJECTS) return false;
    
    uint32_t word_idx = index / 32;
    uint32_t bit_idx = index % 32;
    
    return (active_bitmap[word_idx] & (1u << bit_idx)) != 0;
}

uint32_t ObjectTracker::GetActiveObjects(ActiveObject* buffer, uint32_t max_objects) const {
    FM2KObject* pool = (FM2KObject*)OBJECT_POOL_ADDR;
    uint32_t count = 0;
    
    for (uint32_t i = 0; i < MAX_OBJECTS && count < max_objects; i++) {
        if (IsObjectActive(i)) {
            FM2KObject* obj = &pool[i];
            
            buffer[count].index = i;
            buffer[count].type = obj->type;
            buffer[count].position_x = obj->position_x;
            buffer[count].position_y = obj->position_y;
            buffer[count].checksum = CalculateObjectChecksum(i);
            
            count++;
        }
    }
    
    return count;
}

void ObjectTracker::AddEvent(uint32_t frame, uint16_t index, ObjectEventType type, uint8_t obj_type) {
    ObjectEvent& evt = event_buffer[event_write_idx];
    
    evt.frame = frame;
    evt.object_index = index;
    evt.event_type = (uint8_t)type;
    evt.object_type = obj_type;
    evt.checksum = CalculateObjectChecksum(index);
    
    event_write_idx = (event_write_idx + 1) % EVENT_BUFFER_SIZE;
    total_events++;
}

uint32_t ObjectTracker::CalculateObjectChecksum(uint16_t index) const {
    if (index >= MAX_OBJECTS) return 0;
    
    FM2KObject* pool = (FM2KObject*)OBJECT_POOL_ADDR;
    FM2KObject* obj = &pool[index];
    
    // Simple checksum of key fields
    uint32_t checksum = obj->type;
    checksum ^= obj->position_x;
    checksum ^= obj->position_y;
    checksum ^= obj->velocity_x;
    checksum ^= obj->velocity_y;
    
    return checksum;
}

uint32_t ObjectTracker::SaveMinimalState(uint8_t* buffer, uint32_t buffer_size) const {
    if (buffer_size < sizeof(MinimalObjectState)) return 0;
    
    MinimalObjectState* state = (MinimalObjectState*)buffer;
    state->frame = frame_counter;
    state->active_count = 0;
    
    // Calculate how many objects we can fit
    uint32_t max_entries = (buffer_size - sizeof(MinimalObjectState)) / sizeof(MinimalObjectState::Entry);
    
    FM2KObject* pool = (FM2KObject*)OBJECT_POOL_ADDR;
    MinimalObjectState::Entry* entry = state->entries;
    
    for (uint32_t i = 0; i < MAX_OBJECTS && state->active_count < max_entries; i++) {
        if (IsObjectActive(i)) {
            FM2KObject* obj = &pool[i];
            
            entry->index = i;
            entry->type = obj->type;
            entry->checksum = CalculateObjectChecksum(i);
            entry->position_x = obj->position_x;
            entry->position_y = obj->position_y;
            
            entry++;
            state->active_count++;
        }
    }
    
    return state->GetSizeBytes();
}

bool ObjectTracker::RestoreMinimalState(const uint8_t* buffer, uint32_t buffer_size) {
    if (buffer_size < sizeof(MinimalObjectState)) return false;
    
    const MinimalObjectState* state = (const MinimalObjectState*)buffer;
    
    // Verify we have enough data
    if (buffer_size < state->GetSizeBytes()) return false;
    
    // Clear current tracking
    memset(active_bitmap, 0, sizeof(active_bitmap));
    
    // Restore active objects
    for (uint16_t i = 0; i < state->active_count; i++) {
        const MinimalObjectState::Entry& entry = state->entries[i];
        
        if (entry.index < MAX_OBJECTS) {
            // Mark as active in bitmap
            uint32_t word_idx = entry.index / 32;
            uint32_t bit_idx = entry.index % 32;
            active_bitmap[word_idx] |= (1u << bit_idx);
        }
    }
    
    frame_counter = state->frame;
    return true;
}

// Global functions

uint32_t SaveObjectsAdaptive(uint8_t* buffer, uint32_t buffer_size, uint32_t frame) {
    // Update tracking first
    g_object_tracker.UpdateTracking(frame);
    
    uint32_t active_count = g_object_tracker.GetActiveObjectCount();
    
    // Choose strategy based on active object count
    if (active_count < 50) {
        // For small object counts, save minimal state
        return g_object_tracker.SaveMinimalState(buffer, buffer_size);
    } else if (active_count < 200) {
        // For medium counts, save active objects with full data
        FM2KObject* pool = (FM2KObject*)OBJECT_POOL_ADDR;
        uint32_t saved = 0;
        
        // Save header
        if (buffer_size < 8) return 0;
        *(uint32_t*)buffer = 0x4F424A53; // "OBJS" magic
        *((uint32_t*)buffer + 1) = active_count;
        
        uint8_t* write_ptr = buffer + 8;
        uint32_t remaining = buffer_size - 8;
        
        for (uint32_t i = 0; i < MAX_OBJECTS; i++) {
            if (g_object_tracker.IsObjectActive(i)) {
                if (remaining < sizeof(uint16_t) + OBJECT_SIZE) break;
                
                // Save index + full object data
                *(uint16_t*)write_ptr = i;
                write_ptr += 2;
                
                memcpy(write_ptr, &pool[i], OBJECT_SIZE);
                write_ptr += OBJECT_SIZE;
                
                remaining -= (2 + OBJECT_SIZE);
                saved++;
            }
        }
        
        return buffer_size - remaining;
    } else {
        // For large counts, use compression or full pool save
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, 
                   "FM2K ObjectTracker: High object count (%u), consider full pool save", active_count);
        
        // For now, fall back to full pool
        if (buffer_size >= MAX_OBJECTS * OBJECT_SIZE) {
            FM2KObject* pool = (FM2KObject*)OBJECT_POOL_ADDR;
            memcpy(buffer, pool, MAX_OBJECTS * OBJECT_SIZE);
            return MAX_OBJECTS * OBJECT_SIZE;
        }
    }
    
    return 0;
}

bool RestoreObjectsAdaptive(const uint8_t* buffer, uint32_t buffer_size) {
    if (buffer_size < 8) return false;
    
    // Check magic header
    uint32_t magic = *(uint32_t*)buffer;
    
    if (magic == 0x4F424A53) { // "OBJS" - adaptive format
        uint32_t object_count = *((uint32_t*)buffer + 1);
        const uint8_t* read_ptr = buffer + 8;
        
        FM2KObject* pool = (FM2KObject*)OBJECT_POOL_ADDR;
        
        // Clear pool first (optional - depends on game requirements)
        // memset(pool, 0, MAX_OBJECTS * OBJECT_SIZE);
        
        // Restore each object
        for (uint32_t i = 0; i < object_count; i++) {
            uint16_t index = *(uint16_t*)read_ptr;
            read_ptr += 2;
            
            if (index < MAX_OBJECTS) {
                memcpy(&pool[index], read_ptr, OBJECT_SIZE);
            }
            
            read_ptr += OBJECT_SIZE;
        }
        
        return true;
    } else {
        // Assume full pool format
        if (buffer_size == MAX_OBJECTS * OBJECT_SIZE) {
            FM2KObject* pool = (FM2KObject*)OBJECT_POOL_ADDR;
            memcpy(pool, buffer, MAX_OBJECTS * OBJECT_SIZE);
            return true;
        }
    }
    
    return false;
}

uint32_t AnalyzeObjectTypes(uint16_t* type_counts, uint32_t max_types) {
    if (!type_counts || max_types == 0) return 0;
    
    memset(type_counts, 0, max_types * sizeof(uint16_t));
    
    FM2KObject* pool = (FM2KObject*)OBJECT_POOL_ADDR;
    uint32_t unique_types = 0;
    
    for (uint32_t i = 0; i < MAX_OBJECTS; i++) {
        if (g_object_tracker.IsObjectActive(i)) {
            uint32_t type = pool[i].type;
            
            if (type < max_types) {
                if (type_counts[type] == 0) unique_types++;
                type_counts[type]++;
            }
        }
    }
    
    return unique_types;
}

void DumpObjectPoolState(const char* filename) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== FM2K Object Pool State Dump ===");
    
    auto stats = g_object_tracker.GetStatistics();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Active Objects: %u (Peak: %u, Avg: %u)", 
               stats.current_active, stats.peak_active, stats.avg_active);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creation Rate: %u/100f, Deletion Rate: %u/100f",
               stats.creation_rate, stats.deletion_rate);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Total Created: %u, Total Deleted: %u",
               stats.total_created, stats.total_deleted);
    
    // Analyze object types
    uint16_t type_counts[256] = {0};
    uint32_t unique_types = AnalyzeObjectTypes(type_counts, 256);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Unique Object Types: %u", unique_types);
    for (uint32_t i = 0; i < 256; i++) {
        if (type_counts[i] > 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Type %u: %u objects", i, type_counts[i]);
        }
    }
    
    // List active objects
    ActiveObject active_objects[64];
    uint32_t count = g_object_tracker.GetActiveObjects(active_objects, 64);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "First %u active objects:", count);
    for (uint32_t i = 0; i < count && i < 10; i++) {
        const ActiveObject& obj = active_objects[i];
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  [%u] Type=%u, Pos=(%u,%u), Checksum=0x%08X",
                   obj.index, obj.type, obj.position_x, obj.position_y, obj.checksum);
    }
    
    if (filename) {
        // TODO: Write detailed dump to file
    }
}

} // namespace ObjectTracking
} // namespace FM2K