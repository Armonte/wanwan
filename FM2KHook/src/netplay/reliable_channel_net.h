// reliable_channel_net.h -- control-channel integration for the ReliableChannel
// layer. Address-keyed endpoint registry: one reliable.io endpoint PER peer
// address, so it serves both the 1:1 player channel and the 1:N host->spectator
// stream over the single shared UDP socket. See reliable_channel_net.cpp.
#pragma once

#include <cstdint>

struct sockaddr_storage;

// Application-facing delivery: one complete, in-order (for ordered channels)
// message on `channel`, tagged with the peer it came from.
using ReliableChannel_DeliverFn = void (*)(void* ctx, const sockaddr_storage& from,
                                           uint8_t channel, const uint8_t* data, int len);

// Delivery-class ints mirror fm2k::rc::Class.
enum {
    RC_CLASS_UNRELIABLE         = 0,
    RC_CLASS_RELIABLE_UNORDERED = 1,
    RC_CLASS_RELIABLE_ORDERED   = 2,
};

// Reserved logical channels. Per-channel ORDERING IS INDEPENDENT (like the reference engine's
// per-channel reliableMessages map), so bulk on its own channel can't head-of-line
// -block the live stream even on one endpoint.
enum {
    RC_CHAN_SPEC           = 1,  // host->spectator LIVE EVENT_BATCH stream (latency-critical, ordered)
    RC_CHAN_SPEC_SNAPSHOT  = 2,  // host->spectator backfill EVENT_BATCH + OP_BASELINE (ordered bulk)
    RC_CHAN_SPEC_BLOB      = 3,  // host->spectator SNAPSHOT_BEGIN/CHUNK/END -- UNORDERED, offset-reassembled
                                 // (a lost chunk must not head-of-line-block the other 65 chunks)
};

void ReliableChannel_NetInit();
void ReliableChannel_NetShutdown();
void ReliableChannel_SetDeliver(ReliableChannel_DeliverFn fn, void* ctx);

// Send to a specific peer address (endpoint created on first use).
void ReliableChannel_SendTo(const sockaddr_storage& to, uint8_t channel, int cls,
                            const uint8_t* data, int len);

// Feed a received 0xCB datagram (body AFTER the tag) from `from`. From RawReceive.
void ReliableChannel_OnDatagram(const sockaddr_storage& from, const uint8_t* body, int body_len);

// Destroy this peer's endpoint so the next send/receive builds a fresh one:
// tx msg_seq back to 0, unacked map dropped, rx cursors and reassembly buffers
// gone. THE ONLY WAY to clear a wedged endpoint -- an app-level re-JOIN does
// not, because the registry is keyed by peer address and survives it, which is
// why the spectator starve of 2026-08-07 could not be healed by re-JOINing.
// No-op for an unknown address. Returns true if an endpoint was actually torn
// down.
//
// RESET THE RECEIVER, NOT THE SENDER. The failure this exists for is a pinned
// ORDERED RECEIVE cursor, so the side that must forget is the one that is
// stuck: a fresh receiver anchors on whatever msg_seq the (untouched) sender
// emits next -- DeliverOrdered's documented mid-join anchor -- and the stream
// is whole immediately. Resetting the SENDER instead restarts its msg_seq at 0
// under a receiver whose cursor is at N, and DeliverOrdered only reads that as
// a restart past RC_RESTART_BACKJUMP (256); below it the new stream's head is
// indistinguishable from a retransmit and is dropped as duplicate. Channels
// that carry only a few dozen messages per join (RC_CHAN_SPEC_SNAPSHOT) would
// therefore silently lose the head of the very re-ship the reset provoked. So:
// the VIEWER resets its endpoint, the host resets nothing.
//
// TWO ENTRIES, because the endpoint registry is shared with the MM-timer
// worker thread that drives ControlChannel_Poll when the main thread stalls:
//   ...Locked  -- caller ALREADY holds g_poll_mutex. That is the whole
//                 RawReceive dispatch chain, so any control-message handler
//                 would use this one; taking the mutex there self-deadlocks.
//   plain      -- acquires g_poll_mutex itself. For main-thread callers
//                 outside a poll, i.e. the viewer's TickHealth ladder.
// Tearing an entry out of the map from an unlocked main thread while the timer
// worker iterates it is a use-after-free, so this distinction is load-bearing.
bool ReliableChannel_ResetPeerLocked(const sockaddr_storage& addr);
bool ReliableChannel_ResetPeer(const sockaddr_storage& addr);

// True when at least one ORDERED rx channel from this peer is pinned at a hole
// AND the peer's latest ack carrier advertises that it is STILL HOLDING that
// hole -- i.e. the transport is provably mid-repair and the missing message is
// still being retransmitted.
//
// This exists because the app's starve ladder outranks the transport's own
// repair: escalation #1 calls ReliableChannel_ResetPeer, which DESTROYS the
// endpoint and with it every message the sender's liveness-conditional
// retirement was retaining. Without this predicate the retention is inert for
// spectators past 4000ms. Always false when FM2K_RC_LIVENESS is off, and false
// for an unknown address.
//
// Same two-entry locked/unlocked shape as ResetPeer, and for the same reason:
// the endpoint registry is shared with the MM-timer worker thread. The viewer
// calls the plain entry from TickHealth on the main thread, outside any poll.
bool ReliableChannel_IsRepairingOrderedLocked(const sockaddr_storage& addr);
bool ReliableChannel_IsRepairingOrdered(const sockaddr_storage& addr);

// Pump all endpoints (acks + retransmit + ack-carrier). From PollImplLocked.
void ReliableChannel_NetUpdate();
