// trampoline_battle.cpp -- RunBattleTick (stress/offline/netplay battle phase). Split from main_loop_trampoline.cpp.
#include "main_loop_trampoline.h"
#include "globals.h"
#include "../hooks/hooks.h"
#include "../hooks/wndproc_subclass.h"
#include "../netplay/netplay.h"
#include "../netplay/control_channel.h"
#include "../netplay/game_hash.h"
#include "../netplay/spectator_node.h"
#include "../ui/shared_mem.h"
#include "../parity/parity_recorder.h"
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include "trampoline_internal.h"

// Battle: GekkoNet owns state transitions. main_game_loop's prologue writes
// are gone — we neither replicate them here nor let the native wrapper run.
// Any per-advance setup (like the per-slot buf-idx fan-out) already lives
// inside the AdvanceEvent handler in netplay.cpp.
void RunBattleTick() {
    Hook_CheckGameModeTransition_Public();

    // Apply any pending F8 runahead toggle BEFORE gekko_update_session
    // chews through the tick. Safe even when no session is alive
    // (function early-exits) but most useful right here so the next
    // AdvanceEvent reflects the new runahead immediately.
    Netplay_PollRunaheadToggle();

    // Stress-mode path: no network, no peer sync. GekkoStressSession is the
    // determinism check — it rolls back every check_distance frames and
    // compares save hashes.
    if (g_stress_mode) {
        if (!Netplay_IsActive()) {
            if (!Netplay_StartStressBattle()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Trampoline: Netplay_StartStressBattle failed");
                return;
            }
        }
        Netplay_ProcessBattleInputPhase();
        if (g_frame_pending_render) {
            RenderFrameWithSnapshot();
            g_frame_pending_render = false;
        }
        // Drift adjustment is now handled by SleepToTarget at the
        // outer loop call site, which applies the 1.6% slowdown when
        // ahead. Sleep(0)/Sleep(extra_ms) trick that lived in
        // Netplay_HandleFrameTime was unreliable and never converged.
        return;
    }

    // Offline path: no peer, no GekkoNet, no sync barrier. Just run the sim
    // natively — same shape as RunNativeTick but invoked from the battle
    // phase. Without this branch, g_battle_entry_signaled_pub stays true and
    // Netplay_IsBattleSynced never returns true (there's no remote to sync
    // with), so RunBattleTick would hang forever the moment game_mode hits
    // 3000. Sound rollback / GekkoNet-driven state machine are inert here;
    // the hook gates on Netplay_IsActive() which stays false.
    if (g_offline_mode) {
        // [REMOVED] per-slot fan-out at slot+0xDF79 / slot+0xDF7D.
        // Originally added to mimic main_game_loop's prologue, but IDA
        // xref scan reveals those fields are DirectPlay-specific:
        //   slot+0xDF7D = g_p1_input_buffer_index_field — read only by
        //     check_game_continue (DirectPlay packet handler, no-op
        //     when g_directplay_interface == NULL, which is always the
        //     case for us)
        //   slot+0xDF79 = g_net_sync_frame_counter — written/read only
        //     by check_game_continue
        // Neither KGT scripts nor update_game/process_game_inputs read
        // these fields. Writing them every frame did nothing useful and
        // interfered with adjacent memory the StudioS chars apparently
        // touch. Per-slot fan-out also removed from spectator + GekkoNet
        // paths (same reason — DirectPlay isn't used anywhere).

        // ---- Frame-skip: decouple sim from render (the fix for the Robot
        // Heroes heavy-stage slowdown). The trampoline replaced FM2K's native
        // main_game_loop @ 0x405AD0, which is a "100fps fixed timestep WITH
        // frame skipping": it runs process_inputs+update_game as many 10ms
        // steps as real time demands (capped at 10 = spiral-of-death guard)
        // and calls render_game ONCE per loop. Our offline tick rendered every
        // sim tick, so a heavy stage's ~13ms render dragged the SIM itself to
        // ~70fps -- the game ran at ~70% speed (Yamada's report; render-bound).
        // Catch-up ticks here hold the sim at 100fps; only DISPLAY frames drop
        // on heavy stages, exactly like vanilla. Determinism/netcode-safe: the
        // sim sequence is byte-identical (same inputs, same order; render never
        // feeds the sim), and the netplay + spectator paths above never reach
        // this offline-only branch.
        // [FM2K-DIAG] g_round_end_flag / score globals below are FM2K-only;
        // FM95 equivalents are unmapped (RE-2 in docs/FM95_Support_Status.md).
        // The reads fold to 0 on FM95 so the trip-warnings stay inert there.
        constexpr uintptr_t REF_ADDR = 0x424718;  // FM2K g_round_end_flag (FM95: RE-2)
        const uint64_t qpc_freq = SDL_GetPerformanceFrequency();
        const uint64_t step_qpc = qpc_freq / 100;  // 10ms = one 100fps step
        static uint64_t s_logical_qpc = 0;
        const uint64_t now_qpc = SDL_GetPerformanceCounter();
        if (s_logical_qpc == 0) s_logical_qpc = now_qpc - step_qpc;  // first tick
        int sim_steps = step_qpc ? (int)((now_qpc - s_logical_qpc) / step_qpc) : 1;
        if (sim_steps < 1)  sim_steps = 1;
        if (sim_steps > 10) { sim_steps = 10; s_logical_qpc = now_qpc - 10 * step_qpc; }
        s_logical_qpc += (uint64_t)sim_steps * step_qpc;

        if (sim_steps > 1) {
            static uint32_t s_skip_log = 0;
            static const bool s_perf = []{ const char* v = std::getenv("FM2K_PERF_PROFILE");
                                           return v && v[0] == '1'; }();
            if (s_perf && (s_skip_log++ % 100) == 0)
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[FRAMESKIP] sim caught up %d ticks for 1 render (heavy stage)",
                    sim_steps);
        }

        uint64_t sim_ns = 0;
        for (int _sf = 0; _sf < sim_steps; ++_sf) {
            const uint32_t ref_before = FM2K::kIsFM2K ? *(volatile uint32_t*)REF_ADDR : 0;
            const int32_t  pre_score = FM2K::kIsFM2K ? *(int32_t*)0x470050 : 0;
            const uint32_t pre_ref   = FM2K::kIsFM2K ? *(uint32_t*)0x424718 : 0;
            const uint32_t pre_mode  = *(uint32_t*)FM2K::ADDR_GAME_MODE;
            const uint64_t _s0 = SDL_GetPerformanceCounter();
            if (original_process_game_inputs) original_process_game_inputs();
            const uint32_t ref_after_pgi = FM2K::kIsFM2K ? *(volatile uint32_t*)REF_ADDR : 0;
            if (ref_after_pgi != ref_before && ref_after_pgi != 0)
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[REF-TRIP] round_end_flag flipped %u->%u during PGI",
                    ref_before, ref_after_pgi);
            if (original_update_game) original_update_game();
            ++g_sim_step_count;   // sim-fps: one logic tick (offline frame-skip)
            const uint64_t _s1 = SDL_GetPerformanceCounter();
            sim_ns += _s1 - _s0;
            const uint32_t ref_after_ug = FM2K::kIsFM2K ? *(volatile uint32_t*)REF_ADDR : 0;
            if (ref_after_ug != ref_after_pgi && ref_after_ug != 0)
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[REF-TRIP] round_end_flag flipped %u->%u during UG",
                    ref_after_pgi, ref_after_ug);
            const int32_t post_score = FM2K::kIsFM2K ? *(int32_t*)0x470050 : 0;
            if (pre_mode >= 3000 && pre_mode < 4000) {
                if (pre_score >= 0 && post_score < 0)
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[CASE200-TRIP] score path fired: %d -> %d", pre_score, post_score);
                if (pre_ref != 0)
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[CASE200-TRIP] round_end_flag path fired: pre_ref=%u", pre_ref);
            }
            ParityRecorder::Capture();   // per confirmed sim frame
        }

        g_render_rand_calls = 0;
        const uint64_t _r0 = SDL_GetPerformanceCounter();
        RenderFrameWithSnapshot();       // ONCE per loop, regardless of sim_steps
        const uint64_t _r1 = SDL_GetPerformanceCounter();
        MaybeLogOfflineSections(0, sim_ns, _r1 - _r0, g_render_rand_calls);
        return;
    }

    // Networked path: wait for remote peer to also enter battle mode, then
    // start GekkoNet.
    extern bool g_battle_entry_signaled_pub;
    if (g_battle_entry_signaled_pub && !Netplay_IsActive()) {
        Netplay_PollBattleSync();
        if (!Netplay_IsBattleSynced()) {
            return;  // still waiting for remote
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Trampoline: battle sync complete, starting GekkoNet");
        if (!Netplay_StartBattle()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Trampoline: Netplay_StartBattle failed");
        }
    }

    if (Netplay_IsActive()) {
        // ---- Netplay sim/render decouple (heavy-stage frame-skip, the
        // netplay analog of the offline fix). Step gekko at a fixed 100fps
        // cadence -- one FULL add->update->advance cycle
        // (Netplay_ProcessBattleInputPhase) per 10ms of real time -- and
        // render ONCE per outer iteration. On a heavy stage where render
        // exceeds the 10ms budget (~13ms = ~77fps), the accumulator runs the
        // gekko step 1-2x per render so the SIM holds 100fps while only
        // DISPLAY frames drop.
        //
        // Determinism: unlike the old in-function catch-up (N local adds
        // before ONE update_session -- broke gekko's frame accounting,
        // desynced at RoHe/Aubeclisse f139), each step here is a complete
        // add->update->advance. The sim's clock is virtual (Hook_timeGetTime
        // returns g_virtual_time_ms = frame*10, frame-derived), so a peer
        // running an extra step only deepens PREDICTION, which gekko's
        // rollback reconciles -- the same mechanism runahead already uses. No
        // wall-clock value reaches the sim. FM2K_NO_NETPLAY_CATCHUP=1 forces
        // single-step (the pre-decouple behavior).
        // DEFAULT OFF (2026-06-13). The netplay catch-up holds heavy-stage sim
        // at 100fps, but it fundamentally fights gekko's frame-advantage model:
        // running extra gekko cycles off one peer's wall-clock pushes that peer
        // ahead, the 1.6% time-sync slowdown can't pull it back, so FA pins near
        // the prediction window (+/-13) and that peer rolls back pathologically
        // (2500-frame soak: P2 ~1100 rollbacks vs P1's 12, FA +13.8 vs -13.6).
        // The render-gate stops it engaging on LIGHT stages, but on genuinely
        // heavy stages it still imbalances. Render-bound single-step (both peers
        // equally slow, FA balanced, no one-sided rollback) beats that. Offline
        // frame-skip is unaffected (no gekko / no FA). Opt in for experiments
        // only: FM2K_NETPLAY_CATCHUP=1. A future gekko-paced design could revive
        // it without the imbalance.
        static int s_np_catchup = -1;
        if (s_np_catchup < 0) {
            const char* on = std::getenv("FM2K_NETPLAY_CATCHUP");
            s_np_catchup = (on && on[0] == '1') ? 1 : 0;
        }
        const uint64_t freq     = SDL_GetPerformanceFrequency();
        const uint64_t step_qpc = freq / 100;                // 10ms = one 100fps step
        static uint64_t s_logical_qpc     = 0;
        static uint64_t s_last_render_qpc = 0;               // duration of the previous battle render
        const uint64_t now_qpc = SDL_GetPerformanceCounter();
        if (s_logical_qpc == 0) s_logical_qpc = now_qpc - step_qpc;

        int steps = 1;
        // RENDER-GATED catch-up: only run multiple gekko cycles when the
        // PREVIOUS render genuinely blew the 10ms frame budget (a heavy stage).
        // On a light stage render fits, so we force steps=1 and keep the
        // accumulator pinned one step behind now -- otherwise it banks gekko's
        // RTT-driven frame-advantage slowdown (the intentional ~1.6% when ahead)
        // as wall-clock debt and "corrects" it with a periodic double gekko-step,
        // a 1-frame lurch felt as choppiness on EVERY normal netplay match
        // (confirmed: spurious NET-FRAMESKIP on a light stage under clumsy RTT).
        // Single-stepping light stages is byte-identical to the pre-decouple
        // path. FM2K_NO_NETPLAY_CATCHUP=1 disables catch-up outright.
        if (s_np_catchup == 1 && step_qpc && s_last_render_qpc > step_qpc) {
            steps = (int)((now_qpc - s_logical_qpc) / step_qpc);
            if (steps < 1) steps = 1;
            if (steps > 4) { steps = 4; s_logical_qpc = now_qpc - 4 * step_qpc; }
            s_logical_qpc += (uint64_t)steps * step_qpc;
        } else {
            s_logical_qpc = now_qpc - step_qpc;              // pinned: no banked drift on light stages
        }
        // Run up to `steps` gekko cycles, but stop early if one didn't advance
        // (lockstep stall waiting on remote input) -- spinning would not help
        // and would over-predict.
        int taken = 0;
        for (int s = 0; s < steps; ++s) {
            if (!Netplay_ProcessBattleInputPhase()) break;
            ++taken;
        }
        if (steps > 1 && taken > 1) {
            static uint32_t s_skip_log = 0;
            static const bool s_perf = []{ const char* v = std::getenv("FM2K_PERF_PROFILE");
                                           return v && v[0] == '1'; }();
            if (s_perf && (s_skip_log++ % 100) == 0)
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[NET-FRAMESKIP] sim ran %d gekko steps for 1 render (heavy stage)", taken);
        }
        if (g_frame_pending_render) {
            const uint64_t _r0 = SDL_GetPerformanceCounter();
            RenderFrameWithSnapshot();
            s_last_render_qpc = SDL_GetPerformanceCounter() - _r0;
            g_frame_pending_render = false;
        }
        // Drift adjustment now lives in SleepToTarget at the outer
        // loop (see RunBattleTick comment in TRAMPOLINE_BATTLE case).

        // [BEAT] heartbeat — rate-limited to ~10s wall-clock internally,
        // safe to call every tick. Single INFO line with role/ping/jitter/
        // FA/delay/ra/pred/rollback stats for support triage.
        Netplay_TickHeartbeat();

        // Battle-exit swap-frame gate. Once CheckGameModeTransition has
        // detected game_mode leaving battle range it called
        // Netplay_SignalBattleEnd() which sent BATTLE_END(swap_frame).
        // Both peers + any spectators wait until they reach that frame
        // before tearing down the GekkoNet battle session, so the swap
        // is observed at the same logical point on every node.
        Netplay_PollBattleEndSync();
        bool end_ready = Netplay_IsBattleEndSynced();
        if constexpr (FM2K::kIsFM95) {
            // FM95: both peers agreed on ONE deterministic swap_frame (derived
            // from the confirmed battle-end edge, ahead of both live frames).
            // Actually WAIT until our sim reaches it before tearing down, so
            // both nodes swap at the identical frame and the drain never orphans
            // frames. FM2K keeps its both-signaled semantics (detection there is
            // already frame-deterministic via the game_mode scalar).
            if (end_ready && Netplay_GetFrame() < Netplay_GetBattleEndSwapFrame()) {
                end_ready = false;
            }
        }
        if (end_ready) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Trampoline: battle-end swap_frame reached — destroying battle session");
            Netplay_EndBattle();
        }
    }
}

