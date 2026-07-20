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

// ------------------------------------------------------------------ #66 CSS
// rollback: state ring + sim-per-AdvanceEvent handlers. All dormant unless
// g_css_rollback (FM2K_CSS_ROLLBACK=1). Reuses the battle SaveState machinery
// (SaveState_Save/Load) for the object pool + GAME_STATE (0x470020: sel-grid /
// action-state / round-counts / mode) + RNG + input + char slots -- so the CSS
// controller object and spawned portraits (all POOL objects) roll back for
// free, and NO object suppress/reconcile is needed. Only the CSS-only scalar
// block (cursors / css-timer / 1p-side) lives outside battle's save-set, so we
// snapshot it into a parallel small ring.
namespace {
    constexpr int CSS_STATE_RING = 64;               // match MAX_ROLLBACK_FRAMES
    // SMALL CSS save-set. We deliberately do NOT reuse the battle SaveState (it
    // rolls back the OBJECT POOL, which restores CSS portrait objects that
    // reference the .player sound/sprite buffers the loader frees on every
    // cursor-move reload -- resource_cleanup_manager's GlobalFree then walks a
    // freed block and smashes the heap: the #66 CSS-rollback crash). The CSS
    // SIM (cursor/selection/action) lives entirely in these scalar globals, so
    // rolling back ONLY these is bit-exact; the portrait/cursor OBJECTS are
    // display-only and stay forward-state, so no rolled-back object ever points
    // at a freed buffer.
    struct CssRegion { uintptr_t addr; size_t size; };
    constexpr CssRegion CSS_STATE_REGIONS[] = {
        { 0x41FB1C, 0x0004 },   // g_rand_seed (RNG)
        { 0x4280D8, 0x2008 },   // input history rings (per-slot 1024-frame)
        { 0x447EE0, 0x00A0 },   // input tracking: buffer idx + prev/processed/changes input
        { 0x470020, 0x0220 },   // GAME_STATE: sel-grid, action-state, round-counts, mode, timer
        { 0x424E50, 0x00D8 },   // p1/p2 cursors .. css timer .. 1p side/active
        { 0x541F80, 0x0020 },   // g_input_repeat_state[8] -- process_game_inputs auto-repeat
        { 0x4D1C40, 0x0020 },   // g_input_repeat_timer[8] -- per-slot repeat countdown
    };
    // g_input_repeat_state/timer (0x541F80/0x4D1C40, int[8] each) are read+
    // written every frame by process_game_inputs (WW 0x4146d0) to derive
    // g_processed_input (what the CSS handler consumes); battle never reads them
    // so they'd otherwise be unsaved -- CSS re-sim mutates them and the cursor
    // latches g_selected_char_grid a frame off between peers without them.
    constexpr size_t CSS_STATE_TOTAL =
        0x4 + 0x2008 + 0xA0 + 0x220 + 0xD8 + 0x20 + 0x20;
    struct CssSnapshot { uint32_t frame; uint8_t bytes[CSS_STATE_TOTAL]; };
    CssSnapshot g_css_state_ring[CSS_STATE_RING];

    // CSS-relevant desync checksum for gekko (the battle fingerprint hashes
    // hp/round which are meaningless during CSS). FNV-1a over the CSS sim state.
    uint32_t CssState_Fingerprint() {
        uint32_t h = 2166136261u;
        auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
        mix(*(uint32_t*)0x424E50); mix(*(uint32_t*)0x424E54);  // p1 cursor x,y
        mix(*(uint32_t*)0x424E58); mix(*(uint32_t*)0x424E5C);  // p2 cursor x,y
        mix(*(uint32_t*)0x470020); mix(*(uint32_t*)0x470024);  // g_selected_char_grid[0/1]
        mix(*(uint32_t*)0x47019C); mix(*(uint32_t*)0x4701A0);  // g_action_state[0/1]
        mix(*(uint32_t*)0x424F00);                             // css/round timer
        mix(*(uint32_t*)0x4D1C40); mix(*(uint32_t*)0x4D1C44);  // p1/p2 repeat timer
        return h;
    }

    // NOTE: we deliberately do NOT roll back the controller object. It holds
    // DISPLAY state (portrait pointers +350/+354 into per-peer pool slots) that
    // legitimately differs between peers; rolling it back forces a divergence
    // (measured: transition 262->163, 1-cell->12-cell skew). Portraits stay
    // forward-state and reconcile on confirmed frames.
    void CssState_Save(int frame) {
        if (frame < 0) frame = 0;
        CssSnapshot& slot = g_css_state_ring[frame % CSS_STATE_RING];
        slot.frame = (uint32_t)frame;
        size_t off = 0;
        for (const auto& r : CSS_STATE_REGIONS) {
            std::memcpy(slot.bytes + off, (const void*)r.addr, r.size);
            off += r.size;
        }
    }

    void CssState_Load(int frame) {
        if (frame < 0) frame = 0;
        CssSnapshot& slot = g_css_state_ring[frame % CSS_STATE_RING];
        if (slot.frame != (uint32_t)frame) return;
        size_t off = 0;
        for (const auto& r : CSS_STATE_REGIONS) {
            std::memcpy((void*)r.addr, slot.bytes + off, r.size);
            off += r.size;
        }
    }

    // [CSS-FP] confirmed-emit ring. The parity trace must log each frame's
    // CONFIRMED state, not the first (predicted) advance -- the predicting peer
    // guesses the remote input, emits, then corrects via rollback, and the
    // corrected value was never re-emitted (host had local RIGHT, guest had a
    // predicted 0 for the same frame). So we CAPTURE per-advance (rollback
    // re-advances overwrite the slot) and EMIT only from the confirmed flush
    // (bounded by gekko_confirmed_frame) -- identical discipline to the
    // pending-confirm input ring (e5fe11f).
    struct CssFpEntry {
        uint32_t frame; uint16_t p1, p2;
        int32_t cur[4]; int32_t sel[2]; int32_t act[2];
    };
    CssFpEntry g_cssfp_ring[PENDING_CONFIRM_RING];

    void CssFp_Capture(uint32_t frame, uint16_t p1, uint16_t p2) {
        CssFpEntry& e = g_cssfp_ring[frame % PENDING_CONFIRM_RING];
        e.frame = frame; e.p1 = p1; e.p2 = p2;
        const int32_t* c1 = (const int32_t*)0x424E50;   // g_p1_cursor_pos {x,y}
        const int32_t* c2 = (const int32_t*)0x424E58;   // g_p2_cursor_pos {x,y}
        e.cur[0] = c1[0]; e.cur[1] = c1[1]; e.cur[2] = c2[0]; e.cur[3] = c2[1];
        e.sel[0] = *(const int32_t*)0x470020; e.sel[1] = *(const int32_t*)0x470024;
        e.act[0] = *(const int32_t*)0x47019C; e.act[1] = *(const int32_t*)0x4701A0;
    }

    void CssFp_EmitFrame(uint32_t frame) {
        const CssFpEntry& e = g_cssfp_ring[frame % PENDING_CONFIRM_RING];
        if (e.frame != frame) return;   // never captured (pre-session tick)
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[CSS-FP] fr=%u in=0x%03X/0x%03X cur=(%d,%d)/(%d,%d) sel=%d/%d act=%d/%d",
            e.frame, e.p1, e.p2, e.cur[0], e.cur[1], e.cur[2], e.cur[3],
            e.sel[0], e.sel[1], e.act[0], e.act[1]);
    }

    // Save event: snapshot post-advance state, hand gekko the 4-byte stamp +
    // CSS checksum (identical contract to the battle spectator template).
    void Netplay_CssHandleSave(GekkoGameEvent* update) {
        int frame = update->data.save.frame;
        CssState_Save(frame);
        uint32_t checksum = (frame < 1) ? 0x43535330u /*"CSS0"*/ : CssState_Fingerprint();
        *update->data.save.state_len = sizeof(uint32_t);
        *update->data.save.checksum  = checksum;
        std::memcpy(update->data.save.state, &frame, sizeof(uint32_t));
    }

    void Netplay_CssHandleLoad(GekkoGameEvent* update) {
        CssState_Load(update->data.load.frame);
    }

    // Advance event: run ONE native CSS sim tick with this frame's inputs. The
    // get-input hook returns g_css_advance_p1/p2 (set below), so re-sims consume
    // the right per-frame inputs. Mirrors Netplay_HandleAdvanceEvent (battle).
    void Netplay_CssHandleAdvance(GekkoGameEvent* update) {
        const uint16_t* in = (const uint16_t*)update->data.adv.inputs;
        g_css_advance_p1    = in[0];
        g_css_advance_p2    = in[1];
        g_css_advance_ready = true;
        g_css_frame         = (uint32_t)update->data.adv.frame + 1;
        g_is_rolling_back   = update->data.adv.rolling_back;

        // #66 crash fix lives in the create_game_object detour (css_fastsound):
        // during re-sim it returns an INERT dummy so the game's CSS handler
        // (game_state_manager @0x406FC0) never puts a real portrait -- built
        // from a rolled-back selection but a forward char slot -- into the pool
        // for character_state_machine to walk and crash on. g_is_rolling_back
        // (set above) is the gate. The cursor/selection sim (WrapPosition +
        // g_selected_char_grid writes) is pure and runs normally.
        if (original_process_game_inputs) original_process_game_inputs();
        if (original_update_game)         original_update_game();
        ++g_sim_step_count;   // counts re-sims too (sim-fps)

        // Capture this frame's state + inputs into BOTH rings on EVERY advance
        // (not just the first/predicted one). Rollback re-advances overwrite the
        // slot with the corrected values, so by the time the flush reaches a
        // frame (gekko_confirmed_frame), both rings hold its CONFIRMED state.
        const uint32_t f = (uint32_t)update->data.adv.frame;
        auto& slot = g_pending_confirm[f % PENDING_CONFIRM_RING];
        slot.frame = f;
        slot.p1 = Netplay_GetCSSInput(0);   // MASKED value the engine consumed
        slot.p2 = Netplay_GetCSSInput(1);
        CssFp_Capture(f, g_css_advance_p1, g_css_advance_p2);

        if (!update->data.adv.rolling_back && !update->data.adv.running_ahead)
            ParityRecorder::Capture();   // .pty per non-speculative advance
        g_is_rolling_back = false;
    }

    // Flush confirmed CSS inputs to the replay/spectator stream, bounded by the
    // gekko-confirmed horizon (mirror the battle flush). Predictions can no
    // longer change a frame once gekko_confirmed_frame covers it.
    void Netplay_CssFlushConfirmed() {
        if (!g_session || g_session_kind != SessionKind::CSS) return;
        const int confirmed = gekko_confirmed_frame(g_session);
        while ((int)g_next_confirm_flush <= confirmed) {
            const PendingConfirmInput& pi =
                g_pending_confirm[g_next_confirm_flush % PENDING_CONFIRM_RING];
            if (pi.frame != g_next_confirm_flush) break;  // not yet advanced
            SpectatorNode_OnFrameConfirmed(pi.p1, pi.p2);
            CssFp_EmitFrame(g_next_confirm_flush);   // [CSS-FP] parity, CONFIRMED
            g_next_confirm_flush++;
        }
    }
}  // namespace

bool Netplay_IsCssRollbackRecording() {
    return g_css_rollback && g_session && g_css_synced &&
           g_session_kind == SessionKind::CSS;
}

void Netplay_CssFlushRemaining() {
    if (!g_css_rollback) return;
    // At battle-entry both peers have mutually confirmed CSS, so every CSS
    // frame up to g_css_frame is final. Drain any confirmed-but-unflushed ring
    // frames (in strict order) so the spectator receives the whole CSS stream
    // including the confirm frame that flips its game_mode.
    while (g_next_confirm_flush < g_css_frame) {
        const PendingConfirmInput& pi =
            g_pending_confirm[g_next_confirm_flush % PENDING_CONFIRM_RING];
        if (pi.frame != g_next_confirm_flush) break;
        SpectatorNode_OnFrameConfirmed(pi.p1, pi.p2);
        CssFp_EmitFrame(g_next_confirm_flush);
        g_next_confirm_flush++;
    }
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
        // #66: also zero the auto-repeat input state at the sync frame. BATTLE
        // runs process_game_inputs (0x4146d0) and mutates g_input_repeat_state
        // @0x541F80 + g_input_repeat_timer@0x4D1C40 (int[8] each), but never
        // saves/restores them (menu-only), so its rollback re-sims leave the two
        // peers with DIVERGENT repeat state. Carrying that into the (re)match CSS
        // is a divergent initial condition that save/restore cannot repair --
        // both peers must realign here. Also covers CSS's own re-sim (they ARE
        // in CssState now). FM2K only; addresses are FM2K globals.
        if constexpr (!FM2K::kIsFM95) {
            std::memset((void*)0x541F80, 0, 0x20);   // g_input_repeat_state[8]
            std::memset((void*)0x4D1C40, 0, 0x20);   // g_input_repeat_timer[8]
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
            if constexpr (FM2K::kIsFM95) {
                // FM95 CSS nav = attack pulse off the input ring (RE-9). The
                // FM2K Hook_ComputeAutoplayCssInput reads game_mode==2000
                // (never true on FM95) → would feed 0 and both peers stall in
                // CSS with p1/p2=0x0000. Fall back to 0 pre-battle if nav is
                // off (shouldn't happen — this branch is autoplay-gated).
                const int nav = Fm95ComputeAutoplayNav();
                local_raw = (nav >= 0) ? (uint16_t)nav : 0u;
            } else
            local_raw = Hook_ComputeAutoplayCssInput((int)g_player_index);
        }
    }
    // Under rollback, gate local adds on a started session (mirror e5fe11f):
    // pre-sync ticks would misstamp the input timeline. Lockstep adds
    // unconditionally (its stall model tolerates pre-sync adds).
    if (!g_css_rollback || g_session_ready) {
        gekko_add_local_input(g_session, g_player_index, &local_raw);
    }

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

    // Drain update events. Lockstep (default): only AdvanceEvent fires
    // (Save/Load suppressed at game_session.cpp:226/365/537), and the sim runs
    // ONCE back in RunCssTick. Rollback (#66): Save/Load/Advance all fire and
    // the sim runs PER AdvanceEvent HERE (re-sim), like the battle phase.
    int update_count = 0;
    auto updates = gekko_update_session(g_session, &update_count);
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        if (g_css_rollback) {
            switch (update->type) {
                case GekkoSaveEvent:    Netplay_CssHandleSave(update);    break;
                case GekkoLoadEvent:    Netplay_CssHandleLoad(update);    break;
                case GekkoAdvanceEvent: Netplay_CssHandleAdvance(update); break;
                default: break;
            }
            continue;
        }
        // ---- lockstep path (unchanged) ----
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

    // Under rollback, flush confirmed CSS inputs to replay/spectators (bounded
    // by gekko_confirmed_frame). Lockstep records via Hook_GetPlayerInput.
    if (g_css_rollback) Netplay_CssFlushConfirmed();

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

// [CSS-FP] parity fingerprint (#66 Phase 1). CSS-phase determinism was
// previously unverified across peers -- every harness gate (CINPUT/CHECKSUM/
// rng-hp) is battle-frame-keyed. This dense per-CSS-frame log is emitted by
// host, guest, AND spectator so the harness can align the three streams by
// the confirmed (p1,p2) input sequence (like the battle CINPUT gate) and
// assert bit-exact CSS cursors / selected chars / action states. It's the
// safety net that must catch any CSS determinism or spectate/replay break --
// especially once CSS gains a rollback prediction window (#66 Phase 2).
// Always-on during CSS netplay: CSS is short (a few hundred frames) and quill
// is async, so it also makes wild CSS desyncs (para/Ricky class) visible.
void Netplay_EmitCssFp(uint32_t frame, uint16_t p1, uint16_t p2) {
    const int32_t* p1cur = (const int32_t*)0x424E50;  // g_p1_cursor_pos {x,y}
    const int32_t* p2cur = (const int32_t*)0x424E58;  // g_p2_cursor_pos {x,y}
    const int32_t  p1sel = *(const int32_t*)0x470020; // g_selected_char_grid[0]
    const int32_t  p2sel = *(const int32_t*)0x470024; // g_selected_char_grid[1]
    const int32_t  p1act = *(const int32_t*)0x47019C; // g_action_state[0]
    const int32_t  p2act = *(const int32_t*)0x4701A0; // g_action_state[1]
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[CSS-FP] fr=%u in=0x%03X/0x%03X cur=(%d,%d)/(%d,%d) sel=%d/%d act=%d/%d",
        frame, p1, p2, p1cur[0], p1cur[1], p2cur[0], p2cur[1],
        p1sel, p2sel, p1act, p2act);
}

// Host/guest emit -- reads the confirmed CSS frame + inputs from the gekko
// advance state (file-scope). Called from RunCssTick after the native sim
// tick, so the cursor/selection state is post-update for `g_css_frame - 1`.
void Netplay_EmitCssFpHost() {
    const uint32_t fr = (g_css_frame > 0) ? g_css_frame - 1 : 0;
    Netplay_EmitCssFp(fr, g_css_advance_p1, g_css_advance_p2);
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
    // at the conservative CSS_LOCAL_DELAY=6.
    //
    // CSS is PURE LOCKSTEP (prediction=0) -- and it stays that way on
    // purpose: rollback is unsafe here because cursor hover triggers
    // synchronous .player file loads, so a re-sim would re-fire that disk
    // I/O and stall harder. The catch is that lockstep STALLS unless the
    // input-delay buffer covers the full one-way latency INCLUDING jitter:
    // when delay*10ms < one-way latency, every frame waits for the peer and
    // CSS crawls (measured collapse: 140ms->69fps, 220ms->54fps, 300ms->
    // 45fps on loopback; worse with real jitter). para<->Ricky (2026-07-20)
    // negotiated delay=11-12 on a link whose real latency+jitter exceeded
    // it -> CSS ran ~30fps and the match timed out before either could pick.
    //
    // The old formula used MEAN RTT with a cap of 15 (150ms) and NO jitter
    // margin -- para/Ricky negotiated delay=11-12 and stalled because their
    // real latency+jitter exceeded that. The fix keeps MEAN RTT (NOT worst:
    // the worst-RTT sample at CSS-creation is polluted by boot/handshake
    // latency and grossly over-delays -- measured a "LAN" at delay=16), adds
    // a fixed jitter margin, and raises the cap to 25 (250ms). The asymmetry
    // is deliberate: too-LOW CSS delay is CATASTROPHIC (30fps freeze -> the
    // match times out before you can pick), too-HIGH is only a slightly laggy
    // cursor -- a menu tolerates it. So bias up. FM2K_CSS_DELAY overrides for
    // extreme links / diagnostics.
    int css_delay = CSS_LOCAL_DELAY;  // fallback
    if (const char* e = std::getenv("FM2K_CSS_DELAY"); e && e[0]) {
        int d = std::atoi(e);
        if (d >= 2 && d <= 60) css_delay = d;
    } else {
        const uint32_t rtt_mean_ms = ControlChannel_GetRttMs();
        if (rtt_mean_ms > 0) {
            const uint32_t mean_one_way = rtt_mean_ms / 2;
            int d = (int)((mean_one_way + 9) / 10) + 4;   // ceil + 4-frame jitter margin
            if (d < CSS_LOCAL_DELAY) d = CSS_LOCAL_DELAY;
            if (d > 25) d = 25;                           // 250ms cap
            css_delay = d;
        }
    }
    // #66: CSS rollback opt-in. Default OFF -> lockstep (prediction=0), the
    // shipping behavior unchanged. When FM2K_CSS_ROLLBACK=1, add a prediction
    // window so lockstep STALLS become rollbacks (no 30fps freeze / timeout on
    // high-RTT links) -- the .player load re-fire hazard is handled by the
    // re-sim loader-suppress + full object-pool save/restore (see CssState_*).
    {
        static int s_css_rb = -1;
        if (s_css_rb < 0) {
            const char* v = std::getenv("FM2K_CSS_ROLLBACK");
            s_css_rb = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        g_css_rollback = (s_css_rb == 1);
    }
    int css_pred = 0;
    if (g_css_rollback) {
        css_pred = 8;   // frames of speculative rewind for CSS (small: nav is simple)
        if (const char* e = std::getenv("FM2K_CSS_PREDICTION"); e && e[0]) {
            int p = std::atoi(e);
            if (p >= 0 && p <= 64) css_pred = p;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Creating CSS GekkoSession (%s, prediction=%d, delay=%d)",
        g_css_rollback ? "rollback" : "lockstep", css_pred, css_delay);

    GekkoConfig config = {};
    config.num_players              = 2;
    // Allow up to 4 spectators per session; spectator_delay sized for full
    // CSS catch-up. CSS sessions are short (a few hundred frames at most
    // before battle entry), so default is plenty.
    // GekkoNet NATIVE-spectator actor cap (a SECONDARY confirmed-input path).
    // This is intentionally NOT the same number as SPECTATOR_DEFAULT_CAPACITY
    // (=32), which is the SpectatorNode direct-subscriber cap (TCP/RC — the
    // PRIMARY spectator transport). SpectatorNode serves everyone up to 32;
    // the native actor path is a small extra and is separately hard-bounded in
    // spec_join.cpp (kMaxGekkoSpectators). Keep low — GekkoNet iterates all
    // actors per tick.
    config.max_spectators           = 4;
    config.spectator_delay          = 0;    // see battle-session comment — disables pause-buffer
    // input_history_size: host keeps every confirmed CSS input frame in
    // _net_spectator_queue, capped at this many. Late-joining spectators
    // (last_acked_frame == NULL_FRAME) get the entire history streamed
    // on connect. 60000 frames = 10 min @ 100 FPS — plenty for a CSS
    // lobby session that ran for an unusually long pre-match wait.
    // See vendored/GekkoNet patch + README.md:36.
    config.input_history_size       = 60000;
    config.input_prediction_window  = css_pred;  // 0 = lockstep (default); >0 = #66 rollback
    config.input_size               = sizeof(uint16_t);
    config.state_size               = sizeof(uint32_t);  // dummy stamp; real state in CssState ring
    config.desync_detection         = true;
    config.limited_saving           = false;  // lockstep: Save suppressed; rollback: Save per frame

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
    NetSocket_PinGekkoActorAddr();  // rebind survival: pin this session's actor

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
    // #66: confirmed-input ring is reset per session (battle resets it in
    // Netplay_StartBattle). Under CSS rollback we record replay/spectator
    // inputs confirmed-only through this ring (mirror e5fe11f) so speculative
    // CSS inputs never leak into .fm2krep / the live spectator stream.
    if (g_css_rollback) ResetConfirmRing();

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
