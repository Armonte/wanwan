#pragma once
// Shared state for the engine-split savestate TUs (savestate.cpp common +
// savestate_fm95.cpp + savestate_fm2k_{save,load,diag}.cpp). The rollback
// buffers + region constants + hash/log helpers live here so the per-engine
// bodies can share them. Definitions of the extern globals live in the common
// savestate.cpp (compiled into BOTH DLLs, so exactly one definition per binary).
#include "savestate.h"   // SaveStateData, RegionChecksums, StateSnapshot
#include "globals.h"     // FM2K::ADDR_* (engine-aware)
#include <cstdint>
#include <cstddef>

// FM2K memory-region constants. RNG/object-pool come from globals.h (engine-
// aware); the rest are FM2K-specific palette/effect/input-tracking regions
// (no FM95 equivalent yet -- FM95 savestate is a separate body). Used by the
// common lifecycle (DoInitialSync) and the FM2K save/load/diag TUs.
inline constexpr uintptr_t ADDR_RNG_SEED             = FM2K::ADDR_RANDOM_SEED;
inline constexpr uintptr_t ADDR_OBJECT_POOL          = FM2K::ADDR_OBJECT_POOL;
inline constexpr size_t    SIZE_OBJECT_POOL          = FM2K::SIZE_OBJECT_POOL;
inline constexpr size_t    OBJECT_POOL_STRIDE        = FM2K::OBJECT_POOL_STRIDE;
inline constexpr uintptr_t ADDR_RENDER_FRAME_COUNTER = 0x4456FC;
inline constexpr uintptr_t ADDR_INPUT_BUFFER_INDEX   = 0x447EE0;
inline constexpr uintptr_t ADDR_INPUT_TRACKING       = 0x447EE0;
inline constexpr size_t    SIZE_INPUT_TRACKING       = 0xA0;
inline constexpr uintptr_t ADDR_INPUT_HISTORY        = 0x4280D8;
inline constexpr size_t    SIZE_INPUT_HISTORY        = 0x2008;
inline constexpr uintptr_t ADDR_GAME_STATE           = 0x470020;
inline constexpr size_t    SIZE_GAME_STATE           = 0x220;

// Effect/shake region addresses - saved for rollback determinism (FM2K).
namespace EffectAddrs {
    inline constexpr uintptr_t EFFECT_SYS1     = 0x447D7D;
    inline constexpr size_t    EFFECT_SYS1_SZ  = 42;
    inline constexpr uintptr_t EFFECT_SYS2     = 0x4456B0;
    inline constexpr size_t    EFFECT_SYS2_SZ  = 88;
    inline constexpr uintptr_t SHAKE_EFFECTS   = 0x447DA9;
    inline constexpr size_t    SHAKE_EFFECTS_SZ = 40;
}

// Rollback buffers (defined in the common savestate.cpp).
inline constexpr int MAX_ROLLBACK_FRAMES = 64;
extern SaveStateData g_state_buffer[MAX_ROLLBACK_FRAMES];
extern int g_last_saved_slot;
extern int g_last_saved_frame;
extern uint32_t g_post_render_rng[MAX_ROLLBACK_FRAMES];
extern int32_t  g_post_render_rng_frame[MAX_ROLLBACK_FRAMES];
extern bool g_initial_sync_done;
extern RegionChecksums g_region_checksums;
struct ReplaySaveSnapshot {
    int32_t  frame_number;        // -1 if no replay save for this slot
    bool     valid;
    SaveStateData::SavedRegionCRCs crcs;
};
extern ReplaySaveSnapshot g_replay_saves[MAX_ROLLBACK_FRAMES];

// Hashing + replay-diff log path (defined in the common savestate.cpp). External
// linkage is safe: spec_transport's same-named Fletcher32 is file-static.
uint32_t Fletcher32(const uint8_t* data, size_t len);
const char* ReplayDiffLogPath();

// Defined in the FM2K diag TU, called from the FM2K save TU. (FullChecksum is
// in the public savestate.h; Fingerprint was only a local forward decl.)
uint32_t SaveState_CalculateFingerprint();

// Dead object-pool slots byte-exact across save / load (FM2K_POOL_DEADSLOT_RESTORE, default 1; 0 = the old
// live-only save + zeroing load, the negative control). A child object with the follow-parent flag
// (vHandleHitMovements 0x40F910: x = parent.x +- off, y = parent.y + off) keeps reading its parent's slot after the
// parent dies, i.e. the dead slot's residue. Saving only live slots and zeroing dead ones on load made a rollback
// across a spawn into that slot hand the resim zeros instead of the residue: the child landed at (-off, +off) on the
// rolled-back peer only -- the pkmncc round-1 desync (21 of 65 production desyncs on 0.2.89, f~335; repro
// artifacts/desync_0289_2026-11-11/repro).
bool SaveState_PoolDeadSlotRestore();
