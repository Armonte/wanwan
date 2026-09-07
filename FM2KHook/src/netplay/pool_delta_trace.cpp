#include "pool_delta_trace.h"

#include "../core/globals.h"

#include <SDL3/SDL_log.h>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace fm2k::pool_delta_trace {
namespace {

// Spawns and despawns are rare (the pool sits at ~96-98 live objects for whole
// frames at a time), so this covers an entire match. 4096 * 12 B = 48 KB.
constexpr size_t kCapacity = 4096;

struct Event {
    uint32_t frame;
    uint16_t slot;
    uint16_t type;      // object type at +0x000; 0 means it went away
    uint8_t  added;     // 1 = appeared, 0 = disappeared
};

Event  g_ring[kCapacity];
size_t g_next = 0;
size_t g_count = 0;
int    g_dumps = 0;

// Object pool geometry -- local literals for the same reason round_events.cpp
// and seam_trace.cpp keep their own: this replicates the engine's own address
// math and must not drift onto savestate.h's shifted bases.
constexpr uintptr_t kObjectPool = 0x004701E0;
constexpr size_t    kObjStride  = 382;
constexpr size_t    kObjCount   = 1024;

constexpr size_t kWords = kObjCount / 32;
uint32_t g_live[kWords];      // previous call's live bitmap
uint16_t g_type[kObjCount];   // previous call's types, to report what left
bool     g_primed = false;

}  // namespace

uint32_t Note(uint32_t frame) {
    if constexpr (FM2K::kIsFM95) {
        (void)frame;
        return 0;
    } else {
        uint32_t live[kWords] = {};
        uint16_t types[kObjCount];
        uint32_t digest = 2166136261u;

        // ONE pass. The types are captured here rather than re-walked after the
        // diff: this runs on every gekko Save, and the host saves ~9 times per
        // frame under rollback, so a second 1024-slot walk over the 391 KB pool
        // would double the cost of an instrument watching a timing-sensitive
        // bug.
        for (size_t i = 0; i < kObjCount; ++i) {
            const uint32_t type =
                *(const uint32_t*)(kObjectPool + i * kObjStride);
            types[i] = (uint16_t)type;
            if (!type) continue;
            live[i >> 5] |= (1u << (i & 31));
            const uint32_t mix[2] = { (uint32_t)i, type };
            const uint8_t* p = (const uint8_t*)mix;
            for (size_t b = 0; b < sizeof(mix); ++b) {
                digest ^= p[b];
                digest *= 16777619u;
            }
        }

        // First call establishes the baseline; every slot would otherwise look
        // like a spawn and bury the real events.
        if (!g_primed) {
            g_primed = true;
        } else {
            for (size_t w = 0; w < kWords; ++w) {
                uint32_t changed = live[w] ^ g_live[w];
                while (changed) {
                    const int bit = __builtin_ctz(changed);
                    changed &= changed - 1;
                    const size_t slot = w * 32 + bit;
                    const bool added = (live[w] >> bit) & 1u;
                    Event& e = g_ring[g_next];
                    e.frame = frame;
                    e.slot  = (uint16_t)slot;
                    e.added = added ? 1 : 0;
                    e.type  = added ? types[slot]
                                    : g_type[slot];  // what it WAS: the useful half
                    g_next = (g_next + 1) % kCapacity;
                    if (g_count < kCapacity) ++g_count;
                }
            }
        }

        std::memcpy(g_live, live, sizeof(live));
        std::memcpy(g_type, types, sizeof(types));
        return digest;
    }
}

void Dump(const char* reason, int32_t desync_frame) {
    if constexpr (FM2K::kIsFM95) {
        (void)reason; (void)desync_frame;
        return;
    } else {
        if (g_count == 0) return;

        char dir[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        char* slash = std::strrchr(dir, '\\');
        if (slash) *slash = '\0';

        SYSTEMTIME st = {};
        GetLocalTime(&st);
        const int player = (int)g_player_index + 1;
        char path[MAX_PATH + 64];
        std::snprintf(path, sizeof(path),
                      "%s\\logs\\FM2K_P%d_pooldelta_%02d%02d%02d_%d.csv",
                      dir, player, st.wHour, st.wMinute, st.wSecond, g_dumps);
        ++g_dumps;

        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "PoolDeltaTrace: could not open %s", path);
            return;
        }
        std::fprintf(f, "# object spawn/despawn history player=%d reason=%s "
                        "desync_frame=%d rows=%zu\n",
                     player, reason ? reason : "", desync_frame, g_count);
        std::fprintf(f, "# One row per slot that appeared or disappeared, "
                        "sampled at every gekko Save. Rollback re-simulation "
                        "re-emits events for a replayed frame, so compare the "
                        "per-frame SETS against the peer's file, not row "
                        "numbers. `type` is the object type -- on a despawn it "
                        "is the type the slot HELD, which is the useful half.\n");
        std::fprintf(f, "frame,slot,type,added\n");

        const size_t start = (g_count == kCapacity) ? g_next : 0;
        for (size_t i = 0; i < g_count; ++i) {
            const Event& e = g_ring[(start + i) % kCapacity];
            std::fprintf(f, "%u,%u,%u,%u\n", e.frame, e.slot, e.type, e.added);
        }
        std::fclose(f);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PoolDeltaTrace: wrote %zu events to %s", g_count, path);
    }
}

}  // namespace fm2k::pool_delta_trace
