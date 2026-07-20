// Spectator-side playback driver: the op-apply dispatcher (ApplySessionEvent +
// ApplyResetInputState), the adaptive delay bank, and PopFrameInputs (the
// frame pacemaker + match-boundary state machine). Extracted VERBATIM from
// spectator_node.cpp. Apply fns live in namespace specnode (also called by
// snapshot-cache); the rest is public API (decls in spectator_node.h).
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
#include "spectator_tcp.h"        // TCP transport for INPUT_BATCH stream
#include "control_channel.h"
#include "netplay.h"
#include "replay.h"
#include "savestate.h"            // SaveState_Save / Peek for snapshot capture
#include "netplay_state.h"
#include "../audio/sound_rollback.h"  // Op apply: SOUND_INIT
#include "../hooks/css_autoconfirm.h" // Replay-mode CSS lock-and-confirm
#include "../hooks/per_game_patches.h" // PerGamePatches_SetRuntimeBtbOverrides
#include "../ui/shared_mem.h"         // C10: SharedMem_PublishMatchSession / RoundResult
#include "gekkonet.h"

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <array>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
using namespace specnode;

namespace specnode {

void ApplyResetInputState() {
    // Mirror Netplay_StartBattle's first-call SaveState_Save reset
    // (savestate.cpp:223-237). FM2K addresses, gated: the FM95 spectator
    // port (Gap-4d in docs/FM95_Support_Status.md) must remap these to the
    // FM95 equivalents -- buf_idx 0x437700, frame counter 0x4DD7A8, input
    // edge state 0x4255A8 (see savestate_fm95.cpp Block D).
    if constexpr (FM2K::kIsFM2K) {
        *(uint32_t*)0x447EE0 = 0;            // g_input_buffer_index
        *(uint32_t*)0x4456FC = 0;            // render frame counter
        std::memset((void*)0x447F00, 0, 0x20);    // g_prev_input_state
        std::memset((void*)0x447F40, 0, 0x20);    // g_processed_input
        std::memset((void*)0x447F60, 0, 0x20);    // g_input_changes
        std::memset((void*)0x4280D8, 0, 0x2008);  // input_history rings (P1+P2)
    }
}

void ApplySessionEvent(const SessionEvent& ev) {
    switch (ev.type) {
        case SessionEventType::PIN_RNG:
            // The host emitted PIN_RNG at battle-entry, AFTER title/CSS
            // sim had already consumed RNG. If we apply it eagerly here
            // (at first replay tick = title screen), then title/CSS run
            // RNG-consuming code starting FROM the pinned seed, mutating
            // it further → first battle frame's rng != host's first
            // battle frame's rng. Visual / engine state matches (since
            // title/CSS rng draws don't affect chars), but the parity
            // recorder's rng field diverges.
            //
            // Defer: write rng AT battle-entry (game_mode flip to 3000)
            // instead of immediately. Stash the seed; SpectatorSimOneFrame's
            // initial-sync block applies it at the same logical instant
            // host's PIN_RNG fired.
            g_state.pending_pin_rng_seed  = ev.u.pin_rng_seed;
            g_state.pending_pin_rng_valid = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: queued PIN_RNG=0x%08X (will apply at battle entry)",
                ev.u.pin_rng_seed);
            break;
        case SessionEventType::RESET_INPUT_STATE:
            if (g_state.pb_boundary != State::PbBoundary::NONE) {
                // Seam: defer to battle entry (see pending_reset_input).
                g_state.pending_reset_input = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: queued RESET_INPUT_STATE (seam -- will "
                    "apply at battle entry)");
            } else {
                ApplyResetInputState();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: applied RESET_INPUT_STATE");
            }
            break;
        case SessionEventType::CSS_ENTERED:
            g_state.pb_css_marker_seen = true;
            // Apply the host's CSS start state (cursor + selected) BEFORE the
            // replay walk begins. A snapshot-join spectator's own cursor/selected
            // are stale at a rematch (it never walked CSS1), so without this the
            // pure replay walks from the wrong start and locks the wrong char.
            // Aligning the start makes the natural walk land on the host's char.
            if constexpr (!FM2K::kIsFM95) {
                *(int32_t*)0x424E50 = ev.u.css_entered.p1_cur_x;
                *(int32_t*)0x424E54 = ev.u.css_entered.p1_cur_y;
                *(int32_t*)0x424E58 = ev.u.css_entered.p2_cur_x;
                *(int32_t*)0x424E5C = ev.u.css_entered.p2_cur_y;
                *(int32_t*)0x470020 = ev.u.css_entered.p1_sel;
                *(int32_t*)0x470024 = ev.u.css_entered.p2_sel;
                // #66: reset the auto-repeat input state so the spectator's CSS
                // navigation TIMING starts from the same point as the host
                // (mirror the player reset in the CSS reseed block). Without it
                // the spectator's REMATCH cursor path skews vs the host (the
                // 5-cell CSS-SPEC sess1 residual) even though the final char and
                // battle stay bit-exact -- g_input_repeat_state/timer carry over
                // from the prior match and are menu-only (never battle-saved).
                std::memset((void*)0x541F80, 0, 0x20);  // g_input_repeat_state[8]
                std::memset((void*)0x4D1C40, 0, 0x20);  // g_input_repeat_timer[8]
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied CSS_ENTERED (seam split) -- synced host CSS "
                "start cur=(%d,%d)/(%d,%d) sel=%d/%d",
                (int)ev.u.css_entered.p1_cur_x, (int)ev.u.css_entered.p1_cur_y,
                (int)ev.u.css_entered.p2_cur_x, (int)ev.u.css_entered.p2_cur_y,
                (int)ev.u.css_entered.p1_sel, (int)ev.u.css_entered.p2_sel);
            break;
        case SessionEventType::SOUND_INIT:
            if (g_state.pb_boundary != State::PbBoundary::NONE) {
                g_state.pending_sound_init = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: queued SOUND_INIT (seam -- will apply "
                    "at battle entry)");
            } else {
                SoundRollback::Init();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: applied SOUND_INIT");
            }
            break;
        case SessionEventType::MATCH_START: {
            g_state.pb_awaiting_match_end = true;
            g_state.pb_local_battle_seen  = false;
            // Look up the cached 96-byte header by side-table index.
            // Header layout matches Replay::ReplayHeader on-disk; pull
            // seed/state-hash/char/color and re-publish into the playback
            // metadata so any UI consumers (HUD, replay loader handoff)
            // see the live values.
            if (ev.u.match_start_idx < g_state.pb_match_headers.size()) {
                const uint8_t* h = g_state.pb_match_headers[ev.u.match_start_idx].data();
                uint32_t seed = 0, state_hash = 0;
                std::memcpy(&seed,       h + 20, 4);
                std::memcpy(&state_hash, h + 24, 4);
                g_state.pb_initial_seed  = seed;
                g_state.pb_initial_state = state_hash;
                g_state.pb_p1_char       = h[28];
                g_state.pb_p1_color      = h[29];
                g_state.pb_p2_char       = h[30];
                g_state.pb_p2_color      = h[31];
                g_state.pb_stage_id      = h[80];
                // #66/replay: restore the match's round-timer gameconfig BEFORE
                // CSS init runs (playback re-runs CSS via CssAutoConfirm), so
                // g_round_timer_cfg1 @0x470060 is set from the MATCH's round time,
                // not the replayer's game.ini -> playback timer == match timer.
                // Legacy files carry 0 here -> skip (keep current config).
                if constexpr (!FM2K::kIsFM95) {
                    uint32_t rts = 0, rc = 0;
                    std::memcpy(&rts, h + 81, 4);
                    std::memcpy(&rc,  h + 85, 4);
                    // 0 = legacy (field absent); 0xFFFFFFFF = "no override".
                    if (rts && rts != 0xFFFFFFFFu) *(uint32_t*)0x430114 = rts;  // g_round_time
                    if (rc  && rc  != 0xFFFFFFFFu) *(uint32_t*)0x430124 = rc;   // g_default_round
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: restored round cfg -- time=%u count=%u", rts, rc);
                }
                // Mirror the legacy INITIAL_MATCH packet path so the
                // initial-match cache stays valid for relay-to-sub-spectator.
                std::memcpy(g_state.initial_match.header_bytes, h, 96);
                g_state.initial_match.valid = true;
            }
            g_state.playing_back = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied MATCH_START seed=0x%08X p1=%u/%u p2=%u/%u stage=%u",
                g_state.pb_initial_seed,
                g_state.pb_p1_char, g_state.pb_p1_color,
                g_state.pb_p2_char, g_state.pb_p2_color,
                g_state.pb_stage_id);
            // Arm the CSS auto-lock-and-confirm hook so the local game pins
            // to the announced chars/stage when CSS opens.
            //   - Offline replay (FM2K_REPLAY_FILE): always -- the file's
            //     INPUTs are battle-phase only, CSS must be driven by pins.
            //   - Live spectator at a match boundary (pb_boundary==SEAM):
            //     same reasoning. The old assumption ("live-spec walks CSS
            //     via the upstream input log") only holds for FULL_SESSION
            //     specs on their FIRST CSS, where the fresh boot matches
            //     the host's initial CSS state. At match 2+ the seam can't
            //     align (see PbBoundary), so picks must come from this
            //     header -- raw CSS replay locked the wrong characters.
            {
                static int s_offline_replay_cached = -1;
                if (s_offline_replay_cached < 0) {
                    const char* v = std::getenv("FM2K_REPLAY_FILE");
                    s_offline_replay_cached = (v && v[0]) ? 1 : 0;
                }
                if (s_offline_replay_cached == 1 ||
                    g_state.pb_boundary == State::PbBoundary::SEAM) {
                    CssAutoConfirm_OnReplayMatchStart(
                        g_state.pb_p1_char, g_state.pb_p1_color,
                        g_state.pb_p2_char, g_state.pb_p2_color,
                        g_state.pb_stage_id);
                } else if (*(uint32_t*)FM2K::ADDR_GAME_MODE < 3000u) {
                    // Live FULL_SESSION first-CSS walk: the host's MATCH_START
                    // drained while we're STILL in CSS (mode<3000) -- the host
                    // already entered battle, but our seam-hold-mask-delayed
                    // CSS lock hasn't fired. A naive walk now consumes the
                    // host's early BATTLE inputs as if they were CSS, offsetting
                    // the entire battle stream (the ~+30-frame bf=77 desync).
                    // Arm the SAME deterministic pin the offline/SEAM paths use
                    // (force-locks the host's chars from MATCH_START metadata --
                    // and the pin bypasses the seam-hold mask, css_autoconfirm
                    // masks only when !g_active) and HOLD battle inputs until
                    // the engine crosses into battle, so bf=0 lands exactly on
                    // the host's first battle input (battle-start aligned).
                    CssAutoConfirm_OnReplayMatchStart(
                        g_state.pb_p1_char, g_state.pb_p1_color,
                        g_state.pb_p2_char, g_state.pb_p2_color,
                        g_state.pb_stage_id);
                    g_state.pb_battle_align_pending = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: battle-align -- MATCH_START at CSS "
                        "(mode<3000); pin armed + holding battle inputs until "
                        "mode>=3000 (anchors bf=0 to host battle-start)");
                }
                if (g_state.pb_boundary == State::PbBoundary::SEAM) {
                    g_state.pb_boundary = State::PbBoundary::PINNING;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: boundary SEAM -> PINNING (holding "
                        "battle inputs until local game_mode reaches 3000)");
                }
            }
            break;
        }
        case SessionEventType::MATCH_END: {
            // Don't clear pb_queue — let queued post-MATCH_END frames drain
            // (they render the final battle frames). The next MATCH_START
            // resets metadata and flips playing_back back on.
            g_state.playing_back = false;
            // Enter the seam: from here until the next MATCH_START, INPUT
            // events are host results/CSS frames -- presentation-only and
            // structurally misaligned with the spec's own seam timing.
            // PopFrameInputs discards them and feeds synthetic inputs; the
            // next MATCH_START re-arms character pinning (see PbBoundary).
            //
            // Offline replay keeps the legacy path: single-match .fm2krep
            // files have no following MATCH_START, so a SEAM would feed
            // synthetic inputs forever and block the queue-drained
            // ExitProcess (observed: replay-phase instance stuck at its
            // results screen after the Phase F boundary rework).
            {
                static int s_live_spec = -1;
                if (s_live_spec < 0) {
                    const char* rp = std::getenv("FM2K_REPLAY_FILE");
                    s_live_spec = (rp && rp[0]) ? 0 : 1;
                }
                if (s_live_spec == 1) {
                    // LEAN seam: pure 1:1 replay through the boundary
                    // with exactly two thin protections --
                    //  (a) discard-until-CSS_ENTERED once the local CSS
                    //      opens, so the mirror starts at the host's CSS
                    //      frame 0 even when the two results screens'
                    //      frame counts drift by a few frames;
                    //  (b) a short confirm mask at CSS open, because CSS
                    //      init clears the input-edge state and the first
                    //      consumed input (battle-tail attack bits)
                    //      otherwise registers as a rising confirm for
                    //      both players at their carried cursors (locked
                    //      7/24 five seconds before the host confirmed
                    //      17/6, 2026-06-11).
                    // No pinning, no synthetic CSS walk, no forced locks:
                    // the players' real confirms drive everything.
                    g_state.pb_boundary = State::PbBoundary::SEAM;
                    g_state.pb_css_marker_seen = false;
                }
            }
            const auto& p = ev.u.match_end;
            const char* who = (p.winner_idx == 0) ? "P1"
                            : (p.winner_idx == 1) ? "P2" : "DRAW";
            g_state.pb_awaiting_match_end = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied MATCH_END winner=%s rounds=%u-%u "
                "frames=%u (queued=%zu) -- boundary SEAM entered",
                who, p.rounds_won_p1, p.rounds_won_p2, p.frames_total,
                g_state.pb_queue.size());
            break;
        }
        case SessionEventType::FINGERPRINT: {
            // C9: diagnostic mismatch detection. Host emits its hash here;
            // spectator computes the same hash on its current state and
            // compares. Drain-at-head ordering ensures both sides sample
            // at the same logical frame.
            if (SpectatorFingerprint_Enabled()) {
                const uint32_t host_hash = ev.u.fingerprint_hash;
                const uint32_t local_hash = SpectatorFingerprint_Compute();
                if (host_hash != local_hash) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[SPEC-FP-MISMATCH] host=0x%08X spectator=0x%08X — "
                        "DESYNC at this logical frame (next INPUT is the "
                        "first divergent sim step)",
                        host_hash, local_hash);
                } else {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[SPEC-FP-OK] host=0x%08X (matches local)", host_hash);
                }
            }
            break;
        }
        case SessionEventType::ROUND_START: {
            // C3.5 — informational on the spectator. Simulation drives banner
            // and round-reset state from INPUT events; ROUND_START is a marker
            // for replay-file slicing (round_offsets[]) and overlay diagnostics.
            const auto& p = ev.u.round_start;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: ROUND_START round=%u p1_hp_max=%u p2_hp_max=%u timer=%us",
                p.round_idx, p.p1_hp_max, p.p2_hp_max, p.timer_seconds);
            break;
        }
        case SessionEventType::ROUND_END: {
            const auto& p = ev.u.round_end;
            const char* who = (p.winner_idx == 0) ? "P1"
                            : (p.winner_idx == 1) ? "P2" : "DRAW";
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: ROUND_END winner=%s p1_hp=%u p2_hp=%u frames=%u",
                who, p.p1_hp_remaining, p.p2_hp_remaining, p.frames_elapsed);
            break;
        }
        case SessionEventType::SESSION_ID:
            // C7 — informational on the spectator. The host's session_id
            // already lives at the head of the event stream by the time
            // we apply this; nothing else to do beyond logging. Spectator
            // recordings (.fm2kset / .fm2krep) will inherit this id when
            // C7's writer pulls it from g_state.
            g_state.session_id = ev.u.session_id;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: applied SESSION_ID=0x%016llX",
                (unsigned long long)ev.u.session_id);
            break;
        case SessionEventType::INPUT:
            // Should not reach here — INPUT is handled by the pop path in
            // PopFrameInputs, not the drain.
            break;
    }
}

}  // namespace specnode

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

static size_t SpecDelayBankFrames() {
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

bool SpectatorNode_PopFrameInputs(uint16_t* p1_input, uint16_t* p2_input) {
    // A validated snapshot is waiting for the local engine to reach its
    // capture phase: hold the sim. Popping before the apply consumed real
    // inputs into throwaway pre-snapshot state -- and pushed the consumed
    // cursor past the anchor, which made the rewind guard discard the
    // FIRST snapshot (join-during-CSS run, 2026-06-11: spec played the
    // live stream on a fresh BTB battle, P2 never initialized).
    if (g_state.pb_snapshot_inbox.pending_apply) return false;

    // Drain non-INPUT events from the head before popping the next INPUT.
    // Each non-INPUT event dispatches to ApplySessionEvent — RNG pin,
    // input-state reset, sound dedup init, etc. The dispatch happens at
    // the moment the spectator's local sim is about to consume the next
    // INPUT, which is the same logical-frame moment the host's pin
    // happened. Eliminates the game_mode-driven mirror race.
    //
    // SEAM extension: between MATCH_END and MATCH_START applies, INPUT
    // events at the head are consumed-and-discarded instead of breaking
    // the drain — they are the host's results/CSS frames and must not
    // drive the spectator's local sim (see PbBoundary). The drain
    // naturally reaches the boundary init ops + MATCH_START, whose apply
    // flips the state to PINNING and stops the discard.
    while (!g_state.pb_queue.empty() &&
           g_state.pb_queue.front().type != SessionEventType::INPUT) {
        ApplySessionEvent(g_state.pb_queue.front());
        g_state.pb_queue.erase(g_state.pb_queue.begin());
    }

    // Battle-entry alignment HOLD (armed by the MATCH_START handler when the
    // host's MATCH_START drains while we're still in CSS). The armed pin is
    // force-locking the host's chars in the engine's CSS hook (independent of
    // the input we feed); we HOLD the battle inputs now sitting at the queue
    // head -- feeding neutral -- until the engine crosses into battle. Because
    // no battle input is consumed during the (variable-length) pin-lock frames,
    // the FIRST popped battle input lands exactly on our bf=0 == the host's
    // bf=0, eliminating the CSS-overrun offset regardless of lock duration.
    if (g_state.pb_battle_align_pending) {
        const uint32_t mode_align = *(uint32_t*)FM2K::ADDR_GAME_MODE;
        if (mode_align >= 3000u) {
            g_state.pb_battle_align_pending = false;
            // The seam confirm-mask ends with the pin. The lean-seam flow
            // arms the pin via THIS branch (boundary already NONE, PINNING
            // never entered), so the PINNING->NONE SetSeamHold(false) can't
            // run -- release here as well.
            CssAutoConfirm_SetSeamHold(false);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: battle-align complete -- engine entered battle; "
                "resuming battle-input pops (bf=0 == host battle-start)");
        } else {
            g_state.pb_current_p1 = 0;
            g_state.pb_current_p2 = 0;
            if (p1_input) *p1_input = 0;
            if (p2_input) *p2_input = 0;
            return true;
        }
    }

    // Boundary handling. SEAM: fall through to the normal pop -- the
    // viewer MIRRORS the host's seam (results presses, CSS cursor
    // movements) at the host's own pace; the seam hold masks confirm
    // bits + locks so the rematch flow can never advance early, and the
    // final picks come from the MATCH_START pin. PINNING: battle INPUTs
    // are parked at the head while CssAutoConfirm walks the local CSS to
    // the announced picks on synthetic neutral; exact pops resume when
    // the local game re-enters battle.
    //
    // Release is EDGE-triggered: the local mode must first drop below
    // 3000 (leave the old match's results screen) before a value >= 3000
    // counts as "the new battle". The old level check released instantly
    // when MATCH_START arrived while results were still on screen (UDP
    // live edge), feeding the new match's inputs into the old screen and
    // letting CssAutoConfirm disengage before CSS opened.
    if (g_state.pb_boundary != State::PbBoundary::NONE) {
        const uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
        if (mode < 3000u) g_state.pb_boundary_left_battle = true;
        if (g_state.pb_boundary == State::PbBoundary::PINNING) {
            if (g_state.pb_boundary_left_battle && mode >= 3000u) {
                g_state.pb_boundary = State::PbBoundary::NONE;
                CssAutoConfirm_SetSeamHold(false);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: boundary PINNING -> NONE (battle entered, "
                    "resuming exact input pops, q=%zu)", g_state.pb_queue.size());
                // fall through to the normal pop below
            } else {
                // Pin walk in progress: park the new match's inputs and
                // feed neutral (CssAutoConfirm drives cursor + confirm
                // directly; the results screen, if still up, advances on
                // a synthetic confirm edge).
                if (!g_state.subscribed_upstream) return false;
                uint16_t synthetic = 0;
                if (mode != 2000u) {
                    static uint32_t s_seam_tick = 0;
                    synthetic = (s_seam_tick++ & 1u) ? 0x010u : 0u;
                }
                g_state.pb_current_p1 = synthetic;
                g_state.pb_current_p2 = synthetic;
                if (p1_input) *p1_input = synthetic;
                if (p2_input) *p2_input = synthetic;
                return true;
            }
        }
        if (g_state.pb_boundary == State::PbBoundary::SEAM) {
            if (mode >= 3000u && !g_state.pb_css_marker_seen) {
                // Our results screens are still running: replay the
                // host's results inputs 1:1 (battle-end state matched,
                // so the pacing matches). Fall through to the normal pop.
            } else if (mode >= 3000u && g_state.pb_css_marker_seen) {
                // Stream already reached the host's CSS but our results
                // overran by a few frames: park the CSS inputs (they
                // mirror from CSS frame 0) and walk the remaining
                // results on synthetic edges.
                if (!g_state.subscribed_upstream) return false;
                static uint32_t s_seam_tick3 = 0;
                const uint16_t synthetic =
                    (s_seam_tick3++ & 1u) ? 0x010u : 0u;
                g_state.pb_current_p1 = synthetic;
                g_state.pb_current_p2 = synthetic;
                if (p1_input) *p1_input = synthetic;
                if (p2_input) *p2_input = synthetic;
                return true;
            } else if (mode == 2000u && !g_state.pb_css_marker_seen) {
                // Our CSS opened before the stream's CSS_ENTERED: the
                // remaining head INPUTs are the host's results tail --
                // discard them (apply ops; one is the marker) and hold
                // neutral so nothing can advance.
                while (!g_state.pb_queue.empty() &&
                       !g_state.pb_css_marker_seen) {
                    const SessionEvent& head = g_state.pb_queue.front();
                    if (head.type != SessionEventType::INPUT) {
                        ApplySessionEvent(head);
                    }
                    g_state.pb_queue.erase(g_state.pb_queue.begin());
                }
                if (!g_state.subscribed_upstream) return false;
                g_state.pb_current_p1 = 0;
                g_state.pb_current_p2 = 0;
                if (p1_input) *p1_input = 0;
                if (p2_input) *p2_input = 0;
                return true;
            } else {
                // CSS open + marker seen: the mirror starts at the host's
                // CSS frame 0. Engage the confirm mask for the WHOLE seam
                // mirror -- no pop countdown. Directions pass through (the
                // cursor still mirrors) but a replayed confirm edge can
                // never lock chars early: a lagging/catch-up viewer replays
                // the seam with edge artifacts (2026-07-17 forensics:
                // spurious act=1/1 ~60 pops early on pass-through cells
                // 11/13 while the host picked 2/14 -> the game match-inited
                // the WRONG chars, the late MATCH_START pin re-locked the
                // right ones mid-init, and the HP reset never re-ran ->
                // battle 2 started with match-1's final HP, full-state
                // desync from bf=2). All locking belongs to the MATCH_START
                // pin, which bypasses this mask (css_autoconfirm masks only
                // when !g_active); the PINNING->NONE release drops the hold
                // at battle entry. The 2026-06-11 "hold never released"
                // deadlock predates the pin and cannot recur.
                g_state.pb_boundary = State::PbBoundary::NONE;
                CssAutoConfirm_SetSeamHold(true, 0xFF, 0xFF);  // mask only
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: lean seam -> mirror (CSS aligned at "
                    "host frame 0, confirm mask held until MATCH_START pin, "
                    "q=%zu)", g_state.pb_queue.size());
                // fall through to the normal pop
            }
        }
    }

    // Results-tail guard: the local game can reach CSS a few frames
    // before the stream's MATCH_END applies (our results screens run
    // slightly short), so pb_boundary is still NONE and the seam hasn't
    // engaged. The queued head INPUTs in that window are the host's
    // results presses -- feeding them to the fresh CSS displaced the
    // cursors before the seam engaged and the whole mirrored dance ran
    // offset (wrong chars + colors at the rematch, 2026-06-11 15:09).
    // Discard them while applying ops; the MATCH_END op flips
    // pb_awaiting_match_end and engages the SEAM, whose machinery takes
    // over on the next call. Hold neutral throughout. Gated on
    // pb_local_battle_seen: only a sim that ALREADY played this match's
    // battle can be in its results tail -- at match entry (and offline
    // replay boot) mode 2000 + awaiting means the battle hasn't started
    // locally yet and the queued INPUTs are the battle itself.
    {
        const uint32_t mode_now = *(uint32_t*)FM2K::ADDR_GAME_MODE;
        if (mode_now >= 3000u && mode_now < 4000u) {
            g_state.pb_local_battle_seen = true;
        }
    }
    if (g_state.pb_awaiting_match_end &&
        g_state.pb_local_battle_seen &&
        g_state.pb_boundary == State::PbBoundary::NONE &&
        *(uint32_t*)FM2K::ADDR_GAME_MODE == 2000u) {
        while (!g_state.pb_queue.empty() && g_state.pb_awaiting_match_end) {
            const SessionEvent& head = g_state.pb_queue.front();
            if (head.type != SessionEventType::INPUT) {
                ApplySessionEvent(head);
            }
            g_state.pb_queue.erase(g_state.pb_queue.begin());
        }
        if (!g_state.subscribed_upstream) return false;
        g_state.pb_current_p1 = 0;
        g_state.pb_current_p2 = 0;
        if (p1_input) *p1_input = 0;
        if (p2_input) *p2_input = 0;
        return true;
    }

    // Post-CSS confirm-mask countdown -- FUNCTION level, not inside the
    // boundary block: engaging the mirror clears pb_boundary in the same
    // call, so a countdown nested in that scope decremented exactly once
    // and the hold never released -- the mirror traced the host's dance
    // to the exact picks but the players' lock-ins could never register
    // (spec sat at CSS until the transport died, 2026-06-11).
    if (g_state.pb_post_css_mask_pops > 0) {
        if (--g_state.pb_post_css_mask_pops == 0) {
            CssAutoConfirm_SetSeamHold(false);
        }
    }

    // Natural-boot walk + mirrored-CSS guards run BEFORE the queue-empty
    // checks: at boot the queue is legitimately EMPTY (the stream hasn't
    // arrived), and the old ordering made the synthetic title presses
    // unreachable -- the viewer sat in the attract demo at q=0 until the
    // host's backfill happened to land (2026-06-11 12:49).
    if constexpr (FM2K::kIsFM2K) {
        static int s_natboot_offline_cached = -1;
        if (s_natboot_offline_cached < 0) {
            const char* v = std::getenv("FM2K_REPLAY_FILE");
            s_natboot_offline_cached = (v && v[0]) ? 1 : 0;
        }
        if (s_natboot_offline_cached == 0) {
            static bool s_css_reached = false;
            const uint32_t mode_now = *(uint32_t*)FM2K::ADDR_GAME_MODE;
            if (mode_now >= 2000u && !s_css_reached) {
                s_css_reached = true;
                // The title-mash press straddles the title->CSS
                // transition: the engine's edge detector reads it as a
                // rising confirm on CSS frame ~1 for BOTH players --
                // instant 0/0 lock, 100-frame countdown, battle before
                // the players ever confirmed (NATCSS trace 2026-06-11:
                // act=1/1 by pop 10, timer==pop). Engage the confirm-
                // masking hold for the first 60 CSS frames to eat the
                // stray edge; released in the post-release guard below.
                if (mode_now == 2000u) {
                    CssAutoConfirm_SetSeamHold(true, 0xFF, 0xFF);  // mask only
                }
            }
            // Bank-building hold: once our CSS is open, do NOT start the
            // mirror until the queue holds the full delay bank -- the
            // players are browsing during this, so the extra idle
            // seconds are invisible, and playback then runs the entire
            // session a fixed bank behind live (arrival gaps shorter
            // than the bank never reach the picture). 15s safety cap
            // for short host CSS phases.
            if (s_css_reached && g_state.natural_boot &&
                !g_state.pb_bank_built && mode_now == 2000u) {
                static uint64_t s_bank_start_ms = 0;
                const uint64_t bnow = GetTickCount64();
                if (s_bank_start_ms == 0) s_bank_start_ms = bnow;
                if (g_state.pb_queue.size() >= SpecDelayBankFrames() ||
                    bnow - s_bank_start_ms > 15000) {
                    g_state.pb_bank_built = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: delay bank built (q=%zu, %llums) "
                        "-- mirror starting",
                        g_state.pb_queue.size(),
                        (unsigned long long)(bnow - s_bank_start_ms));
                } else {
                    g_state.pb_current_p1 = 0;
                    g_state.pb_current_p2 = 0;
                    if (p1_input) *p1_input = 0;
                    if (p2_input) *p2_input = 0;
                    return true;
                }
            }
            if (!s_css_reached) {
                // Keep the boot in the VS context: without the netplay
                // handshake P1/P2 have, the title's attract sequence
                // (title.demo / characterselect.demo) takes over within
                // ~300ms and its auto-CSS locks default chars and starts
                // a demo battle (the 0/0 ryu/ryu "join"). Pin the VS
                // game-mode flag and clear the demo state every tick so
                // the demo can never drive, while the synthetic edges
                // walk the menu.
                *(uint32_t*)0x470058u = 1;   // g_game_mode_flag = VS
                *(uint32_t*)0x47010Cu = 0;   // demo mode state
                uint16_t synthetic = 0;
                if (mode_now == 1000u) {
                    static uint32_t s_nat_title_tick = 0;
                    synthetic = (s_nat_title_tick++ & 1u) ? 0x010u : 0u;
                    static uint64_t s_nat_log_ms = 0;
                    const uint64_t nb_now = GetTickCount64();
                    if (nb_now - s_nat_log_ms > 1000) {
                        s_nat_log_ms = nb_now;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[NATBOOT] mode=%u flag=%u demo=%u menu=%u",
                            mode_now, *(uint32_t*)0x470058u,
                            *(uint32_t*)0x47010Cu,
                            *(uint32_t*)0x424F30u);
                    }
                }
                g_state.pb_current_p1 = synthetic;
                g_state.pb_current_p2 = synthetic;
                if (p1_input) *p1_input = synthetic;
                if (p2_input) *p2_input = synthetic;
                return true;
            }
            // Post-release guard: the demo machinery must stay quiet
            // through the mirrored CSS too (it re-engages on input
            // silence; the early replayed CSS frames are mostly idle).
            if (mode_now == 2000u) {
                *(uint32_t*)0x47010Cu = 0;
                static uint32_t s_natcss_pop = 0;
                if (s_natcss_pop == 60 && g_state.natural_boot) {
                    // Stray title-edge window over; hand CSS to the
                    // live mirror (real confirms must pass from here).
                    // natural_boot-gated: only a natboot walk has a title
                    // edge to eat. A snapshot joiner's first CSS is the
                    // REMATCH SEAM -- its cumulative pop counter reaches
                    // 60 mid-seam and this release stripped the
                    // hold-until-pin mask (2026-07-17: S2 desyncs while
                    // css-join S1, whose counter burned 60 in CSS-1,
                    // stayed clean). Residual: a natboot CSS-1 shorter
                    // than 60 pops would leak the one-shot into its seam;
                    // real CSS phases run far longer.
                    CssAutoConfirm_SetSeamHold(false);
                }
                // [NATCSS] every 10th pop until the mechanism that
                // advances a mirrored CSS to battle is identified --
                // logs the state machine's inputs and progression.
                if ((s_natcss_pop++ % 10u) == 0) {
                    const int* p1c = (const int*)0x424E50;
                    const int* p2c = (const int*)0x424E58;
                    const int* sel = (const int*)0x470020;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[NATCSS] pop=%u p1@(%d,%d) p2@(%d,%d) sel=%d/%d "
                        "act=%u/%u timer=%u q=%zu",
                        s_natcss_pop - 1,
                        p1c[0], p1c[1], p2c[0], p2c[1], sel[0], sel[1],
                        *(uint32_t*)0x47019Cu, *(uint32_t*)0x4701A0u,
                        *(uint32_t*)0x424F00u, g_state.pb_queue.size());
                }
            }
        }
    }

    if (g_state.pb_queue.empty()) return false;
    if (g_state.pb_queue.front().type != SessionEventType::INPUT) return false;

    // Offline-replay gate (FM2K only for now).
    //
    // The .fm2krep file is sliced from MATCH_START → MATCH_END — its INPUTs
    // are battle-phase inputs, not CSS-traversal inputs. If we pop them
    // during the spectator's own CSS phase (driven by the auto-CSS hook's
    // direct memory writes rather than these INPUTs), they get applied to
    // the wrong logical frames and the input timeline misaligns with the
    // host's recording by the count of frames CSS took (~134 in practice).
    // Symptom: rounds may coincidentally match (BATTLE_INIT inputs are
    // mostly neutral), but mid-round positions/scripts are visibly off.
    //
    // Live-spec doesn't have this issue: host streams CSS-traversal inputs
    // from session start, so they consume during the spectator's CSS phase
    // as intended. Gate only fires when FM2K_REPLAY_FILE is set.
    //
    // Pre-battle: feed neutral inputs (p1=p2=0) so PGI+UG still runs and
    // the local CSS state machine advances under the auto-CSS hook's pins;
    // the pb_queue's first real INPUT stays at the head until the local
    // game crosses into mode==3000.
    if constexpr (FM2K::kIsFM2K) {
        static int s_offline_replay_cached = -1;
        if (s_offline_replay_cached < 0) {
            const char* v = std::getenv("FM2K_REPLAY_FILE");
            s_offline_replay_cached = (v && v[0]) ? 1 : 0;
        }
        // Live natural-boot alignment gate (tournament-flow CSS join):
        // the host's stream begins at ITS CSS, so a viewer that boots
        // naturally must NOT let its TITLE screen eat those inputs --
        // that shifted the stream cursor and the viewer's CSS started
        // mid-dance with diverged state (locked early, entered battle
        // BEFORE the players). Park the stream and walk the title on
        // synthetic confirm edges until the local CSS opens; from there
        // the dance replays in true lockstep from the host's CSS frame 0.
        // One-shot: once CSS (or any later phase) has been reached, the
        // gate never re-engages (boundaries are mid-session lockstep).
        if (s_offline_replay_cached == 1) {
            // Latch: gate is active only UNTIL the first mode==3000 entry.
            // The gate's purpose is to keep the queue's first INPUT at the
            // head until the local game crosses into battle so spectator's
            // bf=0 reads host's bf=0 input. Once we've entered battle once,
            // misalignment can't happen anymore — and post-match phases
            // (mode dropping back below 3000 for results / CSS rematch /
            // game-over screens) need queue inputs to drain so trailing
            // ROUND_END / MATCH_END / next match's MATCH_START events
            // can apply. Without this latch, the 6 post-R3 INPUTs in the
            // file's tail would block MATCH_END from ever applying and
            // the spec would freeze with q:7 in the queue.
            static bool s_battle_entered = false;
            const uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
            if (mode >= 3000u) {
                s_battle_entered = true;
            }
            if (!s_battle_entered && mode < 3000u) {
                // Pre-battle: don't pop the queue. Synthesize a sentinel
                // input. Title (mode==1000) needs a confirm-button press
                // edge each frame to advance the menu — alternate
                // 0x010/0x000 so the edge detector fires repeatedly.
                // CSS (mode==2000) gets neutral — CssAutoConfirm pins
                // cursor + action_state directly.
                uint16_t synthetic = 0;
                if (mode == 1000u) {
                    static uint32_t s_title_tick = 0;
                    synthetic = (s_title_tick++ & 1u) ? 0x010u : 0u;
                }
                g_state.pb_current_p1 = synthetic;
                g_state.pb_current_p2 = synthetic;
                if (p1_input) *p1_input = synthetic;
                if (p2_input) *p2_input = synthetic;
                return true;
            }
        }
    }

    const SessionEvent ev = g_state.pb_queue.front();
    g_state.pb_queue.erase(g_state.pb_queue.begin());
    g_state.pb_started    = true;
    uint16_t fed_p1 = ev.u.input.p1;
    uint16_t fed_p2 = ev.u.input.p2;
    // Strip the carried title-skip confirm bit from the first replayed CSS
    // frames. The host's recorded CSS frame 0 carries the title->CSS confirm
    // edge (the 0x010 we feed to skip the title); replayed here with our cursor
    // parked on char 0 it makes game_state_manager auto-confirm Ryu/Ryu (spawns
    // the confirmed full-sprite, subtype 81) BEFORE the players ever confirmed.
    // The css_autoconfirm seam-hold mask is meant to catch this but misses the
    // exact mirror-start frame (confirm fires with in_changes=0x10 unmasked).
    // The host's REAL confirms come many seconds later (after navigation), so a
    // short strip window at CSS entry is safe and never eats a real lock-in.
    {
        static uint32_t s_mirror_css_strip = 0;
        const uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
        if (mode == 2000u) {
            if (s_mirror_css_strip < 30u) {
                s_mirror_css_strip++;
                fed_p1 &= ~0x3F0u;   // strip confirm/color bits
                fed_p2 &= ~0x3F0u;
            }
        } else {
            s_mirror_css_strip = 0;  // re-arm per CSS phase (rematch)
        }
    }
    g_state.pb_current_p1 = fed_p1;
    g_state.pb_current_p2 = fed_p2;
    if (p1_input) *p1_input = fed_p1;
    if (p2_input) *p2_input = fed_p2;
    return true;
}

uint16_t SpectatorNode_GetCurrentP1Input() { return g_state.pb_current_p1; }
uint16_t SpectatorNode_GetCurrentP2Input() { return g_state.pb_current_p2; }
size_t   SpectatorNode_PendingFrameCount() { return g_state.pb_queue.size(); }

void SpectatorNode_ResetCurrentInputs() {
    g_state.pb_current_p1 = 0;
    g_state.pb_current_p2 = 0;
}
