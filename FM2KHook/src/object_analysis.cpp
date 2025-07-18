#include "object_analysis.h"
#include <SDL3/SDL.h>
#include <windows.h>

namespace FM2K {
namespace ObjectAnalysis {

constexpr uintptr_t OBJECT_POOL_ADDR = 0x4701E0;
constexpr uint32_t MAX_OBJECTS = 1024;
constexpr uint32_t OBJECT_SIZE = 382;

// Global analyzer instance
ObjectPoolAnalyzer g_pool_analyzer;

DetailedObject ObjectPoolAnalyzer::ReadObjectAtSlot(uint16_t slot) const {
    DetailedObject obj = {};
    
    if (slot >= MAX_OBJECTS) return obj;
    
    uintptr_t object_addr = OBJECT_POOL_ADDR + (slot * OBJECT_SIZE);
    
    if (IsBadReadPtr((void*)object_addr, OBJECT_SIZE)) {
        return obj;
    }
    
    // Read the structured object data
    memcpy(&obj, (void*)object_addr, sizeof(DetailedObject));
    
    return obj;
}

ObjectInfo ObjectPoolAnalyzer::AnalyzeObject(uint16_t slot, const DetailedObject& obj) const {
    ObjectInfo info = {};
    info.slot_index = slot;
    info.type = static_cast<ObjectType>(obj.type);
    info.position_x = obj.position_x;
    info.position_y = obj.position_y;
    
    // Calculate simple checksum
    info.checksum = obj.type ^ obj.position_x ^ obj.position_y ^ obj.velocity_x ^ obj.velocity_y;
    
    // Classify object
    info.is_character = IsCharacterObject(obj);
    info.has_position = (obj.position_x != 0 || obj.position_y != 0);
    info.has_animation = (obj.animation_ptr != 0 && obj.animation_ptr != 0xFFFFFFFF);
    
    // Determine rollback importance
    info.importance = GetRollbackImportance(info.type);
    
    // Generate description
    info.description = DescribeObject(obj);
    
    return info;
}

bool ObjectPoolAnalyzer::IsCharacterObject(const DetailedObject& obj) const {
    // Character objects have type 4 and specific characteristics
    if (obj.type != 4) return false;
    
    // Characters typically have:
    // - Non-zero positions (in world coordinates)
    // - Animation pointers
    // - State flags
    // - Velocity data
    
    bool has_reasonable_position = (obj.position_x < 10000 && obj.position_y < 10000);
    bool has_state_data = (obj.state_flags != 0);
    bool has_non_zero_data = (obj.velocity_x != 0 || obj.velocity_y != 0 || obj.position_x != 0);
    
    return has_reasonable_position && (has_state_data || has_non_zero_data);
}

std::string ObjectPoolAnalyzer::DescribeObject(const DetailedObject& obj) const {
    std::string desc;
    
    switch (obj.type) {
        case 0:
            desc = "INACTIVE";
            break;
        case 1:
            desc = "SYSTEM";
            if (obj.position_x != 0 || obj.position_y != 0) {
                desc += " (positioned)";
            }
            break;
        case 4:
            desc = "CHARACTER";
            if (IsCharacterObject(obj)) {
                desc += " (player/fighter)";
            } else {
                desc += " (inactive/template)";
            }
            break;
        case 5:
            desc = "PROJECTILE/ATTACK";
            break;
        case 6:
            desc = "VISUAL_EFFECT";
            break;
        default:
            desc = "UNKNOWN_TYPE_" + std::to_string(obj.type);
            break;
    }
    
    // Add position info for positioned objects
    if (obj.position_x != 0 || obj.position_y != 0) {
        desc += " @(" + std::to_string(obj.position_x) + "," + std::to_string(obj.position_y) + ")";
    }
    
    // Add animation info
    if (obj.animation_ptr != 0 && obj.animation_ptr != 0xFFFFFFFF) {
        desc += " [anim:0x" + std::to_string(obj.animation_ptr) + "]";
    }
    
    return desc;
}

RollbackImportance ObjectPoolAnalyzer::GetRollbackImportance(ObjectType type) const {
    switch (type) {
        case ObjectType::INACTIVE:
            return RollbackImportance::ROLLBACK_IGNORE;
            
        case ObjectType::CHARACTER:
            return RollbackImportance::ROLLBACK_CRITICAL;
            
        case ObjectType::PROJECTILE:
            return RollbackImportance::ROLLBACK_CRITICAL;
            
        case ObjectType::EFFECT:
            return RollbackImportance::ROLLBACK_IMPORTANT;
            
        case ObjectType::SYSTEM:
            return RollbackImportance::ROLLBACK_IMPORTANT;
            
        case ObjectType::UI_ELEMENT:
            return RollbackImportance::ROLLBACK_OPTIONAL;
            
        default:
            return RollbackImportance::ROLLBACK_IMPORTANT; // Conservative default
    }
}

std::string ObjectPoolAnalyzer::GetObjectTypeDescription(ObjectType type) const {
    switch (type) {
        case ObjectType::INACTIVE: return "Inactive/Empty";
        case ObjectType::SYSTEM: return "System Object";
        case ObjectType::CHARACTER: return "Character/Fighter";
        case ObjectType::PROJECTILE: return "Projectile/Attack";
        case ObjectType::EFFECT: return "Visual Effect";
        case ObjectType::UI_ELEMENT: return "UI Element";
        default: return "Unknown Type";
    }
}

void ObjectPoolAnalyzer::AnalyzeCurrentPool() {
    active_objects.clear();
    total_objects_scanned = 0;
    characters_found = 0;
    critical_objects_found = 0;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== DETAILED OBJECT POOL ANALYSIS ===");
    
    for (uint16_t slot = 0; slot < MAX_OBJECTS; slot++) {
        DetailedObject obj = ReadObjectAtSlot(slot);
        total_objects_scanned++;
        
        // Only analyze active objects
        if (obj.type != 0) {
            ObjectInfo info = AnalyzeObject(slot, obj);
            active_objects.push_back(info);
            
            if (info.is_character) {
                characters_found++;
            }
            
            if (info.importance == RollbackImportance::ROLLBACK_CRITICAL) {
                critical_objects_found++;
            }
            
            // Log detailed info for first 20 active objects
            if (active_objects.size() <= 20) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                           "SLOT %3d: Type=%d (%s) Pos=(%d,%d) Vel=(%d,%d) Checksum=0x%08X - %s",
                           slot, obj.type, GetObjectTypeDescription(info.type).c_str(),
                           obj.position_x, obj.position_y, obj.velocity_x, obj.velocity_y,
                           info.checksum, info.description.c_str());
            }
        }
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ANALYSIS COMPLETE: %zu active objects, %d characters, %d critical objects",
               active_objects.size(), characters_found, critical_objects_found);
}

std::vector<ObjectInfo> ObjectPoolAnalyzer::GetActiveObjects() const {
    return active_objects;
}

std::vector<ObjectInfo> ObjectPoolAnalyzer::GetCharacterObjects() const {
    std::vector<ObjectInfo> characters;
    for (const auto& obj : active_objects) {
        if (obj.is_character) {
            characters.push_back(obj);
        }
    }
    return characters;
}

std::vector<ObjectInfo> ObjectPoolAnalyzer::GetCriticalObjects() const {
    std::vector<ObjectInfo> critical;
    for (const auto& obj : active_objects) {
        if (obj.importance == RollbackImportance::ROLLBACK_CRITICAL) {
            critical.push_back(obj);
        }
    }
    return critical;
}

uint32_t ObjectPoolAnalyzer::CountObjectsByType(ObjectType type) const {
    uint32_t count = 0;
    for (const auto& obj : active_objects) {
        if (obj.type == type) {
            count++;
        }
    }
    return count;
}

uint32_t ObjectPoolAnalyzer::EstimateOptimalSaveSize() const {
    uint32_t critical_objects = GetCriticalObjects().size();
    uint32_t important_objects = 0;
    
    for (const auto& obj : active_objects) {
        if (obj.importance == RollbackImportance::ROLLBACK_IMPORTANT) {
            important_objects++;
        }
    }
    
    // Estimate: Critical objects = full save, Important = partial save
    uint32_t estimated_size = (critical_objects * OBJECT_SIZE) + (important_objects * 64);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
               "ROLLBACK ESTIMATION: %d critical + %d important objects = ~%d bytes (vs %d full pool)",
               critical_objects, important_objects, estimated_size, MAX_OBJECTS * OBJECT_SIZE);
    
    return estimated_size;
}

std::vector<uint16_t> ObjectPoolAnalyzer::GetCriticalObjectSlots() const {
    std::vector<uint16_t> slots;
    for (const auto& obj : active_objects) {
        if (obj.importance == RollbackImportance::ROLLBACK_CRITICAL) {
            slots.push_back(obj.slot_index);
        }
    }
    return slots;
}

void ObjectPoolAnalyzer::PrintDetailedAnalysis() const {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== ROLLBACK STRATEGY ANALYSIS ===");
    
    auto characters = GetCharacterObjects();
    auto critical = GetCriticalObjects();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CHARACTER OBJECTS (%zu found):", characters.size());
    for (const auto& ch : characters) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Slot %d: %s", ch.slot_index, ch.description.c_str());
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CRITICAL OBJECTS (%zu found):", critical.size());
    for (const auto& cr : critical) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Slot %d: %s", cr.slot_index, cr.description.c_str());
    }
    
    uint32_t optimal_size = EstimateOptimalSaveSize();
    float reduction_percent = 100.0f * (1.0f - (float)optimal_size / (MAX_OBJECTS * OBJECT_SIZE));
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "OPTIMIZATION: %.1f%% memory reduction possible", reduction_percent);
}

// Global functions
void DumpDetailedObjectAnalysis() {
    g_pool_analyzer.AnalyzeCurrentPool();
    g_pool_analyzer.PrintDetailedAnalysis();
}

std::vector<uint16_t> GetCharacterObjectSlots() {
    auto characters = g_pool_analyzer.GetCharacterObjects();
    std::vector<uint16_t> slots;
    for (const auto& ch : characters) {
        slots.push_back(ch.slot_index);
    }
    return slots;
}

uint32_t GetOptimalRollbackSaveSize() {
    return g_pool_analyzer.EstimateOptimalSaveSize();
}

} // namespace ObjectAnalysis
} // namespace FM2K