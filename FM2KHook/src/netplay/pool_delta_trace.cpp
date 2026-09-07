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

}  // namespace

uint32_t Note(uint32_t bitmap_out[kBitmapWords]) {
    for (int w = 0; w < kBitmapWords; ++w) bitmap_out[w] = 0;
    if constexpr (FM2K::kIsFM95) {
        return 0;
    } else {
        uint32_t digest = 2166136261u;
        for (size_t i = 0; i < kObjCount; ++i) {
            const uint32_t type =
                *(const uint32_t*)(kObjectPool + i * kObjStride);
            if (!type) continue;
            bitmap_out[i >> 5] |= (1u << (i & 31));
            const uint32_t mix[2] = { (uint32_t)i, type };
            const uint8_t* p = (const uint8_t*)mix;
            for (size_t b = 0; b < sizeof(mix); ++b) {
                digest ^= p[b];
                digest *= 16777619u;
            }
        }
        return digest;
    }
}

}  // namespace fm2k::pool_delta_trace
