// SpectatorTCP — TCP transport for the spectator INPUT_BATCH stream.
//
// See spectator_tcp.h for full design rationale. This file is the SDL3_net
// implementation: a single listener on the host, a per-subscriber stream
// socket, and one upstream client socket on the spectator.

#include "spectator_tcp.h"
#include "spectator_node.h"
#include "../ui/shared_mem.h"

#include <SDL3_net/SDL_net.h>
#include <SDL3/SDL_log.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

// Forward declaration — defined in spectator_node.cpp. The `from` field is
// unused for TCP-sourced frames (dispatch logic doesn't depend on it for
// upstream-INPUT_BATCH/INITIAL_MATCH/MATCH_END), so we pass a zeroed sockaddr.
extern void SpectatorNode_OnUpstreamTcpDead();
extern void SpectatorNode_HandleSpecData(const uint8_t* buf, size_t len,
                                         const sockaddr_in& from, uint8_t rc_channel);

namespace SpectatorTCP {

namespace {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct SubConn {
    sockaddr_in           addr;       // (ip, source-port) tuple from JOIN_REQ
    NET_StreamSocket*     sock;
    std::vector<uint8_t>  read_buf;   // partial-frame staging (currently
                                      // unused — host doesn't read structured
                                      // data from subscribers post-INPUT_REQUEST
                                      // removal; we just discard inbound bytes).
    bool                  backfill_done = false;
                                      // Backfill ordering fence (C5). Set true
                                      // by SpectatorNode_TickHostMaintenance
                                      // after SendInitialMatchTo +
                                      // SendSessionBackfillTo finish. Until
                                      // then, BroadcastToAll skips this sub —
                                      // closes the race where a live FlushBatch
                                      // would otherwise reach the spectator
                                      // before its backfill bytes, anchoring
                                      // next_expected_frame mid-stream and
                                      // dropping the early session events.
};

bool                              g_net_inited        = false;
NET_Server*                       g_listener          = nullptr;
uint16_t                          g_listen_port       = 0;
std::vector<NET_StreamSocket*>    g_pending_clients;
std::vector<SubConn>              g_subs;

NET_StreamSocket*                 g_upstream_sock     = nullptr;
std::vector<uint8_t>              g_upstream_read_buf;
uint64_t                          g_last_upstream_recv_ms = 0;

// External TCP addr learned from a hub TCP-STUN round-trip. Initialized
// false; set to true after PerformTcpStun completes successfully. The
// launcher polls this via SharedMem and forwards to the hub in a
// `tcp_addr` WS message; the hub then carries it in the
// spectator_incoming forward to the host so the host's TCP punch hits
// the right external port even on non-port-preserving NATs.
bool      g_external_tcp_known   = false;
uint32_t  g_external_tcp_ip_be   = 0;
uint16_t  g_external_tcp_port    = 0;

// Throttled-log helper: rate-limit a tagged log site to once per second.
struct LogThrottle { uint64_t last_ms; };
LogThrottle g_throttle_accept_err     = {0};
LogThrottle g_throttle_read_err       = {0};
LogThrottle g_throttle_corrupt_stream = {0};

bool ShouldLog(LogThrottle& t) {
    const uint64_t now = GetTickCount64();
    if (now - t.last_ms >= 1000) { t.last_ms = now; return true; }
    return false;
}

bool EnsureNetInit() {
    if (g_net_inited) return true;
    if (!NET_Init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SpectatorTCP: NET_Init failed: %s", SDL_GetError());
        return false;
    }
    g_net_inited = true;
    return true;
}

bool AddrEquals(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_family == b.sin_family
        && a.sin_port   == b.sin_port
        && a.sin_addr.s_addr == b.sin_addr.s_addr;
}

// IP-only match (used by RegisterAcceptedClient — TCP source port differs
// from the UDP source port, so we can't full-tuple match here).
bool AddrIpEquals(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_family == b.sin_family
        && a.sin_addr.s_addr == b.sin_addr.s_addr;
}

// Resolve a NET_StreamSocket's peer IP into dotted-quad form. Returns false
// if the socket isn't connected or the address isn't yet resolved.
bool GetPeerIpString(NET_StreamSocket* sock, char* out, size_t out_sz) {
    NET_Address* a = NET_GetStreamSocketAddress(sock);
    if (!a) return false;
    const char* s = NET_GetAddressString(a);
    bool ok = false;
    if (s && out_sz) {
        std::strncpy(out, s, out_sz - 1);
        out[out_sz - 1] = '\0';
        ok = true;
    }
    NET_UnrefAddress(a);
    return ok;
}

size_t PayloadLenForType(const SpecDataHeader& hdr) {
    switch (hdr.type) {
        case SpecDataType::INITIAL_MATCH: return 96;
        case SpecDataType::INPUT_BATCH:   return static_cast<size_t>(hdr.frame_count) * 4u;
        case SpecDataType::MATCH_END:     return 0;
        case SpecDataType::INPUT_REQUEST: return 0; // shouldn't appear upstream
        case SpecDataType::EVENT_BATCH:
            // C2+ wire: payload is a packed SessionEvent[] stream of
            // variable length. The byte count lives in hdr.flags (was a
            // reserved field; capped at 65535 B which is well above the
            // BACKFILL_CHUNK_BYTES=1024 chunk size + any reasonable live
            // FlushBatch). frame_count carries the INPUT count for the
            // receiver's dedup gate but doesn't size the payload.
            return static_cast<size_t>(hdr.flags);

        // Snapshot join (task #18). All three carry their payload byte
        // count in hdr.flags so the framer can size the receive without
        // peeking. Phase 1: framer accepts them so the connection
        // doesn't drop on "unknown SpecDataType=N", but the receive
        // dispatch in spectator_node still no-ops these. Phase 4 wires
        // up assembly + SaveState_Load.
        case SpecDataType::SNAPSHOT_BEGIN:
        case SpecDataType::SNAPSHOT_CHUNK:
        case SpecDataType::SNAPSHOT_END:
            return static_cast<size_t>(hdr.flags);

        // Phase F: OP_BASELINE is TCP-borne (u32 payload, byte count in
        // flags). UDP_INPUT_BATCH deliberately has NO case here -- it is
        // datagram-only, and a host bug that ever frames it onto the TCP
        // stream should fail loudly (SIZE_MAX -> connection drop) instead
        // of silently desyncing the framer.
        case SpecDataType::OP_BASELINE:
            return static_cast<size_t>(hdr.flags);
    }
    return SIZE_MAX;  // unknown tag
}

}  // namespace

// ===========================================================================
// HOST-SIDE
// ===========================================================================

bool StartListener(uint16_t bind_port) {
    if (g_listener) return true;
    if (!EnsureNetInit()) return false;

    // Bind to all interfaces (NULL → "any address available").
    g_listener = NET_CreateServer(nullptr, bind_port);
    if (!g_listener) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SpectatorTCP: NET_CreateServer(port=%u) failed: %s",
                     bind_port, SDL_GetError());
        return false;
    }
    g_listen_port = bind_port;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorTCP: listening on TCP port %u", bind_port);
    return true;
}

uint16_t GetListenPort() {
    return g_listen_port;
}

void PollAccepts() {
    if (!g_listener) return;
    for (;;) {
        NET_StreamSocket* incoming = nullptr;
        if (!NET_AcceptClient(g_listener, &incoming)) {
            if (ShouldLog(g_throttle_accept_err)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "SpectatorTCP: NET_AcceptClient failed: %s",
                            SDL_GetError());
            }
            break;
        }
        if (!incoming) break; // no more pending
        g_pending_clients.push_back(incoming);
        char ip[64] = {0};
        GetPeerIpString(incoming, ip, sizeof(ip));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorTCP: accepted pending client from %s", ip);
    }
}

void PollIncoming() {
    if (g_subs.empty()) return;
    uint8_t scratch[4096];
    std::vector<size_t> doomed;
    for (size_t i = 0; i < g_subs.size(); ++i) {
        NET_StreamSocket* s = g_subs[i].sock;
        if (!s) { doomed.push_back(i); continue; }
        const int n = NET_ReadFromStreamSocket(s, scratch, sizeof(scratch));
        if (n < 0) {
            if (ShouldLog(g_throttle_read_err)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "SpectatorTCP: subscriber read error: %s",
                            SDL_GetError());
            }
            doomed.push_back(i);
        }
        // n == 0: nothing pending. n > 0: spectator → host TCP messages
        // don't exist (post-INPUT_REQUEST removal) — discard.
    }
    // Erase in reverse-index order so earlier indices remain valid.
    for (size_t k = doomed.size(); k-- > 0; ) {
        const size_t idx = doomed[k];
        if (g_subs[idx].sock) NET_DestroyStreamSocket(g_subs[idx].sock);
        g_subs.erase(g_subs.begin() + idx);
    }
}

// Normalize an SDL_net peer-address string for IP comparison: strips the
// IPv4-mapped-IPv6 prefix ("::ffff:127.0.0.1" → "127.0.0.1") so we can
// match against inet_ntop's plain v4 form.
static const char* NormalizeIp(const char* s) {
    if (!s) return "";
    if (std::strncmp(s, "::ffff:", 7) == 0) return s + 7;
    return s;
}

bool RegisterAcceptedClient(const sockaddr_in& sub_addr) {
    // Idempotent: if this subscriber already has a TCP socket bound, do
    // nothing and report success. Lets callers (e.g. TickHealth) safely
    // call this every frame for every subscriber without worrying about
    // double-binding.
    for (const auto& sc : g_subs) {
        if (sc.addr.sin_family == sub_addr.sin_family &&
            sc.addr.sin_port   == sub_addr.sin_port &&
            sc.addr.sin_addr.s_addr == sub_addr.sin_addr.s_addr &&
            sc.sock != nullptr) {
            return true;
        }
    }
    if (g_pending_clients.empty()) return false;
    char want_ip[64] = {0};
    inet_ntop(AF_INET, (void*)&sub_addr.sin_addr, want_ip, sizeof(want_ip));

    for (size_t i = 0; i < g_pending_clients.size(); ++i) {
        NET_StreamSocket* sock = g_pending_clients[i];
        char peer_ip[64] = {0};
        if (!GetPeerIpString(sock, peer_ip, sizeof(peer_ip))) continue;
        if (std::strcmp(NormalizeIp(peer_ip), want_ip) != 0) continue;

        SubConn sc{};
        sc.addr = sub_addr;
        sc.sock = sock;
        g_subs.push_back(std::move(sc));
        g_pending_clients.erase(g_pending_clients.begin() + i);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorTCP: bound TCP client %s to subscriber slot",
                    peer_ip);
        return true;
    }
    return false;
}

void BroadcastToAll(const void* buf, size_t len) {
    if (!buf || len == 0) return;
    for (auto& sc : g_subs) {
        if (!sc.sock) continue;
        if (!sc.backfill_done) continue;   // C5 fence: skip until backfill ships
        if (!NET_WriteToStreamSocket(sc.sock, buf, static_cast<int>(len))) {
            // Mark broken; next PollIncoming will reap on read error.
            // We don't tear down here to keep the call non-failing.
        }
    }
}

void MarkBackfillComplete(const sockaddr_in& sub_addr) {
    for (auto& sc : g_subs) {
        if (AddrEquals(sc.addr, sub_addr)) {
            sc.backfill_done = true;
            return;
        }
    }
}

void SendTo(const sockaddr_in& sub_addr, const void* buf, size_t len) {
    if (!buf || len == 0) return;
    for (auto& sc : g_subs) {
        if (AddrEquals(sc.addr, sub_addr)) {
            if (sc.sock) {
                NET_WriteToStreamSocket(sc.sock, buf, static_cast<int>(len));
            }
            return;
        }
    }
}

bool HasLiveConnFor(const sockaddr_in& sub_addr) {
    for (auto& sc : g_subs) {
        if (AddrEquals(sc.addr, sub_addr)) {
            return sc.sock != nullptr && sc.backfill_done;
        }
    }
    return false;
}

void DisconnectSubscriber(const sockaddr_in& sub_addr) {
    for (size_t i = 0; i < g_subs.size(); ++i) {
        if (AddrEquals(g_subs[i].addr, sub_addr)) {
            if (g_subs[i].sock) NET_DestroyStreamSocket(g_subs[i].sock);
            g_subs.erase(g_subs.begin() + i);
            return;
        }
    }
}

void DropConnectionsFromAddr(const sockaddr_in& sub_addr) {
    // Forget EVERYTHING we hold for this subscriber's IP: the bound conn
    // (may be a dead socket from an abandoned dial) and any unpaired
    // pending clients (stale corpses queue ahead of the fresh dial and
    // the binder pairs first-IP-match -- shipping 1MB into a dead pipe).
    // Called when a re-JOIN_REQ resets bind state; the spectator's fresh
    // dial arrives after and pairs cleanly.
    DisconnectSubscriber(sub_addr);
    char want_ip[64] = {0};
    inet_ntop(AF_INET, (void*)&sub_addr.sin_addr, want_ip, sizeof(want_ip));
    for (size_t i = g_pending_clients.size(); i-- > 0; ) {
        char peer_ip[64] = {0};
        if (!GetPeerIpString(g_pending_clients[i], peer_ip, sizeof(peer_ip)))
            continue;
        if (std::strcmp(NormalizeIp(peer_ip), want_ip) != 0) continue;
        NET_DestroyStreamSocket(g_pending_clients[i]);
        g_pending_clients.erase(g_pending_clients.begin() + i);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorTCP: dropped stale pending client from %s",
                    want_ip);
    }
}

// ===========================================================================
// SPECTATOR-SIDE
// ===========================================================================

bool PerformTcpStun() {
    if (g_external_tcp_known) return true;  // already done
    if (g_listen_port == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: skipped (listener not bound yet)");
        return false;
    }
    const char* hub_addr_str = std::getenv("FM2K_HUB_TCP_STUN_ADDR");
    if (!hub_addr_str || !*hub_addr_str) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: FM2K_HUB_TCP_STUN_ADDR unset — skipping");
        return false;
    }
    std::string hub_str = hub_addr_str;
    auto colon = hub_str.rfind(':');
    if (colon == std::string::npos) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: bad addr '%s' (need ip:port)", hub_addr_str);
        return false;
    }
    std::string host = hub_str.substr(0, colon);
    int hub_port = std::atoi(hub_str.c_str() + colon + 1);
    if (hub_port <= 0 || hub_port > 0xFFFF) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: bad port in '%s'", hub_addr_str);
        return false;
    }
    // Bypass SDL_net entirely. v0.2.51 tried sync getaddrinfo + handing a
    // literal IP to SDL_net, but on some peers SDL_net's resolver still
    // hung even for inet_pton-compatible input. Raw winsock is simpler,
    // deterministic, and uses the same WSAStartup the netplay code path
    // already initialized. Whole probe completes in <100ms in normal
    // conditions; total cap is 1s connect + 500ms recv = 1.5s worst case.
    struct in_addr hub_addr{};
    if (inet_pton(AF_INET, host.c_str(), &hub_addr) != 1) {
        // Hostname — resolve via sync getaddrinfo (UDP-STUN uses the
        // same path successfully; SDL_net is what was hanging, not DNS).
        addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int gai = getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (gai != 0 || !res) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TCP-STUN: getaddrinfo('%s') failed (rc=%d)",
                        host.c_str(), gai);
            return false;
        }
        hub_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    char resolved_ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &hub_addr, resolved_ip, sizeof(resolved_ip));

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: socket() failed: %d", WSAGetLastError());
        return false;
    }

    // Set SO_REUSEADDR so we can bind to the same port the spec listener
    // already has open. This is what creates the NAT mapping we want
    // to probe — host punches to whatever external port maps from
    // g_listen_port, so we MUST connect from g_listen_port.
    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(g_listen_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (sockaddr*)&local, sizeof(local)) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: bind(local=%u) failed: %d",
                    (unsigned)g_listen_port, WSAGetLastError());
        closesocket(sock);
        return false;
    }

    // Non-blocking connect so we can cap with select().
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port   = htons((uint16_t)hub_port);
    remote.sin_addr   = hub_addr;
    const int crc = connect(sock, (sockaddr*)&remote, sizeof(remote));
    if (crc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: connect %s:%d failed: %d",
                    resolved_ip, hub_port, WSAGetLastError());
        closesocket(sock);
        return false;
    }

    fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
    timeval tv{1, 0};   // 1s connect timeout
    if (select(0, nullptr, &wfds, nullptr, &tv) <= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: connect %s:%d timed out (1s)",
                    resolved_ip, hub_port);
        closesocket(sock);
        return false;
    }
    // Check SO_ERROR to confirm connect actually succeeded (writable
    // can also mean immediate refusal in some stacks).
    int so_err = 0; int so_len = sizeof(so_err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_err, &so_len);
    if (so_err != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: connect %s:%d refused: SO_ERROR=%d",
                    resolved_ip, hub_port, so_err);
        closesocket(sock);
        return false;
    }

    // Hub sends 8 bytes: 0xCD 0x02 [ip_be:4] [port_be:2]. Block via
    // select with 500ms cap.
    fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
    timeval rtv{0, 500 * 1000};
    if (select(0, &rfds, nullptr, nullptr, &rtv) <= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: no reply from %s:%d in 500ms",
                    resolved_ip, hub_port);
        closesocket(sock);
        return false;
    }
    uint8_t ack[8] = {};
    int total = 0;
    while (total < 8) {
        const int n = recv(sock, (char*)ack + total, 8 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    closesocket(sock);
    if (total < 8 || ack[0] != 0xCD || ack[1] != 0x02) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TCP-STUN: malformed reply (got %d bytes, magic %02X %02X)",
                    total, ack[0], ack[1]);
        return false;
    }
    g_external_tcp_ip_be = *reinterpret_cast<uint32_t*>(&ack[2]);
    g_external_tcp_port  = ntohs(*reinterpret_cast<uint16_t*>(&ack[6]));
    g_external_tcp_known = true;
    char ip_str[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &g_external_tcp_ip_be, ip_str, sizeof(ip_str));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "TCP-STUN: external = %s:%u (local listener was %u)",
                ip_str, (unsigned)g_external_tcp_port,
                (unsigned)g_listen_port);
    // Publish to launcher via SharedMem; launcher's poll loop sees the
    // seq bump and forwards the value to hub via WS `tcp_addr` message.
    SharedMem_PublishExternalTcp(g_external_tcp_ip_be, g_external_tcp_port);
    return true;
}

bool     HasExternalTcpAddr() { return g_external_tcp_known; }
uint32_t GetExternalTcpIpBe() { return g_external_tcp_ip_be; }
uint16_t GetExternalTcpPort() { return g_external_tcp_port; }

bool ConnectUpstream(const char* host_ip, uint16_t host_tcp_port) {
    if (g_upstream_sock) return false;
    if (!host_ip || !*host_ip) return false;
    if (!EnsureNetInit()) return false;

    NET_Address* addr = NET_ResolveHostname(host_ip);
    if (!addr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SpectatorTCP: NET_ResolveHostname(%s) failed: %s",
                     host_ip, SDL_GetError());
        return false;
    }
    // 200ms is plenty for a literal-IP "resolve" — the SDL_net path turns
    // dotted-quad strings into addresses without a DNS round-trip.
    const int rc = NET_WaitUntilResolved(addr, 200);
    if (rc != 1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SpectatorTCP: resolve %s status=%d: %s",
                     host_ip, rc, SDL_GetError());
        NET_UnrefAddress(addr);
        return false;
    }
    // Source the outbound connect from our own listener port (when one is
    // bound) so the host's TCP simultaneous-open punch — which targets our
    // spec_tcp_port via the hub-coordinated spectator_incoming flow — sees
    // a matching 4-tuple at its NAT and lets the reply SYN-ACK back through.
    // local_port=0 falls back to NET_CreateClient's kernel-ephemeral.
    //
    // RETRY dials use the kernel-ephemeral port instead: re-dialing from
    // the SAME source port reuses the previous connection's 4-tuple, which
    // collides with its TIME_WAIT remains on the host stack -- the SYN
    // retransmits for ~5s, which is LONGER than the spectator's 5s silence
    // failover, so reconnect loops chased their own corpse forever (deep
    // mid-battle join repro, 2026-06-11: dial->accept exactly 5.06s, twice).
    // The punch-friendly fixed port only matters for the hub-coordinated
    // first dial; failover re-dials on cone NATs ride the established
    // mapping, and symmetric-NAT retries need the relay transport anyway.
    static uint32_t s_dial_attempts = 0;
    const Uint16 local_bind = (s_dial_attempts++ == 0) ? g_listen_port : 0;
    g_upstream_sock = NET_CreateClientBound(addr, host_tcp_port, local_bind);
    NET_UnrefAddress(addr);
    if (!g_upstream_sock) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SpectatorTCP: NET_CreateClientBound(%s:%u local:%u) "
                     "failed: %s",
                     host_ip, host_tcp_port, (unsigned)local_bind,
                     SDL_GetError());
        return false;
    }
    g_upstream_read_buf.clear();
    g_last_upstream_recv_ms = GetTickCount64();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorTCP: dialing %s:%u from local port %u (async)",
                host_ip, host_tcp_port, (unsigned)local_bind);
    return true;
}

void PollUpstream() {
    if (!g_upstream_sock) return;
    const int conn = NET_GetConnectionStatus(g_upstream_sock);
    if (conn == 0) return; // still pending
    if (conn < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorTCP: upstream connect failed: %s", SDL_GetError());
        NET_DestroyStreamSocket(g_upstream_sock);
        g_upstream_sock = nullptr;
        g_upstream_read_buf.clear();
        return;
    }

    // Connected. Drain whatever's available.
    uint8_t scratch[4096];
    for (;;) {
        const int n = NET_ReadFromStreamSocket(g_upstream_sock,
                                               scratch, sizeof(scratch));
        if (n < 0) {
            if (ShouldLog(g_throttle_read_err)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "SpectatorTCP: upstream read error: %s",
                            SDL_GetError());
            }
            NET_DestroyStreamSocket(g_upstream_sock);
            g_upstream_sock = nullptr;
            g_upstream_read_buf.clear();
            // Tell the node the authoritative stream is gone so it can
            // re-JOIN immediately. Waiting on the silence failover is a
            // trap with the UDP accelerator: datagrams keep the queue
            // non-empty (suppressing failover) while every op stays
            // undeliverable -- observed as the q:7 boundary zombie on
            // the 2026-06-11 multi-match run (RST mid-battle, UDP masked
            // it for 15s, deadlock at MATCH_END).
            SpectatorNode_OnUpstreamTcpDead();
            return;
        }
        if (n == 0) break;
        g_upstream_read_buf.insert(g_upstream_read_buf.end(),
                                   scratch, scratch + n);
        g_last_upstream_recv_ms = GetTickCount64();
        if (n < static_cast<int>(sizeof(scratch))) break;
    }

    // Frame-extract from g_upstream_read_buf.
    while (g_upstream_read_buf.size() >= sizeof(SpecDataHeader)) {
        SpecDataHeader hdr;
        std::memcpy(&hdr, g_upstream_read_buf.data(), sizeof(hdr));
        if (hdr.magic != SPEC_DATA_MAGIC) {
            if (ShouldLog(g_throttle_corrupt_stream)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "SpectatorTCP: corrupt upstream stream "
                             "(magic=0x%02X) — dropping connection",
                             hdr.magic);
            }
            NET_DestroyStreamSocket(g_upstream_sock);
            g_upstream_sock = nullptr;
            g_upstream_read_buf.clear();
            SpectatorNode_OnUpstreamTcpDead();
            return;
        }
        const size_t payload_len = PayloadLenForType(hdr);
        if (payload_len == SIZE_MAX) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SpectatorTCP: unknown SpecDataType=%u upstream — "
                         "dropping connection",
                         static_cast<unsigned>(hdr.type));
            NET_DestroyStreamSocket(g_upstream_sock);
            g_upstream_sock = nullptr;
            g_upstream_read_buf.clear();
            SpectatorNode_OnUpstreamTcpDead();
            return;
        }
        const size_t total = sizeof(SpecDataHeader) + payload_len;
        if (g_upstream_read_buf.size() < total) break; // need more bytes

        sockaddr_in zero_from{};
        zero_from.sin_family = AF_INET;
        SpectatorNode_HandleSpecData(g_upstream_read_buf.data(), total,
                                     zero_from);
        g_upstream_read_buf.erase(g_upstream_read_buf.begin(),
                                  g_upstream_read_buf.begin() + total);
    }
}

bool IsUpstreamConnected() {
    return g_upstream_sock != nullptr;
}

void DisconnectUpstream() {
    if (g_upstream_sock) {
        NET_DestroyStreamSocket(g_upstream_sock);
        g_upstream_sock = nullptr;
    }
    g_upstream_read_buf.clear();
}

uint64_t LastUpstreamRecvMs() {
    return g_last_upstream_recv_ms;
}

// ===========================================================================
// LIFECYCLE
// ===========================================================================

void Shutdown() {
    DisconnectUpstream();

    for (auto& sc : g_subs) {
        if (sc.sock) NET_DestroyStreamSocket(sc.sock);
    }
    g_subs.clear();

    for (auto* p : g_pending_clients) {
        if (p) NET_DestroyStreamSocket(p);
    }
    g_pending_clients.clear();

    if (g_listener) {
        NET_DestroyServer(g_listener);
        g_listener = nullptr;
    }
    g_listen_port = 0;

    if (g_net_inited) {
        NET_Quit();
        g_net_inited = false;
    }
}

}  // namespace SpectatorTCP
