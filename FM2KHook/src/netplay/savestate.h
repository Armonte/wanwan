#pragma once
#include <cstdint>
#include <cstddef>
#include "../audio/sound_rollback.h"  // DesiredState, MAX_CHANNELS
#include "../core/globals.h"          // FM2K::kIsFM95, ADDR_*, sizes

// ============================================================================
// CHARACTER SLOT CONSTANTS
// Wave C state_manager audit (docs/game/state_manager_audit.md) corrected two
// P0 bugs here on FM2K:
//   - base was 0x4D1D80 → correct is 0x4D1D90 (off-by-16, we were reading the
//     tail of an adjacent slot and missing the last 16 B of every slot).
//   - dynamic offset was 55000 → correct is 0 (we only saved the last 2407 B
//     of a 57407 B slot; 95% of each slot was unsaved frame-mutable interpreter
//     state: task vars, the 20-slot box pointer array at +0x125, playerAliveFlag,
//     hit-junction tables, opponent ptr, throw state, etc.).
// FM95's char data is ~all static (per FM95_Integration.h: palette + image
// descriptors + sounds + script payload all loaded once from .player file).
// FM95 char_dynamic is empty in the FM95 SaveStateData -- the per-instance
// state lives in the object pool, which we DO capture.
// ============================================================================
#if defined(ENGINE_FM95)
constexpr size_t CHAR_SLOT_SIZE         = 229844;
constexpr size_t CHAR_SLOT_DYNAMIC_OFFSET = 0;
constexpr size_t CHAR_SLOT_DYNAMIC_SIZE = 0;     // FM95: nothing dynamic to save here
constexpr size_t NUM_CHAR_SLOTS         = 5;
constexpr uintptr_t CHAR_SLOT_BASE      = 0x509100;
#else
constexpr size_t CHAR_SLOT_SIZE = 57407;           // Full slot size from IDA
constexpr size_t CHAR_SLOT_DYNAMIC_OFFSET = 0;     // Save full slot (was 55000)
constexpr size_t CHAR_SLOT_DYNAMIC_SIZE = CHAR_SLOT_SIZE - CHAR_SLOT_DYNAMIC_OFFSET; // 57407 bytes
constexpr size_t NUM_CHAR_SLOTS = 8;
constexpr uintptr_t CHAR_SLOT_BASE = 0x4D1D90;    // g_character_data_base (corrected)
#endif

// ============================================================================
// SAVE STATE DATA
//   FM2K: sizeof(SaveStateData) ~1MB RESERVED (all 8 char slots + 1024 object
//         slots + afterimage), but the per-frame SAVE only memcpys ACTIVE slots
//         (~2 char + ~10 obj in 1v1) => ~105us/save measured (see the profiler
//         in savestate_fm2k_save.cpp). The ~1MB is the reserved struct = the
//         one-time spectator-join/replay TRANSMISSION blob, NOT per-frame cost.
//   FM95: ~50KB per slot (object pool + input rings + minimal scalars).
//         FM95 doesn't have FM2K's afterimage_pool / input_tracking_state /
//         palette flash arithmetic regions -- fewer subsystems to mirror.
// ============================================================================
struct SaveStateData {
    uint32_t frame_number;
    uint32_t checksum;
    uint32_t rng_seed;
    uint32_t input_buffer_index;
    uint32_t render_frame_counter;                         // FM2K: 0x4456FC ; FM95: g_game_tick_counter @ 0x4DD7A8

#if defined(ENGINE_FM95)
    // FM95 lean save layout -- fields ordered per docs/FM95_Savestate_Inventory.md.
    // ~45 KB per slot. char_dynamic is zero-sized (FM95 char data is static
    // after .player load; per-instance state lives in object_pool).
    //
    // Block A: Object pool (256 × 164 = 0xA400).
    uint8_t  object_pool[FM2K::SIZE_OBJECT_POOL];          // src: 0x426A40
    // Block B: game-state scalars (frame counter is in render_frame_counter
    // field above; rng_seed too). Just need game_mode.
    uint32_t game_mode;                                    // src: 0x425558
    // Block C: timer subsystems -- three contiguous blocks at 0x509080..0x5090B0.
    uint8_t  timer_blocks[0x30];                           // src: 0x509080
    // Block D: input ring + edge-detection state.
    uint8_t  p1_input_history[0x400];                      // src: 0x431720
    uint8_t  p2_input_history[0x400];                      // src: 0x431B20
    uint8_t  input_history_extra[0x400];                   // src: 0x431320 -- combo replay ring
    uint32_t p1_input_current;                             // src: 0x437750
    uint32_t p2_input_current;                             // src: 0x437754
    uint32_t p1_input_persistent;                          // src: 0x425500
    uint8_t  input_edge_state[0x10];                       // src: 0x4255A8..B7 -- current/pressed for both players
    // Block E + F merged: per-player arrays + round state form one
    // contiguous range at 0x5E98A0..0x5E9A40 (416 B). Saving as one
    // memcpy is simpler than chasing each 25-stride field individually.
    uint8_t  player_round_state[0x1A0];                    // src: 0x5E98A0
    // Block G: round/match TRANSITION scalars that vs_round_function mutates
    // but that live OUTSIDE every block above -- added 2026-07-11 after the
    // stress soak desynced at a round transition (mode 3, ~frame 1297). A
    // rollback crossing a round boundary must restore these or the re-sim
    // consumes RNG differently (round-count-driven) and diverges.
    uint32_t score_round_count;                            // src: 0x4DD268 (current round #)
    uint8_t  round_index_cluster[0x10];                    // src: 0x426934 (round_starting_player, choices[0/1], round_active_flag)
    uint32_t practice_mode_flag;                           // src: 0x426704
    uint32_t active_demo_id;                               // src: 0x471188
    uint32_t frame_skip_speedup;                           // src: 0x509060
#else
    uint8_t input_tracking_state[0xA0];                    // 160 bytes
    uint8_t char_dynamic[NUM_CHAR_SLOTS][CHAR_SLOT_DYNAMIC_SIZE]; // ~460 KB (full 57 KB slot × 8)
    uint8_t object_pool[0x5F800];                          // ~391KB
    uint8_t input_history[0x2008];                         // ~8KB
    uint8_t game_state[0x220];                             // 544 bytes
    uint8_t effect_sys1[42];                               // Effect system P1
    uint8_t effect_sys2[88];                               // 0x4456B0..0x445708: sysvars + effect-sys2 + timer_countdown1
    uint8_t shake_effects[40];                             // Shake structures
#endif

#if !defined(ENGINE_FM95)
    // g_round_end_flag -- written/read by vs_round_function, drives round
    // transitions. Not previously saved; IDA audit flagged it as unsaved
    // state that rollback must cover for clean round-boundary replay.
    uint32_t round_end_flag;                               // 0x424718

    // Mike Z rollback-safe sound layer: per-channel "desired" state. Not
    // DSound hardware state -- only the sim's authoritative record of what
    // should be playing on each channel. Sound-layer sync at end of each
    // displayed frame reconciles this to the real DSound buffers with the
    // rollback-window filter (see sound_rollback.h).
    SoundRollback::DesiredState sound_desired[SoundRollback::MAX_CHANNELS];

    // Wave C audit additions: these regions were unsaved and are the prime
    // suspects for post-rollback determinism drift.
    // Afterimage pool: dash trails / motion blur objects. ~163 KB.
    uint8_t afterimage_pool[0x46F6C0 - 0x447930];
    // Object linked-list topology. Payloads live in object_pool above, but the
    // list heads/tails/next-ptrs that define iteration order are elsewhere.
    uint32_t current_object_ptr;                           // 0x4259A8
    uint8_t  object_list_heads_tails[0x400];               // 0x430240..0x430640 (1024 B)
    uint8_t  object_node_pool[0x2000];                     // 0x4CFA20 (8192 B)
#else
    // FM95: Mike Z sound layer (engine-agnostic -- applies to any rollback
    // target with a per-channel sound dispatch). Other Wave C subsystems
    // don't exist on FM95.
    SoundRollback::DesiredState sound_desired[SoundRollback::MAX_CHANNELS];
#endif
    // BGM (MIDI/CD/stop) rollback state -- single global stream, both engines.
    // Deterministic fields only (see sound_rollback.h); the real MCI stream is
    // reconstructed by SyncAfterAdvance, not saved.
    SoundRollback::DesiredBgm bgm_desired;

    // Per-region CRCs captured AT SAVE TIME (the forward-sim state at this
    // frame). On desync, we dump these alongside the CURRENT (post-replay)
    // per-region CRCs -- a single log file shows which region diverged between
    // the two without having to diff two dump files across peers.
    struct SavedRegionCRCs {
        uint32_t rng;
        uint32_t game_state;
        uint32_t object_pool;
        uint32_t char_dynamic;
        uint32_t input_tracking;
        uint32_t effect_sys1;
        uint32_t effect_sys2;
        uint32_t shake_effects;
        uint32_t afterimage_pool;
        uint32_t list_heads_tails;
        uint32_t node_pool;
        uint32_t current_object_ptr_val;  // value, not CRC -- 4 bytes only
        uint32_t gameplay_fingerprint;
        uint32_t combined;

        // Raw scalar inputs to the fingerprint hash. Always captured (not gated
        // by the 1/sec full-CRC throttle) so when a user desync shows
        // "GameplayFingerprint DIFF" we can pinpoint WHICH scalar diverged
        // forward-vs-replay without needing FM2K_FULL_CRCS enabled.
        struct FingerprintInputs {
            uint32_t rng;
            uint32_t p1_hp;
            uint32_t p2_hp;
            uint32_t round_timer;
            uint32_t game_timer;
            uint32_t buf_idx;
            uint16_t p1_input;
            uint16_t p2_input;
        } fp_in;

        // Per-object-slot CRCs. Engine-aware sizing: FM2K has 1024 × 382
        // (~391KB pool) and FM95 has 256 × 164 (~40KB pool). Diffing
        // forward-vs-replay localizes a desync to the exact slot index.
#if defined(ENGINE_FM95)
        static constexpr size_t OBJ_SLOT_SIZE  = FM2K::OBJECT_POOL_STRIDE;  // 164
        static constexpr size_t OBJ_SLOT_COUNT = FM2K::OBJECT_POOL_COUNT;   // 256
#else
        static constexpr size_t OBJ_SLOT_SIZE  = 382;
        // FIX (frame-764 rollback gap): pool is 1024 slots (OBJECT_POOL_COUNT),
        // but this was 1023 -> the active-slot save/load loops
        // (savestate_fm2k_save.cpp:374 / savestate_fm2k_load.cpp:187) never
        // touched slot index 1023, so any object landing there kept its
        // pre-rollback state on Load -> divergence only under forced rollback.
        static constexpr size_t OBJ_SLOT_COUNT = 1024;
#endif
        uint32_t object_slot_crcs[OBJ_SLOT_COUNT];

        bool     valid;
    } saved_region_crcs;
};

// Addresses for Wave C audit regions -- FM2K only. FM95 has no equivalent
// afterimage/object-list/node-pool subsystems (different state-machine
// architecture per the IDA decomp). Stubbed to zero so any consumer that
// reads the symbols still compiles on FM95.
namespace WaveCAddrs {
#if defined(ENGINE_FM95)
    constexpr uintptr_t AFTERIMAGE_POOL        = 0;
    constexpr size_t    AFTERIMAGE_POOL_SZ     = 0;
    constexpr uintptr_t CURRENT_OBJECT_PTR     = 0;
    constexpr uintptr_t OBJECT_LIST_HEADS      = 0;
    constexpr size_t    OBJECT_LIST_HEADS_SZ   = 0;
    constexpr uintptr_t OBJECT_NODE_POOL       = 0;
    constexpr size_t    OBJECT_NODE_POOL_SZ    = 0;
#else
    constexpr uintptr_t AFTERIMAGE_POOL        = 0x447930;
    constexpr size_t    AFTERIMAGE_POOL_SZ     = 0x46F6C0 - 0x447930;
    constexpr uintptr_t CURRENT_OBJECT_PTR     = 0x4259A8;
    constexpr uintptr_t OBJECT_LIST_HEADS      = 0x430240;
    constexpr size_t    OBJECT_LIST_HEADS_SZ   = 0x400;
    constexpr uintptr_t OBJECT_NODE_POOL       = 0x4CFA20;
    constexpr size_t    OBJECT_NODE_POOL_SZ    = 0x2000;
#endif
}

// ============================================================================
// SAVESTATE API
// ============================================================================
void SaveState_Init();
// Reset buf_idx / render_frame_counter / input edge state to deterministic
// values BEFORE the first AdvEvent. Pairs with the post-Init() teardown of
// g_initial_sync_done by SaveState_Init(). Idempotent within a battle.
// Must be called from Netplay_Start*Battle paths AFTER SaveState_Init().
void SaveState_DoInitialSync();
// FM95 half of DoInitialSync: reset the FM95 input rings / buf_idx / edge
// state to zero. Defined in savestate_fm95.cpp (the TU that owns the FM95
// addresses). Only ODR-used from DoInitialSync's `else` branch, which is
// discarded by `if constexpr (kIsFM2K)` in the FM2K build, so it needs no
// FM2K definition. The FM2K DoInitialSync body zeroes the FM2K-address
// equivalents (0x447EE0 / 0x447F00.. / 0x4280D8) inline.
void SaveState_Fm95ResetInputSync();
bool SaveState_Save(int frame);
bool SaveState_Load(int frame);
uint32_t SaveState_GetLastChecksum(int frame);

// Snapshot serialization (task #18 phase 2). After SaveState_Save populates
// a slot, copy its raw bytes to a caller-supplied buffer for transmission
// to a spectator. Returns bytes written, or 0 if no slot has been saved yet.
//
// SaveState_GetSlotByteSize() reports the per-slot byte count up-front
// (= sizeof(SaveStateData)) so callers can size their buffer correctly.
// On FM2K with the full save layout that's around 1 MB; FM95's lean layout
// is ~45 KB. The actual bytes are an opaque blob -- they round-trip through
// SaveState_LoadFromBytes (added in phase 4) without external interpretation.
//
// The pointer returned by SaveState_PeekLastSavedSlotBytes is valid only
// until the next SaveState_Save call -- callers that need to keep the bytes
// around must memcpy. Returns nullptr if no Save has run yet.
size_t         SaveState_GetSlotByteSize();
const uint8_t* SaveState_PeekLastSavedSlotBytes();

// Round-trip counterpart to PeekLastSavedSlotBytes: copy `bytes` into the
// rollback-buffer slot indicated by the embedded frame_number, register
// it as the "last saved slot," and apply it to live memory by invoking
// SaveState_Load on the same frame. The blob is opaque from the caller's
// perspective -- it must have come from PeekLastSavedSlotBytes on the same
// engine variant (FM2K vs FM95) and the same build (slot layout is not
// version-stable).
//
// Returns true on success. Failure modes:
//   - bytes == nullptr or n != SaveState_GetSlotByteSize()
//   - SaveState_Load's internal frame_number sanity check fails (would
//     mean the blob's frame_number was somehow corrupted between Peek
//     and Load, which shouldn't happen on a single sender → single
//     receiver path)
//
// Callers (spectator-join SNAPSHOT_END handler) should fall back to
// FULL_SESSION replay if this returns false.
bool           SaveState_LoadFromBytes(const uint8_t* bytes, size_t n);

// Back-patch the most-recently-saved slot's rng_seed with the post-render
// value of g_random_seed. SaveEvent fires BEFORE render in GekkoNet's per-
// frame event order, so SaveState_Save captures the pre-render rng. Render
// then advances rng (ProcessShakeEffect mode 4 + ProcessColorInterpolation
// mode 3 each call game_rand). For rollback determinism the saved slot must
// reflect post-render rng so that:
//   (a) rollback replay's Load gives back the same starting rng forward had
//       at the next frame (forward's pre-sim rng = post-render-of-prev-frame
//       rng; replay must match), and
//   (b) the runahead-rewind Load every wall-clock frame does the same thing
//       as a real rollback would -- restoring the post-render rng so live rng
//       matches across forward and replay.
// Without this back-patch, render-side rand calls accumulate forever on
// forward but never on replay, and the two diverge after any rollback --
// which is what produced the rng-region forward/replay DIFFs in
// FM2K_P*_desync_f*.log.
//
// Call from RenderFrameWithSnapshot AFTER original_render_game returns,
// passing the current live rng. Cheap (single uint32 write into the slot).
// No-op if no Save has run yet (e.g. CSS pre-battle).
void SaveState_PatchPostRenderRng(uint32_t rng);

// Read the POST-render RNG that the forward pass captured for a given
// frame, if any. Returns true and fills *out_rng iff a forward render
// has run for that exact frame number (the parallel buffer carries the
// frame number alongside the rng so a wrap-around or a load to a frame
// that was never rendered doesn't replay a stale value).
//
// Used by Netplay_ProcessBattleInputPhase to re-establish the right
// starting RNG at the head of each replay AdvanceEvent -- without this,
// intermediate frames in a multi-frame rollback batch run their sim
// from POST-sim-prev (replay) instead of POST-render-prev (forward),
// and the render-side game_rand delta accumulates as divergence.
bool SaveState_GetPostRenderRng(int frame, uint32_t* out_rng);

// Per-region diagnostic checksums for desync investigation
struct RegionChecksums {
    uint32_t rng;
    uint32_t game_state;
    uint32_t object_pool;
    uint32_t char_dynamic;
    uint32_t input_tracking;
    uint32_t effect_sys1;
    uint32_t effect_sys2;
    uint32_t shake_effects;
    uint32_t combined;

    // Gameplay fingerprint: hash over just the game-visible fields (HP, positions,
    // RNG, round timer, current inputs). Process-independent by design -- no
    // pointers, no heap addresses, no interpreter-internal state. If the full
    // combined hash differs but this matches, we have a false-positive desync:
    // players would see identical gameplay, only the internal memory layout
    // diverges due to per-process residue.
    uint32_t gameplay_fingerprint;

    // Raw scalar inputs going into the fingerprint hash. Captured every save
    // (not gated by the full-CRC throttle) so a desync dump can show exactly
    // which scalar diverged forward-vs-replay.
    SaveStateData::SavedRegionCRCs::FingerprintInputs fp_inputs;
};

const RegionChecksums& SaveState_GetRegionChecksums();
uint32_t SaveState_CalculateFullChecksum();

// ============================================================================
// TESTING / VERIFICATION
// ============================================================================
struct StateSnapshot {
    uint32_t rng_seed;
    uint32_t input_buffer_index;
    uint32_t p1_x, p1_y;
    uint32_t p2_x, p2_y;
    uint32_t p1_hp, p2_hp;
    uint32_t frame_counter;
    uint32_t checksum;
};

StateSnapshot SaveState_CaptureSnapshot();
bool SaveState_CompareSnapshots(const StateSnapshot& a, const StateSnapshot& b, char* diff_buf, size_t buf_size);
bool SaveState_TestRoundtrip();

// BBBR-style desync diagnostic - dumps per-region CRC + hex to log file
void SaveState_DumpDesyncDiagnostic(int frame, uint32_t local_crc, uint32_t remote_crc, int player_index);

// Per-save rngtrace. Fixed-size in-memory ring, written to CSV on demand.
// Per-frame console logging was too noisy and tanked framerate at 100 fps.
void SaveState_PushRngTrace(int frame, uint32_t rng, uint32_t fp);
void SaveState_FlushRngTrace(int player_index, const char* reason);
