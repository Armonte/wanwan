// trampoline_spectator.cpp -- spectator playback (RunSpectatorTick + SpectatorSimOneFrame + queue constants + g_spec_pop_total). Split from main_loop_trampoline.cpp.
#include "main_loop_trampoline.h"
#include "globals.h"
#include "../hooks/hooks.h"
#include "../hooks/wndproc_subclass.h"
#include "../hooks/facing_trace.h"    // Phase 4b [FACING] ring: match-boundary dump on the spectator plane
#include "../netplay/netplay.h"
#include "../netplay/control_channel.h"
#include "../netplay/game_hash.h"
#include "../netplay/spectator_node.h"
#include "../netplay/spec_css_tripwire.h"  // battle-frame-0 parked-VM detector
#include "../netplay/spec_relatch.h"   // viewer g_round_limit re-derive + its battle-frame-0 tripwire
#include "../ui/shared_mem.h"
#include "../parity/parity_recorder.h"
#include "../parity/parity_pool.h"    // player-slot resolution + pool-topology fencepost term
#include "../netplay/savestate.h"   // RegionChecksums + gameplay_fingerprint (GAP #1 fencepost)
// SaveState_CalculateFingerprint is engine-internal (savestate_internal.h) but externally linked;
// forward-declared so the spectator can RECOMPUTE the fingerprint from its own live FM2K memory --
// SaveState_Save never runs on a spectator, so the stored ring-slot checksum / CRCs are stale here.
uint32_t SaveState_CalculateFingerprint();
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include "trampoline_internal.h"

// Spectator playback -- pure input-replay model.
//
// CONTRACT: spectator's local FM2K only advances sim when it has a confirmed
// (p1, p2) input pair from the host for the next frame. Pop one → run one
// frame → render → wait for the next pair. That's it.
//
// Why this works: every change to FM2K's state is input-driven from a
// canonical default state at boot. Host records every confirmed (p1, p2)
// it returns from Hook_GetPlayerInput (title-screen auto-mash, CSS cursor
// moves, battle commands -- the whole stream). Spectator drains the same
// stream → identical state by construction.
//
// Three regimes by queue depth:
//   QUEUE EMPTY                 → freeze, render last frame, wait
//   QUEUE >= LIVE_TARGET        → pop one, run one, render (live edge)
//   QUEUE > FF_ENTER (catch-up) → outer loop drops to 1 ms sleep so we
//                                 burn through backfill at ~1000 Hz
//
// Pre-JOIN_ACK: queue is empty, no sim runs, screen holds the boot snapshot.
// JOIN_ACK fires SendSessionBackfillFromStart from the host → queue fills with
// every confirmed frame from session start → spectator drains those at
// FF speed → catches up to live edge.
// Jitter cushion: hold playback until this many frames are buffered,
// and rebuild the cushion after a drain-out. 8 (80ms) was a netplay-
// grade number -- the spec rode the live edge and every production
// hiccup hit the picture (q racing 10 -> 1 -> 0). A spectator trades
// 400ms of extra delay for smoothness; the starvation bypass caps any
// hold at 250ms of silence so this can never become a freeze.
constexpr size_t SPECTATOR_LIVE_TARGET = 40;
constexpr size_t SPECTATOR_FF_ENTER    = 100; // ~1 sec of host frames queued
constexpr size_t SPECTATOR_FF_EXIT     = 16;  // hysteresis: 2x live target

// Fast catch-up target (C5.5). Once pb_queue depth drops to or below this,
// we revert to one-pop-per-trampoline-tick paced render. Set higher than
// SPECTATOR_LIVE_TARGET (jitter floor) so the catch-up loop doesn't oscillate
// across the floor on every tick. ~1 sec at 100 Hz = 100 frames of buffered
// headroom is enough to absorb host jitter without lagging the viewer
// noticeably behind real-time.
constexpr size_t SPECTATOR_LIVE_LAG_FRAMES = 100;

// Broadcast delay target: the spectator deliberately sits this many
// frames behind live (default 3s at 100fps). Riding the live edge made
// playback arrival-limited -- every loss burst or host stall hit the
// picture as a stall (q=0, pops=0). With the bank, any arrival gap
// shorter than the bank is structurally invisible; drains target the
// bank, never the edge. Single source of truth lives in spectator_node
// (Phase G: FM2K_SPEC_DELAY floor + adaptive growth from measured
// inter-admission gaps, so links worse than the default profile widen
// their own cushion instead of stalling).
inline size_t SpectatorTargetDelayFrames() {
    return SpectatorNode_TargetDelayFrames();
}

// One sim step (popped INPUT → PGI → UG → diagnostics). Returns false if
// the queue couldn't supply an INPUT (head non-INPUTs were drained but no
// INPUT followed them), true if a frame was simulated. Render is the
// caller's responsibility.
uint32_t g_spec_pop_total = 0;

// task #60: symmetric steady-state pacing accumulator (negative = we ran
// ahead of the wall clock; skip-gate consumes it as render-only ticks).
static double g_spec_steady_accum = 0.0;

// task #60: one-shot handoff from ApplyPendingSnapshot. When a battle
// snapshot applies, it runs initial-sync + PIN_RNG itself (init must
// precede the overlay) and sets this so the NEXT first-3000-iteration
// skips its own init instead of clobbering the applied state (para's
// wild 2.82 session: apply at :08.617, init at :08.666 -> viewer
// replayed real tail inputs over a fresh-boot battle). Consumed once;
// later battles' entries init normally.
bool g_spec_skip_next_battle_init = false;

static bool SpectatorSimOneFrame() {
    uint16_t p1 = 0, p2 = 0;
    if (!SpectatorNode_PopFrameInputs(&p1, &p2)) {
        return false;
    }
    ++g_spec_pop_total;

    // PER-FRAME alignment-trace log: capture RNG before PGI runs so we can
    // pair with [HOST-TRACE] on the host. If host's bf=N rng_pre matches
    // spectator's bf=N rng_pre, alignment is correct. If they diverge, the
    // leak is between frame N-1 post-UG and frame N pre-PGI (inter-frame).
    // If they match but rng_post diverges, leak is inside PGI+UG itself.
    static uint32_t s_spec_trace_bf = 0;
    static bool     s_spec_trace_in_battle = false;
    const uint32_t mode_pre = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    const uint32_t rng_pre_pgi_spec = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    if (mode_pre >= 3000 && mode_pre < 4000) {
        if (!s_spec_trace_in_battle) {
            s_spec_trace_in_battle = true;
            s_spec_trace_bf = 0;
            specnode::SpecCssTripwire_OnBattleFrameZero(); specnode::SpecRelatch_OnBattleFrameZero();  // always-on tripwires (1000-line cap: one line)
            if (g_spec_skip_next_battle_init ||
                SpectatorNode_SnapshotAppliedOnce()) {
                // #60 + spectate rng-desync fix: this battle's entry state came
                // from an applied snapshot. The savestate RESTORED the
                // authoritative gameplay seed (0x41FB1C) + object pool for the
                // snapshot's frame -- rerunning DoInitialSync/PIN_RNG here
                // would CLOBBER that seed with the stale 0x12345678 frame-0
                // seed, offsetting the spectator's entire battle rng (root of
                // the intermittent spectate desync). Skip. The durable
                // pb_snapshot_applied_once check is robust to the CSS-drive vs
                // ApplyPendingSnapshot ordering that made the one-shot
                // g_spec_skip flag miss under loss.
                g_spec_skip_next_battle_init = false;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpecSim: first mode_pre==3000 iteration -- entry init "
                    "already sequenced at snapshot apply, skipping (snap=%d)",
                    (int)SpectatorNode_SnapshotAppliedOnce());
            } else {
                // First iteration where mode_pre==3000 = first FULL battle
                // frame on the spec side (mode_pre==3000 at iteration start,
                // mode_pre==3000 still at end; PGI/UG ran entirely in battle
                // context). Apply PIN_RNG + initial-sync BEFORE PGI here so
                // host's first battle frame post-PGI rng matches replay's.
                SaveState_DoInitialSync();
                SpectatorNode_ApplyPendingPinRng();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpecSim: first mode_pre==3000 iteration -- applied "
                    "initial-sync + PIN_RNG pre-PGI");
            }
        }
    } else {
        if (s_spec_trace_in_battle) {
            // Battle -> non-battle edge (match end -> results/CSS). Clear the
            // snapshot-applied suppression so the NEXT battle entry (a rematch,
            // match 2+, which a continuing spectator always enters from-scratch)
            // runs its own DoInitialSync+PIN_RNG. Without this the durable flag
            // set by match 1's applied snapshot wrongly skips match 2's init ->
            // match-2 rng desync (run_all_tests multi-match-e2e S2 seg1).
            SpectatorNode_ClearSnapshotAppliedForNextBattle();
            FacingTrace_Dump("spectator match end");  // between matches, not mid-match
        }
        s_spec_trace_in_battle = false;
    }

    if (original_process_game_inputs) original_process_game_inputs();
    if (original_update_game)         original_update_game();
    ++g_sim_step_count;   // sim-fps: one logic tick

    ParityRecorder::Capture();

    // [CSS-FP] parity emit during CSS (#66 Phase 1) -- the spectator's applied
    // CSS frame. Pairs with the host/guest RunCssTick emit so the harness can
    // align the three streams by confirmed input and prove the spectator's
    // replayed CSS matches the players' bit-for-bit. Frame counter is local
    // ordering only; the harness aligns by the (p1,p2) input sequence.
    {
        const uint32_t mode_css = *(uint32_t*)FM2K::ADDR_GAME_MODE;
        if (mode_css >= 2000 && mode_css < 3000) {
            static uint32_t s_spec_css_frame = 0;
            Netplay_EmitCssFp(s_spec_css_frame++, p1, p2);
        }
    }

    // [SPEC-TRACE] per-frame for first 100 battle frames. Routed through
    // SDL_LOG_CATEGORY_CUSTOM so LogOutputFunction sends it to quill's
    // backtrace ring instead of disk -- captured forever-cheap, only
    // flushed to disk on a subsequent LOG_ERROR. FM2K_SPECTATOR_DEBUG=1
    // routes CUSTOM to disk like a normal log line for verbose sessions.
    if (s_spec_trace_in_battle && s_spec_trace_bf < 100) {
        SDL_LogInfo(SDL_LOG_CATEGORY_CUSTOM,
            "[SPEC-TRACE] bf=%u rng_pre=0x%08X rng_post=0x%08X "
            "p1=0x%03X p2=0x%03X",
            s_spec_trace_bf, rng_pre_pgi_spec,
            *(uint32_t*)FM2K::ADDR_RANDOM_SEED, p1, p2);
        s_spec_trace_bf++;
    }

    // Per-frame state fingerprint log for desync diagnosis. Counts every
    // simulated frame (catch-up included). Logs once we're in battle
    // (mode 3000) at battle-frame multiples of 30 (~3x per sec). Pairs
    // with the host's matching log at netplay.cpp's AdvanceEvent recording
    // site -- grep both .log files for the same sim_frame to find first
    // divergence.
    {
        static uint32_t s_pop_count = 0;
        ++s_pop_count;
        static uint32_t s_battle_frame_at_entry = 0;
        static bool s_in_battle = false;
        const uint32_t mode_now = *(uint32_t*)FM2K::ADDR_GAME_MODE;
        if (mode_now >= 3000 && mode_now < 4000) {
            if (!s_in_battle) {
                s_in_battle = true;
                s_battle_frame_at_entry = s_pop_count;
            }
            const uint32_t bf = s_pop_count - s_battle_frame_at_entry;
            // [CINPUT] every-frame APPLIED input (bf-keyed) -- the harness's
            // ground-truth desync detector. The spectator's applied input per
            // battle frame is compared against the players' confirmed [CINPUT];
            // a frame-misaligned input (the in-battle position-desync the rng/hp
            // gate is blind to) shows up as a bf where the aligned inputs differ.
            static const bool s_cinput = []{
                const char* v = std::getenv("FM2K_CINPUT");
                return v && v[0] == '1';
            }();
            if (s_cinput) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[CINPUT] bf=%u p1=0x%03X p2=0x%03X", bf, p1, p2);
                // [CHECKSUM] -- full-state fencepost: the SAME gameplay_fingerprint
                // (HP/pos/rng/timer) the players emit at their save event, RECOMPUTED
                // here from the spectator's live FM2K memory. CalculateFingerprint()
                // reads rng/hp/timers/ring-inputs directly (same addresses the host
                // hashes) -- NOT the stale ring-slot checksum (SaveState_Save never
                // runs on a spectator, so GetLastChecksum/GetRegionChecksums would be
                // zero/stale -> the "no overlap" bug). The spectator never rolls back,
                // so this is always the CONFIRMED state -> compared against the host's
                // dedupe-by-last confirmed CRC catches ANY divergence.
                uint32_t spec_fp = SaveState_CalculateFingerprint();
                // top=/nobj= -- POOL-TOPOLOGY term. The crc above covers
                // rng/HP/timers/ring-inputs and is structurally blind to WHICH
                // objects exist, of what kind, bound to whom, at which slot
                // indices -- carry-state family A1, the class the deep-join work
                // uncovered, and the reason a "FULL-STATE IDENTICAL" verdict could
                // coexist with a whole-match pool-slot displacement. Emitted
                // ALONGSIDE crc and never folded into it: on the player side the
                // same fingerprint value is handed to GekkoNet as the P1-vs-P2
                // desync checksum, so folding would change netplay behaviour.
                // Process-independent by construction (parity_pool.h); top=0 means
                // not computed (FM95, or FM2K_CK_TOPOLOGY=0) -- the harness
                // treats that sentinel as a FAILED fatal term, never as a match.
                // tv=2 tags the 4c top=/bind= SPLIT so a pre-4c log (which
                // carries the retired combined digest under the same name) is
                // skipped rather than re-gated red. See review A4a.
                const ParityPool::Topology spec_top = ParityPool::ComputeTopology();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[CHECKSUM] f=%u crc=0x%08X tv=2 top=0x%08X bind=0x%08X nobj=%u",
                    bf, spec_fp, spec_top.digest, spec_top.binding,
                    spec_top.active_count);
            }
            // [SPEC-FP] every 30 battle frames (~3 lines/sec). Routed
            // through SDL_LOG_CATEGORY_CUSTOM so LogOutputFunction puts
            // it in quill's backtrace ring rather than always-on disk.
            // Last 4096 such lines stay in memory; auto-flushed to file
            // when any LOG_ERROR fires (great for desync post-mortems).
            // FM2K_SPECTATOR_DEBUG=1 routes CUSTOM to disk for live tail.
            // [FM2K-DIAG] FM2K-layout fingerprint: global HP scalars + pool
            // slot fields at FM2K offsets. FM95 has no global HP scalar (HP
            // lives at pool slot +72 -- RE-5 wiring) and its round/timer
            // globals are unmapped (RE-2); port with the FM95 spectator
            // (Gap-4d in docs/FM95_Support_Status.md).
            if constexpr (FM2K::kIsFM2K) if ((bf % 30) == 0) {
                const uint32_t rng     = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
                const uint32_t buf_idx = *(uint32_t*)FM2K::ADDR_INPUT_BUFFER_INDEX;
                const uint32_t p1_hp   = *(uint32_t*)0x4DFC85;  // FM2K P1 HP scalar
                const uint32_t p2_hp   = *(uint32_t*)0x4EDCC4;  // FM2K P2 HP scalar
                const uint32_t timer   = *(uint32_t*)FM2K::ADDR_GAME_TIMER;
                // Resolve the fighters by SCAN, not by fixed pool slot.
                // create_game_object allocates first-free, so slot 1 is
                // routinely a system object: the fixed-index read produced 37+
                // consecutive bogus p2_pos=(320,150) p2_script=0 samples for a
                // whole match in the Wave 4.1 sweep and flipped the harness
                // verdict to FAIL while every one of those frames was
                // full-state identical by CRC. slots=(a,b) is appended so any
                // future resolution artifact is self-diagnosing; -1 = no
                // character object for that player right now (pre-spawn), in
                // which case pos reads (0,0) and script reads -1.
                const ParityPool::PlayerView p1v = ParityPool::ReadPlayer(0);
                const ParityPool::PlayerView p2v = ParityPool::ReadPlayer(1);
                SDL_LogInfo(SDL_LOG_CATEGORY_CUSTOM,
                    "[SPEC-FP] bf=%u (pop=%u) rng=0x%08X buf=%u "
                    "p1_hp=%u p2_hp=%u timer=%u "
                    "p1_pos=(%d,%d) p2_pos=(%d,%d) "
                    "p1_script=%d p2_script=%d "
                    "p1_in=0x%03X p2_in=0x%03X catchup=%d slots=(%d,%d) "
                    "rend=%u sim=%u",  // Phase 4b: render:sim ratio (4a rank 4)
                    bf, s_pop_count, rng, buf_idx,
                    p1_hp, p2_hp, timer,
                    p1v.pos_x, p1v.pos_y, p2v.pos_x, p2v.pos_y,
                    p1v.script, p2v.script,
                    p1, p2, (int)g_spectator_catchup,
                    p1v.slot, p2v.slot,
                    *(uint32_t*)0x4456FC, (uint32_t)g_sim_step_count);
            }
        } else {
            s_in_battle = false;
        }
    }

    // Advance the deterministic virtual clock per simulated frame so
    // Hook_timeGetTime stays consistent with sim progression even when
    // we're inside the catch-up loop.
    extern uint32_t g_virtual_time_ms;
    g_virtual_time_ms += 10;

    return true;
}

void RunSpectatorTick() {
    Hook_CheckGameModeTransition_Public();

    // Drain UDP -- control channel (0xCC: SPEC_JOIN_ACK / HEARTBEAT) and
    // 0xCE INPUT_BATCH datagrams land on the same multiplex socket.
    ControlChannel_Poll();

    // Health: heartbeat send, silence-failover, daisy-chain reconnect.
    SpectatorNode_TickHealth();

    // Apply any deferred SNAPSHOT_END that landed before the local engine
    // had finished its pre-WinMain init. Snapshot blobs that arrive within
    // ms of the spectator process starting (very common with the host
    // sitting in a fast loopback connect) will land in pb_snapshot_inbox
    // with pending_apply=true; this drains them on the first spec tick
    // where game_mode != 0. No-op when nothing is pending.
    SpectatorNode_ApplyPendingSnapshot();

    // Cache offline-replay mode once. Several gates downstream behave
    // differently for replay vs. live spec -- replay has no upstream so
    // jitter-floor / catchup logic is meaningless.
    static int s_offline_replay_cached = -1;
    if (s_offline_replay_cached < 0) {
        const char* rp = std::getenv("FM2K_REPLAY_FILE");
        s_offline_replay_cached = (rp && rp[0]) ? 1 : 0;
    }
    const bool s_offline_replay_env_active = (s_offline_replay_cached == 1);

    const size_t qd = SpectatorNode_PendingFrameCount();

    // END-OF-STREAM DRAIN. Once the upstream announced SESSION_END the host is
    // gone: there is nothing left to buffer AGAINST, so the delay bank has no
    // purpose from here on. Leaving the below-bank slowdown armed throttles the
    // final drain to as little as 40% exactly while the graceful-exit path is
    // waiting on PendingFrameCount()==0 -- and the 8s host-gone watchdog above
    // then kills the viewer mid-drain, which is the sweep's "ended N frames
    // behind the host" fingerprint. Play out what we hold instead. Live-only:
    // offline replay has its own drain-exit below and must stay strict 1:1.
    const bool stream_over = !s_offline_replay_env_active &&
                             SpectatorNode_SessionEnded();

    // [SPEC-Q] 1Hz heartbeat: phase + queue depth + pop counters. The
    // [SPEC-FP] trace only logs in battle (mode>=3000), leaving the
    // match-boundary CSS walk a blind spot -- the multi-match journey
    // stalls there and nothing said why.
    {
        static uint64_t s_last_q_log = 0;
        static uint32_t s_pops_window = 0;
        extern uint32_t g_spec_pop_total;
        static uint32_t s_pop_last = 0;
        const uint64_t now = GetTickCount64();
        if (now - s_last_q_log >= 1000) {
            const uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
            s_pops_window = g_spec_pop_total - s_pop_last;
            s_pop_last = g_spec_pop_total;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-Q] mode=%u q=%zu pops/s=%u total=%u catchup=%d",
                mode, qd, s_pops_window, g_spec_pop_total,
                (int)g_spectator_catchup);
            s_last_q_log = now;
        }
    }

    // The no-forward-progress watchdog (host gone OR wedged), the graceful
    // stream-end drains, the SELF-STALL credit + post-stall rung catch-up and
    // the connect-establishment deadline all live in
    // trampoline_spec_watchdog.cpp -- one concern, one TU (the 1000-line rule).
    SpecWatchdogTick(s_offline_replay_env_active);

    // Jitter-buffer floor only applies to LIVE spec (waiting on host's next
    // batch). Offline replay has no upstream -- the file is the entire input
    // stream -- so the last ≤8 events (typically post-match INPUTs +
    // MATCH_END) would be stranded under the LIVE floor. Drain to zero
    // instead, which lets MATCH_END apply and the playback finishes cleanly.
    //
    // Boundary bypass (Phase F): the floor must not strand a queued
    // boundary op or freeze an active SEAM/PINNING walk. With the UDP
    // accelerator the spec rides AT the live edge, so the stream pause at
    // a match seam arrives with q < floor -- the old unconditional freeze
    // trapped MATCH_END behind the last tail inputs forever (q:7 zombie,
    // 2026-06-11). If any non-INPUT op is queued, drain toward it; if a
    // boundary is active, the synthetic feed must keep ticking even at
    // q == 0 (it consumes nothing while walking results/CSS).
    // Starvation bypass: the floor exists to absorb jitter, not to
    // withhold playable frames during a genuine stream pause. q=7 (one
    // below the floor) used to freeze the picture for the entire pause
    // with seven frames in hand (2026-06-11). If nothing has been
    // admitted for >250ms, play out what we hold at 1:1.
    const bool boundary_bypass =
        SpectatorNode_InBoundary() ||
        SpectatorNode_InNaturalBootWalk() ||
        (qd > 0 && qd < SPECTATOR_LIVE_TARGET &&
         SpectatorNode_QueueHasPendingOp());
    if (qd < SpectatorTargetDelayFrames() &&
        !s_offline_replay_env_active && !boundary_bypass && !stream_over) {
        // BANK MAINTENANCE: smoothly slow the playout IN PROPORTION to how far
        // the buffer is below target, instead of slamming to a binary 50% glide.
        // A spectator is a jitter-buffered video player: paces to the host's
        // actual production rate (so a heavy-stage / jittery host that confirms
        // inputs below 100fps doesn't drain the bank into a hard stall) while
        // keeping the slow-down INVISIBLE during normal dips. Near target the
        // skip fraction is ~0 (imperceptible); it ramps only as the buffer
        // genuinely empties. Replaces the old jarring every-other-frame glide.
        const size_t tgt = SpectatorTargetDelayFrames();
        const double deficit = 1.0 - (double)qd / (double)(tgt ? tgt : 1);  // 0..1
        // skip_rate = deficit^2 (gentle near target, stronger near empty),
        // capped at 0.6 so playout never crawls below ~40% even near q=0.
        const double skip_rate = deficit * deficit * 0.6;
        static double s_skip_accum = 0.0;
        s_skip_accum += skip_rate;
        if (qd == 0 || s_skip_accum >= 1.0) {
            if (s_skip_accum >= 1.0) s_skip_accum -= 1.0;
            RenderFrameWithSnapshot();   // hold this frame, don't advance the sim
            return;
        }
        // fall through: sim exactly one frame this tick (full speed)
    }
    if (qd == 0 && !boundary_bypass) {
        // Truly empty -- even offline replay has nothing left to do.
        //
        // For offline replay (FM2K_REPLAY_FILE set): the entire file was
        // loaded into pb_queue at startup; queue==0 + playing_back==false
        // means the last MATCH_END drained and there are no further
        // events. Without an exit here the replay viewer freezes on the
        // last rendered frame indefinitely. ExitProcess from a hook is
        // aggressive but the launcher spawned us specifically for replay
        // playback -- when playback ends, the process is done.
        //
        // Live spec falls through to RenderFrameWithSnapshot -- peer might
        // still send more events (next match) and we want to stay alive. The
        // host-gone/wedged case is handled by the no-forward-progress watchdog
        // above (MsSinceLastAdmit), which fires even when the queue is NOT empty
        // (the old queue-empty check here missed the BATTLE_END-spam wedge).
        if (s_offline_replay_env_active &&
            !SpectatorNode_IsPlayingBack()) {
            static std::atomic<bool> s_exit_armed{false};
            if (!s_exit_armed.exchange(true)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: offline replay drained -- exiting process");
                ExitProcess(0);
            }
        }
        RenderFrameWithSnapshot();
        return;
    }

    // Catch-up inner loop (C5.5). When the queue is far ahead of the
    // jitter floor, drain multiple sim steps per trampoline tick to close
    // the gap to live edge -- without this, late join replays from session
    // start at 1 frame per tick and a 30k-event backlog takes 5 minutes
    // to drain. Audio dispatch and render are gated on g_spectator_catchup
    // so we don't blast compressed audio or burn render bandwidth during
    // the burn-down.
    //
    // Network poll interleave: WITHOUT this, the inner loop holds the
    // trampoline thread for 100s of ms processing thousands of events, and
    // the reliable channel is never serviced -- acks stop going out, the
    // host's retransmit window fills, and from the spectator's perspective
    // the upstream goes silent.
    //
    // ControlChannel_Poll drains UDP control + the reliable channel (which
    // feeds new EVENT_BATCH2 packets into pb_queue). We poll every
    // iteration during catchup since
    // a single SpectatorSimOneFrame can take 100s of ms when FM2K's
    // CSS-cursor-move triggers a synchronous .player character file
    // load -- much longer than the ~10ms a normal sim tick takes.
    //
    // We do NOT call SpectatorNode_TickHealth here: its recovery ladders
    // would fire during legitimate catchup (we've been "silent" because
    // we're disk-blocked, not because the upstream died) and re-JOIN the
    // very upstream we are draining the backfill from. The top-level
    // RunSpectatorTick path calls TickHealth post-catchup when we're back
    // at the live edge.
    // Catchup policy (post-mvp tuning):
    //
    //   1. ONE-SHOT at join. On the first run where the queue is over the
    //      lag target -- typically right after JOIN_ACK + backfill replay --
    //      we burn through the buffered events to reach the live edge.
    //      After that, even if the buffer grows from network jitter,
    //      packet loss, or a reconnect-with-backfill, we play at normal
    //      1-frame-per-tick speed. Once you've been caught up, you stay
    //      at whatever delay naturally accumulates from later disruption
    //      -- no jarring superspeed snap-to-live.
    //
    //   2. NO AUTOMATIC SAFETY DRAIN. Buffer is allowed to grow if the
    //      spectator falls behind; the user explicitly asked us not to
    //      speed up after lag/loss recovery. Memory cost is bounded by
    //      the sender (host TCP send blocks via backpressure if our
    //      kernel recv buffer fills) -- not by us auto-draining.
    //
    //   3. ENV OVERRIDE. FM2K_SPECTATOR_ALWAYS_CATCHUP=1 keeps the
    //      always-catch-up behaviour for users who explicitly want low-
    //      latency live viewing.
    //
    // Per-tick wall-clock bound: catchup yields after CATCHUP_MAX_BURST_MS
    // so a single SpectatorSimOneFrame blocking on a .player disk load
    // (50–500ms cold cache) can't dominate the trampoline thread.
    constexpr uint64_t CATCHUP_MAX_BURST_MS = 250;

    static int  s_always_catchup_env = -1;
    if (s_always_catchup_env < 0) {
        const char* v = std::getenv("FM2K_SPECTATOR_ALWAYS_CATCHUP");
        s_always_catchup_env = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
    }
    // Offline-replay mode: catchup is meaningless because the entire file
    // is pre-loaded into pb_queue at startup -- without this gate, the
    // catchup loop drains the whole match at unbounded rate and battle
    // visibly fast-forwards. We always run at 1 frame per outer tick
    // instead, matching live-pace 100fps playback. (Offline-replay env-var
    // detection is hoisted to the top of RunSpectatorTick -- same cached
    // value is reused here.)
    static bool s_initial_catchup_done   = false;
    static bool s_initial_catchup_active = false;
    if (s_offline_replay_env_active) {
        // Permanent disable -- never engage catchup for offline replay.
        s_initial_catchup_done = true;
        s_initial_catchup_active = false;
    }

    // CSS catchup is now allowed. Earlier we suppressed it because catchup
    // ran PGI+UG without render, and CSS character-preview state is heavily
    // render-RNG dependent (cursor flicker, palette interp) -- diverging there
    // bled into battle as mirrored-character desync. Now that render fires
    // inside the catchup loop alongside sim, RNG stays locked to host and
    // CSS can drain like any other phase. The 250ms burst cap still bounds
    // disk-load thrash from cold-cache .player loads (first time through
    // each character on the roster); subsequent matches replay from warm
    // cache and drain at full speed.

    // Initial catchup state machine. Engages once when the queue first
    // exceeds the lag target (typically right after JOIN_ACK + backfill
    // replay), stays engaged across MANY outer ticks -- bounded per-tick by
    // CATCHUP_MAX_BURST_MS so we yield between bursts -- until the queue
    // actually drops to the live edge. Then disengages permanently for
    // this session: later jitter or packet-loss accumulation no longer
    // re-triggers a catchup burst (no jarring superspeed snap-to-live).
    // The whole point is "drain backfill ONCE, smoothly". Without this,
    // the previous one-shot semantics gave us 30 frames of drain and left
    // the other 1100+ buffered, so first battle started 11 seconds behind
    // host with no recovery path short of F12.
    // NEVER for offline replay: the whole .fm2krep sits in pb_queue from boot,
    // so qd is thousands of frames and "catch up to the live edge" is
    // meaningless -- it looks 1:1 through CSS (expensive .player frames bound the
    // burst) then fast-forwards the instant battle's cheap frames begin (user
    // report: "replay speeds up into battle"). Replays play 1:1; F12
    // (needs_user_ff) is the only speed lever. Same carve-out as
    // needs_battle_emergency below.
    if (!s_offline_replay_env_active &&
        !s_initial_catchup_done && !s_initial_catchup_active &&
        qd > SPECTATOR_LIVE_LAG_FRAMES) {
        s_initial_catchup_active = true;
    }
    if (s_initial_catchup_active && qd <= SpectatorTargetDelayFrames()) {
        s_initial_catchup_active = false;
        s_initial_catchup_done   = true;
    }
    const bool needs_initial_catchup = s_initial_catchup_active;
    const bool needs_always_catchup  = s_always_catchup_env != 0 &&
                                       qd > SpectatorTargetDelayFrames();
    // User-toggled FF (F12) is the explicit "speed up to live" lever.
    const bool needs_user_ff         = g_spectator_ff_user &&
                                       qd > SpectatorTargetDelayFrames();
    // CSS-phase catchup: drain the backlog aggressively while the host
    // sits in character select. Battle has no visible action to "fast-
    // forward through" so the user can't tell catchup from steady-state,
    // but if we let the queue accumulate across rounds we'd start the
    // NEXT battle hundreds of frames behind. Engaging during CSS keeps
    // the spec close to live without disrupting the in-battle viewing
    // experience. (Render is gated on g_spectator_catchup so cursor
    // animations still draw -- see RenderFrameWithSnapshot in the loop.)
    const uint32_t live_game_mode    = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    // NO turbo at CSS, ever: hover-triggered .player loads are real
    // deterministic sim work (100-500ms of CPU each) that the host paid
    // at human pace -- any multi-pop burst stalls into a slideshow. The
    // old `needs_css_catchup = false` placeholder is gone; the same policy
    // is now expressed once, as the mode gate on the sustained drain below.
    // Residual CSS backlog clears in battle where frames cost ~0.1ms.
    // Emergency battle drain: entering battle hundreds of frames behind
    // previously had NO recovery path -- lag compounded into a perceived
    // hang. Battle frames are cheap; turbo to the live band. NEVER for
    // offline replay: the whole file sits in the queue from boot, so
    // queue depth is meaningless and this turbo fast-forwarded the
    // entire match (user report 2026-06-11). Replays play 1:1; F12 is
    // the explicit speed lever, and the harnesses opt into fast drain
    // via FM2K_SPECTATOR_ALWAYS_CATCHUP=1.
    const bool needs_battle_emergency = !s_offline_replay_env_active &&
                                        (live_game_mode >= 3000u) &&
                                        qd > SpectatorTargetDelayFrames() + 600;
    // Render parity is required during catchup -- every phase. Render-side
    // game_rand mutations (ProcessShakeEffect mode 4, ProcessColorInterpolation
    // mode 3, particle FX) run on the host once per simulated frame. If the
    // spectator's catchup loop runs PGI+UG N times but renders only once per
    // outer tick, the host advances the RNG by N renders' worth while we
    // advance by 1, and state diverges in proportion to catchup speedup.
    // Symptom: F12-fast-forward in battle desynced HP/positions/scripts even
    // with PIN_RNG synced -- render mutations had been silently dropped for
    // 50+ catchup-loop iterations. Render in the loop costs more GPU but
    // keeps RNG locked to host across CSS, battle, and inter-match drain.
    g_spectator_catchup = needs_initial_catchup || needs_always_catchup ||
                          needs_user_ff       || needs_battle_emergency;

    // ---- SUSTAINED OVERSHOOT DRAIN (gentle; NOT the catchup turbo) --------
    // Steady state below is exactly 1x wall clock, so a viewer that ends up N
    // frames past its delay bank stays N frames past it for the whole session.
    // Reachable three ways, all observed: the one-shot initial catchup latching
    // done on a transient dip mid-backfill; an arrival burst that the 1x playout
    // can never bleed; and the adaptive bank drifting DOWN at 10 frames/s after
    // a calm patch (spec_playback_state.cpp's shrink comment already assumes a
    // "gentle drain bleeds the excess cushion as the target falls" -- this is
    // that drain, which did not actually exist). Before this, the only drains
    // left after the one-shot latched were battle-emergency at target+600, F12,
    // and FM2K_SPECTATOR_ALWAYS_CATCHUP.
    //
    // Deliberately NOT routed through g_spectator_catchup: that flag mutes
    // audio (hooks_render.cpp) and suppresses the silence failover
    // (spec_health.cpp), neither of which may be disabled for the many seconds
    // a gentle drain runs. Instead it is a small multiplier on the steady-state
    // wall-clock accumulator -- bounded by real time, still one render per outer
    // tick, still hard-capped at 4 frames/tick, so it can never fast-forward.
    //
    // LOSS ABSORPTION IS PRESERVED BY CONSTRUCTION: the drain's floor IS
    // SpectatorTargetDelayFrames(). The adaptive bank grows INSTANTLY when
    // admission gaps grow, so a lossy link raises the target, the overshoot
    // disappears and the drain disengages within a tick. It only ever bleeds
    // depth the bank itself has stopped asking for.
    //
    // (Considered: the netplay lesson that catch-up fights gekko's pacing, and
    // the resulting revert. It does not transfer -- the spectator sim is not
    // gekko-paced, it is a jitter-buffered player draining a one-way stream, so
    // there is no controller to fight. What it DOES have to not fight is the
    // adaptive bank, which is why the floor is the bank and never the live edge.)
    double steady_rate = 1.0;
    {
        constexpr uint64_t kDrainSustainMs = 3000;  // overshoot must HOLD
        constexpr double   kDrainRate      = 1.25;  // +25% == 25 frames/s bleed
        static bool     s_drain_active    = false;
        static uint64_t s_overshoot_since = 0;
        const size_t   tgt = SpectatorTargetDelayFrames();
        // Relative deadband: a big bank means a link whose queue legitimately
        // swings by a big ABSOLUTE number of frames, where a fixed threshold
        // would chatter. Floored at 50 so a small bank still has a real
        // deadband. Engage is sustained (kDrainSustainMs of continuous
        // overshoot, so a single-tick burst can never arm it); release is at
        // the target itself, giving margin+ frames of hysteresis in between.
        const size_t margin = (tgt / 4 > 50) ? tgt / 4 : 50;
        const uint64_t dnow = GetTickCount64();
        if (qd > tgt + margin) {
            if (s_overshoot_since == 0) s_overshoot_since = dnow;
        } else {
            s_overshoot_since = 0;
        }
        if (!s_drain_active) {
            if (s_overshoot_since != 0 &&
                dnow - s_overshoot_since >= kDrainSustainMs) {
                s_drain_active = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SPEC-DRAIN] engaged: q=%zu held above bank+%zu for %llums "
                    "-- gentle %.2fx playout until q<=%zu",
                    qd, margin, (unsigned long long)kDrainSustainMs,
                    kDrainRate, tgt);
            }
        } else if (qd <= tgt) {
            s_drain_active = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-DRAIN] released: q=%zu back at the bank (%zu) "
                "-- 1x wall clock resumed", qd, tgt);
        }
        // Where the drain may act. mode >= 3000 (battle/results) only: that is
        // where frames cost ~0.1ms. CSS stays 1x forever for the same reason
        // the CSS turbo was hardwired off -- a hover-triggered .player load is
        // 100-500ms of real sim work and any speed-up there is a slideshow.
        // Also never during a SEAM/PINNING walk: those pops are SYNTHETIC and
        // consume nothing from the queue, so "draining" would only jab the
        // results screen faster (see the bounded seam walk in spec_playback).
        const bool in_boundary_walk = SpectatorNode_InBoundary();
        const bool drain_ok = !s_offline_replay_env_active &&
                              !in_boundary_walk &&
                              live_game_mode >= 3000u;
        // stream_over bypasses the PHASE gate (the host is gone; finishing what
        // we hold is the only remaining job, and a fast-forwarded tail beats
        // being cut off mid-match) but keeps the boundary gate for the same
        // reason drain_ok has it.
        const bool tail_drain = stream_over && !in_boundary_walk && qd > 0;
        if ((s_drain_active && drain_ok) || tail_drain) {
            steady_rate = kDrainRate;
        }
    }

    const uint64_t catchup_start_ms = GetTickCount64();
    uint32_t catchup_frames = 0;
    while (g_spectator_catchup) {
        if (!SpectatorSimOneFrame()) break;
        // Render only every 64th catchup frame. Per-frame render here
        // existed for RNG parity (render-side game_rand advanced the
        // gameplay seed), which 57b72ad made obsolete: render now has
        // its own isolated RNG stream and never touches the gameplay
        // seed -- proven by deep-join parity matching bit-for-bit
        // through a 3500-frame catchup window. Skipping render turns
        // catchup from ~10ms/frame into ~0.1ms/frame: a mid-match join
        // drains thousands of frames in well under a second ("snapshot
        // feel"). The sparse renders keep visual progress on screen.
        if ((++catchup_frames & 63u) == 0) {
            RenderFrameWithSnapshot();
        }
        ControlChannel_Poll();   // keeps TCP recv drained, no failover side-effects
        if (SpectatorNode_PendingFrameCount() <= SpectatorTargetDelayFrames()) {
            // Live edge reached. The outer-tick state machine flips
            // s_initial_catchup_active → s_initial_catchup_done next
            // call, so this branch only needs to bail.
            g_spectator_catchup = false;
            return;  // catchup-final tick already sim'd + rendered
        }
        if (GetTickCount64() - catchup_start_ms >= CATCHUP_MAX_BURST_MS) {
            // Bounded -- yield to the outer tick. Catchup re-engages next
            // tick if we're still over the lag target. We've already
            // rendered this iteration, so return without the trailing
            // steady-state sim+render.
            g_spectator_catchup = false;
            return;
        }
    }
    g_spectator_catchup = false;  // safety: clear before render path

    // UNIFIED wall-clock pacing (task #60, para 2026-07-19; supersedes the
    // 2026-07-06 slow-tick-only fix). The old steady state popped ONE frame
    // per outer tick unconditionally, plus extras when ticks ran slow. On
    // machines where the tick runs FAST -- 120Hz-monitor vsync pacing the
    // loop at ~8.3ms instead of the 10ms SleepToTarget -- that played out at
    // 105-120fps: the viewer outran the host, drained its jitter buffer,
    // starved, gap-filled, and sawtoothed (para's wild 2.82 log: pops/s
    // 109-132 sustained, q 300->0 cycles, catchup latched). Fixed timestep
    // on the wall clock in BOTH directions: accumulate elapsed/10ms and pop
    // exactly that many frames -- 0 on fast ticks, 2+ on slow ones. Offline
    // replay stays strict 1:1 per tick (whole file queued at boot; wall
    // clock is meaningless there).
    if (!s_offline_replay_env_active) {
        static uint64_t s_last_step_ms = 0;
        const uint64_t now_ms = GetTickCount64();
        if (s_last_step_ms != 0) {
            // steady_rate is 1.0 unless the sustained overshoot drain (or the
            // end-of-stream drain) is engaged -- see the [SPEC-DRAIN] block
            // above. Applied BEFORE the cap so the "never a jarring turbo"
            // bound stays exactly 4 frames per outer tick either way.
            double due = (double)(now_ms - s_last_step_ms) / 10.0 * steady_rate;
            if (due > 4.0) due = 4.0;   // hard cap: never a jarring turbo
            g_spec_steady_accum += due;
            if (g_spec_steady_accum > 4.0) g_spec_steady_accum = 4.0;
        } else {
            g_spec_steady_accum = 1.0;  // first tick: exactly one frame
        }
        s_last_step_ms = now_ms;
        int todo = (int)g_spec_steady_accum;
        g_spec_steady_accum -= (double)todo;
        for (int k = 0; k < todo; ++k) {
            if (!SpectatorSimOneFrame()) break;   // underrun: stop, keep remainder at 0
        }
        RenderFrameWithSnapshot();
        return;
    }

    // Offline replay steady state: one popped frame, one render, 1:1.
    if (!SpectatorSimOneFrame()) {
        RenderFrameWithSnapshot();
        return;
    }
    // FIXED-TIMESTEP CATCH-UP (2026-07-06). The outer tick is paced to 10ms
    // (100fps) via SleepToTarget, but under load -- RC retransmit/FEC + render
    // overrunning 10ms at 20% loss -- a tick takes ~12-14ms, so the effective
    // advance rate falls BELOW the host's 100fps production (measured ~70-90
    // pops/s) and the spectator drowns with no recovery. SleepToTarget can only
    // pad a fast tick UP; it can't speed a slow one. Fix: advance on the WALL
    // CLOCK, not 1-per-tick -- if this tick took N×10ms, advance N frames so the
    // effective playout stays locked to 100fps and can't fall behind. Fractional
    // accumulator; the extra is CAPPED (never a turbo) and stops on underrun
    // (qd==0) so it can't over-drain. Offline replay stays 1:1 (whole file
    // queued at boot). This is the missing symmetric speed-up -- steady, invisible,
    // and keyed on the RIGHT signal (wall-clock deficit), not queue depth.
    RenderFrameWithSnapshot();
}

