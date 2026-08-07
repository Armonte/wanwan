// savestate_fm2k_load.cpp -- FM2K SaveState_Load (cross-process heap-pointer carve-outs). Split (engine x concern) from savestate.cpp.
#if !defined(ENGINE_FM95)
#include "savestate.h"
#include "savestate_internal.h"
#include "globals.h"
#include "../hooks/hooks.h"   // IsBattleMode + RoundEvents_LiveSubstate / Fm2k_ClearAfterimageIndices (task #53) / Fm2k_{Neutralize,Check}... (match-end seam)
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

    // ...except that guard is INERT at the seam it was written for.
    // RoundEvents_LiveSubstate() reads +0x152 off whatever object is currently
    // g_object_data_ptr. That is the round object only when called from inside
    // Hook_vs_round_function; called from SaveState_Load (outside the object
    // loop) it reads the LAST-walked object, and once the match-complete branch
    // has spawned the type-10 CSS driver that is game_state_manager's own state
    // field, whose values are 0/1/4. So `>= 900` is false for exactly the
    // restores that cross the teardown.
    //
    // Live game_mode is the reliable seam test: the engine leaves [3000,4000)
    // at game_state_manager STATE 0 (0x407247) and only afterwards calls
    // player_data_file_loader -> ClearCharacterSlot, so "mode has left battle"
    // strictly precedes the GlobalFree of the character blobs. Captured BEFORE
    // the restore because g_game_mode (0x470054) sits inside the restored
    // ADDR_GAME_STATE region and the memcpy below rewinds it (that rewind is
    // directly observable in a peer's log as [AUTOPLAY] mode= 2000 -> 3000 ->
    // 2000 across a rollback burst).
    const uint32_t live_game_mode_pre = *(uint32_t*)FM2K::ADDR_GAME_MODE;

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

    // The battle->CSS teardown seam, as a property of THIS apply: a BATTLE
    // snapshot is being written over a process that has already left battle.
    // Both halves matter --
    //   * the live half alone is far too broad. SaveState_Load is also reached
    //     from the SPECTATOR paths (the spectate-session load handler in
    //     netplay_battle_phase.cpp and SaveState_LoadFromBytes for a snapshot
    //     join), and a spectator that joins during CSS sits at game_mode 2000
    //     for its whole CSS phase. Neutralizing script objects there would
    //     freeze the CSS's own type-4 objects. (Player CSS rollback is a
    //     different code path entirely -- CssState_Load -- so it never gets
    //     here.)
    //   * the snapshot half alone says nothing: mid-battle rollbacks restore
    //     battle snapshots constantly and are perfectly safe.
    // g_game_mode (0x470054) lives inside the saved ADDR_GAME_STATE region
    // (0x470020, 0x220 B), so the snapshot's own mode is available directly.
    const uint32_t snap_game_mode =
        *(const uint32_t*)(state->game_state + (FM2K::ADDR_GAME_MODE - ADDR_GAME_STATE));
    const bool crossing_teardown =
        IsBattleMode(snap_game_mode) && !IsBattleMode(live_game_mode_pre);

    // RNG seed: RESTORE. SaveState_Save zeros the seed in the slot, then
    // SaveState_PatchPostRenderRng (called from RenderFrameWithSnapshot
    // after render returns) writes the post-render rng into the slot. So
    // the value we restore here is post-render rng of whatever frame the
    // saved slot represents -- exactly what forward sim sees as the
    // starting rng for the NEXT frame, which is also what we want replay
    // sim to start with for rollback determinism.
    *(uint32_t*)ADDR_RNG_SEED = state->rng_seed;

    // Render frame counter: SKIP restore. Live counter is the only reliable
    // source -- restoring would freeze it at the saved value and break shake
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
    // those pointers with host-process heap addresses -- they're wild here
    // and the engine AVs the moment something dereferences them (observed
    // in ui_state_manager on pkmncc spec join: `mov ebx, g_charslot0_-
    // action_table` then `mov cx, [edx+ebx+0x20]` AV'd reading host's
    // ~0x0B9B6708).
    //
    // The fix: BEFORE the memcpy, scan the live slot for DWORDs that
    // LOOK like heap pointers (>= 0x01000000, i.e. above the static-mem
    // ceiling on 32-bit Win). Capture them. After the memcpy, for each
    // captured offset, if the host's value there ALSO looks heap-shaped
    // (confirming the field is a pointer in BOTH processes -- not a
    // coincidentally-high non-pointer value), restore our local pointer.
    // Non-pointer dynamic state (HP, super meter, anim frame indices,
    // etc -- all small values) flows through normally from snapshot.
    //
    // The heap-SHAPE SCAN above is only enabled for spec mode
    // (g_player_index==2); single-process rollback for players keeps the
    // bit-for-bit memcpy that determinism depends on (same heap, valid
    // pointers). NOTE: the narrow FORCED resource carve-out below is a
    // different thing and now runs for ALL applies -- see its own comment.
    extern int g_player_index;
    const bool is_spec_apply = (g_player_index == 2);
    constexpr uint32_t HEAP_PTR_FLOOR = 0x01000000u;

    // RESOURCE-OWNERSHIP CARVE-OUT WINDOWS (buffer-relative; the save buffer
    // starts at CHAR_SLOT_BASE = engine slot base + 16, so buffer 0x100 ==
    // engine slot +0x110 and buffer 0x220C == engine slot +0x221C -- the shift
    // is uniform across slots so these relative offsets hold for all 8):
    //   0x100 .. 0x10B (12 B)  = action_table (+0x110), commands_ptr (+0x114),
    //                            pictures_ptr (+0x118)
    //   0x220C .. 0x2223 (24 B) = sounds_ptr (+0x221C), sound_channel_id,
    //                            file_loaded_flag, action_count, picture_count,
    //                            sound_count
    // Verified against the CharacterData layout in the IDB and against
    // resource_cleanup_manager @ 0x403520, which reads EXACTLY these fields
    // (and gates its whole body on file_loaded_flag) when it GlobalFree's the
    // slot.
    constexpr size_t RES_A_OFF = 0x100,  RES_A_LEN = 0x0C;
    constexpr size_t RES_B_OFF = 0x220C, RES_B_LEN = 0x18;

    for (size_t i = 0; i < NUM_CHAR_SLOTS; i++) {
        uintptr_t slot_base = CHAR_SLOT_BASE + (i * CHAR_SLOT_SIZE);
        uintptr_t dynamic_addr = slot_base + CHAR_SLOT_DYNAMIC_OFFSET;
        // Mirror of the active-slot save: only restore loaded slots (save
        // buffer's first byte != 0). Unloaded slots are left alone; if the
        // game hasn't loaded a character there nothing reads those bytes.
        if (state->char_dynamic[i][0] == 0) continue;

        // Preserve the LIVE process's resource bookkeeping across the restore
        // -- for EVERY apply, not just the cross-process spectator one.
        //
        // These are load-time OWNERSHIP state, never gameplay state:
        // character_data_loader @ 0x403600 GlobalAlloc's action_table (39 B per
        // action), commands_ptr (16 B per script item, up to 0x10000 items) and
        // pictures_ptr (20 B per picture) plus the per-sound blocks reachable
        // through sounds_ptr, and player_data_file_loader @ 0x4039F0 ->
        // ClearCharacterSlot @ 0x4039B0 GlobalFree's them and then
        // memory_clear's the slot. That reload runs at EVERY character load
        // site: the CSS entry after a match AND vs_round_function's own
        // substate-0/1/100 bodies (6 call sites at 0x4087B2..0x408A29), so the
        // pointers change under us at every round transition, not only at the
        // match end.
        //
        // Rolling those pointers back is wrong in two directions:
        //   1. it re-installs a pointer the engine already GlobalFree'd, and
        //      the next character_state_machine tick reads an opcode through
        //      it -- the 0x4125FC AV at the match-end seam; and
        //   2. the NEXT ClearCharacterSlot then GlobalFree's the resurrected
        //      pointer a second time (latent double-free at the CSS entry).
        // Keeping the live values makes a restored cursor index the CURRENTLY
        // loaded blob instead of a dead one. At a round transition the reload
        // is of the SAME .player file, so content and size are identical and
        // the restored cursor stays exactly as valid as it was -- the pointer
        // value is the only thing that moved.
        //
        // Determinism: identity write in the steady state. Outside a
        // free/realloc seam the live values ARE the snapshot values (nothing
        // but the loader writes them), so this is byte-for-byte the same
        // restore as before. The values are process-local heap addresses, are
        // not in GekkoNet's desync fingerprint (rng/HP/timer/input scalars
        // only, SaveState_CalculateFingerprint), and the sim never branches on
        // them -- it only dereferences them.
        uint8_t live_res_a[RES_A_LEN], live_res_b[RES_B_LEN];
        std::memcpy(live_res_a, (const uint8_t*)dynamic_addr + RES_A_OFF, RES_A_LEN);
        std::memcpy(live_res_b, (const uint8_t*)dynamic_addr + RES_B_OFF, RES_B_LEN);

        if (!is_spec_apply) {
            memcpy((void*)dynamic_addr, state->char_dynamic[i], CHAR_SLOT_DYNAMIC_SIZE);
            std::memcpy((uint8_t*)dynamic_addr + RES_A_OFF, live_res_a, RES_A_LEN);
            std::memcpy((uint8_t*)dynamic_addr + RES_B_OFF, live_res_b, RES_B_LEN);
            continue;
        }

        // Spec path: capture pointer-shaped DWORDs in live slot first.
        // CHAR_SLOT_DYNAMIC_SIZE is currently the full slot (14352
        // DWORDs); the first cut at MAX_PRESERVED=1024 hit the cap on
        // both active slots, so we sized up generously. Lives on stack
        // (~64 KB × 2 fields = 128 KB) -- well under the per-thread
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

        memcpy((void*)dynamic_addr, state->char_dynamic[i], CHAR_SLOT_DYNAMIC_SIZE);

        // FORCED carve-out windows, captured above the is_spec_apply split.
        // The spec path needs them because the heap-shape heuristic fails open
        // when the live slot hasn't finished loading (a live 0 lets the host's
        // pointer through and ClearStageData/ClearCharacterSlot then frees a
        // host-heap address); the player path needs them because the engine
        // reallocates these blocks under a rollback window. Same write, one
        // reason each.
        std::memcpy((uint8_t*)dynamic_addr + RES_A_OFF, live_res_a, RES_A_LEN);
        std::memcpy((uint8_t*)dynamic_addr + RES_B_OFF, live_res_b, RES_B_LEN);

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
            // Diagnostic line -- fires once per snapshot apply per active
            // slot. `candidates` is the total count of heap-shaped
            // DWORDs found in the live slot; `preserved` is how many fit
            // in the buffer (cap = MAX_PRESERVED); `restored` is the
            // subset whose post-memcpy host value was ALSO heap-shaped
            // (confirming a real pointer field). If candidates > cap we
            // need to bump MAX_PRESERVED -- silently truncating would
            // leave some host pointers in place and re-crash.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SaveState: spec char_slot[%zu] candidates=%zu preserved=%zu "
                "restored=%zu (scanned %zu DWORDs, cap=%zu)",
                i, pointer_candidates, preserved_count, restored,
                dword_count, MAX_PRESERVED);
            if (pointer_candidates > MAX_PRESERVED) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SaveState: spec char_slot[%zu] pointer-preserve TRUNCATED "
                    "(%zu > cap %zu) -- host pointers in the uncaptured tail "
                    "will fault on first deref; bump MAX_PRESERVED",
                    i, pointer_candidates, MAX_PRESERVED);
            }
        }
    }

    // Restore object pool
    // Object pool: mirror of the active-slot save. If the save buffer's
    // first byte is 0 the slot was inactive at save time -- zero the live
    // slot to match (and prevent stale active data from bleeding through
    // if the slot is currently live but was dead at the saved frame).
    //
    // Carve-out: per-object color override fields at +68/72/76/80 (16 B at
    // offset 0x44 within each 382-byte object struct). These are written
    // by ProcessColorInterpolation during render and read by
    // sprite_rendering_engine (`mov edi, [ecx+44h]` @ 0x40D5C7) -- sim
    // never reads them. They need to PERSIST across frames so palette mode
    // 1 (Tyrogue's "fade-to-black-and-stay") can keep showing the last
    // interpolated value after the timer hits 0 and ProcessColorInterpolation
    // skips writing. Restoring them on Load wipes that persistence and the
    // visual snaps back to default palette every frame. Three-slice
    // restore: pre-override, [SKIP] override, post-override. Same per-slot
    // logic for the inactive-slot case (zero the slot but skip the override
    // bytes -- though "inactive" objects shouldn't have meaningful overrides
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
        // -- host's rollback Load leaves it alone, replay's forward sim
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
    // re-sim decrements would converge -- but that froze host's palette at
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
    // hardware state is NOT touched -- the post-advance sync decides which
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
    // crossing_teardown OR'd in because the substate term alone never fires
    // once the CSS-init object owns g_object_data_ptr (see the entry comment),
    // which is precisely the set of restores it was written to cover.
    if (live_substate_pre >= 900 || crossing_teardown) {
        Fm2k_ClearAfterimageIndices();
    }

    // Match-end script-VM guard. The primary fix is sim-side (round_events.cpp,
    // `post >= RSS_MATCH_COMPLETE`), which is what makes both peers and every
    // resim agree; this is the restore-side backstop for the one case that
    // cannot reach it: a rollback that lands BEFORE the 902 edge while the live
    // process has already torn the battle down. Such a restore resurrects live
    // type-4 objects holding battle-era cursors into blobs the CSS entry has
    // already GlobalFree'd (or replaced with a smaller one for a different
    // character), and the very next resim tick would fetch an opcode through
    // them at 0x4125FC.
    //
    // Determinism note, stated plainly: a rollback whose restored frames were
    // originally simulated BEFORE the heap was freed can never be bit-identical
    // to that original pass -- SaveState_Load cannot un-GlobalFree memory, so
    // that window is already outside the deterministic contract regardless of
    // what we do here (this is the same trade v0.2.81 made for the afterimage
    // pool). For every restore at or after the 902 edge -- the overwhelmingly
    // common case, because Fm2k_Neutralize... ran inside the tick that produced
    // those savestates -- the call below is a pure identity write and changes
    // nothing. The window it can perturb is bounded by the battle-end swap
    // frame, a handful of frames later, after which the session is destroyed.
    if (crossing_teardown) {
        Fm2k_CheckEndSeamScriptCursors("load-teardown");
        Fm2k_NeutralizeMatchEndScriptObjects();
    }

    return true;
}

// =============================================================================
// TESTING / VERIFICATION
// =============================================================================

// Addresses for snapshot capture
#endif  // engine guard

