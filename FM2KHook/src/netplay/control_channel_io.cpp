// Control Channel -- RAW UDP send/receive (split from control_channel.cpp).
// RawSend wraps relay/direct egress; RawReceive demuxes control (0xCC) / NAT
// (0xCD) / spectator-UDP (0xCE) / GekkoNet packets. Shares state via
// control_channel_internal.h. CONTRACT: run under g_poll_mutex (RawReceive
// mutates g_gekko_packet_queue / g_recv_buffer). ENGINE-AGNOSTIC.
#include "control_channel.h"
#include "control_channel_internal.h"
#include "host_clock.h"         // host-clock sync sampling on PING/PONG
#include "globals.h"            // g_player_index (peer-addr learning context)
#include "nat_traversal.h"      // fm2k::nat relay wrap/unwrap + 0xCD datagrams
#include <SDL3/SDL_log.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "addr6_util.h"        // Sendto4or6 + v4-mapped un-map helpers

// =============================================================================
// RAW SEND/RECEIVE
// =============================================================================

void RawSend(const void* data, size_t len) {
    if (g_socket == INVALID_SOCKET) return;

    // Relay mode: wrap every gameplay byte in a 0xCF envelope and ship
    // to the hub-supplied relay endpoint. The relay forwards by
    // session_id to the other peer, who unwraps in their own
    // RawReceive. Direct g_remote_sockaddr is bypassed entirely.
    if (::fm2k::nat::IsRelayMode()) {
        const sockaddr_in* relay = ::fm2k::nat::GetRelayAddr();
        if (!relay) return;
        // 18-byte header (0xCF 0x01 + 16-byte session_id) + payload.
        // Most FM2K control / GekkoNet packets are well under 1500B so
        // a 2KB stack buffer is plenty.
        uint8_t wrapped[2048];
        size_t wrapped_len = ::fm2k::nat::WrapForRelay(
            reinterpret_cast<const uint8_t*>(data), len,
            wrapped, sizeof(wrapped));
        if (wrapped_len == 0) return;  // payload too large
        int result = fm2k::Sendto4or6(g_socket,
                            reinterpret_cast<const char*>(wrapped),
                            (int)wrapped_len, *relay);
        if (result == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "NetSocket: relay sendto failed: %d", err);
            }
        }
        return;
    }

    if (!fm2k::Addr_HasPort(g_remote_sockaddr)) return;  // peer addr not known yet

    int result = fm2k::SendtoStorage(g_socket, (const char*)data, (int)len,
                                     g_remote_sockaddr);
    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: sendto failed: %d", err);
        }
    }
}

// Receive all pending packets, filter control vs GekkoNet
// Race detector (FM2K_RACE_DETECT=1). RawReceive mutates the shared
// g_gekko_packet_queue / g_recv_buffer. It is meant to run only under
// g_poll_mutex; the MM-timer path holds it, but MultiplexAdapter_Receive
// (the bug) did not. This guard increments an atomic on entry and
// decrements on exit -- if two threads are ever inside concurrently the
// depth exceeds 1, which is the smoking-gun data race that corrupts
// GekkoNet's heap. Deterministic proof for the A/B: unfixed build trips
// this under clumsy within seconds; fixed build (lock added) never does.
struct RawReceiveRaceGuard {
    static std::atomic<int>      s_depth;
    static std::atomic<uint32_t> s_first_tid;
    bool armed;
    RawReceiveRaceGuard() {
        static int cached = -1;
        if (cached < 0) {
            const char* v = std::getenv("FM2K_RACE_DETECT");
            cached = (v && v[0] == '1') ? 1 : 0;
        }
        armed = (cached == 1);
        if (!armed) return;
        const uint32_t tid = (uint32_t)GetCurrentThreadId();
        const int d = s_depth.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (d == 1) {
            s_first_tid.store(tid, std::memory_order_relaxed);
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[RACE-DETECT] RawReceive concurrent entry: depth=%d "
                "this_tid=%u other_tid=%u -- g_gekko_packet_queue mutated "
                "from two threads WITHOUT g_poll_mutex (heap-corruption bug)",
                d, tid, s_first_tid.load(std::memory_order_relaxed));
        }
    }
    ~RawReceiveRaceGuard() {
        if (armed) s_depth.fetch_sub(1, std::memory_order_acq_rel);
    }
};
std::atomic<int>      RawReceiveRaceGuard::s_depth{0};
std::atomic<uint32_t> RawReceiveRaceGuard::s_first_tid{0};

void RawReceive() {
    if (g_socket == INVALID_SOCKET) return;
    RawReceiveRaceGuard _race_guard;

    while (true) {
        // Receive into a sockaddr_in6 (28 bytes) so the dual-stack socket can
        // report v4-mapped / native-v6 sources. from_len is in/out -- re-set to
        // the FULL size every iteration or a stale 16 would truncate the
        // v4-mapped octets and mis-detect every v4 peer as native v6.
        sockaddr_in6 from6;
        int from_len = sizeof(from6);
        static_assert(sizeof(from6) >= sizeof(sockaddr_in),
                      "recv buffer must hold a v4 source too");

        int recv_len = recvfrom(g_socket, g_recv_buffer, RECV_BUFFER_SIZE, 0,
                                (sockaddr*)&from6, &from_len);

        if (recv_len == SOCKET_ERROR) {
            int err = WSAGetLastError();
            // WSAEWOULDBLOCK = no data available (normal)
            // WSAECONNRESET (10054) = remote port not open yet (normal during connection)
            if (err != WSAEWOULDBLOCK && err != WSAECONNRESET) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: recvfrom failed: %d", err);
            }
            break;  // No more data
        }

        if (recv_len == 0) break;

        // SHORT-PACKET HYGIENE. Every 0xCC dispatch below casts the buffer to
        // a CtrlPacket and reads named fields WITHOUT re-checking the datagram
        // length past the 7-byte header, so any field appended to the union
        // (session_id in host_config, 2026-08) would read whatever the PREVIOUS
        // datagram left in this static buffer when the sender is an older build
        // whose packet is shorter. Zero the tail so a missing field reads as its
        // "not announced" sentinel (0) instead of stale bytes. Bounded to one
        // CtrlPacket past the payload -- this is per-datagram on the poll path,
        // not per-frame, and 64 bytes of memset is free next to the recvfrom.
        if (recv_len > 0 && (size_t)recv_len < RECV_BUFFER_SIZE) {
            const size_t tail = RECV_BUFFER_SIZE - (size_t)recv_len;
            std::memset(g_recv_buffer + recv_len, 0,
                        tail < sizeof(CtrlPacket) ? tail : sizeof(CtrlPacket));
        }

        // Normalize the source to a real-family sockaddr_storage BEFORE any
        // compare/stamp/learn/dispatch below. On the dual-stack socket v4 peers
        // arrive v4-mapped (un-mapped to AF_INET here); native v6 peers are now
        // supported (kept AF_INET6). On the v4-fallback socket the kernel wrote
        // a plain sockaddr_in. Addr_ActorString / Addr_Equal handle either
        // family, so the gekko actor-string match holds for v4 and v6 alike.
        sockaddr_storage from_addr;
        fm2k::Addr_NormalizeRecv(from6, from_addr);

        g_last_recv_time = GetTimeMs();

        // Relay-envelope unwrap. If this is a 0xCF packet for our
        // configured relay session, strip the 18-byte header and treat
        // the inner bytes as if they arrived directly from the peer.
        // Skip peer-address learning for the inner packet -- the actual
        // sockaddr is the relay, which we don't want latched as peer.
        bool from_relay = false;
        const uint8_t* eff_data = reinterpret_cast<const uint8_t*>(g_recv_buffer);
        size_t eff_len = static_cast<size_t>(recv_len);
        const uint8_t* inner = nullptr;
        size_t inner_len = 0;
        if (recv_len >= 1 && static_cast<uint8_t>(g_recv_buffer[0]) == 0xCF) {
            if (::fm2k::nat::UnwrapFromRelay(eff_data, eff_len, &inner, &inner_len)) {
                eff_data = inner;
                eff_len  = inner_len;
                from_relay = true;
            } else {
                continue;  // bad envelope or wrong session -- drop
            }
        }
        const uint8_t first = eff_len ? eff_data[0] : 0;

        // Check if this is a control packet (magic byte 0xCC)
        if (eff_len >= 1 && first == CTRL_MAGIC) {
            // Peer-address learning. Skip when the packet came in via
            // the relay envelope -- the actual sockaddr is the relay,
            // and latching the relay as g_remote_sockaddr would later
            // cause RawSend (in non-relay mode) to send to the relay
            // instead of the peer. In relay mode we don't need a
            // peer addr at all (RawSend's relay branch ignores it).
            // Type gate shared by learn + adoption below: only message types
            // that ONLY the session peer sends may change the peer address.
            // A spectator's SPEC_* control traffic (same public IP behind a
            // shared NAT; always same-IP on loopback) must never be latched
            // as the peer -- neither pre-connect (learn) nor mid-session
            // (adoption); the pre-connect hole let a spectator that joined
            // before the peer's HELLO become the "peer".
            const CtrlMsg ctrl_type =
                (eff_len >= sizeof(CtrlPacketHeader))
                    ? reinterpret_cast<const CtrlPacket*>(eff_data)->header.type
                    : CtrlMsg::DISCONNECT;
            const bool peer_only_type =
                ctrl_type == CtrlMsg::PING || ctrl_type == CtrlMsg::PONG ||
                ctrl_type == CtrlMsg::HELLO || ctrl_type == CtrlMsg::HELLO_ACK ||
                ctrl_type == CtrlMsg::BATTLE_READY || ctrl_type == CtrlMsg::BATTLE_ACK ||
                ctrl_type == CtrlMsg::BATTLE_ENTERING || ctrl_type == CtrlMsg::BATTLE_START ||
                ctrl_type == CtrlMsg::BATTLE_END;
            if (!from_relay && !g_connected && peer_only_type &&
                !fm2k::Addr_Equal(g_remote_sockaddr, from_addr)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "NetSocket: Learned peer address %s (was %s)",
                            fm2k::Addr_ActorString(from_addr).c_str(),
                            fm2k::Addr_ActorString(g_remote_sockaddr).c_str());
                g_remote_sockaddr = from_addr;
            }
            // Mid-session PORT-REBIND adoption (task #52): a carrier NAT can
            // remap the peer's flow mid-session -- same public IP, new source
            // port, old mapping dead. Without adopting NOW, our sends keep
            // dying into the dead mapping until the 1.5-10s connection
            // timeout tears everything down (in-flight battle freezes at the
            // prediction wall; a CSS straddling it force-completes into a
            // desync). Same-IP-only: a different IP is not adoptable on
            // faith (spoof risk) -- log it for visibility. Rate-limited so
            // an old/new source interleave can't flap the addr every packet.
            else if (!from_relay && g_connected &&
                     !fm2k::Addr_Equal(g_remote_sockaddr, from_addr)) {
                // Identity gate: peer_only_type (hoisted above, shared with
                // the pre-connect learn). Observed without it: host
                // flip-flopped 7001<->7002 every 2s from spectator SPEC_*
                // traffic, gate RED.
                static uint32_t s_last_adopt_ms = 0;
                const uint32_t now_ms = GetTickCount();
                if (peer_only_type &&
                    fm2k::Addr_SameIp(g_remote_sockaddr, from_addr)) {
                    if (now_ms - s_last_adopt_ms >= 2000) {
                        s_last_adopt_ms = now_ms;
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "NetSocket: peer PORT REBIND -- adopted %s (was %s), "
                            "carrier-NAT mid-session remap",
                            fm2k::Addr_ActorString(from_addr).c_str(),
                            fm2k::Addr_ActorString(g_remote_sockaddr).c_str());
                        g_remote_sockaddr = from_addr;
                    }
                } else if (peer_only_type) {
                    // A peer-only message type from a DIFFERENT IP: either a
                    // spoofer or an IP-changing CGNAT remap (not adoptable on
                    // faith). Spectator SPEC_* traffic never reaches here.
                    static uint32_t s_last_foreign_ms = 0;
                    if (now_ms - s_last_foreign_ms >= 5000) {
                        s_last_foreign_ms = now_ms;
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "NetSocket: peer-type CTRL from foreign source %s while "
                            "connected to %s -- ignored (different IP, not "
                            "adoptable)",
                            fm2k::Addr_ActorString(from_addr).c_str(),
                            fm2k::Addr_ActorString(g_remote_sockaddr).c_str());
                    }
                }
            }

            // Control packet - process immediately
            if (eff_len >= sizeof(CtrlPacketHeader)) {
                const CtrlPacket* packet = reinterpret_cast<const CtrlPacket*>(eff_data);

                // Update ack
                if (packet->header.seq > g_recv_seq) {
                    g_recv_seq = packet->header.seq;
                }
                g_recv_ack = packet->header.ack;

                // Handle PING/PONG internally before callback
                if (packet->header.type == CtrlMsg::PING) {
                    // Sample the sender's host-clock stamp + frame (clock sync +
                    // frame projection), then respond with a PONG carrying our own.
                    if (fm2k::hostclock::Enabled()) {
                        uint64_t their = ((uint64_t)packet->data.ping.host_us_hi << 32)
                                       | packet->data.ping.host_us_lo;
                        if (their) fm2k::hostclock::OnInboundTimestamp(
                                       their, packet->data.ping.frame,
                                       fm2k::hostclock::LocalMicros());
                    }
                    CtrlPacket pong = {};
                    pong.header.type = CtrlMsg::PONG;
                    pong.data.ping.send_ms = packet->data.ping.send_ms;  // echo for RTT
                    uint64_t h = fm2k::hostclock::Enabled()
                               ? fm2k::hostclock::StampOutbound() : 0;
                    pong.data.ping.host_us_lo = (uint32_t)(h & 0xFFFFFFFFu);
                    pong.data.ping.host_us_hi = (uint32_t)(h >> 32);
                    pong.data.ping.frame      = fm2k::hostclock::Enabled()
                                              ? fm2k::hostclock::LocalFrame() : 0;
                    ControlChannel_Send(pong);
                } else if (packet->header.type == CtrlMsg::PONG) {
                    // Calculate RTT from echoed timestamp, then sample the peer's
                    // host-clock stamp + frame.
                    ControlChannel_HandlePong(packet->data.ping.send_ms);
                    if (fm2k::hostclock::Enabled()) {
                        uint64_t their = ((uint64_t)packet->data.ping.host_us_hi << 32)
                                       | packet->data.ping.host_us_lo;
                        if (their) fm2k::hostclock::OnInboundTimestamp(
                                       their, packet->data.ping.frame,
                                       fm2k::hostclock::LocalMicros());
                    }
                } else if (packet->header.type == CtrlMsg::DELAY_PROPOSAL) {
                    // Stash the peer's delay candidate (#24). Battle
                    // start adopts max(local, this) so both peers run
                    // identical input delay.
                    const int d = packet->data.delay_proposal.delay;
                    if (d >= 0 && d <= 16) {
                        g_remote_delay_candidate.store(
                            d, std::memory_order_relaxed);
                    }
                }

                // Forward all messages to callback (including PING/PONG for logging)
                if (g_msg_callback) {
                    // OnControlMessage ignores the addr family; the peer is
                    // already learned above. v4 view is safe for both families.
                    g_msg_callback(packet,
                        *reinterpret_cast<const sockaddr_in*>(&from_addr));
                }
            }
        } else if (eff_len >= 1 && first == 0xCD) {
            // NAT-layer datagram (0xCD) -- STUN ack or peer punch probe.
            // Defined in nat_traversal.h. Returning here keeps the byte
            // out of GekkoNet's queue and the spectator path.
            ::fm2k::nat::HandleDatagram(eff_data, eff_len, from_addr);
        } else if (eff_len >= 1 && first == 0xCB) {
            // ReliableChannel datagram (0xCB) -- reliable-ordered + FEC message
            // layer over UDP (reliable.io endpoint). Body after the tag goes to
            // the RC endpoint; delivery is dispatched by channel. Runs under
            // g_poll_mutex (this loop's contract), which RC requires.
            extern void ReliableChannel_OnDatagram(const sockaddr_storage& from,
                                                   const uint8_t* body, int body_len);
            ReliableChannel_OnDatagram(
                from_addr,
                reinterpret_cast<const uint8_t*>(eff_data) + 1,
                static_cast<int>(eff_len) - 1);
        } else if (eff_len >= 1 && first == 0xCE) {
            // 0xCE was the spectator UDP input accelerator, deleted with the
            // TCP transport it accelerated around. Swallow the tag rather
            // than hand it to GekkoNet: an old peer can still emit one and
            // gekko would log a bad-magic discard for every datagram.
        } else {
            // GekkoNet packet - queue for adapter
            std::vector<char> pkt_data(
                reinterpret_cast<const char*>(eff_data),
                reinterpret_cast<const char*>(eff_data) + eff_len);
            // Source-address stamping. GekkoNet drops any inbound packet
            // whose source string doesn't byte-match the address handed to
            // gekko_add_actor (netplay.cpp:1928 / 2520). For DIRECT packets
            // that's from_addr (the genuine peer source) -- unchanged, the
            // path stays bit-identical to before (preserves D9). For RELAY
            // packets from_addr is the relay (e.g. 127.0.0.1:7712), NOT the
            // peer, so stamping it would make every relayed gekko input get
            // dropped and stall CSS lockstep forever. Stamp g_remote_sockaddr
            // instead: it is the configured remote (peer-addr learning is
            // skipped for relayed packets at ~507, so it stays exactly as
            // FM2K_REMOTE_ADDR was parsed). The actor string CANNOT diverge
            // from this -- both the actor string (NetSocket_GetRemoteAddr ->
            // inet_ntop) and the receive string (MultiplexAdapter_Receive ->
            // inet_ntoa) are derived from this same g_remote_sockaddr, and
            // it was set via inet_pton (dotted-decimal only, no hostnames),
            // so the two formattings produce identical "ip:port" text.
            // Rebind survival (task #52): gekko's actor match is EXACT --
            // after a carrier-NAT port remap, direct packets arrive from a
            // NEW port and would all be silently dropped (and NetworkHealth's
            // RTT match breaks -> ping pins 0 -> one-sided rollback). Stamp
            // any same-IP direct packet with the PINNED actor addr so the
            // in-flight session keeps accepting; a genuinely foreign IP still
            // gets its raw source (gekko drops it, as before).
            const sockaddr_storage& stamp =
                from_relay
                    ? (g_gekko_actor_pinned ? g_gekko_actor_addr
                                            : g_remote_sockaddr)
                    : (g_gekko_actor_pinned &&
                       fm2k::Addr_SameIp(g_gekko_actor_addr, from_addr))
                        ? g_gekko_actor_addr
                        : from_addr;
            g_gekko_packet_queue.push_back({std::move(pkt_data), stamp});
        }
    }
}
