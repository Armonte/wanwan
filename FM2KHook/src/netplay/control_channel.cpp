// Control Channel - Multiplexed UDP Socket Implementation
#include "control_channel.h"
#include "delay_math.h"
#include "globals.h"
#include "gekkonet.h"
#include "spectator_node.h"
#include "spectator_tcp.h"
#include "nat_traversal.h"
#include "addr6_util.h"        // Sendto4or6 (dual-stack v4-mapped send)
#include "netplay.h"           // Netplay_IsActive -- keepalive during live gekko session
#include "../ui/shared_mem.h"  // SharedMem_PublishMatchOutcome on ControlChannel timeout
#include <SDL3/SDL_log.h>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include "control_channel_internal.h"  // shared state + GetTimeMs + RawSend/RawReceive
// =============================================================================
// INTERNAL STATE
// =============================================================================
SOCKET g_socket = INVALID_SOCKET;
sockaddr_in6 g_local_sockaddr = {};  // AF_INET6 dual-stack bind (or v4-fallback port)
sockaddr_storage g_remote_sockaddr = {};  // peer: v4 (un-mapped) OR native v6
sockaddr_storage g_gekko_actor_addr = {}; // addr registered with gekko_add_actor
bool             g_gekko_actor_pinned = false;
bool g_socket_initialized = false;

// Sequence numbers for reliable messaging
uint16_t g_send_seq = 0;
uint16_t g_recv_seq = 0;
uint16_t g_recv_ack = 0;

// Connection state
bool g_connected = false;
uint32_t g_last_recv_time = 0;
// When did g_connected go true? Used by ControlChannel_Poll's timeout
// check to apply a startup grace window — the first ~15s after
// connect we use a much wider timeout so cnc-ddraw / other slow boot
// paths don't false-DC on the second peer's late init. Reset to 0
// when g_connected drops back to false.
uint32_t g_connected_at_ms = 0;

// Keepalive heartbeat (#fixes-modal-drag-disconnect 2026-05-05).
// Multimedia timer that fires on a worker thread, independent of the
// main loop's Windows message pump. When the user drags the title bar
// or opens the System menu, DefWindowProc enters a modal loop that
// blocks DispatchMessage; without this thread the main loop stops
// pumping pings and the peer's ControlChannel_Poll declares us
// disconnected after PING_TIMEOUT_MS. The MM timer keeps sending
// pings during modal mode so peers stay connected through window
// drags / brief stalls. Receive + state mutation stays on the main
// thread; only the outbound ping is fired from here.
UINT g_keepalive_timer_id = 0;

// Serialises ControlChannel_Poll between the main thread and the
// MM-timer worker. The MM-timer drives Poll itself when the main
// thread is blocked (modal window drag / Alt-Tab inactivity / boot
// stalls), so the handshake + receive path keeps making progress.
// The lock is fine-grained — RawReceive + dispatch is non-blocking
// and finishes in microseconds — so contention between threads is a
// non-issue in practice.
std::mutex g_poll_mutex;
// Tracks the last time the MAIN thread (not the MM timer) ran
// ControlChannel_Poll. Updated at the top of ControlChannel_Poll. The
// timeout-detection logic checks this value AGAINST g_last_recv_time
// to avoid declaring DC after the main thread itself has been frozen
// (modal drag pause); we extend the recv deadline by however long the
// main pump was asleep.
uint32_t g_last_main_pump_ms = 0;
uint32_t g_last_ping_time = 0;
uint32_t g_rtt_ms = 0;
uint32_t g_ping_send_time = 0;

// Worst-RTT accumulator (CCCaster-style). Tracks the maximum RTT observed
// over a rolling window so Netplay_StartBattleSession can pick a delay
// that covers the spike, not just the mean. Reset by ResetWorstRttMs()
// at the start of each session-config measurement window.
uint32_t g_rtt_worst_ms = 0;
uint32_t g_rtt_sample_count = 0;

// Input-delay negotiation (#24). g_delay_mode picks the formula (avg vs
// peak RTT); g_remote_delay_candidate holds the last value the peer
// advertised via DELAY_PROPOSAL (-1 = none received). Both are touched
// from the MM-timer thread (send) and the main thread (receive / battle
// start), so they're atomic.
std::atomic<int> g_delay_mode{0};               // 0=avg, 1=peak
std::atomic<int> g_remote_delay_candidate{-1};

// Recent RTT samples for delay sizing (#24). A single cold-start ping
// (peer still injecting / loading) or a scheduler hiccup easily round-
// trips in 200-300 ms even on a 10 ms link -- and an all-time-max
// worst-RTT latches that forever, which is exactly why worst-based
// delay was a bad idea. avg mode takes the mean of this window; peak
// mode takes the p90, which sizes above the median but discards the
// worst ~10% of outliers. Lock-free: written on the receive path, read
// from the timer thread / battle start -- a torn read just nudges one
// delay estimate by a sample, self-corrects next tick.
uint32_t g_rtt_ring[kRttRingCap] = {};
int      g_rtt_ring_count = 0;   // valid entries (caps at cap)
int      g_rtt_ring_head  = 0;   // next write index

// Player ID (set from globals)
uint8_t g_local_player_id = 0;

// Callback for received control messages
ControlMsgCallback g_msg_callback = nullptr;

// Buffer for receiving packets
char g_recv_buffer[RECV_BUFFER_SIZE];

// Queue for GekkoNet packets (filtered from receive)
std::vector<std::pair<std::vector<char>, sockaddr_storage>> g_gekko_packet_queue;

// GekkoNet result pointers for adapter (actual results are heap-allocated)
std::vector<GekkoNetResult*> g_gekko_result_ptrs;

// =============================================================================
// SOCKET MANAGEMENT
// =============================================================================

// MM-timer callback: fires on a worker thread every PING_INTERVAL_MS
// regardless of the message pump state. Two responsibilities:
//   1. Send a ping if we're connected and the main thread hasn't
//      pinged recently — keeps us alive across modal drags.
//   2. Drive ControlChannel_Poll itself when the main thread is
//      stuck (modal drag during HANDSHAKE, alt-tab during boot, slow
//      ddraw init). Without this, a window drag during the
//      "connecting…" phase leaves the handshake permanently stalled
//      because the main thread is blocked inside DefWindowProc and
//      Poll never runs. try_lock so the timer thread never blocks if
//      the main thread is mid-Poll already; concurrency is serialised
//      by g_poll_mutex.
//
// Forward decl — body is lower in this TU.
static void PollImplLocked();
static void CALLBACK KeepaliveTimerProc(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR) {
    if (!g_socket_initialized) return;
    // Flush the test-only network-impairment delay queue every timer tick,
    // independent of send activity -- so an RTT-induced CSS/battle-entry stall
    // can't deadlock waiting on a queued-but-never-sent packet. No-op in prod.
    fm2k::NetImpair_Pump();
    // Outbound ping (post-handshake only). ALSO ping while a gekko session
    // is still alive even if the control layer timed itself out: after a
    // carrier-NAT rebind, the peer's adoption of our new mapping is driven
    // by OUR control packets -- if a timed-out side stops pinging, the
    // other side never sees a peer-type packet from the new port and both
    // stay frozen at the prediction wall forever (task #52/#53 soak,
    // ~2-in-8 rebind runs lost the 1.5s adoption race).
    if (g_connected || Netplay_IsActive()) {
        const uint32_t now_send = (uint32_t)timeGetTime();
        if (now_send - g_last_ping_time >= PING_INTERVAL_MS / 2) {
            ControlChannel_SendPing();
            // Piggyback the delay-candidate broadcast on the ping
            // cadence so both peers have each other's value well
            // before the CSS->battle transition (#24).
            ControlChannel_SendDelayProposal();
        }
    }
    // Inbound poll — runs whether connected or not so the handshake
    // makes forward progress even when the main thread is parked.
    std::unique_lock<std::mutex> lock(g_poll_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
        PollImplLocked();
    }
}

static void KeepaliveTimer_Start() {
    if (g_keepalive_timer_id != 0) return;
    // TIME_PERIODIC + TIME_KILL_SYNCHRONOUS: callbacks fire every
    // PING_INTERVAL_MS, and timeKillEvent() blocks until any in-flight
    // callback returns (so shutdown doesn't race with sendto).
    g_keepalive_timer_id = timeSetEvent(
        PING_INTERVAL_MS, PING_INTERVAL_MS / 2,
        KeepaliveTimerProc, 0,
        TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
    if (g_keepalive_timer_id == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ControlChannel: timeSetEvent failed; modal-drag will still DC");
    }
}

static void KeepaliveTimer_Stop() {
    if (g_keepalive_timer_id != 0) {
        timeKillEvent(g_keepalive_timer_id);
        g_keepalive_timer_id = 0;
    }
}

bool NetSocket_Init(uint16_t local_port, const char* remote_addr) {
    if (g_socket_initialized) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: Already initialized");
        return true;
    }

    // Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: WSAStartup failed: %d", WSAGetLastError());
        return false;
    }

    // Create the shared UDP socket. Prefer AF_INET6 dual-stack (IPV6_V6ONLY=0)
    // so one handle carries IPv4 (via v4-mapped) and, in a later phase, native
    // IPv6 peers. If IPv6 is unavailable -- disabled by policy/registry, common
    // on locked-down ISP boxes which are exactly the CGNAT population we want to
    // help -- SOFT-FALL-BACK to a legacy AF_INET socket so pure-v4 netplay still
    // works. The winning family is handed to addr6_util (Addr_SetSocketV6),
    // which owns all v4/v6 send/recv normalization from here on.
    bool socket_is_v6 = false;
    g_socket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket != INVALID_SOCKET) {
        DWORD v6only = 0;
        setsockopt(g_socket, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&v6only), sizeof(v6only));
        // Verify it actually took -- a stack that silently keeps V6ONLY=1 would
        // drop every v4-mapped datagram, so trust the readback, not the call.
        DWORD readback = 1; int rb_len = sizeof(readback);
        if (getsockopt(g_socket, IPPROTO_IPV6, IPV6_V6ONLY,
                       reinterpret_cast<char*>(&readback), &rb_len) == 0
            && readback == 0) {
            socket_is_v6 = true;
        } else {
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
        }
    }
    if (!socket_is_v6) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "NetSocket: AF_INET6 dual-stack unavailable (err=%d) -- "
            "falling back to legacy IPv4 socket", WSAGetLastError());
        g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
    if (g_socket == INVALID_SOCKET) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: socket() failed: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }
    fm2k::Addr_SetSocketV6(socket_is_v6);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "NetSocket: socket family = %s",
        socket_is_v6 ? "AF_INET6 dual-stack" : "AF_INET (v4 fallback)");

    // Set non-blocking
    u_long mode = 1;
    if (ioctlsocket(g_socket, FIONBIO, &mode) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: ioctlsocket() failed: %d", WSAGetLastError());
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    // SO_REUSEADDR — allows rebinding to a port still in TIME_WAIT
    // from a previous run. Without this, restarting the launcher
    // within ~30s of a previous session hits WSAEADDRINUSE (10048).
    BOOL reuse = TRUE;
    setsockopt(g_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // Bind address. g_local_sockaddr is AF_INET6 -- a dual-stack socket bound to
    // the zero (in6addr_any) address accepts both v4 (as v4-mapped) and v6. Only
    // sin6_port is read downstream (NetSocket_GetLocalPort); it is set in BOTH
    // the v6 and v4-fallback paths. The fallback v4 socket binds a plain
    // sockaddr_in instead (an AF_INET socket rejects a 28-byte sockaddr_in6).
    g_local_sockaddr = {};
    g_local_sockaddr.sin6_family = AF_INET6;
    // sin6_addr left zero == in6addr_any (avoids the in6addr_any extern symbol)
    g_local_sockaddr.sin6_port = htons(local_port);
    sockaddr_in v4_bind = {};
    v4_bind.sin_family = AF_INET;
    v4_bind.sin_addr.s_addr = INADDR_ANY;
    v4_bind.sin_port = htons(local_port);
    const sockaddr* bind_sa = socket_is_v6
        ? reinterpret_cast<const sockaddr*>(&g_local_sockaddr)
        : reinterpret_cast<const sockaddr*>(&v4_bind);
    const int bind_len = socket_is_v6
        ? static_cast<int>(sizeof(g_local_sockaddr))
        : static_cast<int>(sizeof(v4_bind));

    // Bind with retry. The port can be TRANSIENTLY held at game boot:
    // the launcher's pre-match STUN probe (LauncherStunProbe) binds the
    // same local port and releases it "shortly before" we bind -- a
    // designed-in handoff that a one-shot bind turns into a coin flip
    // (observed as intermittent err=10013 "stuck at connecting", both in
    // hub matches and the loopback harnesses). Retry for up to 10s
    // before declaring the port genuinely taken.
    {
        int bind_attempts = 0;
        for (;;) {
            if (bind(g_socket, bind_sa, bind_len) != SOCKET_ERROR) {
                if (bind_attempts > 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "NetSocket: bound port %d after %d retries (%.1fs)",
                        (int)local_port, bind_attempts, bind_attempts * 0.25);
                }
                break;
            }
            const int err = WSAGetLastError();
            ++bind_attempts;
            if ((err == WSAEACCES || err == WSAEADDRINUSE) && bind_attempts < 40) {
                if (bind_attempts == 1) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "NetSocket: port %d busy (err=%d) -- retrying for up "
                        "to 10s (launcher STUN handoff / stale socket)",
                        (int)local_port, err);
                }
                Sleep(250);
                continue;
            }
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "NetSocket: bind() failed on port %d (err=%d, %d attempts). %s",
                (int)local_port, err, bind_attempts,
                err == WSAEADDRINUSE || err == WSAEACCES
                    ? "Port already in use — another launcher instance is on this port. "
                      "If running two launchers on the same machine, configure each "
                      "with a different FM2K_LOCAL_PORT (e.g. 7000 and 7001)."
                    : "");
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            WSACleanup();
            return false;
        }
    }

    // Parse remote address (format: "ip:port"). Empty/missing is allowed for
    // pure hosts: we'll learn the peer from the first inbound HELLO.
    // The env-supplied remote (FM2K_REMOTE_ADDR) is always v4 (the peer's
    // reflexive/LAN). A v6 peer is only ever LEARNED later (v6 punch -> peer
    // learning), so init parses v4 into the storage via a sockaddr_in view.
    std::memset(&g_remote_sockaddr, 0, sizeof(g_remote_sockaddr));
    sockaddr_in* r4 = reinterpret_cast<sockaddr_in*>(&g_remote_sockaddr);
    r4->sin_family = AF_INET;
    bool have_remote = false;
    if (remote_addr && remote_addr[0]) {
        std::string addr_str(remote_addr);
        size_t colon_pos = addr_str.find(':');
        if (colon_pos != std::string::npos) {
            std::string ip = addr_str.substr(0, colon_pos);
            uint16_t port = static_cast<uint16_t>(std::stoi(addr_str.substr(colon_pos + 1)));
            if (inet_pton(AF_INET, ip.c_str(), &r4->sin_addr) == 1 && port != 0) {
                r4->sin_port = htons(port);
                have_remote = true;
            }
        }
        if (!have_remote) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "NetSocket: Ignoring invalid remote '%s', waiting for peer HELLO",
                        remote_addr);
            r4->sin_port = 0;
            r4->sin_addr.s_addr = 0;
        }
    }

    g_socket_initialized = true;
    // ReliableChannel endpoint (0xCB) up once the socket is live.
    extern void ReliableChannel_NetInit();
    ReliableChannel_NetInit();
    g_local_player_id = static_cast<uint8_t>(g_player_index);
    g_last_recv_time = GetTimeMs();
    // 0 sentinel — the first ControlChannel_Poll call sees this and
    // skips its pump-stall ride-through. Otherwise the natural gap
    // between NetSocket_Init and the first main-loop tick (sometimes
    // hundreds of ms) gets mis-classified as a stall and resets the
    // recv deadline before HELLO/HELLO_ACK has even completed.
    g_last_main_pump_ms = 0;
    KeepaliveTimer_Start();

    if (have_remote) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: Initialized on port %d, remote=%s",
                    local_port, remote_addr);
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "NetSocket: Initialized on port %d, awaiting peer HELLO", local_port);
    }

    return true;
}

void NetSocket_Shutdown() {
    if (!g_socket_initialized) return;

    KeepaliveTimer_Stop();

    extern void ReliableChannel_NetShutdown();
    ReliableChannel_NetShutdown();

    if (g_socket != INVALID_SOCKET) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }

    WSACleanup();
    g_socket_initialized = false;
    g_connected = false;

    // Clear queues
    g_gekko_packet_queue.clear();
    g_gekko_result_ptrs.clear();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "NetSocket: Shutdown");
}

bool NetSocket_IsInitialized() {
    return g_socket_initialized;
}

SOCKET NetSocket_GetHandle() {
    return g_socket;
}

uint16_t NetSocket_GetLocalPort() {
    if (!g_socket_initialized) return 0;
    return ntohs(g_local_sockaddr.sin6_port);
}

const sockaddr_in* NetSocket_GetRemoteAddr() {
    // v4 view of the peer storage (valid for v4 + the v4-only spectator path).
    return reinterpret_cast<const sockaddr_in*>(&g_remote_sockaddr);
}

// The canonical GekkoNet actor string for the current peer (family-aware: v4
// byte-identical to the legacy form, v6 "[..]:port"). Both the actor register
// (netplay_battle/css) and the relay diag format through this so they match the
// recv stamp exactly -- the load-bearing actor-string invariant.
std::string NetSocket_GetRemoteActorString() {
    return fm2k::Addr_ActorString(g_remote_sockaddr);
}

void NetSocket_PinGekkoActorAddr() {
    g_gekko_actor_addr   = g_remote_sockaddr;
    g_gekko_actor_pinned = true;
}

void NetSocket_ClearGekkoActorPin() {
    g_gekko_actor_addr   = {};
    g_gekko_actor_pinned = false;
}

// =============================================================================
// CONTROL CHANNEL
// =============================================================================

SOCKET ControlChannel_GetSocket() {
    return g_socket;
}

void ControlChannel_LatchPeerAddr(const sockaddr_storage& peer) {
    std::string peer_s = fm2k::Addr_ActorString(peer);
    if (!fm2k::Addr_Equal(g_remote_sockaddr, peer)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "ControlChannel: peer addr latched -> %s", peer_s.c_str());
        // Winning-candidate telemetry: which path authenticated first.
        const char* kind = ::fm2k::nat::IsRelayMode() ? "relay"
                         : (peer.ss_family == AF_INET6 ? "v6" : "v4");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[NAT] winning candidate = %s -> %s", kind, peer_s.c_str());
        // [RELAY-RTT-DIAG] A re-latch AFTER g_connected desyncs g_remote_sockaddr
        // from the address ALREADY registered with gekko_add_actor. gekko's RTT
        // only accrues when addr.Equals(actor); once the stamp diverges, last_ping
        // pins at 0 -> that peer runs ahead -> one-sided rollback. (2026-06-13)
        if (g_connected) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[RELAY-RTT-DIAG] post-connect peer re-latch -> %s DIVERGES "
                "gekko actor addr; expect ping=0 + one-sided rollback",
                peer_s.c_str());
        }
    }
    g_remote_sockaddr = peer;
}

// Body of ControlChannel_Poll. Called either by the main thread (via
// ControlChannel_Poll, which acquires the mutex) or by the MM-timer
// worker (via KeepaliveTimerProc, also under the mutex). Touching this
// directly without holding g_poll_mutex is a bug.
static void PollImplLocked() {
    if (!g_socket_initialized) return;

    // Receive all pending packets
    RawReceive();

    // ReliableChannel: process acks + retransmit unacked reliable messages.
    // Runs under g_poll_mutex (this fn's contract); inbound 0xCB datagrams were
    // already fed to the RC endpoint inside RawReceive above.
    extern void ReliableChannel_NetUpdate();
    ReliableChannel_NetUpdate();

    // TCP path for the spectator INPUT_BATCH stream. Host-side: drain
    // the listener accept queue + read-discard from connected subs.
    // Spectator-side: drive the upstream connect to completion + drain
    // inbound bytes through the SpecDataHeader parser. Each call is
    // non-blocking and cheap when nothing is pending; safe to run on
    // both host and spectator processes (no-op when not applicable).
    SpectatorTCP::PollAccepts();
    SpectatorTCP::PollIncoming();
    SpectatorTCP::PollUpstream();

    // Pair freshly-accepted TCP clients to known subscriber slots and ship
    // deferred INITIAL_MATCH/backfill. Runs on host (which doesn't go
    // through SpectatorNode_TickHealth's spectator path); idempotent and
    // cheap when there are no subscribers.
    SpectatorNode_TickHostMaintenance();

    // Check for timeout. Pump-stall ride-through: if the main thread
    // paused longer than half the ping timeout (e.g. Win32 modal
    // title-drag, scheduler hiccup, init->first-frame gap), the
    // peer's pings probably arrived but went unread. Reset
    // g_last_recv_time to `now` so the timeout countdown effectively
    // restarts — the MM-timer kept OUR pings flowing, so this is
    // symmetric: neither side false-DCs the other across a stall.
    //
    // The first-call sentinel (g_last_main_pump_ms == 0) prevents a
    // false-stall on the very first Poll after init; otherwise the
    // gap from NetSocket_Init to the first main-loop iteration would
    // look like a 300-1000ms "stall" and reset the deadline before
    // the real timeout window has even started counting.
    uint32_t now = GetTimeMs();
    if (g_last_main_pump_ms != 0) {
        const uint32_t main_pause = now - g_last_main_pump_ms;
        if (main_pause > PING_TIMEOUT_MS / 2) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: main pump stalled %ums — resetting recv deadline",
                (unsigned)main_pause);
            g_last_recv_time = now;
        }
    }
    g_last_main_pump_ms = now;
    // Startup grace window: the first ~15s after handshake we use a 10s
    // timeout instead of 1.5s. cnc-ddraw / other proxy DLLs add measurable
    // boot latency to the OTHER peer; if their hook hasn't started its
    // ping cycle by the time our 1.5s window expires we'd false-DC them.
    // After 15s both peers are in steady state and the tight timeout
    // catches real disconnects fast.
    constexpr uint32_t kStartupGraceWindowMs = 15000;
    constexpr uint32_t kStartupTimeoutMs     = 10000;
    const uint32_t timeout_ms =
        (g_connected_at_ms != 0 &&
         (now - g_connected_at_ms) < kStartupGraceWindowMs)
            ? kStartupTimeoutMs
            : (uint32_t)PING_TIMEOUT_MS;
    if (g_connected && (now - g_last_recv_time) > timeout_ms) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Connection timed out");
        g_connected = false;
        g_connected_at_ms = 0;
        // Surface the disconnect to the launcher even when the failure
        // happens BEFORE we reach the BATTLE GekkoNet session — the user
        // bug report ("closed client during CSS, host stayed frozen with
        // music playing") was the CSS phase + GekkoPlayerDisconnected
        // never firing because we hadn't created the battle session yet.
        // PublishMatchOutcome with DISCONNECT bumps match_outcome_seq;
        // the launcher's PollMatchOutcome reads it on the next frame and
        // calls on_session_stop to terminate the surviving game process.
        // No-op when shared mem isn't initialised.
        SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_DISCONNECT);
    }

    // Send periodic ping
    if (g_connected && (now - g_last_ping_time) > PING_INTERVAL_MS) {
        ControlChannel_SendPing();
        g_last_ping_time = now;
    }
}

void ControlChannel_Poll() {
    // Public entry. Lock-then-delegate so the MM-timer's parallel poll
    // path (which uses try_lock to avoid blocking the timer worker)
    // can't race with us on RawReceive's dispatch into Netplay_*
    // handlers.
    std::lock_guard<std::mutex> lock(g_poll_mutex);
    PollImplLocked();
}

void ControlChannel_Send(const CtrlPacket& packet) {
    if (!g_socket_initialized) return;

    // Make a copy to modify header
    CtrlPacket pkt = packet;
    pkt.header.magic = CTRL_MAGIC;
    pkt.header.seq = ++g_send_seq;
    pkt.header.ack = g_recv_seq;
    pkt.header.player_id = g_local_player_id;

    RawSend(&pkt, sizeof(pkt));
}

void ControlChannel_SendTo(const CtrlPacket& packet, const sockaddr_in& dest) {
    if (!g_socket_initialized) return;
    if (g_socket == INVALID_SOCKET) return;
    if (dest.sin_port == 0) return;

    CtrlPacket pkt = packet;
    pkt.header.magic     = CTRL_MAGIC;
    pkt.header.seq       = ++g_send_seq;
    pkt.header.ack       = g_recv_seq;
    pkt.header.player_id = g_local_player_id;

    fm2k::Sendto4or6(g_socket, reinterpret_cast<const char*>(&pkt),
                     sizeof(pkt), dest);
}

void ControlChannel_SendRawTo(const void* buf, size_t len, const sockaddr_in& dest) {
    if (!g_socket_initialized) return;
    if (g_socket == INVALID_SOCKET) return;
    if (dest.sin_port == 0) return;
    fm2k::Sendto4or6(g_socket, reinterpret_cast<const char*>(buf),
                     (int)len, dest);
}

bool ControlChannel_IsConnected() {
    return g_connected;
}

uint32_t ControlChannel_GetLastRecvMs() {
    return GetTimeMs() - g_last_recv_time;
}

uint32_t ControlChannel_GetRttMs() {
    return g_rtt_ms;
}

uint32_t ControlChannel_GetWorstRttMs() {
    return g_rtt_worst_ms;
}

uint32_t ControlChannel_GetRttSampleCount() {
    return g_rtt_sample_count;
}

void ControlChannel_ResetWorstRttMs() {
    g_rtt_worst_ms     = g_rtt_ms;  // seed with the most recent sample so
                                    // we never report 0 if no PONGs land
                                    // in the next window
    g_rtt_sample_count = (g_rtt_ms > 0) ? 1 : 0;
}

void ControlChannel_SetCallback(ControlMsgCallback callback) {
    g_msg_callback = callback;
}

// --- Input-delay negotiation (#24) -------------------------------------

void ControlChannel_SetDelayMode(int mode) {
    g_delay_mode.store(mode == 1 ? 1 : 0, std::memory_order_relaxed);
}

int ControlChannel_GetDelayMode() {
    return g_delay_mode.load(std::memory_order_relaxed);
}

int ControlChannel_GetLocalDelayCandidate() {
    // A manual FM2K_LOCAL_DELAY override is the user's explicit choice. It is
    // still advertised as this peer's candidate so an AUTO opponent can factor
    // it into their max(); it does NOT force a manual opponent (who runs their
    // own value verbatim -- see Netplay_StartBattle).
    if (const char* env = std::getenv("FM2K_LOCAL_DELAY"); env && env[0]) {
        int v = std::atoi(env);
        // No small clamp -- the player owns their delay. Only ceiling is
        // GekkoNet's input ring (DEFAULT_BUFF_SIZE=128); keep clear of it.
        if (v >= 0 && v <= 120) return v;
    }
    // No override: size from the measured RTT window. avg mode uses the
    // mean (lower delay, spikes can rollback); peak mode uses the p90
    // (higher delay, rides out jitter). The window math is pure and
    // lives in delay_math.h -- see tests/delay_math_test.cpp.
    const uint32_t rtt = fm2k::delay::RttFromWindow(
        g_rtt_ring, g_rtt_ring_count,
        g_delay_mode.load(std::memory_order_relaxed));
    return fm2k::delay::DelayCandidateFromRtt(rtt);
}

int ControlChannel_GetRemoteDelayCandidate() {
    return g_remote_delay_candidate.load(std::memory_order_relaxed);
}

void ControlChannel_SendDelayProposal() {
    if (!g_connected) return;
    const int cand = ControlChannel_GetLocalDelayCandidate();
    if (cand < 0) return;  // nothing measured to propose yet
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::DELAY_PROPOSAL;
    pkt.data.delay_proposal.delay = (uint8_t)cand;
    pkt.data.delay_proposal.mode  =
        (uint8_t)g_delay_mode.load(std::memory_order_relaxed);
    ControlChannel_Send(pkt);
}

// Handle incoming PONG to calculate RTT
void ControlChannel_HandlePong(uint32_t sent_time) {
    g_rtt_ms = GetTimeMs() - sent_time;
    if (g_rtt_ms > g_rtt_worst_ms) g_rtt_worst_ms = g_rtt_ms;
    ++g_rtt_sample_count;
    // Feed the delay-sizing window (#24).
    g_rtt_ring[g_rtt_ring_head] = g_rtt_ms;
    g_rtt_ring_head = (g_rtt_ring_head + 1) % kRttRingCap;
    if (g_rtt_ring_count < kRttRingCap) ++g_rtt_ring_count;
}

// Mark connection as established (called when HELLO_ACK received)
void ControlChannel_SetConnected(bool connected) {
    g_connected = connected;
    if (connected) {
        g_last_recv_time = GetTimeMs();
    } else {
        // Drop the peer's delay candidate + our RTT window so a
        // reconnect renegotiates from scratch instead of reusing a
        // stale value or another peer's latency samples (#24).
        g_remote_delay_candidate.store(-1, std::memory_order_relaxed);
        g_rtt_ring_count = 0;
        g_rtt_ring_head  = 0;
    }
}
