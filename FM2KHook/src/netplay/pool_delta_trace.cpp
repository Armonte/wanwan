#include "pool_delta_trace.h"

#include "../core/globals.h"

#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace fm2k::pool_delta_trace {
namespace {

// Object pool geometry -- local literals for the same reason round_events.cpp
// and seam_trace.cpp keep their own: this replicates the engine's own address
// math and must not drift onto savestate.h's shifted bases.
constexpr uintptr_t kObjectPool = 0x004701E0;
constexpr size_t    kObjStride  = 382;
constexpr size_t    kObjCount   = 1024;
constexpr uintptr_t kCurObjPtr  = 0x004CFA00;   // g_css_object_ptr
constexpr uintptr_t kClearObjectReferences = 0x0040E4A0;

struct Retire { uint32_t frame; uint32_t ra; uint16_t slot; uint16_t type; };
constexpr size_t kRetireCap = 2048;
Retire   g_ret[kRetireCap];
size_t   g_ret_next = 0, g_ret_count = 0;
int      g_ret_dumps = 0;
uint32_t g_ret_frame = 0;

using ClearObjRefsFn = void(__cdecl*)();
ClearObjRefsFn g_orig_clear_obj_refs = nullptr;

void __cdecl Hook_ClearObjectReferences() {
    // Read the victim BEFORE forwarding -- the original sets type = 1, so the
    // type recorded here is the one it is being retired FROM.
    const uintptr_t obj = *(const uintptr_t*)kCurObjPtr;
    if (obj >= kObjectPool && obj < kObjectPool + kObjStride * kObjCount) {
        const uintptr_t off = obj - kObjectPool;
        if (off % kObjStride == 0) {
            Retire& r = g_ret[g_ret_next];
            r.frame = g_ret_frame;
            r.ra    = (uint32_t)(uintptr_t)__builtin_return_address(0);
            r.slot  = (uint16_t)(off / kObjStride);
            r.type  = (uint16_t)*(const uint32_t*)obj;
            g_ret_next = (g_ret_next + 1) % kRetireCap;
            if (g_ret_count < kRetireCap) ++g_ret_count;
        }
    }
    if (g_orig_clear_obj_refs) g_orig_clear_obj_refs();
}
constexpr ptrdiff_t kOffScriptId = 0x030;
constexpr ptrdiff_t kOffCursor   = 0x02C;
constexpr ptrdiff_t kOffWait     = 0x03C;
constexpr ptrdiff_t kOffPosX     = 0x008;
constexpr ptrdiff_t kOffPosY     = 0x00C;

}  // namespace

uint32_t Note(uint32_t bitmap_out[kBitmapWords],
              uint8_t types_out[kMaxLiveTypes],
              uint16_t scripts_out[kMaxLiveTypes],
              uint32_t cursors_out[kMaxLiveTypes],
              uint16_t waits_out[kMaxLiveTypes],
              int32_t xs_out[kMaxLiveTypes],
              int32_t ys_out[kMaxLiveTypes],
              int* live_out) {
    for (int w = 0; w < kBitmapWords; ++w) bitmap_out[w] = 0;
    for (int i = 0; i < kMaxLiveTypes; ++i) {
        types_out[i] = 0; scripts_out[i] = 0;
        cursors_out[i] = 0; waits_out[i] = 0;
        xs_out[i] = 0; ys_out[i] = 0;
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
                xs_out[live]      = *(const int32_t*)(o + kOffPosX);
                ys_out[live]      = *(const int32_t*)(o + kOffPosY);
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

void SetRetireFrame(uint32_t frame) { g_ret_frame = frame; }

bool InstallRetireHook() {
    if constexpr (FM2K::kIsFM95) return true;
    if (MH_CreateHook((void*)kClearObjectReferences,
                      (void*)Hook_ClearObjectReferences,
                      (void**)&g_orig_clear_obj_refs) != MH_OK ||
        MH_QueueEnableHook((void*)kClearObjectReferences) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PoolDeltaTrace: failed to hook ClearObjectReferences");
        return false;
    }
    return true;
}

void DumpRetires(const char* reason, int32_t desync_frame) {
    if constexpr (FM2K::kIsFM95) { (void)reason; (void)desync_frame; return; }
    if (g_ret_count == 0) return;
    char dir[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = std::strrchr(dir, '\\');
    if (slash) *slash = '\0';
    SYSTEMTIME st = {}; GetLocalTime(&st);
    char path[MAX_PATH + 64];
    std::snprintf(path, sizeof(path), "%s\\logs\\FM2K_P%d_retire_%02d%02d%02d_%d.csv",
                  dir, (int)g_player_index + 1, st.wHour, st.wMinute, st.wSecond,
                  g_ret_dumps);
    ++g_ret_dumps;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "# object retirements player=%d reason=%s desync_frame=%d rows=%zu\n",
                 (int)g_player_index + 1, reason ? reason : "", desync_frame, g_ret_count);
    std::fprintf(f, "# ra names the ClearObjectReferences call site: 0x4124F5 / "
                    "0x412510 / 0x412756 are normal script paths; 0x413BCB is the "
                    "opcode dispatcher's DEFAULT case (ScriptMainLoopError).\n");
    std::fprintf(f, "frame,slot,type_before,ra\n");
    const size_t start = (g_ret_count == kRetireCap) ? g_ret_next : 0;
    for (size_t i = 0; i < g_ret_count; ++i) {
        const Retire& r = g_ret[(start + i) % kRetireCap];
        std::fprintf(f, "%u,%u,%u,0x%06X\n", r.frame, r.slot, r.type, r.ra);
    }
    std::fclose(f);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "PoolDeltaTrace: wrote %zu retirements to %s", g_ret_count, path);
}

}  // namespace fm2k::pool_delta_trace
