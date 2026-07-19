#pragma once
// control_channel.cpp shared state, externed so the split control_channel_*.cpp
// TUs (io / send / gekko-adapter) can share it. Pure linkage move -- definitions
// live in control_channel.cpp (the core). ENGINE-AGNOSTIC netcode: FM2K AND FM95
// reuse this transport unchanged (no engine addresses anywhere in here).
#include "control_channel.h"
#include "gekkonet.h"
#include <atomic>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <vector>
#include <utility>
#include <winsock2.h>
#include <ws2tcpip.h>   // sockaddr_in6 / dual-stack

// ---- socket + sequence + connection state (defined in control_channel.cpp) --
extern SOCKET g_socket;
// Bind address. sockaddr_in6 because the shared socket is opened AF_INET6
// dual-stack when possible; in the v4 fallback only sin6_port is meaningful
// (read by NetSocket_GetLocalPort). The v4/v6 family flag lives in addr6_util.
extern sockaddr_in6 g_local_sockaddr;
// Peer address as sockaddr_storage so it can hold an IPv4 (un-mapped at recv)
// OR a native IPv6 peer. The v4 family bytes/format are byte-identical to the
// old sockaddr_in path (addr6_util's Addr_ActorString preserves the GekkoNet
// actor-string match); v6 only ever appears when a v6 candidate latches.
extern sockaddr_storage g_remote_sockaddr;
// Gekko actor pin (carrier-NAT rebind survival, task #52). Set at every
// gekko session create to the EXACT address whose string was handed to
// gekko_add_actor. Inbound gekko packets from the same peer IP are stamped
// with THIS address (so gekko keeps accepting + RTT-matching after the
// peer's NAT remaps its port), and MultiplexAdapter_Send redirects sends
// addressed to it onto the CURRENT learned g_remote_sockaddr.
extern sockaddr_storage g_gekko_actor_addr;
extern bool             g_gekko_actor_pinned;
extern bool g_socket_initialized;

extern uint16_t g_send_seq;
extern uint16_t g_recv_seq;
extern uint16_t g_recv_ack;

extern bool g_connected;
extern uint32_t g_last_recv_time;
extern uint32_t g_connected_at_ms;

extern UINT g_keepalive_timer_id;
extern std::mutex g_poll_mutex;
extern uint32_t g_last_main_pump_ms;
extern uint32_t g_last_ping_time;
extern uint32_t g_rtt_ms;
extern uint32_t g_ping_send_time;

extern uint32_t g_rtt_worst_ms;
extern uint32_t g_rtt_sample_count;

extern std::atomic<int> g_delay_mode;               // 0=avg, 1=peak
extern std::atomic<int> g_remote_delay_candidate;

inline constexpr int kRttRingCap = 64;
extern uint32_t g_rtt_ring[kRttRingCap];
extern int      g_rtt_ring_count;
extern int      g_rtt_ring_head;

extern uint8_t g_local_player_id;
extern ControlMsgCallback g_msg_callback;

inline constexpr size_t RECV_BUFFER_SIZE = 2048;
extern char g_recv_buffer[RECV_BUFFER_SIZE];

extern std::vector<std::pair<std::vector<char>, sockaddr_storage>> g_gekko_packet_queue;
extern std::vector<GekkoNetResult*> g_gekko_result_ptrs;

// task #56 forensics: stamped by MultiplexAdapter_Receive whenever gekko-
// classified datagrams are handed to the session. Read by the [BEAT]/[WEDGE]
// heartbeat to tell a receive-starved wedge from an admit-starved one.
extern std::atomic<uint64_t> g_gekko_rx_last_ms;
extern std::atomic<uint64_t> g_gekko_rx_total;

// ---- current time in milliseconds (steady clock) ----
inline uint32_t GetTimeMs() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return static_cast<uint32_t>(ms.count());
}

// ---- raw socket I/O (control_channel_io.cpp). Un-static'd from the original so
// the core poll path + the gekko adapter can both drive them. CONTRACT: callers
// must hold g_poll_mutex (RawReceive mutates g_gekko_packet_queue/g_recv_buffer). --
void RawSend(const void* data, size_t len);
void RawReceive();
