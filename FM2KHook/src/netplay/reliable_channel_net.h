// reliable_channel_net.h — control-channel integration for the ReliableChannel
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
    RC_CHAN_SPEC_BLOB      = 3,  // host->spectator SNAPSHOT_BEGIN/CHUNK/END — UNORDERED, offset-reassembled
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

// Pump all endpoints (acks + retransmit + ack-carrier). From PollImplLocked.
void ReliableChannel_NetUpdate();
