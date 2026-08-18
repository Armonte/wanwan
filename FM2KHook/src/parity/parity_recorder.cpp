/* SPDX-License-Identifier: Apache-2.0 */
/* FM2K-side parity-snapshot recorder TEMPLATE.
 *
 * Drop into /mnt/c/dev/wanwan/FM2KHook/src/parity/ (create the dir) and
 * add to FM2KHook's CMake. Captures one KgtParitySnapshot per call from
 * WonderfulWorld_ver_0946.exe live globals and appends to a binary log
 * file. Pairs with kgtengine's recorder + diff tool.
 *
 * Integration:
 *   1. Copy this file + parity_recorder.h to FM2KHook/src/parity/.
 *   2. Add to CMakeLists.txt: target_sources(FM2KHook PRIVATE
 *      src/parity/parity_recorder.cpp)
 *   3. Add include path so it can find <kgt/kgt_parity_snapshot.h>:
 *      target_include_directories(FM2KHook PRIVATE
 *      "${CMAKE_CURRENT_SOURCE_DIR}/../../kgtengine/include")
 *   4. Wire ParityRecorder::Capture() into the per-frame trampoline
 *      AFTER the original update_game tick (so we record post-frame
 *      state, matching what kgtengine's recorder captures after
 *      kgt_engine_advance).
 *   5. Set FM2K_PARITY_RECORD_PATH=run.pty before launching the game
 *      to enable recording.
 *
 * Output: a .pty file readable by kgtengine's tools/kgt_diff_pty.
 *
 * Address derivation (all from /mnt/c/dev/wanwan/FM2KHook/src/core/globals.h
 * and IDA inspection of WonderfulWorld_ver_0946.exe):
 *   - Object pool @ 0x4701E0, 1024 slots × 382 bytes each
 *   - Player fighters are NOT at fixed slots 0/1. That was the original
 *     assumption and it is false: create_game_object @ 0x406570 allocates by
 *     first-free scan, so a fighter lands wherever the allocator put it and
 *     slot 1 routinely holds a system object. Resolve via
 *     ParityPool::FindPlayerObjectSlot (parity_pool.h).
 *   - Per-slot offsets:
 *       +0   type (4 = player)
 *       +4   facing
 *       +8   pos_x (Q14.18 fixed)
 *       +12  pos_y
 *       +16  vel_x  (TODO confirm -- may be accel)
 *       +20  vel_y
 *       +0x2C item_idx (script item index / PC)
 *       +0x30 script_idx
 *       +0x40 hitstun
 *       +0x44 hitstop  (TODO confirm)
 *       +0x156 char_index
 *   - HP @ 0x470134 (P1) / 0x470138 (P2) -- separate g_player_hp table
 *   - Super: g_char_value_current @ 0x4DFC95 + char_index * 0xE03F
 *   - RNG @ 0x41FB1C
 *   - Input ring index @ 0x447EE0 (g_input_buffer_index)
 *   - P1/P2 input rings @ 0x4280E0 / 0x4290E0
 *   - Match phase: TODO map from FM2K's game_state struct (0x470020+0x220) */

#include "parity_recorder.h"
#include "parity_pool.h"      // shared process-independent pool facts (player slots + topology digest)
#include "css_window.h"       // [CSS-WIN]/[CSS-OBJ] character-select window gate (dark unless FM2K_CSS_WIN=1)
#include "../core/globals.h"  // Fm2k_BuildLogPath
#include "../netplay/savestate.h"  // SaveState_CalculateFullChecksum / RegionChecksums ([FULLFP] tracer)

#include <kgt/kgt_parity_snapshot.h>
#include <SDL3/SDL_log.h>
#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace {

constexpr uintptr_t ADDR_OBJECT_POOL_BASE  = 0x4701E0;
constexpr size_t    OBJECT_SLOT_SIZE       = 382;

/* Per-player HP at fixed addresses; 57407-byte stride between slots.
 * Verified via IDA xref of g_p1_hp (read in vs_round_function @ 0x4086A0
 * and camera_manager @ 0x40AF30). Was previously 0x470134/138 -- those
 * are inside g_player_action_history, NOT the HP table, so HP always
 * read 0 in captures. */
constexpr uintptr_t ADDR_P1_HP             = 0x4DFC85;
constexpr uintptr_t ADDR_P2_HP             = 0x4EDCC4;

constexpr uintptr_t ADDR_RNG               = 0x41FB1C;
constexpr uintptr_t ADDR_INPUT_BUF_INDEX   = 0x447EE0;
constexpr uintptr_t ADDR_P1_INPUT_HISTORY  = 0x4280E0;
constexpr uintptr_t ADDR_P2_INPUT_HISTORY  = 0x4290E0;
// Current-frame confirmed input scalars (FM2K). Overwritten once per frame in
// update; deterministic at capture time -- unlike the ring, whose layout drifts
// under rollback. See the input_p1/p2 capture below.
constexpr uintptr_t ADDR_P1_INPUT_CUR      = 0x4259C0;
constexpr uintptr_t ADDR_P2_INPUT_CUR      = 0x4259C4;

constexpr uintptr_t ADDR_CHAR_DATA_BASE    = 0x4D1D90;
constexpr size_t    CHAR_DATA_STRIDE       = 57407;
constexpr size_t    CHAR_VALUE_CURRENT_OFFSET = 0xDF05;  /* 0x4DFC95 - 0x4D1D90 */

constexpr uintptr_t ADDR_FRAME_COUNTER     = 0x4456FC;
/* Per FM2KHook globals.h:
 *   ADDR_GAME_MODE  = 0x470054 (g_game_mode: 0=boot, 2000=title,
 *                                3000=CSS, 4000=stage, 5000=battle)
 *   ADDR_GAME_TIMER = 0x470044 (g_game_timer per-frame counter)
 *   ADDR_ROUND_TIMER_COUNTER = 0x424F00 (post-CSS lock counter; battle
 *                                         starts when > 100; per IDA
 *                                         game_state_manager @ 0x406FC0)
 * Camera offsets aren't yet mapped in FM2KHook source -- parity diff
 * surfaces them as divergences (kgt populates camera_x/y, FM2K side
 * leaves zero until offset is found). */
constexpr uintptr_t ADDR_MATCH_PHASE       = 0x470054;   /* g_game_mode */
constexpr uintptr_t ADDR_ROUND_TIMER       = 0x470044;   /* g_game_timer */
/* Camera = g_screen_x/y (per camera_manager @ 0x40AF30 disasm). */
constexpr uintptr_t ADDR_CAMERA_X          = 0x447F2C;
constexpr uintptr_t ADDR_CAMERA_Y          = 0x447F30;

inline uint32_t Read32(uintptr_t a) {
    return a ? *reinterpret_cast<const uint32_t*>(a) : 0u;
}
inline int32_t Read32S(uintptr_t a) {
    return a ? *reinterpret_cast<const int32_t*>(a) : 0;
}

/* Object-slot offsets per WonderfulWorld_ver_0946.exe object layout
 * (382 B total per slot):
 *   +0x89  : 20 × KgtScriptItem* hurtbox slot pointers (4B each = 80 B)
 *   +0xD9  : 20 × KgtScriptItem* hitbox slot pointers (4B each = 80 B)
 *   +0x129 : KgtScriptItem* active [C] cancel pointer (4B)
 *   +0x131 : 16 × int16 task variables ([V] bank 0x00, 32 B total)
 * Box arrays are stored as cross-process-incomparable pointers; we
 * popcount non-null entries (matches what the v2/v3 snapshot expects).
 * Task vars are stored as plain int16 values, directly comparable. */
constexpr size_t SLOT_HURTBOX_OFFSET    = 0x89u;
constexpr size_t SLOT_HITBOX_OFFSET     = 0xD9u;
constexpr size_t SLOT_CANCEL_OFFSET     = 0x129u;
constexpr size_t SLOT_TASK_VARS_OFFSET  = 0x131u;
constexpr int    BOX_SLOT_COUNT         = 20;
constexpr int    TASK_VAR_COUNT         = 16;

/* Player-object resolution moved to ParityPool::FindPlayerObjectSlot (see
 * parity_pool.h; implemented just below this anonymous namespace) so the
 * .pty recorder, the [SPEC-FP]/[HOST-FP] traces and the [CHECKSUM] fencepost
 * all resolve the fighters the SAME way. It is the identical scan this file
 * has used since 2026-06-11, just no longer private to the recorder. */

void FillPlayerSnapshot(KgtParityPlayer& dst, int slot_idx) {
    if (slot_idx < 0 || slot_idx >= 1024) {
        std::memset(&dst, 0, sizeof(dst));
        dst.script_idx         = -1;
        dst.item_idx           = -1;
        dst.facing             = 1;
        dst.cancel_script_item = -1;
        return;
    }
    const uintptr_t slot_addr = ADDR_OBJECT_POOL_BASE +
                                static_cast<uintptr_t>(slot_idx) * OBJECT_SLOT_SIZE;
    const uint32_t type = Read32(slot_addr + 0);
    if (type < 4u) {
        std::memset(&dst, 0, sizeof(dst));
        dst.script_idx         = -1;
        dst.item_idx           = -1;
        dst.facing             = 1;
        dst.cancel_script_item = -1;
        return;
    }
    /* Object-slot byte offsets per WW_0946.exe hit_detection_system @
     * 0x40F010 disasm (this session -- see docs/parity_runbook.md):
     *   +0x08  pos_x (Q14.18)             confirmed
     *   +0x0C  pos_y                      confirmed
     *   +0x18  vel_x (MoveCmd target)     per docs/editor/opcode_dispatcher.md
     *   +0x1C  vel_y                      per same
     *   +0x2C  item_idx                   confirmed
     *   +0x30  script_idx                 confirmed
     *   +0x40  hitstop                    confirmed (was wrongly at +0x44)
     *   +0x5C  facing                     confirmed (was wrongly at +0x04)
     *   +0x15E state_flags                confirmed
     * (hitstun field -- defender state offset -- needs further RE; v1
     * leaves this 0). */
    /* Facing at +0x5C is a DWORD where only bit 0 is meaningful:
     *   bit 0 clear = facing right, bit 0 set = facing left.
     * Verified via hit_detection_system @ 0x40F010: the hitbox-x
     * mirroring branches on `(attacker_object_ptr[23] & 1) != 0`.
     * Normalize to kgt's +1/-1 convention so the diff is meaningful
     * across engines. */
    {
        const int32_t raw_facing = Read32S(slot_addr + 0x5C);
        dst.facing = (raw_facing & 1) ? -1 : 1;
    }
    dst.pos_x       = Read32S(slot_addr + 0x08);
    dst.pos_y       = Read32S(slot_addr + 0x0C);
    dst.vel_x       = Read32S(slot_addr + 0x18);
    dst.vel_y       = Read32S(slot_addr + 0x1C);
    dst.item_idx    = Read32S(slot_addr + 0x2C);
    dst.script_idx  = Read32S(slot_addr + 0x30);
    dst.hitstun     = 0;                            /* TODO offset */
    dst.hitstop     = Read32S(slot_addr + 0x40);
    dst.state_flags = Read32S(slot_addr + 0x15E);

    /* HP/super: not in object slot -- pull from per-character data table. */
    const int char_idx = Read32S(slot_addr + 0x156);
    if (char_idx >= 0 && char_idx < 8) {
        const uintptr_t char_base = ADDR_CHAR_DATA_BASE +
                                     static_cast<uintptr_t>(char_idx) * CHAR_DATA_STRIDE;
        dst.super_meter = Read32S(char_base + CHAR_VALUE_CURRENT_OFFSET);
    } else {
        dst.super_meter = 0;
    }

    /* Per-player HP override: FM2K maintains a separate HP table, keyed
     * by the object's char index (+0x156), NOT the pool slot index --
     * P2's char object is not in pool slot 1 (see FindPlayerObjectSlot). */
    if (char_idx >= 0 && char_idx < 8) {
        dst.hp = static_cast<int32_t>(
            Read32(ADDR_P1_HP + static_cast<uintptr_t>(char_idx) * CHAR_DATA_STRIDE));
    } else {
        dst.hp = 0;
    }

    /* v2: hit/hurt slot popcounts -- cross-process comparable summary
     * of the 20 + 20 box-pointer arrays. The slot_idx for [C] cancel
     * needs to be DERIVED from the script-item pointer (subtract the
     * script-items base, divide by item size). For v1 of the FM2K
     * recorder we just store -1 if pointer is null, else a positive
     * "armed" sentinel -- the kgt side stores the actual item index, so
     * exact-match parity only holds when both sides are NULL/-1.
     * TODO: reverse-engineer the script-items array base address to
     * convert pointer → index for true parity. */
    int hit_n = 0, hurt_n = 0;
    for (int i = 0; i < BOX_SLOT_COUNT; ++i) {
        if (Read32(slot_addr + SLOT_HURTBOX_OFFSET + i * 4u)) ++hurt_n;
        if (Read32(slot_addr + SLOT_HITBOX_OFFSET  + i * 4u)) ++hit_n;
    }
    dst.hit_box_active_count  = hit_n;
    dst.hurt_box_active_count = hurt_n;
    const uint32_t cancel_ptr = Read32(slot_addr + SLOT_CANCEL_OFFSET);
    dst.cancel_script_item    = cancel_ptr ? 1 : -1;   /* "armed" sentinel */

    /* v3: task_vars per-object -- direct memcpy from the slot's int16
     * array at +0x131. */
    for (int i = 0; i < TASK_VAR_COUNT; ++i) {
        dst.task_vars[i] = *reinterpret_cast<const int16_t*>(
            slot_addr + SLOT_TASK_VARS_OFFSET + i * 2u);
    }
}

}  /* anonymous namespace */

/* ==========================================================================
 * ParityPool -- process-INDEPENDENT object-pool facts (see parity_pool.h)
 *
 * DIAGNOSTIC-ONLY, BY CONSTRUCTION. Every memory access below is a read
 * through a const pointer; nothing here writes engine memory, consumes an
 * RNG draw, allocates, or calls into the engine. The only mutable state is a
 * function-local `static int` env cache, written once. Compiling this in or
 * out cannot change simulation results.
 * ========================================================================== */
namespace ParityPool {

namespace {

/* FNV-1a, one 32-bit word at a time. Chosen over the Fletcher-style
 * accumulator the [POOLSET] fingerprint uses because Fletcher needs two
 * `% 65535` divisions per half-word, and this digest runs on EVERY captured
 * frame rather than only when a deep diagnostic env is set. Multiplicative
 * mixing is order-sensitive, which is the property that matters here: a pool
 * where the same objects sit at permuted slot indices must NOT hash equal.
 *
 * The seed carries a field-set version tag. Bump it whenever the tuple below
 * changes, so a digest from an old build can never accidentally compare equal
 * to one from a new build. */
constexpr uint32_t kFnvPrime  = 16777619u;
constexpr uint32_t kTopoSeed  = 0x811C9DC5u ^ 0x50544F32u;  /* 'PTO2' -- slot map */
constexpr uint32_t kBindSeed  = 0x811C9DC5u ^ 0x50424E31u;  /* 'PBN1' -- bindings */

inline uint32_t Mix(uint32_t h, uint32_t v) {
    return (h ^ v) * kFnvPrime;
}

/* parent_object_ptr @ +0x17A is the one pointer-typed field the topology
 * tuple cares about (docs/game/pointer_audit.md classifies it Class A --
 * self-referential into the object pool). It is converted to a SLOT INDEX
 * and never hashed as a pointer:
 *   null              -> kParentNone
 *   slot-aligned, in  -> the slot index 0..1023
 *   anything else     -> kParentForeign (stale/garbage/heap: the raw value is
 *                        discarded so a per-process address cannot reach the
 *                        digest even in a corrupted state)
 * Both sentinels are >= 0xFFFFFFFE, so they can never collide with a real
 * slot index. */
constexpr uint32_t kParentNone    = 0xFFFFFFFFu;
constexpr uint32_t kParentForeign = 0xFFFFFFFEu;

inline uint32_t ParentSlotToken(uint32_t raw) {
    if (raw == 0u) return kParentNone;
    const uintptr_t off = static_cast<uintptr_t>(raw) - kPoolBase;
    /* Unsigned wrap makes "below the pool" fail this same bound check. */
    if (off >= kSlotCount * kSlotStride) return kParentForeign;
    const uint32_t k = static_cast<uint32_t>(off / kSlotStride);
    if (static_cast<uintptr_t>(k) * kSlotStride != off) return kParentForeign;
    return k;
}

}  /* anonymous namespace */

bool TopologyEnabled() {
    if constexpr (!FM2K::kIsFM2K) {
        return false;
    }
    /* Escape hatch only. The topology term rides the fencepost's OWN gate
     * (the [CHECKSUM] call sites are already behind FM2K_CINPUT=1), so a
     * normal build never pays for it; within a parity run it is deliberately
     * always-on, because the blind spot it closes bit in exactly the runs
     * where nobody thought to enable an extra tracer. FM2K_CK_TOPOLOGY=0
     * turns it off without rebuilding if it ever proves too costly. */
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = std::getenv("FM2K_CK_TOPOLOGY");
        s_on = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
    }
    return s_on != 0;
}

int FindPlayerObjectSlot(int player_idx) {
    if constexpr (!FM2K::kIsFM2K) {
        return -1;
    }
    /* The old convention "slot 0 = P1, slot 1 = P2" is FALSE for P2 (verified
     * 2026-06-11: stress-mode players[1] captured a static system object --
     * pos=(320,150), script=0/0 -- so every P2-side divergence was invisible
     * to parity_diff; the same artifact flipped a Wave 4.1 harness verdict via
     * [SPEC-FP]). Match the engine's own player scans (camera_manager @
     * 0x40AF30, hit_detection_system @ 0x40F010): object type dword == 4 AND
     * player slot id at +0x156 == player_idx. Ascending scan returns the first
     * match; char objects are created at battle init in low slots, well below
     * the BG-handler objects whose +0x156 accumulators could transiently equal
     * 0/1.
     *
     * The +0x156 compare stays a 32-bit read on purpose. playerSlotId is a
     * u16 followed by two unmapped bytes, so the dword compare is STRICTER
     * than a u16 compare (it also requires +0x158..+0x159 == 0) -- which is
     * what has been validated on this predicate since 2026-06-11. The
     * topology digest below reads the u16 instead; see the note there. */
    for (int i = 0; i < static_cast<int>(kSlotCount); ++i) {
        const uintptr_t a = kPoolBase + static_cast<uintptr_t>(i) * kSlotStride;
        if (Read32(a + kOffType) != kTypePlayerChar) continue;
        if (Read32S(a + kOffPlayerSlot) != player_idx) continue;
        return i;
    }
    return -1;
}

int FindCssPreviewSlot(int player_idx) {
    if constexpr (!FM2K::kIsFM2K) {
        return -1;
    }
    /* FindPlayerObjectSlot + the engine's own entity_kind filter. See the
     * header for the whole argument; the short version is that in mode 2000 a
     * type-4 object with +0x156 == 0 is NOT necessarily player 0 -- the
     * create_game_object memset makes 0 the DEFAULT, and the character-select
     * background-script children (entity_kind 3) carry it while allocating
     * below the previews.
     *
     * kind >= 2 is the engine's line, not ours (Css_UnloadPlayerPreview uses
     * < 2), and it deliberately also excludes kind 5: no kind-5 object exists
     * in a mode-2000 pool in any kept corpus, and if one ever does it is a
     * battle fighter surviving the CSS-entry demote -- which deserves a loud
     * line, not a widened predicate. The reads match FindPlayerObjectSlot's
     * (32-bit, stricter than a u16 compare) and CensusPreviewObjects's. */
    for (int i = 0; i < static_cast<int>(kSlotCount); ++i) {
        const uintptr_t a = kPoolBase + static_cast<uintptr_t>(i) * kSlotStride;
        if (Read32(a + kOffType) != kTypePlayerChar) continue;
        if (Read32S(a + kOffEntityKind) >= 2) continue;
        if (Read32S(a + kOffPlayerSlot) != player_idx) continue;
        return i;
    }
    return -1;
}

PlayerView ReadPlayer(int player_idx) {
    PlayerView v{-1, 0, 0, -1};
    if constexpr (!FM2K::kIsFM2K) {
        return v;
    }
    const int slot = FindPlayerObjectSlot(player_idx);
    if (slot < 0) return v;   /* pre-spawn / non-battle: sentinel block */
    const uintptr_t a = kPoolBase + static_cast<uintptr_t>(slot) * kSlotStride;
    v.slot   = slot;
    v.pos_x  = Read32S(a + kOffPosX);
    v.pos_y  = Read32S(a + kOffPosY);
    v.script = Read32S(a + kOffScriptId);
    return v;
}

Scan ScanPool(bool want_legacy_fp, char* active_list, size_t list_cap) {
    Scan out{0u, 0u, 0u};
    if (active_list && list_cap) active_list[0] = '\0';
    if constexpr (!FM2K::kIsFM2K) {
        return out;
    }

    uint32_t topo = kTopoSeed;
    uint32_t bind = kBindSeed;
    /* Legacy [POOLSET] accumulator -- kept bit-for-bit identical to the
     * original inline form (Fletcher-style over (slot, type, owner, posX,
     * posY), low half-word then high half-word) so fingerprints from this
     * build still line up with the ones in the Wave 3.1/4.x reports. */
    uint32_t s1 = 0xFFFF, s2 = 0xFFFF;
    auto legacy_mix = [&](uint32_t v) {
        s1 = (s1 + (v & 0xFFFF)) % 65535; s2 = (s2 + s1) % 65535;
        s1 = (s1 + (v >> 16))    % 65535; s2 = (s2 + s1) % 65535;
    };

    size_t lp = 0;
    for (size_t i = 0; i < kSlotCount; ++i) {
        const uintptr_t a = kPoolBase + static_cast<uintptr_t>(i) * kSlotStride;
        const uint32_t type = Read32(a + kOffType);
        if (type == 0u) continue;          /* free slot: one dword touched */
        ++out.active_count;

        /* SLOT-MAP tuple (`top=`). Occupancy is implied by being here; the
         * slot index paired with the type is what makes a pure re-INDEXING of
         * the same object set (carry-state family A1) visible, and it is
         * EXACTLY the "which objects exist, of what kind, at which slot
         * indices" this term is documented as meaning. Deliberately NOT
         * included: positions, velocities, script cursors, HP -- those are
         * gameplay STATE and belong to the crc term.
         *
         * BINDING tuple (`bind=`), Phase 4c: owner, player slot, entity kind
         * and the creator link, i.e. who each object belongs to rather than
         * where it sits. Split out of `top=` because 4b measured a wanwan match
         * that was bit-identical on every other term for 5320/5320 frames while
         * `top=` was red on all of them, and the whole difference was one
         * object's parent link -- a real difference, but not the one the term
         * claimed to report, and useless as a gate at 100% red. Every member is
         * an integer the pointer audit classifies as a non-pointer, except
         * parent which is normalised to a slot index above. */
        const uint32_t owner  = Read32(a + kOffOwner);
        const uint32_t player = *reinterpret_cast<const uint16_t*>(a + kOffPlayerSlot);
        const uint32_t kind   = Read32(a + kOffEntityKind);
        const uint32_t parent = ParentSlotToken(Read32(a + kOffParentPtr));
        /* u16 read, unlike the finder's dword compare above: +0x158..+0x159
         * is an unmapped field in docs/editor/runtime_entity.md, and hashing
         * bytes nobody has reverse-engineered is how a fencepost earns a
         * reputation for false positives. */
        topo = Mix(topo, static_cast<uint32_t>(i));
        topo = Mix(topo, type);
        bind = Mix(bind, static_cast<uint32_t>(i));
        bind = Mix(bind, owner);
        bind = Mix(bind, player);
        bind = Mix(bind, kind);
        bind = Mix(bind, parent);

        if (want_legacy_fp) {
            legacy_mix(static_cast<uint32_t>(i));
            legacy_mix(type);
            legacy_mix(owner);
            legacy_mix(Read32(a + kOffPosX));
            legacy_mix(Read32(a + kOffPosY));
        }
        if (active_list && list_cap > 20u && lp < list_cap - 20u) {
            lp += static_cast<size_t>(std::snprintf(active_list + lp, list_cap - lp,
                                                    "%u:%u ", (unsigned)i, type));
        }
    }
    /* Fold the population count in last: a pool that gained an object AND
     * lost one at the same index would otherwise have to rely on the field
     * tuple alone. The |1 on a zero result keeps 0 reserved as the documented
     * "not computed" value -- a 1-in-2^32 collision is not worth an ambiguous
     * sentinel. */
    const uint32_t folded  = Mix(topo, out.active_count);
    const uint32_t bfolded = Mix(bind, out.active_count);
    out.topology       = folded  ? folded  : 1u;
    out.binding        = bfolded ? bfolded : 1u;
    out.legacy_poolset = want_legacy_fp ? ((s2 << 16) | s1) : 0u;
    return out;
}

/* Observation-based per-slot creation frame (Lane C bind= probe, 2026-08-17).
 *
 * There is NO creation-frame field in KgtRuntimeObject (docs/editor/
 * runtime_entity.md enumerates every mapped byte of the 382 and none of them is
 * one), so this is SAMPLED, not read: the first scan on which a slot is seen
 * occupied after being seen free records the engine's own frame counter
 * (0x4456FC), which IS inside the save-state envelope and therefore rewinds
 * with a rollback exactly like the pool does. Under resim a slot's recorded
 * value converges on the frame the object was really created.
 *
 * DIAGNOSTIC ONLY -- never hashed into `top=`/`bind=`. It is sampled at
 * [POOLTOPO] cadence, so it is a function of THIS process's rollback schedule
 * in a way a digest member may never be. kCreatedUnknown marks a slot that was
 * already occupied on the very first scan (nothing can be said about when it
 * appeared).
 *
 * Single-threaded: DumpTopoDetail is only ever called from ParityRecorder::
 * Capture() on the sim thread. */
constexpr uint32_t kCreatedUnknown = 0xFFFFFFFFu;
static uint32_t s_slot_created[kSlotCount] = {};
static bool     s_slot_occupied[kSlotCount] = {};
static bool     s_topo_scanned = false;

size_t DumpTopoDetail(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if constexpr (!FM2K::kIsFM2K) {
        return 0;
    }
    const uint32_t now = Read32(ADDR_FRAME_COUNTER);
    /* Reserve for ONE worst-case record plus the truncation marker.
     * Worst case is now "1023:65535:4294967295:4294967295:FFFFFFFF:4294967295 "
     * = 53 chars (it was 33 before the raw/created members were added), and the
     * parent sentinels kParentNone / kParentForeign print as 4294967295 /
     * 4294967294 routinely -- the old 24-byte reserve was arithmetically too
     * small, so the last record on a capped line could be cut MID-TOKEN into a
     * valid-looking but wrong tuple. Measured before that fix: 1881 of 20738
     * [POOLTOPO] lines on wanwan (9%) were pegged at the 1024-byte cap, with no
     * marker of any kind, while the report that cites [POOLTOPO] as the
     * authority for naming a bind= residual assumed whole lines. */
    constexpr size_t kRecordReserve = 64u;
    /* ...and for the trailing marker, so it can NEVER be the thing that gets
     * dropped: "TRUNCATED (n=1023/1023)" is 23 chars. */
    constexpr size_t kMarkerReserve = 32u;
    size_t lp = 0;
    size_t emitted = 0, active = 0;
    bool   truncated = false;
    for (size_t i = 0; i < kSlotCount; ++i) {
        const uintptr_t a = kPoolBase + static_cast<uintptr_t>(i) * kSlotStride;
        if (Read32(a + kOffType) == 0u) {
            s_slot_occupied[i] = false;
            continue;
        }
        ++active;
        /* Occupancy bookkeeping runs for EVERY active slot, including the ones
         * a truncated line drops -- otherwise a pool that overflows the buffer
         * would poison the created= column of the slots that do fit. */
        if (!s_slot_occupied[i]) {
            s_slot_occupied[i] = true;
            s_slot_created[i]  = s_topo_scanned ? now : kCreatedUnknown;
        }
        if (lp + kRecordReserve + kMarkerReserve >= cap) { truncated = true; continue; }
        const uint32_t player = *reinterpret_cast<const uint16_t*>(a + kOffPlayerSlot);
        const uint32_t kind   = Read32(a + kOffEntityKind);
        const uint32_t raw    = Read32(a + kOffParentPtr);
        const uint32_t parent = ParentSlotToken(raw);
        /* raw= is the UNNORMALISED +0x17A dword. Lane C's triage asked for it
         * by name: `bind=` is red from the first paired frame with `parent`
         * normalising to different in-pool slots on the two planes, and the
         * question that decides whether the IDA writer-pass is justified is
         * whether both planes hold in-pool pointers (creator really differs) or
         * one holds a heap value (different framing entirely). Printed as hex,
         * never hashed -- see the process-independence contract in parity_pool.h. */
        lp += static_cast<size_t>(std::snprintf(out + lp, cap - lp,
                                                "%u:%u:%u:%u:%08X:%u ",
                                                (unsigned)i, player, kind, parent,
                                                raw, s_slot_created[i]));
        ++emitted;
    }
    s_topo_scanned = true;
    /* LOUD, always: a consumer can tell a whole line from a partial one without
     * counting characters, and the counts say exactly how much was dropped. */
    lp += static_cast<size_t>(std::snprintf(out + lp, cap - lp,
        "%s(n=%u/%u)", truncated ? "TRUNCATED " : "",
        (unsigned)emitted, (unsigned)active));
    if (lp > cap - 1u) lp = cap - 1u;   /* snprintf returns the WOULD-BE length */
    return lp;
}

Topology ComputeTopology() {
    if (!TopologyEnabled()) return Topology{0u, 0u, 0u};
    const Scan s = ScanPool(/*want_legacy_fp=*/false, nullptr, 0u);
    return Topology{s.topology, s.binding, s.active_count};
}

}  /* namespace ParityPool */

namespace ParityRecorder {

struct Recorder {
    std::FILE* fp;
    uint32_t   frames_written;
    bool       seed_captured;
};

static Recorder* g_active_recorder = nullptr;

bool Open(const char* path) {
    if (g_active_recorder) Close();

    std::FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;

    KgtParitySnapshotHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    std::memcpy(hdr.magic, KGT_PARITY_MAGIC, 4);
    hdr.version       = KGT_PARITY_SNAPSHOT_VERSION;
    hdr.snapshot_size = static_cast<uint32_t>(sizeof(KgtParitySnapshot));
    hdr.flags         = KGT_PARITY_FLAG_FROM_FM2K;
    hdr.frame_count   = 0u;
    /* initial_seed is patched on the first Capture() call -- at Open()
     * time (DLL attach) FM2K hasn't yet executed srand(time(NULL)) so
     * g_rand_seed reads as the C-runtime default (1). The first frame
     * we capture is post-init, so g_rand_seed is the real time-based
     * seed there. Header field stays zero until we patch it. */
    hdr.initial_seed  = 0u;

    if (std::fwrite(&hdr, sizeof(hdr), 1, fp) != 1) {
        std::fclose(fp);
        return false;
    }

    auto* rec = new Recorder{};
    rec->fp = fp;
    rec->frames_written = 0u;
    rec->seed_captured = false;
    g_active_recorder = rec;
    return true;
}

void Capture() {
    if (!g_active_recorder || !g_active_recorder->fp) return;

    // Diagnostic: log every capture with rng + frame_counter + buf_idx so
    // we can pair host's captures with replay's captures across the
    // entire run. Gated on FM2K_PARITY_CAPTURE_TRACE=1 -- off by default.
    {
        static int s_trace_cached = -1;
        if (s_trace_cached < 0) {
            const char* v = std::getenv("FM2K_PARITY_CAPTURE_TRACE");
            s_trace_cached = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_trace_cached == 1) {
            const uint32_t rng = Read32(ADDR_RNG);
            const uint32_t rfc = Read32(ADDR_FRAME_COUNTER);
            const uint32_t buf_idx = Read32(ADDR_INPUT_BUF_INDEX);
            // Hash char_dynamic[0] (p1's 57407-byte slot). Comparing this
            // per-frame between host and replay pinpoints which character
            // field differs, when parity's 92-byte player snap matches but
            // engine state actually diverges. Fletcher32-style sum for
            // cheap hashing (~57KB scan; called per Capture which fires
            // once per battle frame).
            constexpr uintptr_t CHAR_SLOT_0_BASE = 0x4D1D90;
            constexpr size_t    CHAR_SLOT_SIZE   = 57407;
            uint32_t s1 = 0xFFFF, s2 = 0xFFFF;
            const uint8_t* p = (const uint8_t*)CHAR_SLOT_0_BASE;
            for (size_t i = 0; i < CHAR_SLOT_SIZE; i++) {
                s1 = (s1 + p[i]) % 65535;
                s2 = (s2 + s1)   % 65535;
            }
            const uint32_t char0_crc = (s2 << 16) | s1;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[PARITY-CAPTURE] seq=%u rfc=%u buf=%u rng=0x%08X char0=0x%08X",
                g_active_recorder->frames_written, rfc, buf_idx, rng, char0_crc);
        }
    }


    // [FULLFP] Per-region full-state fingerprint tracer (mid-join spectate
    // desync hunt). FM2K_FULLFP=1 logs one line per captured battle frame
    // with a Fletcher32 per savestate region so host-vs-spectator diffing
    // (aligned by rng) names the FIRST region that diverges -- object_pool /
    // char_dynamic / effects are exactly the state the .pty parity does NOT
    // capture. Same capture point as the parity snapshot, so timing matches.
    {
        static int s_fullfp = -1;
        if (s_fullfp < 0) {
            const char* v = std::getenv("FM2K_FULLFP");
            s_fullfp = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_fullfp == 1) {
            SaveState_CalculateFullChecksum();  // populates g_region_checksums
            const RegionChecksums& rc = SaveState_GetRegionChecksums();
            // gp = gameplay-seed game_rand draws since the last capture (~1 frame).
            // If host vs spectator gp diverges at some frame, that frame has the
            // extra/missing gameplay draw = the rng leak.
            const uint32_t gp = g_gameplay_rand_calls;
            g_gameplay_rand_calls = 0;
            // fn = per-caller gameplay-rand counts: cam/shake/color/sprite/hit/ai/csm/other
            uint32_t fn[8];
            for (int i = 0; i < 8; i++) { fn[i] = g_gp_rand_by_fn[i]; g_gp_rand_by_fn[i] = 0; }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[FULLFP] seq=%u gp=%u fn=%u,%u,%u,%u,%u,%u,%u,%u rng=0x%08X gs=0x%08X "
                "pool=0x%08X char=0x%08X inp=0x%08X fx1=0x%08X fx2=0x%08X shake=0x%08X",
                g_active_recorder->frames_written, gp,
                fn[0], fn[1], fn[2], fn[3], fn[4], fn[5], fn[6], fn[7],
                rc.rng, rc.game_state, rc.object_pool, rc.char_dynamic,
                rc.input_tracking, rc.effect_sys1, rc.effect_sys2, rc.shake_effects);
        }
    }

    // [POOLSET] Process-INDEPENDENT active-object-set fingerprint (mid-join
    // spectate desync hunt). The raw object bytes carry per-process pointers
    // (sprite/loaded-data addrs) so [FULLFP] pool= always differs -- useless.
    // Here we hash ONLY gameplay fields (slot index, type@0x0, owner@0x4,
    // posX@0x8, posY@0xC) of each active slot, so host-vs-spectator diffing
    // (aligned by rng) names the FIRST frame the object SET diverges and
    // which slot/type -- the item_idx -96 says the spectator ends up with
    // fewer objects, this finds the first one. FM2K_POOLSET=1.
    {
        static int s_poolset = -1;
        if (s_poolset < 0) {
            const char* v = std::getenv("FM2K_POOLSET");
            s_poolset = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_poolset == 1) {
            // The scan itself now lives in ParityPool::ScanPool so the
            // [CHECKSUM] topology term and this diagnostic can never drift
            // apart. fp= is bit-for-bit the historical value (same tuple, same
            // Fletcher-style accumulator); top= is the NEW topology digest,
            // printed here as well so a POOLSET investigation and a fencepost
            // mismatch can be read against each other in one log.
            char list[512];
            const ParityPool::Scan sc =
                ParityPool::ScanPool(/*want_legacy_fp=*/true, list, sizeof(list));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[POOLSET] seq=%u cnt=%u fp=0x%08X top=0x%08X bind=0x%08X active=%s",
                g_active_recorder->frames_written, sc.active_count,
                sc.legacy_poolset, sc.topology, sc.binding, list);
            // Phase 4b: the members `fp=` cannot see, plus the Lane C raw
            // +0x17A / created-frame probe. See ParityPool::DumpTopoDetail.
            // Same gate, same cadence.
            //
            // CHUNKED, and the buffer is static (2026-08-17 soak lane). Two
            // reasons the old `char topo_detail[1024]` on the stack was the
            // wrong shape for an evidence trap:
            //   * the record grew from 33 to 53 worst-case chars, so 1024 now
            //     holds ~18 slots -- wanwan already pegged that cap on 9% of
            //     lines with the SHORT record, and vanpri carries 80-150 active
            //     objects. A truncated line is exactly the evidence a `top=`/
            //     `bind=` recurrence needs and cannot recover later.
            //   * one 8 KB line is a long line to hand a formatter. Emitting in
            //     <=900-char pieces keeps each [POOLTOPO] line the same order of
            //     magnitude as every other log line here.
            // p=N numbers the pieces from 0; the LAST piece carries
            // DumpTopoDetail's own "(n=emitted/active)" marker, so a consumer
            // can tell a complete reassembly from a partial one without
            // counting. Single-threaded (sim thread only), same as the recorder.
            static char topo_detail[8192];
            const size_t td_len =
                ParityPool::DumpTopoDetail(topo_detail, sizeof(topo_detail));
            constexpr size_t kChunk = 900u;
            size_t td_off = 0; unsigned td_part = 0;
            do {
                size_t n = td_len - td_off;
                if (n > kChunk) {
                    n = kChunk;
                    // Split on a record boundary so no consumer ever has to
                    // stitch a tuple across two lines.
                    while (n > 0 && topo_detail[td_off + n - 1] != ' ') --n;
                    if (n == 0) n = kChunk;   // one pathological record > kChunk
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[POOLTOPO] seq=%u p=%u d=%.*s",
                    g_active_recorder->frames_written, td_part,
                    (int)n, topo_detail + td_off);
                td_off += n; ++td_part;
            } while (td_off < td_len);
        }
    }

    // [CAMTRACE] Frame-windowed object-pool divergence tracer for the
    // forced-rollback drift (task #34). FM2K_CAMTRACE=lo-hi (capture-seq
    // window, e.g. "740-800") logs g_screen_x/y plus one line per active
    // object slot: full-slot Fletcher32 + the fields the camera/BG handler
    // (0x40AF30) uses as live sim state -- including dword offsets 68/72/76
    // which sit INSIDE the savestate restore's color-override carve-out.
    // Diff record-side (FM2K_P1_Debug.log) vs replay-side
    // (FM2K_P3_Debug.log) [CAMTRACE] lines to find which slot/field
    // diverges first.
    {
        static int s_ct_lo = -1, s_ct_hi = -2;
        static bool s_ct_parsed = false;
        if (!s_ct_parsed) {
            s_ct_parsed = true;
            if (const char* v = std::getenv("FM2K_CAMTRACE"); v && v[0]) {
                int lo = 0, hi = 0;
                if (std::sscanf(v, "%d-%d", &lo, &hi) == 2) {
                    s_ct_lo = lo;
                    s_ct_hi = hi;
                }
            }
        }
        const int seq = (int)g_active_recorder->frames_written;
        if (seq >= s_ct_lo && seq <= s_ct_hi) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[CAMTRACE] seq=%d screen=(%d,%d)",
                seq, Read32S(ADDR_CAMERA_X), Read32S(ADDR_CAMERA_Y));
            constexpr uintptr_t POOL_BASE = 0x4701E0;  // g_object_pool
            constexpr size_t POOL_STRIDE  = 382;
            constexpr size_t POOL_COUNT   = 1024;
            for (size_t i = 0; i < POOL_COUNT; i++) {
                const uint8_t* obj = (const uint8_t*)(POOL_BASE + i * POOL_STRIDE);
                if (obj[0] == 0) continue;
                uint32_t s1 = 0xFFFF, s2 = 0xFFFF;
                for (size_t b = 0; b < POOL_STRIDE; b++) {
                    s1 = (s1 + obj[b]) % 65535;
                    s2 = (s2 + s1)     % 65535;
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[CAMTRACE]   slot=%03u id=0x%08X crc=0x%08X o8=%d o12=%d "
                    "st338=%u a342=%d o68=%d o72=%d o76=%d",
                    (unsigned)i,
                    *(const uint32_t*)obj,
                    (s2 << 16) | s1,
                    *(const int32_t*)(obj + 8),  *(const int32_t*)(obj + 12),
                    *(const uint32_t*)(obj + 338), *(const int32_t*)(obj + 342),
                    *(const int32_t*)(obj + 68), *(const int32_t*)(obj + 72),
                    *(const int32_t*)(obj + 76));
                // FM2K_CAMTRACE_HEX=1: full 382-byte hex dump per active
                // slot (4 chunked lines) so an offline diff can pin the
                // exact divergent byte offsets. Keep the seq window tiny
                // (2-3 frames) when using this -- it's ~80 lines/frame.
                static int s_ct_hex = -1;
                if (s_ct_hex < 0) {
                    const char* h = std::getenv("FM2K_CAMTRACE_HEX");
                    s_ct_hex = (h && h[0] && h[0] != '0') ? 1 : 0;
                }
                if (s_ct_hex == 1) {
                    char hexbuf[2 * 96 + 1];
                    for (size_t base = 0; base < POOL_STRIDE; base += 96) {
                        const size_t nb = (POOL_STRIDE - base) < 96
                                        ? (POOL_STRIDE - base) : 96;
                        for (size_t j = 0; j < nb; j++) {
                            std::snprintf(hexbuf + 2 * j, 3, "%02X", obj[base + j]);
                        }
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[CAMTRACE-HEX] slot=%03u off=%03u %s",
                            (unsigned)i, (unsigned)base, hexbuf);
                    }
                }
            }
        }
    }

    /* Patch initial_seed into the header on the first frame where FM2K
     * is in battle phase (g_game_mode == 3000). srand(time(NULL)) runs
     * during the title-to-CSS transition, well before battle. Capturing
     * here means kgt boots with the same seed FM2K's vs_round_function
     * has at battle start -- frame-0-of-battle rng matches by construction.
     *
     * Reading at Open() (DLL attach) catches g_rand_seed == 1 (CRT
     * default), and even reading on the very first capture catches the
     * pre-srand value since the recorder fires from process attach. */
    if (!g_active_recorder->seed_captured) {
        const uint32_t mode = Read32(ADDR_MATCH_PHASE);
        if (mode >= 3000u) {
            const uint32_t seed = Read32(ADDR_RNG);
            const long here = std::ftell(g_active_recorder->fp);
            if (here >= 0) {
                std::fseek(g_active_recorder->fp,
                           offsetof(KgtParitySnapshotHeader, initial_seed),
                           SEEK_SET);
                std::fwrite(&seed, sizeof(seed), 1, g_active_recorder->fp);
                std::fseek(g_active_recorder->fp, here, SEEK_SET);
            }
            g_active_recorder->seed_captured = true;
        }
    }

    /* Capture every post-update frame. Alignment with kgt's .pty (which
     * starts already-in-battle from kgt_engine_create) happens at the
     * diff-tool layer: kgt_diff_pty searches for the first frame on
     * each side where game_mode == 5000 AND p1.hp > 0, then aligns. No
     * recorder-side filtering -- we want every frame's state for the
     * full session so future tools (timeline diff, divergence-since-N)
     * have the data they need. */

    KgtParitySnapshot snap;
    std::memset(&snap, 0, sizeof(snap));

    snap.frame    = Read32(ADDR_FRAME_COUNTER);
    snap.rng      = Read32(ADDR_RNG);

    /* Frame N's CONFIRMED p1/p2 input, read from the engine's current-frame
     * input SCALARS (0x4259C0 / 0x4259C4 -- what Hook_ProcessGameInputs logs
     * as "Synced: P1/P2"), NOT from input_history[buf_idx].
     *
     * Why not the ring: the input RING LAYOUT differs between a forward+
     * rollback run (record) and a linear replay -- rollback re-sim advances
     * the write cursor differently, so the same logical frame's input lands
     * in a different ring slot, and the capture-time cursor is callsite-
     * dependent. No ring index (buf_idx OR frame counter) gives a clean
     * cross-run match. The current-input scalar is overwritten once per frame
     * during update, so at capture (post-update) it holds frame N's input on
     * BOTH sides -- byte-identical when the sim is deterministic. */
    snap.input_p1 = Read32(ADDR_P1_INPUT_CUR);
    snap.input_p2 = Read32(ADDR_P2_INPUT_CUR);

    snap.match_phase = ADDR_MATCH_PHASE ? Read32S(ADDR_MATCH_PHASE) : 0;
    snap.round_timer = ADDR_ROUND_TIMER ? Read32S(ADDR_ROUND_TIMER) : 0;
    snap.camera_x    = ADDR_CAMERA_X    ? Read32S(ADDR_CAMERA_X)    : 0;
    snap.camera_y    = ADDR_CAMERA_Y    ? Read32S(ADDR_CAMERA_Y)    : 0;

    /* Resolve each player's char object by id -- the "slots 0 & 1 are
     * the fighters" convention is FALSE for P2 (its char object lives in
     * a different slot; pool slot 1 holds a system object). Not-found
     * (-1, pre-battle) produces the same zeroed/script=-1 block the old
     * empty-slot path did, so parity_diff alignment is unchanged.
     *
     * CHARACTER-SELECT SCOPE ONLY (spec_faller_diagnosis.md). In mode 2000 the
     * pool's low slots hold the CSS background-script children (entity_kind 3,
     * +0x156 never written, so create_game_object's memset leaves it 0 ==
     * "player 0"), and they can allocate BELOW the preview objects. Resolve
     * previews the way the ENGINE does -- Css_UnloadPlayerPreview @0x406520:
     * type == 4 && entity_kind < 2 && pslot == idx -- so the .pty (and
     * CssWindow, which shares this resolution) cannot bind a UI object as a
     * player. Battle frames keep FindPlayerObjectSlot unchanged, which is why
     * the desync oracle (CHECKSUM / CINPUT / determinism, all battle-segment
     * terms) is byte-identical across this change. */
    const bool css_phase = (snap.match_phase == 2000);
    const int p1_slot = css_phase ? ParityPool::FindCssPreviewSlot(0)
                                  : ParityPool::FindPlayerObjectSlot(0);
    const int p2_slot = css_phase ? ParityPool::FindCssPreviewSlot(1)
                                  : ParityPool::FindPlayerObjectSlot(1);
    FillPlayerSnapshot(snap.players[0], p1_slot);
    FillPlayerSnapshot(snap.players[1], p2_slot);

    /* [CSS-WIN] / [CSS-OBJ] -- the character-select window's object-pool gate
     * and the falling-object diagnosis (css_window.h). Dark unless
     * FM2K_CSS_WIN=1. Placed HERE, sharing the .pty's own player resolution, so
     * both planes sample the identical quantities at the identical point in the
     * frame -- the window is otherwise unmeasured by every gate in the tree. */
    CssWindow::OnCapture(g_active_recorder->frames_written, snap.match_phase,
                         p1_slot, snap.players[0].pos_y, snap.players[0].script_idx,
                         p2_slot, snap.players[1].pos_y, snap.players[1].script_idx);

    /* v2 match-level fields. rng_after_frame mirrors rng (we capture
     * post-update so they're identical). system_vars maps to FM2K's
     * dword_601B34 + idx*2 array per the [V] opcode handler -- that's
     * a 32-byte i16[16] block. */
    snap.rng_after_frame = Read32(ADDR_RNG);
    /* TODO: confirm dword_601B34 base address in WW build; if it differs,
     * patch ADDR_SYSTEM_VARS here. For now leave system_vars zeroed
     * (no FM2KHook reference for this address yet). */

    if (std::fwrite(&snap, sizeof(snap), 1, g_active_recorder->fp) == 1) {
        ++g_active_recorder->frames_written;
    }
}

void Close() {
    if (!g_active_recorder) return;
    if (g_active_recorder->fp) {
        if (std::fseek(g_active_recorder->fp,
                       offsetof(KgtParitySnapshotHeader, frame_count),
                       SEEK_SET) == 0) {
            (void)std::fwrite(&g_active_recorder->frames_written,
                              sizeof(uint32_t), 1, g_active_recorder->fp);
        }
        std::fclose(g_active_recorder->fp);
    }
    delete g_active_recorder;
    g_active_recorder = nullptr;
}

bool MaybeAutoOpen() {
    /* FM2K-ONLY. Every snapshot address (FillPlayerSnapshot / Capture) is a
     * hardcoded FM2K literal (object pool 0x4701E0, char slot 0x4D1D90, input
     * rings 0x4259C0.., etc.). On FM95 those are wrong memory, so refuse to
     * open the recorder at all -- Capture() then no-ops on the null fp and no
     * FM2K address is ever read. FM95 parity recording is a separate task with
     * its own address set (workplan Phase 2c); it will get its own snapshot
     * path, not this one. */
    if constexpr (!FM2K::kIsFM2K) {
        return false;
    }
    /* Honor FM2K_PARITY_RECORD_PATH env var: if set, open at startup.
     * Relative paths (no drive letter, no leading slash) get routed into
     * `<game_dir>/logs/` via Fm2k_BuildLogPath. Absolute paths pass through
     * unchanged. */
    const char* path = std::getenv("FM2K_PARITY_RECORD_PATH");
    if (!path || !*path) return false;
    bool is_absolute = (path[1] == ':') || path[0] == '/' || path[0] == '\\';
    if (is_absolute) return Open(path);
    char resolved[MAX_PATH];
    if (!Fm2k_BuildLogPath(resolved, sizeof(resolved), path)) {
        return Open(path);  // fallback: cwd
    }
    return Open(resolved);
}

}  /* namespace ParityRecorder */
