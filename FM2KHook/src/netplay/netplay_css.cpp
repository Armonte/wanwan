// Netplay CSS lockstep: PollCSS/CanAdvanceCSS/ProcessCSS/GetCSSInput (delay-based
// character-select sync) + the CSS GekkoSession create/teardown + spectator-actor
// add. Extracted VERBATIM from netplay.cpp; shares state via netplay_internal.h.
// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
#include "netplay.h"
#include "netplay_internal.h"  // shared file-scope state, externed for the split netplay_*.cpp TUs
#include "../hooks/hooks.h"   // Hook_ApplySOCD_Public for SOCD-pre-apply on spec capture
#include "../hooks/css_autoconfirm.h"  // CssAutoConfirm_OnReplayMatchStart (TEST_CSS_CHAR pin)
#include "control_channel.h"
#include "game_hash.h"
#include "input.h"
#include "savestate.h"
#include "spectator_node.h"
#include "nat_traversal.h"
#include "upload_queue.h"
#include "globals.h"
#include "gekkonet.h"
#include "../audio/sound_rollback.h"
#include "../ui/shared_mem.h"  // SharedMem_PublishMatchOutcome
#include "../parity/parity_recorder.h"  // ParityRecorder::Close on harness auto-terminate
#include <SDL3/SDL_log.h>
#include <ws2tcpip.h>
#include <cstdlib>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ctime>
#include <random>
#include <cstdio>
#include <cstring>
#include <atomic>

void Netplay_PollCSS() {
    ControlChannel_Poll();
    if (g_session && g_session_kind == SessionKind::CSS) {
        gekko_network_poll(g_session);
    }
}

bool Netplay_CanAdvanceCSS() {
    // Not synced yet — let game run freely (pre-CSS or waiting for remote)
    if (!g_css_synced) {
        return true;
    }
    // Once the CSS session is up, advance is gated on the AdvanceEvent
    // having fired during the most recent Netplay_ProcessCSS call.
    return g_css_advance_ready;
}

bool Netplay_ProcessCSS() {
    // Poll for incoming control-channel messages (BATTLE_READY rendezvous,
    // BATTLE_ENTERING, etc.) — independent of GekkoNet's transport.
    ControlChannel_Poll();

    // Not connected yet — let game run with local input
    if (g_simple_state < SimpleState::CONNECTED) {
        return true;
    }

    uint32_t now = GetTickCount();

    // Signal we're in CSS
    if (!g_css_active) {
        g_css_active = true;
        g_local_css_ready = true;
        ControlChannel_SendBattleReady();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Entered, signaling remote...");
        // Phase F seam mirror: mark the seam stream so viewers know where
        // the results-screen inputs end and the CSS dance begins. The marker
        // captures the host's CSS start state (cursor + selected) so a
        // snapshot-join spectator -- which carries stale CSS state into a rematch
        // (it never walked CSS1) -- can align its replay walk to the host's.
        SpectatorNode_AppendCssEntered();
    }

    // Keep resending BATTLE_READY until BOTH sides are bilaterally
    // confirmed in the GekkoNet CSS session.
    //
    // Why not gate on `!g_remote_css_ready`? That flag flips true the
    // moment THIS side receives one BATTLE_READY from the peer — which
    // can happen before this side has even entered CSS, because the peer
    // who-entered-first is spamming. Then when this side finally enters
    // CSS, the unconditional first-send fires (line 762) but the spam
    // loop's `!g_remote_css_ready` is already false, so resends stop.
    // If THAT one BATTLE_READY drops on a lossy / high-RTT link, the
    // peer never receives this side's signal and stays stuck forever.
    // Observed live in P1/P2 logs under simulated loss.
    //
    // Gate on `g_css_frame == 0` instead: g_css_frame is incremented in
    // the GekkoNet CSS AdvanceEvent handler, which only fires once
    // BOTH sides have joined the CSS session. So spam keeps going on
    // both sides independently until bilateral sync is genuinely
    // confirmed by a real GekkoNet frame. Once g_css_frame > 0 on a
    // side, both sides have it (frame numbers are agreed). Idempotent
    // BATTLE_READYs in the meantime are harmless (small payload, peer
    // ignores duplicates beyond setting g_remote_css_ready).
    static uint32_t last_ready_send = 0;
    if (g_css_active && g_css_frame == 0 && now - last_ready_send > 100) {
        ControlChannel_SendBattleReady();
        last_ready_send = now;
    }

    // Wait for both clients to be in CSS before bringing up the GekkoNet
    // CSS session. Pre-rendezvous frames run unsynchronized (identical to
    // today's pre-g_css_synced behavior).
    if (!g_remote_css_ready) {
        return true;  // Let game run but don't drive the session yet
    }

    // First frame after rendezvous: reseed RNG and stand up the CSS session.
    if (!g_css_synced) {
        // CRITICAL: Re-seed RNG now that both clients are synced. Pre-CSS
        // frames ran unsynchronized and diverged the RNG. Stage selection
        // uses RNG during CSS->battle transition, so it MUST be identical
        // from this point forward.
        *(uint32_t*)FM2K::ADDR_RANDOM_SEED = Netplay_TestBattleSeed();
        SpectatorNode_AppendPinRng(Netplay_TestBattleSeed());

        // Canonical CSS open (belt-and-braces for the swap-window input
        // guard in Hook_GetPlayerInput): no confirm state and no rematch
        // countdown may survive into the lockstep stream. The engine's
        // own CSS init zeroes these, so in a healthy run this writes 0
        // over 0 -- it only corrects state if some input leaked into the
        // unsynchronized window between CSS init and the first advance.
        *(uint32_t*)FM2K::ADDR_P1_ACTION_STATE = 0;
        *(uint32_t*)FM2K::ADDR_P2_ACTION_STATE = 0;
        if constexpr (FM2K::ADDR_ROUND_TIMER_COUNTER != 0) {
            *(uint32_t*)FM2K::ADDR_ROUND_TIMER_COUNTER = 0;
        }
        // Restart the harness-autoplay browse window for this CSS phase
        // (authoritative per-session reset; the in-function gap heuristic
        // only covers offline runs).
        Hook_AutoplayCssResetDwell();

        if (!Netplay_StartCSSSession()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: Netplay_StartCSSSession failed");
            return false;
        }
        g_css_synced = true;
        g_css_frame  = 0;
        // Arm BATTLE_ENTERING acceptance for this match. Stale packets from
        // the prior match arriving before this point are dropped; from
        // here through the actual battle-session start they're accepted
        // as legitimate signaling. The epoch tags this barrier instance —
        // both peers arm here (bilateral CSS rendezvous) so counters match.
        g_battle_entry_armed = true;
        g_entry_epoch = NextBarrierEpoch();
        g_entry_local_proposal  = 0;
        g_entry_remote_proposal = 0;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CSS SYNCED: Both ready, GekkoNet CSS session up, RNG reseeded");

        // Test-harness char pin (FM2K_TEST_CSS_CHAR=<grid_idx>[,<color>]):
        // arm CssAutoConfirm on BOTH live peers so the netplay CSS
        // deterministically selects a SPECIFIC character mirror instead
        // of confirming char 0 at the grid origin. Needed to reproduce
        // content-specific bugs on the real game (e.g. Bewear=3 in
        // pkmncc, babel's counterhit crash) -- char 0/0 in WonderfulWorld
        // never exercised the same moves/effects. Both peers run the same
        // pin with the same target, so the lockstep stays in step.
        {
            static int s_css_char = -2;
            static int s_css_color = 0;
            if (s_css_char == -2) {
                const char* v = std::getenv("FM2K_TEST_CSS_CHAR");
                if (v && v[0]) {
                    s_css_char = std::atoi(v);
                    const char* comma = std::strchr(v, ',');
                    s_css_color = comma ? std::atoi(comma + 1) : 0;
                } else {
                    s_css_char = -1;  // disabled
                }
            }
            if (s_css_char >= 0) {
                CssAutoConfirm_OnReplayMatchStart(
                    (uint8_t)s_css_char, (uint8_t)s_css_color,
                    (uint8_t)s_css_char, (uint8_t)s_css_color,
                    /*stage_id=*/0);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: TEST_CSS_CHAR pin armed -- both players -> "
                    "char %d color %d (mirror)", s_css_char, s_css_color);
            }
        }
    }

    // Drive the GekkoNet CSS session for this tick.
    g_css_advance_ready = false;
    if (!g_session || g_session_kind != SessionKind::CSS) {
        // Session torn down (e.g., we just swapped to battle); nothing to do.
        return true;
    }

    gekko_network_poll(g_session);

    // Submit local input. With prediction=0, this commits at frame
    // local_frame + CSS_LOCAL_DELAY; AdvanceEvent fires later for the
    // committed frame once the remote's input for that frame arrives.
    uint16_t local_raw = Input_CaptureLocal();
    // Test-harness CSS auto-advance: when FM2K_TEST_AUTO_CSS is set,
    // alternate 0x010 (button A) every other frame so the rising edge
    // fires CSS confirm on both peers. CssAutoConfirm pins cursor /
    // selected_char via its game_state_manager detour; this pulse fills
    // in the missing gekko-delivered input that PGI needs to actually
    // process the confirm. Without it, gekko delivers 0x0000 forever
    // and CSS never advances in netplay mode (CssAutoConfirm overrides
    // engine memory AFTER PGI, but the underlying gekko CSS-delay
    // session needs a real input pulse to keep both peers in sync).
    {
        static int s_test_auto_css = -1;
        if (s_test_auto_css < 0) {
            const char* v = std::getenv("FM2K_TEST_AUTO_CSS");
            s_test_auto_css = (v && v[0]) ? 1 : 0;
        }
        if (s_test_auto_css == 1) {
            static uint32_t s_pulse = 0;
            local_raw = (s_pulse++ & 1) ? 0x010u : 0u;
        }
    }
    // Harness autoplay for netplay CSS (split-brain fix, 2026-06-11):
    // feed OUR slot with the deterministic wander/dwell/confirm stream
    // so the CSS dance travels through the lockstep session and both
    // sims consume the identical (p1, p2) pair. Supersedes the
    // FM2K_TEST_AUTO_CSS pulse above when both envs are set. Mirrors
    // the FM2K_PARITY_AUTOPLAY_BATTLE feed in ProcessBattleInputPhase.
    // Production (env unset) keeps Input_CaptureLocal untouched.
    {
        static int s_np_autoplay_css = -1;
        if (s_np_autoplay_css < 0) {
            const char* v = std::getenv("FM2K_PARITY_AUTOPLAY");
            s_np_autoplay_css = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_np_autoplay_css == 1) {
            local_raw = Hook_ComputeAutoplayCssInput((int)g_player_index);
        }
    }
    gekko_add_local_input(g_session, g_player_index, &local_raw);

    // Drain session events (Connected/Syncing/Disconnected/Desync).
    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case GekkoSessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: GekkoNet CSS session started");
                g_session_ready = true;
                break;
            case GekkoPlayerConnected:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: GekkoNet player %d connected", event->data.connected.handle);
                g_session_ready = true;
                break;
            case GekkoPlayerSyncing:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: GekkoNet syncing %u/%u",
                    event->data.syncing.current, event->data.syncing.max);
                break;
            case GekkoPlayerDisconnected:
                // Peer's CSS-phase Gekko session went silent past
                // DISCONNECT_TIMEOUT. Publish CSS_ABORT (NOT DISCONNECT)
                // so the launcher closes the surviving local game but
                // doesn't record this in W/L/D — battle never started,
                // there's no result to commit. DISCONNECT outcome is
                // reserved for "peer dropped during battle", which IS
                // a forfeit and counts.
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS: peer disconnected (handle=%d) — publishing CSS_ABORT outcome",
                    event->data.disconnected.handle);
                SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_CSS_ABORT);
                break;
            case GekkoDesyncDetected:
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS DESYNC f=%d local=0x%08X remote=0x%08X",
                    event->data.desynced.frame,
                    event->data.desynced.local_checksum,
                    event->data.desynced.remote_checksum);
                break;
            default:
                break;
        }
    }

    // Drain update events. With prediction=0 + limited_saving=false, only
    // AdvanceEvent fires (lockstep mode skips Save/Load — see
    // game_session.cpp:226 / :365 / :537).
    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        if (update->type != GekkoAdvanceEvent) {
            continue;  // Save/Load shouldn't fire under lockstep, but ignore if they do.
        }
        // Inputs are packed in slot order (p1 at index 0, p2 at index 1).
        const uint16_t* in = (const uint16_t*)update->data.adv.inputs;
        g_css_advance_p1    = in[0];
        g_css_advance_p2    = in[1];
        g_css_advance_ready = true;
        g_css_frame         = (uint32_t)update->data.adv.frame + 1;

        // session_history recording moved to Hook_GetPlayerInput where
        // the actual returned input values pass through. That captures
        // pre-rendezvous title-screen / auto-mash inputs too, which this
        // post-AdvanceEvent point misses (no AdvanceEvents fire pre-
        // rendezvous). One canonical log spanning FM2K boot to disconnect.

        if ((g_css_frame - 1) % 100 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: advance frame=%d p1=0x%04X p2=0x%04X",
                update->data.adv.frame, g_css_advance_p1, g_css_advance_p2);
        }
    }

    // No AdvanceEvent this tick → lockstep is waiting on remote → stall.
    return g_css_advance_ready;
}

uint16_t Netplay_GetCSSInput(int player_id) {
    uint16_t input;
    if (player_id == 0) {
        input = g_css_advance_p1;
    } else {
        input = g_css_advance_p2;
    }

    // CCCaster-style: block confirm/cancel for the first CSS_CONFIRM_LOCKOUT
    // frames (moon selector workaround). g_css_frame is one past the last
    // confirmed AdvanceEvent, so the "current" read frame is g_css_frame - 1.
    const uint32_t read_frame = (g_css_frame > 0) ? g_css_frame - 1 : 0;
    if (read_frame < (uint32_t)CSS_CONFIRM_LOCKOUT) {
        input &= 0x0FF;  // Mask button presses, keep direction bits.
    }

    return input;
}


void AddSubscribedSpectatorsToSession() {
    // Spectators are NOT GekkoSpectator actors. Input distribution to
    // spectators flows over the SpectatorNode INPUT_BATCH path — every
    // confirmed (p1, p2) frame is recorded into session_history at
    // Hook_GetPlayerInput's capture_and_return and the host's
    // FlushBatch broadcasts to every subscriber.
    //
    // Adding spectators to GekkoNet was the wrong architecture — it required
    // host/spectator sub-state to match at session-create time, which is
    // launch-timing dependent and a snapshot transfer to fix. Pure input
    // replay sidesteps all of that: spectator boots → starts consuming
    // host's recorded inputs from frame 0 → walks title→CSS→battle in
    // lockstep with host's recorded execution.
    (void)0;  // intentionally empty — kept as a hook for future per-session
              // setup if needed.
}

// =============================================================================
// GEKKONET SESSION - CSS Lockstep (input_prediction_window = 0)
// =============================================================================

bool Netplay_StartCSSSession() {
    if (g_session) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartCSSSession: session already exists (kind=%d)",
            (int)g_session_kind);
        return g_session_kind == SessionKind::CSS;
    }

    // Compute CSS delay dynamically from current RTT instead of pinning
    // at the conservative CSS_LOCAL_DELAY=6. With prediction=0 lockstep,
    // delay too low for the actual link makes CSS visibly choppy: every
    // frame stalls waiting for peer input. Same formula the battle path
    // uses: ceil(mean_one_way_ms / 10ms), floored at 2, capped at 15.
    // RTT samples come from the existing PING / HELLO ack cycle so this
    // is meaningful by the time we create the CSS session (post-HELLO_ACK).
    int css_delay = CSS_LOCAL_DELAY;  // fallback
    {
        const uint32_t rtt_mean_ms  = ControlChannel_GetRttMs();
        if (rtt_mean_ms > 0) {
            const uint32_t mean_one_way = rtt_mean_ms / 2;
            int d = (int)((mean_one_way + 9) / 10);
            if (d < 2)  d = 2;
            if (d > 15) d = 15;
            css_delay = d;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Creating CSS GekkoSession (lockstep, prediction=0, delay=%d)",
        css_delay);

    GekkoConfig config = {};
    config.num_players              = 2;
    // Allow up to 4 spectators per session; spectator_delay sized for full
    // CSS catch-up. CSS sessions are short (a few hundred frames at most
    // before battle entry), so default is plenty.
    config.max_spectators           = 4;
    config.spectator_delay          = 0;    // see battle-session comment — disables pause-buffer
    // input_history_size: host keeps every confirmed CSS input frame in
    // _net_spectator_queue, capped at this many. Late-joining spectators
    // (last_acked_frame == NULL_FRAME) get the entire history streamed
    // on connect. 60000 frames = 10 min @ 100 FPS — plenty for a CSS
    // lobby session that ran for an unusually long pre-match wait.
    // See vendored/GekkoNet patch + README.md:36.
    config.input_history_size       = 60000;
    config.input_prediction_window  = 0;    // lockstep — IsLockstepActive() in game_session.cpp:520
    config.input_size               = sizeof(uint16_t);
    config.state_size               = sizeof(uint32_t);
    config.desync_detection         = true;
    config.limited_saving           = false;  // No effect in lockstep — Save events suppressed

    gekko_create(&g_session, GekkoGameSession);
    gekko_start(g_session, &config);
    // Fresh session = no GekkoSpectator actors yet. Reset the dedup
    // tracking so any post-boundary spec rejoins re-add cleanly.
    SpectatorNode_ClearGekkoSpectatorTracking();

    auto adapter = CreateMultiplexAdapter();
    gekko_net_adapter_set(g_session, adapter);

    // Refresh remote address string from learned sockaddr (post-HELLO_ACK).
    if (const sockaddr_in* learned = NetSocket_GetRemoteAddr()) {
        if (learned->sin_port != 0) {  // sin_port aliases sin6_port (offset 2)
            std::string actor = NetSocket_GetRemoteActorString();
            snprintf(g_remote_addr, sizeof(g_remote_addr), "%s", actor.c_str());
        }
    }

    for (int i = 0; i < 2; i++) {
        if (i == g_player_index) {
            gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(g_session, i, css_delay);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: Added local player at slot %d (delay=%d)", i, css_delay);
        } else {
            GekkoNetAddress addr = {};
            addr.data = (void*)g_remote_addr;
            addr.size = (int)strlen(g_remote_addr);
            gekko_add_actor(g_session, GekkoRemotePlayer, &addr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CSS: Added remote player at slot %d -> %s", i, g_remote_addr);
        }
    }

    // No runahead in lockstep mode (suppressed by IsLockstepActive at
    // game_session.cpp:537 even if requested).

    // Set kind BEFORE the spectator add — AddSubscribedSpectatorsToSession
    // re-broadcasts SPEC_JOIN_ACK carrying g_session_kind, which spectators
    // use to swap their SpectateSession config to match.
    g_session_kind      = SessionKind::CSS;
    g_session_ready     = false;
    g_css_advance_ready = false;
    g_css_advance_p1    = 0;
    g_css_advance_p2    = 0;
    g_css_frame         = 0;
    g_local_delay       = css_delay;

    AddSubscribedSpectatorsToSession();

    return true;
}

void Netplay_EndCSSSession() {
    if (g_session && g_session_kind == SessionKind::CSS) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Destroying CSS GekkoSession");
        gekko_destroy(&g_session);
        g_session       = nullptr;
        g_session_kind  = SessionKind::NONE;
        g_session_ready = false;
    }
    g_css_advance_ready = false;
}
