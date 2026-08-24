// FM2K NAT traversal -- 0xCD STUN probe + 0xCD demux + burst hole-punch +
// relay fallback + dual-stack IPv6 (global-v6 discover/advertise).
//
// Reads:
//   FM2K_HUB_UDP_ADDR    -- "ip:port" of the hub's UDP STUN responder.
//                           Same address as the WebSocket lobby in
//                           Phase 1; can split later if needed.
//   FM2K_HUB_USER_ID     -- 12-char hex id assigned by the lobby on
//                           hello_ack. Identifies this client to the
//                           hub when its STUN probe arrives.
//   FM2K_HUB_MATCH_TOKEN -- 16 hex chars (8 bytes) -- shared with peer
//                           via match_start. Used to authenticate
//                           inbound CTRL_PUNCH packets.
//
// The burst-punch driver (port of bbbr_holepunch.cpp's burst+priority
// approach) IS implemented: StartPunch fires a 30-packet priority burst over
// v4 + same-LAN + native IPv6 candidates in parallel, HandleDatagram latches
// the first authenticated peer, and the worker falls back to the hub relay
// (0xCF envelope) if no direct path latches. DiscoverAndPublishLocalV6
// advertises our own global v6 so the peer can punch us directly (no NAT).
// See docs/FM2K_Matchmaking_Design.md.

#include "nat_traversal.h"
#include "control_channel.h"
#include "addr6_util.h"        // Sendto4or6 (dual-stack v4-mapped send)
#include "../ui/shared_mem.h"  // SharedMem_PublishLocalV6 (hook -> launcher -> hub)

#include <SDL3/SDL_log.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <ws2tcpip.h>
#include <mmsystem.h>

namespace fm2k::nat {

namespace {

constexpr uint8_t MAGIC          = 0xCD;
constexpr uint8_t TAG_PROBE      = 0x01;
constexpr uint8_t TAG_ACK        = 0x02;
constexpr uint8_t TAG_CTRL_PUNCH = 0x10;

constexpr size_t USER_ID_LEN     = 24;  // fits Discord snowflake (≤19) + "_<suffix>"; hub STUN_USER_ID_LEN must match
constexpr size_t MATCH_TOKEN_LEN = 16;

bool g_have_reflexive = false;
sockaddr_in g_reflexive{};

// Our own global IPv6 endpoint (addr + bound UDP port), discovered at NAT
// init. Advertised to the peer so it can punch us directly over v6 (no NAT).
bool         g_have_local_v6 = false;
sockaddr_in6 g_local_v6{};

uint8_t g_match_token[MATCH_TOKEN_LEN] = {};
bool    g_match_token_set = false;

// Burst-punch state. The burst thread loops until either the 30
// packets are out OR g_punching latches false (set by a successful
// authenticated peer punch landing in HandleDatagram).
std::atomic<bool> g_punching{false};
std::atomic<bool> g_peer_authenticated{false};
std::thread       g_punch_thread;
sockaddr_in       g_punch_peer{};
// Optional same-LAN candidate: the peer's PRIVATE addr (192.168/10/172.16),
// learned via the hub's local_ip exchange. When set, the burst ALSO punches
// here so two players behind the SAME router connect directly over the LAN
// instead of hairpinning their shared public IP (which most routers refuse)
// and falling to the hub relay. Harmless off-LAN: a 192.168.x.x that isn't the
// peer never authenticates (the 0xCC handshake + match_token gate adoption).
sockaddr_in       g_punch_peer_lan{};
bool              g_have_lan_peer = false;
// Optional GLOBAL IPv6 candidate: the peer's native v6 host addr + its local
// UDP bind port (no NAT remap on v6). When set, the burst ALSO punches here --
// direct v6 bypasses CGNAT entirely. Sent NATIVELY (not via the v4-mapped
// Sendto4or6 wrapper). Harmless if the peer has no v6 (never authenticates).
sockaddr_in6      g_punch_peer_v6{};
bool              g_have_v6_peer = false;

constexpr int PUNCH_PACKETS  = 30;
constexpr int PUNCH_PERIOD_MS = 10;   // ~300 ms total burst

// Relay fallback state. Configured at init from FM2K_HUB_RELAY_ADDR /
// FM2K_HUB_RELAY_SESSION. Activated when burst-punch fails to latch a
// peer within the burst window.
constexpr uint8_t TAG_RELAY_DATA = 0x01;        // matches hub.py RELAY_TAG_DATA
constexpr uint8_t MAGIC_RELAY    = 0xCF;        // matches hub.py RELAY_MAGIC

bool        g_relay_configured = false;
sockaddr_in g_relay_addr{};
uint8_t     g_relay_session[16] = {};
std::atomic<bool> g_relay_mode{false};

// Relay came from the PEER's verdict (AdoptRelayFromPeer) rather than our
// own timeout. Blocks the late-punch disengage below: the peer is on the
// relay and cannot hear a direct send, whatever our own inbound evidence
// says about the other direction.
std::atomic<bool> g_relay_peer_driven{false};

// An inbound message proved the peer received something we sent. A latch
// on an inbound CTRL_PUNCH proves the OPPOSITE direction and does not
// count -- see NotePeerAckedUs in the header.
std::atomic<bool> g_peer_acked_us{false};

// Single engage point so every path logs identically. Idempotent. The
// "relay mode ENGAGED" substring is load-bearing: tools/
// replay_netplay_selftest.py --relay greps both peer logs for it.
void EngageRelay(const char* reason) {
    if (g_relay_mode.exchange(true)) return;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "NAT: relay mode ENGAGED -- %s", reason);
}

bool ParseHostPort(const std::string& s, std::string& host, uint16_t& port) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    host = s.substr(0, colon);
    int p = std::atoi(s.c_str() + colon + 1);
    if (p <= 0 || p > 65535) return false;
    port = static_cast<uint16_t>(p);
    return true;
}

// Resolve a hostname (or literal IPv4 string) to in_addr. Returns
// false on failure. Used so the launcher / dllmain can pass either
// "127.0.0.1" or "hub.2dfm.org" as the hub address -- getaddrinfo
// handles both cases. Only the FIRST A record is taken; sufficient
// for our small hub deployments.
bool ResolveHostA(const std::string& host, in_addr& out) {
    if (inet_pton(AF_INET, host.c_str(), &out) == 1) return true;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        return false;
    }
    out = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

}  // namespace

bool SendStunProbe() {
    const char* hub_addr_str = std::getenv("FM2K_HUB_UDP_ADDR");
    const char* user_id      = std::getenv("FM2K_HUB_USER_ID");
    if (!hub_addr_str || !user_id) {
        return false;
    }

    std::string host;
    uint16_t port = 0;
    if (!ParseHostPort(hub_addr_str, host, port)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: invalid FM2K_HUB_UDP_ADDR='%s'", hub_addr_str);
        return false;
    }

    sockaddr_in hub{};
    hub.sin_family = AF_INET;
    hub.sin_port   = htons(port);
    if (!ResolveHostA(host, hub.sin_addr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: failed to resolve hub host='%s'", host.c_str());
        return false;
    }

    uint8_t pkt[2 + USER_ID_LEN] = {MAGIC, TAG_PROBE};
    size_t n = std::strlen(user_id);
    if (n > USER_ID_LEN) n = USER_ID_LEN;
    std::memcpy(pkt + 2, user_id, n);
    // Pad with NUL is implicit -- pkt was zero-initialized by the brace init.

    SOCKET sock = ControlChannel_GetSocket();
    if (sock == INVALID_SOCKET) return false;

    int sent = fm2k::Sendto4or6(sock, reinterpret_cast<const char*>(pkt),
                                sizeof(pkt), hub);
    if (sent != (int)sizeof(pkt)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: STUN probe sendto failed (err=%d)", WSAGetLastError());
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "NAT: STUN probe sent to %s:%u (user_id=%s)",
        host.c_str(), (unsigned)port, user_id);
    return true;
}

bool DiscoverGlobalV6(sockaddr_in6& out) {
    // Connected-UDP source-address trick. connect() to a global v6 dest does
    // NOT send anything; it makes the OS pick the source address it would use
    // outbound, which getsockname() then reveals. That address is exactly what
    // a peer should send to (it matches our real outbound source, so the peer's
    // v6 firewall opens the right pinhole when we burst-punch). No dependency
    // on GetAdaptersAddresses / iphlpapi, and it naturally excludes link-local,
    // ULA, and loopback (the OS won't pick those to reach a global dest).
    SOCKET s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in6 dst{};
    dst.sin6_family = AF_INET6;
    dst.sin6_port   = htons(53);
    // A well-known global v6 address (Google public DNS) purely to steer
    // source-address selection. Nothing is sent to it.
    inet_pton(AF_INET6, "2001:4860:4860::8888", &dst.sin6_addr);

    bool ok = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
        sockaddr_in6 name{};
        int nlen = sizeof(name);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&name), &nlen) == 0) {
            // Global unicast is 2000::/3 (first 3 bits == 001). Anything else
            // (link-local fe80::/10, ULA fc00::/7, loopback ::1, v4-mapped) is
            // not a routable v6 endpoint a remote peer can reach.
            const uint8_t first = name.sin6_addr.s6_addr[0];
            if ((first & 0xE0) == 0x20) {
                out = name;
                ok = true;
            }
        }
    }
    closesocket(s);
    return ok;
}

void DiscoverAndPublishLocalV6() {
    sockaddr_in6 v6{};
    if (!DiscoverGlobalV6(v6)) {
        g_have_local_v6 = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: no global IPv6 on this host -- v4/relay only (no direct v6 path)");
        // Publish an all-zero addr so the launcher can clear any stale value.
        uint8_t zero[16] = {};
        SharedMem_PublishLocalV6(zero, 0);
        return;
    }
    // Stamp OUR bound UDP port onto the endpoint (getsockname above returned
    // the scratch socket's ephemeral port, not the game socket's). The peer
    // punches (our global v6 addr : our game UDP port).
    const uint16_t port_be = htons(NetSocket_GetLocalPort());
    v6.sin6_port = port_be;
    g_local_v6      = v6;
    g_have_local_v6 = true;

    char v6s[INET6_ADDRSTRLEN] = {};
    inet_ntop(AF_INET6, &v6.sin6_addr, v6s, sizeof(v6s));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "NAT: global IPv6 [%s]:%u -- advertising as a DIRECT v6 punch candidate "
        "(no NAT); publishing to launcher for the peer",
        v6s, (unsigned)NetSocket_GetLocalPort());

    SharedMem_PublishLocalV6(v6.sin6_addr.s6_addr, port_be);
}

const sockaddr_in6* GetLocalGlobalV6() {
    return g_have_local_v6 ? &g_local_v6 : nullptr;
}

void StartPunch(uint32_t peer_ip_be, uint16_t peer_port,
                const uint8_t match_token[16],
                uint32_t lan_ip_be, uint16_t lan_port,
                bool punch_reflexive,
                const uint8_t* peer_v6_addr, uint16_t v6_port) {
    char ip_str[INET_ADDRSTRLEN] = {};
    in_addr ia{};
    ia.s_addr = peer_ip_be;
    inet_ntop(AF_INET, &ia, ip_str, sizeof(ip_str));

    if (match_token) {
        std::memcpy(g_match_token, match_token, MATCH_TOKEN_LEN);
        g_match_token_set = true;
    }

    // Stop any prior burst (e.g. user reconnecting / new match) before
    // launching a fresh one. join() is bounded -- the loop checks
    // g_punching every iteration.
    if (g_punching.exchange(false) && g_punch_thread.joinable()) {
        g_punch_thread.join();
    }

    g_punch_peer = {};
    g_punch_peer.sin_family      = AF_INET;
    g_punch_peer.sin_addr.s_addr = peer_ip_be;
    g_punch_peer.sin_port        = htons(peer_port);

    // Same-LAN candidate (peer's private addr). Punched alongside the reflexive
    // addr below so same-router pairs go direct over the LAN. Skip if it equals
    // the reflexive addr (nothing gained) or is zero (not provided).
    g_have_lan_peer = false;
    if (lan_ip_be != 0 && lan_ip_be != peer_ip_be) {
        g_punch_peer_lan = {};
        g_punch_peer_lan.sin_family      = AF_INET;
        g_punch_peer_lan.sin_addr.s_addr = lan_ip_be;
        g_punch_peer_lan.sin_port        = htons(lan_port ? lan_port : peer_port);
        g_have_lan_peer = true;
        char lan_str[INET_ADDRSTRLEN] = {};
        in_addr la{};
        la.s_addr = lan_ip_be;
        inet_ntop(AF_INET, &la, lan_str, sizeof(lan_str));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: same-LAN candidate %s:%u -- burst will also punch direct over LAN",
            lan_str, (unsigned)ntohs(g_punch_peer_lan.sin_port));
    }

    // Global IPv6 candidate (peer's native v6 + local bind port; no NAT remap).
    g_have_v6_peer = false;
    if (peer_v6_addr && v6_port) {
        g_punch_peer_v6 = {};
        g_punch_peer_v6.sin6_family = AF_INET6;
        std::memcpy(&g_punch_peer_v6.sin6_addr, peer_v6_addr, 16);
        g_punch_peer_v6.sin6_port = htons(v6_port);
        g_have_v6_peer = true;
        char v6s[INET6_ADDRSTRLEN] = {};
        inet_ntop(AF_INET6, &g_punch_peer_v6.sin6_addr, v6s, sizeof(v6s));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: IPv6 candidate [%s]:%u -- burst will also punch direct over v6 "
            "(CGNAT bypass)", v6s, (unsigned)v6_port);
    }

    // Loopback shortcut: same-machine peers don't need NAT punch. Two
    // FM2K instances on 127.0.0.1 were eating ~2.4s each on the
    // relay-fallback timeout and 30×10ms burst, all to fight a NAT
    // that doesn't exist.
    //
    // BUT: only safe when relay is NOT configured. If the hub gave us
    // a relay endpoint (hub.2dfm.org:7712 + session), it means the hub
    // expected a real cross-NAT match -- and the launcher's preflight
    // code has historically misset peer_ip=127.0.0.1 when the public
    // probe failed even on different machines. Skipping the relay in
    // that case left both peers sending HELLO into their own loopback
    // forever. Fall through to the normal punch + relay-wait path so
    // the relay saves us when the loopback assumption is wrong.
    if (peer_ip_be == htonl(INADDR_LOOPBACK) && !g_relay_configured) {
        SOCKET sock = ControlChannel_GetSocket();
        if (sock != INVALID_SOCKET) {
            uint8_t pkt[2 + MATCH_TOKEN_LEN] = {MAGIC, TAG_CTRL_PUNCH};
            std::memcpy(pkt + 2, g_match_token, MATCH_TOKEN_LEN);
            for (int i = 0; i < 3; ++i) {
                fm2k::Sendto4or6(sock,
                       reinterpret_cast<const char*>(pkt), sizeof(pkt),
                       g_punch_peer);
            }
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: loopback peer %s:%u -- same-box, skipping NAT punch / relay wait",
            ip_str, (unsigned)peer_port);
        return;
    }
    if (peer_ip_be == htonl(INADDR_LOOPBACK)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: peer claimed loopback %s:%u BUT relay configured -- "
            "running normal punch + relay path (launcher likely set "
            "127.0.0.1 by mistake during preflight)",
            ip_str, (unsigned)peer_port);
    }

    g_punching.store(true);
    g_punch_thread = std::thread([ip_str_copy = std::string(ip_str), peer_port,
                                  punch_reflexive]() {
        // Boost only this thread's priority -- process-wide boost would
        // starve the game's main loop. timeBeginPeriod(1) tightens
        // Sleep granularity so 10 ms means ~10 ms instead of ~16 ms
        // (Windows default scheduler tick).
        HANDLE th = GetCurrentThread();
        int prev_pri = GetThreadPriority(th);
        SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
        timeBeginPeriod(1);

        SOCKET sock = ControlChannel_GetSocket();
        if (sock == INVALID_SOCKET) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "NAT: punch aborted -- control socket unavailable");
            timeEndPeriod(1);
            SetThreadPriority(th, prev_pri);
            g_punching.store(false);
            return;
        }

        uint8_t pkt[2 + MATCH_TOKEN_LEN] = {MAGIC, TAG_CTRL_PUNCH};
        std::memcpy(pkt + 2, g_match_token, MATCH_TOKEN_LEN);

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: burst punch -> %s:%u (%d packets, ~%d ms)",
            ip_str_copy.c_str(), (unsigned)peer_port,
            PUNCH_PACKETS, PUNCH_PACKETS * PUNCH_PERIOD_MS);

        if (!punch_reflexive) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "NAT: peer reflexive port UNVERIFIED -- skipping reflexive burst "
                "(rely on reverse punch / peer-learning / v6 / relay); still "
                "punching the LAN candidate if present");
        }
        int sent_ok = 0;
        for (int i = 0; i < PUNCH_PACKETS; ++i) {
            if (!g_punching.load()) break;  // peer latched, stop early
            if (punch_reflexive) {
                int sent = fm2k::Sendto4or6(sock,
                                  reinterpret_cast<const char*>(pkt), sizeof(pkt),
                                  g_punch_peer);
                if (sent == (int)sizeof(pkt)) ++sent_ok;
            }
            // Same-LAN candidate gets the same burst. Whichever address answers
            // first authenticates and control_channel peer-learning adopts it
            // (one socket carries control + gekko), so a successful LAN punch
            // routes the whole match over the LAN with zero further config.
            if (g_have_lan_peer) {
                fm2k::Sendto4or6(sock,
                       reinterpret_cast<const char*>(pkt), sizeof(pkt),
                       g_punch_peer_lan);
            }
            if (g_have_v6_peer) {
                // Native v6 -- direct, no NAT translation (NOT Sendto4or6).
                sendto(sock, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                       reinterpret_cast<sockaddr*>(&g_punch_peer_v6),
                       sizeof(g_punch_peer_v6));
            }
            Sleep(PUNCH_PERIOD_MS);
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: burst complete -- %d/%d sent (%s)",
            sent_ok, PUNCH_PACKETS,
            g_punching.load() ? "no peer ack yet" : "peer-latch fired");

        timeEndPeriod(1);
        SetThreadPriority(th, prev_pri);
        g_punching.store(false);

        // Final fallback gate: give the peer a much longer window to
        // hit us back via direct UDP. The 200 ms we used originally
        // was too short -- Netplay_Init runs early in DllMain, before
        // the game's main loop and ControlChannel_Poll start, so
        // inbound CTRL_PUNCH packets can sit in the kernel buffer
        // for several hundred ms before we ever drain them. Use 2 s
        // to cover that startup latency. The user said "we never
        // want burst punch to fail" -- relay is the safety net but
        // direct should always get the chance to win first.
        if (g_relay_configured) {
            // PHASE 1 -- has the direct path proved itself at all?
            for (int i = 0; i < 200 && !g_peer_authenticated.load(); ++i) {
                Sleep(10);
                // If gameplay traffic is already flowing BOTH ways, drop
                // the relay-engage idea entirely. CTRL_PUNCH (0xCD) didn't
                // ack but 0xCC did -- the path between peers is fine for
                // our actual packets. Engaging relay anyway would route
                // every subsequent packet through the hub for no reason
                // and (worse) tear down a working session if relay quality
                // is worse than direct.
                //
                // The test is PeerAckedUs(), NOT ControlChannel_
                // IsConnected(). g_connected goes true on SENDING
                // HELLO_ACK, which needs only an inbound HELLO -- on a
                // one-way path we are "connected" while nothing we send
                // lands, and this skip would then be a lie that leaves us
                // stranded on direct forever.
                if (PeerAckedUs()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "NAT: relay-engage skipped -- direct UDP already "
                        "carrying gameplay traffic BOTH ways (peer acked "
                        "our traffic); CTRL_PUNCH ack lost but path works");
                    return;
                }
                if (g_relay_mode.load()) return;   // adopted from the peer
            }

            if (!g_peer_authenticated.load()) {
                if (!PeerAckedUs()) {
                    EngageRelay("direct punch did not authenticate after 2s");
                }
                return;
            }

            // PHASE 2 -- latched, which proves only PEER -> US.
            //
            // A CTRL_PUNCH we received says the peer can reach us. It says
            // nothing about whether we can reach the peer, and the two are
            // genuinely independent: a NAT that remaps our source port per
            // destination, or a one-sided firewall drop, kills exactly one
            // leg. Before this phase existed, that single inbound punch
            // committed us to direct for the whole session and the match
            // deadlocked (see AdoptRelayFromPeer's comment).
            //
            // So wait for the peer to answer something we sent. HELLO goes
            // out every ~510ms and PING every PING_INTERVAL_MS/2 once
            // connected, so 3s is several chances to be heard, and it caps
            // the player's stare at ~3.3s including the burst.
            for (int i = 0; i < 300 && !PeerAckedUs(); ++i) {
                Sleep(10);
                if (g_relay_mode.load()) return;   // adopted from the peer
            }
            if (!PeerAckedUs()) {
                EngageRelay("peer latched but never answered anything we "
                            "sent in 3s -- one-way path");
            }
        } else if (!g_peer_authenticated.load() && !PeerAckedUs()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "NAT: direct punch failed and no relay configured -- peer "
                "may stay unreachable");
        }
    });
}

void HandleDatagram(const uint8_t* data, size_t len, const sockaddr_storage& from) {
    if (len < 2 || data[0] != MAGIC) return;
    const uint8_t tag = data[1];

    // Family-aware "ip:port" / "[v6]:port" for logging + (on auth) the latch.
    std::string from_s = fm2k::Addr_ActorString(from);

    switch (tag) {
        case TAG_ACK: {
            // Hub's STUN response. data[2..5] = ip_be, data[6..7] = port_be.
            if (len < 2 + 4 + 2) return;
            uint32_t ip_be;
            uint16_t port_be;
            std::memcpy(&ip_be,   data + 2, 4);
            std::memcpy(&port_be, data + 6, 2);
            in_addr ia{};
            ia.s_addr = ip_be;
            char ip_str[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &ia, ip_str, sizeof(ip_str));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "NAT: STUN ack -- reflexive %s:%u",
                ip_str, (unsigned)ntohs(port_be));
            g_reflexive = {};
            g_reflexive.sin_family = AF_INET;
            g_reflexive.sin_addr.s_addr = ip_be;
            g_reflexive.sin_port = port_be;
            g_have_reflexive = true;
            return;
        }
        case TAG_CTRL_PUNCH: {
            // Authenticated punch from peer. data[2..2+16) = match_token.
            if (len < 2 + MATCH_TOKEN_LEN) return;
            if (!g_match_token_set) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "NAT: CTRL_PUNCH from %s dropped -- no local token set",
                    from_s.c_str());
                return;
            }
            if (std::memcmp(data + 2, g_match_token, MATCH_TOKEN_LEN) != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "NAT: CTRL_PUNCH from %s dropped -- token mismatch",
                    from_s.c_str());
                return;
            }
            // First authentic peer punch: latch the address into
            // control_channel's gameplay peer slot and signal the
            // burst thread to stop early. The 0xCC HELLO loop will
            // start hitting the right address from this point on, and
            // the existing peer-learning code in RawReceive's 0xCC
            // branch will keep tracking it across NAT remapping.
            //
            // Subsequent valid CTRL_PUNCH packets are common (peer's
            // burst sends 30) -- log only the first to avoid spamming
            // the debug log; remaining drops are benign.
            const bool first_auth = !g_peer_authenticated.exchange(true);
            if (first_auth) {
                ControlChannel_LatchPeerAddr(from);
                g_punching.store(false);
                // If relay engaged before this auth landed (CTRL_PUNCH
                // can arrive after the burst grace expires because the
                // game's ControlChannel_Poll loop hadn't started yet),
                // turn relay back off -- direct path now works and is
                // strictly cheaper.
                //
                // NOT when the peer put us on the relay. This punch shows
                // the peer reaching US; the peer is relaying because IT
                // could not reach US the other way, and dropping back to
                // direct here would re-open the exact deadlock
                // AdoptRelayFromPeer exists to close. Only the peer can
                // undo the peer's verdict.
                if (g_relay_peer_driven.load()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "NAT: direct punch landed late but relay STAYS -- "
                        "the peer is relaying and cannot hear a direct send");
                } else if (g_relay_mode.exchange(false)) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "NAT: relay disengaged -- direct punch landed late");
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "NAT: CTRL_PUNCH from %s authenticated -- peer latched",
                    from_s.c_str());
            }
            return;
        }
        default:
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "NAT: unknown 0xCD tag=0x%02X from %s (ignoring)",
                (unsigned)tag, from_s.c_str());
            return;
    }
}

// =============================================================================
// Relay
// =============================================================================

bool ConfigureRelay() {
    g_relay_configured = false;
    g_relay_mode.store(false);

    const char* addr_s    = std::getenv("FM2K_HUB_RELAY_ADDR");
    const char* session_s = std::getenv("FM2K_HUB_RELAY_SESSION");
    if (!addr_s || !session_s) return false;

    std::string host;
    uint16_t port = 0;
    if (!ParseHostPort(addr_s, host, port)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: invalid FM2K_HUB_RELAY_ADDR='%s'", addr_s);
        return false;
    }

    g_relay_addr = {};
    g_relay_addr.sin_family = AF_INET;
    g_relay_addr.sin_port   = htons(port);
    if (!ResolveHostA(host, g_relay_addr.sin_addr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: failed to resolve relay host='%s'", host.c_str());
        return false;
    }

    // Session id is the same hex string as match token; decode 32 hex
    // chars to 16 binary bytes (matches hub.py make_relay_session).
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    std::memset(g_relay_session, 0, sizeof(g_relay_session));
    size_t hex_len = std::strlen(session_s);
    if (hex_len > 32) hex_len = 32;
    for (size_t i = 0; i + 1 < hex_len; i += 2) {
        int hi = nibble(session_s[i]);
        int lo = nibble(session_s[i + 1]);
        if (hi < 0 || lo < 0) break;
        g_relay_session[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }

    g_relay_configured = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "NAT: relay configured -> %s:%u (session=%.32s)",
        host.c_str(), (unsigned)port, session_s);
    return true;
}

bool IsRelayMode() {
    return g_relay_mode.load(std::memory_order_acquire);
}

void AdoptRelayFromPeer() {
    if (!g_relay_configured) return;
    // Order matters: set the sticky bit BEFORE relay mode. A CTRL_PUNCH
    // racing us on the poll thread must never observe relay-on with
    // peer-driven still clear, or it would disengage what we just adopted.
    g_relay_peer_driven.store(true);
    if (g_relay_mode.exchange(true)) return;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "NAT: relay mode ENGAGED -- peer is relaying to us (adopted its "
        "verdict); our direct sends were not reaching it");
}

void NotePeerAckedUs() {
    if (g_peer_acked_us.exchange(true)) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "NAT: peer answered our traffic -- path confirmed BIDIRECTIONAL");
}

bool PeerAckedUs() {
    return g_peer_acked_us.load(std::memory_order_acquire);
}

void ForceRelayMode() {
    if (!g_relay_configured) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NAT: ForceRelayMode ignored -- relay not configured");
        return;
    }
    g_relay_mode.store(true);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "NAT: relay mode FORCED via diagnostic");
}

const sockaddr_in* GetRelayAddr() {
    return g_relay_configured ? &g_relay_addr : nullptr;
}

const uint8_t* GetRelaySessionId() {
    return g_relay_configured ? g_relay_session : nullptr;
}

bool SendToMaybeRelayWrapped(uintptr_t s, const void* buf, int len,
                             const sockaddr_in& dest) {
    if (!g_relay_configured || len < 0) return false;
    if (dest.sin_addr.s_addr != g_relay_addr.sin_addr.s_addr ||
        dest.sin_port != g_relay_addr.sin_port) return false;
    uint8_t wrapped[2048];
    size_t wl = WrapForRelay(reinterpret_cast<const uint8_t*>(buf),
                             static_cast<size_t>(len), wrapped, sizeof(wrapped));
    if (wl == 0) return true;  // oversize: dropped, mirroring RawSend's relay branch
    fm2k::Sendto4or6(static_cast<SOCKET>(s),
                     reinterpret_cast<const char*>(wrapped),
                     static_cast<int>(wl), dest);
    return true;
}

size_t WrapForRelay(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap) {
    constexpr size_t HDR = 2 + 16;
    if (out_cap < HDR + len) return 0;
    out[0] = MAGIC_RELAY;
    out[1] = TAG_RELAY_DATA;
    std::memcpy(out + 2, g_relay_session, 16);
    std::memcpy(out + HDR, in, len);
    return HDR + len;
}

bool UnwrapFromRelay(const uint8_t* data, size_t len,
                     const uint8_t** out_inner, size_t* out_inner_len) {
    constexpr size_t HDR = 2 + 16;
    if (!g_relay_configured) return false;
    if (len < HDR + 1) return false;
    if (data[0] != MAGIC_RELAY || data[1] != TAG_RELAY_DATA) return false;
    if (std::memcmp(data + 2, g_relay_session, 16) != 0) return false;
    *out_inner     = data + HDR;
    *out_inner_len = len - HDR;
    return true;
}

bool GetMatchTokenHex(char* out, size_t out_size) {
    if (!out || out_size < 33) return false;
    out[0] = '\0';
    if (!g_match_token_set) return false;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < MATCH_TOKEN_LEN; ++i) {
        out[i * 2 + 0] = hex[(g_match_token[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[g_match_token[i]        & 0xF];
    }
    out[MATCH_TOKEN_LEN * 2] = '\0';
    return true;
}

}  // namespace fm2k::nat
