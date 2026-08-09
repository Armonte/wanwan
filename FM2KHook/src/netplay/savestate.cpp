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
// extension, game state block) -- they have no FM95 equivalent yet, and
// savestate is a no-op on FM95 (rollback isn't activated; trampoline
// refactor in Task 7 will design FM95's save layout).
#include "savestate_internal.h"  // shared rollback buffers + region constants + hashing


// Rollback buffer
SaveStateData g_state_buffer[MAX_ROLLBACK_FRAMES];

// Tracks the slot most recently filled by SaveState_Save. Used by
// SaveState_PatchPostRenderRng to back-patch the rng_seed field after the
// trampoline's render call returns, so the saved slot reflects post-render
// rng (matching what forward sim sees as the starting rng for the next
// frame). -1 = no save has happened yet (e.g. CSS).
int g_last_saved_slot  = -1;
int g_last_saved_frame = -1;

// Parallel "post-render RNG" buffer -- separate from g_state_buffer because
// SaveState_Save overwrites rng_seed on REPLAY saves, wiping the value
// PatchPostRenderRng wrote during the original forward pass. During a
// rollback batch, render fires only ONCE (after the last AdvanceEvent),
// so intermediate frames in the batch never get their POST-render RNG
// applied to the engine -- replay sim_K+2 starts from POST-sim-K+1 instead
// of POST-render-K+1, accumulating render's RNG delta as divergence.
//
// Phase F (#23): the v0.2.43 user-reported desyncs all show "RNG_Seed
// forward != replay" or "GameplayFingerprint DIFF" caused exactly by
// this. Fix is to keep a per-frame snapshot here that replay does NOT
// touch, then apply it at the start of each replay AdvanceEvent so sim
// starts from the same RNG the forward pass's sim started from.
//
// Frame number stored alongside so a wrap-around (rollback >MAX_ROLLBACK_
// FRAMES, which we don't expect to actually happen -- typical rewinds are
// <10 -- but be defensive) doesn't restore a stale value from a previous
// session.
uint32_t g_post_render_rng[MAX_ROLLBACK_FRAMES] = {};
int32_t  g_post_render_rng_frame[MAX_ROLLBACK_FRAMES] = {};

// Initial sync flag - forces deterministic state on first save (like BBBR's m_initialSyncDone)
bool g_initial_sync_done = false;

// Per-region checksums for desync investigation (forward-declared here so
// SaveState_Save can snapshot it into per-frame saved_region_crcs; full
// definition + accessors live further down next to the calculator).
RegionChecksums g_region_checksums = {};

// Replay-save shadow buffer (stress-mode forward-vs-replay diff).
// SaveState_Save runs once per save-event. In stress mode (and during real
// online rollback), GekkoNet may issue a SaveEvent for the SAME frame twice:
// first when the forward sim reaches it, second after a Load+Advance replays
// it. We use the FIRST save's per-region CRCs as "forward" (kept in
// g_state_buffer[slot].saved_region_crcs), and the SECOND save's as
// "replay" (stored here). Diffing these in the desync dump pinpoints which
// region replayed nondeterministically.
ReplaySaveSnapshot g_replay_saves[64 /* MAX_ROLLBACK_FRAMES */] = {};

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

void SaveState_ResetRngTrace() {
    g_rngtrace_head  = 0;
    g_rngtrace_count = 0;
}

// FM2K_RNGTRACE: write the CSV on a routine flush. DEFAULT OFF -- this is a
// desync-forensics dump, not telemetry, and it is not free. The ring holds up
// to RNGTRACE_CAPACITY entries and the dump is ONE fprintf per entry into a
// stdio stream, ~1.4 MB, on the GAME THREAD at the match-end seam. On an SSD
// that is tens of ms; on a spinning disk, an encrypted volume, or with a
// real-time AV scanner inspecting a freshly created .csv, it is seconds -- and
// it used to be paid at the end of EVERY match, on a ring that was never
// cleared between them, so match five of a set dumped ten minutes of history.
// The forced call sites (desync handler, harness auto-terminate) ignore this
// gate: both are already tearing the process down, so the cost is not a hitch
// in anyone's match.
static bool RngTraceDumpEnabled() {
    static int s_cached = -1;
    if (s_cached < 0) {
        const char* v = std::getenv("FM2K_RNGTRACE");
        s_cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return s_cached == 1;
}

void SaveState_FlushRngTrace(int player_index, const char* reason, bool force) {
    const size_t count = g_rngtrace_count;
    // Always-on summary: counts, never contents. One line per flush (i.e. per
    // match boundary), so the ring's behaviour stays visible in a default
    // build without anyone paying for the dump.
    if (!force && !RngTraceDumpEnabled()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "rngtrace: %zu entries buffered, dump SKIPPED (%s) -- "
                    "set FM2K_RNGTRACE=1 to write FM2K_P%d_rngtrace.csv",
                    count, reason ? reason : "", player_index + 1);
        SaveState_ResetRngTrace();
        return;
    }

    char filename[256];
    snprintf(filename, sizeof(filename), "FM2K_P%d_rngtrace.csv", player_index + 1);
    FILE* f = fopen(filename, "w");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "rngtrace: failed to open %s", filename);
        SaveState_ResetRngTrace();
        return;
    }
    fprintf(f, "# rngtrace flush reason=%s count=%zu\n", reason ? reason : "",
            count);
    fprintf(f, "frame,rng,fingerprint\n");
    // Walk from oldest to newest
    size_t start = (count < RNGTRACE_CAPACITY)
                   ? 0
                   : g_rngtrace_head;  // head points at the oldest when full
    for (size_t i = 0; i < count; i++) {
        const auto& e = g_rngtrace_ring[(start + i) % RNGTRACE_CAPACITY];
        fprintf(f, "%d,0x%08X,0x%08X\n", e.frame, e.rng, e.fingerprint);
    }
    fclose(f);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "rngtrace: wrote %zu entries to %s (%s)",
                count, filename, reason ? reason : "");
    // Every flush is a match boundary, so the ring starts the next match
    // empty. Without this the dump grew monotonically across a set -- the
    // reason the tail got worse the longer people played.
    SaveState_ResetRngTrace();
}

// Hashing: use xxHash3-64 (truncated to u32) instead of pure-C Fletcher32.
// Fletcher32 topped out ~500 MB/s on a 32-bit build; in stress mode at
// ~1 MB hashed per save × 10× rollback amplification × 100 Hz we were
// hitting 1 GB/s and saturating one core. XXH3 runs 15-30 GB/s on the
// same hardware and has strictly better collision resistance. Kept the
// Fletcher32 symbol as a compatibility wrapper so call sites don't need
// to change -- they still see `uint32_t Fletcher32(ptr, len)`.
#define XXH_INLINE_ALL
#define XXH_NO_STREAM
#include "../../../vendored/xxhash/xxhash.h"

uint32_t Fletcher32(const uint8_t* data, size_t len) {
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

const char* ReplayDiffLogPath() {
    static char s_path[MAX_PATH] = {0};
    if (s_path[0] == 0) {
        if (!Fm2k_BuildLogPath(s_path, sizeof(s_path), REPLAY_DIFF_LOG_NAME)) {
            std::snprintf(s_path, sizeof(s_path), "%s", REPLAY_DIFF_LOG_NAME);
        }
    }
    return s_path;
}

void SaveState_Init() {
    memset(g_state_buffer, 0, sizeof(g_state_buffer));
    // Reset replay-shadow buffer so stale forward-vs-replay diffs from a
    // previous battle don't show up on the first desync of a new session.
    for (int i = 0; i < MAX_ROLLBACK_FRAMES; i++) {
        g_replay_saves[i].frame_number = -1;
        g_replay_saves[i].valid = false;
    }
    // Reset the post-render-RNG parallel buffer -- stale values from a
    // previous session would cause the Phase F fix to apply the wrong
    // RNG at the start of AdvanceEvents in the new session.
    for (int i = 0; i < MAX_ROLLBACK_FRAMES; i++) {
        g_post_render_rng[i]       = 0;
        g_post_render_rng_frame[i] = -1;
    }
    // Truncate the consolidated replay-diff log so each battle session starts
    // with a fresh file.
    if (FILE* f = fopen(ReplayDiffLogPath(), "w")) {
        fprintf(f, "# replay-diff log -- appends one block per replay save that\n"
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
// -- used to be lazy-fired on the first SaveEvent which is post-PGI, and
// that off-by-one timing caused cumulative drift over rollback cycles
// (replay_selftest harness's frame-91 RNG divergence). Now: eager call
// at battle init from Netplay_Start*Battle.
void SaveState_DoInitialSync() {
    if (g_initial_sync_done) return;
    if constexpr (FM2K::kIsFM2K) {
        // FM2K edge-detection working set + input history. The bare literals
        // (0x447F00/40/60) and the savestate_internal.h ADDR_* constants are
        // ALL FM2K addresses -- on FM95 they stomp unrelated memory, which is
        // why this branch is engine-gated (the FM95 equivalents live in
        // savestate_fm95.cpp's fm95save:: namespace).
        *(uint32_t*)ADDR_INPUT_BUFFER_INDEX = 0;         // Reset buf_idx
        *(uint32_t*)ADDR_RENDER_FRAME_COUNTER = 0;       // Reset render frame counter
        memset((void*)0x447F00, 0, 0x20);   // g_prev_input_state
        memset((void*)0x447F40, 0, 0x20);   // g_processed_input
        memset((void*)0x447F60, 0, 0x20);   // g_input_changes
        memset((void*)ADDR_INPUT_HISTORY, 0, SIZE_INPUT_HISTORY);
    } else {
        // FM95: reset buf_idx 0x437700 + the 3 input rings + edge state +
        // current inputs (fm95save:: addresses). BLOCKER fix -- before this
        // split, the FM2K body above ran on FM95 and corrupted memory at
        // every stress/battle start.
        SaveState_Fm95ResetInputSync();
    }
    // REVERTED 2026-05-17: shake/palette zero on battle entry was
    // helpful for replay-self-test parity (host vs replay both start
    // at zero), but user reports cross-peer desync in real netplay
    // after this change. Possibly the zero clobbers state real
    // netplay needs. Reverting to bisect.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SaveState: Initial sync (eager, pre-first-AdvEvent) -- "
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

// =============================================================================
// SNAPSHOT SERIALIZATION (task #18 phase 2) -- engine-agnostic
// =============================================================================
//
// Build-agnostic helpers; both FM2K and FM95 builds compile these. The slot
// layout differs across builds (sizeof(SaveStateData) ≈ 1 MB on FM2K vs
// ≈ 45 KB on FM95), but the API contract is the same: caller asks for the
// size, then peeks the bytes of the most recently Saved slot.
//
// Spectator-join wire format will round-trip these bytes through
// SaveState_LoadFromBytes (added in phase 4) without external interpretation
// -- they're an opaque blob from the spectator's perspective. Both peers
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

