// Netplay battle FRAME PROCESSING: HandleFrameTime + Netplay_ProcessBattleInputPhase
// (the rollback frame driver) + GetInput + ProcessSpectatorPhase. Extracted VERBATIM
// from netplay.cpp; shares state + perf instrumentation via netplay_internal.h.
// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
//
// ENGINE-AGNOSTIC: this driver is the part FM95 reuses verbatim. It contains
// NO engine-specific memory addresses -- local input routes through Input_*,
// the sim ticks through original_process_game_inputs/update_game (engine
// globals), and the per-event work lives in the engine-aware handlers in
// netplay_battle_events.cpp (Netplay_HandleSaveEvent/LoadEvent/AdvanceEvent).
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

// =============================================================================
// BATTLE FRAME PROCESSING
// =============================================================================

// Frame pacing state (matches GekkoNet examples' handle_frame_time)
LARGE_INTEGER g_perf_freq = {};
LARGE_INTEGER g_frame_start = {};
bool g_frame_timer_initialized = false;

// Called at the END of each frame to apply frame advantage throttle (our
// "frame lengthening", NetherRealm GDC doc 11). The game already has its OWN
// 10ms frame limiter (timeGetTime in WinMain). We only add EXTRA delay when
// ahead of remote, to slow our advance so the peer's late inputs can still
// arrive in time -- trading a smooth slowdown for the jarring net-pause (the
// hard gekko stall when we reach the prediction-window edge) or the rare
// confirm-at-edge desync under heavy loss.
//
// PROGRESSIVE escalation toward the window edge (g_pred_window): the old brake
// flat-lined at +2ms past 4 frames ahead, so a bad connection raced the last
// ~12 frames to the hard stall at near-full speed. NetherRealm's lesson is to
// stretch MORE the closer you get to the pause. Thresholds scale with the actual
// prediction window so an FM2K_PREDICTION_WINDOW override stays proportionate.
// A healthy connection holds < 2 frames ahead and is untouched (0-4 range is
// byte-identical to the prior behavior -- no good-connection regression). The
// max (+5ms -> ~67fps at 100fps) only bites a connection already at the edge,
// where a playable slowdown beats a hard freeze.
void HandleFrameTime(float frames_ahead) {
    // Only throttle when ahead -- the game handles base frame timing
    if (frames_ahead <= 0.5f) {
        return;  // Not ahead, let game's own limiter handle timing
    }
    const float W = (g_pred_window > 4) ? (float)g_pred_window : 16.0f;
    DWORD extra_ms;
    if      (frames_ahead > 0.875f * W) extra_ms = 5;   // near the edge (~14/16)
    else if (frames_ahead > 0.69f  * W) extra_ms = 4;   //              (~11/16)
    else if (frames_ahead > 0.50f  * W) extra_ms = 3;   //              (~8/16)
    else if (frames_ahead > 4.0f)       extra_ms = 2;   // unchanged from prior
    else if (frames_ahead > 2.0f)       extra_ms = 1;   // unchanged from prior
    else {
        // 0.5-2.0: minimal throttle, just yield a timeslice (~0.5ms effective)
        Sleep(0);
        return;
    }
    Sleep(extra_ms);
}

// Collect this peer's local input and feed gekko's slot(s). ENGINE-AGNOSTIC:
// routes through Input_Capture* + gekko_add_local_input only, so FM95 reuses
// it unchanged. The autoplay branches are harness-only (env-gated).
static void CollectAndAddLocalInput() {
    uint16_t local_input = Input_CaptureLocal();
    if (g_stress_mode) {
        // Single-instance determinism test. When FM2K_PARITY_AUTOPLAY_BATTLE
        // is on, drive both gekko slots with per-player autoplay values
        // (deterministic-pseudo-random from g_input_buffer_index+player_id).
        // Without this, gekko sees Input_CaptureLocal (keyboard, typically
        // 0) and the .fm2krep + spec stream record 0/0 -- but the engine
        // sims with autoplay values, so --replay re-runs with 0/0 and
        // diverges from the record at frame 0.
        //
        // Cached env-var check: once active for this run, ALWAYS use
        // autoplay values (including legitimate 0s on phase=0/1 idle
        // frames). Falling back to keyboard on zero would let stale
        // focus-state poison the input stream.
        static int s_autoplay_battle = -1;
        if (s_autoplay_battle < 0) {
            const char* v = std::getenv("FM2K_PARITY_AUTOPLAY_BATTLE");
            s_autoplay_battle = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_autoplay_battle == 1) {
            uint16_t auto_p1 = Hook_ComputeAutoplayBattleInput(0);
            uint16_t auto_p2 = Hook_ComputeAutoplayBattleInput(1);
            gekko_add_local_input(g_session, 0, &auto_p1);
            gekko_add_local_input(g_session, 1, &auto_p2);
        } else {
            // Local 2P: P1 keeps the captured local input (binder slot 0);
            // P2 gets its OWN binder mask (slot 1). Previously both slots got
            // the same local_input -- a leftover from idle-only stress -- which
            // made the P1 controller drive BOTH players, so you couldn't set
            // up a real combo against an independent P2 (e.g. keyboard).
            uint16_t p2_input = Input_CaptureLocalPlayer(1);
            gekko_add_local_input(g_session, 0, &local_input);
            gekko_add_local_input(g_session, 1, &p2_input);
        }
    } else {
        // Harness autoplay for REAL netplay sessions too: each peer
        // drives ITS OWN gekko slot with the deterministic autoplay
        // stream when FM2K_PARITY_AUTOPLAY_BATTLE=1. Without this the
        // loopback netplay/spectator harnesses played IDLE matches
        // (keyboard reads 0x000 headless, nobody moves, HP never
        // changes) -- sync verdicts were real but exercised a
        // near-static sim. Env-gated; production input is untouched.
        static int s_np_autoplay_battle = -1;
        if (s_np_autoplay_battle < 0) {
            const char* v = std::getenv("FM2K_PARITY_AUTOPLAY_BATTLE");
            s_np_autoplay_battle = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_np_autoplay_battle == 1) {
            local_input = Hook_ComputeAutoplayBattleInput(g_player_index);
        }
        // Only feed inputs into a STARTED session. The peer that enters
        // battle first ticks this loop while gekko is still syncing;
        // AddLocalInput stamps those adds with the pre-start frame
        // counter and the input buffer's sequential gate drops/misplaces
        // them -- net effect (cross-peer fork hunt, 2026-06-11): the two
        // peers permanently disagreed about THIS player's input timeline
        // by ~11 frames (= the leader's pre-sync tick count). States
        // matched through the round intro (inputs ignored there), then
        // forked at the first actionable frame (k=71, p1.script_idx 982
        // vs 986) -- the live "transient desync" class users hit.
        // GekkoNet's examples only add inputs in lockstep with a started
        // session.
        if (g_session_ready) {
            // ONE local input per call. This function is exactly one gekko
            // step (add -> update -> advance); the heavy-stage sim/render
            // decouple is orchestrated ONE LEVEL UP in the trampoline's
            // RunBattleTick, which calls this N times per rendered frame to
            // hold the sim at 100fps while display frames drop. Doing the
            // catch-up here (the old approach -- N adds before one
            // update_session) broke gekko's per-frame accounting and desynced
            // (RoHe/Aubeclisse f139); N full add->update->advance cycles do
            // not, because the sim clock is virtual (g_virtual_time_ms =
            // frame*10) so per-peer wall-clock only changes prediction depth,
            // which rollback absorbs. See RunBattleTick for the accumulator.
            gekko_add_local_input(g_session, g_player_index, &local_input);
        }
    }
}

// Drain gekko session events (connect/syncing/disconnect/desync). ENGINE-
// AGNOSTIC: pure gekko bookkeeping + shared-mem outcome publish, no engine
// addresses. FM95 reuses unchanged.
static void HandleBattleSessionEvents() {
    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case GekkoPlayerConnected:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GekkoNet player %d connected",
                    event->data.connected.handle);
                g_session_ready = true;
                break;

            case GekkoSessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GekkoNet session started");
                g_session_ready = true;
                break;

            case GekkoPlayerSyncing:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GekkoNet syncing %u/%u",
                    event->data.syncing.current, event->data.syncing.max);
                break;

            case GekkoPlayerDisconnected: {
                // Peer dropped (timeout / closed game / network died). Publish
                // a DISCONNECT outcome so the launcher's shared-mem poll
                // forwards a match_result to the hub AND tears down the
                // surviving local game. Fires on CSS too -- without this the
                // survivor froze on the character-select screen with music
                // playing when their opponent closed during CSS (real bug
                // report 2026-05-05).
                if (g_session_kind == SessionKind::BATTLE ||
                    g_session_kind == SessionKind::CSS) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Netplay: peer disconnected (handle=%d kind=%d) -- publishing DISCONNECT outcome",
                        event->data.disconnected.handle,
                        (int)g_session_kind);
                    SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_DISCONNECT);
                }
                break;
            }

            case GekkoDesyncDetected: {
                HandleDesyncDetected(
                    event->data.desynced.frame,
                    event->data.desynced.local_checksum,
                    event->data.desynced.remote_checksum,
                    /*synthetic=*/false);
                break;
            }

            default:
                break;
        }
    }
}

bool Netplay_ProcessBattleInputPhase() {
    if (!g_session) return true;

    // In stress mode, skip the network poll (no adapter, no socket).
    if (!g_stress_mode) {
        gekko_network_poll(g_session);

        // Battle-entry signal insurance (match-2 deadlock, 2026-06-11).
        // When the peer's BATTLE_ENTERING arrives during OUR armed window
        // BEFORE our own entry fires, IsBattleSynced latches the instant
        // we enter -- we swap to battle without ever running the
        // PollBattleSync wait loop, which is where both the
        // BATTLE_ENTERING resender and the CSS transport keepalive live.
        // Our single entry signal can then be the ONLY one the peer ever
        // gets; if it's lost (or lands outside their armed window), they
        // starve in CSS spamming proposals we drop as stale, while we sit
        // in an unsyncable battle session at bf=0. There is no entry ACK,
        // but gekko SessionStarted IS one: it can only fire once the peer
        // also swapped and synced. Resend until then, plus poll the
        // control channel so the peer's swap-side traffic keeps flowing.
        if (!g_session_ready && g_local_battle_entered) {
            static uint32_t s_last_entry_resend_ms = 0;
            const uint32_t now_ms = GetTickCount();
            if (now_ms - s_last_entry_resend_ms > 100) {
                // We're past the barrier (battle session exists), so this
                // is a completed-flag announce: peer latches remote=true
                // and won't echo back. Carries our settings digest -- a peer
                // still holding on the match-settings gate needs it to
                // complete, and this resend may be the only packet it gets.
                ControlChannel_SendBattleEntering(g_battle_entry_swap_frame,
                                                  g_entry_done_epoch, 0x1,
                                                  Netplay_MatchSettingsDigest());
                s_last_entry_resend_ms = now_ms;
            }
            ControlChannel_Poll();
        }
    }

    // 1) Collect + add this peer's local input (engine-agnostic).
    CollectAndAddLocalInput();

    // 2) Drain session lifecycle events (engine-agnostic).
    HandleBattleSessionEvents();

    // 3) Pump gekko and dispatch its per-frame update events. Save/Load/Advance
    //    handlers live in netplay_battle_events.cpp; the loop-window state they
    //    accumulate (has_advance / earliest_advance / load_events_in_batch) is
    //    owned here and threaded in by reference.
    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);

    bool has_advance = false;
    // Track the advance-batch window for the Mike Z sound sync. Forward-only
    // batches have earliest == latest; rollback batches span the rewind range.
    uint32_t earliest_advance = UINT32_MAX;
    uint32_t latest_advance = g_netplay_frame;
    // Per-batch LoadEvent counter (1st load/batch = runahead rewind, not a real
    // rollback). See Netplay_HandleLoadEvent for the full rationale.
    int load_events_in_batch = 0;
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case GekkoSaveEvent:
                Netplay_HandleSaveEvent(update);
                break;
            case GekkoLoadEvent:
                Netplay_HandleLoadEvent(update, load_events_in_batch);
                break;
            case GekkoAdvanceEvent:
                Netplay_HandleAdvanceEvent(update, has_advance, earliest_advance);
                break;
            default:
                break;
        }
    }

    // Mike Z sound sync: once per displayed frame, AFTER all advances have
    // completed and BEFORE render. Desired[] now reflects the authoritative
    // sim history for this frame; reconcile to actual DSound plays, applying
    // the rollback-window filter that prevents erased/re-triggered sounds.
    if (has_advance && earliest_advance != UINT32_MAX) {
        latest_advance = g_netplay_frame;
        // Flush CONFIRMED inputs to the recorder/spectator stream. A frame
        // is safe once gekko has REAL inputs from all players for it --
        // predictions can no longer change it. Entries flush in strict
        // frame order; the pi.frame guard stops at frames not yet
        // advanced locally (confirmed horizon can lead our sim).
        if (!g_stress_mode && g_session && g_session_kind == SessionKind::BATTLE) {
            const int confirmed = gekko_confirmed_frame(g_session);
            while ((int)g_next_confirm_flush <= confirmed) {
                const PendingConfirmInput& pi =
                    g_pending_confirm[g_next_confirm_flush % PENDING_CONFIRM_RING];
                if (pi.frame != g_next_confirm_flush) break;
                SpectatorNode_OnFrameConfirmed(pi.p1, pi.p2);
                // [CINPUT]: the CONFIRMED input per battle frame (post gekko
                // confirmation -- not the speculative HOST-FP input). This is the
                // ground truth the harness compares P2 and every spectator against;
                // P1==P2 here proves the players are in lockstep. FM2K_CINPUT.
                static const bool s_cinput = []{
                    const char* v = std::getenv("FM2K_CINPUT");
                    return v && v[0] == '1';
                }();
                if (s_cinput) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[CINPUT] bf=%u p1=0x%03X p2=0x%03X",
                        g_next_confirm_flush, pi.p1, pi.p2);
                }
                g_next_confirm_flush++;
            }
        }

        SoundRollback::SyncAfterAdvance(earliest_advance, latest_advance);
    }

    return has_advance;
}

uint16_t Netplay_GetInput(int player_id) {
    return (player_id == 0) ? g_p1_input : g_p2_input;
}

// Spectator-side per-tick driver. Mirrors Netplay_ProcessBattleInputPhase
// minus the gekko_add_local_input call (spectators have no local input).
// Save/Load events fire only when host's session is in BATTLE config
// (rollback-capable); CSS spectate sessions are pure forward AdvanceEvent.
//
// Returns true if the sim advanced this tick (caller renders); false on
// stall (waiting for confirmed inputs from host).

bool Netplay_ProcessSpectatorPhase() {
    if (!g_session || g_session_kind != SessionKind::SPECTATE) return false;

    gekko_network_poll(g_session);

    // No local input add -- spectator is passive.

    // Drain session events.
    int event_count = 0;
    auto events = gekko_session_events(g_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case GekkoSessionStarted:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: GekkoSpectateSession started");
                g_session_ready = true;
                break;
            case GekkoPlayerConnected:
                g_session_ready = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: handle %d connected",
                    event->data.connected.handle);
                break;
            case GekkoPlayerSyncing:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: syncing %u/%u",
                    event->data.syncing.current, event->data.syncing.max);
                break;
            case GekkoPlayerDisconnected:
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: host disconnected (handle=%d)",
                    event->data.disconnected.handle);
                break;
            case GekkoSpectatorPaused:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: paused -- buffering");
                break;
            case GekkoSpectatorUnpaused:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: unpaused -- playback resumed");
                break;
            case GekkoDesyncDetected:
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Spectator: DESYNC f=%d local=0x%08X remote=0x%08X",
                    event->data.desynced.frame,
                    event->data.desynced.local_checksum,
                    event->data.desynced.remote_checksum);
                break;
            default:
                break;
        }
    }

    // Drain Save/Load/Advance events -- same handlers as the host's battle
    // path, ensuring the spectator's render-side mutations and virtual
    // clock stay locked to the host's confirmed state.
    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);

    bool advanced = false;
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case GekkoSaveEvent: {
                int frame = update->data.save.frame;
                SaveState_Save(frame);
                (void)SaveState_GetLastChecksum(frame);
                // f=0 pre-init constant -- same rationale as the
                // netplay_battle_events.cpp save site (false desync-kill
                // from the frame-0 capture race, 2026-07-18).
                uint32_t checksum = (frame < 1)
                    ? 0x464D3230u
                    : SaveState_GetRegionChecksums().gameplay_fingerprint;
                *update->data.save.state_len = sizeof(uint32_t);
                *update->data.save.checksum  = checksum;
                memcpy(update->data.save.state, &frame, sizeof(uint32_t));
                break;
            }
            case GekkoLoadEvent: {
                int frame = update->data.load.frame;
                SaveState_Load(frame);
                break;
            }
            case GekkoAdvanceEvent: {
                const uint16_t* in = (const uint16_t*)update->data.adv.inputs;
                g_p1_input = in[0];
                g_p2_input = in[1];
                g_netplay_frame = (uint32_t)update->data.adv.frame;
                // Lock virtual clock to host's frame schedule -- same contract
                // as the host's GekkoAdvance handler. This is what closes H3
                // (g_virtual_time_ms skew vs host's rollback-rewinds).
                extern uint32_t g_virtual_time_ms;
                g_virtual_time_ms = g_netplay_frame * 10;

                if (original_process_game_inputs) original_process_game_inputs();
                if (original_update_game)         original_update_game();
                // ParityRecorder::Capture() runs from the trampoline post-tick
                // (mirrors the offline + battle paths) -- keeps the parity
                // header dependency out of netplay.cpp.
                advanced = true;

                // Pending CSS<->battle phase swap: now that the local sim
                // has caught up to the host's announced swap_frame, tear
                // down this session and bring up the next-kind one. Stop
                // draining further events from the now-destroyed session.
                if (MaybeSwapPendingSpectator(g_netplay_frame)) {
                    return advanced;
                }
                break;
            }
            default:
                break;
        }
    }

    return advanced;
}
