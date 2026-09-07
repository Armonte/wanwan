#include "pool_delta_trace.h"

#include "../core/globals.h"

namespace fm2k::pool_delta_trace {
namespace {

// Object pool geometry -- local literals for the same reason round_events.cpp
// and seam_trace.cpp keep their own: this replicates the engine's own address
// math and must not drift onto savestate.h's shifted bases.
constexpr uintptr_t kObjectPool = 0x004701E0;
constexpr size_t    kObjStride  = 382;
constexpr size_t    kObjCount   = 1024;
constexpr ptrdiff_t kOffScriptId = 0x030;
constexpr ptrdiff_t kOffCursor   = 0x02C;
constexpr ptrdiff_t kOffWait     = 0x03C;

}  // namespace

uint32_t Note(uint32_t bitmap_out[kBitmapWords],
              uint8_t types_out[kMaxLiveTypes],
              uint16_t scripts_out[kMaxLiveTypes],
              uint32_t cursors_out[kMaxLiveTypes],
              uint16_t waits_out[kMaxLiveTypes],
              int* live_out) {
    for (int w = 0; w < kBitmapWords; ++w) bitmap_out[w] = 0;
    for (int i = 0; i < kMaxLiveTypes; ++i) {
        types_out[i] = 0; scripts_out[i] = 0;
        cursors_out[i] = 0; waits_out[i] = 0;
    }
    if (live_out) *live_out = 0;
    if constexpr (FM2K::kIsFM95) {
        return 0;
    } else {
        uint32_t digest = 2166136261u;
        int live = 0;
        for (size_t i = 0; i < kObjCount; ++i) {
            const uint32_t type =
                *(const uint32_t*)(kObjectPool + i * kObjStride);
            if (!type) continue;
            bitmap_out[i >> 5] |= (1u << (i & 31));
            if (live < kMaxLiveTypes) {
                types_out[live]   = (uint8_t)type;
                const uintptr_t o = kObjectPool + i * kObjStride;
                scripts_out[live] = (uint16_t)*(const uint32_t*)(o + kOffScriptId);
                cursors_out[live] = *(const uint32_t*)(o + kOffCursor);
                waits_out[live]   = (uint16_t)*(const uint32_t*)(o + kOffWait);
            }
            ++live;
            const uint32_t mix[2] = { (uint32_t)i, type };
            const uint8_t* p = (const uint8_t*)mix;
            for (size_t b = 0; b < sizeof(mix); ++b) {
                digest ^= p[b];
                digest *= 16777619u;
            }
        }
        if (live_out) *live_out = live;
        return digest;
    }
}

}  // namespace fm2k::pool_delta_trace
