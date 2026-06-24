// Netplay rollback UPDATE-EVENT handlers: GekkoSaveEvent / GekkoLoadEvent /
// GekkoAdvanceEvent. Extracted VERBATIM from Netplay_ProcessBattleInputPhase
// (netplay_battle_phase.cpp) so the driver there stays a slim, engine-agnostic
// dispatch loop. Shares file-scope state + perf instrumentation via
// netplay_internal.h.
//
// ENGINE SEAM (FM95 reuse): these handlers are engine-agnostic in STRUCTURE --
// save/load route through SaveState_* (engine-split) and the sim ticks through
// original_process_game_inputs / original_update_game (engine globals). The
// FM2K-specific bits are isolated:
//   * LOAD-BEARING: the palette-flash speculative guard in the Advance handler
//     poke kEffectSys1Addr/kPflash2Addr (FM2K globals). It is compiled out on
//     FM95 (`if constexpr (!FM2K::kIsFM95)`) -- FM95 must supply its own when
//     support lands; until then it must NOT poke FM2K addresses into its space.
//   * DIAGNOSTIC-ONLY ([RING]/[HOST-TRACE]/[HOST-FP]/[PHASE-F]/BATTLE STATUS):
//     hardcoded FM2K addresses, but every one is env-gated and OFF by default,
//     so it never fires on a normal FM95 run. Marked [FM2K-DIAG]; converting
//     these literals to FM2K::ADDR_* is a follow-up when FM95 lands.
#include "netplay.h"
#include "netplay_internal.h"  // shared file-scope state + PerfScope/PerfBucket
#include "../hooks/hooks.h"     // Hook_ApplySOCD_Public, Hook_FlushRngTrace
#include "savestate.h"
#include "spectator_node.h"
#include "globals.h"            // FM2K::ADDR_*, FM2K::kIsFM95, original_*, g_sim_step_count
#include "gekkonet.h"
#include "../parity/parity_recorder.h"  // ParityRecorder::Capture/Close
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <atomic>

// [RING] trace gate (task #34 + cross-peer fork hunt): cached
// FM2K_RING_TRACE env check. Value 1 = legacy 40-frame window;
// value N>1 = trace confirmed frames 0..N. [FM2K-DIAG] -- the trace
// addresses below (0x447EE0 buf_idx, 0x4280E0/0x4290E0 input history)
// are FM2K-only and env-gated; inert on FM95 unless the env var is set.
static int RingTraceMax() {
    static int c = -2;
    if (c == -2) {
        const char* v = std::getenv("FM2K_RING_TRACE");
        c = (v && v[0] && v[0] != '0') ? std::atoi(v) : 0;
        if (c == 1) c = 40;
    }
    return c;
}
static bool RingTraceOn() { return RingTraceMax() > 0; }

void Netplay_HandleSaveEvent(GekkoGameEvent* update) {
    int frame = update->data.save.frame;
    // [PHASE-F-DIAG] log save event details BEFORE Save (rng_seed
    // is read from live engine state; should match the AdvEvent
    // EXIT log's rng if no mutation happened in between).
    static int s_save_diag_cached = -1;
    if (s_save_diag_cached < 0) {
        const char* v = std::getenv("FM2K_STRESS_DIAG");
        s_save_diag_cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (s_save_diag_cached == 1) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[PHASE-F] SaveEvent save.frame=%d g_netplay_frame=%u "
            "live_rng=0x%08X",
            frame, g_netplay_frame,
            *(uint32_t*)FM2K::ADDR_RANDOM_SEED);
    }
    { PerfScope _ps(&g_perf_save); SaveState_Save(frame); }  // profile save cost (#62)
    // [RING] trace (FM2K_RING_TRACE=1, frames 0..40): buf_idx +
    // ring slots at every Save/Load/Advance so the one-slot
    // input-history shift (task #34) can be watched being born
    // at the first rollback.
    if (RingTraceOn() && frame <= RingTraceMax()) {
        const uint32_t b = *(uint32_t*)0x447EE0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[RING] SAVE f=%d buf=%u h1[b]=%03X h1[b-1]=%03X",
            frame, b,
            *(uint32_t*)(0x4280E0 + (b & 0x3FF) * 4),
            *(uint32_t*)(0x4280E0 + ((b - 1) & 0x3FF) * 4));
    }
    (void)SaveState_GetLastChecksum(frame);  // updates RegionChecksums via side effect
    // Use the gameplay fingerprint as GekkoNet's desync checksum.
    // If both peers see the same HP/pos/rng/timer values at this
    // frame, fingerprints match and GekkoNet stays happy even when
    // per-process memory residue differs. Full combined hash is
    // still computed and available for the diagnostic dump.
    uint32_t checksum = SaveState_GetRegionChecksums().gameplay_fingerprint;
    *update->data.save.state_len = sizeof(uint32_t);
    *update->data.save.checksum = checksum;
    memcpy(update->data.save.state, &frame, sizeof(uint32_t));

    // [CHECKSUM] -- FULL-STATE FENCEPOST (GDC GAP #1). The gameplay_fingerprint is
    // the DETERMINISTIC HP/pos/rng/timer hash GekkoNet uses for P1-vs-P2 desync
    // detection, computed at the SAVE event = re-emitted on every re-sim, so the
    // harness's dedupe-by-frame-last yields the CONFIRMED state (positions
    // included). The players are already cross-checked by gekko; logging it lets
    // the harness extend the same check to the SPECTATORS (not in the gekko
    // session). Gated FM2K_CINPUT.
    {
        static int s_ck = -1;
        if (s_ck < 0) { const char* v = std::getenv("FM2K_CINPUT"); s_ck = (v && v[0] == '1') ? 1 : 0; }
        if (s_ck == 1)
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[CHECKSUM] f=%d crc=0x%08X", frame, checksum);
    }
}

// GekkoNet's update sequence is:
//   RewindRunahead -> [maybe HandleRollback] -> Advance -> Save
//   -> [HandleRunahead emits Save + N speculative Advances]
// RewindRunahead unconditionally emits a LoadEvent once runahead has
// run at least once (to undo last tick's speculative advances). It
// is NOT a real rollback. Real rollbacks emit a SECOND LoadEvent in
// the same batch from HandleRollback. The first LoadEvent of every
// batch is therefore the runahead rewind; only the 2nd+ should be
// counted as user-visible rollbacks. `load_events_in_batch` (owned by
// the driver, passed by ref) is that per-batch counter.
void Netplay_HandleLoadEvent(GekkoGameEvent* update, int& load_events_in_batch) {
    int frame = update->data.load.frame;
    load_events_in_batch++;
    // The first LoadEvent in a batch is the runahead rewind from
    // last tick's speculative advance -- a fixed-distance load
    // every tick, NOT a network rollback -- so it's excluded from
    // the stats. BUT that's only true when runahead is actually
    // ON. With runahead OFF (the bleeding default since the
    // runahead-off change), there IS no speculative rewind: a
    // real network rollback is a SINGLE LoadEvent (batch==1).
    // Gating only on (batch==1) then misclassified every real
    // rollback as a runahead rewind, so g_rollback_count stayed
    // pinned at 0 forever even while rollbacks visibly happened
    // (pringle's "visual rollbacks but the number stays 0", and
    // the rb_total=0 on the 200ms c785d0ca desync). Only skip the
    // first load when runahead is live.
    const bool runahead_on =
        g_runahead_active.load(std::memory_order_acquire) > 0;
    const bool is_runahead_rewind =
        runahead_on && (load_events_in_batch == 1);
    g_last_rollback_frame = g_netplay_frame;
    // Rolling stats only fire for real (non-runahead) rollbacks.
    // The first LoadEvent in any batch is the runahead rewind
    // from last tick's speculative advance -- a fixed-distance
    // load every tick under runahead, NOT a network-driven
    // rollback. Counting it inflated max_rewind to a constant
    // value and `% 100 == 0` was true when count==0, so the
    // log fired every tick until the first real rollback (15MB
    // of spam in steady-state replays).
    static uint32_t s_rb_window_rewind_sum = 0;
    static uint32_t s_rb_window_rewind_max = 0;
    if (!is_runahead_rewind) {
        g_rollback_count++;
        if (g_rollback_count == 1) {
            s_rb_window_rewind_sum = 0;
            s_rb_window_rewind_max = 0;
        }
        uint32_t rewind = g_netplay_frame - (uint32_t)frame;
        s_rb_window_rewind_sum += rewind;
        if (rewind > s_rb_window_rewind_max) s_rb_window_rewind_max = rewind;
        // [BEAT] window — same numbers but reset on each
        // heartbeat emit (every ~10s) so the BEAT line
        // describes the most recent window, not session totals.
        g_beat_window_rb_sum += rewind;
        g_beat_window_rb_count++;
        if (rewind > g_beat_window_rb_max) g_beat_window_rb_max = rewind;
        if (g_rollback_count > 0 && g_rollback_count % 100 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ROLLBACK stats: total=%u, last-100 avg_rewind=%.1f max_rewind=%u (last frame=%d current=%u)",
                g_rollback_count,
                (double)s_rb_window_rewind_sum / 100.0,
                s_rb_window_rewind_max,
                frame, g_netplay_frame);
            s_rb_window_rewind_sum = 0;
            s_rb_window_rewind_max = 0;
        }
    }
    { PerfScope _pl(&g_perf_load); SaveState_Load(frame); }  // profile load cost (#62)
    if (RingTraceOn() && frame <= RingTraceMax()) {
        const uint32_t b = *(uint32_t*)0x447EE0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[RING] LOAD f=%d buf=%u h1[b]=%03X h1[b-1]=%03X",
            frame, b,
            *(uint32_t*)(0x4280E0 + (b & 0x3FF) * 4),
            *(uint32_t*)(0x4280E0 + ((b - 1) & 0x3FF) * 4));
    }
    // Rewind the deterministic virtual clock to match the loaded
    // frame. Without this g_virtual_time_ms stays at its forward-sim
    // value, replay main_game_loop iterations read timeGetTime()
    // values AHEAD of where forward was at the same frame, and the
    // self-referential arithmetic at main_game_loop 0x405BA8/0x405BBB
    // (g_last_frame_time += N*10) writes a different byte pattern
    // into g_last_frame_time @ 0x447DD4 — which is what produced
    // the "AfterimagePool +0x4A4" divergence at f=9.
    extern uint32_t g_virtual_time_ms;
    g_virtual_time_ms = (uint32_t)frame * 10;
    // Also rewind g_netplay_frame so the per-advance increments
    // below produce the same frame numbers the forward pass did.
    g_netplay_frame = (uint32_t)frame;
}

void Netplay_HandleAdvanceEvent(GekkoGameEvent* update, bool& has_advance,
                                uint32_t& earliest_advance) {
    // Authoritative frame label straight from gekko (task #34
    // root-cause fix, 2026-06-11). The old scheme (increment at
    // the end of each advance + relabel to load.frame at
    // LoadEvent) went ONE LOW after the first rollback batch:
    // gekko's Load(N) restores POST-frame-N state, so the next
    // advance produces frame N+1 -- but we relabeled to N. The
    // recorder's monotonic dedup gate below then swallowed the
    // first confirmed frame after the batch, leaving every
    // .fm2krep one INPUT short from the session's first
    // rollback onward. Replay playback consumed the shifted
    // stream (each input one frame early), eventually visibly
    // desyncing -- the "replays desync from matches" bug.
    // Proven via [RING] trace + raw .fm2krep diff: sim frame
    // 11's input (000,006) absent from the file, two confirmed
    // advances both labeled f=10.
    g_netplay_frame = (uint32_t)update->data.adv.frame;

    // Record the pre-sim frame for the Mike Z sound sync window.
    // `g_netplay_frame` here is the frame we're about to *produce*;
    // Hook_DispatchScriptSoundCommand tags desired[] with this same
    // value (via Netplay_GetFrame) so earliest/latest compare apples
    // to apples.
    if (g_netplay_frame < earliest_advance) {
        earliest_advance = g_netplay_frame;
    }

    // Set inputs for THIS frame
    if (update->data.adv.inputs && update->data.adv.input_len >= sizeof(uint16_t) * 2) {
        uint16_t* inputs = (uint16_t*)update->data.adv.inputs;
        g_p1_input = inputs[0] & 0x7FF;
        g_p2_input = inputs[1] & 0x7FF;
    }

    // Track rollback state (matching BBBR reference implementation).
    // During rollback replay, input edge detection globals must NOT
    // be overwritten - they get restored by LoadEvent and should only
    // be updated on the authoritative (non-rollback) frame.
    g_is_rolling_back = update->data.adv.rolling_back;

    // [REMOVED] per-slot fan-out at slot+0xDF79 / slot+0xDF7D.
    // main_game_loop's prologue writes these every iteration,
    // but IDA xref of g_p1_input_buffer_index_field (0x4DFD0D)
    // shows the only reader is check_game_continue, which is
    // a no-op when g_directplay_interface == NULL — and we
    // never initialize DirectPlay. KGT scripts and
    // update_game/process_game_inputs do not read these
    // fields. The writes were just trampling adjacent slot
    // memory the StudioS chars happen to use, breaking
    // their scripts.

    // Snapshot RNG BEFORE PGI/UG run. Used by the [HOST-TRACE] log
    // below — distinguishes "per-frame leak inside PGI/UG" from
    // "inter-frame leak between confirmed advances." [FM2K-DIAG addr]
    const uint32_t rng_pre_advance = *(uint32_t*)0x41FB1C;

    // Speculative-advance carve-out for palette flash state.
    //
    // Why: original_update_game is the game's update_game_state @
    // 0x404CD0 — a 7-instruction stub that does nothing but
    // decrement two palette flash timers (g_timer_countdown1 @
    // 0x4456E4 and g_timer_countdown2 @ 0x447D91). PGI also runs
    // character_state_machine which may re-fire [EB] and write
    // timer = duration during a script's trigger anim slot.
    //
    // Under runahead = 4, every wall-clock frame fires 1 confirmed
    // + 4 speculative AdvanceEvents. Each speculative advance also
    // hits update_game_state, decrementing palette timers. Net:
    // palette drains 5x per wall-clock frame instead of 1x — a
    // 43-frame palette flash plays for ~9 wall-clock frames and
    // then "cuts out", which is the bug user reported.
    //
    // Fix: snapshot the palette flash structs before PGI runs,
    // and after UG returns, restore them IFF this advance is
    // speculative (running_ahead). Confirmed advances drain the
    // timer naturally; speculative advances are a no-op on
    // palette state. End result: timer drains 1/wall-clock-frame
    // matching vanilla.
    //
    // Why not the same fix for shake? Shake is decremented in
    // RENDER (ProcessShakeEffect), not sim. Render runs at most
    // once per wall-clock frame regardless of advance count, so
    // shake already drains correctly. Verified empirically — the
    // same diag log shows shake decrementing 1/frame while
    // palette decremented 5/frame.
    constexpr uintptr_t kEffectSys1Addr = 0x447D7D;
    constexpr size_t    kEffectSys1Size = 42;
    constexpr uintptr_t kPflash2Addr    = 0x4456D0;
    constexpr size_t    kPflash2Size    = 44;
    struct PaletteSnapshot {
        uint8_t  effect_sys1[kEffectSys1Size];   // 0x447D7D, 42 B
        uint8_t  pflash2[kPflash2Size];          // 0x4456D0, palette-flash-2 struct
        uint32_t rng_seed;                       // 0x41FB1C
    };
    [[maybe_unused]] PaletteSnapshot pal_pre{};
    [[maybe_unused]] const bool running_ahead = update->data.adv.running_ahead;
    // Run-ahead-only palette snapshot. Rolling-back re-sim
    // INTENTIONALLY lets palette decrement naturally so it
    // reaches the same final state as forward sim — paired
    // with the savestate.cpp change that now saves/restores
    // real palette bytes (was zeroed). Initial attempt
    // extended the snapshot to rolling_back too, but that
    // froze palette at the Load-time value instead of letting
    // re-sim drain it, which is worse — replay's palette
    // drains naturally over forward sim so host needs to
    // match by also draining during re-sim.
    //
    // [FM2K-SPECIFIC, LOAD-BEARING] kEffectSys1Addr (0x447D7D) and
    // kPflash2Addr (0x4456D0) are FM2K globals. FM95 has no palette
    // flash at these addresses; poking them on FM95 would corrupt
    // its memory, so the guard is compiled out there (kIsFM95). FM95
    // must supply its own speculative-state guard when support lands.
    // On FM2K (kIsFM95==false) this is identical codegen to before.
    if constexpr (!FM2K::kIsFM95) {
        if (running_ahead) {
            // Palette-flash TIMER snapshot only (prevents the flash
            // timer draining ~5x under runahead -> the "flash cuts
            // out" bug). The RNG snapshot/restore that used to live
            // here is GONE: render-side rng is now its own stream
            // (see Hook_GameRand / globals.h), so speculative sim
            // advances no longer over-consume render rng, and manually
            // restoring the gameplay seed here is exactly what made
            // cross-peer rng diverge under real rollback.
            std::memcpy(pal_pre.effect_sys1,
                        (void*)kEffectSys1Addr,
                        kEffectSys1Size);
            std::memcpy(pal_pre.pflash2, (void*)kPflash2Addr, kPflash2Size);
        }
    }

    // v0.2.48's Phase F PostRenderRng restore was re-tested
    // 2026-05-17 (faithful re-add WITH the running_ahead skip)
    // and STILL caused cross-peer desync at battle frame ~873.
    // So v0.2.48's fix itself is bugged in real-netplay rollback,
    // not just my missing-skip from earlier today. Keeping
    // the SaveState_GetPostRenderRng helper compiled in but
    // not calling it — replay determinism will need a
    // different approach.
    //
    // Diagnostic note: every desync we've seen has buf_idx
    // forward=K replay=K+1 — that's gekko's RunaheadSave
    // (saves frame K state at end-of-K-1) vs ConfirmedSave
    // (saves frame K state at end-of-K) artifact, NOT a real
    // engine divergence. But the CROSS-PEER Local≠Remote CRC
    // IS real, and it goes away when this PostRenderRng
    // restore is removed.

    // [PHASE-F-DIAG] (FM2K_STRESS_DIAG=1): log every AdvanceEvent
    // entry/exit RNG + inputs + rolling_back/running_ahead flag.
    // Goal: see whether forward vs replay sim_N read the same
    // pre-sim engine state and produce the same post-sim state.
    // If pre matches but post differs → sim consumes RNG based
    // on memory we don't save/restore. If pre already differs
    // → load isn't restoring correctly. Gated on env var so
    // production builds don't pay the log cost.
    const bool diag_enabled = []() {
        static int cached = -1;
        if (cached < 0) {
            const char* v = std::getenv("FM2K_STRESS_DIAG");
            cached = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        return cached == 1;
    }();
    const uint32_t pre_sim_rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    if (diag_enabled) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[PHASE-F] AdvEvent ENTRY g_netplay_frame=%u rolling_back=%d running_ahead=%d "
            "p1=0x%03X p2=0x%03X rng=0x%08X",
            g_netplay_frame, (int)update->data.adv.rolling_back,
            (int)update->data.adv.running_ahead,
            g_p1_input, g_p2_input, pre_sim_rng);
    }

    // Run a FULL game tick for EVERY AdvanceEvent (matching GekkoNet examples).
    // The game loop must NOT run its own tick - we handle everything here.
    {
        PerfScope _pa(&g_perf_adv);  // profile game-tick cost (#62)
        if (original_process_game_inputs) {
            original_process_game_inputs();
        }
        if (original_update_game) {
            original_update_game();
        }
        ++g_sim_step_count;   // sim-fps: one logic tick (netplay battle advance, incl. re-sims)
    }
    PerfMaybeReport();

    // [FM2K-SPECIFIC, LOAD-BEARING] restore — see snapshot guard above.
    if constexpr (!FM2K::kIsFM95) {
        if (running_ahead) {
            // Restore palette-flash TIMER only (see snapshot above).
            // Gameplay RNG is intentionally NOT restored here anymore.
            std::memcpy((void*)kEffectSys1Addr,
                        pal_pre.effect_sys1,
                        kEffectSys1Size);
            std::memcpy((void*)kPflash2Addr, pal_pre.pflash2, kPflash2Size);
        }
    }

    if (diag_enabled) {
        const uint32_t post_sim_rng = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[PHASE-F] AdvEvent EXIT  g_netplay_frame=%u rng=0x%08X (delta=0x%08X)",
            g_netplay_frame, post_sim_rng,
            post_sim_rng - pre_sim_rng);
    }

    if (RingTraceOn() && g_netplay_frame <= (uint32_t)RingTraceMax()) {
        const uint32_t b = *(uint32_t*)0x447EE0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[RING] ADV  f=%u rb=%d ra=%d buf=%u h1[b]=%03X in1=%03X in2=%03X h2[b]=%03X",
            g_netplay_frame, (int)update->data.adv.rolling_back,
            (int)update->data.adv.running_ahead, b,
            *(uint32_t*)(0x4280E0 + (b & 0x3FF) * 4),
            g_p1_input, g_p2_input,
            *(uint32_t*)(0x4290E0 + (b & 0x3FF) * 4));
    }

    // Confirmed-input ring: every non-runahead advance writes
    // its frame's inputs; rolling_back corrections OVERWRITE
    // the speculative first pass, so by the time the flush
    // below reaches a frame its values are final.
    if (!g_stress_mode && !update->data.adv.running_ahead) {
        const uint32_t f = (uint32_t)update->data.adv.frame;
        auto& slot = g_pending_confirm[f % PENDING_CONFIRM_RING];
        slot.frame = f;
        slot.p1 = Hook_ApplySOCD_Public(g_p1_input);
        slot.p2 = Hook_ApplySOCD_Public(g_p2_input);
    }

    g_is_rolling_back = false;
    g_netplay_frame++;
    has_advance = true;

    // Parity recorder: post-advance snapshot per confirmed
    // battle frame. In stress mode the GekkoNet-driven loop
    // bypasses main_loop_trampoline's per-frame Capture
    // calls (those fire from CSS / spec-playback paths), so
    // without this the .pty captures only the header and
    // record-side stays at 0 snapshots. Skip rolling_back
    // and running_ahead re-advances — they'd produce
    // duplicate frame entries.
    if (!update->data.adv.rolling_back &&
        !update->data.adv.running_ahead) {
        // Monotonic per-frame dedup. Under the heavy-stage
        // sim/render decouple (RunBattleTick runs this N times per
        // render) with asymmetric rollback, gekko can re-emit a
        // confirmed advance for a frame already captured, padding
        // the .pty and misaligning the cross-peer index diff
        // (wanwan stall run: 850 vs 813 snapshots). A confirmed
        // frame's state is FINAL, so record each frame number once.
        // != (not >) so a fresh battle's frame 0 after a stale high
        // value still records.
        static uint32_t s_last_parity_frame = UINT32_MAX;
        if (g_netplay_frame != s_last_parity_frame) {
            ParityRecorder::Capture();
            s_last_parity_frame = g_netplay_frame;
        }
    }

    // Synthetic desync trigger — runs after the frame
    // counter advances so we can match on a specific frame.
    // One-shot per match; arms only when the env var is
    // set. Skip on rollback re-sims (g_is_rolling_back is
    // false here because we just unset it, but check anyway
    // for safety against future refactors).
    MaybeFireSyntheticDesync();

    // Harness terminator — clean exit at a configured battle
    // frame so the replay-self-test driver can pair record
    // and playback runs of identical length. Env-driven,
    // one-shot, fires on confirmed advances only (skip
    // rollback re-sims). Pairs with FM2K_PARITY_AUTOPLAY +
    // FM2K_PARITY_RECORD_PATH on a record-then-replay run.
    //
    // Goes through HandleDesyncDetected so the same
    // .fm2krep/.pty flush + manifest path runs, then
    // TerminateProcess(0) for a clean exit (vs the desync
    // path's exit(1)). Reusing the dump pipeline keeps the
    // diagnostic artifacts comparable.
    {
        static int s_terminate_at = -2;
        if (s_terminate_at == -2) {
            const char* v = std::getenv("FM2K_AUTO_TERMINATE_AT_FRAME");
            s_terminate_at = (v && *v) ? std::atoi(v) : -1;
            if (s_terminate_at > 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Harness: will TerminateProcess at battle frame %d "
                    "(env FM2K_AUTO_TERMINATE_AT_FRAME=%s)",
                    s_terminate_at, v);
            }
        }
        // FM2K_AUTO_TERMINATE_TOTAL: session-cumulative variant.
        // g_netplay_frame resets per battle, so AT_FRAME cannot
        // target anything past match 1 -- multi-match harness
        // runs (spectator across MATCH_END -> CSS -> match 2)
        // terminate on TOTAL confirmed battle frames instead.
        static int s_terminate_total = -2;
        static uint32_t s_total_confirmed = 0;
        if (s_terminate_total == -2) {
            const char* v = std::getenv("FM2K_AUTO_TERMINATE_TOTAL");
            s_terminate_total = (v && *v) ? std::atoi(v) : -1;
            if (s_terminate_total > 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Harness: will TerminateProcess at %d TOTAL "
                    "confirmed battle frames (across matches)",
                    s_terminate_total);
            }
        }
        if (!update->data.adv.rolling_back &&
            !update->data.adv.running_ahead) {
            ++s_total_confirmed;
        }
        const bool fire_at    = s_terminate_at > 0 &&
            (int)g_netplay_frame >= s_terminate_at;
        const bool fire_total = s_terminate_total > 0 &&
            (int)s_total_confirmed >= s_terminate_total;
        if ((fire_at || fire_total) &&
            !update->data.adv.rolling_back &&
            !update->data.adv.running_ahead) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Harness: reached battle frame %u (total %u) — "
                "flushing logs, writing .fm2krep, terminating cleanly.",
                g_netplay_frame, s_total_confirmed);
            // Append a synthetic MATCH_END so the slice writer
            // produces a complete .fm2krep. Replay reader pops
            // events until MATCH_END; without it playback
            // stalls at the last INPUT.
            MatchEndPayload mep = {};
            mep.winner_idx = 2;  // draw / unknown
            SpectatorNode_OnMatchEnd(mep);
            // Write per-battle slice. Same filename pattern
            // as Netplay_EndBattle so the harness driver knows
            // where to look. Fails silently if no MATCH_START
            // was emitted (e.g., session torn down mid-frame).
            char ts[64] = {};
            std::time_t now = std::time(nullptr);
            std::tm tm_buf{};
            localtime_s(&tm_buf, &now);
            // Per-player filename: in the loopback netplay
            // harness BOTH peers hit this terminator in the
            // same second and previously wrote the SAME path,
            // P2 clobbering P1's canonical file (P1's carries
            // session_id + ROUND events; P2's does not).
            char pattern[64];
            std::snprintf(pattern, sizeof(pattern),
                "replays/%%Y-%%m-%%d_%%H%%M%%S_p%d_harness.fm2krep",
                g_player_index);
            std::strftime(ts, sizeof(ts), pattern, &tm_buf);
            CreateDirectoryA("replays", nullptr);
            bool wrote = SpectatorNode_WriteCurrentBattleFile(ts);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Harness: .fm2krep slice %s -> %s",
                wrote ? "written" : "WRITE FAILED",
                wrote ? ts : "(no MATCH_START in session_events?)");
            SaveState_FlushRngTrace(g_player_index,
                "harness auto-terminate");
            // Flush parity recorder — fopen("wb") is fully
            // buffered and TerminateProcess kills the stdio
            // buffer without writing. Without this the
            // record.pty hits disk as 0 bytes.
            ParityRecorder::Close();
            // Same flush for FM2K_RNG_TRACE=1 log (Hook_GameRand
            // buffered output). Without this, the trace ends
            // at the last 8192-call autoflush which may be
            // many frames behind.
            Hook_FlushRngTrace();
            // Same for the CSM dispatch-loop diagnostic log.
            extern void Hook_FlushCsmDiag();
            Hook_FlushCsmDiag();
            fflush(stdout);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }

    // Replay recording + spectator stream. Gate on TRUE
    // confirmed advances only:
    //
    //   running_ahead=true → speculative runahead with
    //     PREDICTED inputs. We must NOT record these — when
    //     the prediction turns out wrong, GekkoNet rolls back
    //     and replays with the real input, but our recorded
    //     session_history would already contain the wrong
    //     prediction → spectator processes wrong inputs →
    //     desync. (This was the running-ahead-after-resync
    //     bug.)
    //   rolling_back=true → mid-rollback replay. Inputs are
    //     correct (post-confirmation) but we already recorded
    //     this frame on its first forward pass.
    //   monotonic gate → belt-and-suspenders against any
    //     other re-advance pattern.
    //
    // Only !rolling_back && !running_ahead && new-frame ever
    // hits session_history.
    if (!update->data.adv.rolling_back &&
        !update->data.adv.running_ahead &&
        g_netplay_frame > g_highest_recorded_frame)
    {
        g_highest_recorded_frame = g_netplay_frame;

        // PER-FRAME alignment-trace log: every confirmed frame
        // for the first 100 frames of each battle. Pairs with the
        // identical [SPEC-TRACE] log on spectator. Comparing
        // (rng_pre, rng_post, p1_in, p2_in) at every battle frame
        // tells us:
        //   * inputs match every frame? → alignment correct
        //   * inputs differ at some bf? → alignment off / wrong
        //                                 input popped that frame
        //   * rng_pre matches but rng_post differs → PGI+UG itself
        //                                            calls game_rand
        //                                            different #
        //   * rng_pre differs → leak between confirmed advances
        //                       (something between this frame and
        //                       last frame mutated RNG)
        // Long trace: 5000 confirmed frames (~50 sec), so we
        // capture the wall-clock window where the user reports
        // visible desync. "bf-1" subtraction aligns this with
        // spec's bf=K labelling (spec starts at 0, host's
        // post-increment makes its first log bf=1).
        // [HOST-TRACE] per-frame for first 5000 battle frames.
        // Routed via SDL_LOG_CATEGORY_CUSTOM into quill's
        // backtrace ring (in-memory; flushed to disk on any
        // LOG_ERROR). FM2K_SPECTATOR_DEBUG=1 routes to disk.
        //
        // Off by default since 2026-05-18 — set FM2K_HOST_TRACE=1
        // to opt in. Previous default added ~25K lines per
        // session (36% of log volume) just for diagnosis runs.
        // [FM2K-DIAG] pool/HP addresses below are FM2K-only + env-gated.
        static int s_host_trace_enabled = -1;
        if (s_host_trace_enabled < 0) {
            const char* v = std::getenv("FM2K_HOST_TRACE");
            s_host_trace_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_host_trace_enabled &&
            g_netplay_frame > 0 && g_netplay_frame <= 5000) {
            constexpr uintptr_t POOL = 0x4701E0;
            constexpr size_t    SLOT = 382;
            const int32_t p1_x = *(int32_t*)(POOL + 0 * SLOT + 0x08);
            const int32_t p2_x = *(int32_t*)(POOL + 1 * SLOT + 0x08);
            const int32_t p1_script = *(int32_t*)(POOL + 0 * SLOT + 0x30);
            const int32_t p2_script = *(int32_t*)(POOL + 1 * SLOT + 0x30);
            const uint32_t p1_hp = *(uint32_t*)0x4DFC85;
            const uint32_t p2_hp = *(uint32_t*)0x4EDCC4;
            SDL_LogInfo(SDL_LOG_CATEGORY_CUSTOM,
                "[HOST-TRACE] bf=%u rng_pre=0x%08X rng_post=0x%08X "
                "p1=0x%03X p2=0x%03X p1_x=%d p2_x=%d "
                "p1_s=%d p2_s=%d hp=%u/%u",
                g_netplay_frame - 1u, rng_pre_advance,
                *(uint32_t*)0x41FB1C,
                g_p1_input, g_p2_input,
                p1_x, p2_x, p1_script, p2_script,
                p1_hp, p2_hp);
        }

        // (Legacy Replay::Replay_RecordFrame call retired in
        // v0.2.27 — SpectatorNode_OnFrameConfirmed below pushes
        // the same frame data into the v2 .fm2krep event stream
        // via the SessionEvent log.)
        // Spectator input forwarding: gate is here because this
        // site is already !rolling_back && !running_ahead &&
        // new_frame — the dedup we need to avoid runahead /
        // rollback re-recording the same frame. Pre-battle
        // frames (title screen, CSS) are captured by
        // Hook_GetPlayerInput's capture_and_return, which is
        // correct for those phases (no rollback there).
        //
        // Phase F (#23, para's replay desync bug): the values
        // gekko delivers in g_p1_input / g_p2_input are RAW —
        // pre-SOCD, pre-facing-flip. The HOST engine then
        // applies Hook_ApplySOCD before feeding them into the
        // game. A spectator (live or .fm2krep replay) reads
        // these stored values and ALSO applies SOCD — but
        // using ITS OWN env-var-derived SOCD mode, which can
        // differ from the host's. Result: divergent input on
        // any frame where the user held L+R or U+D and the
        // spec's mode resolves it differently → cascading
        // script / position drift (parity_diff showed first
        // divergence at bf=1260 in p1_pos/p1_script for
        // vanpri, NOT in RNG).
        //
        // Fix: pre-apply the HOST's SOCD here. The stored
        // value carries the host's resolution. Spec's later
        // SOCD-application becomes idempotent (resolved
        // inputs don't trigger SOCD branches), so it doesn't
        // matter what mode the spec is in. Facing flip still
        // happens on the spec side, driven by deterministic
        // sim state — correct as long as sim hasn't diverged
        // upstream (it hasn't, since we now feed identical
        // post-SOCD inputs to both engines).
        const uint16_t p1_for_spec = Hook_ApplySOCD_Public(g_p1_input);
        const uint16_t p2_for_spec = Hook_ApplySOCD_Public(g_p2_input);
        // STRESS: record immediately -- no remotes, no
        // predictions, every advance is confirmed truth.
        //
        // NETPLAY: do NOT record here. Under input prediction
        // (window 16) this rb=0 advance may be SPECULATIVE --
        // gekko advances with predicted remote inputs and the
        // correction arrives later as a rolling_back re-advance
        // (which this gate skips, and the monotonic gate blocks
        // re-recording). Result: .fm2krep + live spectator
        // stream recorded mispredicted inputs on any predicting
        // peer -- on real internet BOTH peers predict, so real
        // matches shipped corrupted replays/spec streams. The
        // netplay path records via the pending ring + confirmed
        // -horizon flush after the event loop instead.
        if (g_stress_mode) {
            SpectatorNode_OnFrameConfirmed(p1_for_spec, p2_for_spec);
        }

        // Per-frame state fingerprint for spectator-desync
        // diagnosis — pairs with [SPEC-FP] log in
        // RunSpectatorTick. Same sample addresses; spectator's
        // bf counter is its own pop count post-battle-entry,
        // host's bf is g_netplay_frame. Match by bf to find
        // first divergent frame.
        // [HOST-FP] every 30 frames — same diagnostic-ring
        // routing as [HOST-TRACE] above (CUSTOM category).
        // Same FM2K_HOST_TRACE=1 env gate so the pair stays
        // together when investigating a desync.
        // [FM2K-DIAG] pool/HP/timer addresses below are FM2K-only + env-gated.
        static int s_host_fp_enabled = -1;
        if (s_host_fp_enabled < 0) {
            const char* v = std::getenv("FM2K_HOST_TRACE");
            s_host_fp_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_host_fp_enabled && (g_netplay_frame % 30) == 0) {
            const uint32_t rng     = *(uint32_t*)0x41FB1C;
            const uint32_t buf_idx = *(uint32_t*)0x447EE0;
            const uint32_t p1_hp   = *(uint32_t*)0x4DFC85;
            const uint32_t p2_hp   = *(uint32_t*)0x4EDCC4;
            const uint32_t timer   = *(uint32_t*)0x470044;
            // World positions live in the object pool: slot 0 +0x08
            // = pos_x, slot 1 = slot 0 + 382. (0x470020 was the
            // CSS character-slot index, not the in-battle x —
            // earlier logs were comparing useless data.)
            constexpr uintptr_t POOL = 0x4701E0;
            constexpr size_t    SLOT = 382;
            const int32_t p1_x = *(int32_t*)(POOL + 0 * SLOT + 0x08);
            const int32_t p1_y = *(int32_t*)(POOL + 0 * SLOT + 0x0C);
            const int32_t p2_x = *(int32_t*)(POOL + 1 * SLOT + 0x08);
            const int32_t p2_y = *(int32_t*)(POOL + 1 * SLOT + 0x0C);
            const int32_t p1_script = *(int32_t*)(POOL + 0 * SLOT + 0x30);
            const int32_t p2_script = *(int32_t*)(POOL + 1 * SLOT + 0x30);
            SDL_LogInfo(SDL_LOG_CATEGORY_CUSTOM,
                "[HOST-FP] bf=%u rng=0x%08X buf=%u "
                "p1_hp=%u p2_hp=%u timer=%u "
                "p1_pos=(%d,%d) p2_pos=(%d,%d) "
                "p1_script=%d p2_script=%d "
                "p1_in=0x%03X p2_in=0x%03X",
                g_netplay_frame, rng, buf_idx,
                p1_hp, p2_hp, timer,
                p1_x, p1_y, p2_x, p2_y,
                p1_script, p2_script,
                g_p1_input, g_p2_input);
        }

        // C9: append FINGERPRINT op every 30 confirmed INPUTs.
        // Off by default; gated on FM2K_SPEC_FINGERPRINT=1.
        // Spectator's ApplySessionEvent computes the same hash
        // and logs WARN on mismatch.
        if (SpectatorFingerprint_Enabled() &&
            (g_netplay_frame % 30) == 0) {
            SpectatorNode_AppendFingerprint(
                SpectatorFingerprint_Compute());
        }
    }

    // Shake safety cap removed — the real fix is letting the
    // render-path timer decrement persist (see carve-out in
    // main_loop_trampoline.cpp:RenderFrameWithSnapshot). With
    // that in place, the KGT-scripted `Duration` value the
    // character author wrote drives how long shake lasts, and
    // the timer naturally reaches 0 -> ProcessShakeEffect zeros
    // the visible offset -> shake ends at exactly the designed
    // frame count. No per-frame force-clear needed.

    // Advance the deterministic virtual clock used by Hook_timeGetTime.
    // Exactly one bump per AdvanceEvent.
    extern uint32_t g_virtual_time_ms;
    g_virtual_time_ms += 10;

    // NOTE: we intentionally do NOT write g_last_frame_time here.
    // Setting it equal to g_virtual_time_ms made
    // main_game_loop's frame-skip check
    //   Time (= timeGetTime = g_virtual_time_ms) >= g_last_frame_time + 10
    // always false, so the game stopped running new iterations
    // (session hang at the save just before the write would have
    // fired). Leave g_last_frame_time to main_game_loop's own
    // prologue writes; if the forward-vs-replay divergence at
    // 0x447DD4 reappears we'll mask it at save time instead of
    // trying to drive it from here.

    // Tell Hook_RenderGame there is an unrendered authoritative
    // tick. The render hook is allowed to draw exactly this one
    // sim state, then clears the flag. Rolled-back replay ticks
    // do not set the flag — we only render the final resolved
    // state of an advance, never the intermediate replay frames.
    extern bool g_frame_pending_render;
    if (!update->data.adv.rolling_back) {
        g_frame_pending_render = true;
    }

    // Periodic status every 500 frames (~5 sec). Gate on
    // NON-rolling_back advances only — in stress mode g_netplay_frame
    // hits any given value N times (once forward, N-1 times on
    // replay) so a naive `% 500 == 0` fires on every replay
    // pass and floods the log. Also dedupe by last-logged frame
    // so rollback that re-hits a multiple-of-500 frame doesn't
    // re-log. [FM2K-DIAG] frame_time/skip/timer addresses are FM2K-only.
    // [FA-TRACE] (FM2K_FA_TRACE=1): log gekko frames-ahead every confirmed
    // battle frame for the first ~300 frames so the battle-start FA transient
    // can be measured. PURE LOGGING -- no logic/throttle/timing change. bf is
    // the live battle-frame label (g_netplay_frame). Reused by the fix phase.
    if (!update->data.adv.rolling_back && !update->data.adv.running_ahead) {
        static int s_fa_trace_enabled = -1;
        if (s_fa_trace_enabled < 0) {
            const char* v = std::getenv("FM2K_FA_TRACE");
            s_fa_trace_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_fa_trace_enabled && g_netplay_frame <= 300) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[FA-TRACE] bf=%u fa=%.2f",
                g_netplay_frame, Netplay_GetFramesAhead());
        }
    }

    if (!update->data.adv.rolling_back) {
        static uint32_t last_status_frame = 0;
        if (g_netplay_frame >= last_status_frame + 500) {
            last_status_frame = g_netplay_frame;
            float fa = g_session ? gekko_frames_ahead(g_session) : 0.0f;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "BATTLE STATUS: frame=%u rb=%u desync=%u ahead=%.1f frame_time_ms=%u skip=%u",
                g_netplay_frame, g_rollback_count, g_desync_count, fa,
                *(uint32_t*)0x41E2F0,
                *(uint32_t*)0x4246F4);
        }

        static uint32_t last_state_frame = 0;
        if (g_netplay_frame >= last_state_frame + 1000) {
            last_state_frame = g_netplay_frame;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "STATE f=%u: rng=0x%08X game_timer=%u round_timer_ctr=%u render_fc=%u",
                g_netplay_frame,
                *(uint32_t*)0x41FB1C,
                *(uint32_t*)0x470044,
                *(uint32_t*)0x424F00,
                *(uint32_t*)0x4456FC);
        }
    }
}
