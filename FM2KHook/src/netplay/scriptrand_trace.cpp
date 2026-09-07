#include "scriptrand_trace.h"

#include "../core/globals.h"

#include <SDL3/SDL_log.h>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace fm2k::scriptrand_trace {
namespace {

// ~1170 frames at the measured ~7 draws/frame, which comfortably spans the
// approach to every desync captured so far (they land 15-17 frames after a
// visible transition). 8192 * 24 B = 192 KB.
constexpr size_t kCapacity = 8192;

struct Entry {
    uint32_t frame;
    uint16_t slot;        // object pool slot index, or 0xFFFF if out of range
    uint16_t kind;        // obj +0x15A entity kind
    uint32_t script_id;   // obj +0x030
    uint32_t cursor;      // obj +0x02C script_item_idx
    uint32_t player;      // obj +0x156 owning character slot
    uint32_t value;       // what game_rand actually returned
};

Entry  g_ring[kCapacity];
size_t g_next = 0;
size_t g_count = 0;
int    g_dumps = 0;

// Object pool geometry. Deliberately local literals rather than shared
// constants, for the same reason round_events.cpp and seam_trace.cpp keep
// their own: this replicates the engine's own address math and must not drift
// onto savestate.h's shifted bases.
constexpr uintptr_t kObjectPool     = 0x004701E0;
constexpr size_t    kObjStride      = 382;
constexpr size_t    kObjCount       = 1024;
constexpr uintptr_t kCurObjectPtr   = 0x004CFA00;   // g_css_object_ptr
constexpr ptrdiff_t kOffScriptCursor = 0x02C;
constexpr ptrdiff_t kOffScriptId     = 0x030;
constexpr ptrdiff_t kOffEntityKind   = 0x15A;
constexpr ptrdiff_t kOffPlayerSlot   = 0x156;

}  // namespace

void Note(uint32_t frame, uint32_t value) {
    if constexpr (FM2K::kIsFM95) {
        (void)frame; (void)value;
        return;
    } else {
        const uintptr_t obj = *(const uintptr_t*)kCurObjectPtr;
        // update_game_state walks the pool one slot past the end before it
        // finishes, so g_css_object_ptr is not always inside the pool. Record
        // that honestly rather than computing a nonsense slot index.
        uint16_t slot = 0xFFFF;
        uint32_t script_id = 0, cursor = 0, player = 0;
        uint16_t kind = 0;
        if (obj >= kObjectPool && obj < kObjectPool + kObjStride * kObjCount) {
            const uintptr_t off = obj - kObjectPool;
            if (off % kObjStride == 0) {
                slot      = (uint16_t)(off / kObjStride);
                cursor    = *(const uint32_t*)(obj + kOffScriptCursor);
                script_id = *(const uint32_t*)(obj + kOffScriptId);
                kind      = *(const uint16_t*)(obj + kOffEntityKind);
                player    = *(const uint32_t*)(obj + kOffPlayerSlot);
            }
        }

        Entry& e = g_ring[g_next];
        e.frame     = frame;
        e.slot      = slot;
        e.kind      = kind;
        e.script_id = script_id;
        e.cursor    = cursor;
        e.player    = player;
        e.value     = value;
        g_next = (g_next + 1) % kCapacity;
        if (g_count < kCapacity) ++g_count;
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
                      "%s\\logs\\FM2K_P%d_scriptrand_%02d%02d%02d_%d.csv",
                      dir, player, st.wHour, st.wMinute, st.wSecond, g_dumps);
        ++g_dumps;

        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "ScriptRandTrace: could not open %s", path);
            return;
        }
        std::fprintf(f, "# script random-branch draws player=%d reason=%s "
                        "desync_frame=%d rows=%zu\n",
                     player, reason ? reason : "", desync_frame, g_count);
        std::fprintf(f, "# One row per gameplay game_rand draw at 0x4139A3, in "
                        "order. Diff against the peer's copy: the first row "
                        "where the two files disagree names the object slot, "
                        "script and instruction where the sims stopped running "
                        "the same code. Rows repeat across rollback "
                        "re-simulation, so compare the SEQUENCE, not row "
                        "numbers.\n");
        std::fprintf(f, "frame,slot,kind,player,script_id,cursor,value\n");

        const size_t start = (g_count == kCapacity) ? g_next : 0;
        for (size_t i = 0; i < g_count; ++i) {
            const Entry& e = g_ring[(start + i) % kCapacity];
            std::fprintf(f, "%u,%d,%u,%u,%u,%u,%u\n", e.frame,
                         e.slot == 0xFFFF ? -1 : (int)e.slot,
                         e.kind, e.player, e.script_id, e.cursor, e.value);
        }
        std::fclose(f);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "ScriptRandTrace: wrote %zu draws to %s", g_count, path);
    }
}

}  // namespace fm2k::scriptrand_trace
