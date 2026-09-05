// Netplay spectate session: passive observer of a remote host (GekkoSpectator),
// host->spectator battle-swap handoff + the pending-spectator frame swap. Extracted
// VERBATIM from netplay.cpp; shares state via netplay_internal.h.
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
// GEKKONET SESSION - Spectate (passive observer of a remote host)
// =============================================================================

// Build the GekkoConfig for a spectator session, mirroring the host's config
// for the given phase. Spectators only need session_type, input_size,
// state_size, and spectator_delay -- num_players + prediction_window are
// informational but kept consistent with host for clarity.
static GekkoConfig MakeSpectateConfig(SessionKind host_kind) {
    GekkoConfig config = {};
    config.num_players      = 2;
    config.max_spectators   = 0;     // Spectator doesn't accept further spectators via gekko
    config.input_size       = sizeof(uint16_t);
    config.state_size       = sizeof(uint32_t);
    config.desync_detection = true;
    config.limited_saving   = false;
    // spectator_delay = 0: disable GekkoNet's spectator pause-buffer.
    // With > 0, SpectatorSession::ShouldDelaySpectator gates AdvanceEvent
    // emission until min_received - current >= delay frames, which can
    // never converge on low-latency connections (current advances each
    // tick once unpaused; min only advances on incoming packets, so the
    // diff stays ~stable). 0 == always advance as fast as inputs arrive.
    config.spectator_delay     = 0;
    // Late-joiner backfill: receive buffer holds up to ~10 minutes of
    // confirmed inputs (60000 frames @ 100 FPS, 4B each = ~240 KB). Lets
    // the spectator's local sim FF through the host's full session
    // history on connect. See vendored/GekkoNet patch for README.md:36.
    config.input_history_size  = 60000;
    if (host_kind == SessionKind::BATTLE) {
        // Must match the players' battle prediction_window (16) so the
        // spectator's rollback + desync-checkpoint budget lines up with the
        // session it's mirroring. Was stale at 8.
        config.input_prediction_window = 16;
    } else {
        // CSS or anything else → lockstep config
        config.input_prediction_window = 0;
    }
    return config;
}

NetplaySessionKind Netplay_GetSessionKind() {
    return g_session_kind;
}

GekkoSession* Netplay_GetActiveSession() {
    return g_session;
}

bool Netplay_StartSpectateSession(NetplaySessionKind host_kind, const char* host_addr) {
    // Idempotent: if a SpectateSession is already alive against the same
    // host with the same kind, don't tear it down. SpectatorNode's silence-
    // failover currently fires on legacy 0xCE quiet (input streaming dead)
    // and re-runs JOIN_REQ → JOIN_ACK → here. Without this guard we'd
    // destroy a healthy session every 5 seconds and re-handshake.
    if (g_session && g_session_kind == SessionKind::SPECTATE &&
        host_addr && host_addr[0] && std::strcmp(g_remote_addr, host_addr) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartSpectateSession: session already alive for %s -- keeping",
            host_addr);
        return true;
    }
    if (g_session) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartSpectateSession: session already exists (kind=%d) -- destroying first",
            (int)g_session_kind);
        gekko_destroy(&g_session);
        g_session = nullptr;
        g_session_kind = SessionKind::NONE;
    }

    if (!host_addr || !host_addr[0]) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Netplay_StartSpectateSession: empty host_addr");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Creating SpectateSession (host_kind=%d, host=%s)",
        (int)host_kind, host_addr);

    GekkoConfig config = MakeSpectateConfig(host_kind);

    gekko_create(&g_session, GekkoSpectateSession);
    gekko_start(g_session, &config);
    g_session_kind = SessionKind::SPECTATE;
    SpectatorNode_ClearGekkoSpectatorTracking();

    auto adapter = CreateMultiplexAdapter();
    gekko_net_adapter_set(g_session, adapter);

    // Add host as the (single) remote actor we receive input forwards from.
    GekkoNetAddress addr = {};
    addr.data = (void*)host_addr;
    addr.size = (int)strlen(host_addr);
    gekko_add_actor(g_session, GekkoRemotePlayer, &addr);

    g_session_ready     = false;
    g_p1_input          = 0;
    g_p2_input          = 0;
    g_netplay_frame     = 0;
    g_css_advance_ready = false;

    return true;
}

void Netplay_EndSpectateSession() {
    if (g_session && g_session_kind == SessionKind::SPECTATE) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Destroying SpectateSession");
        gekko_destroy(&g_session);
        g_session       = nullptr;
        g_session_kind  = SessionKind::NONE;
        g_session_ready = false;
    }
}

// Spectator-side phase-flip handlers, called from the BATTLE_ENTERING /
// BATTLE_END control-channel handlers when g_session_kind == SPECTATE.
//
// Host phase-change announcements, received by a legacy GekkoSpectateSession
// viewer. These are TRACE ONLY: the actual CSS<->battle transition for a
// spectator is driven by the stream's own MATCH_START / MATCH_END ops, not by
// these control messages. The pending-swap arm that used to live here (and the
// MaybeSwapPendingSpectator drain it fed) is gone -- its only consumer was
// Netplay_ProcessSpectatorPhase, which never had a caller, so the arm could
// never fire. See the HandleJoinAck comment in spec_join_viewer.cpp for the
// full three-fact retirement. The log lines stay: they are the only record
// that the host announced a swap at all.
void Netplay_OnHostBattleEntering(uint32_t swap_frame) {
    if (g_session_kind != SessionKind::SPECTATE) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Spectator: host entering battle (swap_frame=%u) -- announcement only, "
        "the stream's MATCH_START op drives the transition",
        swap_frame);
}

void Netplay_OnHostBattleEnd(uint32_t swap_frame) {
    if (g_session_kind != SessionKind::SPECTATE) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Spectator: host ending battle (swap_frame=%u) -- announcement only, "
        "the stream's MATCH_END op drives the transition",
        swap_frame);
}

