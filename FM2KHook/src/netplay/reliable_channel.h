// reliable_channel.h -- reliable-ordered (+ future FEC) message layer over UDP.
//
// Sits on top of vendored reliable.io (per-packet seq/ack + fragmentation) and
// adds: logical channels, delivery classes, in-order delivery, and ack-driven
// retransmit. Purpose: give the spectator stream reliable, in-order delivery
// that does NOT head-of-line-block the way TCP does under loss (FEC layer, added
// next, reconstructs a lost packet without a retransmit round-trip).
//
// This module is transport-decoupled on purpose: egress goes through a
// caller-supplied RawSend (wire it to Sendto4or6 behind the 0xCB demux tag),
// and delivery goes through a caller-supplied Deliver callback (feed it into
// SpectatorNode_HandleSpecData). See docs/dev/reliable_channel_transport_design.md.
#pragma once

#include <cstdint>

namespace fm2k {
namespace rc {

// Delivery guarantee per message.
enum class Class : uint8_t {
    Unreliable        = 0,  // fire-and-forget
    ReliableUnordered = 1,  // exactly once, any order
    ReliableOrdered   = 2,  // exactly once, in per-channel order (spectator stream)
};

// Egress: send one already-built datagram PAYLOAD to the peer. The caller is
// responsible for prefixing the 0xCB wire tag and routing to Sendto4or6 (so the
// test-impairment queue + relay wrap still apply). `data`/`len` is one UDP body.
using RawSendFn = void (*)(void* ctx, const uint8_t* data, int len);

// Ingress delivery: one complete, in-order message for `channel`.
using DeliverFn = void (*)(void* ctx, uint8_t channel, const uint8_t* data, int len);

struct Endpoint;  // opaque

// Lifecycle. `id` distinguishes the two directions of a link (peer A/B use
// different ids). `now` is seconds (monotonic). Returns nullptr on failure.
Endpoint* Create(uint64_t id,
                 RawSendFn raw_send, void* raw_send_ctx,
                 DeliverFn deliver, void* deliver_ctx,
                 double now);
void Destroy(Endpoint* ep);

// Queue a message on `channel` with delivery class `cls`. Copied internally;
// caller keeps ownership of `data`.
void Send(Endpoint* ep, uint8_t channel, Class cls, const uint8_t* data, int len);

// Tuning (platform/config drives these; codec keeps sane defaults).
// FEC: enable/disable + group size K (data packets) + M (parity packets, recovers
// up to M losses per group instantly via Reed-Solomon). m defaults to keep prior.
void SetFec(Endpoint* ep, bool enabled, uint8_t k, uint8_t m = 0);
// Adaptive parity: when on (default), M is retuned to measured loss (2..6);
// when off, M is fixed at the SetFec value. An explicit FM2K_RC_FEC_M turns it off.
void SetFecAdaptive(Endpoint* ep, bool on);
// Retransmit timeout in seconds. Pass <= 0 for "auto" = ~1.5x measured RTT
// (floored), so it scales with the link instead of a fixed 100ms.
void SetResendSeconds(Endpoint* ep, double seconds);

// Send pacing: token-bucket rate (datagrams/sec), burst cap (immediate
// allowance), and congestion window (max reliable msgs awaiting ack before new
// sends stall). Bounds bulk bursts so they can't self-inflict loss + HOL stall.
void SetPacing(Endpoint* ep, double rate_pps, double burst_max, int cwnd);

// Feed a received 0xCB datagram PAYLOAD (after the caller strips the 0xCB tag).
void OnDatagram(Endpoint* ep, const uint8_t* data, int len);

// Pump: process acks, retransmit unacked reliable messages, drive delivery.
// Call every poll tick with the current monotonic time in seconds.
void Update(Endpoint* ep, double now);

// Metrics from the underlying reliable.io endpoint (for pacing / FEC rate).
float Rtt(Endpoint* ep);
float PacketLoss(Endpoint* ep);

}  // namespace rc
}  // namespace fm2k
