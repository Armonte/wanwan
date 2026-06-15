// Optimized savestate - only save dynamic portions of character slots
// Static character data (sprites, animations, hitboxes) loaded from .player files doesn't change
#include "savestate.h"
#include "globals.h"
#include <SDL3/SDL_log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>  // getenv for FM2K_FULL_CRCS diagnostic env-gate

// Memory region addresses. RNG / object pool / input ring use the engine-
// aware FM2K::ADDR_* constants from globals.h so the FM95 build picks up
// FM95 addresses automatically. The remaining constants here are FM2K-
// specific (input tracking @ 0x447EE0, render frame counter, input history
// extension, game state block) — they have no FM95 equivalent yet, and
// savestate is a no-op on FM95 (rollback isn't activated; trampoline
// refactor in Task 7 will design FM95's save layout).
namespace {
    // RNG seed + object pool come from globals.h so the FM95 build picks up
    // FM95 addresses. Other constants here are FM2K-specific palette/effect/
    // input-tracking regions with no FM95 equivalent. The savestate body is
    // gated to FM2K only — Task 7 (trampoline refactor) will design FM95's
    // save layout from scratch.
    constexpr uintptr_t ADDR_RNG_SEED            = FM2K::ADDR_RANDOM_SEED;
    constexpr uintptr_t ADDR_OBJECT_POOL         = FM2K::ADDR_OBJECT_POOL;
    constexpr size_t    SIZE_OBJECT_POOL         = FM2K::SIZE_OBJECT_POOL;
    constexpr size_t    OBJECT_POOL_STRIDE       = FM2K::OBJECT_POOL_STRIDE;

    // FM2K-only addresses (palette flash arithmetic depends on the exact
    // 0x4456FC value; FM95 uses a different counter at 0x4DD7A8).
    constexpr uintptr_t ADDR_RENDER_FRAME_COUNTER = 0x4456FC;
    constexpr uintptr_t ADDR_INPUT_BUFFER_INDEX  = 0x447EE0;
    constexpr uintptr_t ADDR_INPUT_TRACKING      = 0x447EE0;
    constexpr size_t    SIZE_INPUT_TRACKING      = 0xA0;
    constexpr uintptr_t ADDR_INPUT_HISTORY       = 0x4280D8;
    constexpr size_t    SIZE_INPUT_HISTORY       = 0x2008;
    constexpr uintptr_t ADDR_GAME_STATE          = 0x470020;
    constexpr size_t    SIZE_GAME_STATE          = 0x220;
}

// Effect/shake region addresses - saved for rollback determinism
namespace EffectAddrs {
    constexpr uintptr_t EFFECT_SYS1     = 0x447D7D;
    constexpr size_t    EFFECT_SYS1_SZ  = 42;
    // Script-sysvar + effect-param block. IDA audit (task
    // accfbf054043e03dd) traced the persistent obj[1]+0x2C rollback
    // divergence to a 32-byte script-sysvar array immediately preceding
    // the old 44-byte effect_sys2 window. animation-script opcodes
    // read/write g_global_variable_array_FM2K_SYSTEM_VARS[16] @
    // 0x4456B0..0x4456D0, and g_timer_countdown1 at 0x4456D8 auto-
    // decrements every frame — replay inherited stale live-memory
    // sysvars, script arithmetic produced off-by-2 result (0x5D8 vs
    // 0x5DA), and that cascaded into every subsequent object update.
    //
    //   OLD: 0x4456D0..0x4456FC (44 B) — only effect-sys2 tail
    //   NEW: 0x4456B0..0x445708 (88 B) — sysvars + effect-sys2 + timer
    constexpr uintptr_t EFFECT_SYS2     = 0x4456B0;
    constexpr size_t    EFFECT_SYS2_SZ  = 88;
    constexpr uintptr_t SHAKE_EFFECTS   = 0x447DA9;
    constexpr size_t    SHAKE_EFFECTS_SZ = 40;
}

// --- TEMP per-region save sub-profiler (FM2K_PERF_PROFILE=1) -------------
// Attributes the 1.58ms/save cost to specific memcpy blocks so #62 can
// target the heaviest one. Same env gate as the netplay-side [PERF] line.
// Reports avg microsec/region every 500 saves. Remove once #62 lands.
namespace {
struct SBucket { uint64_t ns = 0; uint32_t n = 0; };
static const bool g_ss_perf_on = [] {
    const char* v = std::getenv("FM2K_PERF_PROFILE");
    return v && v[0] && v[0] != '0';
}();
static uint64_t SsQpcFreq() {
    static uint64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (uint64_t)q.QuadPart; }();
    return f;
}
static inline uint64_t SsNowNs() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((long double)c.QuadPart * 1e9L / (long double)SsQpcFreq());
}
static SBucket g_ss_char, g_ss_obj, g_ss_after, g_ss_misc, g_ss_fp, g_ss_sound;
static SBucket g_ss_head, g_ss_tail, g_ss_full;
struct SScope {
    SBucket* b; uint64_t t0;
    explicit SScope(SBucket* x) : b(g_ss_perf_on ? x : nullptr), t0(b ? SsNowNs() : 0) {}
    ~SScope() { if (b) { b->ns += SsNowNs() - t0; b->n++; } }
};
static void SsMaybeReport() {
    if (!g_ss_perf_on) return;
    static uint32_t t = 0;
    if (++t % 500 != 0) return;
    auto us = [](const SBucket& b) { return b.n ? (double)b.ns / b.n / 1000.0 : 0.0; };
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[PERF-SAVE] head=%.1fus char=%.1fus obj=%.1fus afterimage=%.1fus misc=%.1fus "
        "fingerprint=%.1fus sound=%.1fus tail=%.1fus fullcrc=%.1fus(x%u) (n=%u)",
        us(g_ss_head), us(g_ss_char), us(g_ss_obj), us(g_ss_after), us(g_ss_misc),
        us(g_ss_fp), us(g_ss_sound), us(g_ss_tail), us(g_ss_full), g_ss_full.n,
        g_ss_char.n);
}
}  // namespace

// Rollback buffer
static constexpr int MAX_ROLLBACK_FRAMES = 64;
static SaveStateData g_state_buffer[MAX_ROLLBACK_FRAMES];

// Tracks the slot most recently filled by SaveState_Save. Used by
// SaveState_PatchPostRenderRng to back-patch the rng_seed field after the
// trampoline's render call returns, so the saved slot reflects post-render
// rng (matching what forward sim sees as the starting rng for the next
// frame). -1 = no save has happened yet (e.g. CSS).
static int g_last_saved_slot  = -1;
static int g_last_saved_frame = -1;

// Parallel "post-render RNG" buffer — separate from g_state_buffer because
// SaveState_Save overwrites rng_seed on REPLAY saves, wiping the value
// PatchPostRenderRng wrote during the original forward pass. During a
// rollback batch, render fires only ONCE (after the last AdvanceEvent),
// so intermediate frames in the batch never get their POST-render RNG
// applied to the engine — replay sim_K+2 starts from POST-sim-K+1 instead
// of POST-render-K+1, accumulating render's RNG delta as divergence.
//
// Phase F (#23): the v0.2.43 user-reported desyncs all show "RNG_Seed
// forward != replay" or "GameplayFingerprint DIFF" caused exactly by
// this. Fix is to keep a per-frame snapshot here that replay does NOT
// touch, then apply it at the start of each replay AdvanceEvent so sim
// starts from the same RNG the forward pass's sim started from.
//
// Frame number stored alongside so a wrap-around (rollback >MAX_ROLLBACK_
// FRAMES, which we don't expect to actually happen — typical rewinds are
// <10 — but be defensive) doesn't restore a stale value from a previous
// session.
static uint32_t g_post_render_rng[MAX_ROLLBACK_FRAMES] = {};
static int32_t  g_post_render_rng_frame[MAX_ROLLBACK_FRAMES] = {};

// Initial sync flag - forces deterministic state on first save (like BBBR's m_initialSyncDone)
static bool g_initial_sync_done = false;

// Per-region checksums for desync investigation (forward-declared here so
// SaveState_Save can snapshot it into per-frame saved_region_crcs; full
// definition + accessors live further down next to the calculator).
static RegionChecksums g_region_checksums = {};

// Replay-save shadow buffer (stress-mode forward-vs-replay diff).
// SaveState_Save runs once per save-event. In stress mode (and during real
// online rollback), GekkoNet may issue a SaveEvent for the SAME frame twice:
// first when the forward sim reaches it, second after a Load+Advance replays
// it. We use the FIRST save's per-region CRCs as "forward" (kept in
// g_state_buffer[slot].saved_region_crcs), and the SECOND save's as
// "replay" (stored here). Diffing these in the desync dump pinpoints which
// region replayed nondeterministically.
struct ReplaySaveSnapshot {
    int32_t  frame_number;        // -1 if no replay save for this slot
    bool     valid;
    SaveStateData::SavedRegionCRCs crcs;
};
static ReplaySaveSnapshot g_replay_saves[64 /* MAX_ROLLBACK_FRAMES */] = {};

// ============================================================================
// RNGTRACE RING BUFFER
// Records {frame, rng, fingerprint} per authoritative save into a fixed-size
// ring in memory. Console logging every save tanks framerate (~100 Hz logs
// through quill/SDL), so we write bytes into memory at ~100 B/frame and only
// flush to a file on demand (desync event, session end). Stores the last
// ~10 minutes of gameplay at 100 fps = 60000 entries × 12 B = 720 KB.
// ============================================================================
struct RngTraceEntry {
    int32_t  frame;
    uint32_t rng;
    uint32_t fingerprint;
};
static constexpr size_t RNGTRACE_CAPACITY = 60000;  // ~10 min at 100 fps
static RngTraceEntry g_rngtrace_ring[RNGTRACE_CAPACITY];
static size_t g_rngtrace_head   = 0;   // next write slot (mod capacity)
static size_t g_rngtrace_count  = 0;   // total pushed (clamped at capacity for wrap)

void SaveState_PushRngTrace(int frame, uint32_t rng, uint32_t fp) {
    g_rngtrace_ring[g_rngtrace_head] = { frame, rng, fp };
    g_rngtrace_head = (g_rngtrace_head + 1) % RNGTRACE_CAPACITY;
    if (g_rngtrace_count < RNGTRACE_CAPACITY) g_rngtrace_count++;
}

void SaveState_FlushRngTrace(int player_index, const char* reason) {
    char filename[256];
    snprintf(filename, sizeof(filename), "FM2K_P%d_rngtrace.csv", player_index + 1);
    FILE* f = fopen(filename, "w");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "rngtrace: failed to open %s", filename);
        return;
    }
    fprintf(f, "# rngtrace flush reason=%s count=%zu\n", reason ? reason : "",
            g_rngtrace_count);
    fprintf(f, "frame,rng,fingerprint\n");
    // Walk from oldest to newest
    size_t start = (g_rngtrace_count < RNGTRACE_CAPACITY)
                   ? 0
                   : g_rngtrace_head;  // head points at the oldest when full
    for (size_t i = 0; i < g_rngtrace_count; i++) {
        const auto& e = g_rngtrace_ring[(start + i) % RNGTRACE_CAPACITY];
        fprintf(f, "%d,0x%08X,0x%08X\n", e.frame, e.rng, e.fingerprint);
    }
    fclose(f);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "rngtrace: wrote %zu entries to %s (%s)",
                g_rngtrace_count, filename, reason ? reason : "");
}

// Hashing: use xxHash3-64 (truncated to u32) instead of pure-C Fletcher32.
// Fletcher32 topped out ~500 MB/s on a 32-bit build; in stress mode at
// ~1 MB hashed per save × 10× rollback amplification × 100 Hz we were
// hitting 1 GB/s and saturating one core. XXH3 runs 15-30 GB/s on the
// same hardware and has strictly better collision resistance. Kept the
// Fletcher32 symbol as a compatibility wrapper so call sites don't need
// to change — they still see `uint32_t Fletcher32(ptr, len)`.
#define XXH_INLINE_ALL
#define XXH_NO_STREAM
#include "../../../vendored/xxhash/xxhash.h"

static uint32_t Fletcher32(const uint8_t* data, size_t len) {
    // Truncate the 64-bit hash to 32 bits (xor fold for better avalanche).
    XXH64_hash_t h = XXH3_64bits(data, len);
    return (uint32_t)(h ^ (h >> 32));
}

// Kept the original Fletcher32 body below (unused) for reference; the
// wrapper above takes precedence at the linker level.
[[maybe_unused]] static uint32_t Fletcher32_orig(const uint8_t* data, size_t len) {
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    while (len) {
        size_t tlen = (len > 360) ? 360 : len;
        len -= tlen;
        do {
            sum1 += *data++;
            sum2 += sum1;
        } while (--tlen);
        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }
    sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
    sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    return (sum2 << 16) | sum1;
}

// Single replay-diff log per battle session. Truncated in SaveState_Init,
// then appended every time a replay save diverges from its forward save.
// Replaces the old one-file-per-frame scheme that flooded the disk.
// Resolved through Fm2k_BuildLogPath at first use so it lands in
// `<game_dir>/logs/`.
constexpr const char* REPLAY_DIFF_LOG_NAME = "FM2K_replay_diffs.log";

static const char* ReplayDiffLogPath() {
    static char s_path[MAX_PATH] = {0};
    if (s_path[0] == 0) {
        if (!Fm2k_BuildLogPath(s_path, sizeof(s_path), REPLAY_DIFF_LOG_NAME)) {
            std::snprintf(s_path, sizeof(s_path), "%s", REPLAY_DIFF_LOG_NAME);
        }
    }
    return s_path;
}

// Forward decls — definitions appear after SaveState_Save for readability.
uint32_t SaveState_CalculateFingerprint();
uint32_t SaveState_CalculateFullChecksum();

void SaveState_Init() {
    memset(g_state_buffer, 0, sizeof(g_state_buffer));
    // Reset replay-shadow buffer so stale forward-vs-replay diffs from a
    // previous battle don't show up on the first desync of a new session.
    for (int i = 0; i < MAX_ROLLBACK_FRAMES; i++) {
        g_replay_saves[i].frame_number = -1;
        g_replay_saves[i].valid = false;
    }
    // Reset the post-render-RNG parallel buffer — stale values from a
    // previous session would cause the Phase F fix to apply the wrong
    // RNG at the start of AdvanceEvents in the new session.
    for (int i = 0; i < MAX_ROLLBACK_FRAMES; i++) {
        g_post_render_rng[i]       = 0;
        g_post_render_rng_frame[i] = -1;
    }
    // Truncate the consolidated replay-diff log so each battle session starts
    // with a fresh file.
    if (FILE* f = fopen(ReplayDiffLogPath(), "w")) {
        fprintf(f, "# replay-diff log — appends one block per replay save that\n"
                   "# diverges from its forward save. fopen+fclose per write.\n");
        fclose(f);
    }
    g_initial_sync_done = false;  // Reset so next battle re-syncs
    g_last_saved_slot = -1;
    g_last_saved_frame = -1;
}

// Reset frame-index / edge-detection state to a deterministic value so
// input-change detection produces identical local input streams on both
// peers (or host vs replay). MUST be called BEFORE the first AdvanceEvent
// — used to be lazy-fired on the first SaveEvent which is post-PGI, and
// that off-by-one timing caused cumulative drift over rollback cycles
// (replay_selftest harness's frame-91 RNG divergence). Now: eager call
// at battle init from Netplay_Start*Battle.
void SaveState_DoInitialSync() {
    if (g_initial_sync_done) return;
    *(uint32_t*)ADDR_INPUT_BUFFER_INDEX = 0;         // Reset buf_idx
    *(uint32_t*)ADDR_RENDER_FRAME_COUNTER = 0;       // Reset render frame counter
    memset((void*)0x447F00, 0, 0x20);   // g_prev_input_state
    memset((void*)0x447F40, 0, 0x20);   // g_processed_input
    memset((void*)0x447F60, 0, 0x20);   // g_input_changes
    memset((void*)ADDR_INPUT_HISTORY, 0, SIZE_INPUT_HISTORY);
    // REVERTED 2026-05-17: shake/palette zero on battle entry was
    // helpful for replay-self-test parity (host vs replay both start
    // at zero), but user reports cross-peer desync in real netplay
    // after this change. Possibly the zero clobbers state real
    // netplay needs. Reverting to bisect.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SaveState: Initial sync (eager, pre-first-AdvEvent) — "
        "reset buf_idx/render_fc/input edge state");
    g_initial_sync_done = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SaveState: Initialized %d rollback slots (~%zuKB each)",
                MAX_ROLLBACK_FRAMES, sizeof(SaveStateData) / 1024);
}

void SaveState_PatchPostRenderRng(uint32_t rng) {
    if (g_last_saved_slot < 0) return;  // no save has happened yet
    g_state_buffer[g_last_saved_slot].rng_seed = rng;
    // Mirror into the replay-safe parallel buffer (see g_post_render_rng
    // comment up top). Forward sim writes this; rollback replay reads it
    // to re-establish the right starting RNG for each intermediate
    // AdvanceEvent in a multi-frame rollback batch.
    g_post_render_rng[g_last_saved_slot]       = rng;
    g_post_render_rng_frame[g_last_saved_slot] = g_last_saved_frame;
}

bool SaveState_GetPostRenderRng(int frame, uint32_t* out_rng) {
    if (frame < 0 || !out_rng) return false;
    const int slot = frame % MAX_ROLLBACK_FRAMES;
    if (g_post_render_rng_frame[slot] != frame) return false;
    *out_rng = g_post_render_rng[slot];
    return true;
}

#if defined(ENGINE_FM95)
// ============================================================================
// FM95 save/load — addresses sourced from docs/FM95_Savestate_Inventory.md
// (IDA xrefs + decomp on the live CPW IDB).
// ============================================================================
namespace fm95save {
    // Block A
    constexpr uintptr_t OBJECT_POOL          = FM2K::ADDR_OBJECT_POOL;            // 0x426A40
    // Block B
    constexpr uintptr_t GAME_TICK_COUNTER    = FM2K::ADDR_FRAME_COUNTER;          // 0x4DD7A8
    constexpr uintptr_t RNG_SEED             = FM2K::ADDR_RANDOM_SEED;            // 0x4243FC
    constexpr uintptr_t GAME_MODE            = FM2K::ADDR_GAME_MODE;              // 0x425558
    // Block C
    constexpr uintptr_t TIMER_BLOCKS         = 0x509080;
    constexpr size_t    TIMER_BLOCKS_SZ      = 0x30;
    // Block D
    constexpr uintptr_t INPUT_BUF_IDX        = FM2K::ADDR_INPUT_BUFFER_INDEX;     // 0x437700
    constexpr uintptr_t P1_INPUT_HISTORY     = FM2K::ADDR_P1_INPUT_HISTORY;       // 0x431720
    constexpr uintptr_t P2_INPUT_HISTORY     = FM2K::ADDR_P2_INPUT_HISTORY;       // 0x431B20
    constexpr uintptr_t INPUT_HISTORY_EXTRA  = 0x431320;
    constexpr size_t    HISTORY_RING_SZ      = 0x400;       // 256 × 4
    constexpr uintptr_t P1_INPUT_CURRENT     = FM2K::ADDR_P1_INPUT;               // 0x437750
    constexpr uintptr_t P2_INPUT_CURRENT     = FM2K::ADDR_P2_INPUT;               // 0x437754
    constexpr uintptr_t P1_INPUT_PERSISTENT  = 0x425500;
    constexpr uintptr_t INPUT_EDGE_STATE     = 0x4255A8;
    constexpr size_t    INPUT_EDGE_STATE_SZ  = 0x10;        // p1/p2 current + pressed, 4 dwords
    // Block E + F merged
    constexpr uintptr_t PLAYER_ROUND_STATE   = 0x5E98A0;
    constexpr size_t    PLAYER_ROUND_STATE_SZ = 0x1A0;      // through 0x5E9A40 inclusive of round_count_max+4
}

bool SaveState_Save(int frame) {
    if (frame < 0) frame = 0;
    const int slot = frame % MAX_ROLLBACK_FRAMES;
    SaveStateData* s = &g_state_buffer[slot];

    s->frame_number         = (uint32_t)frame;
    s->rng_seed             = *(uint32_t*)fm95save::RNG_SEED;
    s->input_buffer_index   = *(uint32_t*)fm95save::INPUT_BUF_IDX;
    s->render_frame_counter = *(uint32_t*)fm95save::GAME_TICK_COUNTER;
    s->game_mode            = *(uint32_t*)fm95save::GAME_MODE;

    std::memcpy(s->object_pool,         (const void*)fm95save::OBJECT_POOL,
                sizeof(s->object_pool));
    std::memcpy(s->timer_blocks,        (const void*)fm95save::TIMER_BLOCKS,
                fm95save::TIMER_BLOCKS_SZ);
    std::memcpy(s->p1_input_history,    (const void*)fm95save::P1_INPUT_HISTORY,
                fm95save::HISTORY_RING_SZ);
    std::memcpy(s->p2_input_history,    (const void*)fm95save::P2_INPUT_HISTORY,
                fm95save::HISTORY_RING_SZ);
    std::memcpy(s->input_history_extra, (const void*)fm95save::INPUT_HISTORY_EXTRA,
                fm95save::HISTORY_RING_SZ);
    s->p1_input_current     = *(uint32_t*)fm95save::P1_INPUT_CURRENT;
    s->p2_input_current     = *(uint32_t*)fm95save::P2_INPUT_CURRENT;
    s->p1_input_persistent  = *(uint32_t*)fm95save::P1_INPUT_PERSISTENT;
    std::memcpy(s->input_edge_state,    (const void*)fm95save::INPUT_EDGE_STATE,
                fm95save::INPUT_EDGE_STATE_SZ);
    std::memcpy(s->player_round_state,  (const void*)fm95save::PLAYER_ROUND_STATE,
                fm95save::PLAYER_ROUND_STATE_SZ);

    // Mike Z sound desired — engine-agnostic snapshot.
    SoundRollback::CaptureDesired(s->sound_desired);

    // Combined Fletcher32 over all captured regions for desync detection.
    s->checksum = Fletcher32((const uint8_t*)s->object_pool,
                             sizeof(s->object_pool));
    s->checksum ^= Fletcher32(s->p1_input_history, fm95save::HISTORY_RING_SZ);
    s->checksum ^= Fletcher32(s->p2_input_history, fm95save::HISTORY_RING_SZ);
    s->checksum ^= Fletcher32(s->player_round_state,
                              fm95save::PLAYER_ROUND_STATE_SZ);
    s->checksum ^= s->rng_seed ^ s->render_frame_counter ^ s->game_mode;

    g_last_saved_slot  = slot;
    g_last_saved_frame = frame;
    return true;
}

bool SaveState_Load(int frame) {
    if (frame < 0) frame = 0;
    const int slot = frame % MAX_ROLLBACK_FRAMES;
    const SaveStateData* s = &g_state_buffer[slot];

    if ((int)s->frame_number != frame) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SaveState_Load: requested frame=%d but slot %d holds frame=%u",
            frame, slot, s->frame_number);
        return false;
    }

    *(uint32_t*)fm95save::RNG_SEED          = s->rng_seed;
    *(uint32_t*)fm95save::INPUT_BUF_IDX     = s->input_buffer_index;
    *(uint32_t*)fm95save::GAME_TICK_COUNTER = s->render_frame_counter;
    *(uint32_t*)fm95save::GAME_MODE         = s->game_mode;

    std::memcpy((void*)fm95save::OBJECT_POOL,         s->object_pool,
                sizeof(s->object_pool));
    std::memcpy((void*)fm95save::TIMER_BLOCKS,        s->timer_blocks,
                fm95save::TIMER_BLOCKS_SZ);
    std::memcpy((void*)fm95save::P1_INPUT_HISTORY,    s->p1_input_history,
                fm95save::HISTORY_RING_SZ);
    std::memcpy((void*)fm95save::P2_INPUT_HISTORY,    s->p2_input_history,
                fm95save::HISTORY_RING_SZ);
    std::memcpy((void*)fm95save::INPUT_HISTORY_EXTRA, s->input_history_extra,
                fm95save::HISTORY_RING_SZ);
    *(uint32_t*)fm95save::P1_INPUT_CURRENT    = s->p1_input_current;
    *(uint32_t*)fm95save::P2_INPUT_CURRENT    = s->p2_input_current;
    *(uint32_t*)fm95save::P1_INPUT_PERSISTENT = s->p1_input_persistent;
    std::memcpy((void*)fm95save::INPUT_EDGE_STATE,    s->input_edge_state,
                fm95save::INPUT_EDGE_STATE_SZ);
    std::memcpy((void*)fm95save::PLAYER_ROUND_STATE,  s->player_round_state,
                fm95save::PLAYER_ROUND_STATE_SZ);

    SoundRollback::RestoreDesired(s->sound_desired);
    return true;
}

uint32_t SaveState_CalculateFingerprint() {
    // Lightweight gameplay-visible hash — hits the bits that two peers
    // running the same input stream MUST match: RNG seed, frame counter,
    // game mode, and the per-player score block (positions, HP, meters).
    uint32_t h = *(uint32_t*)fm95save::RNG_SEED;
    h ^= *(uint32_t*)fm95save::GAME_TICK_COUNTER;
    h ^= *(uint32_t*)fm95save::GAME_MODE;
    h ^= Fletcher32((const uint8_t*)fm95save::PLAYER_ROUND_STATE,
                    fm95save::PLAYER_ROUND_STATE_SZ);
    return h;
}

uint32_t SaveState_CalculateFullChecksum() {
    uint32_t h = Fletcher32((const uint8_t*)fm95save::OBJECT_POOL,
                            FM2K::SIZE_OBJECT_POOL);
    h ^= Fletcher32((const uint8_t*)fm95save::P1_INPUT_HISTORY,
                    fm95save::HISTORY_RING_SZ);
    h ^= Fletcher32((const uint8_t*)fm95save::P2_INPUT_HISTORY,
                    fm95save::HISTORY_RING_SZ);
    h ^= Fletcher32((const uint8_t*)fm95save::PLAYER_ROUND_STATE,
                    fm95save::PLAYER_ROUND_STATE_SZ);
    h ^= *(uint32_t*)fm95save::RNG_SEED;
    h ^= *(uint32_t*)fm95save::GAME_TICK_COUNTER;
    h ^= *(uint32_t*)fm95save::GAME_MODE;
    return h;
}

StateSnapshot SaveState_CaptureSnapshot() {
    StateSnapshot snap{};
    snap.rng_seed           = *(uint32_t*)fm95save::RNG_SEED;
    snap.input_buffer_index = *(uint32_t*)fm95save::INPUT_BUF_IDX;
    snap.frame_counter      = *(uint32_t*)fm95save::GAME_TICK_COUNTER;
    // Player positions live in the object pool; pull from slot 0 / slot 1
    // (player main objects) at offset +36 (pos_x) / +40 (pos_y) per the
    // FM95::ObjectSlot layout.
    const uint8_t* pool = (const uint8_t*)fm95save::OBJECT_POOL;
    snap.p1_x = *(uint32_t*)(pool + 0 * FM2K::OBJECT_POOL_STRIDE + 36);
    snap.p1_y = *(uint32_t*)(pool + 0 * FM2K::OBJECT_POOL_STRIDE + 40);
    snap.p2_x = *(uint32_t*)(pool + 1 * FM2K::OBJECT_POOL_STRIDE + 36);
    snap.p2_y = *(uint32_t*)(pool + 1 * FM2K::OBJECT_POOL_STRIDE + 40);
    // HP at slot offset +72 per FM95::ObjectSlot.
    snap.p1_hp = *(uint32_t*)(pool + 0 * FM2K::OBJECT_POOL_STRIDE + 72);
    snap.p2_hp = *(uint32_t*)(pool + 1 * FM2K::OBJECT_POOL_STRIDE + 72);
    snap.checksum = SaveState_CalculateFingerprint();
    return snap;
}

bool SaveState_CompareSnapshots(const StateSnapshot& a, const StateSnapshot& b,
                                char* diff_buf, size_t buf_size) {
    bool ok = (a.rng_seed == b.rng_seed)
           && (a.input_buffer_index == b.input_buffer_index)
           && (a.frame_counter == b.frame_counter)
           && (a.p1_x == b.p1_x) && (a.p1_y == b.p1_y)
           && (a.p2_x == b.p2_x) && (a.p2_y == b.p2_y)
           && (a.p1_hp == b.p1_hp) && (a.p2_hp == b.p2_hp);
    if (diff_buf && buf_size > 0) {
        if (ok) {
            std::snprintf(diff_buf, buf_size, "ok");
        } else {
            std::snprintf(diff_buf, buf_size,
                "rng %08X/%08X buf %u/%u tick %u/%u "
                "p1 (%u,%u) hp=%u / (%u,%u) hp=%u "
                "p2 (%u,%u) hp=%u / (%u,%u) hp=%u",
                a.rng_seed, b.rng_seed,
                a.input_buffer_index, b.input_buffer_index,
                a.frame_counter, b.frame_counter,
                a.p1_x, a.p1_y, a.p1_hp,
                b.p1_x, b.p1_y, b.p1_hp,
                a.p2_x, a.p2_y, a.p2_hp,
                b.p2_x, b.p2_y, b.p2_hp);
        }
    }
    return ok;
}

bool SaveState_TestRoundtrip() {
    // Save → mutate one byte of the object pool → Load → verify byte
    // restored. Cheap correctness check we can call on demand from a
    // dev-panel button.
    if (!SaveState_Save(0)) return false;
    uint8_t* pool = (uint8_t*)fm95save::OBJECT_POOL;
    const uint8_t before = pool[0];
    pool[0] = (uint8_t)~before;
    if (!SaveState_Load(0)) return false;
    const bool ok = (pool[0] == before);
    if (!ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SaveState_TestRoundtrip: object_pool[0] = %02X expected %02X",
            pool[0], before);
    }
    return ok;
}

uint32_t SaveState_GetLastChecksum(int frame) {
    if (frame < 0) frame = 0;
    return g_state_buffer[frame % MAX_ROLLBACK_FRAMES].checksum;
}

void SaveState_DumpDesyncDiagnostic(int frame, uint32_t local_crc,
                                    uint32_t remote_crc, int player_index) {
    char filename[MAX_PATH];
    char base[64];
    std::snprintf(base, sizeof(base),
                  "FM95_P%d_desync_f%d.log", player_index + 1, frame);
    if (!Fm2k_BuildLogPath(filename, sizeof(filename), base)) {
        std::snprintf(filename, sizeof(filename), "%s", base);
    }
    FILE* f = std::fopen(filename, "w");
    if (!f) return;
    std::fprintf(f, "=== FM95 Desync Diagnostic — frame %d ===\n", frame);
    std::fprintf(f, "Local CRC : 0x%08X\n", local_crc);
    std::fprintf(f, "Remote CRC: 0x%08X\n", remote_crc);
    std::fprintf(f, "RNG seed  : 0x%08X\n", *(uint32_t*)fm95save::RNG_SEED);
    std::fprintf(f, "Tick      : %u\n",
                 *(uint32_t*)fm95save::GAME_TICK_COUNTER);
    std::fprintf(f, "Mode      : %u\n", *(uint32_t*)fm95save::GAME_MODE);
    std::fprintf(f, "ObjPool   : 0x%08X\n",
                 Fletcher32((const uint8_t*)fm95save::OBJECT_POOL,
                            FM2K::SIZE_OBJECT_POOL));
    std::fprintf(f, "P1 ring   : 0x%08X\n",
                 Fletcher32((const uint8_t*)fm95save::P1_INPUT_HISTORY,
                            fm95save::HISTORY_RING_SZ));
    std::fprintf(f, "P2 ring   : 0x%08X\n",
                 Fletcher32((const uint8_t*)fm95save::P2_INPUT_HISTORY,
                            fm95save::HISTORY_RING_SZ));
    std::fprintf(f, "PlrRound  : 0x%08X\n",
                 Fletcher32((const uint8_t*)fm95save::PLAYER_ROUND_STATE,
                            fm95save::PLAYER_ROUND_STATE_SZ));
    std::fclose(f);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "FM95 desync diagnostic written to %s", filename);
}

const RegionChecksums& SaveState_GetRegionChecksums() {
    static RegionChecksums z = {};
    z.rng         = *(uint32_t*)fm95save::RNG_SEED;
    z.object_pool = Fletcher32((const uint8_t*)fm95save::OBJECT_POOL,
                               FM2K::SIZE_OBJECT_POOL);
    z.gameplay_fingerprint = SaveState_CalculateFingerprint();
    z.combined    = SaveState_CalculateFullChecksum();
    return z;
}
#else  // FM2K — full body below

bool SaveState_Save(int frame) {
    const uint64_t _ss_t_entry = g_ss_perf_on ? SsNowNs() : 0;
    // Handle frame -1 as frame 0 (initial state before first frame)
    // GekkoNet sends this during initialization
    if (frame < 0) {
        frame = 0;
    }

    // Initial-sync step: SaveState_DoInitialSync() now fires eagerly from
    // Netplay_Start*Battle BEFORE the first AdvEvent — see the comment
    // at SaveState_DoInitialSync() below for the full reasoning. The
    // lazy reset that used to fire here (gated on g_initial_sync_done)
    // clobbered the first frame's PGI work and mis-aligned host's
    // buf_idx vs replay's. Now it's a no-op on the first Save (sync
    // already done).
    if (!g_initial_sync_done) {
        // Safety net: if for some reason eager-init wasn't called,
        // fall back to the lazy reset. Should never fire in practice
        // since Netplay_Start*Battle always calls
        // SaveState_DoInitialSync() now.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SaveState_Save: eager-init didn't run? Falling back to "
            "lazy reset (may misalign buf_idx).");
        *(uint32_t*)ADDR_INPUT_BUFFER_INDEX = 0;
        *(uint32_t*)ADDR_RENDER_FRAME_COUNTER = 0;
        memset((void*)0x447F00, 0, 0x20);
        memset((void*)0x447F40, 0, 0x20);
        memset((void*)0x447F60, 0, 0x20);
        memset((void*)ADDR_INPUT_HISTORY, 0, SIZE_INPUT_HISTORY);
        g_initial_sync_done = true;
    }

    int slot = frame % MAX_ROLLBACK_FRAMES;
    SaveStateData* state = &g_state_buffer[slot];

    // Track the slot for SaveState_PatchPostRenderRng. Set BEFORE we mutate
    // the slot so a same-frame re-save (replay save) still reaches the same
    // slot — both forward and replay back-patch the same address.
    g_last_saved_slot  = slot;
    g_last_saved_frame = frame;

    // Detect: is this a re-save of a frame we already saved (= REPLAY save
    // following a Load+Advance), or the first save (= FORWARD-sim save)?
    // The slot is keyed by (frame % MAX), so a true re-save has a matching
    // frame_number AND a valid prior CRC snapshot.
    bool is_replay_save = (state->frame_number == (uint32_t)frame
                        && state->saved_region_crcs.valid);

    // Byte-level first-diff detector + side-file writer. MUST run BEFORE the
    // memcpy block below overwrites state->object_pool / afterimage_pool /
    // char_dynamic with live (replay) bytes — otherwise fwd and cur become
    // identical and the scan reports nothing. On a replay save,
    // state->object_pool still holds the FORWARD bytes from the first save.
    //
    // Replay-diff entries get appended to a SINGLE consolidated file
    // (REPLAY_DIFF_LOG = "FM2K_replay_diffs.log") to avoid flooding the disk
    // with one file per frame. Each entry is delimited by a header line so
    // they're easy to grep/separate.
    // Throttle the entire replay-diff scan to run at most once per second.
    // Without the throttle every replay save (100 Hz) computed Fletcher32
    // over ~1 MB of memory (object_pool + afterimage_pool + 8 × char_slot)
    // just to produce diagnostics — dominant CPU cost in stress mode.
    // Gekko's own desync detection (via gameplay_fingerprint as the save
    // checksum) is unaffected; this only gates the per-byte localization
    // scan used for post-desync investigation.
    static DWORD s_last_scan_tick = 0;
    DWORD s_scan_now = GetTickCount();
    bool run_scan = is_replay_save && (s_scan_now - s_last_scan_tick) >= 1000;
    if (run_scan) s_last_scan_tick = s_scan_now;

    if (run_scan) {
        const auto& fwd_crcs = state->saved_region_crcs;

        // Compute current (replay-side) region CRCs inline so we can gate the
        // diff log on actual divergence. Per-region only here — full
        // this_save_crcs is built AFTER the memcpy below.
        uint32_t cur_obj_crc = Fletcher32((uint8_t*)ADDR_OBJECT_POOL,
                                          SIZE_OBJECT_POOL);
        // Compute cur_ai_crc with the SAME exclusion the save buffer gets:
        // zero the 4 bytes at offset 0x4A4 (= g_last_frame_time @ 0x447DD4)
        // in a local copy before hashing, so this CRC is comparable to
        // fwd_crcs.afterimage_pool (which was hashed from the save buffer
        // after the zero-out). Without this, every replay save would flag
        // a spurious DIFF because live memory has the real timer byte
        // while the save-buffer CRC was over the zeroed version.
        static uint8_t s_ai_cmp_buf[WaveCAddrs::AFTERIMAGE_POOL_SZ];
        memcpy(s_ai_cmp_buf, (void*)WaveCAddrs::AFTERIMAGE_POOL, WaveCAddrs::AFTERIMAGE_POOL_SZ);
        constexpr size_t G_LFT_OFFSET = 0x447DD4 - WaveCAddrs::AFTERIMAGE_POOL;  // 0x4A4
        *(uint32_t*)(s_ai_cmp_buf + G_LFT_OFFSET) = 0;
        uint32_t cur_ai_crc  = Fletcher32(s_ai_cmp_buf, WaveCAddrs::AFTERIMAGE_POOL_SZ);
        uint32_t cur_char_dyn = 0;
        for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
            uintptr_t da = CHAR_SLOT_BASE + i * CHAR_SLOT_SIZE + CHAR_SLOT_DYNAMIC_OFFSET;
            cur_char_dyn ^= Fletcher32((uint8_t*)da, CHAR_SLOT_DYNAMIC_SIZE);
        }

        bool any_region_diff = (fwd_crcs.object_pool     != cur_obj_crc)
                            || (fwd_crcs.afterimage_pool != cur_ai_crc)
                            || (fwd_crcs.char_dynamic    != cur_char_dyn);

        // Gekko-authoritative verdict: recompute the gameplay fingerprint
        // (HP/pos/rng/timer/current-input -- the SAME hash gekko uses as the
        // save checksum) from live (replay) memory and compare to the forward
        // save's fingerprint. This is the ONLY divergence that constitutes a
        // REAL desync. A region CRC can differ while the fingerprint matches --
        // that's NON-AUTHORITATIVE noise (e.g. g_processed_input recomputed
        // from the menu-only g_input_repeat_state/timer @0x541F80/0x4D1C40,
        // which battle never reads; the deliberately-zeroed shake/effect carve-
        // outs; or a spawned-effect object's subtype). Proven 2026-06-14:
        // Tyrogue-mirror stress at FM2K_PREDICTION_WINDOW=0 + CHECK_DISTANCE=12
        // logged region diffs every ~100f yet gekko reported desync=0 across
        // ~987 forced rollbacks -- i.e. the rollback save/restore IS
        // deterministic for everything that matters. Recompute is idempotent
        // with the normal-save fingerprint at line ~1020 (same live memory).
        const uint32_t cur_fingerprint = SaveState_CalculateFingerprint();
        const bool fingerprint_diff =
            (fwd_crcs.gameplay_fingerprint != cur_fingerprint);

        // Rate-limit the SDL console log: the byte-level scan re-fires every
        // replay save while a divergence persists across frames, which floods
        // stderr. File output stays per-event (useful for post-mortem); the
        // console gets at most 1 log per second per diagnostic call-site.
        static DWORD s_last_sdl_log_tick = 0;
        DWORD s_now_tick = GetTickCount();
        bool should_sdl_log = (s_now_tick - s_last_sdl_log_tick) >= 1000;
        if (should_sdl_log) s_last_sdl_log_tick = s_now_tick;

        FILE* df = nullptr;
        if (any_region_diff) {
            df = fopen(ReplayDiffLogPath(), "a");  // append to the consolidated log
            if (df) {
                fprintf(df, "\n========================================\n");
                fprintf(df, "REPLAY DIFF for frame %d\n", frame);
                fprintf(df, "========================================\n");
                fprintf(df, "  forward.combined=0x%08X\n", fwd_crcs.combined);
                fprintf(df, "  forward.rng=0x%08X\n", fwd_crcs.rng);
                fprintf(df, "  ObjectPool:     fwd=0x%08X  replay=0x%08X  %s\n",
                        fwd_crcs.object_pool, cur_obj_crc,
                        fwd_crcs.object_pool == cur_obj_crc ? "MATCH" : "DIFF");
                fprintf(df, "  AfterimagePool: fwd=0x%08X  replay=0x%08X  %s\n",
                        fwd_crcs.afterimage_pool, cur_ai_crc,
                        fwd_crcs.afterimage_pool == cur_ai_crc ? "MATCH" : "DIFF");
                fprintf(df, "  CharDynamic:    fwd=0x%08X  replay=0x%08X  %s\n",
                        fwd_crcs.char_dynamic, cur_char_dyn,
                        fwd_crcs.char_dynamic == cur_char_dyn ? "MATCH" : "DIFF");
                // Authoritative verdict line -- the reader's first stop.
                fprintf(df, "  GameplayFingerprint: fwd=0x%08X  replay=0x%08X  %s\n",
                        fwd_crcs.gameplay_fingerprint, cur_fingerprint,
                        fingerprint_diff ? "DIFF" : "MATCH");
                fprintf(df, "  VERDICT: %s\n", fingerprint_diff
                        ? "*** REAL DESYNC (gekko-authoritative fingerprint DIFFERS) -- the region diffs below are the cause; investigate."
                        : "NON-AUTHORITATIVE NOISE -- fingerprint MATCHES, so this is NOT a desync (menu input-repeat / zeroed render carve-outs / spawned-effect subtype). Safe to ignore for determinism.");
            }
        }

        // ObjectPool byte scan — pinpoints first divergent object + field.
        if (fwd_crcs.object_pool != cur_obj_crc) {
            const uint8_t* fwd = state->object_pool;
            const uint8_t* cur = (const uint8_t*)ADDR_OBJECT_POOL;
            for (size_t i = 0; i < SIZE_OBJECT_POOL; i++) {
                if (fwd[i] != cur[i]) {
                    int obj_idx = (int)(i / OBJECT_POOL_STRIDE);
                    int field_off = (int)(i % OBJECT_POOL_STRIDE);
                    uintptr_t obj_base = ADDR_OBJECT_POOL + obj_idx * OBJECT_POOL_STRIDE;
                    // SDL console log disabled — file output (REPLAY_DIFF_LOG)
                    // captures the same data for post-mortem. Keep the flag
                    // variable alive for grep-based re-enable.
                    if (false && should_sdl_log) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "REPLAY DIFF f=%d ObjectPool: obj[%d] @ 0x%X +0x%X (=%d) "
                        "forward=0x%02X replay=0x%02X",
                        frame, obj_idx, (unsigned)obj_base, field_off, field_off,
                        fwd[i], cur[i]);
                    if (df) {
                        fprintf(df, "\nFirst differing BYTE in ObjectPool:\n"
                                "  obj[%d] @ 0x%08X +0x%X (= +%d)\n"
                                "  forward = 0x%02X  replay = 0x%02X\n"
                                "  fwd ctx (16 B): ",
                                obj_idx, (unsigned)obj_base, field_off, field_off,
                                fwd[i], cur[i]);
                        for (int k = 0; k < 16 && i+k < SIZE_OBJECT_POOL; k++)
                            fprintf(df, "%02X ", fwd[i+k]);
                        fprintf(df, "\n  replay ctx     : ");
                        for (int k = 0; k < 16 && i+k < SIZE_OBJECT_POOL; k++)
                            fprintf(df, "%02X ", cur[i+k]);
                        fprintf(df,
                            "\n  KgtRuntimeObject field map:\n"
                            "    +0..3 type | +4..7 subtype (=create_game_object a2; spawn id) | +8..15 xPos/yPos (q16)\n"
                            "    +40 flags40 | +44 itemIdx | +48 scriptId | +52 prevScriptId\n"
                            "    +60 wait | +88 gravBase | +92 facing | +137..216 obj-mgr A\n"
                            "    +217..296 box-ptr array | +338 stateInit | +342 pid\n"
                            "    +346 role | +350 flags350\n"
                            "  NOTE: spawned-object (+4 subtype etc.) divergences CASCADE from input-edge\n"
                            "  divergence (g_prev_input_state @0x447F00 / g_input_changes @0x447F60). Under\n"
                            "  CHECK_DISTANCE self-check the 'forward' save may be a SPECULATIVE (predicted)\n"
                            "  save vs a post-confirmation 'replay' save -- such a diff is legitimate\n"
                            "  prediction-correction, not a determinism bug. Cross-check gekko fingerprint.\n");
                    }
                    break;
                }
            }
        }
        // AfterimagePool byte scan. Compare against s_ai_cmp_buf (live memory
        // with g_last_frame_time already zeroed to match the save buffer),
        // NOT raw live memory — otherwise every entry reports a phantom
        // diff at +0x4A4 from our deliberate save-time exclusion.
        if (fwd_crcs.afterimage_pool != cur_ai_crc) {
            const uint8_t* fwd = state->afterimage_pool;
            const uint8_t* cur = s_ai_cmp_buf;
            for (size_t i = 0; i < WaveCAddrs::AFTERIMAGE_POOL_SZ; i++) {
                if (fwd[i] != cur[i]) {
                    // SDL console log disabled — file output (REPLAY_DIFF_LOG)
                    // captures the same data for post-mortem. Keep the flag
                    // variable alive for grep-based re-enable.
                    if (false && should_sdl_log) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "REPLAY DIFF f=%d AfterimagePool: +0x%X fwd=0x%02X replay=0x%02X",
                        frame, (unsigned)i, fwd[i], cur[i]);
                    if (df) {
                        fprintf(df,
                            "\nFirst differing BYTE in AfterimagePool:\n"
                            "  +0x%X (= byte %zu) fwd=0x%02X replay=0x%02X\n",
                            (unsigned)i, i, fwd[i], cur[i]);
                    }
                    break;
                }
            }
        }
        // CharDynamic byte scan (per slot)
        if (fwd_crcs.char_dynamic != cur_char_dyn) {
            for (size_t s = 0; s < NUM_CHAR_SLOTS; s++) {
                const uint8_t* fwd = state->char_dynamic[s];
                const uint8_t* cur = (const uint8_t*)(CHAR_SLOT_BASE + s * CHAR_SLOT_SIZE);
                // Skip UNLOADED slots: Save() only memcpys loaded slots (.kgt
                // magic byte0 != 0); for unloaded slots char_dynamic[s] is a
                // STALE buffer, so comparing it vs live yields a guaranteed
                // false "divergence" (the CharSlot[2] artifact). Only loaded
                // slots can be a real re-sim divergence. (Diagnostic-only gate;
                // does NOT touch the actual save/restore path.)
                if (fwd[0] == 0) continue;
                bool found = false;
                for (size_t i = 0; i < CHAR_SLOT_DYNAMIC_SIZE; i++) {
                    if (fwd[i] != cur[i]) {
                        // SDL console log disabled — file output (REPLAY_DIFF_LOG)
                    // captures the same data for post-mortem. Keep the flag
                    // variable alive for grep-based re-enable.
                    if (false && should_sdl_log) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                            "REPLAY DIFF f=%d CharSlot[%zu]: +0x%X fwd=0x%02X replay=0x%02X",
                            frame, s, (unsigned)i, fwd[i], cur[i]);
                        if (df) {
                            fprintf(df,
                                "\nFirst differing BYTE in CharSlot[%zu]:\n"
                                "  +0x%X fwd=0x%02X replay=0x%02X\n",
                                s, (unsigned)i, fwd[i], cur[i]);
                        }
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
        }

        if (df) fclose(df);  // explicit flush — survives TerminateProcess
    }

    state->frame_number = frame;

    // RNG seed: capture live (= pre-render value at SaveEvent time). The
    // trampoline back-patches this slot via SaveState_PatchPostRenderRng
    // after render returns, so by the time anyone Loads from this slot the
    // rng_seed reflects post-render rng. That gives both vanilla-matching
    // visual evolution AND rollback determinism: forward and replay both
    // start from post-render-of-prev-frame rng for any given frame.
    state->rng_seed = *(uint32_t*)ADDR_RNG_SEED;

    // Render frame counter: ZERO the saved copy (deterministic CRC), DO NOT
    // capture live state. Rationale identical to the shake_effects carve-out
    // — render_frame_counter is incremented at the end of every render_game
    // call and is the basis for ProcessShakeEffect's per-frame parity-flip
    // (negate offset on odd renders). If we save+restore the live counter,
    // GekkoNet's per-frame Save+Load cycle freezes it at whatever value Save
    // captured, the parity stops alternating, and screen shake only shifts
    // one direction — making the stage look permanently offset. See the
    // matching carve-outs for shake_effects below.
    state->render_frame_counter = 0;

    // Save input buffer index - CRITICAL for rollback!
    state->input_buffer_index = *(uint32_t*)ADDR_INPUT_BUFFER_INDEX;

    // Save input tracking state - CRITICAL for correct input change detection!
    memcpy(state->input_tracking_state, (void*)ADDR_INPUT_TRACKING, SIZE_INPUT_TRACKING);

    // Character slots: 8 × 57407 B = 460 KB per save. In 1v1 only slots 0
    // and 1 are loaded; slots 2-7 hold stale AI-scratch bytes that the PvP
    // sim path never reads (per IDA audit). Each loaded slot starts with
    // the ".kgt" magic bytes 0x32 0x44 0x4B 0x47 ("2DKG"); unloaded slots
    // start with 0x00. Use the first byte as a cheap active-flag.
    //
    //   Typical cost before: 460 KB memcpy per save.
    //   Typical cost after : 8 × 1-byte check + 2 × 57 KB memcpy ≈ 114 KB.
    if (g_ss_perf_on) { g_ss_head.ns += SsNowNs() - _ss_t_entry; g_ss_head.n++; }
    { SScope _s(&g_ss_char);
    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t slot_base = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE);
        uintptr_t dynamic_addr = slot_base + CHAR_SLOT_DYNAMIC_OFFSET;
        if (*(uint8_t*)slot_base != 0) {
            memcpy(state->char_dynamic[i], (void*)dynamic_addr, CHAR_SLOT_DYNAMIC_SIZE);
        } else {
            state->char_dynamic[i][0] = 0;  // mark slot inactive for Load
        }
    }
    }

    // Save object pool (projectiles, effects)
    // Object pool: active-slot-only save. First byte of each 382 B slot is
    // the "active" flag (0 = slot empty). In 1v1 typically <20 of 1023 slots
    // are live; saving 391 KB every frame was dominating memcpy cost. Skip
    // inactive slots entirely — only their first byte is written (=0), which
    // tells the Load path "clear this slot".
    //
    //   Typical cost before: 391 KB memcpy per save.
    //   Typical cost after : 1023 × 1-byte read + ~10 × 382 B memcpy ≈ 5 KB.
    {
        SScope _s(&g_ss_obj);
        constexpr size_t OBJ_SZ = OBJECT_POOL_STRIDE;
        constexpr size_t OBJ_COUNT = SaveStateData::SavedRegionCRCs::OBJ_SLOT_COUNT;
        const uint8_t* src_base = (const uint8_t*)ADDR_OBJECT_POOL;
        uint8_t* dst_base = state->object_pool;
        for (size_t i = 0; i < OBJ_COUNT; i++) {
            const uint8_t* src = src_base + i * OBJ_SZ;
            uint8_t* dst = dst_base + i * OBJ_SZ;
            if (src[0] != 0) {
                memcpy(dst, src, OBJ_SZ);  // active slot: full copy
            } else {
                dst[0] = 0;                 // inactive: mark only (rest stale, OK)
            }
        }
    }

    // Save input history
    g_ss_misc.ns -= g_ss_perf_on ? SsNowNs() : 0;
    memcpy(state->input_history, (void*)ADDR_INPUT_HISTORY, SIZE_INPUT_HISTORY);

    // Save game state
    memcpy(state->game_state, (void*)ADDR_GAME_STATE, SIZE_GAME_STATE);
    if (g_ss_perf_on) { g_ss_misc.ns += SsNowNs(); g_ss_misc.n++; }

    // Save effect/shake state - affects animation script execution during rollback
    memcpy(state->effect_sys1, (void*)EffectAddrs::EFFECT_SYS1, EffectAddrs::EFFECT_SYS1_SZ);
    memcpy(state->effect_sys2, (void*)EffectAddrs::EFFECT_SYS2, EffectAddrs::EFFECT_SYS2_SZ);
    // EFFECT_SYS1 (palette-flash-1, 42 B): zero the saved buffer + skip
    // restore on Load. ProcessColorInterpolation (render-side) reads this
    // every frame and may call game_rand based on its state — and the
    // timer in update_game_state decrements each frame. For host+replay
    // determinism, BOTH engines need to see the SAME palette state at
    // each render. That happens iff palette evolves continuously through
    // forward sims on both sides (1 decrement per main-loop tick). If we
    // save/restore, host's rollback Load resets palette to pre-render-mod
    // state while replay keeps accumulating — divergent palette → divergent
    // render rng calls → drift. The "skip restore" pattern is the only one
    // that keeps host's many rollbacks aligned with replay's straight
    // forward-only sim. Same logic for shake_effects + pflash2 below.
    std::memset(state->effect_sys1, 0, EffectAddrs::EFFECT_SYS1_SZ);
    // EFFECT_SYS2 carve-outs (REVERTED back to original — same reasoning
    // as effect_sys1 above):
    //   - 0x00..0x20 (sysvars): KEEP saved
    //   - 0x20..0x4C (palette-flash-2 struct, 44 B at 0x4456D0): ZERO + skip
    //                  restore. Palette state must evolve naturally on both
    //                  host and replay so render-side game_rand calls produce
    //                  identical rng sequences.
    //   - 0x4C..0x50 (g_render_frame_counter, 4 B): ZERO + skip restore.
    //   - 0x50..0x58 (tail): KEEP saved
    constexpr size_t PFLASH2_OFFSET_IN_SYS2 =
        0x4456D0 - EffectAddrs::EFFECT_SYS2;  // 0x20
    constexpr size_t PFLASH2_SIZE = 44;
    constexpr size_t RENDER_FRAME_COUNTER_OFFSET_IN_SYS2 =
        ADDR_RENDER_FRAME_COUNTER - EffectAddrs::EFFECT_SYS2;  // 0x4C
    std::memset(state->effect_sys2 + PFLASH2_OFFSET_IN_SYS2, 0, PFLASH2_SIZE);
    *(uint32_t*)(state->effect_sys2 + RENDER_FRAME_COUNTER_OFFSET_IN_SYS2) = 0;
    // Afterimage_pool overlap: EFFECT_SYS1 (0x447D7D, 42 B) is INSIDE the
    // afterimage_pool save range. Zero it in the saved afterimage_pool too
    // so the load doesn't re-inject palette state via that back-door.
    state->round_end_flag = *(uint32_t*)0x424718;
    // Shake effects: zero in save + skip restore. REVERTED to original.
    // ProcessShakeEffect decrements g_shake_effect_*.timer in render. For
    // host (with rollback) and replay (no rollback) to consume the same
    // game_rand calls during render, shake state must evolve naturally
    // through forward render passes on BOTH sides — saving/restoring on
    // rollback would reset host's shake state to a pre-render-mod value
    // each Load, while replay (no rollback) keeps the natural evolution.
    // Divergent shake → divergent render rng → drift.
    memset(state->shake_effects, 0, EffectAddrs::SHAKE_EFFECTS_SZ);

    // Wave C additions: afterimage pool + object list topology.
    //
    // IMPORTANT: the "afterimage pool" range 0x447930..0x46F6C0 is a big slice
    // of the data segment that happens to CONTAIN the afterimage buffers but
    // also contains unrelated globals. The only one we've proven harmful for
    // determinism is `g_last_frame_time @ 0x447DD4` (offset 0x4A4) — written
    // by main_game_loop's pacing arithmetic (0x405AE9/AFD/BA8/BBB) and by
    // main_window_proc on alt-tab/F4/Alt+Enter (0x40610F/0x4062A5) and by
    // the menu handler (0x4177BF). None of those writers are sim code —
    // nothing gameplay-visible reads from `g_last_frame_time`. We zero the
    // saved copy at that offset so forward- and replay-sim both produce the
    // same saved bytes there, killing the f=9 AfterimagePool +0x4A4 diff.
    // Live memory keeps whatever main_game_loop last wrote; the save buffer
    // has a deterministic 0 at that slot.
    { SScope _s(&g_ss_after);
      memcpy(state->afterimage_pool,       (void*)WaveCAddrs::AFTERIMAGE_POOL,      WaveCAddrs::AFTERIMAGE_POOL_SZ); }
    constexpr size_t G_LAST_FRAME_TIME_OFFSET = 0x447DD4 - WaveCAddrs::AFTERIMAGE_POOL;  // 0x4A4
    *(uint32_t*)(state->afterimage_pool + G_LAST_FRAME_TIME_OFFSET) = 0;
    // Shake region overlap: g_shake_effect_1/_2 (40 bytes at 0x447DA9) live
    // INSIDE the afterimage_pool slice. Zero them in the saved copy too —
    // mirror of the dedicated state->shake_effects treatment above. Without
    // this, the next Restore would re-inject the post-[EB] timer values via
    // the afterimage_pool path (defeating the dedicated-slot fix).
    // Shake / palette-flash-1 overlap inside afterimage_pool: zero them
    // in the saved copy so Load doesn't re-inject post-[EB] timer values
    // via the afterimage_pool memcpy. The dedicated state->shake_effects
    // and state->effect_sys1 slots are also zeroed (above) for the same
    // reason. End result: render-side timer state evolves continuously
    // across both host and replay without rollback resets.
    constexpr size_t SHAKE_OFFSET_IN_AI = EffectAddrs::SHAKE_EFFECTS - WaveCAddrs::AFTERIMAGE_POOL;
    memset(state->afterimage_pool + SHAKE_OFFSET_IN_AI, 0, EffectAddrs::SHAKE_EFFECTS_SZ);
    constexpr size_t PFLASH1_OFFSET_IN_AI = EffectAddrs::EFFECT_SYS1 - WaveCAddrs::AFTERIMAGE_POOL;
    static_assert(PFLASH1_OFFSET_IN_AI + EffectAddrs::EFFECT_SYS1_SZ <= SHAKE_OFFSET_IN_AI,
                  "EFFECT_SYS1 must end before shake region in afterimage_pool");
    memset(state->afterimage_pool + PFLASH1_OFFSET_IN_AI, 0, EffectAddrs::EFFECT_SYS1_SZ);
    state->current_object_ptr            = *(uint32_t*)WaveCAddrs::CURRENT_OBJECT_PTR;
    memcpy(state->object_list_heads_tails, (void*)WaveCAddrs::OBJECT_LIST_HEADS,    WaveCAddrs::OBJECT_LIST_HEADS_SZ);
    memcpy(state->object_node_pool,        (void*)WaveCAddrs::OBJECT_NODE_POOL,     WaveCAddrs::OBJECT_NODE_POOL_SZ);

    // Mike Z sound-rollback: capture per-channel "desired" state. This is NOT
    // DSound hardware state — it's the sim's record of what each channel
    // should be playing. Actual DSound plays are driven post-advance by the
    // sync step; hardware state is deliberately not rolled back.
    { SScope _s(&g_ss_sound); SoundRollback::CaptureDesired(state->sound_desired); }

    // Checksum path split for perf:
    //   - Always compute the cheap gameplay fingerprint (~44 B hash). This
    //     is what GekkoNet compares for desync detection via
    //     *update->data.save.checksum in netplay.cpp.
    //   - Only run the full ~1 MB per-region diagnostic hash when the
    //     replay-diff scan is due to run (same 1/sec throttle). At other
    //     times saved_region_crcs contains only fingerprint + rng;
    //     per-region fields are zeroed (not a problem because nothing
    //     reads them outside the throttled scan path).
    { SScope _s(&g_ss_fp); SaveState_CalculateFingerprint(); }
    const uint64_t _ss_t_tail = g_ss_perf_on ? SsNowNs() : 0;
    static DWORD last_full_crc_tick = 0;
    DWORD full_crc_now = GetTickCount();
    // FM2K_FULL_CRCS=1 forces the expensive per-region CRC on EVERY save
    // event (vs the default 1/sec throttle). For autonomous Phase F
    // determinism diagnostics — without this, the desync dump's
    // forward-vs-replay per-region diff shows "0x00000000 MATCH" for
    // most regions (because they weren't computed on that save event)
    // and we can't attribute the divergence to a specific region.
    // Cost: ~5ms per save. Don't ship enabled.
    static int s_full_crcs_cached = -1;
    if (s_full_crcs_cached < 0) {
        const char* v = getenv("FM2K_FULL_CRCS");
        s_full_crcs_cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    bool full_crcs_due = s_full_crcs_cached == 1
                         || (full_crc_now - last_full_crc_tick) >= 1000;
    if (full_crcs_due) {
        last_full_crc_tick = full_crc_now;
        SScope _s(&g_ss_full);
        SaveState_CalculateFullChecksum();
    }
    state->checksum = g_region_checksums.combined;  // zero on non-due saves, fine
    uint32_t checksum = state->checksum;

    // Build a fresh per-region CRC snapshot for THIS save (forward or replay).
    //
    // PERF: The cheap fingerprint fields (rng/game_state/object_pool/etc) are
    // populated by SaveState_CalculateFingerprint above and just copied here.
    // The EXPENSIVE Fletcher32 calls — afterimage_pool (~159 KB), list_heads
    // (1 KB), node_pool (8 KB) — are gated on full_crcs_due so they only fire
    // once per second instead of every save. During an 8-frame rollback the
    // host fires up to 16 saves in a single outer iteration; without this gate
    // those three hashes alone burned ~5.4ms of the 10ms frame budget,
    // overflowing it and "eating" the local player's keystrokes that fell
    // between Input_CaptureLocal samples. CRCs are diagnostic only (only used
    // by the desync-dump path's per-region diff and the throttled replay-diff
    // scan above) so once-per-second resolution is plenty.
    SaveStateData::SavedRegionCRCs this_save_crcs = {};
    {
        const auto& rc = g_region_checksums;
        this_save_crcs.rng                    = rc.rng;
        this_save_crcs.gameplay_fingerprint   = rc.gameplay_fingerprint;
        this_save_crcs.fp_in                  = rc.fp_inputs;
        if (full_crcs_due) {
            this_save_crcs.game_state             = rc.game_state;
            this_save_crcs.object_pool            = rc.object_pool;
            this_save_crcs.char_dynamic           = rc.char_dynamic;
            this_save_crcs.input_tracking         = rc.input_tracking;
            this_save_crcs.effect_sys1            = rc.effect_sys1;
            this_save_crcs.effect_sys2            = rc.effect_sys2;
            this_save_crcs.shake_effects          = rc.shake_effects;
            this_save_crcs.combined               = rc.combined;
            // Hash the SAVED copy of afterimage_pool (with g_last_frame_time zeroed
            // above), NOT live memory — this is what makes the exclusion work
            // across forward- and replay-sim.
            this_save_crcs.afterimage_pool        = Fletcher32(state->afterimage_pool,                     WaveCAddrs::AFTERIMAGE_POOL_SZ);
            this_save_crcs.list_heads_tails       = Fletcher32((uint8_t*)WaveCAddrs::OBJECT_LIST_HEADS,    WaveCAddrs::OBJECT_LIST_HEADS_SZ);
            this_save_crcs.node_pool              = Fletcher32((uint8_t*)WaveCAddrs::OBJECT_NODE_POOL,     WaveCAddrs::OBJECT_NODE_POOL_SZ);
        }
        this_save_crcs.current_object_ptr_val = *(uint32_t*)WaveCAddrs::CURRENT_OBJECT_PTR;

        // Per-object-slot CRCs: NOT computed here. ~400 KB of Fletcher32 per
        // save × 10 saves/check_distance × rollback-every-10-frames = ~14 MB/s
        // of hashing overhead in stress mode. At dump time we compute these
        // on demand from state->object_pool (forward bytes are still stored).
        // Slot CRCs stay zeroed until then; the dump path recomputes both
        // sides from the saved byte buffers to do its per-slot diff.
        memset(this_save_crcs.object_slot_crcs, 0,
               sizeof(this_save_crcs.object_slot_crcs));

        this_save_crcs.valid = true;
    }

    if (is_replay_save) {
        // Second save for this frame = REPLAY save. Keep forward (already in
        // state->saved_region_crcs) and stash this snapshot in the replay
        // shadow keyed by slot. The desync dump diffs the two.
        // (The byte-level + side-file diff was already written EARLIER, before
        // the memcpy block, so state->object_pool still held forward's bytes.)
        g_replay_saves[slot].frame_number = frame;
        g_replay_saves[slot].crcs         = this_save_crcs;
        g_replay_saves[slot].valid        = true;
    } else {
        // First save for this frame = FORWARD save. Stash here, clear any
        // stale replay shadow for this slot from a previous match.
        state->saved_region_crcs       = this_save_crcs;
        g_replay_saves[slot].frame_number = -1;
        g_replay_saves[slot].valid        = false;
    }

    // Per-save logging.
    //   First 10 frames: verbose (combined + fingerprint + rng + buf_idx).
    //   After that: push into the in-memory rngtrace ring. Writing a console
    //   line every save at 100 fps tanks framerate and is user-visible.
    //   The ring is flushed to FM2K_P<N>_rngtrace.csv when SaveState_FlushRngTrace
    //   is called (on desync detection or session end). Non-rollback saves
    //   only — replay ticks are not part of the authoritative per-frame trace.
    static int save_log_count = 0;
    if (save_log_count++ < 10) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SAVE f=%d: combined=0x%08X fingerprint=0x%08X (rng=0x%08X) buf_idx=%u",
            frame, checksum, SaveState_GetRegionChecksums().gameplay_fingerprint,
            state->rng_seed, state->input_buffer_index);
    } else if (!g_is_rolling_back) {
        SaveState_PushRngTrace(frame, state->rng_seed,
            SaveState_GetRegionChecksums().gameplay_fingerprint);
    }

    if (g_ss_perf_on) { g_ss_tail.ns += SsNowNs() - _ss_t_tail; g_ss_tail.n++; }
    SsMaybeReport();
    return true;
}

uint32_t SaveState_GetLastChecksum(int frame) {
    if (frame < 0) frame = 0;
    int slot = frame % MAX_ROLLBACK_FRAMES;
    return g_state_buffer[slot].checksum;
}

bool SaveState_Load(int frame) {
    // Handle frame -1 as frame 0 (initial state before first frame)
    if (frame < 0) {
        frame = 0;
    }

    int slot = frame % MAX_ROLLBACK_FRAMES;
    SaveStateData* state = &g_state_buffer[slot];

    if (state->frame_number != (uint32_t)frame) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SaveState: Frame mismatch! Wanted %d, slot has %u",
                     frame, state->frame_number);
        return false;
    }

    // RNG seed: RESTORE. SaveState_Save zeros the seed in the slot, then
    // SaveState_PatchPostRenderRng (called from RenderFrameWithSnapshot
    // after render returns) writes the post-render rng into the slot. So
    // the value we restore here is post-render rng of whatever frame the
    // saved slot represents — exactly what forward sim sees as the
    // starting rng for the NEXT frame, which is also what we want replay
    // sim to start with for rollback determinism.
    *(uint32_t*)ADDR_RNG_SEED = state->rng_seed;

    // Render frame counter: SKIP restore. Live counter is the only reliable
    // source — restoring would freeze it at the saved value and break shake
    // parity-flip (see Save() comment). Live memory keeps incrementing from
    // wherever it is, so ProcessShakeEffect's odd/even check actually toggles.

    // Restore input buffer index - CRITICAL for rollback!
    *(uint32_t*)ADDR_INPUT_BUFFER_INDEX = state->input_buffer_index;

    // Restore input tracking state - CRITICAL for correct input change detection!
    memcpy((void*)ADDR_INPUT_TRACKING, state->input_tracking_state, SIZE_INPUT_TRACKING);

    // Restore dynamic portion of each character slot
    //
    // CROSS-PROCESS SPEC CARVE-OUT: the 57407-byte char slot was loaded
    // from the .player file at engine init. It includes many heap-pointer
    // fields (g_charslotN_action_table at slot+0x100, sprite tables, etc)
    // populated by character_data_loader with addresses in THIS process's
    // heap. Bulk-memcpy'ing the host's snapshot onto our slot overwrites
    // those pointers with host-process heap addresses — they're wild here
    // and the engine AVs the moment something dereferences them (observed
    // in ui_state_manager on pkmncc spec join: `mov ebx, g_charslot0_-
    // action_table` then `mov cx, [edx+ebx+0x20]` AV'd reading host's
    // ~0x0B9B6708).
    //
    // The fix: BEFORE the memcpy, scan the live slot for DWORDs that
    // LOOK like heap pointers (>= 0x01000000, i.e. above the static-mem
    // ceiling on 32-bit Win). Capture them. After the memcpy, for each
    // captured offset, if the host's value there ALSO looks heap-shaped
    // (confirming the field is a pointer in BOTH processes — not a
    // coincidentally-high non-pointer value), restore our local pointer.
    // Non-pointer dynamic state (HP, super meter, anim frame indices,
    // etc — all small values) flows through normally from snapshot.
    //
    // Only enabled for spec mode (g_player_index==2); single-process
    // rollback for players keeps the bit-for-bit memcpy that determinism
    // depends on (same heap, valid pointers).
    extern int g_player_index;
    const bool is_spec_apply = (g_player_index == 2);
    constexpr uint32_t HEAP_PTR_FLOOR = 0x01000000u;

    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t slot_base = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE);
        uintptr_t dynamic_addr = slot_base + CHAR_SLOT_DYNAMIC_OFFSET;
        // Mirror of the active-slot save: only restore loaded slots (save
        // buffer's first byte != 0). Unloaded slots are left alone; if the
        // game hasn't loaded a character there nothing reads those bytes.
        if (state->char_dynamic[i][0] == 0) continue;

        if (!is_spec_apply) {
            memcpy((void*)dynamic_addr, state->char_dynamic[i], CHAR_SLOT_DYNAMIC_SIZE);
            continue;
        }

        // Spec path: capture pointer-shaped DWORDs in live slot first.
        // CHAR_SLOT_DYNAMIC_SIZE is currently the full slot (14352
        // DWORDs); the first cut at MAX_PRESERVED=1024 hit the cap on
        // both active slots, so we sized up generously. Lives on stack
        // (~64 KB × 2 fields = 128 KB) — well under the per-thread
        // stack budget. SaveState_Load isn't called frequently enough
        // for the upfront cost to matter.
        struct PtrPreserve { uint32_t didx; uint32_t value; };
        constexpr size_t MAX_PRESERVED = 8192;
        PtrPreserve preserved[MAX_PRESERVED];
        size_t preserved_count = 0;
        size_t pointer_candidates = 0;  // total >= HEAP_PTR_FLOOR (may exceed cap)

        const uint32_t* p = (const uint32_t*)dynamic_addr;
        const size_t    dword_count = CHAR_SLOT_DYNAMIC_SIZE / sizeof(uint32_t);
        for (size_t o = 0; o < dword_count; ++o) {
            if (p[o] >= HEAP_PTR_FLOOR) {
                ++pointer_candidates;
                if (preserved_count < MAX_PRESERVED) {
                    preserved[preserved_count++] = { (uint32_t)o, p[o] };
                }
            }
        }

        // FORCED carve-out windows (resource bookkeeping the per-slot
        // cleanup walks; same rationale as the stage block in the
        // afterimage apply below -- the heap-shape heuristic fails open
        // when the live slot hasn't finished loading):
        //   buffer 0x100..0x10C = game slot+0x110/0x114/0x118 ptrs
        //   buffer 0x220C..0x2224 = game slot+0x221C sound bookkeeping
        uint8_t live_res_a[0x0C], live_res_b[0x18];
        std::memcpy(live_res_a, (const uint8_t*)dynamic_addr + 0x100, sizeof(live_res_a));
        std::memcpy(live_res_b, (const uint8_t*)dynamic_addr + 0x220C, sizeof(live_res_b));

        memcpy((void*)dynamic_addr, state->char_dynamic[i], CHAR_SLOT_DYNAMIC_SIZE);

        std::memcpy((uint8_t*)dynamic_addr + 0x100,  live_res_a, sizeof(live_res_a));
        std::memcpy((uint8_t*)dynamic_addr + 0x220C, live_res_b, sizeof(live_res_b));

        // Restore preserved pointers IF the host's value there is also
        // heap-shaped. Confirms the field is a pointer in both processes
        // (rather than a non-pointer field that happened to be high in
        // ours but low in theirs).
        uint32_t* live = (uint32_t*)dynamic_addr;
        size_t restored = 0;
        for (size_t k = 0; k < preserved_count; ++k) {
            const PtrPreserve& ps = preserved[k];
            if (live[ps.didx] >= HEAP_PTR_FLOOR) {
                live[ps.didx] = ps.value;
                ++restored;
            }
        }
        if (pointer_candidates > 0) {
            // Diagnostic line — fires once per snapshot apply per active
            // slot. `candidates` is the total count of heap-shaped
            // DWORDs found in the live slot; `preserved` is how many fit
            // in the buffer (cap = MAX_PRESERVED); `restored` is the
            // subset whose post-memcpy host value was ALSO heap-shaped
            // (confirming a real pointer field). If candidates > cap we
            // need to bump MAX_PRESERVED — silently truncating would
            // leave some host pointers in place and re-crash.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SaveState: spec char_slot[%zu] candidates=%zu preserved=%zu "
                "restored=%zu (scanned %zu DWORDs, cap=%zu)",
                i, pointer_candidates, preserved_count, restored,
                dword_count, MAX_PRESERVED);
            if (pointer_candidates > MAX_PRESERVED) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SaveState: spec char_slot[%zu] pointer-preserve TRUNCATED "
                    "(%zu > cap %zu) — host pointers in the uncaptured tail "
                    "will fault on first deref; bump MAX_PRESERVED",
                    i, pointer_candidates, MAX_PRESERVED);
            }
        }
    }

    // Restore object pool
    // Object pool: mirror of the active-slot save. If the save buffer's
    // first byte is 0 the slot was inactive at save time — zero the live
    // slot to match (and prevent stale active data from bleeding through
    // if the slot is currently live but was dead at the saved frame).
    //
    // Carve-out: per-object color override fields at +68/72/76/80 (16 B at
    // offset 0x44 within each 382-byte object struct). These are written
    // by ProcessColorInterpolation during render and read by
    // sprite_rendering_engine (`mov edi, [ecx+44h]` @ 0x40D5C7) — sim
    // never reads them. They need to PERSIST across frames so palette mode
    // 1 (Tyrogue's "fade-to-black-and-stay") can keep showing the last
    // interpolated value after the timer hits 0 and ProcessColorInterpolation
    // skips writing. Restoring them on Load wipes that persistence and the
    // visual snaps back to default palette every frame. Three-slice
    // restore: pre-override, [SKIP] override, post-override. Same per-slot
    // logic for the inactive-slot case (zero the slot but skip the override
    // bytes — though "inactive" objects shouldn't have meaningful overrides
    // anyway).
    {
        constexpr size_t OBJ_SZ = OBJECT_POOL_STRIDE;
        constexpr size_t OBJ_COUNT = SaveStateData::SavedRegionCRCs::OBJ_SLOT_COUNT;
        constexpr size_t OVERRIDE_OFFSET = 68;
        constexpr size_t OVERRIDE_SIZE   = 16;
        constexpr size_t OVERRIDE_END    = OVERRIDE_OFFSET + OVERRIDE_SIZE;
        const uint8_t* src_base = state->object_pool;
        uint8_t* dst_base = (uint8_t*)ADDR_OBJECT_POOL;
        // Color-override carve-out (REVERTED back to original): skip
        // +68..+84 (16 B). ProcessColorInterpolation writes these during
        // render and sprite_rendering_engine reads them; sim never reads
        // them. Restoring on Load wipes Tyrogue mode-1 fade-to-black
        // persistence. Skipping the restore matches the same "evolve
        // render-side state continuously" pattern as effect_sys1 / shake
        // — host's rollback Load leaves it alone, replay's forward sim
        // evolves it naturally, both stay in sync at render time.
        //
        // Spec hub-relay: same cross-process heap-pointer hazard as
        // char_slot above. Object slots contain pointers to per-object
        // animation tables / effect lists / etc. that the host's
        // process allocated; copying them verbatim into our process
        // would AV on first deref. Capture live heap-shaped DWORDs
        // per-slot, memcpy, conditionally restore. Per-slot scan keeps
        // the preserve buffer small (~37 DWORDs per slot max);
        // MAX_PRESERVED_PER_OBJ=64 has headroom.
        constexpr size_t MAX_PRESERVED_PER_OBJ = 64;
        size_t total_obj_candidates = 0;
        size_t total_obj_restored   = 0;
        size_t total_obj_truncated  = 0;
        for (size_t i = 0; i < OBJ_COUNT; i++) {
            const uint8_t* src = src_base + i * OBJ_SZ;
            uint8_t* dst = dst_base + i * OBJ_SZ;
            if (src[0] != 0) {
                if (!is_spec_apply) {
                    // NOTE(task #34, 2026-06-11): object slot byte 299 (a
                    // render-written anim timer on stage BG objects) drifts
                    // record-vs-replay under forced rollbacks, but a
                    // skip-restore experiment on it changed NOTHING about
                    // the k=774 divergence -- it is sim-inert (red
                    // herring). The actual #34 mechanism is a one-slot
                    // input-history ring shift introduced at the first
                    // rollback (see CAM_DIAG findings in csm_diag.cpp);
                    // motion-command history reads then diverge.
                    memcpy(dst, src, OVERRIDE_OFFSET);
                    memcpy(dst + OVERRIDE_END, src + OVERRIDE_END,
                           OBJ_SZ - OVERRIDE_END);
                    continue;
                }
                // Spec path: preserve heap-shaped DWORDs in live slot
                // BEFORE memcpy, conditionally restore after. Skip the
                // override carve-out region (we don't write to it anyway).
                struct ObjPtrPreserve { uint16_t didx; uint32_t value; };
                ObjPtrPreserve preserved[MAX_PRESERVED_PER_OBJ];
                size_t pcount = 0;
                const uint32_t* live_r = (const uint32_t*)dst;
                const size_t obj_dwords          = OBJ_SZ / sizeof(uint32_t);
                const size_t override_start_dw   = OVERRIDE_OFFSET / sizeof(uint32_t);
                const size_t override_end_dw     = OVERRIDE_END / sizeof(uint32_t);
                for (size_t o = 0; o < obj_dwords; ++o) {
                    if (o >= override_start_dw && o < override_end_dw) continue;
                    if (live_r[o] >= HEAP_PTR_FLOOR) {
                        ++total_obj_candidates;
                        if (pcount < MAX_PRESERVED_PER_OBJ) {
                            preserved[pcount++] = { (uint16_t)o, live_r[o] };
                        } else {
                            ++total_obj_truncated;
                        }
                    }
                }
                memcpy(dst, src, OVERRIDE_OFFSET);
                memcpy(dst + OVERRIDE_END, src + OVERRIDE_END,
                       OBJ_SZ - OVERRIDE_END);
                uint32_t* live_w = (uint32_t*)dst;
                for (size_t k = 0; k < pcount; ++k) {
                    if (live_w[preserved[k].didx] >= HEAP_PTR_FLOOR) {
                        live_w[preserved[k].didx] = preserved[k].value;
                        ++total_obj_restored;
                    }
                }
            } else if (dst[0] != 0) {
                memset(dst, 0, OVERRIDE_OFFSET);
                memset(dst + OVERRIDE_END, 0, OBJ_SZ - OVERRIDE_END);
            }
        }
        if (is_spec_apply && total_obj_candidates > 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SaveState: spec object_pool: candidates=%zu restored=%zu "
                "truncated=%zu (per-obj cap=%zu)",
                total_obj_candidates, total_obj_restored,
                total_obj_truncated, MAX_PRESERVED_PER_OBJ);
            if (total_obj_truncated > 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SaveState: spec object_pool pointer-preserve TRUNCATED "
                    "in some slot(s) -- host pointers in the uncaptured tail "
                    "will fault on first deref; bump MAX_PRESERVED_PER_OBJ");
            }
        }
    }

    // Restore input history
    memcpy((void*)ADDR_INPUT_HISTORY, state->input_history, SIZE_INPUT_HISTORY);

    // Restore game state. Same cross-process heap-pointer hazard as
    // char_slot / object_pool above -- game_state may include resource-
    // table pointers (e.g. asset list heads, palette pool, music handle)
    // that the host's process allocated. Single contiguous 544-byte
    // region, so one scan + memcpy + conditional restore.
    if (!is_spec_apply) {
        memcpy((void*)ADDR_GAME_STATE, state->game_state, SIZE_GAME_STATE);
    } else {
        constexpr size_t MAX_PRESERVED_GAME = 256;
        struct GamePtrPreserve { uint16_t didx; uint32_t value; };
        GamePtrPreserve preserved[MAX_PRESERVED_GAME];
        size_t pcount = 0;
        size_t pcandidates = 0;
        const uint32_t* live_r = (const uint32_t*)ADDR_GAME_STATE;
        const size_t dword_count = SIZE_GAME_STATE / sizeof(uint32_t);
        for (size_t o = 0; o < dword_count; ++o) {
            if (live_r[o] >= HEAP_PTR_FLOOR) {
                ++pcandidates;
                if (pcount < MAX_PRESERVED_GAME) {
                    preserved[pcount++] = { (uint16_t)o, live_r[o] };
                }
            }
        }
        memcpy((void*)ADDR_GAME_STATE, state->game_state, SIZE_GAME_STATE);
        uint32_t* live_w = (uint32_t*)ADDR_GAME_STATE;
        size_t restored = 0;
        for (size_t k = 0; k < pcount; ++k) {
            if (live_w[preserved[k].didx] >= HEAP_PTR_FLOOR) {
                live_w[preserved[k].didx] = preserved[k].value;
                ++restored;
            }
        }
        if (pcandidates > 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SaveState: spec game_state: candidates=%zu preserved=%zu "
                "restored=%zu (scanned %zu DWORDs, cap=%zu)",
                pcandidates, pcount, restored, dword_count, MAX_PRESERVED_GAME);
            if (pcandidates > MAX_PRESERVED_GAME) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SaveState: spec game_state pointer-preserve TRUNCATED "
                    "(%zu > cap %zu) -- bump MAX_PRESERVED_GAME",
                    pcandidates, MAX_PRESERVED_GAME);
            }
        }
    }

    // EFFECT_SYS1 restore: SKIP. Render-side state must evolve continuously
    // across both host (with rollback) and replay (no rollback) so they
    // produce identical game_rand call sequences during render. (Reverted
    // from a brief experiment that tried Save+Restore on the theory that
    // re-sim decrements would converge — but that froze host's palette at
    // pre-render-mod state while replay kept evolving, producing exactly
    // the drift the experiment was meant to fix.)

    // EFFECT_SYS2 restore: three carved-out regions:
    //   - 0x00..0x20 (sysvars, 32 B): RESTORE
    //   - 0x20..0x4C (palette-flash-2 struct, 44 B at 0x4456D0): SKIP
    //   - 0x4C..0x50 (g_render_frame_counter, 4 B): SKIP
    //   - 0x50..0x58 (tail, 8 B): RESTORE
    constexpr size_t PFLASH2_OFFSET = 0x4456D0 - EffectAddrs::EFFECT_SYS2;       // 0x20
    constexpr size_t RFC_OFFSET     = ADDR_RENDER_FRAME_COUNTER - EffectAddrs::EFFECT_SYS2;  // 0x4C
    constexpr size_t RFC_END        = RFC_OFFSET + 4;                             // 0x50
    memcpy((void*)EffectAddrs::EFFECT_SYS2,
           state->effect_sys2,
           PFLASH2_OFFSET);
    memcpy((void*)(EffectAddrs::EFFECT_SYS2 + RFC_END),
           state->effect_sys2 + RFC_END,
           EffectAddrs::EFFECT_SYS2_SZ - RFC_END);
    *(uint32_t*)0x424718 = state->round_end_flag;
    // Shake effects: SKIP restore. Save zeros the buffer for fingerprint
    // determinism; restoring would clobber the live decrementing timer that
    // ProcessShakeEffect maintains via render. Leaving live memory alone
    // means [EB] fires once (script PC then advances), the timer then
    // counts down naturally render-by-render, and the shake plays out for
    // its full scripted duration even under GekkoNet's per-frame Save+Load
    // runahead cycle.

    // Wave C additions: afterimage pool + object list topology.
    // Restore afterimage_pool EXCEPT carve-outs (render-side state that
    // must evolve continuously across host's rollback cycles to stay in
    // sync with replay's forward-only sim):
    //   - g_last_frame_time @ +0x4A4: per-process pacing anchor (skip)
    //   - shake_effects (40 B at 0x447DA9): skip (same reason as
    //     state->shake_effects)
    //   - palette-flash-1 (42 B at 0x447D7D): skip (same as state->effect_sys1)
    constexpr size_t G_LAST_FRAME_TIME_OFFSET = 0x447DD4 - WaveCAddrs::AFTERIMAGE_POOL;  // 0x4A4
    constexpr size_t G_LAST_FRAME_TIME_SIZE   = 4;
    constexpr size_t PFLASH1_OFFSET_IN_AI = EffectAddrs::EFFECT_SYS1 - WaveCAddrs::AFTERIMAGE_POOL;
    constexpr size_t PFLASH1_END_IN_AI    = PFLASH1_OFFSET_IN_AI + EffectAddrs::EFFECT_SYS1_SZ;
    constexpr size_t SHAKE_OFFSET_IN_AI = EffectAddrs::SHAKE_EFFECTS - WaveCAddrs::AFTERIMAGE_POOL;
    constexpr size_t SHAKE_END_IN_AI    = SHAKE_OFFSET_IN_AI + EffectAddrs::SHAKE_EFFECTS_SZ;
    static_assert(PFLASH1_END_IN_AI <= SHAKE_OFFSET_IN_AI,
                  "EFFECT_SYS1 must end before shake region");
    static_assert(SHAKE_END_IN_AI <= G_LAST_FRAME_TIME_OFFSET,
                  "shake region must end before g_last_frame_time");
    // CROSS-PROCESS SPEC CARVE-OUT: this region is a raw data-segment slice
    // that CONTAINS the tail of the stage data block (g_special_object_
    // character_data_base @ 0x445740). Its sound-resource bookkeeping lands
    // at 0x44795C..0x447970 (= AI offset 0x2C): GlobalAlloc'd sound-entry
    // array ptr, bank index, loaded flag, entry counts. Copying the host's
    // heap pointer verbatim crashes the spectator at the NEXT CSS entry:
    // ClearStageData -> resource_cleanup_manager walks *(0x44795C) entry by
    // entry (stride 0x2A) freeing each -- AV read of host heap addr at game
    // EXE 0x40356F (observed 2026-06-11 on the A4 multi-match run, match-1
    // -> CSS boundary). Same scan/memcpy/conditional-restore treatment as
    // char_dynamic above. At a BTB join the live pool is near-empty (few
    // false candidates); static .data pointers sit below HEAP_PTR_FLOOR so
    // only genuine heap pointers are preserved. Restores in the carved-out
    // slices are identity writes (live value == preserved value there).
    struct AiPtrPreserve { uint32_t didx; uint32_t value; };
    constexpr size_t MAX_PRESERVED_AI = 16384;
    static AiPtrPreserve s_ai_preserved[MAX_PRESERVED_AI];
    static uint8_t s_ai_live_stage_snd[0x18];
    size_t ai_pcount = 0;
    size_t ai_candidates = 0;
    if (is_spec_apply) {
        std::memcpy(s_ai_live_stage_snd,
                    (const uint8_t*)WaveCAddrs::AFTERIMAGE_POOL +
                        (0x44795Cu - WaveCAddrs::AFTERIMAGE_POOL),
                    sizeof(s_ai_live_stage_snd));
        const uint32_t* live_r = (const uint32_t*)WaveCAddrs::AFTERIMAGE_POOL;
        const size_t dword_count = WaveCAddrs::AFTERIMAGE_POOL_SZ / sizeof(uint32_t);
        for (size_t o = 0; o < dword_count; ++o) {
            if (live_r[o] >= HEAP_PTR_FLOOR) {
                ++ai_candidates;
                if (ai_pcount < MAX_PRESERVED_AI) {
                    s_ai_preserved[ai_pcount++] = { (uint32_t)o, live_r[o] };
                }
            }
        }
    }
    memcpy((void*)WaveCAddrs::AFTERIMAGE_POOL,
           state->afterimage_pool,
           PFLASH1_OFFSET_IN_AI);
    memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + PFLASH1_END_IN_AI),
           state->afterimage_pool + PFLASH1_END_IN_AI,
           SHAKE_OFFSET_IN_AI - PFLASH1_END_IN_AI);
    memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + SHAKE_END_IN_AI),
           state->afterimage_pool + SHAKE_END_IN_AI,
           G_LAST_FRAME_TIME_OFFSET - SHAKE_END_IN_AI);
    memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + G_LAST_FRAME_TIME_OFFSET + G_LAST_FRAME_TIME_SIZE),
           state->afterimage_pool + G_LAST_FRAME_TIME_OFFSET + G_LAST_FRAME_TIME_SIZE,
           WaveCAddrs::AFTERIMAGE_POOL_SZ - G_LAST_FRAME_TIME_OFFSET - G_LAST_FRAME_TIME_SIZE);
    if (is_spec_apply) {
        uint32_t* live_w = (uint32_t*)WaveCAddrs::AFTERIMAGE_POOL;
        size_t restored = 0;
        for (size_t k = 0; k < ai_pcount; ++k) {
            if (live_w[s_ai_preserved[k].didx] >= HEAP_PTR_FLOOR) {
                live_w[s_ai_preserved[k].didx] = s_ai_preserved[k].value;
                ++restored;
            }
        }
        // FORCED carve-out: the stage block's sound bookkeeping (game
        // 0x44795C..0x44797x = AI offset 0x2C..0x44: entry-array ptr, bank
        // idx, loaded flag, counts) must ALWAYS keep the live values. The
        // heap-shape heuristic above fails open when the spec's own stage
        // sounds haven't finished loading at apply time (live ptr still 0
        // -> host pointer flows in -> ClearStageData AV at the NEXT stage
        // confirm; recurred 2026-06-11 on the fast CSS-join path). A live
        // window of zeros is a safe cleanup no-op; a host pointer never is.
        {
            constexpr size_t kStageSndOff = 0x44795Cu - WaveCAddrs::AFTERIMAGE_POOL;
            constexpr size_t kStageSndLen = 0x18;  // ptr, bank, flag, ?, cnt, cnt2
            std::memcpy((uint8_t*)WaveCAddrs::AFTERIMAGE_POOL + kStageSndOff,
                        s_ai_live_stage_snd, kStageSndLen);
        }
        if (ai_candidates > 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SaveState: spec afterimage_pool: candidates=%zu preserved=%zu "
                "restored=%zu (cap=%zu)",
                ai_candidates, ai_pcount, restored, MAX_PRESERVED_AI);
            if (ai_candidates > MAX_PRESERVED_AI) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SaveState: spec afterimage_pool pointer-preserve TRUNCATED "
                    "(%zu > cap %zu) -- bump MAX_PRESERVED_AI",
                    ai_candidates, MAX_PRESERVED_AI);
            }
        }
    }
    *(uint32_t*)WaveCAddrs::CURRENT_OBJECT_PTR    = state->current_object_ptr;
    memcpy((void*)WaveCAddrs::OBJECT_LIST_HEADS,    state->object_list_heads_tails, WaveCAddrs::OBJECT_LIST_HEADS_SZ);
    memcpy((void*)WaveCAddrs::OBJECT_NODE_POOL,     state->object_node_pool,        WaveCAddrs::OBJECT_NODE_POOL_SZ);

    // Mike Z sound-rollback: restore per-channel "desired" state. Actual DSound
    // hardware state is NOT touched — the post-advance sync decides which
    // restored desires cross into "play now" vs. "leave the channel alone"
    // based on whether the play frame falls inside the rollback window.
    //
    // CROSS-PROCESS SPEC CARVE-OUT: DesiredState.script_item_ptr is a raw
    // pointer into the HOST's heap, stored so SyncAfterAdvance can re-invoke
    // the original dispatcher. Restoring it on a spectator snapshot apply
    // crashes on the first sync (AV read of host heap addr inside
    // Hook_DispatchScriptSoundCommand reading script_item+40 -- observed
    // 2026-06-11 on the spec_selftest CURRENT_MATCH join, FM2KHook+0x102B2).
    // Zero desired[] for spec applies instead: in-flight sounds at join
    // are not worth reconstructing (catchup is muted anyway) and the spec's
    // own forward sim re-records fresh entries with LOCAL pointers.
    if (!is_spec_apply) {
        SoundRollback::RestoreDesired(state->sound_desired);
    } else {
        static const SoundRollback::DesiredState s_zero_desired[SoundRollback::MAX_CHANNELS] = {};
        SoundRollback::RestoreDesired(s_zero_desired);
    }

    return true;
}

// =============================================================================
// TESTING / VERIFICATION
// =============================================================================

// Addresses for snapshot capture
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



// Cheap path: gameplay fingerprint only — what GekkoNet actually compares
// for desync detection. Called on every save. ~44 B of hashing.
uint32_t SaveState_CalculateFingerprint() {
    g_region_checksums.rng = *(uint32_t*)ADDR_RNG_SEED;

    uint32_t buf_idx = *(uint32_t*)0x447EE0;
    uint32_t idx     = buf_idx & 0x3FF;
    uint16_t p1_in = ((const uint16_t*)0x4280E0)[idx];
    uint16_t p2_in = ((const uint16_t*)0x4290E0)[idx];
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

    // Game state CRC: split to EXCLUDE known per-process/CSS-residue fields:
    //   0x470050 (4 bytes): g_score_value - CSS frame counter residue, used for battle transition
    //   0x4701CC (4 bytes): g_hInstance - different per process
    // Region 1: 0x470020 to 0x470050 (0x30 bytes)
    // Region 2: 0x470054 to 0x4701CC (0x178 bytes)
    uint32_t gs1 = Fletcher32((uint8_t*)ADDR_GAME_STATE, 0x470050 - ADDR_GAME_STATE);
    uint32_t gs2 = Fletcher32((uint8_t*)0x470054, 0x4701CC - 0x470054);
    g_region_checksums.game_state = gs1 ^ gs2;

    // Object pool: hash the FULL pool, not just the first 4KB.
    // Previous 4 KB covered only ~10 objects; stress-mode desync at frame 273
    // showed Obj[7] CRC changing without the hashed region moving, meaning the
    // 4KB hash was silent to divergence past object 10. Hash the full 391 KB
    // so ANY object mutation shows up in the combined checksum immediately.
    g_region_checksums.object_pool = Fletcher32((uint8_t*)ADDR_OBJECT_POOL, SIZE_OBJECT_POOL);

    // Character dynamic state — only hash loaded slots (first byte != 0
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
    // History ring buffers: 1024 frames × 2 bytes per player. Must match across
    // peers byte-for-byte — if a facing/slot swap mutates the stored input on
    // one side and not the other, these CRCs diverge on frame 1 and give us
    // an early desync signal instead of waiting for the cascade into game_state.
    uint32_t it4 = Fletcher32((uint8_t*)0x4280E0, 0x800);                 // g_p1_input_history (uint16[1024])
    uint32_t it5 = Fletcher32((uint8_t*)0x4290E0, 0x800);                 // g_p2_input_history (uint16[1024])
    g_region_checksums.input_tracking = it1 ^ it2 ^ it3 ^ it4 ^ it5;

    // Effect/shake regions (now saved for rollback)
    g_region_checksums.effect_sys1 = Fletcher32((uint8_t*)EffectAddrs::EFFECT_SYS1, EffectAddrs::EFFECT_SYS1_SZ);
    g_region_checksums.effect_sys2 = Fletcher32((uint8_t*)EffectAddrs::EFFECT_SYS2, EffectAddrs::EFFECT_SYS2_SZ);
    g_region_checksums.shake_effects = Fletcher32((uint8_t*)EffectAddrs::SHAKE_EFFECTS, EffectAddrs::SHAKE_EFFECTS_SZ);

    // Wave C regions — PREVIOUSLY SAVED BUT NOT HASHED.
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
        // Wave C regions — now properly included
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
    // Object pool windows — hex dumps for small ones (<=256B), CRCs for large.
    // Combined CRC now covers the FULL 391 KB pool; these windows localize any
    // divergence to specific object-index ranges in the diagnostic log.
    { 0x4701E0, 0x100, "ObjectPool_First256",          false },  // Obj[0] first 256 B
    { 0x4702E0, 0x100, "ObjectPool_Second256",         false },  // Obj[0] tail + Obj[1] first 130 B
    { 0x470C52, 0x100, "ObjectPool_Obj7_first256",     false },  // Obj[7] — was changing invisibly pre-fix
    { 0x4703E0, 0x800, "ObjectPool_Obj_2_10_CRC",      false },  // Obj[2..10] aggregate CRC (no hex)
    { 0x471000, 0x800, "ObjectPool_Obj_11_20_CRC",     false },  // Obj[11..20] aggregate CRC
    { 0x4720C2, 0x2000,"ObjectPool_Obj_21_42_CRC",     false },  // Obj[21..42] aggregate CRC (approx)
    { 0x447D7D,  0x2A, "EffectSystem1",                false },  // saved + hashed
    { 0x4456D0,  0x2C, "EffectSystem2",                false },  // saved + hashed
    { 0x447DA9,  0x28, "ShakeEffects",                 false },  // saved + hashed
    // Wave C regions — now saved AND hashed in combined CRC. Showing their
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
        // Stress-mode desyncs are single-instance determinism failures —
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
    // Legacy demo-mode fields — retain for backward diff compatibility. These
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
    // was NOT deterministically reproduced from the saved state — that IS
    // the bug source. Same logical moment on both sides — no timing artifacts.
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
            fprintf(f, "  (no forward snapshot for frame %d — rollback buffer stale)\n", saved_frame);
        } else if (!have_replay) {
            fprintf(f, "  (no replay snapshot for frame %d — desync may have fired before\n"
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
                "        Both captured at save time — no timing artifacts from later mutation.\n"
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
                    "        addresses (0x4DFC85 / 0x4EDCC4) — upper 16 bits include\n"
                    "        adjacent CharDynamic bytes. A DIFF in only those upper\n"
                    "        bits points at a neighboring field, not HP itself.\n"
                    "        p?_input is read from input_history[buf_idx & 0x3FF].\n");
            }

            // Per-object-slot breakdown. If ObjectPool(full) differed, this
            // pinpoints the exact slot indices. The per-slot CRCs aren't
            // computed at save time any more (too expensive — see
            // SaveState_Save) so we derive them here from the FORWARD save
            // buffer (state->object_pool, which is still live in the rollback
            // ring). Replay bytes aren't retained, so we compute replay-side
            // per-slot CRCs from live memory — valid as long as we're dumping
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
                    fprintf(f, "  (aggregate ObjectPool CRC differed but no slot matched on-demand —\n"
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
    // range — diff the two files side-by-side and the first differing
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
    // characters in the SAME logical slot — CSS character assignment is
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

        // First 32 bytes of the static region — character ID / name lives at
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

#endif  // !ENGINE_FM95 (FM2K full save/load body)

// =============================================================================
// SNAPSHOT SERIALIZATION (task #18 phase 2) — engine-agnostic
// =============================================================================
//
// Build-agnostic helpers; both FM2K and FM95 builds compile these. The slot
// layout differs across builds (sizeof(SaveStateData) ≈ 1 MB on FM2K vs
// ≈ 45 KB on FM95), but the API contract is the same: caller asks for the
// size, then peeks the bytes of the most recently Saved slot.
//
// Spectator-join wire format will round-trip these bytes through
// SaveState_LoadFromBytes (added in phase 4) without external interpretation
// — they're an opaque blob from the spectator's perspective. Both peers
// must run the same engine variant for SaveState_Load to succeed; mismatches
// are caught by checksum verification before Load is attempted.

size_t SaveState_GetSlotByteSize() {
    return sizeof(SaveStateData);
}

const uint8_t* SaveState_PeekLastSavedSlotBytes() {
    if (g_last_saved_slot < 0) return nullptr;
    if (g_last_saved_slot >= MAX_ROLLBACK_FRAMES) return nullptr;
    return reinterpret_cast<const uint8_t*>(&g_state_buffer[g_last_saved_slot]);
}

bool SaveState_LoadFromBytes(const uint8_t* bytes, size_t n) {
    if (!bytes || n != sizeof(SaveStateData)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SaveState_LoadFromBytes: size mismatch (got %zu, want %zu)",
            n, sizeof(SaveStateData));
        return false;
    }

    // frame_number is the first field of SaveStateData (see savestate.h:44),
    // so we can read it straight off the wire bytes without round-tripping
    // through a ~1MB stack-local SaveStateData. The previous version blew
    // the 1MB default Windows main-thread stack on the spec /F-boot path
    // for games with extra DLL init pressure (pkmncc crashed; wanwan was
    // just within margin). Copy directly into the destination slot.
    const int frame  = (int)*(const uint32_t*)bytes;
    const int slot   = ((frame % MAX_ROLLBACK_FRAMES) + MAX_ROLLBACK_FRAMES)
                       % MAX_ROLLBACK_FRAMES;
    std::memcpy(&g_state_buffer[slot], bytes, sizeof(SaveStateData));
    g_last_saved_slot = slot;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SaveState_LoadFromBytes: applying %zu-byte snapshot "
        "(frame=%d → slot=%d)", n, frame, slot);

    return SaveState_Load(frame);
}
