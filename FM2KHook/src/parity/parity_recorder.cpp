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
 *   - Slot 0 = P1 main fighter, slot 1 = P2 main fighter (after spawn)
 *   - Per-slot offsets:
 *       +0   type (4 = player)
 *       +4   facing
 *       +8   pos_x (Q14.18 fixed)
 *       +12  pos_y
 *       +16  vel_x  (TODO confirm — may be accel)
 *       +20  vel_y
 *       +0x2C item_idx (script item index / PC)
 *       +0x30 script_idx
 *       +0x40 hitstun
 *       +0x44 hitstop  (TODO confirm)
 *       +0x156 char_index
 *   - HP @ 0x470134 (P1) / 0x470138 (P2) — separate g_player_hp table
 *   - Super: g_char_value_current @ 0x4DFC95 + char_index * 0xE03F
 *   - RNG @ 0x41FB1C
 *   - Input ring index @ 0x447EE0 (g_input_buffer_index)
 *   - P1/P2 input rings @ 0x4280E0 / 0x4290E0
 *   - Match phase: TODO map from FM2K's game_state struct (0x470020+0x220) */

#include "parity_recorder.h"
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
 * and camera_manager @ 0x40AF30). Was previously 0x470134/138 — those
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
 * Camera offsets aren't yet mapped in FM2KHook source — parity diff
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

/* Locate player_idx's CHARACTER object in the pool. The old code
 * hardcoded pool slots 0/1, but P2's char object does NOT live in slot 1
 * (verified 2026-06-11: stress-mode players[1] captured a static system
 * object -- pos=(320,150), script=0/0 -- so every P2-side divergence was
 * invisible to parity_diff). Match the engine's own player scans
 * (camera_manager @ 0x40AF30, hit_detection_system @ 0x40F010): object
 * type dword == 4 AND char index at +0x156 == player_idx. Ascending scan
 * returns the first match; char objects are created at battle init in
 * low slots, well below the BG-handler objects whose +0x156 accumulators
 * could transiently equal 0/1. */
int FindPlayerObjectSlot(int player_idx) {
    for (int i = 0; i < 1024; ++i) {
        const uintptr_t a = ADDR_OBJECT_POOL_BASE +
                            static_cast<uintptr_t>(i) * OBJECT_SLOT_SIZE;
        if (Read32(a) != 4u) continue;
        if (Read32S(a + 0x156) != player_idx) continue;
        return i;
    }
    return -1;
}

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
     * 0x40F010 disasm (this session — see docs/parity_runbook.md):
     *   +0x08  pos_x (Q14.18)             confirmed
     *   +0x0C  pos_y                      confirmed
     *   +0x18  vel_x (MoveCmd target)     per docs/editor/opcode_dispatcher.md
     *   +0x1C  vel_y                      per same
     *   +0x2C  item_idx                   confirmed
     *   +0x30  script_idx                 confirmed
     *   +0x40  hitstop                    confirmed (was wrongly at +0x44)
     *   +0x5C  facing                     confirmed (was wrongly at +0x04)
     *   +0x15E state_flags                confirmed
     * (hitstun field — defender state offset — needs further RE; v1
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

    /* HP/super: not in object slot — pull from per-character data table. */
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

    /* v2: hit/hurt slot popcounts — cross-process comparable summary
     * of the 20 + 20 box-pointer arrays. The slot_idx for [C] cancel
     * needs to be DERIVED from the script-item pointer (subtract the
     * script-items base, divide by item size). For v1 of the FM2K
     * recorder we just store -1 if pointer is null, else a positive
     * "armed" sentinel — the kgt side stores the actual item index, so
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

    /* v3: task_vars per-object — direct memcpy from the slot's int16
     * array at +0x131. */
    for (int i = 0; i < TASK_VAR_COUNT; ++i) {
        dst.task_vars[i] = *reinterpret_cast<const int16_t*>(
            slot_addr + SLOT_TASK_VARS_OFFSET + i * 2u);
    }
}

}  /* anonymous namespace */

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
    /* initial_seed is patched on the first Capture() call — at Open()
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
    // entire run. Gated on FM2K_PARITY_CAPTURE_TRACE=1 — off by default.
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
            constexpr uintptr_t POOL_BASE = 0x4701E0;
            constexpr size_t POOL_STRIDE  = 382;
            constexpr size_t POOL_COUNT   = 1024;
            uint32_t s1 = 0xFFFF, s2 = 0xFFFF;
            auto mix = [&](uint32_t v) {
                s1 = (s1 + (v & 0xFFFF)) % 65535; s2 = (s2 + s1) % 65535;
                s1 = (s1 + (v >> 16))    % 65535; s2 = (s2 + s1) % 65535;
            };
            unsigned cnt = 0;
            char list[512]; size_t lp = 0; list[0] = 0;
            for (size_t i = 0; i < POOL_COUNT; i++) {
                const uint8_t* obj = (const uint8_t*)(POOL_BASE + i * POOL_STRIDE);
                const uint32_t type = *(const uint32_t*)(obj + 0x0);
                if (type == 0) continue;
                cnt++;
                mix((uint32_t)i); mix(type);
                mix(*(const uint32_t*)(obj + 0x4));   // owner
                mix(*(const uint32_t*)(obj + 0x8));   // posX
                mix(*(const uint32_t*)(obj + 0xC));   // posY
                if (lp < sizeof(list) - 20)
                    lp += (size_t)std::snprintf(list + lp, sizeof(list) - lp,
                                                "%u:%u ", (unsigned)i, type);
            }
            const uint32_t fp = (s2 << 16) | s1;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[POOLSET] seq=%u cnt=%u fp=0x%08X active=%s",
                g_active_recorder->frames_written, cnt, fp, list);
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
     * has at battle start — frame-0-of-battle rng matches by construction.
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
     * recorder-side filtering — we want every frame's state for the
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
     * empty-slot path did, so parity_diff alignment is unchanged. */
    FillPlayerSnapshot(snap.players[0], FindPlayerObjectSlot(0));
    FillPlayerSnapshot(snap.players[1], FindPlayerObjectSlot(1));

    /* v2 match-level fields. rng_after_frame mirrors rng (we capture
     * post-update so they're identical). system_vars maps to FM2K's
     * dword_601B34 + idx*2 array per the [V] opcode handler — that's
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
     * open the recorder at all — Capture() then no-ops on the null fp and no
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
