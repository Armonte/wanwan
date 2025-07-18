#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace FM2K {
namespace ObjectAnalysis {

// Enhanced object structure with known field mappings
struct DetailedObject {
    uint32_t type;           // +0x00: Object type (0=inactive, 1=system, 4=character, etc)
    uint32_t id;             // +0x04: Object ID/handle  
    uint32_t position_x;     // +0x08: X coordinate
    uint32_t position_y;     // +0x0C: Y coordinate
    uint32_t velocity_x;     // +0x10: X velocity
    uint32_t velocity_y;     // +0x14: Y velocity
    uint32_t unknown_18;     // +0x18
    uint32_t unknown_1C;     // +0x1C
    uint32_t unknown_20;     // +0x20
    uint32_t unknown_24;     // +0x24
    uint32_t animation_ptr;  // +0x28: Pointer to animation data?
    uint32_t state_flags;    // +0x2C: State/flags field
    // ... rest of 382-byte structure
    uint8_t remaining_data[382 - 48];
};

// Object type classifications
enum class ObjectType : uint32_t {
    INACTIVE = 0,
    SYSTEM = 1,
    UNKNOWN_2 = 2,
    UNKNOWN_3 = 3, 
    CHARACTER = 4,
    PROJECTILE = 5,
    EFFECT = 6,
    UI_ELEMENT = 7
};

// Object classification for rollback
enum class RollbackImportance {
    ROLLBACK_CRITICAL,    // Must save for rollback (characters, projectiles)
    ROLLBACK_IMPORTANT,   // Should save for consistency (effects, some system objects)
    ROLLBACK_OPTIONAL,    // Can skip for performance (UI, static objects)
    ROLLBACK_IGNORE       // Never save (inactive slots)
};

struct ObjectInfo {
    uint16_t slot_index;
    ObjectType type;
    RollbackImportance importance;
    uint32_t position_x;
    uint32_t position_y;
    uint32_t checksum;
    bool is_character;
    bool has_position;
    bool has_animation;
    std::string description;
};

// Comprehensive object pool analysis
class ObjectPoolAnalyzer {
public:
    void AnalyzeCurrentPool();
    std::vector<ObjectInfo> GetActiveObjects() const;
    std::vector<ObjectInfo> GetCharacterObjects() const;
    std::vector<ObjectInfo> GetCriticalObjects() const;
    
    // Object type analysis
    uint32_t CountObjectsByType(ObjectType type) const;
    std::string GetObjectTypeDescription(ObjectType type) const;
    RollbackImportance GetRollbackImportance(ObjectType type) const;
    
    // Rollback strategy recommendations
    uint32_t EstimateOptimalSaveSize() const;
    std::vector<uint16_t> GetCriticalObjectSlots() const;
    void PrintDetailedAnalysis() const;
    
private:
    std::vector<ObjectInfo> active_objects;
    uint32_t total_objects_scanned;
    uint32_t characters_found;
    uint32_t critical_objects_found;
    
    DetailedObject ReadObjectAtSlot(uint16_t slot) const;
    ObjectInfo AnalyzeObject(uint16_t slot, const DetailedObject& obj) const;
    bool IsCharacterObject(const DetailedObject& obj) const;
    std::string DescribeObject(const DetailedObject& obj) const;
};

// Global analyzer instance
extern ObjectPoolAnalyzer g_pool_analyzer;

// Analysis functions
void DumpDetailedObjectAnalysis();
std::vector<uint16_t> GetCharacterObjectSlots();
uint32_t GetOptimalRollbackSaveSize();

} // namespace ObjectAnalysis
} // namespace FM2K