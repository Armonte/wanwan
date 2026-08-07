// Spectator join protocol, VIEWER side: JOIN_REQ emission, JOIN_ACK/REDIRECT
// handling, SESSION_END latch, the /F-hold join kick and the relay fallback.
// Split VERBATIM out of spec_join.cpp when that TU reached the 1000-line cap;
// the host half (accept/redirect + JOIN_ACK construction) stayed there and the
// two share spec_join_internal.h. Public API decls in spectator_node.h;
// reaches specnode helpers via using.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_join_internal.h"       // shared with spec_join.cpp
#include "spectator_tcp.h"        // TCP transport for INPUT_BATCH stream
#include "control_channel.h"
#include "nat_traversal.h"        // GetRelayAddr for the spec-relay fallback (#58)
#include "netplay.h"
#include "netplay_state.h"
#include "../hooks/css_autoconfirm.h" // Replay-mode CSS lock-and-confirm
#include "../hooks/per_game_patches.h" // PerGamePatches_SetRuntimeBtbOverrides

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
using namespace specnode;

// Viewer side: upstream sent SPEC_SESSION_END. Unlike SPEC_LEAVE (which fails
// over to root), this means the whole session is OVER -- set session_ended so
// the reconnect path in TickHealth stays parked instead of storming a dead host.
void SpectatorNode_HandleSessionEnd(const sockaddr_in& from) {
    // Deliberately NOT gated on subscribed_upstream: RequestJoin clears that
    // flag, so a SESSION_END landing inside a re-JOIN window -- exactly when a
    // shutting-down host has stopped ACKing -- was dropped on the floor and
    // the viewer then reconnect-stormed a dead host. root_addr is accepted
    // alongside upstream_addr because the relay fallback moves upstream_addr
    // off the real host while every re-JOIN goes back to root. Not open to
    // arbitrary peers: both are written only by InitAsSpectator/RequestJoin
    // and are zero-port in any process that never asked to spectate.
    const bool from_upstream = g_state.upstream_addr.sin_port != 0 &&
                               AddrEqual(g_state.upstream_addr, from);
    const bool from_root     = g_state.root_addr.sin_port != 0 &&
                               AddrEqual(g_state.root_addr, from);
    if (!from_upstream && !from_root) return;
    if (g_state.session_ended) return;   // broadcast is sent 3x; log once
    char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: upstream %s sent SPEC_SESSION_END -- session "
                "over, stopping (no reconnect; subscribed=%d)",
                buf, (int)g_state.subscribed_upstream);
    g_state.session_ended = true;
    g_state.subscribed_upstream = false;
}


bool SpectatorNode_RequestJoin(const sockaddr_in& upstream, SpecJoinMode mode) {
    // Drop the pre-subscribe stash ONLY when the upstream actually changes.
    // RC already ACKed everything in it, so the transport will never resend:
    // clearing on a routine re-JOIN to the SAME host destroyed a burst that
    // only another full host-side re-ship could replace. That was the
    // total=0-forever freeze -- KickJoin re-JOINed faster (1s) than the host's
    // 3s destructive-reset floor, so a JOIN_ACK that finally landed replayed
    // an EMPTY stash against a host that already considered the sub
    // backfilled, and live RC_CHAN_SPEC batches cannot anchor
    // have_frame_baseline by themselves. Redirect and relay-fallback DO change
    // the upstream (different endpoint, different burst) and still clear. The
    // kept stash is deliberately not trimmed for headroom: what is stashed is
    // what RC delivered COMPLETELY (the lost datagram was the unreliable ACK,
    // not the payload), so evicting it to make room for a re-ship would trade
    // a whole burst for a partial one.
    const bool upstream_changed = !AddrEqual(g_state.upstream_addr, upstream);
    g_state.upstream_addr       = upstream;
    g_state.subscribed_upstream = false;
    if (upstream_changed) {
        g_state.pre_sub_stash.clear();
        g_state.pre_sub_stash_bytes = 0;
    } else if (!g_state.pre_sub_stash.empty()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: re-JOIN to same upstream -- KEEPING %zu stashed "
            "pre-subscribe msg(s) / %zu B",
            g_state.pre_sub_stash.size(), g_state.pre_sub_stash_bytes);
    }
    g_state.session_ended       = false;  // fresh join clears any prior SESSION_END
    // Re-armed by the next JOIN_ACK that actually grants BATTLE. Clearing here
    // keeps the backstop keyed to the CURRENT grant, so a re-JOIN that comes
    // back as FULL_SESSION cannot leave a stale "expecting a snapshot" flag
    // firing against a stream that correctly has none.
    g_state.spec_boot_expects_snapshot = false;
    g_state.last_requested_mode = mode;  // sticky -- see comment in State decl
    // Bump reconnect timestamp so TickHealth's failover backoff covers the
    // INITIAL JOIN_REQ too. Without this, last_reconnect_attempt_ms stays
    // 0 after the first send, the next TickHealth pass sees
    // `now - 0 >= BACKOFF_MS`, and fires a second RequestJoin within
    // milliseconds -- clobbering the spectator's declared mode before the
    // host's first JOIN_ACK even arrives.
    g_state.last_reconnect_attempt_ms = GetTickCount64();
    g_state.join_req_pending    = true;  // Gates HandleJoinAck so a stray
                                         // JOIN_ACK from the wire doesn't
                                         // promote a non-spectator client
                                         // into spectator mode.
    CtrlPacket req = {};
    req.header.type            = CtrlMsg::SPEC_JOIN_REQ;
    req.data.spec_join_req.mode = static_cast<uint8_t>(mode);
    // Phase F capability advertisement (remaining reserved bytes stay
    // zero from the {} init above). Old hosts ignore reserved bits.
    if (SpecUdpEnabled()) {
        req.data.spec_join_req.reserved[0] |= SPEC_JOIN_UDP_OK;
    }
    // Light re-join: mid-stream viewers declare where their admission
    // cursor stands so the host backfills exactly the gap (no snapshot).
    if (g_state.have_frame_baseline && g_state.pb_started) {
        req.data.spec_join_req.reserved[0] |= SPEC_JOIN_RESUME;
        const uint32_t resume = g_state.next_expected_frame;
        std::memcpy(&req.data.spec_join_req.reserved[1], &resume, 4);
    }
    // Version gate advertisement (reserved[5]=minor, reserved[6]=patch).
    req.data.spec_join_req.reserved[0] |= SPEC_JOIN_VERSIONED;
    SpecJoin_AppVersionBytes(&req.data.spec_join_req.reserved[5],
                             &req.data.spec_join_req.reserved[6]);
    ControlChannel_SendTo(req, upstream);
    return true;
}

void SpectatorNode_RequestGapFill() {
    // Surgical gap-fill: the live stream is flowing (we're still
    // subscribed) but our admission cursor stalled behind a gap that
    // neither the backfill nor the mid-stream live channel filled. Ask
    // the host to re-ship [next_expected .. live-cursor) over the reliable
    // conn. Unlike RequestJoin this leaves subscribed_upstream / bind state
    // untouched -- the host's HandleJoinReq recognises resume_frame>0 on a
    // still-live sub and ships the gap WITHOUT dropping the connection or
    // re-sending the snapshot.
    if (g_state.upstream_addr.sin_port == 0) return;
    CtrlPacket req = {};
    req.header.type             = CtrlMsg::SPEC_JOIN_REQ;
    req.data.spec_join_req.mode = static_cast<uint8_t>(g_state.last_requested_mode);
    if (SpecUdpEnabled()) {
        req.data.spec_join_req.reserved[0] |= SPEC_JOIN_UDP_OK;
    }
    req.data.spec_join_req.reserved[0] |= SPEC_JOIN_RESUME;
    const uint32_t resume = g_state.next_expected_frame;
    std::memcpy(&req.data.spec_join_req.reserved[1], &resume, 4);
    // Version gate: every JOIN_REQ (incl. gap-fill re-joins) must carry
    // the version bytes or the host's gate rejects it.
    req.data.spec_join_req.reserved[0] |= SPEC_JOIN_VERSIONED;
    SpecJoin_AppVersionBytes(&req.data.spec_join_req.reserved[5],
                             &req.data.spec_join_req.reserved[6]);
    ControlChannel_SendTo(req, g_state.upstream_addr);
}

void SpectatorNode_HandleJoinAck(const sockaddr_in& from, uint8_t host_session_kind,
                                 uint16_t host_tcp_port,
                                 uint8_t host_p1_char, uint8_t host_p2_char,
                                 uint8_t host_stage,
                                 uint8_t host_p1_color, uint8_t host_p2_color) {
    // SPEC_JOIN_ACK is dual-purpose:
    //   1. First-time arrival (initial subscribe): completes the JOIN_REQ
    //      handshake, pins RNG, marks subscribed_upstream, opens TCP up.
    //   2. Re-broadcast on host session-kind change (host crosses a
    //      session boundary like CSS->battle or first-CSS-create): the
    //      payload's host_session_kind tells the spectator which kind to
    //      mirror. Treated as authoritative current-host-kind.
    //
    // Both paths funnel through the same handler so the spectator can
    // recover cleanly if the host transitions before the JOIN_REQ ack
    // round-trip lands.
    //
    // Both guards moved AHEAD of the BTB seeding below. They used to sit after
    // it, which meant a JOIN_ACK from a peer we had already rejected as
    // non-upstream still got to stamp this process's battle char overrides.
    const bool first_time = g_state.join_req_pending;
    if (!first_time && !AddrEqual(g_state.upstream_addr, from)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: ignoring SPEC_JOIN_ACK from non-upstream peer");
        return;
    }

    // A LIVE REFRESH says where the host is right now; a GRANT says what this
    // subscriber was pinned to. Only a grant may complete a first-time
    // handshake. The battle-entry broadcast is built from live host state for
    // every subscriber at once and knows nothing about what any of them was
    // pinned to, so if it is the first ACK we manage to process -- our accept
    // ACK was lost, or the two datagrams reordered, both plain unreliable UDP
    // -- obeying it would /F-boot us into battle on a promise nobody made. If
    // the host had pinned us FULL_SESSION, the stream we then receive is the
    // from-frame-0 CSS mirror, which we would replay as battle input from
    // uninitialised state (hp 690/700, timer=1, garbage positions): a
    // deterministic desync. Refuse, stay in join_req_pending, and let KickJoin
    // or the reconnect loop fetch a real grant -- the fast first kick makes
    // that ~1s, and the host's reset-floor re-ACK re-states the grant.
    const bool live_refresh =
        (host_session_kind & SPEC_ACK_LIVE_REFRESH) != 0;
    const uint8_t eff_kind = host_session_kind & SPEC_ACK_KIND_MASK;
    if (first_time && live_refresh) {
        char rbuf[48] = {}; FormatAddr(from, rbuf, sizeof(rbuf));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: first JOIN_ACK from %s is a LIVE REFRESH "
            "(host_kind=%u), not a grant -- our accept ACK was lost or "
            "reordered. Refusing to boot on it (obeying a battle refresh "
            "against a FULL_SESSION grant /F-boots us and replays the CSS "
            "stream as battle input); staying in join_req_pending and "
            "re-JOINing for a real grant", rbuf, (unsigned)eff_kind);
        return;
    }

    // "Fresh boot" == this ACK decides how the engine comes up. A re-JOIN also
    // sets join_req_pending, so first_time alone is not enough: a viewer that
    // already has a stream must not re-take a boot decision (the natural_boot
    // latch permanently disables snapshot apply, so re-latching it on a
    // mid-stream re-JOIN would silently kill every later snapshot). Same
    // "truly fresh viewer" test the RNG pin below already uses.
    const bool fresh_boot = first_time && !g_state.have_frame_baseline;

    // If host advertised real chars (in-battle), forward to the BTB
    // runtime-override channel so the slot-0 /F dispatcher loads the
    // host's actual character files. We CAN'T use SetEnvironmentVariableA
    // + getenv here -- Win32 SetEnv updates the process env block but
    // not the CRT's _environ cache, so getenv() in BTB returns the stale
    // launcher-provided placeholder (char 0). PerGamePatches keeps a
    // hook-internal struct that BTB reads first; this bypasses the CRT
    // cache entirely.
    if (eff_kind != SPEC_ACK_KIND_BATTLE /* pre-battle: CSS or NONE */) {
        // Tournament flow: the host's players are still at CSS (or
        // between sessions). The viewer must NOT /F-boot -- it walks
        // title->CSS naturally and replays the from-frame-0 stream,
        // watching the lock-ins live.
        PerGamePatches_AbortBtbNaturalBoot();
        if (fresh_boot) g_state.natural_boot = true;
        // The join mode is pinned HOST-side at JOIN_REQ time (a
        // CURRENT_MATCH request reaching a pre-battle host becomes
        // FULL_SESSION there) -- a spec-side re-request raced: it made
        // the host reset bind state and drop the freshly-dialed TCP
        // conn, leaving a 15s zombie (2026-06-11 12:49).
    }
    // BTB seeding is a BOOT action, so ONLY a grant may do it. Seeding from a
    // refresh is how an already-subscribed FULL_SESSION mirror got hijacked
    // into battle, and the mechanism is not the obvious one -- it is not the
    // phase swap below, it is the /F hold's entry condition:
    //
    //   per_game_patches_battle.cpp:519
    //     if (s_is_spec == 1 && g_runtime_btb_p1_char == 0xFF) { ...hold... }
    //
    // The `g_btb_natural_boot_abort` check that DROPS /F lives inside that
    // hold. So a refresh landing between the CSS grant and the slot-0
    // dispatcher sets g_runtime_btb_p1_char, the hold is never entered, the
    // abort never runs, and /F boots the viewer straight into battle with the
    // host's chars -- while its assigned stream is the from-frame-0 CSS
    // mirror. Observed as `game_mode 0 -> 3000` at pop=1 with the verbatim bad
    // head (hp=690/700, timer=1). A from-frame-0 mirror never needs these
    // overrides: it walks title -> CSS itself and its own MATCH_START pins the
    // chars through CssAutoConfirm.
    if (eff_kind == SPEC_ACK_KIND_BATTLE && !live_refresh) {
        PerGamePatches_SetRuntimeBtbOverrides(host_p1_char,
                                              host_p2_char,
                                              host_stage,
                                              host_p1_color,
                                              host_p2_color);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: seeded runtime BTB from JOIN_ACK grant "
            "(p1=%u/c%u p2=%u/c%u stage=%u)",
            (unsigned)host_p1_char, (unsigned)host_p1_color,
            (unsigned)host_p2_char, (unsigned)host_p2_color,
            (unsigned)host_stage);
        // We are booting as a snapshot joiner: the host granted BATTLE, which
        // means and only means "a snapshot is coming". Arm the consistency
        // backstop in TickHealth -- if the stream turns up without one, the
        // grant contradicted the stream and we must not keep simulating.
        if (fresh_boot) g_state.spec_boot_expects_snapshot = true;
    } else if (eff_kind == SPEC_ACK_KIND_BATTLE) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: battle-entry refresh (p1=%u p2=%u stage=%u) -- NOT "
            "seeding BTB; boot posture is owned by our grant, and arming /F "
            "here would disable the natural-boot abort",
            (unsigned)host_p1_char, (unsigned)host_p2_char,
            (unsigned)host_stage);
    }

    g_state.join_req_pending      = false;
    g_state.upstream_addr         = from;
    g_state.subscribed_upstream   = true;
    g_state.ever_subscribed       = true;   // task #70: latch for the reconnect cadence

    // Phase F: a fresh UDP admission epoch starts ONLY when a new TCP
    // connection (whose OP_BASELINE re-arms it exactly) is coming --
    // i.e. when we are NOT currently connected and will dial below.
    // The host re-broadcasts JOIN_ACK informationally at every battle
    // entry (char/color refresh for late joiners); disarming on those
    // killed UDP admission on every viewer at every battle start, with
    // no OP_BASELINE ever coming to re-arm it -- the recurring "UDP
    // dies at battle entry" (2026-06-11 15:04).
    if (!SpectatorTCP::IsUpstreamConnected()) {
        g_state.udp_epoch_armed = false;
    }

    // RNG sync + queue clear: ONLY apply on FIRST-TIME subscribe (spectator
    // is still at title/pre-CSS, no game state to lose). On reconnect-after-
    // silence-failover or on re-broadcast ACK, spectator's local sim is
    // mid-match -- clobbering RNG / wiping the queue would erase in-progress
    // state. Gate both on first-time only.
    //
    // have_frame_baseline sub-gate (review hole 9): RequestJoin sets
    // join_req_pending on EVERY reconnect attempt, so first_time is true
    // for genuine reconnects too. If a baseline already exists, the queue
    // holds received-but-unplayed frames the dedup cursor has already
    // counted -- clearing them would skip those frames forever. Only a
    // truly fresh viewer (no baseline yet) gets the clear + RNG pin.
    if (first_time && !g_state.have_frame_baseline) {
        *(uint32_t*)0x41FB1C = 0x12345678;
        g_state.pb_queue.clear();
        g_state.pb_current_p1 = 0;
        g_state.pb_current_p2 = 0;
        g_state.pb_boundary         = State::PbBoundary::NONE;
        g_state.pending_reset_input = false;
        g_state.pending_sound_init  = false;
        CssAutoConfirm_SetSeamHold(false);
    }
    g_state.playing_back = true;

    // Dial the host's TCP port. Bulk INPUT_BATCH / INITIAL_MATCH /
    // MATCH_END flow over TCP exclusively in legacy mode. In relay
    // mode (Phase 3), spec data arrives via hub WS -> launcher -> our
    // inbound ring, so there's no P2P TCP to dial.
    if (g_state.spec_transport_relay) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: JOIN_ACK accepted in relay mode -- skipping "
            "ConnectUpstream (spec data arrives via hub via launcher via "
            "inbound shared-mem ring)");
    } else if (SpecRcSnapshotEnabled()) {
        // RC full-transport mode: live events AND snapshot+backfill all
        // ride RC over UDP -- TCP is not load-bearing, so don't dial it.
        // Dialing anyway was actively harmful (task #55): when the dial
        // failed (Nathan's NAT), each failure escalated through
        // OnUpstreamTcpDead -> background re-JOIN -> host "resetting bind
        // state for fresh backfill" -- a 2s churn loop that reset the RC
        // stream forever and the viewer never admitted a frame.
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: JOIN_ACK accepted in RC full-transport mode "
                "-- skipping ConnectUpstream (all spec data rides RC/UDP)");
        }
    } else {
        if (host_tcp_port == 0) {
            // Mixed-mode failure: we're a TCP-mode spec but the host
            // didn't advertise a TCP port. Two likely causes:
            //   1. Host is running FM2K_SPEC_TRANSPORT=relay; their hook
            //      skipped the TCP listener, so GetListenPort()=0. Our
            //      launcher should have auto-set FM2K_SPEC_TRANSPORT=relay
            //      via spectate_grant.spec_transport (Phase 4). If we're
            //      here, our launcher is older than that or the env
            //      didn't propagate.
            //   2. Host has a genuinely broken hook init (listener bind
            //      failed on all candidates).
            // Either way the spec won't get data; refuse cleanly with
            // an actionable error message rather than the silent dial.
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: JOIN_ACK from host advertises no TCP port "
                "AND we are in legacy TCP mode -- likely a relay-mode host "
                "but our launcher didn't auto-derive (Phase 4 requires "
                ">= v0.2.58). Workaround: set FM2K_SPEC_TRANSPORT=relay "
                "in the spec's env before launching, OR update the spec's "
                "launcher. Refusing the subscription.");
            g_state.subscribed_upstream = false;
            return;
        }
        char host_ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, (void*)&from.sin_addr, host_ip, sizeof(host_ip));
        if (SpectatorTCP::IsUpstreamConnected()) {
            // Session-kind-change re-broadcast over a healthy connection
            // (e.g. the battle-entry JOIN_ACK that seeds BTB chars):
            // nothing to dial, keep the stream.
        } else if (!SpectatorTCP::ConnectUpstream(host_ip, host_tcp_port)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: SpectatorTCP::ConnectUpstream(%s:%u) failed",
                host_ip, (unsigned)host_tcp_port);
            g_state.subscribed_upstream = false;
            return;
        }
    }

    char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: JOIN_ACK from %s (host_kind=%u%s, tcp_port=%u) -- subscribed",
                buf, (unsigned)host_session_kind,
                live_refresh ? " refresh" : " grant", (unsigned)host_tcp_port);

    // eff_kind == 0 (NONE): host has no session active yet
    // (e.g. JOIN_REQ landed during host's title-skip phase before its
    // first CSS session was created). DO NOT create a SpectateSession
    // with a guessed config -- that caused config mismatch deadlocks
    // before. Wait for host's follow-up SPEC_JOIN_ACK after session create.
    if (eff_kind == SPEC_ACK_KIND_NONE) {
        char buf2[48] = {}; FormatAddr(from, buf2, sizeof(buf2));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: host has no session yet -- waiting (from=%s)", buf2);
        SpecReplayPreSubStash();  // task #55: anything that beat this ACK
        return;
    }

    NetplaySessionKind kind = (eff_kind == SPEC_ACK_KIND_CSS)
        ? NetplaySessionKind::CSS
        : NetplaySessionKind::BATTLE;

    char host_addr_str[64];
    snprintf(host_addr_str, sizeof(host_addr_str), "%s:%u",
             inet_ntoa(from.sin_addr), ntohs(from.sin_port));

    // If a SpectateSession is already alive, only act on a kind CHANGE.
    // Same-kind re-broadcasts (host re-acks for liveness / new spectator
    // joining the same session) are no-ops via Netplay_StartSpectateSession's
    // own idempotency guard.
    const NetplaySessionKind current = Netplay_GetSessionKind();
    if (current == NetplaySessionKind::SPECTATE) {
        // Only a LIVE REFRESH may swap a running mirror's phase. A grant
        // re-states how THIS subscriber was told to boot, and that answer is
        // pinned: a viewer granted CSS keeps being told CSS long after the
        // host reached battle, so letting a grant drive the swap would kick a
        // battle mirror back out to CSS on any re-ACK (the host's reset-floor
        // branch re-ACKs on every re-JOIN inside 3s). The stream's
        // MATCH_START / MATCH_END ops are the primary transition signal
        // anyway; this path is the robustness backup for them.
        if (!live_refresh) {
            SpecReplayPreSubStash();  // task #55: anything that beat this ACK
            return;
        }
        // Wrong-kind alive -- treat as a phase swap. swap_frame=0 fires
        // immediately on the next AdvanceEvent.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: host kind changed mid-session, requesting swap to kind=%d",
                    (int)kind);
        if (kind == NetplaySessionKind::BATTLE) {
            Netplay_OnHostBattleEntering(0);
        } else {
            Netplay_OnHostBattleEnd(0);
        }
        SpecReplayPreSubStash();  // task #55: anything that beat this ACK
        return;
    }

    // Creating the session is the last BOOT action, so it is grant-only too.
    // A refresh reaching here means no SpectateSession exists (it was torn
    // down), and spinning one up from a broadcast would pick its kind from the
    // host's live phase rather than from what we were granted -- the same
    // class of mistake as the BTB seeding above. The reconnect loop re-JOINs
    // and a grant rebuilds it correctly.
    if (live_refresh) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: live refresh with no SpectateSession alive -- not "
            "creating one from a broadcast; waiting for a grant");
        SpecReplayPreSubStash();
        return;
    }
    Netplay_StartSpectateSession(kind, host_addr_str);
    SpecReplayPreSubStash();  // task #55: backfill that beat the first ACK
}

// task #58: viewer auto-fallback onto the hub spec relay. Called by the
// connect watchdog when the direct join produced no admitted frames after
// its grace window. Swaps the upstream to the relay endpoint and re-JOINs
// with the sticky mode; every send to the new upstream wraps into the
// 0xCF envelope by destination match (SendToMaybeRelayWrapped) and the
// host sees a normal direct-looking spectator at the relay's per-session
// socket -- no host-side involvement at all. One-shot: if the relay leg
// ALSO produces nothing, the 30s connect deadline still closes cleanly.
bool SpectatorNode_EngageRelayFallback() {
    static bool s_engaged = false;
    if (s_engaged) return false;
    const sockaddr_in* relay = ::fm2k::nat::GetRelayAddr();
    if (!relay) return false;   // launcher provided no spec_relay block
    s_engaged = true;
    char buf[48] = {}; FormatAddr(*relay, buf, sizeof(buf));
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: direct join produced no frames -- engaging hub "
        "spec-relay fallback via %s", buf);
    SpectatorNode_RequestJoin(*relay, g_state.last_requested_mode);
    return true;
}

void SpectatorNode_HandleJoinRedirect(const sockaddr_in& from,
                                      uint32_t redirect_ip,
                                      uint16_t redirect_port)
{
    (void)from;
    if (redirect_ip == 0 && redirect_port == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: Redirect with no target -- giving up");
        g_state.subscribed_upstream = false;
        return;
    }
    sockaddr_in target = {};
    target.sin_family      = AF_INET;
    target.sin_addr.s_addr = redirect_ip;
    target.sin_port        = htons(redirect_port);
    char buf[48] = {}; FormatAddr(target, buf, sizeof(buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Redirecting to %s (mode=%s)", buf,
                g_state.last_requested_mode == SpecJoinMode::CURRENT_MATCH
                    ? "CURRENT_MATCH" : "FULL_SESSION");
    SpectatorNode_RequestJoin(target, g_state.last_requested_mode);
}


void SpectatorNode_OnUpstreamTcpDead() {
    if (!g_state.subscribed_upstream) return;
    if (g_state.tcp_rejoin_pending) return;  // already riding it out
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: upstream TCP died -- riding on UDP, re-JOIN in "
        "background (q=%zu, ops_seen=%u, boundary=%d)",
        g_state.pb_queue.size(), g_state.ops_seen,
        (int)g_state.pb_boundary);
    // The subscription and UDP admission stay ARMED. The host's node-
    // level subscriber entry survives its TCP-layer erase (heartbeats
    // flow over UDP), so datagrams -- inputs AND the redundant ops tail
    // -- keep arriving through the dead window. Dropping the
    // subscription here used to reject every one of them: an 8-second
    // self-inflicted starvation while 5 re-JOINs begged a host whose
    // control channel was stalled in its own boundary (2026-06-11).
    // Op indexing is global, so ops_seen survives the rebind; the new
    // connection's OP_BASELINE re-seeds only the per-conn dedup cursor.
    g_state.tcp_rejoin_pending = true;
    // TickHealth's reconnect branch (rejoin pending + no live conn)
    // fires RequestJoin on its 2s backoff; the host's existing-sub path
    // resets bind state and re-ships backfill on the fresh socket.
}

bool SpectatorNode_InBoundary() {
    return g_state.pb_boundary != State::PbBoundary::NONE;
}

bool SpectatorNode_QueueHasPendingOp() {
    for (const auto& ev : g_state.pb_queue) {
        if (ev.type != SessionEventType::INPUT) return true;
    }
    return false;
}

// Self-sufficient join kick for the /F dispatch hold: the JOIN_REQ is
// normally sent by Netplay_InitAsSpectator on the DLL-init path, but the
// dispatcher's first-tick hold can win that race -- it would then pump a
// socket with NO join in flight and sit black until the host's battle-
// entry re-broadcast finally arrived (the "black screen until P1/P2
// confirm" failure). Re-requests on SPECTATOR_KICK_JOIN_INTERVAL_MS until
// subscribed; harmless when the original request already landed (host's
// existing-sub path ACKs idempotently).
//
// The cadence is load-bearing: it must exceed the host's 3s destructive-reset
// floor so every kick provokes a real re-ship instead of a bare re-ACK. The
// FIRST kick stays fast because at that point the host either never heard us
// (the initial JOIN_REQ was the lost datagram -- nothing shipped, so a 1s
// retry costs nothing and saves 2.5s of black screen on the ~1-in-5 joins that
// lose it at 20% loss) or it heard us and has last_reset_ms==0, which the
// floor exempts anyway. TickHealth's reconnect loop does NOT run during the /F
// dispatch hold, so this is the only retry driver for the first seconds.
void SpectatorNode_KickJoin() {
    if (g_state.subscribed_upstream) return;
    if (g_state.root_addr.sin_port == 0) return;  // init hasn't configured us yet
    static uint32_t s_kicks = 0;
    const uint64_t interval = (s_kicks == 0) ? 1000u : SPECTATOR_KICK_JOIN_INTERVAL_MS;
    const uint64_t now = GetTickCount64();
    if (now - g_state.last_reconnect_attempt_ms < interval) return;
    ++s_kicks;
    SpectatorNode_RequestJoin(g_state.root_addr, g_state.last_requested_mode);
}
