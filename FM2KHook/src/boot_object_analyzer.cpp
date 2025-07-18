#include <cstdint>
#include <windows.h>
#include "logging.h"

namespace FM2K {
namespace BootAnalysis {

constexpr uintptr_t OBJECT_POOL_ADDR = 0x4701E0;
constexpr uint32_t OBJECT_SIZE = 382;
constexpr uint32_t MAX_OBJECTS = 1024;

struct BootObject {
    uint32_t type;          // +0x00
    uint32_t id;            // +0x04
    uint32_t field_08;      // +0x08
    uint32_t field_0C;      // +0x0C
    uint32_t field_10;      // +0x10
    uint32_t field_14;      // +0x14
    uint32_t field_18;      // +0x18
    uint32_t field_1C;      // +0x1C
    uint32_t field_20;      // +0x20
    uint32_t field_24;      // +0x24
    uint32_t field_28;      // +0x28
    uint32_t field_2C;      // +0x2C
    // Continue mapping as we discover patterns...
    uint8_t data[OBJECT_SIZE - 48];
};

void AnalyzeBootSequenceObject() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== FM2K BOOT OBJECT ANALYSIS ===");
    
    // Find active objects
    uint32_t active_count = 0;
    for (uint32_t i = 0; i < MAX_OBJECTS; i++) {
        uintptr_t obj_addr = OBJECT_POOL_ADDR + (i * OBJECT_SIZE);
        
        if (IsBadReadPtr((void*)obj_addr, OBJECT_SIZE)) continue;
        
        BootObject* obj = (BootObject*)obj_addr;
        
        // Check if object is active (type != 0)
        if (obj->type != 0) {
            active_count++;
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "ACTIVE OBJECT - Slot %d:", i);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "  Type: 0x%08X (%d)", obj->type, obj->type);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "  ID: 0x%08X (%d)", obj->id, obj->id);
            
            // Dump first 128 bytes in detail
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Raw data (first 128 bytes):");
            uint8_t* raw = (uint8_t*)obj;
            for (int row = 0; row < 8; row++) {
                char hex_str[64] = {0};
                char ascii_str[17] = {0};
                
                for (int col = 0; col < 16; col++) {
                    int idx = row * 16 + col;
                    if (idx < 128) {
                        sprintf(hex_str + (col * 3), "%02X ", raw[idx]);
                        ascii_str[col] = (raw[idx] >= 32 && raw[idx] < 127) ? raw[idx] : '.';
                    }
                }
                
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                           "    %04X: %s | %s", row * 16, hex_str, ascii_str);
            }
            
            // Look for patterns
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Pattern analysis:");
            
            // Check for pointers (values that look like addresses)
            for (int i = 0; i < 32; i++) {
                uint32_t* val_ptr = (uint32_t*)(raw + (i * 4));
                uint32_t val = *val_ptr;
                
                // Common FM2K address ranges
                if ((val >= 0x400000 && val < 0x600000) ||  // Code/data segment
                    (val >= 0x10000000 && val < 0x20000000)) { // Heap
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                               "    Possible pointer at +0x%02X: 0x%08X", i * 4, val);
                }
            }
            
            // Check for non-zero regions
            int last_nonzero = -1;
            for (int i = OBJECT_SIZE - 1; i >= 0; i--) {
                if (raw[i] != 0) {
                    last_nonzero = i;
                    break;
                }
            }
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "  Last non-zero byte at offset: 0x%02X (%d)", 
                       last_nonzero, last_nonzero);
            
            // Compare to giuroll's approach - they track specific memory regions
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "  Memory tracking approach:");
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "    - Object exists at: 0x%08X", obj_addr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "    - Size for save: %d bytes (vs full %d)", 
                       last_nonzero + 1, OBJECT_SIZE);
        }
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
               "Total active objects: %d", active_count);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
               "=== END BOOT ANALYSIS ===");
}

// Call this during different boot phases to see object evolution
void TrackBootObjectChanges(const char* phase_name) {
    static BootObject last_state = {0};
    static bool first_call = true;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
               "=== BOOT PHASE: %s ===", phase_name);
    
    // Find the active boot object (usually slot 1 during boot)
    for (uint32_t i = 0; i < 10; i++) { // Check first 10 slots
        uintptr_t obj_addr = OBJECT_POOL_ADDR + (i * OBJECT_SIZE);
        if (IsBadReadPtr((void*)obj_addr, OBJECT_SIZE)) continue;
        
        BootObject* obj = (BootObject*)obj_addr;
        if (obj->type == 0) continue;
        
        if (first_call) {
            memcpy(&last_state, obj, sizeof(BootObject));
            first_call = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "  Initial state captured for slot %d", i);
        } else {
            // Compare with last state
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                       "  Changes in slot %d since last check:", i);
            
            uint32_t* curr = (uint32_t*)obj;
            uint32_t* prev = (uint32_t*)&last_state;
            
            for (int j = 0; j < 12; j++) { // Check first 48 bytes
                if (curr[j] != prev[j]) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
                               "    +0x%02X: 0x%08X -> 0x%08X", 
                               j * 4, prev[j], curr[j]);
                }
            }
            
            memcpy(&last_state, obj, sizeof(BootObject));
        }
        
        break; // Just track first active object
    }
}

} // namespace BootAnalysis
} // namespace FM2K