// savestate_fm2k_save.cpp -- FM2K SaveState_Save (+ GetLastChecksum) + the per-region save sub-profiler (FM2K-save-only). Split (engine x concern) from savestate.cpp.
#if !defined(ENGINE_FM95)
#include "savestate.h"
#include "savestate_internal.h"
#include "globals.h"
#include "seam_trace.h"   // Phase 1bc per-save fingerprint ring (diagnostic)
#include <SDL3/SDL_log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <windows.h>

// --- Per-region save sub-profiler (FM2K_PERF_PROFILE=1) ------------------
// Attributes per-save cost to specific memcpy blocks. Same env gate as the
// netplay-side [PERF] line. Reports avg microsec/region every 500 saves.
// MEASURED 2026-07-06 (1v1, ~active slots): ~105us/save total --
// afterimage=37 char=27 obj=22 sound=10 (+ throttled fullcrc ~1/sec 180us).
// The active-slot char/obj optimizations already cut this ~15x from the
// original full-copy cost; per-frame save is ~1% of the 10ms budget, not a
// bottleneck. Remaining lever is the ~1MB *transmission* blob (reserved
// struct), not per-frame work.
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

bool SaveState_Save(int frame) {
    const uint64_t _ss_t_entry = g_ss_perf_on ? SsNowNs() : 0;
    // Handle frame -1 as frame 0 (initial state before first frame)
    // GekkoNet sends this during initialization
    if (frame < 0) {
        frame = 0;
    }

    // Initial-sync step: SaveState_DoInitialSync() now fires eagerly from
    // Netplay_Start*Battle BEFORE the first AdvEvent -- see the comment
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
    // slot -- both forward and replay back-patch the same address.
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
    // char_dynamic with live (replay) bytes -- otherwise fwd and cur become
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
    // just to produce diagnostics -- dominant CPU cost in stress mode.
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
        // diff log on actual divergence. Per-region only here -- full
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
        // (rng/HP/round+game timer/current-input -- the SAME hash gekko uses as
        // the save checksum; NO positions, despite what this comment used to
        // say) from live (replay) memory and compare to the forward
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

        // ObjectPool byte scan -- pinpoints first divergent object + field.
        if (fwd_crcs.object_pool != cur_obj_crc) {
            const uint8_t* fwd = state->object_pool;
            const uint8_t* cur = (const uint8_t*)ADDR_OBJECT_POOL;
            for (size_t i = 0; i < SIZE_OBJECT_POOL; i++) {
                if (fwd[i] != cur[i]) {
                    int obj_idx = (int)(i / OBJECT_POOL_STRIDE);
                    int field_off = (int)(i % OBJECT_POOL_STRIDE);
                    uintptr_t obj_base = ADDR_OBJECT_POOL + obj_idx * OBJECT_POOL_STRIDE;
                    // SDL console log disabled -- file output (REPLAY_DIFF_LOG)
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
        // NOT raw live memory -- otherwise every entry reports a phantom
        // diff at +0x4A4 from our deliberate save-time exclusion.
        if (fwd_crcs.afterimage_pool != cur_ai_crc) {
            const uint8_t* fwd = state->afterimage_pool;
            const uint8_t* cur = s_ai_cmp_buf;
            for (size_t i = 0; i < WaveCAddrs::AFTERIMAGE_POOL_SZ; i++) {
                if (fwd[i] != cur[i]) {
                    // SDL console log disabled -- file output (REPLAY_DIFF_LOG)
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
                        // SDL console log disabled -- file output (REPLAY_DIFF_LOG)
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

        if (df) fclose(df);  // explicit flush -- survives TerminateProcess
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
    // -- render_frame_counter is incremented at the end of every render_game
    // call and is the basis for ProcessShakeEffect's per-frame parity-flip
    // (negate offset on odd renders). If we save+restore the live counter,
    // GekkoNet's per-frame Save+Load cycle freezes it at whatever value Save
    // captured, the parity stops alternating, and screen shake only shifts
    // one direction -- making the stage look permanently offset. See the
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
    // inactive slots entirely -- only their first byte is written (=0), which
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
    // every frame and may call game_rand based on its state -- and the
    // timer in update_game_state decrements each frame. For host+replay
    // determinism, BOTH engines need to see the SAME palette state at
    // each render. That happens iff palette evolves continuously through
    // forward sims on both sides (1 decrement per main-loop tick). If we
    // save/restore, host's rollback Load resets palette to pre-render-mod
    // state while replay keeps accumulating -- divergent palette → divergent
    // render rng calls → drift. The "skip restore" pattern is the only one
    // that keeps host's many rollbacks aligned with replay's straight
    // forward-only sim. Same logic for shake_effects + pflash2 below.
    std::memset(state->effect_sys1, 0, EffectAddrs::EFFECT_SYS1_SZ);
    // EFFECT_SYS2 carve-outs (REVERTED back to original -- same reasoning
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
    // through forward render passes on BOTH sides -- saving/restoring on
    // rollback would reset host's shake state to a pre-render-mod value
    // each Load, while replay (no rollback) keeps the natural evolution.
    // Divergent shake → divergent render rng → drift.
    memset(state->shake_effects, 0, EffectAddrs::SHAKE_EFFECTS_SZ);

    // Wave C additions: afterimage pool + object list topology.
    //
    // IMPORTANT: the "afterimage pool" range 0x447930..0x46F6C0 is a big slice
    // of the data segment that happens to CONTAIN the afterimage buffers but
    // also contains unrelated globals. The only one we've proven harmful for
    // determinism is `g_last_frame_time @ 0x447DD4` (offset 0x4A4) -- written
    // by main_game_loop's pacing arithmetic (0x405AE9/AFD/BA8/BBB) and by
    // main_window_proc on alt-tab/F4/Alt+Enter (0x40610F/0x4062A5) and by
    // the menu handler (0x4177BF). None of those writers are sim code --
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
    // INSIDE the afterimage_pool slice. Zero them in the saved copy too --
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
    // DSound hardware state -- it's the sim's record of what each channel
    // should be playing. Actual DSound plays are driven post-advance by the
    // sync step; hardware state is deliberately not rolled back.
    { SScope _s(&g_ss_sound); SoundRollback::CaptureDesired(state->sound_desired); }
    // BGM (MIDI/CD/stop) desired -- single global stream, same rationale.
    SoundRollback::CaptureBgm(&state->bgm_desired);

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
    // determinism diagnostics -- without this, the desync dump's
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
    // The EXPENSIVE Fletcher32 calls -- afterimage_pool (~159 KB), list_heads
    // (1 KB), node_pool (8 KB) -- are gated on full_crcs_due so they only fire
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
            // above), NOT live memory -- this is what makes the exclusion work
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

    // Phase 1bc seam ring (DIAGNOSTIC, FM2K_SEAM_TRACE=1, memory only).
    // Deliberately records EVERY save, forward and resim, in append order --
    // the whole question is whether a resim re-save reproduces its forward
    // save, which a dedupe-by-frame ring would erase. Written to a file only
    // from HandleDesyncDetected.
    SeamTrace_PushSave(frame, is_replay_save, g_is_rolling_back);

    // Per-save logging.
    //   First 10 frames: verbose (combined + fingerprint + rng + buf_idx).
    //   After that: push into the in-memory rngtrace ring. Writing a console
    //   line every save at 100 fps tanks framerate and is user-visible.
    //   The ring is flushed to FM2K_P<N>_rngtrace.csv when SaveState_FlushRngTrace
    //   is called (on desync detection or session end). Non-rollback saves
    //   only -- replay ticks are not part of the authoritative per-frame trace.
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
#endif  // engine guard

