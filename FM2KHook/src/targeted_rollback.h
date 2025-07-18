#pragma once

#include <cstdint>
#include <vector>
#include "object_analysis.h"

namespace FM2K {
namespace TargetedRollback {

// Rollback strategy based on object type analysis
enum class SaveStrategy {
    CRITICAL_ONLY,      // Only save character and projectile objects
    CRITICAL_PLUS,      // Characters + projectiles + important effects  
    GAMEPLAY_COMPLETE   // All gameplay-relevant objects (exclude UI)
};

struct RollbackSaveSlot {
    uint32_t frame_number;
    SaveStrategy strategy_used;
    uint32_t data_size;
    uint32_t object_count;
    std::vector<uint16_t> saved_slots;
    uint8_t* data_buffer;
    uint32_t buffer_capacity;
};

class TargetedRollbackManager {
public:
    TargetedRollbackManager();
    ~TargetedRollbackManager();
    
    // Initialize with buffer allocation
    bool Initialize(uint32_t max_save_slots = 8);
    void Shutdown();
    
    // Object-type-aware saving
    uint32_t SaveGameStateTargeted(uint32_t frame, SaveStrategy strategy = SaveStrategy::CRITICAL_PLUS);
    bool LoadGameStateTargeted(uint32_t frame);
    
    // Strategy selection based on current game state
    SaveStrategy SelectOptimalStrategy() const;
    uint32_t EstimateSaveSize(SaveStrategy strategy) const;
    
    // Slot management
    uint32_t GetSlotForFrame(uint32_t frame) const { return frame % max_slots; }
    bool IsSlotValid(uint32_t slot) const;
    const RollbackSaveSlot* GetSlot(uint32_t slot) const;
    
    // Performance monitoring
    struct Performance {
        uint32_t total_saves;
        uint32_t total_loads; 
        uint32_t avg_save_time_us;
        uint32_t avg_load_time_us;
        uint32_t avg_save_size;
        uint32_t memory_peak_usage;
    };
    Performance GetPerformanceStats() const { return perf_stats; }
    
private:
    std::vector<RollbackSaveSlot> save_slots;
    uint32_t max_slots;
    Performance perf_stats;
    bool initialized;
    
    // Core save/load operations
    uint32_t SaveCriticalOnly(RollbackSaveSlot& slot, uint32_t frame);
    uint32_t SaveCriticalPlus(RollbackSaveSlot& slot, uint32_t frame);
    uint32_t SaveGameplayComplete(RollbackSaveSlot& slot, uint32_t frame);
    
    bool LoadFromSlot(const RollbackSaveSlot& slot);
    
    // Object filtering
    std::vector<uint16_t> GetObjectSlotsForStrategy(SaveStrategy strategy) const;
    bool ShouldSaveObject(const ObjectAnalysis::ObjectInfo& obj, SaveStrategy strategy) const;
    
    // Buffer management
    bool EnsureSlotCapacity(RollbackSaveSlot& slot, uint32_t required_size);
    void FreeSlotBuffer(RollbackSaveSlot& slot);
    
    // Performance tracking
    void RecordSavePerformance(uint32_t time_us, uint32_t size);
    void RecordLoadPerformance(uint32_t time_us);
};

// Global instance
extern TargetedRollbackManager g_targeted_rollback;

// Helper functions for GekkoNet integration
uint32_t SaveStateForGekkoNet(uint32_t frame);
bool LoadStateForGekkoNet(uint32_t frame);
SaveStrategy GetCurrentRollbackStrategy();

} // namespace TargetedRollback
} // namespace FM2K