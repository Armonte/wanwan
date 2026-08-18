/* SPDX-License-Identifier: Apache-2.0 */
/* Process-INDEPENDENT object-pool facts for parity / desync diagnostics.
 *
 * WHY THIS EXISTS
 * ---------------
 * Two separate defects motivated this module, both recorded in the Wave 4.1
 * validation report and in docs/dev/spectate_deep_join_rollout.md Phase 1.5:
 *
 *  1. [SPEC-FP] / [HOST-FP] read the players' world state out of pool slots 0
 *     and 1 BY FIXED INDEX. That convention is false: FM2K allocates objects
 *     with a first-free scan (create_game_object @ 0x406570), so the two
 *     character objects sit wherever the allocator put them. In the Wave 4.1
 *     sweep a spectator held a NON-PLAYER object in slot 1 for an entire
 *     match -- the FP line printed p2_pos=(320,150) p2_script=0 (raw
 *     coordinates, not the <<16 fixed-point of a fighter) for 37+ consecutive
 *     samples and flipped the harness verdict to FAIL while the CRC gate said
 *     the frames were identical. parity_recorder.cpp had already fixed the
 *     same bug for the .pty capture in 2026-06-11 by scanning the pool the way
 *     the engine does; this header lifts that scan out so every diagnostic
 *     uses ONE resolution instead of three copies of a wrong assumption.
 *
 *  2. The [CHECKSUM] full-state fencepost hashes rng / HP / round+game timers /
 *     ring inputs (SaveState_CalculateFingerprint). It is structurally blind to
 *     pool-slot TOPOLOGY -- which objects exist, of what kind, bound to which
 *     player, at which slot indices. That is carry-state family A1 from the
 *     Wave 3.1 enumeration (a host that finished match N carries live type-4
 *     objects into the next match; the first-free allocator then places match
 *     N+1's objects at different indices on the two peers, which changes
 *     iteration and hit-resolution order). ComputeTopology() is the term that
 *     makes the fencepost see it, so the divergence is caught AT the fence
 *     instead of surfacing later as an unexplained positional desync.
 *
 * PROCESS INDEPENDENCE IS THE HARD CONSTRAINT
 * -------------------------------------------
 * Raw byte-hashing of the pool is useless across processes: [FULLFP] pool=
 * always differs because the slots embed Class-C script-stream pointers (20
 * hit + 20 hurt box pointers, the reaction table pointer) into per-process
 * heap allocations. See docs/game/pointer_audit.md section 2.1 for the full
 * classification. Everything hashed here is therefore either
 *   - a slot INDEX we computed ourselves, or
 *   - a field the audit explicitly classifies as a non-pointer integer.
 * The single pointer-typed field we look at, parent_object_ptr @ +0x17A, is
 * NEVER hashed as a pointer: it is converted to a pool slot index, and any
 * value that is not null and not exactly slot-aligned inside the pool is
 * replaced by a fixed sentinel. No pointer-derived bit can reach the digest.
 *
 * FM2K ONLY. Every address here is a WonderfulWorld_ver_0946.exe literal and
 * matches FM2K::ADDR_OBJECT_POOL / OBJECT_POOL_STRIDE in core/globals.h. The
 * literals are deliberate rather than aliases of those constants so that an
 * ENGINE_FM95 build can never silently repoint this scan at FM95's unrelated
 * 0x426A40 / 0xA4 pool. Under ENGINE_FM95 every entry point below returns a
 * neutral value and touches no memory.
 *
 * Implemented in parity_recorder.cpp (same TU, no CMake change needed). If
 * this grows, parity_pool.cpp is the natural sibling split.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace ParityPool {

/* g_object_pool @ 0x4701E0, 1024 slots x 382 bytes (docs/analysis/
 * fm2k_object_system.md). */
constexpr uintptr_t kPoolBase   = 0x4701E0u;
constexpr size_t    kSlotStride = 382u;
constexpr size_t    kSlotCount  = 1024u;

/* Per-slot offsets. Types/classes per docs/game/pointer_audit.md 2.1 and
 * docs/editor/runtime_entity.md (KgtRuntimeObject, 382 B).
 *   +0x000 status/type   dword, 0 = free slot; 4 = player character in WW
 *   +0x004 owner/kind    integer written by the creator ("Owner/Category ID")
 *   +0x008/+0x00C        posX / posY (16.16 fixed) -- state, not topology
 *   +0x030 scriptId      current script id
 *   +0x156 playerSlotId  u16, indexes the per-player 57407-byte slot table
 *   +0x15A entityKind    i32, 0 = player-owned, 1..4 = sub-object kinds
 *   +0x17A parentPtr     KgtRuntimeObject* into this same pool (Class A) */
constexpr size_t kOffType       = 0x000u;
constexpr size_t kOffOwner      = 0x004u;
constexpr size_t kOffPosX       = 0x008u;
constexpr size_t kOffPosY       = 0x00Cu;
constexpr size_t kOffScriptId   = 0x030u;
constexpr size_t kOffPlayerSlot = 0x156u;
constexpr size_t kOffEntityKind = 0x15Au;
constexpr size_t kOffParentPtr  = 0x17Au;

/* Object type value that marks a player's character object in WonderfulWorld.
 * Matches the engine's own player scans (camera_manager @ 0x40AF30,
 * hit_detection_system @ 0x40F010). */
constexpr uint32_t kTypePlayerChar = 4u;

/* One player's world state, resolved by SCAN rather than by fixed slot.
 * slot == -1 means "no character object for that player right now" (pre-spawn
 * battle frames, CSS, results). In that case pos is (0,0) and script is -1 --
 * the same not-found convention FillPlayerSnapshot uses for the .pty, so a
 * consumer sees one sentinel shape, not two. */
struct PlayerView {
    int     slot;
    int32_t pos_x;
    int32_t pos_y;
    int32_t script;
};

/* The pool-topology fencepost terms. digest == 0 means "not computed"
 * (ENGINE_FM95, or FM2K_CK_TOPOLOGY=0); a real digest can never be 0 because
 * the FNV seed is non-zero and the final mix is multiplicative.
 *
 * TWO DIGESTS, NOT ONE (Phase 4c). Until 4c a single `top=` folded the slot map
 * and the object-to-object BINDINGS together, and Phase 4b then measured that
 * on a wanwan run whose every other term was bit-identical for 5320/5320 frames
 * the entire divergence was ONE object's creator back-pointer -- so a term
 * documented as "which objects exist, of what kind, at which slot indices" was
 * in practice reporting something else, and was 100% red in passing runs. Split:
 *   digest  -- the SLOT MAP: (slot index, type) over active slots + population.
 *              This is exactly what the doc comment above claims, it is what
 *              a re-INDEXING moves, and it is what a gate can be built on.
 *   binding -- owner (+4), playerSlotId (+0x156), entityKind (+0x15A) and the
 *              normalised parent token (+0x17A), in slot order + population.
 *              Strictly more sensitive and strictly harder to interpret: kept,
 *              reported, and deliberately NOT the primary term.
 * Nothing was dropped; the seed tags are bumped so an old build's digest can
 * never compare equal to a new build's. */
struct Topology {
    uint32_t digest;
    uint32_t binding;
    uint32_t active_count;
};

/* Everything one pass over the pool can produce. */
struct Scan {
    uint32_t active_count;    /* slots with type != 0 */
    uint32_t topology;        /* process-independent SLOT-MAP digest */
    uint32_t binding;         /* process-independent BINDING digest (see above) */
    uint32_t legacy_poolset;  /* the historical [POOLSET] fingerprint, bit-for-bit
                               * (slot, type, owner, posX, posY) -- only filled
                               * when want_legacy_fp, else 0 */
};

/* Locate player_idx's CHARACTER object. Ascending scan for
 * type == kTypePlayerChar AND playerSlotId == player_idx, exactly the
 * predicate the engine's own player scans use. Returns -1 when absent. */
int FindPlayerObjectSlot(int player_idx);

/* CHARACTER-SELECT ONLY. FindPlayerObjectSlot with the ENGINE's own preview
 * predicate added: type == kTypePlayerChar && entityKind < 2 && playerSlotId ==
 * player_idx, lifted verbatim from Css_UnloadPlayerPreview @0x406520 (the same
 * predicate css_autoconfirm.cpp's [CSSPIN] census already uses).
 *
 * WHY IT EXISTS (spec_faller_diagnosis.md). create_game_object @0x406570
 * memsets 0x17C bytes, so +0x156 (playerSlotId) defaults to 0 on every object
 * that never writes it -- and in mode 2000 the character-select background
 * script children (entity_kind 3, spawned by sub_406790's scripts) can allocate
 * BELOW the preview objects. FindPlayerObjectSlot then resolves a scrolling UI
 * object as "player 0", which is what the CSS-WIN FALL term was measuring: a
 * 15 px/frame two-way scroll pair, on BOTH planes, reported as a falling
 * fighter. Its own comment already named the hazard ("BG-handler objects whose
 * +0x156 accumulators could transiently equal 0/1") on a BATTLE premise that
 * does not hold in character select.
 *
 * SCOPE: battle frames keep FindPlayerObjectSlot untouched -- it feeds the
 * CHECKSUM / CINPUT / determinism oracle and five other consumers, and its
 * battle behaviour has been validated since 2026-06-11. Returns -1 when this
 * player has no preview object this frame (the ~43 engine teardown frames per
 * window), which the .pty already encodes as script_idx == -1 and both harness
 * terms already skip as "no character object this frame". */
int FindCssPreviewSlot(int player_idx);

/* FindPlayerObjectSlot + the three fields every FP line prints. */
PlayerView ReadPlayer(int player_idx);

/* Per-frame fencepost term. O(kSlotCount) with one dword load per inactive
 * slot and ~6 loads + ~6 multiplies per active slot; no division, no syscall,
 * no allocation. Returns {0,0} when disabled. */
Topology ComputeTopology();

/* Full single-pass scan. active_list (may be null) receives the legacy
 * "idx:type " listing used by the [POOLSET] diagnostic, always
 * NUL-terminated and truncated to fit list_cap. */
Scan ScanPool(bool want_legacy_fp, char* active_list, size_t list_cap);

/* Phase 4b DIAGNOSTIC (docs/dev/ seam campaign corpus): per-active-slot dump of
 * exactly the three topology members the legacy [POOLSET] fingerprint does NOT
 * cover -- playerSlotId (+0x156), entityKind (+0x15A) and the normalised parent
 * slot token (+0x17A). The 4b runs showed `fp=` (slot,type,owner,posX,posY)
 * IDENTICAL host-vs-spectator on 3984/3984 frames while `top=` differed on
 * 3984/3984, so the divergent field is provably one of these three and nothing
 * else.
 *
 * Record format (2026-08-17, Lane C bind= probe):
 *     "slot:playerSlotId:entityKind:parentSlotToken:RAW17A:createdFrame "
 * The last two are NEW and DIAGNOSTIC ONLY -- neither reaches any digest:
 *   RAW17A       the UNNORMALISED +0x17A dword, hex. Lane C's triage: `bind=`
 *                is red from the FIRST paired frame with parentSlotToken
 *                normalising to different in-pool slots on the two planes, and
 *                the cheapest question that decides whether an IDA writer-pass
 *                is justified is whether BOTH planes hold in-pool pointers (the
 *                creator genuinely differs) or one holds a heap value (the
 *                framing changes entirely).
 *   createdFrame the engine frame counter (0x4456FC) on the first scan that saw
 *                this slot occupied after seeing it free; 0xFFFFFFFF when the
 *                slot was already occupied on the very first scan. SAMPLED at
 *                this function's cadence, not read from the object -- there is
 *                no creation-frame field in KgtRuntimeObject.
 *
 * NUL-terminated, truncated to cap, and the trailing "(n=emitted/active)"
 * marker (prefixed TRUNCATED when it applies) is always present. Returns the
 * written length so the caller can chunk it. Rides the [POOLSET] gate
 * (FM2K_POOLSET=1); nothing calls it otherwise. */
size_t DumpTopoDetail(char* out, size_t cap);

/* False under ENGINE_FM95 or when FM2K_CK_TOPOLOGY is set to 0. Callers do
 * not need to check this -- ComputeTopology() already honours it -- it is
 * exposed so a diagnostic can say "off" rather than print a zero. */
bool TopologyEnabled();

}  /* namespace ParityPool */
