// Spectator join/redirect protocol (host-side accept/redirect + viewer-side
// request/ack), the GekkoSpectator dedup set, and BuildJoinAckPacket. Extracted
// VERBATIM from spectator_node.cpp. Public API (decls in spectator_node.h);
// reaches specnode helpers via using.
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
#include "version_local.h"            // fm2k::kAppVersion -- spectate version gate
#include "gekkonet.h"

// kAppVersion "0.M.P" -> (M, P) for the SPEC_JOIN_VERSIONED gate. Parsed
// once; a malformed string (never happens -- make_version.sh stamps it)
// degrades to 0.0, which simply fails the gate closed.
static void AppVersionBytes(uint8_t* out_minor, uint8_t* out_patch) {
    static uint8_t s_minor = 0xFF, s_patch = 0xFF;
    if (s_minor == 0xFF) {
        unsigned mj = 0, mn = 0, pa = 0;
        if (std::sscanf(fm2k::kAppVersion, "%u.%u.%u", &mj, &mn, &pa) == 3) {
            s_minor = (uint8_t)mn;
            s_patch = (uint8_t)pa;
        } else {
            s_minor = 0;
            s_patch = 0;
        }
    }
    *out_minor = s_minor;
    *out_patch = s_patch;
}

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
using namespace specnode;

// Per-session tracking of which GekkoSpectator addrs we've already added.
// GekkoNet has no remove-actor API (gekkonet.h:185-203 -- only
// gekko_add_actor exists), so a naive add-on-rejoin pattern leaks one
// GekkoSpectator actor per spec retry. Over a 5s-retry storm, the host's
// per-tick spectator iteration cost grows linearly and crushes the frame
// budget down to single-digit FPS.
//
// Fix: gate gekko_add_actor on this set. Cleared once per session boundary
// by SpectatorNode_ClearGekkoSpectatorTracking() (called from netplay.cpp
// after each fresh gekko_create + gekko_start -- new session = no actors
// yet = empty set). Worst case per session: one zombie actor per
// ever-seen spec addr, instead of one per retry.
//
// Keyed by "ip:port" string (matches the addr_str gekko_add_actor sees).
// std::set instead of unordered_set so we don't have to hash, and the
// member count is tiny (single-digit per match in practice).
std::set<std::string> g_gekko_spectator_addrs;

// Clear the GekkoSpectator addr-tracking set. Called from netplay.cpp
// after each fresh gekko_create + gekko_start so the next session
// starts with no "already added" entries. Without this, post-session
// spec rejoins would be skipped because their addr is "remembered"
// from the previous (now-destroyed) session.
void SpectatorNode_ClearGekkoSpectatorTracking() {
    if (!g_gekko_spectator_addrs.empty()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: cleared %zu GekkoSpectator addr tracking entries (session boundary)",
            g_gekko_spectator_addrs.size());
        g_gekko_spectator_addrs.clear();
    }
}

CtrlPacket BuildJoinAckPacket() {
    CtrlPacket ack = {};
    ack.header.type = CtrlMsg::SPEC_JOIN_ACK;
    const NetplaySessionKind k = Netplay_GetSessionKind();
    ack.data.spec_join_ack.host_session_kind = static_cast<uint8_t>(k);
    // Tell the spectator which TCP port to dial for the INPUT_BATCH
    // stream. Zero would mean the listener failed at startup, in which
    // case the spectator refuses the subscription.
    ack.data.spec_join_ack.host_tcp_port = SpectatorTCP::GetListenPort();
    // Default "unknown" — only valid when host is in battle.
    ack.data.spec_join_ack.host_p1_char = 0xFF;
    ack.data.spec_join_ack.host_p2_char = 0xFF;
    ack.data.spec_join_ack.host_stage   = 0xFF;
    if (k == NetplaySessionKind::BATTLE) {
        // Read the engine's current post-CSS-confirm chars + stage so
        // the spec can /F-boot with the RIGHT character files. These
        // live at ADDR_P1_SELECTED_CHAR / ADDR_P2_SELECTED_CHAR (the
        // same addresses Netplay_StartBattle reads for its "match
        // chars p1=N(...) p2=N(...)" log) and ADDR_SELECTED_STAGE.
        //
        // Note: g_config_value1/3 (0x4300E0/0x4300F0) are only
        // populated when the HOST itself was /F-launched — for a
        // normal CSS walk they stay at 0, which is why the previous
        // read gave us p1=0/p2=0 and pkmncc crashed loading a
        // mirror Blaziken matchup.
        const uint32_t p1 = *(const uint32_t*)FM2K::ADDR_P1_SELECTED_CHAR;
        const uint32_t p2 = *(const uint32_t*)FM2K::ADDR_P2_SELECTED_CHAR;
        const uint32_t st = (FM2K::ADDR_SELECTED_STAGE != 0)
                              ? *(const uint32_t*)FM2K::ADDR_SELECTED_STAGE
                              : 0u;
        if (p1 < 50u) ack.data.spec_join_ack.host_p1_char = (uint8_t)p1;
        if (p2 < 50u) ack.data.spec_join_ack.host_p2_char = (uint8_t)p2;
        if (st < 50u) ack.data.spec_join_ack.host_stage   = (uint8_t)st;
        // Per-slot confirm colors (slot+0xE00B, set by AssignPlayerColor
        // from the confirm button at CSS -- the engine fact that button
        // choice IS the color). The /F boot path on the viewer hardcodes
        // P1=0/P2=1; these let it stamp the real palettes instead.
        ack.data.spec_join_ack.host_p1_color = 0xFF;
        ack.data.spec_join_ack.host_p2_color = 0xFF;
        const int32_t c1 = *(const int32_t*)0x4DFD8Bu;
        const int32_t c2 = *(const int32_t*)(0x4DFD8Bu + 0xE03Fu);
        if (c1 >= 0 && c1 < 8) ack.data.spec_join_ack.host_p1_color = (uint8_t)c1;
        if (c2 >= 0 && c2 < 8) ack.data.spec_join_ack.host_p2_color = (uint8_t)c2;
    }
    return ack;
}

void SpectatorNode_HandleJoinReq(const sockaddr_in& from, SpecJoinMode mode,
                                 uint8_t caps, uint32_t resume_frame,
                                 uint8_t ver_minor, uint8_t ver_patch) {
    // Version gate. Savestate blobs and the sim are version-specific; a
    // cross-version spectator silently black-screens (live report
    // 2026-07-17), and pre-gate viewers can't parse EVENT_BATCH2 anyway.
    // Reject unversioned (old build) and mismatched joiners with the
    // null-redirect the capacity path uses -- the viewer gives up cleanly.
    {
        uint8_t my_minor = 0, my_patch = 0;
        AppVersionBytes(&my_minor, &my_patch);
        const bool versioned = (caps & SPEC_JOIN_VERSIONED) != 0;
        if (!versioned || ver_minor != my_minor || ver_patch != my_patch) {
            char addr_buf[48] = {};
            FormatAddr(from, addr_buf, sizeof(addr_buf));
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: version gate REJECTED JOIN_REQ from %s -- "
                "viewer %s0.%u.%u vs host 0.%u.%u (spectate needs the same "
                "build; viewer should update)",
                addr_buf, versioned ? "" : "unversioned/",
                ver_minor, ver_patch, my_minor, my_patch);
            CtrlPacket redir = {};
            redir.header.type = CtrlMsg::SPEC_JOIN_REDIRECT;
            redir.data.spec_redirect.redirect_ip   = 0;
            redir.data.spec_redirect.redirect_port = 0;
            ControlChannel_SendTo(redir, from);
            return;
        }
    }
    const bool udp_ok = SpecUdpEnabled() && (caps & SPEC_JOIN_UDP_OK) != 0;
    if ((caps & SPEC_JOIN_RESUME) == 0) resume_frame = 0;
    // Pin the mode NOW, from the host's state at this instant -- the
    // same instant the ACK's kind is computed from, so the viewer's
    // natural-boot/battle-boot decision and the host's delivery path
    // can never diverge. (The bind used to decide from its own LATER
    // state: a CSS-time joiner whose bind fired after battle started
    // got a battle snapshot against a title-screen engine = deadlock.)
    if (mode == SpecJoinMode::CURRENT_MATCH && resume_frame == 0 &&
        Netplay_GetSessionKind() != NetplaySessionKind::BATTLE) {
        mode = SpecJoinMode::FULL_SESSION;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: pre-battle JOIN -- pinning mode to "
            "FULL_SESSION (from-frame-0 stream, no snapshot)");
    }
    char addr_buf[48] = {};
    FormatAddr(from, addr_buf, sizeof(addr_buf));

    auto BuildJoinAck = []() { return BuildJoinAckPacket(); };

    // Helper: if there's a live GekkoNet session on this node (player slot),
    // add the joining spectator as a GekkoSpectator actor so confirmed-input
    // events and (battle) Save/Load events reach them natively. Both CSS
    // and BATTLE sessions get spectators added — CSS doesn't emit Save/Load
    // (lockstep suppresses them) but it does emit GekkoAdvanceEvent per
    // confirmed frame, and that's the source of truth that drives the
    // spectator's local sim 1:1 with the host.
    auto AddSpectatorToSession = [](const sockaddr_in& spec_from) {
        const NetplaySessionKind k = Netplay_GetSessionKind();
        if (k != NetplaySessionKind::CSS && k != NetplaySessionKind::BATTLE) {
            return;
        }
        char ip_str[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, (void*)&spec_from.sin_addr, ip_str, sizeof(ip_str));
        char addr_str[64];
        snprintf(addr_str, sizeof(addr_str), "%s:%u",
                 ip_str, ntohs(spec_from.sin_port));
        GekkoSession* sess = Netplay_GetActiveSession();
        if (!sess) return;

        // Dedup against this-session's previously-added spec addrs. Without
        // this, a spec stuck in a 5s retry loop (e.g. TCP punch failing on
        // symmetric NAT) re-fires SPEC_JOIN_REQ every cycle and we'd
        // gekko_add_actor on each, leaking one actor per retry. GekkoNet
        // has no remove_actor counterpart, so dedup-on-add is the only
        // bound. Set is cleared at session boundaries from netplay.cpp.
        const std::string addr_key(addr_str);
        if (g_gekko_spectator_addrs.count(addr_key)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: GekkoSpectator already on %s session for %s -- skipping re-add",
                k == NetplaySessionKind::CSS ? "CSS" : "BATTLE",
                addr_str);
            return;
        }
        // Hard bound on native GekkoSpectator actors per session. GekkoNet has
        // no remove_actor, so a NAT-remapped reconnect (new ip:port each time)
        // would otherwise add a PERMANENT actor per churn cycle -> O(N)/tick
        // event fan-out that crushes the host frame budget to single-digit FPS.
        // Well above any real spectator count; a joiner past the cap still gets
        // the full stream over the SpectatorNode TCP/RC path (this native actor
        // is a secondary confirmed-input delivery, not the primary transport).
        constexpr size_t kMaxGekkoSpectators = 64;
        if (g_gekko_spectator_addrs.size() >= kMaxGekkoSpectators) {
            static uint32_t s_cap_log = 0;
            if ((s_cap_log++ & 0x1F) == 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: GekkoSpectator actor cap %zu reached (%zu addrs this "
                    "session) — %s served via SpectatorNode stream only (no native actor)",
                    kMaxGekkoSpectators, g_gekko_spectator_addrs.size(), addr_str);
            }
            return;
        }
        GekkoNetAddress addr = {};
        addr.data = (void*)addr_str;
        addr.size = (int)strlen(addr_str);
        gekko_add_actor(sess, GekkoSpectator, &addr);
        g_gekko_spectator_addrs.insert(addr_key);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: Late-joiner added as GekkoSpectator on %s session -> %s",
            k == NetplaySessionKind::CSS ? "CSS" : "BATTLE",
            addr_str);
    };

    // Already subscribed? Reset the slot's TCP-bound state so the new
    // JOIN_REQ re-fires the bind + backfill path. Without this, a previous
    // spectator session whose TCP read-errored leaves the slot with
    // tcp_bound=true; the next JOIN_REQ from same UDP source treats it as
    // a duplicate and never re-ships snapshot/backfill — symptom is the
    // spectator's silence-failover triggering every 5s in a reconnect
    // loop with the host accepting TCP but never sending data.
    //
    // Also refreshes join_mode in case the spectator switched modes (e.g.
    // CURRENT_MATCH on first connect, FULL_SESSION on retry after fallback)
    // and bumps last_seen_ms so the host's own subscriber-expiry sweep
    // doesn't cull this slot mid-rebind.
    for (auto& sub : g_state.subscribers) {
        if (AddrEqual(sub.addr, from)) {
            // Surgical gap-fill: a still-live viewer (TCP conn up) declared
            // a resume cursor BEHIND our live edge. This is the mid-match
            // snapshot-join gap -- the viewer's RC live stream started past
            // the snapshot anchor (RC endpoint-up delay under loss) and the
            // frames between its TCP backfill's end and the RC live start
            // reached it over neither channel. Re-ship exactly
            // [resume_frame .. live-cursor) over the EXISTING reliable conn
            // (SendSessionBackfillFromFrame -> TCP by default) without
            // resetting bind state, dropping the conn, or re-sending the
            // snapshot. The viewer's positional dedup drops any overlap
            // with the RC stream, so the gap heals in one round trip. The
            // old recovery (SPEC_LEAVE + full re-JOIN + re-snapshot) looped
            // forever under sustained loss.
            // "Live conn" for the surgical/dup branches: a healthy TCP
            // conn, OR RC full-transport mode where the RC endpoint to
            // sub.addr IS the reliable conn (task #55: HasLiveConnFor is
            // always false with no TCP dial, which made every gap-fill
            // pull fall through to the destructive full-reset path --
            // 500ms full-re-backfill storm whenever a viewer starved).
            const bool reliable_path_up =
                SpectatorTCP::HasLiveConnFor(sub.addr) ||
                (SpecRcSnapshotEnabled() && sub.tcp_bound);
            if (resume_frame > 0 && sub.tcp_bound &&
                !g_state.spec_transport_relay &&
                reliable_path_up) {
                sub.last_seen_ms = GetTickCount64();
                sub.udp_ok       = udp_ok;
                CtrlPacket ack = BuildJoinAckPacket();
                ControlChannel_SendTo(ack, sub.addr);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: gap-fill for %s from INPUT-frame=%u "
                    "(live conn -- reliable re-ship, no reset)",
                    addr_buf, resume_frame);
                SendSessionBackfillFromFrame(sub.addr, resume_frame);
                return;
            }
            // Destructive-reset rate limit (task #55): floor full
            // bind-reset + re-backfill to one per 3s per sub. A JOIN
            // retry storm (starved viewer, 500ms period) burns at most
            // one full backfill per window; a genuinely restarted viewer
            // still gets its fresh backfill within 3s. NOTE: this is the
            // ONLY reset suppression in RC mode -- an unconditional
            // dup-suppress (like the TCP branch below) was tried and
            // broke self-heal: with no TCP conn there is no liveness
            // signal to know the sub actually got its one-shot backfill,
            // so a viewer that lost it must be able to force a re-ship.
            {
                const uint64_t now_ms = GetTickCount64();
                if (sub.last_reset_ms != 0 &&
                    now_ms - sub.last_reset_ms < 3000) {
                    sub.last_seen_ms = now_ms;
                    sub.udp_ok       = udp_ok;
                    CtrlPacket ack = BuildJoinAckPacket();
                    ControlChannel_SendTo(ack, sub.addr);
                    return;
                }
            }
            if (sub.tcp_bound && !g_state.spec_transport_relay &&
                SpectatorTCP::HasLiveConnFor(sub.addr)) {
                // Live stream already flowing: this JOIN_REQ is a dup or
                // an over-eager retry. Re-ACK and change NOTHING -- the
                // old reset dropped the conn the previous JOIN opened,
                // and the viewer's heal retried 500ms later: an infinite
                // join storm that DoS'ed this host's main loop and
                // starved its own netplay sends (one-directional 30s
                // blackout -> P1/P2 barrier wedge).
                sub.last_seen_ms = GetTickCount64();
                sub.udp_ok       = udp_ok;
                CtrlPacket ack = BuildJoinAckPacket();
                ControlChannel_SendTo(ack, sub.addr);
                return;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: JOIN_REQ from existing subscriber %s — "
                        "resetting bind state for fresh backfill (mode=%s)",
                        addr_buf,
                        mode == SpecJoinMode::CURRENT_MATCH ? "CURRENT_MATCH"
                                                            : "FULL_SESSION");
            sub.tcp_bound    = false;
            sub.ack_frame    = 0;
            sub.join_mode    = mode;
            sub.udp_ok       = udp_ok;
            sub.resume_frame = resume_frame;
            sub.last_seen_ms = GetTickCount64();
            sub.last_reset_ms = sub.last_seen_ms;
            // Drop the old TCP conn + any stale pending clients from this
            // IP so the bind path pairs the spectator's FRESH dial instead
            // of an abandoned one (deep-join reconnect-loop fix).
            SpectatorTCP::DropConnectionsFromAddr(sub.addr);
            // Phase 2c: late-arriving spec_user_id backfill. If the
            // first JOIN_REQ raced past our spec_incoming poll (common
            // on loopback where UDP RTT is microseconds), sub.spec_user_id
            // stayed empty -- relay-mode SendTo can't address it. The
            // launcher refreshes the punch dict on every WS event for
            // this addr, so a retry JOIN_REQ should find the user_id
            // by now. Pop on success.
            if (sub.spec_user_id.empty()) {
                auto it = g_state.pending_spec_user_ids.find(addr_buf);
                if (it != g_state.pending_spec_user_ids.end()) {
                    sub.spec_user_id = it->second;
                    g_state.pending_spec_user_ids.erase(it);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: backfilled spec_user_id=%s on existing "
                        "sub %s (raced past first JOIN_REQ)",
                        sub.spec_user_id.c_str(), addr_buf);
                }
            }
            CtrlPacket ack = BuildJoinAck();
            ControlChannel_SendTo(ack, from);
            return;
        }
    }

    if (g_state.subscribers.size() < g_state.capacity) {
        Subscriber sub = {};
        sub.addr         = from;
        sub.last_seen_ms = GetTickCount64();
        sub.ack_frame    = 0;
        sub.tcp_bound    = false;
        sub.join_mode    = mode;
        sub.udp_ok       = udp_ok;
        sub.resume_frame = resume_frame;
        // Phase 2c: pop the cached spec_user_id (if any) for this addr.
        // Punch-target poll wrote it earlier when the hub's
        // spec_incoming forwarded the sub's user_id. Used by relay-mode
        // SendTo to address binary frames; ignored in TCP mode.
        {
            char ip_str[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, (void*)&from.sin_addr, ip_str, sizeof(ip_str));
            char addr_key[64];
            std::snprintf(addr_key, sizeof(addr_key), "%s:%u",
                          ip_str, ntohs(from.sin_port));
            auto it = g_state.pending_spec_user_ids.find(addr_key);
            if (it != g_state.pending_spec_user_ids.end()) {
                sub.spec_user_id = it->second;
                g_state.pending_spec_user_ids.erase(it);
            } else {
                // Race fallback: ControlChannel_Poll runs RawReceive
                // (which dispatched this JOIN_REQ) BEFORE
                // TickHostMaintenance's punch-target poll updates our
                // dict. If launcher published the user_id just before
                // this tick, the dict won't have it yet on this call.
                // Read directly from shared mem as the
                // single-source-of-truth fallback. If the shm punch
                // target matches our `from` addr, use its user_id.
                FM2KSharedMemData* shm = GetSharedMemory();
                if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC &&
                    shm->spectator_punch_ip_be == from.sin_addr.s_addr &&
                    shm->spectator_punch_port  == ntohs(from.sin_port) &&
                    shm->spectator_punch_user_id[0]) {
                    sub.spec_user_id = std::string(
                        shm->spectator_punch_user_id,
                        strnlen(shm->spectator_punch_user_id,
                                sizeof(shm->spectator_punch_user_id)));
                }
            }
        }
        g_state.subscribers.push_back(sub);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: Accepted subscriber %s (%zu/%zu, mode=%s, "
                    "transport=%s, user_id=%s)",
                    addr_buf, g_state.subscribers.size(), g_state.capacity,
                    mode == SpecJoinMode::CURRENT_MATCH ? "CURRENT_MATCH"
                                                       : "FULL_SESSION",
                    g_state.spec_transport_relay ? "RELAY" : "TCP",
                    sub.spec_user_id.empty() ? "(none)" : sub.spec_user_id.c_str());

        CtrlPacket ack = BuildJoinAck();
        ControlChannel_SendTo(ack, from);

        // If we already have a live GekkoNet session, add this late joiner
        // as a GekkoSpectator actor so the input stream reaches them.
        AddSpectatorToSession(from);

        // INITIAL_MATCH + SendSessionBackfillTo are sent by TickHealth's
        // TryBindPendingTCP path the first time the spectator's accepted
        // TCP connection gets paired with this subscriber slot.
        return;
    }

    // At capacity — random redirect à la CCCaster.
    if (!g_state.subscribers.empty()) {
        const size_t i = static_cast<size_t>(std::rand()) % g_state.subscribers.size();
        const sockaddr_in& target = g_state.subscribers[i].addr;

        CtrlPacket redir = {};
        redir.header.type = CtrlMsg::SPEC_JOIN_REDIRECT;
        redir.data.spec_redirect.redirect_ip   = target.sin_addr.s_addr;
        redir.data.spec_redirect.redirect_port = ntohs(target.sin_port);
        ControlChannel_SendTo(redir, from);

        char target_buf[48] = {};
        FormatAddr(target, target_buf, sizeof(target_buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: At capacity, redirecting %s -> %s",
                    addr_buf, target_buf);
        return;
    }

    // Capacity=0, no subscribers — reject with null redirect. Viewer gives up.
    CtrlPacket redir = {};
    redir.header.type = CtrlMsg::SPEC_JOIN_REDIRECT;
    redir.data.spec_redirect.redirect_ip   = 0;
    redir.data.spec_redirect.redirect_port = 0;
    ControlChannel_SendTo(redir, from);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Rejected JOIN_REQ from %s (capacity=0)", addr_buf);
}

void SpectatorNode_HandleLeave(const sockaddr_in& from) {
    // First check: was this from our upstream telling us to leave (it's
    // shutting down)? If so, immediately fail over to root rather than
    // waiting out the silence timer.
    if (g_state.subscribed_upstream && AddrEqual(g_state.upstream_addr, from)) {
        char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: upstream %s sent SPEC_LEAVE — failing over to root",
                    buf);
        g_state.subscribed_upstream = false;
        // TickHealth will pick up the disconnected state and trigger
        // RequestJoin(root) on its next call (rate-limited).
        return;
    }
    // Otherwise, treat as a downstream subscriber leaving us.
    for (auto it = g_state.subscribers.begin(); it != g_state.subscribers.end(); ++it) {
        if (AddrEqual(it->addr, from)) {
            char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: Subscriber %s left", buf);
            SpectatorTCP::DisconnectSubscriber(it->addr);
            g_state.subscribers.erase(it);
            return;
        }
    }
}

// Host side: tell every subscriber the session is ending cleanly (player quit /
// left). Without this the viewer sees only the dropped stream and treats it as a
// glitch -- storm-reconnecting to a dead host for seconds. Sent a few times
// because UDP is lossy and we tear down right after. Mirrors the expiry sweep's
// SPEC_LEAVE send (spec_health.cpp) but with do-not-reconnect semantics.
void SpectatorNode_BroadcastSessionEnd() {
    if (g_state.subscribers.empty()) return;
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::SPEC_SESSION_END;
    for (int rep = 0; rep < 3; ++rep) {
        for (const auto& sub : g_state.subscribers) {
            ControlChannel_SendTo(pkt, sub.addr);
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: broadcast SPEC_SESSION_END to %zu subscriber(s)",
                g_state.subscribers.size());
}

// Viewer side: upstream sent SPEC_SESSION_END. Unlike SPEC_LEAVE (which fails
// over to root), this means the whole session is OVER -- set session_ended so
// the reconnect path in TickHealth stays parked instead of storming a dead host.
void SpectatorNode_HandleSessionEnd(const sockaddr_in& from) {
    if (g_state.subscribed_upstream && AddrEqual(g_state.upstream_addr, from)) {
        char buf[48] = {}; FormatAddr(from, buf, sizeof(buf));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: upstream %s sent SPEC_SESSION_END — session "
                    "over, stopping (no reconnect)", buf);
        g_state.session_ended = true;
        g_state.subscribed_upstream = false;
    }
}

void SpectatorNode_HandleHeartbeat(const sockaddr_in& from) {
    // Viewer side: an echo from the upstream is the gameplay-independent
    // liveness proof. The session-derived datagram flow stops whenever
    // the host is between sessions (CSS sync under loss can take many
    // seconds) and the silence failover then read a healthy-but-quiet
    // host as dead -- the CSS "disconnect/re-subscribe" the user kept
    // seeing (2026-06-11 14:32).
    if (g_state.subscribed_upstream &&
        AddrEqual(from, g_state.upstream_addr)) {
        g_state.last_udp_recv_ms = GetTickCount64();
        return;
    }
    for (auto& sub : g_state.subscribers) {
        if (AddrEqual(sub.addr, from)) {
            sub.last_seen_ms = GetTickCount64();
            // Echo so the viewer's liveness clock ticks even when no
            // session is confirming frames (1Hz, 16 bytes).
            CtrlPacket hb = {};
            hb.header.type = CtrlMsg::SPEC_HEARTBEAT;
            ControlChannel_SendTo(hb, sub.addr);
            return;
        }
    }
}

// -----------------------------------------------------------------------------
// STATUS
// -----------------------------------------------------------------------------

size_t SpectatorNode_GetSubscriberCount() { return g_state.subscribers.size(); }
bool   SpectatorNode_IsBroadcasting()     { return g_state.broadcasting;      }

std::vector<sockaddr_in> SpectatorNode_GetSubscriberAddrs() {
    std::vector<sockaddr_in> out;
    out.reserve(g_state.subscribers.size());
    for (const auto& sub : g_state.subscribers) {
        out.push_back(sub.addr);
    }
    return out;
}

// -----------------------------------------------------------------------------
// VIEWER-SIDE
// -----------------------------------------------------------------------------

bool SpectatorNode_RequestJoin(const sockaddr_in& upstream, SpecJoinMode mode) {
    g_state.upstream_addr       = upstream;
    g_state.subscribed_upstream = false;
    g_state.pre_sub_stash.clear();        // fresh join -- drop any stale stash
    g_state.pre_sub_stash_bytes = 0;
    g_state.session_ended       = false;  // fresh join clears any prior SESSION_END
    g_state.last_requested_mode = mode;  // sticky — see comment in State decl
    // Bump reconnect timestamp so TickHealth's failover backoff covers the
    // INITIAL JOIN_REQ too. Without this, last_reconnect_attempt_ms stays
    // 0 after the first send, the next TickHealth pass sees
    // `now - 0 >= BACKOFF_MS`, and fires a second RequestJoin within
    // milliseconds — clobbering the spectator's declared mode before the
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
    AppVersionBytes(&req.data.spec_join_req.reserved[5],
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
    AppVersionBytes(&req.data.spec_join_req.reserved[5],
                    &req.data.spec_join_req.reserved[6]);
    ControlChannel_SendTo(req, g_state.upstream_addr);
}

void SpectatorNode_HandleJoinAck(const sockaddr_in& from, uint8_t host_session_kind,
                                 uint16_t host_tcp_port,
                                 uint8_t host_p1_char, uint8_t host_p2_char,
                                 uint8_t host_stage,
                                 uint8_t host_p1_color, uint8_t host_p2_color) {
    // If host advertised real chars (in-battle), forward to the BTB
    // runtime-override channel so the slot-0 /F dispatcher loads the
    // host's actual character files. We CAN'T use SetEnvironmentVariableA
    // + getenv here — Win32 SetEnv updates the process env block but
    // not the CRT's _environ cache, so getenv() in BTB returns the stale
    // launcher-provided placeholder (char 0). PerGamePatches keeps a
    // hook-internal struct that BTB reads first; this bypasses the CRT
    // cache entirely.
    if (host_session_kind != 2 /* pre-battle: CSS or NONE */) {
        // Tournament flow: the host's players are still at CSS (or
        // between sessions). The viewer must NOT /F-boot -- it walks
        // title->CSS naturally and replays the from-frame-0 stream,
        // watching the lock-ins live.
        PerGamePatches_AbortBtbNaturalBoot();
        g_state.natural_boot = true;
        // The join mode is pinned HOST-side at JOIN_REQ time (a
        // CURRENT_MATCH request reaching a pre-battle host becomes
        // FULL_SESSION there) -- a spec-side re-request raced: it made
        // the host reset bind state and drop the freshly-dialed TCP
        // conn, leaving a 15s zombie (2026-06-11 12:49).
    }
    if (host_session_kind == 2 /* BATTLE */) {
        PerGamePatches_SetRuntimeBtbOverrides(host_p1_char,
                                              host_p2_char,
                                              host_stage,
                                              host_p1_color,
                                              host_p2_color);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: seeded runtime BTB from JOIN_ACK "
            "(p1=%u/c%u p2=%u/c%u stage=%u)",
            (unsigned)host_p1_char, (unsigned)host_p1_color,
            (unsigned)host_p2_char, (unsigned)host_p2_color,
            (unsigned)host_stage);
    }

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
    // round-trip lands. Stray ACKs from non-upstream peers are still
    // dropped (sender addr check below).
    const bool first_time = g_state.join_req_pending;

    if (!first_time && !AddrEqual(g_state.upstream_addr, from)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: ignoring SPEC_JOIN_ACK from non-upstream peer");
        return;
    }

    g_state.join_req_pending      = false;
    g_state.upstream_addr         = from;
    g_state.subscribed_upstream   = true;

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
    // mid-match — clobbering RNG / wiping the queue would erase in-progress
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
                "SpectatorNode: JOIN_ACK from %s (host_kind=%u, tcp_port=%u) — subscribed",
                buf, (unsigned)host_session_kind, (unsigned)host_tcp_port);

    // host_session_kind == 0 (NONE): host has no session active yet
    // (e.g. JOIN_REQ landed during host's title-skip phase before its
    // first CSS session was created). DO NOT create a SpectateSession
    // with a guessed config — that caused config mismatch deadlocks
    // before. Wait for host's follow-up SPEC_JOIN_ACK after session create.
    if (host_session_kind == 0) {
        char buf2[48] = {}; FormatAddr(from, buf2, sizeof(buf2));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: host has no session yet — waiting (from=%s)", buf2);
        SpecReplayPreSubStash();  // task #55: anything that beat this ACK
        return;
    }

    NetplaySessionKind kind = (host_session_kind == 1)
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
        // Wrong-kind alive — treat as a phase swap. swap_frame=0 fires
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

    Netplay_StartSpectateSession(kind, host_addr_str);
    SpecReplayPreSubStash();  // task #55: backfill that beat the first ACK
}

void SpectatorNode_HandleJoinRedirect(const sockaddr_in& from,
                                      uint32_t redirect_ip,
                                      uint16_t redirect_port)
{
    (void)from;
    if (redirect_ip == 0 && redirect_port == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: Redirect with no target — giving up");
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
// confirm" failure). Re-requests at 1Hz until subscribed; harmless when
// the original request already landed (host's existing-sub path ACKs
// idempotently).
void SpectatorNode_KickJoin() {
    if (g_state.subscribed_upstream) return;
    if (g_state.root_addr.sin_port == 0) return;  // init hasn't configured us yet
    const uint64_t now = GetTickCount64();
    if (now - g_state.last_reconnect_attempt_ms < 1000) return;
    SpectatorNode_RequestJoin(g_state.root_addr, g_state.last_requested_mode);
}
