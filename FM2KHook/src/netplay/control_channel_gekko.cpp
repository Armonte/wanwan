// Control Channel -- GekkoNet custom adapter (MultiplexAdapter). Split from
// control_channel.cpp; shares the control socket + queues, drains RawReceive
// under g_poll_mutex (the cross-thread heap-corruption fix lives in
// MultiplexAdapter_Receive). Shares state via control_channel_internal.h.
// ENGINE-AGNOSTIC.
#include "control_channel.h"
#include "control_channel_internal.h"
#include "gekkonet.h"
#include "nat_traversal.h"      // fm2k::nat::IsRelayMode
#include <SDL3/SDL_log.h>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "addr6_util.h"        // Sendto4or6 (dual-stack v4-mapped send)

// =============================================================================
// GEKKONET CUSTOM ADAPTER
// Shares socket with control channel, filters control packets
// =============================================================================

// Adapter send function - sends directly via our socket.
//
// Pre-spectator era this ignored `addr` and sent to the single peer via
// RawSend → g_remote_sockaddr. With spectator support GekkoNet now
// addresses multiple destinations (player + spectators), so we honor the
// supplied address. Format on the wire is "ip:port" (matches what
// MultiplexAdapter_Receive constructs from inbound sockaddr_in via
// inet_ntoa+ntohs). Falls back to RawSend(g_remote_sockaddr) when the
// addr string can't be parsed (defensive).
static void MultiplexAdapter_Send(GekkoNetAddress* addr, const char* data, int length) {
    if (g_socket == INVALID_SOCKET) return;
    if (!addr || !addr->data || addr->size <= 0) {
        RawSend(data, length);
        return;
    }

    // Parse "ip:port" — addr->data may not be null-terminated, copy out.
    char addr_buf[64] = {};
    int n = (addr->size < (int)sizeof(addr_buf) - 1) ? addr->size : (int)sizeof(addr_buf) - 1;
    std::memcpy(addr_buf, addr->data, n);
    char* colon = std::strrchr(addr_buf, ':');
    if (!colon) {
        RawSend(data, length);
        return;
    }
    *colon = '\0';
    const char* ip_str = addr_buf;
    const int   port   = std::atoi(colon + 1);
    if (port <= 0 || port > 65535) {
        RawSend(data, length);
        return;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip_str, &dst.sin_addr) != 1) {
        RawSend(data, length);
        return;
    }

    // In relay mode all gameplay traffic must still go through the relay
    // wrapper. RawSend handles that path internally; we keep the explicit
    // peer address only for direct (non-relay) sends.
    //
    // KNOWN LIMITATION (cross-region 40k): spectators go DIRECT ONLY. The 0xCF
    // relay is a 2-slot A<->B pair keyed by one session_id, so it can't fan a
    // host out to many spectators. A spectator behind symmetric/CGNAT that
    // can't hole-punch the host is therefore currently UNREACHABLE. The real
    // fix is a spectator-aware relay (hub multiplexes one host to N spec
    // session_ids) or the existing hub WS-fanout path (FM2K_SPEC_TRANSPORT=
    // relay) — NOT this P2P 0xCF envelope. Until then, symmetric-NAT specs
    // must use the WS-relay transport.
    if (::fm2k::nat::IsRelayMode()) {
        RawSend(data, length);
        return;
    }

    int result = fm2k::Sendto4or6(g_socket, data, length, dst);
    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "MultiplexAdapter: sendto(%s:%d) failed: %d", ip_str, port, err);
        }
    }
}

// Adapter receive function - returns queued GekkoNet packets
static GekkoNetResult** MultiplexAdapter_Receive(int* length) {
    // CRITICAL: hold g_poll_mutex for the whole body. This runs on the
    // MAIN thread (gekko's receive_data callback, fired from
    // gekko_network_poll / gekko_update_session). RawReceive() push_backs
    // onto g_gekko_packet_queue, and the MM-timer worker thread
    // (KeepaliveTimerProc -> PollImplLocked -> RawReceive) push_backs onto
    // the SAME vector under this mutex. Without taking it here, the two
    // threads mutate g_gekko_packet_queue / g_recv_buffer / g_gekko_result_ptrs
    // concurrently: a push_back reallocation on the timer thread frees the
    // buffer this thread is iterating, the freed block is recycled into
    // GekkoNet's own heap (the session_health std::map node pool), and the
    // received InputMsg bytes land inside a tree node -> AV the next time
    // SendSessionHealthCheck walks the map (registers full of the
    // "Gekko::InputMsg" serializer tag). This was the intermittent
    // netplay-only counterhit crash (babel, pkmncc, 0.2.71): counterhits
    // spawn extra effects -> heavier frames -> gekko polls harder -> the
    // race window widens. RawReceive's own contract ("touching this
    // without holding g_poll_mutex is a bug") was being violated here.
    // The timer thread uses try_lock, so it simply skips its poll on the
    // ticks we hold the lock -- no deadlock, no recursion (the main-thread
    // gekko_* callers do NOT hold g_poll_mutex).
    std::lock_guard<std::mutex> lock(g_poll_mutex);

    // Clear previous pointer array (results were freed by GekkoNet)
    g_gekko_result_ptrs.clear();

    // Process any pending packets first (in case Poll wasn't called)
    RawReceive();

    // Debug logging removed - too verbose for production

    // Convert queued packets to GekkoNet format
    // IMPORTANT: Each result must be heap-allocated because GekkoNet will free them!
    for (auto& [pkt_data, from_addr] : g_gekko_packet_queue) {
        // Allocate the result struct on heap - GekkoNet will free it
        GekkoNetResult* result = (GekkoNetResult*)malloc(sizeof(GekkoNetResult));

        // Canonical family-aware actor string (v4 byte-identical to the old
        // inet_ntoa form; v6 "[..]:port"). MUST match gekko_add_actor's string
        // or gekko drops every packet from this source.
        std::string addr_s = fm2k::Addr_ActorString(from_addr);

        result->addr.data = _strdup(addr_s.c_str());  // GekkoNet will free this
        result->addr.size = (int)addr_s.size();
        result->data = (char*)malloc(pkt_data.size());
        memcpy(result->data, pkt_data.data(), pkt_data.size());
        result->data_len = (int)pkt_data.size();

        g_gekko_result_ptrs.push_back(result);
    }

    // Clear the queue
    g_gekko_packet_queue.clear();

    *length = (int)g_gekko_result_ptrs.size();
    return g_gekko_result_ptrs.empty() ? nullptr : g_gekko_result_ptrs.data();
}

// Adapter free function
static void MultiplexAdapter_Free(void* data_ptr) {
    free(data_ptr);
}

// Static adapter instance
static GekkoNetAdapter g_multiplex_adapter = {
    .send_data = MultiplexAdapter_Send,
    .receive_data = MultiplexAdapter_Receive,
    .free_data = MultiplexAdapter_Free
};

GekkoNetAdapter* CreateMultiplexAdapter() {
    return &g_multiplex_adapter;
}

void DestroyMultiplexAdapter() {
    // Clear any remaining queued packets
    g_gekko_packet_queue.clear();
    g_gekko_result_ptrs.clear();  // Note: results themselves are freed by GekkoNet
}
