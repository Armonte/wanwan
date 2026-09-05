// scene_frame_guard.cpp -- see scene_frame_guard.h for the full mechanism.
//
// Two things live here and they are deliberately separate:
//   * the DETECTOR (FM2K_FRAMEIDX_DETECT=1) measures out-of-range frame
//     indices, faulting or not, and names the object that caused one;
//   * the FIX suppresses the draw of DESTROYED objects (type 1), which is
//     the state the stale index always arrives in.
// The detector runs first so a run can tell us whether every out-of-range
// draw really is a destroyed object. If one ever is not, the log says so in
// as many words and the fix is incomplete.
#include "scene_frame_guard.h"

#include "../core/globals.h"

#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdlib>
#include <cstring>

namespace {

// --- engine layout (FM2K) --------------------------------------------------
constexpr uintptr_t kCurrentObject = 0x4CFA00;  // g_current_object

constexpr uintptr_t kBlockKgt   = 0x433240;  // kind 2 -- .kgt game system
constexpr uintptr_t kBlockDemo  = 0x425A60;  // kind 3 -- .demo scene
constexpr uintptr_t kBlockStage = 0x445740;  // kind 4 -- .stage
constexpr uintptr_t kBlockChar0 = 0x4D1D80;  // kinds 0/1/5, stride 0xE03F
constexpr uintptr_t kCharStride = 0xE03F;

constexpr uintptr_t kOffAnimTable  = 0x110;   // 39-byte entries
constexpr uintptr_t kOffFrameTable = 0x114;   // 16-byte entries
constexpr uintptr_t kOffAnimCount  = 0x2228;

constexpr uintptr_t kObjType   = 0x00;    // 0 free, 1 destroyed (Obj_ReapMarkedSlot)
constexpr uintptr_t kObjPrio   = 0x04;    // update/render bucket
constexpr uintptr_t kObjMode   = 0x10;    // -1 = the sprite path
constexpr uintptr_t kObjFrame  = 0x2C;    // 1-based frame index
constexpr uintptr_t kObjAnim   = 0x30;    // animation id it was spawned from
constexpr uintptr_t kObjCharIx = 0x156;
constexpr uintptr_t kObjKind   = 0x15A;

constexpr int32_t kObjTypeDestroyed = 1;
constexpr size_t  kAnimEntrySize    = 39;
constexpr size_t  kAnimStartOffset  = 32;   // u16 start frame
constexpr size_t  kFrameEntrySize   = 16;

bool EnvIsOne(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '1' && v[1] == '\0';
}

// Kill switch for the fix, default OFF (i.e. the fix is ON).
// FM2K_RENDER_DEAD_OBJECTS=1 restores the vanilla behaviour byte-for-byte so
// the crash can be reproduced on demand. Never ship it on.
// SUPPRESSION IS OFF BY DEFAULT, and that is a deliberate retreat.
//
// Refusing these draws removes a real crash: two to seven out-of-range
// descriptor reads happen on every process launch and roughly one in four
// overshoots lands on an unmapped page. But it also breaks three spectator
// stages of the gate, measured as a clean A/B on one quiet box with the same
// binaries and one variable: suppression ON gives 12 pass 3 fail
// (multi-match, css-gate-selftest, deep-join), suppression OFF gives 15 pass
// 0 fail. Narrowing the refusal from "every destroyed object" to "destroyed
// AND actually out of range" reduced it but did not remove it.
//
// That result says sprite_rendering_engine is NOT the pure display path we
// assumed -- skipping a draw changes something a spectator can observe. Until
// we know what, shipping the suppression trades a rare crash for a
// reproducible spectator regression, which is the worse side of the trade:
// the crash was 2 of 48 in the field corpus, while spectating is on every
// match somebody watches.
//
// So the DETECTOR ships and the refusal does not. FM2K_SUPPRESS_DEAD_DRAWS=1
// turns the fix on for whoever picks this up; the numbers above are the
// experiment to beat. Everything else about the diagnosis stands and is
// written up in docs/dev/crash_40dd62_2026-09-04.md.
bool RenderDeadObjects() {
    static const bool s_suppress = EnvIsOne("FM2K_SUPPRESS_DEAD_DRAWS");
    return !s_suppress;
}

// --- counters --------------------------------------------------------------
uint64_t g_draws_seen        = 0;
uint64_t g_dead_suppressed   = 0;
uint64_t g_bad_kind          = 0;
uint64_t g_oob_total         = 0;   // out-of-range frame index, any object
uint64_t g_oob_dead          = 0;   // ... on a destroyed object (the known bug)
uint64_t g_oob_live          = 0;   // ... on a LIVE object (would mean more)
uint64_t g_oob_would_fault   = 0;   // ... where the descriptor leaves the block
uint64_t g_oob_allowed       = 0;   // ... that the fix did NOT stop (kill switch on)
uint32_t g_reports           = 0;

// GlobalSize is a heap lock, so cache it per frame-table pointer. Eight slots
// covers the four block kinds plus a couple of character slots; a miss just
// re-queries.
struct SizeCacheEntry { const void* ptr; size_t bytes; };
SizeCacheEntry g_size_cache[8] = {};
uint32_t g_size_cache_cursor = 0;

size_t CachedGlobalSize(const void* p) {
    for (const SizeCacheEntry& e : g_size_cache) {
        if (e.ptr == p) return e.bytes;
    }
    const size_t bytes = (size_t)GlobalSize((HGLOBAL)p);
    SizeCacheEntry& slot = g_size_cache[g_size_cache_cursor++ & 7u];
    slot.ptr = p;
    slot.bytes = bytes;
    return bytes;
}

const char* KindName(int32_t kind) {
    switch (kind) {
        case 0: case 1: case 5: return "character";
        case 2: return "kgt";
        case 3: return "demo";
        case 4: return "stage";
        default: return "INVALID";
    }
}

// Returns 0 ONLY when the kind itself selects no block, i.e. kind > 5 (the
// engine's own switch default leaves its table register UNINITIALISED there --
// see ShouldSuppressDraw). A character kind with an implausible character
// index returns kNoBoundsCheck instead of 0: the engine computes that address
// without a clamp, so refusing the draw would be a behaviour change on a path
// the engine considers legal. We just decline to bounds-check it.
constexpr uintptr_t kNoBoundsCheck = 1;

uintptr_t BlockForKind(int32_t kind, int32_t char_index) {
    switch (kind) {
        case 0: case 1: case 5:
            if (char_index < 0 || char_index > 16) return kNoBoundsCheck;
            return kBlockChar0 + kCharStride * (uintptr_t)char_index;
        case 2: return kBlockKgt;
        case 3: return kBlockDemo;
        case 4: return kBlockStage;
        default: return 0;
    }
}

// The frame count is not stored anywhere. It is recoverable because the
// animation table's LAST entry is a sentinel: every walker in the binary
// (demo_spawn_scene_objects @0x406790, kgt_spawn_system_objects @0x406610)
// iterates i in [1, count-1) and treats anim[i+1].start as anim[i]'s end,
// so anim[count-1].start IS the total frame count. Returns -1 when the
// tables are not loaded.
int32_t LogicalFrameCount(uintptr_t block) {
    const uint8_t* anim = *(const uint8_t* const volatile*)(block + kOffAnimTable);
    const int32_t count = *(const volatile int32_t*)(block + kOffAnimCount);
    if (!anim || count <= 0 || count > 0x400) return -1;
    return (int32_t)*(const uint16_t*)(anim + kAnimEntrySize * (size_t)(count - 1)
                                       + kAnimStartOffset);
}

void ReportOutOfRange(uintptr_t obj, int32_t kind, uintptr_t block,
                      int32_t frame_idx, const uint8_t* table,
                      int32_t logical_frames, size_t block_bytes,
                      bool would_fault, bool suppressed) {
    // Loud, but bounded: hook logging is synchronous, and a broken title can
    // hit this on every object of every frame.
    if (g_reports >= 16 && (g_oob_total % 1000) != 0) return;
    ++g_reports;

    const int32_t type = *(const volatile int32_t*)(obj + kObjType);
    const uintptr_t descriptor =
        (uintptr_t)table + kFrameEntrySize * (uintptr_t)frame_idx - kFrameEntrySize;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[FRAMEIDX] OUT OF RANGE #%llu: obj=0x%08X type=%d prio=%d kind=%d (%s) "
        "anim=%d frame_idx=%d valid=1..%d -- table=0x%08X block=0x%08X "
        "alloc=%llu bytes (%lld frames) descriptor=0x%08X %s. %s %s",
        (unsigned long long)g_oob_total, (unsigned)obj, (int)type,
        (int)*(const volatile int32_t*)(obj + kObjPrio), (int)kind, KindName(kind),
        (int)*(const volatile int32_t*)(obj + kObjAnim), (int)frame_idx,
        (int)logical_frames, (unsigned)(uintptr_t)table, (unsigned)block,
        (unsigned long long)block_bytes,
        (long long)(block_bytes / kFrameEntrySize), (unsigned)descriptor,
        would_fault ? "PAST THE ALLOCATION (this is the 0x40DD62 fault)"
                    : "inside the allocation (silent bad read)",
        type == kObjTypeDestroyed
            ? "object is DESTROYED (type 1) -- the known stale-scene-table bug"
            : "object is LIVE -- NOT the known bug, the dead-object fix does "
              "NOT cover this",
        suppressed ? "DRAW SUPPRESSED, the descriptor is never read."
                   : "DRAW ALLOWED -- the engine reads that descriptor next "
                     "(FM2K_RENDER_DEAD_OBJECTS=1 or a LIVE object).");
}

}  // namespace

namespace SceneFrameGuard {

bool DetectorArmed() {
    static const bool s_on = EnvIsOne("FM2K_FRAMEIDX_DETECT");
    return s_on;
}

// True when this object's frame index falls outside the scene table it would
// be read from. `block` is the caller's already-resolved scene block; a
// kNoBoundsCheck block means we could not resolve a table and must not guess.
bool FrameIndexOutOfRange(uintptr_t obj, uintptr_t block) {
    if (block == kNoBoundsCheck || block == 0) return false;
    const uint8_t* table = *(const uint8_t* const volatile*)(block + kOffFrameTable);
    if (!table) return false;
    const int32_t frame_idx = *(const volatile int32_t*)(obj + kObjFrame);
    const int32_t logical = LogicalFrameCount(block);
    const size_t  bytes   = CachedGlobalSize(table);
    const int32_t hard    = (int32_t)(bytes / kFrameEntrySize);
    const int32_t limit   = (logical > 0) ? logical : hard;
    if (limit <= 0) return false;          // nothing to bound against
    return frame_idx < 1 || frame_idx > limit;
}

bool ShouldSuppressDraw() {
    if constexpr (!FM2K::kIsFM2K) {
        return false;
    } else {
        const uintptr_t obj = *(const volatile uintptr_t*)kCurrentObject;
        if (!obj) return false;
        // Not the sprite path: obj[+0x10] holds a render_mode the engine
        // handles in its own switch and none of it touches a frame table.
        if (*(const volatile int32_t*)(obj + kObjMode) != -1) return false;

        ++g_draws_seen;
        const int32_t kind = *(const volatile int32_t*)(obj + kObjKind);
        const int32_t type = *(const volatile int32_t*)(obj + kObjType);

        // Kind outside 0..5 falls through the engine's switch WITHOUT writing
        // its table register (0x40CC52 -> def_40CC54 reads an uninitialised
        // stack slot), so `[ebx+0x114]` dereferences a stale stack value.
        // There is no correct render for such an object; refuse it.
        const uintptr_t block =
            BlockForKind(kind, *(const volatile int32_t*)(obj + kObjCharIx));
        if (!block) {
            if (++g_bad_kind <= 8) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "[FRAMEIDX] obj=0x%08X type=%d has kind=%d, which selects no "
                    "scene block -- sprite_rendering_engine would read its frame "
                    "table through an UNINITIALISED register. Draw refused.",
                    (unsigned)obj, (int)type, (int)kind);
            }
            return true;
        }

        // THE FIX. A destroyed object has no business on the render list; it
        // is only there because update_game_state built the list before the
        // script that killed it ran, and its frame index may now point into a
        // scene table that was freed and reloaded in that same script call.
        // Decided BEFORE the detector reports, so the report can say whether
        // the descriptor is about to be read or not -- that difference is the
        // whole A/B between the kill-switch arms.
        // NARROWED 2026-09-04, and the gate is why. Suppressing EVERY
        // destroyed object was three-for-three red on the spectator liveness
        // arm across three full gate runs, against green without it -- so
        // refusing draws that were never going to fault costs something real,
        // even though this is a display path and the mechanism is not yet
        // understood. It is also more than the crash needs.
        //
        // What the crash needs is narrower: refuse the draw only when this
        // object's frame index is ACTUALLY outside its scene table. A
        // destroyed object whose index still points inside the table draws
        // exactly what it drew before, reads nothing it should not, and
        // cannot fault. So bound the refusal to the case that motivated it.
        //
        // FrameIndexOutOfRange is a handful of loads on the sprite path and
        // is computed unconditionally now, not only under the detector -- the
        // fix cannot depend on a diagnostic being armed.
        const bool dead = (type == kObjTypeDestroyed);
        const bool oob  = FrameIndexOutOfRange(obj, block);
        const bool suppress = dead && oob && !RenderDeadObjects();

        if (DetectorArmed() && block != kNoBoundsCheck) {
            const uint8_t* table =
                *(const uint8_t* const volatile*)(block + kOffFrameTable);
            const int32_t frame_idx = *(const volatile int32_t*)(obj + kObjFrame);
            if (table) {
                const int32_t logical = LogicalFrameCount(block);
                const size_t bytes = CachedGlobalSize(table);
                const int32_t hard = (int32_t)(bytes / kFrameEntrySize);
                // Logical bound when we could recover it, otherwise the
                // allocation. Both are reported either way.
                const int32_t limit = (logical > 0) ? logical : hard;
                if (frame_idx < 1 || frame_idx > limit) {
                    ++g_oob_total;
                    if (type == kObjTypeDestroyed) ++g_oob_dead; else ++g_oob_live;
                    const bool would_fault =
                        (frame_idx < 1) ||
                        ((size_t)frame_idx * kFrameEntrySize > bytes);
                    if (would_fault) ++g_oob_would_fault;
                    if (!suppress) ++g_oob_allowed;
                    ReportOutOfRange(obj, kind, block, frame_idx, table,
                                     logical, bytes, would_fault, suppress);
                }
            }
        }

        if (suppress) {
            if (++g_dead_suppressed == 1) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[FRAMEIDX] suppressing the draw of destroyed objects "
                    "(first: obj=0x%08X kind=%d %s frame_idx=%d). These were "
                    "killed earlier in this frame's update pass and stayed on "
                    "the render list; drawing them is what reads a freed scene "
                    "table. Set FM2K_RENDER_DEAD_OBJECTS=1 to restore vanilla.",
                    (unsigned)obj, (int)kind, KindName(kind),
                    (int)*(const volatile int32_t*)(obj + kObjFrame));
            }
            return true;
        }
        return false;
    }
}

void ReportSummary() {
    if (!g_draws_seen && !g_dead_suppressed && !g_oob_total && !g_bad_kind) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[FRAMEIDX] summary: %llu sprite draws, %llu destroyed-object draws "
        "suppressed, %llu bad-kind draws refused; detector %s: %llu "
        "out-of-range frame indices (%llu on destroyed objects, %llu on LIVE "
        "objects, %llu past the allocation, %llu actually read by the engine).",
        (unsigned long long)g_draws_seen,
        (unsigned long long)g_dead_suppressed,
        (unsigned long long)g_bad_kind,
        DetectorArmed() ? "armed" : "DARK (FM2K_FRAMEIDX_DETECT=1 to arm)",
        (unsigned long long)g_oob_total,
        (unsigned long long)g_oob_dead,
        (unsigned long long)g_oob_live,
        (unsigned long long)g_oob_would_fault,
        (unsigned long long)g_oob_allowed);
}

}  // namespace SceneFrameGuard
