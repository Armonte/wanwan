// Netplay battle entry/end SYNC BARRIERS: two-phase swap-frame agreement so both
// peers enter/leave battle on the same frame + the spectator swap broadcast.
// Extracted VERBATIM from netplay.cpp; shares state via netplay_internal.h.
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

// =============================================================================
// BATTLE ENTRY SYNC BARRIER
// Ensures both clients enter battle mode at the same time
// =============================================================================

// Helper: send a BATTLE_ENTERING / BATTLE_END payload to every currently
// subscribed spectator so they can mirror the swap. Called alongside the
// usual unicast-to-remote-peer send.
static void BroadcastSwapToSubscribers(CtrlMsg type, uint32_t swap_frame) {
    auto subs = SpectatorNode_GetSubscriberAddrs();
    for (const auto& addr : subs) {
        CtrlPacket pkt = {};
        pkt.header.type     = type;
        pkt.data.sync.frame = swap_frame;
        ControlChannel_SendTo(pkt, addr);
    }
}

// Boot-to-battle (FM2K_BOOT_TO_BATTLE) test/dev path: the two peers skip the
// CSS rendezvous entirely and boot straight into a battle stage. Normally the
// battle-entry barrier is armed inside the CSS-synced path (g_battle_entry_armed
// = true after Netplay_StartCSSSession). With CSS skipped that never runs, so
// each peer's BATTLE_ENTERING packet is rejected by the other as "out-of-window"
// (armed=0) and both wedge forever at ">>> ENTERING BATTLE MODE - waiting for
// sync". Arm the barrier here so boot-to-battle netplay can sync. epoch stays 0
// (both peers send epoch 0, which the receive handler always accepts), and
// g_css_frame is 0 so both propose the same swap_frame. Production never calls
// this (it always goes through CSS); it's only invoked from the BTB signal site.
void Netplay_ArmBattleEntryBarrier() {
    if (g_battle_entry_armed) return;  // already armed (normal CSS path)
    g_battle_entry_armed    = true;
    g_entry_local_proposal  = 0;
    g_entry_remote_proposal = 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: armed battle-entry barrier for boot-to-battle (no CSS rendezvous)");
}

void Netplay_SignalBattleEntry() {
    if (g_local_battle_entered) {
        return;  // Already signaled
    }

    // Compute our proposed swap frame on the active CSS session. Remote may
    // already have proposed a higher value (we adopt it via the receive
    // handler); take max so we never go backwards.
    const uint32_t local_proposal = g_css_frame + SWAP_FRAME_BUFFER;
    if (local_proposal > g_battle_entry_swap_frame) {
        g_battle_entry_swap_frame = local_proposal;
    }
    g_local_battle_entered = true;
    g_entry_local_proposal = local_proposal;
    ControlChannel_SendBattleEntering(g_battle_entry_swap_frame, g_entry_epoch, 0);
    BroadcastSwapToSubscribers(CtrlMsg::BATTLE_ENTERING, g_battle_entry_swap_frame);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "BATTLE SYNC: Local entered battle mode (css_frame=%u, swap_frame=%u)",
        g_css_frame, g_battle_entry_swap_frame);
}

bool Netplay_IsBattleSynced() {
    // Once the game's CSS detects the battle transition (game_mode -> 3000),
    // the trampoline phase classifier flips us into TRAMPOLINE_BATTLE and we
    // stop driving Netplay_ProcessCSS — so g_css_frame stops advancing. The
    // swap_frame value (g_css_frame + SWAP_FRAME_BUFFER) is therefore
    // unreachable from the CSS session itself. Lockstep already guarantees
    // both peers detect the transition at the same logical frame (the same
    // shared CSS input stream produced the same selected character + lock-in
    // events), so the agreed-on-both-sides BATTLE_ENTERING is enough — no
    // need to also gate on css_frame parity. swap_frame stays in the message
    // for diagnostic logging and future battle-side gating.
    if (!g_battle_synced &&
        g_local_battle_entered &&
        g_remote_battle_entered) {
        g_battle_synced = true;
        // Record completion so the handler can keep ANSWERING the peer's
        // retries after we disarm (deafness fix — see the state block).
        g_entry_done_epoch = g_entry_epoch;
        g_entry_done_ms    = GetTickCount();
        // Completed-flag announce: saves the lagging peer one retry
        // round-trip, and tells an already-completed peer to go silent.
        ControlChannel_SendBattleEntering(g_battle_entry_swap_frame,
                                          g_entry_epoch, 0x1);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BATTLE SYNC: both peers signaled (css_frame=%u, swap_frame=%u, epoch=%u) - swap CSS->battle now",
            g_css_frame, g_battle_entry_swap_frame, g_entry_epoch);
        // CSS divergence canary: in lockstep both sims flip to battle at
        // the same logical frame, so the two proposals should differ by
        // at most transit skew. A large gap means the CSS sims diverged
        // (different chars/colors are likely locked on each side) and
        // the upcoming battle is doomed to desync — make that loudly
        // visible at the moment it's decided, not minutes later.
        if (g_entry_local_proposal != 0 && g_entry_remote_proposal != 0) {
            const uint32_t a = g_entry_local_proposal;
            const uint32_t b = g_entry_remote_proposal;
            const uint32_t gap = (a > b) ? (a - b) : (b - a);
            if (gap > 300) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "BATTLE SYNC: CSS DIVERGENCE SUSPECTED — flip proposals "
                    "%u frames apart (local=%u remote=%u). Both sims should "
                    "leave CSS at the same lockstep frame.",
                    gap, a, b);
            }
        }
    }
    return g_battle_synced;
}

uint32_t Netplay_GetBattleEntrySwapFrame() {
    return g_battle_entry_swap_frame;
}

void Netplay_PollBattleSync() {
    // Poll control channel to receive BATTLE_ENTERING from remote
    ControlChannel_Poll();

    // CSS-session transport keepalive while waiting for the trailing peer
    // (CSS->battle swap deadlock, found 2026-06-11). The peer whose
    // game_mode flips to 3000 first stops running Netplay_ProcessCSS (the
    // phase classifier moves it to the battle wait = this function), so
    // its CSS gekko session went silent: the final CSS input packets the
    // trailing peer still needs to reach ITS OWN detection frame could sit
    // unflushed, ACKs stopped, and the trailing peer either stalled a few
    // frames short of detection forever or hit the 5s gekko disconnect ->
    // CSS_ABORT. 3-for-3 repro in the autoplay loopback harness (which
    // races the flip); real matches usually masked it because humans idle
    // on CSS long enough for the transport to flush. Fix: keep polling the
    // session, keep feeding neutral padding inputs (frames past both
    // peers' detection point -- never consumed by either sim, both flip at
    // the same lockstep-determined frame), and drain-discard its events
    // until both peers signal and the swap runs.
    if (g_session && g_session_kind == SessionKind::CSS) {
        gekko_network_poll(g_session);
        uint16_t neutral = 0;
        gekko_add_local_input(g_session, g_player_index, &neutral);
        int event_count = 0;
        auto events = gekko_session_events(g_session, &event_count);
        for (int i = 0; i < event_count; i++) {
            if (events[i]->type == GekkoPlayerDisconnected) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "BATTLE SYNC: CSS peer disconnected while waiting for "
                    "swap -- publishing CSS_ABORT");
                SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_CSS_ABORT);
            }
        }
        int update_count = 0;
        (void)gekko_update_session(g_session, &update_count);
        // AdvanceEvents drained here are post-detection padding frames;
        // the local sim already left CSS, so they are intentionally not
        // applied.
    }

    // Resend BATTLE_ENTERING until remote acknowledges, carrying the latest
    // agreed swap_frame each time. If remote's proposal arrived higher than
    // ours, the agreed value bumped — keep both sides in sync via resend.
    static uint32_t last_send = 0;
    static uint32_t wait_started = 0;
    static uint32_t last_wait_warn = 0;
    uint32_t now = GetTickCount();
    if (g_local_battle_entered && !g_remote_battle_entered) {
        if (now - last_send > 50) {
            ControlChannel_SendBattleEntering(g_battle_entry_swap_frame,
                                              g_entry_epoch, 0);
            last_send = now;
        }
        if (wait_started == 0) wait_started = now;
        if (now - wait_started > 5000 && now - last_wait_warn > 5000) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "BATTLE SYNC: waiting on remote for %us (swap=%u epoch=%u) — "
                "peer hasn't flipped to battle yet (its CSS sim may be "
                "behind or diverged)",
                (now - wait_started) / 1000, g_battle_entry_swap_frame,
                g_entry_epoch);
            last_wait_warn = now;
        }
    } else {
        wait_started = 0;
    }
}

// =============================================================================
// BATTLE EXIT SYNC BARRIER
// Mirrors the entry barrier; gates battle->CSS swap on agreed swap_frame.
// =============================================================================

void Netplay_SignalBattleEnd() {
    if (g_local_battle_end_signaled) {
        return;
    }

    const uint32_t local_proposal = g_netplay_frame + SWAP_FRAME_BUFFER;
    if (local_proposal > g_battle_end_swap_frame) {
        g_battle_end_swap_frame = local_proposal;
    }
    g_local_battle_end_signaled = true;
    ControlChannel_SendBattleEnd(g_battle_end_swap_frame, g_end_epoch, 0);
    BroadcastSwapToSubscribers(CtrlMsg::BATTLE_END, g_battle_end_swap_frame);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "BATTLE END SYNC: Local left battle mode (battle_frame=%u, swap_frame=%u)",
        g_netplay_frame, g_battle_end_swap_frame);
}

// FM95 deterministic variant. `end_frame` is the confirmed battle->non-battle
// edge, identical on both peers (scanned from the phase ring up to the confirmed
// horizon), so both propose the SAME swap_frame with no per-peer live component.
// FM95_END_SWAP_BUFFER clears the prediction-window lead so swap_frame is ahead
// of both live frames -> both run gekko to it and tear down at one frame.
void Netplay_SignalBattleEndAtFrame(uint32_t end_frame) {
    if (g_local_battle_end_signaled) {
        return;
    }
    const uint32_t local_proposal = end_frame + FM95_END_SWAP_BUFFER;
    if (local_proposal > g_battle_end_swap_frame) {
        g_battle_end_swap_frame = local_proposal;
    }
    g_local_battle_end_signaled = true;
    ControlChannel_SendBattleEnd(g_battle_end_swap_frame, g_end_epoch, 0);
    BroadcastSwapToSubscribers(CtrlMsg::BATTLE_END, g_battle_end_swap_frame);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "BATTLE END SYNC: FM95 confirmed end_frame=%u (deterministic) swap_frame=%u "
        "live=%u", end_frame, g_battle_end_swap_frame, g_netplay_frame);
}

bool Netplay_IsBattleEndSynced() {
    // Same chicken-and-egg as the entry direction: once game_mode leaves
    // the [3000,4000) battle range, the phase classifier flips to CSS and
    // RunBattleTick stops driving the battle session — so g_netplay_frame
    // stops, the swap_frame target is unreachable. Both peers' confirmed
    // exit detection happens at the same logical battle frame (deterministic
    // from shared inputs), so the both-signaled gate is sufficient.
    if (!g_battle_end_synced &&
        g_local_battle_end_signaled &&
        g_remote_battle_end_signaled) {
        g_battle_end_synced = true;
        // Completion record + announce — mirrors the entry barrier (the
        // handler answers post-disarm retries from a lagging peer).
        g_end_done_epoch = g_end_epoch;
        g_end_done_ms    = GetTickCount();
        ControlChannel_SendBattleEnd(g_battle_end_swap_frame, g_end_epoch, 0x1);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "BATTLE END SYNC: both peers signaled (battle_frame=%u, swap_frame=%u, epoch=%u) - swap battle->CSS now",
            g_netplay_frame, g_battle_end_swap_frame, g_end_epoch);
    }
    return g_battle_end_synced;
}

uint32_t Netplay_GetBattleEndSwapFrame() {
    return g_battle_end_swap_frame;
}

// Force-complete timeout (ms) for the battle-end barrier. Under packet loss the
// BATTLE_END signal exchange can fail one-sidedly (your sends drop, or the peer
// is gone) -- the resend loop below only runs while !remote_signaled, so once
// YOU receive the peer's BATTLE_END you stop resending yours, and a peer that
// never got yours spins here forever (the observed 75ms/5%-loss wedge: host
// re-sent BATTLE_END every 50ms with f climbing past swap_frame). Once we've
// REACHED the (deterministic, shared) swap_frame, the peer has reached the same
// frame too -- only the SIGNAL was lost -- so force-completing the swap is safe;
// both peers re-sync at the next (CSS) barrier. Default 8s; FM2K_BARRIER_FORCE_MS.
static uint32_t BattleEndForceMs() {
    static uint32_t c = 0;
    if (c == 0) {
        const char* v = std::getenv("FM2K_BARRIER_FORCE_MS");
        c = (v && v[0]) ? (uint32_t)std::atoi(v) : 8000u;
        if (c < 1000) c = 1000;  // floor: never force before a real loss recovery
    }
    return c;
}

void Netplay_PollBattleEndSync() {
    ControlChannel_Poll();

    static uint32_t last_send = 0;
    static uint32_t wait_started = 0;
    static uint32_t last_wait_warn = 0;
    uint32_t now = GetTickCount();
    if (g_local_battle_end_signaled && !g_remote_battle_end_signaled) {
        if (now - last_send > 50) {
            ControlChannel_SendBattleEnd(g_battle_end_swap_frame,
                                         g_end_epoch, 0);
            last_send = now;
        }
        if (wait_started == 0) wait_started = now;
        if (now - wait_started > 5000 && now - last_wait_warn > 5000) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "BATTLE END SYNC: waiting on remote for %us (swap=%u epoch=%u)",
                (now - wait_started) / 1000, g_battle_end_swap_frame,
                g_end_epoch);
            last_wait_warn = now;
        }
        // Loss-tolerant force-complete: we've reached the deterministic swap
        // frame and the peer hasn't acked our BATTLE_END for too long. Proceed
        // to CSS instead of spinning forever -- the swap point is shared, so
        // this can't desync; the next barrier resynchronizes coordination.
        if (g_netplay_frame >= g_battle_end_swap_frame &&
            (now - wait_started) > BattleEndForceMs()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "BATTLE END SYNC: force-completing after %ums waiting on remote "
                "(reached swap=%u at frame=%u, epoch=%u) — proceeding to CSS",
                now - wait_started, g_battle_end_swap_frame, g_netplay_frame,
                g_end_epoch);
            g_remote_battle_end_signaled = true;  // Netplay_IsBattleEndSynced completes
            wait_started = 0;
        }
    } else {
        wait_started = 0;
    }
}
