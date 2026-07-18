// savestate_fm2k_load.cpp -- FM2K SaveState_Load (cross-process heap-pointer carve-outs). Split (engine x concern) from savestate.cpp.
#if !defined(ENGINE_FM95)
#include "savestate.h"
#include "savestate_internal.h"
#include "globals.h"
#include "../hooks/hooks.h"   // RoundEvents_LiveSubstate / Fm2k_ClearAfterimageIndices (task #53)
#include <SDL3/SDL_log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <windows.h>


bool SaveState_Load(int frame) {
    // task #53: capture the LIVE round substate BEFORE the restore
    // overwrites it. If the live sim already entered the end/transition
    // sequence (>= 900 -- the engine frees .player heap and starts CSS
    // loads there), a restore of a pre-end state resurrects afterimage
    // pool indices whose backing heap is GONE; the next
    // sprite_rendering_engine pass AVs at 0x40cd47 (deterministic under
    // the CGNAT-rebind rig: the barrier lags, the game transitions while
    // the gekko session still rolls back). Cleared post-restore below;
    // trails are invisible during results/fade so there is no visual cost.
    const int live_substate_pre = RoundEvents_LiveSubstate();

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
        SoundRollback::RestoreBgm(&state->bgm_desired);
    } else {
        static const SoundRollback::DesiredState s_zero_desired[SoundRollback::MAX_CHANNELS] = {};
        SoundRollback::RestoreDesired(s_zero_desired);
        // Same carve-out: bgm_desired.script_item_ptr is a host-heap pointer;
        // zero it on a spectator apply so the sync step doesn't deref host mem.
        static const SoundRollback::DesiredBgm s_zero_bgm = {};
        SoundRollback::RestoreBgm(&s_zero_bgm);
    }

    // task #53 afterimage guard -- see the live_substate_pre capture at entry.
    if (live_substate_pre >= 900) {
        Fm2k_ClearAfterimageIndices();
    }

    return true;
}

// =============================================================================
// TESTING / VERIFICATION
// =============================================================================

// Addresses for snapshot capture
#endif  // engine guard

