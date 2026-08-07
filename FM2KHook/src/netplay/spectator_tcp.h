// SpectatorTCP -- TCP transport for the spectator INPUT_BATCH stream.
//
// Why TCP for spectators: under packet loss / high RTT our hand-rolled UDP
// recovery (REDUNDANCY_WINDOW + INPUT_REQUEST + on-gap retransmit) was a
// fragile reimplementation of TCP semantics, and we kept finding bugs in
// it (RNG re-pin on reconnect, queue clear on JOIN_ACK, alignment off-by-
// one, etc). CCCaster's spectator path uses raw TCP for the bulk stream
// and trivially gets ordering + dedup + retransmit "for free" from the
// kernel. This module mirrors that approach: each spectator opens a TCP
// connection to the host, the host writes SpecDataHeader-framed messages,
// the spectator reads + dispatches into the unchanged
// SpectatorNode_HandleSpecData parser.
//
// What stays UDP:
//   - GekkoNet player ↔ player traffic (low-latency rollback, untouched).
//   - 0xCC control plane (HELLO, BATTLE_*, PING, CHAT). Tiny + rare.
//   - SPEC_JOIN_REQ / SPEC_JOIN_ACK / SPEC_HEARTBEAT / SPEC_LEAVE
//     handshake. Fires once per session; UDP control channel is fine.
//   - Spectator → sub-spectator daisy-chain relay (rare; v1 keeps UDP).
//
// What moves to TCP (this module):
//   - INPUT_BATCH     -- host → subscriber per-frame stream.
//   - INITIAL_MATCH   -- host → subscriber match metadata.
//   - MATCH_END       -- host → subscriber session terminator.
//
// Wire framing is unchanged: 10-byte SpecDataHeader followed by payload
// of size (header.frame_count * 4) for INPUT_BATCH, 96 bytes for
// INITIAL_MATCH, 0 bytes for MATCH_END. TCP is a stream, so receivers
// MUST maintain a per-connection partial-read buffer that tolerates
// being interrupted between bytes -- see SpectatorTCP_PollIncoming /
// PollUpstream impls.

#pragma once

#include <cstdint>
#include <cstddef>
#include <winsock2.h>

namespace SpectatorTCP {

// =============================================================================
// HOST-SIDE
// =============================================================================

// Bring up the TCP listener on `bind_port`. If port==0, the OS picks an
// ephemeral port; query the actual bound port via GetListenPort. Idempotent
// -- calling a second time is a no-op (returns true if listener is alive).
// Returns false on socket-create / bind failure (port collision, etc).
bool StartListener(uint16_t bind_port);

// The actual port the listener is bound to. 0 if not started. Callers
// (HandleJoinReq) include this in the SPEC_JOIN_ACK payload so spectators
// know where to dial.
uint16_t GetListenPort();

// Drain the accept queue. Each newly-accepted client lands in an
// unidentified-pending pool until its peer address is matched against the
// subscriber list (see RegisterAcceptedClient below). Cheap to call every
// frame; loops until accept returns "would block".
void PollAccepts();

// Drain inbound bytes from every connected spectator client. Today
// spectators don't send anything to host over TCP (INPUT_REQUEST is
// gone), so this just reads-to-discard plus detects orderly disconnects /
// errors and removes the client. Cheap; non-blocking.
void PollIncoming();

// Pair an accepted-but-unidentified client with a subscriber whose
// SPEC_JOIN_REQ arrived over UDP. Called from SpectatorNode_HandleJoinReq
// once the JOIN_ACK has been sent -- the spectator will TCP-connect right
// after, and the next PollAccepts sees that connect. Match by source IP
// (the same host:port the JOIN_REQ came from); the spectator-side TCP
// client will use the same source IP, but a different ephemeral source
// port. So we match IP only and accept the first pending connect from
// that IP.
//
// Returns true if a pending client was successfully bound to this
// subscriber's slot. False if no pending client matched (caller can retry
// next frame; PollAccepts continues to fill the pending pool).
bool RegisterAcceptedClient(const sockaddr_in& sub_addr);

// Broadcast a buffer to every connected subscriber. Replaces the
// for-each-subscriber UDP sendto loop in FlushBatch /
// SendInitialMatchTo / SendMatchEndToAll. Non-blocking; on pending-write
// the bytes are queued in the per-connection out buffer and drained
// later in PollIncoming.
//
// Skips subscribers whose backfill hasn't completed yet -- see
// MarkBackfillComplete. This avoids the race where a live FlushBatch
// reaches a freshly-bound spectator before its backfill bytes anchor
// next_expected_frame at session start.
void BroadcastToAll(const void* buf, size_t len);

// Mark a subscriber's initial backfill (INITIAL_MATCH +
// SendSessionBackfillTo chunks) complete. Called by
// SpectatorNode_TickHostMaintenance after the last backfill chunk's
// send returns. From this point on, BroadcastToAll will include this
// subscriber in live-batch broadcasts. Idempotent.
void MarkBackfillComplete(const sockaddr_in& sub_addr);

// Send to ONE subscriber. Used by SendSessionBackfillTo for late-joiner
// chunks. Address-matched against the subscriber list. No-op if the sub
// has no TCP socket yet (pending JOIN_REQ accept window).
void SendTo(const sockaddr_in& sub_addr, const void* buf, size_t len);

// Drop a subscriber's TCP connection. Called from
// SpectatorNode_HandleLeave + the silent-subscriber expiry sweep + the
// at-capacity-evict path. Idempotent; safe if the subscriber never had
// a TCP socket.
void DisconnectSubscriber(const sockaddr_in& sub_addr);

// Drop the bound conn AND all unpaired pending clients from this addr's
// IP (stale corpses from abandoned dials). Used on re-JOIN bind reset.
void DropConnectionsFromAddr(const sockaddr_in& sub_addr);

// True when this subscriber has a live, backfill-complete stream socket.
// A JOIN_REQ from such a subscriber is a duplicate/keepalive and must be
// ACK'd idempotently -- resetting bind state would drop the very conn the
// previous JOIN opened (the 500ms join storm that DoS'ed the host's main
// loop and starved its own netplay sends, 2026-06-11 14:33-14:42).
bool HasLiveConnFor(const sockaddr_in& sub_addr);

// =============================================================================
// SPECTATOR-SIDE
// =============================================================================

// Dial the host's TCP port (parsed from SPEC_JOIN_ACK's host_tcp_port
// field). Called from SpectatorNode_HandleJoinAck after the first-time
// gate. Returns true on connect-initiated (the SDL_NET handle is then
// pending until WaitUntilConnected resolves; PollUpstream does that
// implicitly). Returns false on resolve failure / already connected.
bool ConnectUpstream(const char* host_ip, uint16_t host_tcp_port);

// True while an upstream dial/connection socket exists (pending or
// established). Used to make session-kind-change JOIN_ACK re-broadcasts
// idempotent -- re-dialing over a healthy connection would churn it.
bool IsUpstreamConnected();

// Run an outbound TCP-STUN probe against the hub's TCP-STUN endpoint
// (FM2K_HUB_TCP_STUN_ADDR env). Source-binds the connect to the
// listener's port (g_listen_port) so the kernel-NAT registers a
// (listener_port -> hub:tcp_stun) outbound flow, the hub observes the
// external mapping, and replies with the (ip, port) it saw. We block
// up to ~500 ms for the round-trip -- runs once at hook init, before
// the spectator's first JOIN_REQ goes out, so the right external port
// is known by the time the launcher's `tcp_addr` message reaches the
// hub. Stashes results in g_external_tcp_{ip_be,port}; query via
// HasExternalTcpAddr / GetExternalTcp{IpBe,Port}. Returns true on
// successful round-trip, false on any failure (no env, no listener,
// connect/recv timeout). Failures are logged but non-fatal; the spec
// just reports its local listener port instead, which works on
// port-preserving NATs.
bool PerformTcpStun();
bool     HasExternalTcpAddr();
uint32_t GetExternalTcpIpBe();
uint16_t GetExternalTcpPort();

// Drain inbound bytes on the upstream socket. Each fully-buffered
// SpecDataHeader+payload gets dispatched into
// SpectatorNode_HandleSpecData. Maintains a partial-read buffer across
// calls because TCP is a stream. Cheap; non-blocking.
void PollUpstream();

// Tear down the upstream TCP socket. Called from
// SpectatorNode_LeaveUpstream + Shutdown.
void DisconnectUpstream();

// Returns the wall-clock timestamp (ms) of the most recent inbound TCP
// activity from upstream. SpectatorNode_TickHealth uses this for the
// silence-failover trigger (replaces the UDP-keyed
// last_upstream_recv_ms).
uint64_t LastUpstreamRecvMs();

// =============================================================================
// LIFECYCLE
// =============================================================================

// Tear down ALL TCP sockets (host listener + every subscriber slot +
// upstream client). Called from SpectatorNode_Shutdown. Safe to call
// when nothing is open.
void Shutdown();

}  // namespace SpectatorTCP
