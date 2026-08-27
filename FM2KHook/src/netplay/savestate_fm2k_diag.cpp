// savestate_fm2k_diag.cpp -- FM2K snapshot/compare/fingerprint/full-checksum/roundtrip + desync diagnostic dump + diag region table. Split (engine x concern) from savestate.cpp.
#if !defined(ENGINE_FM95)
#include "savestate.h"
#include "savestate_internal.h"
#include "globals.h"
#include <SDL3/SDL_log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <windows.h>

namespace SnapshotAddrs {
    constexpr uintptr_t FRAME_COUNTER = 0x447EE0;  // Input buffer index serves as frame
    constexpr uintptr_t P1_OBJ_BASE = ADDR_OBJECT_POOL;
    constexpr uintptr_t P2_OBJ_BASE = ADDR_OBJECT_POOL + OBJECT_POOL_STRIDE;
    constexpr uintptr_t P1_HP = 0x470134;
    constexpr uintptr_t P2_HP = 0x470138;
}

StateSnapshot SaveState_CaptureSnapshot() {
    StateSnapshot snap = {};

    snap.rng_seed = *(uint32_t*)ADDR_RNG_SEED;
    snap.input_buffer_index = *(uint32_t*)ADDR_INPUT_BUFFER_INDEX;
    snap.frame_counter = g_frame_counter;

    // Player 1 position (offset 8 and 12 in object structure)
    snap.p1_x = *(uint32_t*)(SnapshotAddrs::P1_OBJ_BASE + 8);
    snap.p1_y = *(uint32_t*)(SnapshotAddrs::P1_OBJ_BASE + 12);

    // Player 2 position
    snap.p2_x = *(uint32_t*)(SnapshotAddrs::P2_OBJ_BASE + 8);
    snap.p2_y = *(uint32_t*)(SnapshotAddrs::P2_OBJ_BASE + 12);

    // HP
    snap.p1_hp = *(uint32_t*)SnapshotAddrs::P1_HP;
    snap.p2_hp = *(uint32_t*)SnapshotAddrs::P2_HP;

    // Full checksum
    snap.checksum = SaveState_CalculateFullChecksum();

    return snap;
}

bool SaveState_CompareSnapshots(const StateSnapshot& a, const StateSnapshot& b, char* diff_buf, size_t buf_size) {
    bool match = true;
    diff_buf[0] = '\0';
    char* p = diff_buf;
    size_t remaining = buf_size;

#define CHECK_FIELD(field) \
    if (a.field != b.field) { \
        int written = snprintf(p, remaining, #field ": 0x%X vs 0x%X\n", a.field, b.field); \
        if (written > 0 && (size_t)written < remaining) { p += written; remaining -= written; } \
        match = false; \
    }

    CHECK_FIELD(rng_seed);
    CHECK_FIELD(input_buffer_index);
    CHECK_FIELD(p1_x);
    CHECK_FIELD(p1_y);
    CHECK_FIELD(p2_x);
    CHECK_FIELD(p2_y);
    CHECK_FIELD(p1_hp);
    CHECK_FIELD(p2_hp);
    CHECK_FIELD(checksum);

#undef CHECK_FIELD

    return match;
}

// (g_region_checksums defined earlier for forward-reference in SaveState_Save)

const RegionChecksums& SaveState_GetRegionChecksums() {
    return g_region_checksums;
}



// Cheap path: gameplay fingerprint only -- what GekkoNet actually compares
// for desync detection. Called on every save. ~44 B of hashing.
//
// WHAT IS IN IT (and what is NOT): exactly seven scalars -- rng, p1_hp, p2_hp,
// round_timer (0x470060), game_timer (0x470044), p1_input, p2_input. There are
// NO POSITION FIELDS, no object pool, no list topology, no afterimage pool.
// Several comments elsewhere used to call this an "HP/pos/rng/timer" hash; they
// were wrong and have been corrected. This is GDC tooling gap #1.
//
// WHY POSITIONS ARE NOT ADDED HERE (deliberate, 2026-08-08):
//   * This value is SESSION-KILLING. A mismatch reaches HandleDesyncDetected,
//     which dumps and then TerminateProcess()es BOTH peers' games. Every field
//     added here becomes a new way for a live match to die.
//   * The comparison itself is safe against rollback transients -- gekko's
//     SendSessionHealthCheck only ever exchanges the checksum of
//     `current - prediction_window - 1`, a CONFIRMED frame -- so the risk is
//     not "benign prediction noise". The risk is a field that is legitimately
//     NOT part of the rolled-back contract. Positions live in the object pool,
//     which is exactly where that hazard is known to exist today:
//     trampoline_render.cpp does not revert render-written object-pool state,
//     and savestate_fm2k_load.cpp skips slot +68..+84 on restore. Promoting a
//     region with a live, unresolved "is this actually rolled back" question
//     into the kill-the-match term is the wrong order of operations.
//   * It would not have caught the bug that prompted this review: in the
//     analysed capture the positions were IDENTICAL on both peers at the
//     divergent frame (the divergence was a settings mismatch stamped at
//     RSS_BATTLE_INIT, now gated at the battle-entry barrier).
//   * The coverage gap is already being closed OUT OF BAND and non-fatally:
//     the [CHECKSUM] line in netplay_battle_events.cpp emits a
//     process-independent pool-topology term (top=/nobj=) alongside the
//     checksum, deliberately NOT folded into it.
// REVISIT WHEN: the topology/POOLSET term has soaked across a large corpus
// with zero cross-peer differences on confirmed frames, and the render-written
// object-pool question in savestate_fm2k_load.cpp is settled. Then positions
// can be promoted with evidence instead of hope.
uint32_t SaveState_CalculateFingerprint() {
    g_region_checksums.rng = *(uint32_t*)ADDR_RNG_SEED;

    uint32_t buf_idx = *(uint32_t*)0x447EE0;
    uint32_t idx     = buf_idx & 0x3FF;
    // uint32 PER ENTRY, not uint16. Proven twice in the engine's own code:
    // process_game_inputs writes `mov DWORD PTR [edx*4+0x4280e0],eax` (0x41472E)
    // and the motion-command check reads `mov edi,[ebx+ebp*4]` (0x410AC5) --
    // both a 4-byte stride. SIZE_INPUT_HISTORY (0x2008) agrees: 2 players x
    // 1024 entries x 4 bytes + an 8-byte header.
    //
    // Reading it as uint16 indexed by `idx` did not read frame `idx` at all --
    // it read the low half of entry idx/2. That made p1_input/p2_input in the
    // seam ring garbage, and the consequence was NOT cosmetic:
    // seam_ring_check exempts a group as an INPUT CORRECTION (forward pass ran
    // on a PREDICTED input, resim on the corrected one -- normal rollback)
    // only when an input field differs. With both fields reading the same
    // wrong element, a genuine input correction looked input-identical and was
    // reported as a determinism VIOLATION instead.
    const uint32_t* p1_hist = (const uint32_t*)0x4280E0;
    const uint32_t* p2_hist = (const uint32_t*)0x4290E0;
    uint16_t p1_in = (uint16_t)(p1_hist[idx] & 0xFFFF);
    uint16_t p2_in = (uint16_t)(p2_hist[idx] & 0xFFFF);
    struct __attribute__((packed)) Fingerprint {
        uint32_t rng;
        uint32_t p1_hp;
        uint32_t p2_hp;
        uint32_t round_timer;
        uint32_t game_timer;
        uint16_t p1_input;
        uint16_t p2_input;
    } fp = {
        *(uint32_t*)0x41FB1C,
        *(uint32_t*)0x4DFC85,
        *(uint32_t*)0x4EDCC4,
        *(uint32_t*)0x470060,
        *(uint32_t*)0x470044,
        p1_in, p2_in,
    };
    g_region_checksums.gameplay_fingerprint =
        Fletcher32((const uint8_t*)&fp, sizeof(fp));

    // Stash the raw scalars too. They're cheap (32 bytes) and on a
    // fingerprint-DIFF dump we can name the specific field that diverged
    // without needing FM2K_FULL_CRCS or per-region CRCs at all.
    g_region_checksums.fp_inputs = {
        fp.rng, fp.p1_hp, fp.p2_hp, fp.round_timer, fp.game_timer,
        buf_idx, p1_in, p2_in,
    };
    return g_region_checksums.gameplay_fingerprint;
}

uint32_t SaveState_CalculateFullChecksum() {
    // Expensive diagnostic path: ~1 MB hashing per call. Callers should
    // only hit this when they need per-region CRCs (the replay-diff scan,
    // throttled to 1/sec). Every normal save uses
    // SaveState_CalculateFingerprint() above instead.
    g_region_checksums.rng = *(uint32_t*)ADDR_RNG_SEED;

    // Game state CRC. Only ONE field is legitimately excluded:
    //   0x4701CC (4 bytes): g_hInstance -- different per process, never sim state.
    //
    // 0x470050 USED TO BE EXCLUDED TOO, mislabelled "g_score_value - CSS frame
    // counter residue". It is not residue: g_score_value is the ROUND-TIMER
    // COUNTDOWN (docs/c3.5_round_events_ida_handoff.md -- RSS_BATTLE_INIT sets
    // it to 100 * stage_timer - 1, substate 200 decrements it once per frame,
    // and round_events.cpp derives the [ROUND-START]/[ROUND-END] `timer=` from
    // it). It sits inside the saved region (ADDR_GAME_STATE + SIZE_GAME_STATE)
    // so it IS rolled back, and it is the value that decides WHEN the round
    // ends -- yet it was in neither fencepost nor GekkoNet's fingerprint. That
    // is precisely why the lost-HOST_CONFIG desync (host on a 15s timer, guest
    // silently on its own 60s) looked bit-identical for ~1500 frames before
    // surfacing in the round-end tail. Hashing it makes that divergence trip at
    // battle frame 0, where it is actually caused.
    //
    // Scoped to battle mode, which is the ONE part of the old carve-out that
    // might have had a basis: out of battle nothing maintains this counter, so
    // whatever the previous match left there is genuinely uninteresting and
    // could differ across peers for benign reasons (a peer that spent a
    // different number of frames in the round-end tail). In battle both peers
    // stamp it from the same settings at RSS_BATTLE_INIT and decrement it in
    // lockstep, so any difference is real. `combined` is a DIAGNOSTIC term
    // (FULLFP / the replay-diff scan / the desync dump) -- GekkoNet's live
    // desync compare uses gameplay_fingerprint, which this does not touch --
    // so widening it cannot kill a session.
    const uint32_t game_mode_now = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (game_mode_now >= 3000 && game_mode_now < 4000) {
        // Contiguous 0x470020..0x4701CC: g_score_value included.
        g_region_checksums.game_state =
            Fletcher32((uint8_t*)ADDR_GAME_STATE, 0x4701CC - ADDR_GAME_STATE);
    } else {
        // Region 1: 0x470020 to 0x470050 (0x30 bytes)
        // Region 2: 0x470054 to 0x4701CC (0x178 bytes)
        uint32_t gs1 = Fletcher32((uint8_t*)ADDR_GAME_STATE, 0x470050 - ADDR_GAME_STATE);
        uint32_t gs2 = Fletcher32((uint8_t*)0x470054, 0x4701CC - 0x470054);
        g_region_checksums.game_state = gs1 ^ gs2;
    }

    // Object pool: hash the FULL pool, not just the first 4KB.
    // Previous 4 KB covered only ~10 objects; stress-mode desync at frame 273
    // showed Obj[7] CRC changing without the hashed region moving, meaning the
    // 4KB hash was silent to divergence past object 10. Hash the full 391 KB
    // so ANY object mutation shows up in the combined checksum immediately.
    g_region_checksums.object_pool = Fletcher32((uint8_t*)ADDR_OBJECT_POOL, SIZE_OBJECT_POOL);

    // Character dynamic state -- only hash loaded slots (first byte != 0
    // indicates .kgt magic "2DKG"). In 1v1 that's 2 of 8, trimming this
    // cost from 460 KB to ~115 KB.
    uint32_t char_ck = 0;
    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t slot_base = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE);
        if (*(uint8_t*)slot_base == 0) continue;
        uintptr_t dynamic_addr = slot_base + CHAR_SLOT_DYNAMIC_OFFSET;
        char_ck ^= Fletcher32((uint8_t*)dynamic_addr, CHAR_SLOT_DYNAMIC_SIZE);
    }
    g_region_checksums.char_dynamic = char_ck;

    // Input tracking CRC: only hash the actual input state, skip screen dimensions
    // (wDest/hDest/g_screen_x/g_screen_y at 0x447F20-0x447F3F may differ between instances)
    uint32_t it1 = Fletcher32((uint8_t*)0x447EE0, 0x447F00 - 0x447EE0);   // buf_idx region
    uint32_t it2 = Fletcher32((uint8_t*)0x447F00, 0x447F20 - 0x447F00);   // g_prev_input_state
    uint32_t it3 = Fletcher32((uint8_t*)0x447F40, 0x447F80 - 0x447F40);   // g_processed_input + g_input_changes
    // History ring buffers: 1024 frames x 4 BYTES per player (uint32, see the
    // stride proof in SaveState_CalculateFingerprint), so a FULL hash would be
    // 0x1000 each and the 0x800 below covers only the first 512 entries.
    //
    // DELIBERATELY LEFT AT 0x800 FOR NOW. Widening it to 0x1000 was tried and
    // backed out: it is not needed for the seam false-positive fix (that is the
    // uint32 stride in the fingerprint plus the classifier ordering), and it
    // changes the combined checksum that the SPECTATOR comparison uses. A
    // spectator that joined mid-battle can hold different residue in the
    // upper half of its ring, which the widened hash would report as a
    // full-state desync from frame 0 -- a new false-positive class in exchange
    // for coverage nobody has shown to matter. Widen it only alongside proof
    // that both sides' upper halves are meant to agree. Must match across
    // peers byte-for-byte -- if a facing/slot swap mutates the stored input on
    // one side and not the other, these CRCs diverge on frame 1 and give us
    // an early desync signal instead of waiting for the cascade into game_state.
    uint32_t it4 = Fletcher32((uint8_t*)0x4280E0, 0x800);                 // g_p1_input_history: FIRST HALF ONLY -- see note
    uint32_t it5 = Fletcher32((uint8_t*)0x4290E0, 0x800);                 // g_p2_input_history: FIRST HALF ONLY -- see note
    g_region_checksums.input_tracking = it1 ^ it2 ^ it3 ^ it4 ^ it5;

    // Effect/shake regions (now saved for rollback)
    g_region_checksums.effect_sys1 = Fletcher32((uint8_t*)EffectAddrs::EFFECT_SYS1, EffectAddrs::EFFECT_SYS1_SZ);
    g_region_checksums.effect_sys2 = Fletcher32((uint8_t*)EffectAddrs::EFFECT_SYS2, EffectAddrs::EFFECT_SYS2_SZ);
    g_region_checksums.shake_effects = Fletcher32((uint8_t*)EffectAddrs::SHAKE_EFFECTS, EffectAddrs::SHAKE_EFFECTS_SZ);

    // Wave C regions -- PREVIOUSLY SAVED BUT NOT HASHED.
    // These silently diverged until their effects leaked into the object pool
    // or slot state. Hashing them now means any divergence trips the combined
    // checksum at its source frame instead of cascading.
    uint32_t afterimage_ck = Fletcher32((uint8_t*)WaveCAddrs::AFTERIMAGE_POOL,      WaveCAddrs::AFTERIMAGE_POOL_SZ);
    uint32_t list_heads_ck = Fletcher32((uint8_t*)WaveCAddrs::OBJECT_LIST_HEADS,    WaveCAddrs::OBJECT_LIST_HEADS_SZ);
    uint32_t node_pool_ck  = Fletcher32((uint8_t*)WaveCAddrs::OBJECT_NODE_POOL,     WaveCAddrs::OBJECT_NODE_POOL_SZ);
    uint32_t cur_obj_ptr   = *(uint32_t*)WaveCAddrs::CURRENT_OBJECT_PTR;

    // Combined CRC for GekkoNet desync detection.
    // Principle (matches GekkoNet examples): hash every byte you save. Any
    // saved-but-not-hashed region silently diverges until it leaks elsewhere.
    // Only render_fc and g_hInstance are legitimately process-local and stay
    // out of the hash (render_fc: render-only counter; g_hInstance: excluded
    // upstream from the game_state sub-hash).
    //
    // If this triggers an immediate-start desync, the CSS residue theory that
    // justified the old exclusions is real and must be fixed at the residue
    // source (zero residue deterministically on both peers before
    // SaveState_Init), NOT by re-excluding regions here.
    {
        uint32_t combined = 0x811C9DC5;  // FNV offset basis
        auto mix = [&](uint32_t v) {
            combined ^= v;
            combined *= 0x01000193;
        };
        mix(g_region_checksums.rng);
        mix(g_region_checksums.game_state);
        mix(g_region_checksums.input_tracking);
        mix(g_region_checksums.object_pool);
        mix(g_region_checksums.char_dynamic);
        mix(g_region_checksums.effect_sys1);
        mix(g_region_checksums.effect_sys2);
        mix(g_region_checksums.shake_effects);
        // Wave C regions -- now properly included
        mix(afterimage_ck);
        mix(list_heads_ck);
        mix(node_pool_ck);
        mix(cur_obj_ptr);
        g_region_checksums.combined = combined;
    }

    // Fingerprint is populated by SaveState_CalculateFingerprint() on every
    // save; no need to recompute here.
    return g_region_checksums.combined;
}

bool SaveState_TestRoundtrip() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== SAVESTATE ROUNDTRIP TEST ===");

    // Capture state BEFORE save
    StateSnapshot before = SaveState_CaptureSnapshot();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "BEFORE: rng=0x%08X p1=(%d,%d) p2=(%d,%d) checksum=0x%08X",
        before.rng_seed, before.p1_x, before.p1_y, before.p2_x, before.p2_y, before.checksum);

    // Save state at frame 9999 (special test slot)
    int test_frame = 9999 % MAX_ROLLBACK_FRAMES;
    SaveState_Save(test_frame);

    // Corrupt state to verify load works
    *(uint32_t*)ADDR_RNG_SEED = 0xDEADBEEF;
    *(uint32_t*)(SnapshotAddrs::P1_OBJ_BASE + 8) = 0x12345678;

    StateSnapshot corrupted = SaveState_CaptureSnapshot();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CORRUPTED: rng=0x%08X p1=(%d,%d) checksum=0x%08X",
        corrupted.rng_seed, corrupted.p1_x, corrupted.p1_y, corrupted.checksum);

    // Load state back
    SaveState_Load(test_frame);

    // Capture state AFTER load
    StateSnapshot after = SaveState_CaptureSnapshot();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "AFTER: rng=0x%08X p1=(%d,%d) p2=(%d,%d) checksum=0x%08X",
        after.rng_seed, after.p1_x, after.p1_y, after.p2_x, after.p2_y, after.checksum);

    // Compare
    char diff_buf[1024];
    bool success = SaveState_CompareSnapshots(before, after, diff_buf, sizeof(diff_buf));

    if (success) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ROUNDTRIP TEST: PASSED - State restored correctly!");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ROUNDTRIP TEST: FAILED - Differences:\n%s", diff_buf);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== END ROUNDTRIP TEST ===");
    return success;
}

// =============================================================================
// BBBR-STYLE DESYNC DIAGNOSTIC DUMP
// Writes per-region CRC + hex dump to file for easy comparison between P1/P2
// =============================================================================

// Named memory regions for diagnostic comparison (like BBBR's memory_regions.hpp)
struct DiagRegion {
    uintptr_t addr;
    size_t size;
    const char* name;
    bool exclude_from_crc;  // If true, saved/restored but not in desync CRC
};

static const DiagRegion g_diag_regions[] = {
    { 0x41FB1C,     4, "RNG_Seed",                    false },
    { 0x4456FC,     4, "RenderFrameCounter",           true  },  // Render-only, diverges
    { 0x447EE0,  0xA0, "InputTrackingState",           false },
    { 0x4280D8, 0x2008,"InputHistory",                 false },
    { 0x470020,  0x24, "PlayerStagePositions",         false },
    { 0x470044,  0x10, "GameTimerAndRound",            false },  // g_game_timer, g_round_limit, g_round_state, g_score_value
    { 0x470054,  0x14, "GameModeAndRoundTimer",        false },  // g_game_mode through g_round_setting
    { 0x47006C,  0x80, "PlayerMoveHistory",            false },
    { 0x4700EC,  0x20, "RoundCounts",                  false },  // g_p1_round_count, g_p1_round_state
    { 0x47010C,  0x10, "DemoModeState",                false },
    { 0x47011C,  0x80, "PlayerActionHistory",          false },
    { 0x47019C,   0x4, "P1ActionState",                false },
    { 0x4701A0,   0x4, "P2ActionState",                false },
    { 0x4701BC,   0x4, "GamePaused",                   false },
    { 0x4701C0,   0x4, "ReplayMode",                   false },
    { 0x4701C4,   0x8, "HitEffectState",               false },
    { 0x4701CC,   0x4, "hInstance",                     true  },  // ALWAYS different per process
    // Object pool windows -- hex dumps for small ones (<=256B), CRCs for large.
    // Combined CRC now covers the FULL 391 KB pool; these windows localize any
    // divergence to specific object-index ranges in the diagnostic log.
    { 0x4701E0, 0x100, "ObjectPool_First256",          false },  // Obj[0] first 256 B
    { 0x4702E0, 0x100, "ObjectPool_Second256",         false },  // Obj[0] tail + Obj[1] first 130 B
    { 0x470C52, 0x100, "ObjectPool_Obj7_first256",     false },  // Obj[7] -- was changing invisibly pre-fix
    { 0x4703E0, 0x800, "ObjectPool_Obj_2_10_CRC",      false },  // Obj[2..10] aggregate CRC (no hex)
    { 0x471000, 0x800, "ObjectPool_Obj_11_20_CRC",     false },  // Obj[11..20] aggregate CRC
    { 0x4720C2, 0x2000,"ObjectPool_Obj_21_42_CRC",     false },  // Obj[21..42] aggregate CRC (approx)
    { 0x447D7D,  0x2A, "EffectSystem1",                false },  // saved + hashed
    { 0x4456D0,  0x2C, "EffectSystem2",                false },  // saved + hashed
    { 0x447DA9,  0x28, "ShakeEffects",                 false },  // saved + hashed
    // Wave C regions -- now saved AND hashed in combined CRC. Showing their
    // per-region CRCs here lets us pinpoint which of these 4 newly-hashed
    // regions is the divergent one when the next stress desync fires.
    { 0x4259A8,   0x4, "CurrentObjectPtr",             false },  // g_current_object_ptr
    { 0x430240, 0x400, "ObjectListHeadsTails",         false },  // 256 heads + 256 tails
    { 0x4CFA20,0x2000, "ObjectNodePool",               false },  // 1024 × 8-byte nodes
    { 0x447930,0x27D90,"AfterimagePool",               false },  // 162,704 B (includes preamble)
};
static constexpr int NUM_DIAG_REGIONS = sizeof(g_diag_regions) / sizeof(g_diag_regions[0]);

void SaveState_DumpDesyncDiagnostic(int frame, uint32_t local_crc, uint32_t remote_crc, int player_index) {
    char filename[256];
    if (g_stress_mode) {
        // Stress-mode desyncs are single-instance determinism failures --
        // tag the file so they're distinguishable from online desyncs.
        snprintf(filename, sizeof(filename), "FM2K_stress_desync_f%d.log", frame);
    } else {
        snprintf(filename, sizeof(filename), "FM2K_P%d_desync_f%d.log", player_index + 1, frame);
    }

    FILE* f = fopen(filename, "w");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to open desync diagnostic file: %s", filename);
        return;
    }

    fprintf(f, "=== FM2K DESYNC DIAGNOSTIC%s ===\n", g_stress_mode ? " (STRESS TEST)" : "");
    fprintf(f, "Player: P%d%s\n", player_index + 1,
            g_stress_mode ? " (stress mode - single instance determinism failure)" : "");
    fprintf(f, "Frame: %d\n", frame);
    fprintf(f, "Local CRC: 0x%08X\n", local_crc);
    fprintf(f, "Remote CRC: 0x%08X\n", remote_crc);
    // Gameplay fingerprint: if this matches across peers the desync is a
    // memory-residue false positive. Dump raw values so the diff points
    // directly at the first gameplay field that differs, if any.
    fprintf(f, "\n=== Gameplay Fingerprint (HP/rng/timer/current-input ONLY) ===\n");
    fprintf(f, "  fingerprint_crc = 0x%08X\n", g_region_checksums.gameplay_fingerprint);
    fprintf(f, "  rng        = 0x%08X  (g_rand_seed @ 0x41FB1C)\n", *(uint32_t*)0x41FB1C);
    {
        uint32_t buf_idx = *(uint32_t*)0x447EE0 & 0x3FF;
        uint16_t p1_in = ((const uint16_t*)0x4280E0)[buf_idx];
        uint16_t p2_in = ((const uint16_t*)0x4290E0)[buf_idx];
        fprintf(f, "  buf_idx    = %u  (slot %u of 1024)\n",
                *(uint32_t*)0x447EE0, buf_idx);
        fprintf(f, "  p1_input   = 0x%04X  (g_p1_input_history[buf_idx])\n", p1_in);
        fprintf(f, "  p2_input   = 0x%04X  (g_p2_input_history[buf_idx])\n", p2_in);
    }
    fprintf(f, "  p1_hp      = %d  (g_p1_hp @ 0x4DFC85)\n", *(int32_t*)0x4DFC85);
    fprintf(f, "  p2_hp      = %d  (g_p2_hp @ 0x4EDCC4, = g_p1_hp + 57407)\n", *(int32_t*)0x4EDCC4);
    fprintf(f, "  game_timer = %u  (g_game_timer @ 0x470044)\n", *(uint32_t*)0x470044);
    fprintf(f, "  round_timer= %u  (g_round_timer @ 0x470060)\n\n", *(uint32_t*)0x470060);
    // Legacy demo-mode fields -- retain for backward diff compatibility. These
    // used to be (mis-)labeled as p1_hp / p1_max_hp / p2_hp / p2_max_hp in
    // pre-audit dumps; they are actually g_demo_mode_player_id / g_demo_mode_hp
    // etc. Keep them visible so pre-existing log comparison scripts don't
    // break, but make clear these are NOT gameplay HP.
    fprintf(f, "  [legacy-aliases, not HP]\n");
    fprintf(f, "    g_demo_mode_player_id @ 0x47010C = %u\n", *(uint32_t*)0x47010C);
    fprintf(f, "    g_demo_mode_hp        @ 0x470110 = %u\n", *(uint32_t*)0x470110);
    fprintf(f, "\n");

    // ============================================================================
    // FORWARD-VS-REPLAY PER-REGION CRC DIFF
    // Compares the per-region CRCs captured at TWO save events for the same
    // frame: the forward-sim save (first time we reached this frame) and the
    // replay save (after Load+Advance re-simulated this frame). Any region
    // whose CRC differs between these two snapshots is a region whose state
    // was NOT deterministically reproduced from the saved state -- that IS
    // the bug source. Same logical moment on both sides -- no timing artifacts.
    // ============================================================================
    {
        int saved_frame = frame < 0 ? 0 : frame;
        int slot = saved_frame % MAX_ROLLBACK_FRAMES;
        const auto& fwd    = g_state_buffer[slot].saved_region_crcs;
        const auto& replay = g_replay_saves[slot].crcs;
        const bool have_replay = (g_replay_saves[slot].valid &&
                                  g_replay_saves[slot].frame_number == saved_frame);

        fprintf(f, "\n=== FORWARD-vs-REPLAY Per-Region Diff ===\n");
        if (!fwd.valid) {
            fprintf(f, "  (no forward snapshot for frame %d -- rollback buffer stale)\n", saved_frame);
        } else if (!have_replay) {
            fprintf(f, "  (no replay snapshot for frame %d -- desync may have fired before\n"
                       "   GekkoNet executed a Load+Advance for this frame; only forward shown)\n", saved_frame);
            auto fwd_row = [&](const char* n, uint32_t v) {
                fprintf(f, "  %-24s  0x%08X (forward only)\n", n, v);
            };
            fwd_row("RNG_Seed",             fwd.rng);
            fwd_row("GameState",            fwd.game_state);
            fwd_row("ObjectPool(full)",     fwd.object_pool);
            fwd_row("CharDynamic(8 slots)", fwd.char_dynamic);
            fwd_row("InputTracking",        fwd.input_tracking);
            fwd_row("AfterimagePool",       fwd.afterimage_pool);
            fwd_row("ObjectListHeadsTails", fwd.list_heads_tails);
            fwd_row("ObjectNodePool",       fwd.node_pool);
            fwd_row("CurrentObjectPtr",     fwd.current_object_ptr_val);
            fwd_row("GameplayFingerprint",  fwd.gameplay_fingerprint);
            fwd_row("CombinedCRC",          fwd.combined);
        } else {
            fprintf(f, "  %-24s  %-10s  %-10s  %s\n",
                    "region", "forward", "replay", "status");
            auto row = [&](const char* name, uint32_t a, uint32_t b) {
                const char* status = (a == b) ? "MATCH" : "*** DIFF ***";
                fprintf(f, "  %-24s  0x%08X  0x%08X  %s\n", name, a, b, status);
            };
            row("RNG_Seed",             fwd.rng,                    replay.rng);
            row("GameState",            fwd.game_state,             replay.game_state);
            row("ObjectPool(full)",     fwd.object_pool,            replay.object_pool);
            row("CharDynamic(8 slots)", fwd.char_dynamic,           replay.char_dynamic);
            row("InputTracking",        fwd.input_tracking,         replay.input_tracking);
            row("EffectSystem1",        fwd.effect_sys1,            replay.effect_sys1);
            row("EffectSystem2",        fwd.effect_sys2,            replay.effect_sys2);
            row("ShakeEffects",         fwd.shake_effects,          replay.shake_effects);
            row("AfterimagePool",       fwd.afterimage_pool,        replay.afterimage_pool);
            row("ObjectListHeadsTails", fwd.list_heads_tails,       replay.list_heads_tails);
            row("ObjectNodePool",       fwd.node_pool,              replay.node_pool);
            row("CurrentObjectPtr",     fwd.current_object_ptr_val, replay.current_object_ptr_val);
            row("GameplayFingerprint",  fwd.gameplay_fingerprint,   replay.gameplay_fingerprint);
            row("CombinedCRC",          fwd.combined,               replay.combined);
            fprintf(f,
                "\n  NOTE: forward = CRCs at the FIRST save for frame %d (forward-sim).\n"
                "        replay  = CRCs at the SECOND save for frame %d (after Load+Advance).\n"
                "        Both captured at save time -- no timing artifacts from later mutation.\n"
                "        Any *** DIFF *** row is the bug: that region replayed nondeterministically.\n",
                saved_frame, saved_frame);

            // Raw scalar inputs to the fingerprint hash. Always captured (no
            // throttle gate), so when fingerprint diverges we know which
            // specific field broke determinism even without FM2K_FULL_CRCS.
            if (fwd.gameplay_fingerprint != replay.gameplay_fingerprint) {
                const auto& a = fwd.fp_in;
                const auto& b = replay.fp_in;
                fprintf(f, "\n=== Fingerprint Input Diff (forward vs replay) ===\n");
                fprintf(f, "  %-14s  %-12s  %-12s  %s\n",
                        "field", "forward", "replay", "status");
                auto fp_row32 = [&](const char* n, uint32_t x, uint32_t y) {
                    fprintf(f, "  %-14s  0x%08X    0x%08X    %s\n",
                            n, x, y, (x == y) ? "MATCH" : "*** DIFF ***");
                };
                auto fp_row16 = [&](const char* n, uint16_t x, uint16_t y) {
                    fprintf(f, "  %-14s  0x%04X        0x%04X        %s\n",
                            n, x, y, (x == y) ? "MATCH" : "*** DIFF ***");
                };
                fp_row32("rng",         a.rng,         b.rng);
                fp_row32("p1_hp(u32)",  a.p1_hp,       b.p1_hp);
                fp_row32("p2_hp(u32)",  a.p2_hp,       b.p2_hp);
                fp_row32("round_timer", a.round_timer, b.round_timer);
                fp_row32("game_timer",  a.game_timer,  b.game_timer);
                fp_row32("buf_idx",     a.buf_idx,     b.buf_idx);
                fp_row16("p1_input",    a.p1_input,    b.p1_input);
                fp_row16("p2_input",    a.p2_input,    b.p2_input);
                fprintf(f,
                    "  NOTE: p1_hp/p2_hp are read as uint32 from unaligned u16 HP\n"
                    "        addresses (0x4DFC85 / 0x4EDCC4) -- upper 16 bits include\n"
                    "        adjacent CharDynamic bytes. A DIFF in only those upper\n"
                    "        bits points at a neighboring field, not HP itself.\n"
                    "        p?_input is read from input_history[buf_idx & 0x3FF].\n");
            }

            // Per-object-slot breakdown. If ObjectPool(full) differed, this
            // pinpoints the exact slot indices. The per-slot CRCs aren't
            // computed at save time any more (too expensive -- see
            // SaveState_Save) so we derive them here from the FORWARD save
            // buffer (state->object_pool, which is still live in the rollback
            // ring). Replay bytes aren't retained, so we compute replay-side
            // per-slot CRCs from live memory -- valid as long as we're dumping
            // shortly after the desync before many more frames have run.
            if (fwd.object_pool != replay.object_pool) {
                fprintf(f, "\n=== Object Slot Divergence ===\n");
                int printed = 0;
                const uint8_t* fwd_bytes = g_state_buffer[slot].object_pool;
                const uint8_t* rep_bytes = (const uint8_t*)ADDR_OBJECT_POOL;
                for (size_t i = 0; i < SaveStateData::SavedRegionCRCs::OBJ_SLOT_COUNT; i++) {
                    const size_t off = i * SaveStateData::SavedRegionCRCs::OBJ_SLOT_SIZE;
                    uint32_t fwd_c = Fletcher32(fwd_bytes + off,
                                                 SaveStateData::SavedRegionCRCs::OBJ_SLOT_SIZE);
                    uint32_t rep_c = Fletcher32(rep_bytes + off,
                                                 SaveStateData::SavedRegionCRCs::OBJ_SLOT_SIZE);
                    if (fwd_c != rep_c) {
                        uintptr_t obj_base =
                            ADDR_OBJECT_POOL + i * SaveStateData::SavedRegionCRCs::OBJ_SLOT_SIZE;
                        fprintf(f, "  obj[%4zu] base=0x%08X  forward_crc=0x%08X  replay_crc=0x%08X\n",
                                i, (unsigned)obj_base, fwd_c, rep_c);
                        if (++printed >= 32) {
                            fprintf(f, "  ... (truncated)\n");
                            break;
                        }
                    }
                }
                if (printed == 0) {
                    fprintf(f, "  (aggregate ObjectPool CRC differed but no slot matched on-demand --\n"
                               "   either the live-memory state shifted between save and dump, or\n"
                               "   the divergence is in inter-slot padding)\n");
                }
            }
        }
        fprintf(f, "\n");
    }

    fprintf(f, "=== Per-Region CRC (live state at detection time) ===\n");
    for (int i = 0; i < NUM_DIAG_REGIONS; i++) {
        const auto& r = g_diag_regions[i];
        uint32_t crc = Fletcher32((uint8_t*)r.addr, r.size);
        fprintf(f, "  %-28s (0x%06X +%4zu): crc=0x%08X %s\n",
                r.name, (unsigned)r.addr, r.size, crc,
                r.exclude_from_crc ? "[EXCLUDED]" : "");

        // Hex dump regions up to 256 bytes
        if (r.size <= 256) {
            const uint8_t* p = (const uint8_t*)r.addr;
            for (size_t row = 0; row < r.size; row += 16) {
                fprintf(f, "    +%03X:", (unsigned)row);
                for (size_t col = 0; col < 16 && row + col < r.size; col++)
                    fprintf(f, " %02X", p[row + col]);
                fprintf(f, "\n");
            }
        }
    }

    // Input history per-frame table. The histories are 1024-frame rings
    // indexed by g_input_buffer_index at 0x447EE0. We print `window` slots
    // ending at the current buf_idx so both peers dump the same logical
    // range -- diff the two files side-by-side and the first differing
    // frame line points directly at the asymmetric input write.
    {
        const uint32_t buf_idx = *(uint32_t*)0x447EE0;
        const uint16_t* p1_hist = (const uint16_t*)0x4280E0;
        const uint16_t* p2_hist = (const uint16_t*)0x4290E0;
        const int window = 256;  // last ~2.5s at 100fps
        fprintf(f, "\n=== Input History Tail (last %d frames up to buf_idx=%u) ===\n",
                window, buf_idx);
        fprintf(f, "  rel  slot  p1_input  p2_input\n");
        for (int i = window - 1; i >= 0; i--) {
            uint32_t slot = (buf_idx - (uint32_t)i) & 0x3FF;  // 1024-slot ring
            int rel = -i;
            fprintf(f, "  %+04d  %4u     0x%04X    0x%04X\n",
                    rel, slot, p1_hist[slot], p2_hist[slot]);
        }
    }

    // Character slots: dump BOTH static and dynamic CRCs.
    //
    // Static region (bytes 0..CHAR_SLOT_DYNAMIC_OFFSET) is the character
    // template loaded from .kgt during CSS. If this region's CRC differs
    // peer-to-peer for the same slot index, the two peers put DIFFERENT
    // characters in the SAME logical slot -- CSS character assignment is
    // perspective-based (local vs remote) rather than left vs right.
    // That's the root cause of post-residue-wipe desyncs, because the
    // game's first advance tick reads the template and writes dynamic
    // state derived from it.
    //
    // Dynamic region (bytes CHAR_SLOT_DYNAMIC_OFFSET..end) is runtime
    // state: position, health, move counters. This is what's in the CRC.
    fprintf(f, "\n=== Character Slots (static template + dynamic runtime) ===\n");
    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t slot_base = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE);
        uintptr_t dynamic_addr = slot_base + CHAR_SLOT_DYNAMIC_OFFSET;
        uint32_t static_crc  = Fletcher32((uint8_t*)slot_base, CHAR_SLOT_DYNAMIC_OFFSET);
        uint32_t dynamic_crc = Fletcher32((uint8_t*)dynamic_addr, CHAR_SLOT_DYNAMIC_SIZE);
        fprintf(f, "  Slot[%zu] base=0x%08X  static_crc=0x%08X  dynamic_crc=0x%08X\n",
                i, (unsigned)slot_base, static_crc, dynamic_crc);

        // First 32 bytes of the static region -- character ID / name lives at
        // the start of the template for most FM2K games, so a visual diff
        // here immediately says "same char in this slot?".
        const uint8_t* p = (const uint8_t*)slot_base;
        fprintf(f, "    static+000:");
        for (int j = 0; j < 32; j++) fprintf(f, " %02X", p[j]);
        fprintf(f, "\n");
    }

    // Object pool summary (first 16 objects, OBJECT_POOL_STRIDE bytes each)
    fprintf(f, "\n=== Object Pool Summary (first 16 objects) ===\n");
    constexpr size_t OBJ_SIZE = OBJECT_POOL_STRIDE;
    for (int i = 0; i < 16; i++) {
        uintptr_t obj_addr = ADDR_OBJECT_POOL + (i * OBJ_SIZE);
        uint8_t first_byte = *(uint8_t*)obj_addr;
        uint32_t crc = Fletcher32((uint8_t*)obj_addr, OBJ_SIZE);
        fprintf(f, "  Obj[%2d] (0x%08X): active=%d crc=0x%08X\n",
                i, (unsigned)obj_addr, first_byte, crc);
    }

    fprintf(f, "\n=== How to Compare ===\n");
    fprintf(f, "Both players produce this file. Diff the per-region CRCs to find\n");
    fprintf(f, "the FIRST region with different CRC -- that's the desync source.\n");
    fprintf(f, "Small regions include hex dumps for direct byte comparison.\n");

    fclose(f);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Desync diagnostic written to %s", filename);
}

#endif  // engine guard

