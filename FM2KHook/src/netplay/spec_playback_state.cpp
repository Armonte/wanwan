// spec_playback_state.cpp -- spectator admission pacing, adaptive delay bank
// and the small session-state accessors, split verbatim out of
// spec_playback.cpp (which had crept back over the 1000-line limit).
//
// Pure move, no behavior change. What is left behind in spec_playback.cpp is
// the two big apply paths -- ApplySessionEvent and PopFrameInputs -- which is
// the actual sim-facing work; everything here is measurement and accessors.
//
// Only linkage change: SpecDelayBankFrames() lost `static` because
// PopFrameInputs (still in spec_playback.cpp) calls it. It is declared in
// spectator_node_internal.h.

#include "spectator_node.h"
#include "spectator_node_internal.h"   // State model + g_state
#include "netplay.h"
#include "globals.h"   // FM2K:: engine constants used by the pacing math

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>


static uint64_t g_last_input_admit_ms = 0;

// Layer-2 render pacing: the host's PRODUCTION RATE, measured as admissions per
// rolling 500ms window (robust to the 8-frame EVENT_BATCH bursts, which make
// per-gap timing useless -- gaps are bimodal ~0ms within-batch / ~80ms between).
// Smoothed -> g_prod_period_ms. SpectatorNode_RenderPeriodMs() clamps it to
// [10,20]ms ([100,50]fps) and the spectator render loop paces to it, so a slow
// heavy-stage host is rendered at its true rate with no duplicate frames.
static uint32_t g_admit_window_count = 0;
static uint64_t g_admit_window_start = 0;
static double   g_prod_period_ms     = 10.0;  // start at the 100fps assumption

// Adaptive delay bank (Phase G). The static 300-frame bank absorbs any
// arrival gap shorter than 3s -- enough for the tested clumsy profile,
// but a link with longer retransmit bursts still drains to q:0 and
// stalls. Measure the real inter-admission gaps (two rotating 30s
// buckets = rolling 30-60s max) and GROW the bank target to fit the
// link: target = max(env floor, observed_max_gap * 1.5), capped at
// 2000 frames (20s). Grow-only per session -- over-buffering after a
// bad patch is the right bias (no-stall beats low-latency here), and
// shrinking mid-stream would oscillate the glide. Gaps above 5s are
// ignored: that's an outage (TCP death, host frozen) owned by the
// failover/rejoin machinery, not jitter for the pacing layer to absorb.
// FM2K_SPEC_ADAPTIVE=0 pins the bank to the static floor.
static uint32_t g_admit_gap_bucket_cur   = 0;   // max gap (ms), current 30s bucket
static uint32_t g_admit_gap_bucket_prev  = 0;
static uint64_t g_admit_gap_bucket_start = 0;
static size_t   g_adaptive_bank_frames   = 0;   // grow-only published target
static uint64_t g_first_input_admit_ms   = 0;   // session's first admission
// Last INPUT admitted via a UDP datagram specifically (heartbeats and
// control traffic don't count). Drives the TCP-only floor pre-arm.
static uint64_t g_last_udp_input_admit_ms = 0;
void SpectatorNode_StampUdpInputAdmit() {
    g_last_udp_input_admit_ms = GetTickCount64();
}

void SpectatorNode_StampInputAdmit() {
    const uint64_t now = GetTickCount64();
    if (g_first_input_admit_ms == 0) g_first_input_admit_ms = now;
    // Production-rate window (Layer-2 render pacing). Count admissions per 500ms
    // -> fps -> smoothed period. A catch-up UDP flood spikes the rate, which only
    // drives the period BELOW 10ms -> clamped back to 10ms by the accessor, so
    // catch-up can't make the render crawl; no separate catchup gate needed.
    ++g_admit_window_count;
    if (g_admit_window_start == 0) {
        g_admit_window_start = now;
    } else if (now - g_admit_window_start >= 500) {
        const double win_ms = (double)(now - g_admit_window_start);
        const double rate_fps = (double)g_admit_window_count * 1000.0 / win_ms;
        if (rate_fps > 1.0) {
            const double period = 1000.0 / rate_fps;
            g_prod_period_ms += 0.3 * (period - g_prod_period_ms);  // smooth
        }
        g_admit_window_count = 0;
        g_admit_window_start = now;
    }
    if (g_last_input_admit_ms != 0) {
        const uint64_t gap = now - g_last_input_admit_ms;
        // 10s ceiling: longer silences are outages (TCP death, frozen
        // host) owned by the failover machinery. Everything under it is
        // jitter the bank must absorb -- the first UDP-off control run
        // showed 8.8s TCP retransmit bursts under clumsy that a 5s
        // ceiling wrongly discarded, so the bank stayed at 705 frames
        // while the link needed ~1300 and mid-battle q:0 stalls kept
        // happening (2026-06-11 18:15).
        if (gap <= 10000) {
            if (g_admit_gap_bucket_start == 0) g_admit_gap_bucket_start = now;
            if (now - g_admit_gap_bucket_start >= 30000) {
                g_admit_gap_bucket_prev  = g_admit_gap_bucket_cur;
                g_admit_gap_bucket_cur   = 0;
                g_admit_gap_bucket_start = now;
            }
            if ((uint32_t)gap > g_admit_gap_bucket_cur) {
                g_admit_gap_bucket_cur = (uint32_t)gap;
            }
        }
    }
    g_last_input_admit_ms = now;
}

uint32_t SpectatorNode_RenderPeriodMs() {
    // Offline replay plays at a RIGID 1:1 (paced by the file, not a live host).
    // The whole file is queued at boot so the production-rate window is
    // meaningless -- never let it drive the replay's tick rate (F12 is the only
    // speed lever). Belt-and-braces vs the catch-up carve-out in
    // trampoline_spectator.cpp.
    static int s_is_replay = -1;
    if (s_is_replay < 0) {
        const char* rf = std::getenv("FM2K_REPLAY_FILE");
        s_is_replay = (rf && rf[0]) ? 1 : 0;
    }
    if (s_is_replay) return 10;
    // Pace the spectator render loop to the measured host production period,
    // clamped [10ms, 20ms] = [100fps, 50fps]. 100fps host -> 10ms (identical to
    // the old rigid behavior); ~70fps heavy-stage host -> ~14ms, one render per
    // produced frame, no duplicates, stable bank. Below 50fps the bank + the
    // proportional slowdown (trampoline_spectator.cpp) absorb the rest.
    double p = g_prod_period_ms;
    if (p < 10.0) p = 10.0;
    if (p > 20.0) p = 20.0;
    return (uint32_t)(p + 0.5);
}

size_t SpectatorNode_TargetDelayFrames() {
    static size_t s_floor = []() -> size_t {
        const char* e = std::getenv("FM2K_SPEC_DELAY");
        if (e && e[0]) {
            const long n = std::strtol(e, nullptr, 10);
            if (n >= 50 && n <= 2000) return (size_t)n;
        }
        return 300;
    }();
    static int s_adaptive = -1;
    if (s_adaptive < 0) {
        const char* v = std::getenv("FM2K_SPEC_ADAPTIVE");
        s_adaptive = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
    }
    size_t floor_eff = s_floor;
    // TCP-only pre-arm: with the UDP accelerator dead (firewalled, or
    // FM2K_SPEC_UDP=0), arrivals come in TCP retransmit bursts that
    // routinely exceed the 3s default under loss -- don't wait for the
    // first stall to teach the adaptive growth; start from a 6s floor.
    // Engages only after 10s of admissions with no UDP-borne INPUT.
    if (s_adaptive == 1 && floor_eff < 600 &&
        g_state.subscribed_upstream && g_first_input_admit_ms != 0) {
        const uint64_t now = GetTickCount64();
        const bool udp_quiet =
            (g_last_udp_input_admit_ms == 0)
                ? (now - g_first_input_admit_ms > 10000)
                : (now - g_last_udp_input_admit_ms > 10000);
        if (udp_quiet) floor_eff = 600;
    }
    if (s_adaptive == 1) {
        const uint32_t max_gap_ms =
            (g_admit_gap_bucket_cur > g_admit_gap_bucket_prev)
                ? g_admit_gap_bucket_cur : g_admit_gap_bucket_prev;
        size_t want = (size_t)(max_gap_ms + max_gap_ms / 4) / 10;  // x1.25, ms -> frames
        if (want > 2000) want = 2000;
        const uint64_t now = GetTickCount64();
        static uint64_t s_last_grow_ms   = 0;
        static uint64_t s_last_shrink_ms = 0;
        if (want > g_adaptive_bank_frames) {
            g_adaptive_bank_frames = want;
            s_last_grow_ms = now;
            if (g_adaptive_bank_frames > floor_eff) {
                static uint64_t s_grow_log_ms = 0;
                if (now - s_grow_log_ms >= 1000) {
                    s_grow_log_ms = now;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[SPEC-BANK] adaptive bank grew to %zu frames "
                        "(max admission gap %ums x1.25)",
                        g_adaptive_bank_frames, max_gap_ms);
                }
            }
        } else if (g_adaptive_bank_frames > want &&
                   g_adaptive_bank_frames > floor_eff &&
                   s_last_grow_ms != 0 &&
                   now - s_last_grow_ms > 60000 &&
                   now - s_last_shrink_ms >= 100) {
            // Shrink-back: grow-only pinned the session at its WORST
            // moment forever -- one early 9s burst meant 12s+ latency
            // for the rest of the night even on a recovered link. The
            // rolling buckets age the bad gap out within 60s; once no
            // growth has been needed for 60s, drift the target down at
            // 10 frames/s (the gentle 2x drain bleeds the excess cushion
            // as the target falls -- smooth catch-up, no jump cut).
            // Never below the current window's want or the floor.
            s_last_shrink_ms = now;
            --g_adaptive_bank_frames;
            static uint64_t s_shrink_log_ms = 0;
            if (now - s_shrink_log_ms >= 5000) {
                s_shrink_log_ms = now;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SPEC-BANK] calm link -- bank drifting down: %zu "
                    "frames (window want %zu)",
                    g_adaptive_bank_frames, want);
            }
        }
    }
    return (g_adaptive_bank_frames > floor_eff) ? g_adaptive_bank_frames
                                                : floor_eff;
}

size_t SpecDelayBankFrames() {
    return SpectatorNode_TargetDelayFrames();
}
uint32_t SpectatorNode_MsSinceLastAdmit() {
    if (g_last_input_admit_ms == 0) return 0;  // nothing admitted yet
    return (uint32_t)(GetTickCount64() - g_last_input_admit_ms);
}

// True once the spectator has admitted at least one frame (i.e. the JOIN
// handshake + snapshot succeeded and the stream started). Distinguishes a
// still-CONNECTING spectator (never admitted) from a mid-stream stall (admitted
// then went quiet) so the connect-establishment deadline only fires on the
// former -- see the watchdog in trampoline_spectator.cpp.
bool SpectatorNode_HasEverAdmitted() { return g_first_input_admit_ms != 0; }

// True once the upstream told us the whole session is OVER (SPEC_SESSION_END
// — the host quit cleanly). Distinguishes a GRACEFUL stream end (drain what's
// left, then close with "stream ended") from an ungraceful host vanish
// (crash/drop, handled by the host-gone watchdog). See trampoline_spectator.
bool SpectatorNode_SessionEnded() { return g_state.session_ended; }

bool SpectatorNode_IsSubscribedUpstream() { return g_state.subscribed_upstream; }

// True while the upstream TCP died but the subscription is riding on UDP
// with a background re-JOIN in flight. Surfaced in the window title as
// "Resyncing..." -- distinct from a cold "Connecting..." (no
// subscription at all) and from a healthy "Subscribed".
bool SpectatorNode_IsTcpRejoinPending() { return g_state.tcp_rejoin_pending; }

// Natural-boot title/menu walk in progress: the synthetic title presses
// live inside PopFrameInputs, so the jitter floor must not gate the tick
// on queue depth while the local game is still pre-CSS (q=0 at boot is
// normal -- the title walk is what gets us to where the stream starts).
bool SpectatorNode_InNaturalBootWalk() {
    if (!g_state.natural_boot) return false;
    return *(uint32_t*)FM2K::ADDR_GAME_MODE < 2000u;
}

// -----------------------------------------------------------------------------
// PLAYBACK DRIVER API (called from main_loop_trampoline + Hook_GetPlayerInput)
// -----------------------------------------------------------------------------

// Spectate rng-desync fix: a snapshot-join restored the authoritative gameplay
// seed (0x41FB1C is in the savestate) + full object state for the snapshot's
// battle frame. The SpecSim battle-entry init must then NOT re-apply PIN_RNG
// (the stale 0x12345678 frame-0 seed) -- that clobber is the desync root. Only
// a from-scratch replay (no snapshot) needs the pin. Durable flag (not the
// timing-fragile g_spec_skip_next_battle_init).
bool SpectatorNode_SnapshotAppliedOnce() {
    return g_state.pb_snapshot_applied_once;
}

// Clear the snapshot-applied suppression when the spectator LEAVES a battle
// (match end -> results/CSS). The flag exists to suppress the battle-entry
// DoInitialSync+PIN_RNG for the ONE battle a snapshot restored -- but it is
// otherwise durable for the whole session (only reset at teardown). Left set,
// a REMATCH (match 2+) -- which a continuing spectator always enters
// from-scratch (no new snapshot: the rewind guard discards re-join snapshots
// once pb_started) -- would wrongly skip its battle-entry init and desync on
// its pinned seed. Clearing it at battle exit makes the gate per-battle-entry:
// only the battle the snapshot actually landed in skips the init.
void SpectatorNode_ClearSnapshotAppliedForNextBattle() {
    g_state.pb_snapshot_applied_once = false;
}

bool SpectatorNode_IsPlayingBack() {
    // Sticky once subscribed: stays true from JOIN_ACK through everything
    // (active matches, MATCH_END drains, post-match idle, between-match
    // CSS), only resetting on shutdown / leave. This is what makes
    // Hook_GetPlayerInput unconditionally route through the
    // spectator-cached values — important because the spectator is
    // marked Netplay_IsConnected() (we set it in InitAsSpectator), so
    // without this gate the CSS branch of Hook_GetPlayerInput would
    // serve garbage from the spectator's empty CSS input buffers.
    return g_state.subscribed_upstream
        || g_state.playing_back
        || !g_state.pb_queue.empty();
}

