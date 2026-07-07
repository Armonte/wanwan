// reliable_channel_net.cpp — address-keyed glue between the ReliableChannel
// layer and the shared control-channel UDP socket.
//
// One reliable.io endpoint per peer address (keyed by Addr_ActorString), so a
// host serves N spectators (1:N) and the player channel (1:1) over the same
// socket + 0xCB wire tag. Each endpoint transmits to ITS peer address via
// SendtoStorage; inbound 0xCB datagrams are routed to the endpoint for their
// source address. Update() pumps every endpoint (acks + retransmit + carrier).
#include "reliable_channel.h"
#include "reliable_channel_net.h"
#include "control_channel_internal.h"
#include "addr6_util.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include <SDL3/SDL_log.h>

namespace {

struct RcPeer {
    sockaddr_storage addr{};
    fm2k::rc::Endpoint* ep = nullptr;
    double last_activity = 0.0;   // NowSeconds() of last send/recv touch (for eviction)
};

std::map<std::string, std::unique_ptr<RcPeer>> g_peers;  // ActorString -> peer

// DoS bound: every inbound 0xCB from any (spoofable) source used to allocate a
// permanent endpoint, so an attacker could spray spoofed sources and exhaust
// RAM + O(N)-per-poll CPU until the frame budget collapsed. Cap the live
// endpoint set well above the real max (max_spectators + 1 player + headroom),
// evict the LEAST-RECENTLY-ACTIVE peer when full (a one-shot spoofer is always
// stalest, so sustained legit peers survive), and idle-sweep in NetUpdate.
constexpr size_t RC_MAX_PEERS     = 64;
constexpr double RC_PEER_IDLE_SEC = 30.0;
ReliableChannel_DeliverFn g_app_deliver = nullptr;
void* g_app_deliver_ctx = nullptr;
std::chrono::steady_clock::time_point g_t0;
bool   g_inited = false;
bool   g_fec_enabled = true;
uint8_t g_fec_k = 4;
uint8_t g_fec_m = 4;
bool    g_fec_adaptive = true;  // default: retune M to measured loss
double g_resend_seconds = 0.0;  // 0 => auto
double g_rate_pps = 0.0;        // 0 => codec default
double g_cwnd = 0.0;            // 0 => codec default

double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
}

// RC egress -> this endpoint's peer address (ctx is the RcPeer). `data` already
// carries the 0xCB tag from the RC TransmitCb. Spectators are direct even in
// relay mode (matches the existing spectator UDP path); the player channel,
// which GekkoNet already covers, is not migrated here.
void RcRawSend(void* ctx, const uint8_t* data, int len) {
    RcPeer* peer = static_cast<RcPeer*>(ctx);
    if (g_socket == INVALID_SOCKET) return;
    fm2k::SendtoStorage(g_socket, reinterpret_cast<const char*>(data), len, peer->addr);
}

// RC delivery -> app, tagged with the source peer address.
void RcDeliver(void* ctx, uint8_t channel, const uint8_t* data, int len) {
    RcPeer* peer = static_cast<RcPeer*>(ctx);
    if (g_app_deliver) g_app_deliver(g_app_deliver_ctx, peer->addr, channel, data, len);
}

// Drop the endpoint whose key is `key` (destroys the reliable.io endpoint).
void EvictPeer(const std::string& key) {
    auto it = g_peers.find(key);
    if (it == g_peers.end()) return;
    if (it->second->ep) fm2k::rc::Destroy(it->second->ep);
    g_peers.erase(it);
}

RcPeer* GetOrCreate(const sockaddr_storage& addr) {
    std::string key = fm2k::Addr_ActorString(addr);
    auto it = g_peers.find(key);
    if (it != g_peers.end()) {
        it->second->last_activity = NowSeconds();
        return it->second.get();
    }
    // At capacity: evict the least-recently-active peer to make room. A
    // spoofed one-shot source never sustains traffic, so it is always the
    // stalest entry and gets reclaimed before any real, active peer.
    if (g_peers.size() >= RC_MAX_PEERS) {
        auto stalest = g_peers.begin();
        for (auto p = g_peers.begin(); p != g_peers.end(); ++p) {
            if (p->second->last_activity < stalest->second->last_activity) stalest = p;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "ReliableChannel: peer cap %zu reached — evicting stalest %s to admit %s",
            RC_MAX_PEERS, stalest->first.c_str(), key.c_str());
        EvictPeer(stalest->first);
    }
    auto peer = std::make_unique<RcPeer>();
    peer->addr = addr;
    peer->last_activity = NowSeconds();
    RcPeer* raw = peer.get();
    raw->ep = fm2k::rc::Create(/*id*/1, &RcRawSend, raw, &RcDeliver, raw, NowSeconds());
    if (!raw->ep) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ReliableChannel: endpoint create failed for %s", key.c_str());
        return nullptr;
    }
    fm2k::rc::SetFec(raw->ep, g_fec_enabled, g_fec_k, g_fec_m);
    fm2k::rc::SetFecAdaptive(raw->ep, g_fec_adaptive);
    fm2k::rc::SetResendSeconds(raw->ep, g_resend_seconds);
    if (g_rate_pps > 0.0 || g_cwnd > 0.0)
        fm2k::rc::SetPacing(raw->ep, g_rate_pps, /*burst_max*/0.0, static_cast<int>(g_cwnd));
    g_peers.emplace(std::move(key), std::move(peer));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ReliableChannel: endpoint up for %s (fec=%d k=%d)",
                fm2k::Addr_ActorString(addr).c_str(), (int)g_fec_enabled, (int)g_fec_k);
    return raw;
}

}  // namespace

void ReliableChannel_NetInit() {
    if (g_inited) return;
    g_t0 = std::chrono::steady_clock::now();
    g_inited = true;
    if (const char* v = std::getenv("FM2K_RC_FEC")) g_fec_enabled = !(v[0] == '0' && v[1] == '\0');
    if (const char* v = std::getenv("FM2K_RC_FEC_K")) { int n = std::atoi(v); if (n >= 1 && n <= 64) g_fec_k = (uint8_t)n; }
    if (const char* v = std::getenv("FM2K_RC_FEC_M")) { int n = std::atoi(v); if (n >= 1 && n <= 16) { g_fec_m = (uint8_t)n; g_fec_adaptive = false; } }  // explicit M => fixed
    if (const char* v = std::getenv("FM2K_RC_FEC_ADAPTIVE")) g_fec_adaptive = !(v[0] == '0' && v[1] == '\0');
    if (const char* v = std::getenv("FM2K_RC_RESEND_MS")) { int ms = std::atoi(v); g_resend_seconds = ms > 0 ? ms / 1000.0 : 0.0; }
    if (const char* v = std::getenv("FM2K_RC_RATE_PPS")) { int n = std::atoi(v); if (n > 0) g_rate_pps = n; }
    if (const char* v = std::getenv("FM2K_RC_CWND")) { int n = std::atoi(v); if (n > 0) g_cwnd = n; }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ReliableChannel: registry init (0xCB), fec=%d k=%d", (int)g_fec_enabled, (int)g_fec_k);
}

void ReliableChannel_NetShutdown() {
    for (auto& kv : g_peers) if (kv.second->ep) fm2k::rc::Destroy(kv.second->ep);
    g_peers.clear();
    g_inited = false;
}

void ReliableChannel_SetDeliver(ReliableChannel_DeliverFn fn, void* ctx) {
    g_app_deliver = fn;
    g_app_deliver_ctx = ctx;
}

void ReliableChannel_SendTo(const sockaddr_storage& to, uint8_t channel, int cls,
                            const uint8_t* data, int len) {
    if (!g_inited) return;
    RcPeer* peer = GetOrCreate(to);
    if (peer && peer->ep)
        fm2k::rc::Send(peer->ep, channel, static_cast<fm2k::rc::Class>(cls), data, len);
}

// From RawReceive's 0xCB branch. Runs under g_poll_mutex (RawReceive contract).
void ReliableChannel_OnDatagram(const sockaddr_storage& from, const uint8_t* body, int body_len) {
    if (!g_inited) return;
    RcPeer* peer = GetOrCreate(from);
    if (peer && peer->ep) fm2k::rc::OnDatagram(peer->ep, body, body_len);
}

// From PollImplLocked (under g_poll_mutex).
void ReliableChannel_NetUpdate() {
    if (!g_inited) return;
    double now = NowSeconds();
    // Idle-sweep: reclaim endpoints with no send/recv activity for a while
    // (dead spectators, spoofed one-shot sources). Bounds steady-state memory
    // + per-poll cost even below the hard cap. Collect-then-erase so we don't
    // invalidate the iterator mid-loop.
    for (auto it = g_peers.begin(); it != g_peers.end();) {
        if (now - it->second->last_activity >= RC_PEER_IDLE_SEC) {
            if (it->second->ep) fm2k::rc::Destroy(it->second->ep);
            it = g_peers.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& kv : g_peers) if (kv.second->ep) fm2k::rc::Update(kv.second->ep, now);
}
