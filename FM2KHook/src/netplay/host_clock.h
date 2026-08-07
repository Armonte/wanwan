// host_clock.h -- host-clock sync + "rift" frame pacing.
//
// One peer (the host) is the authoritative clock; the other estimates a µs offset
// (single-sample latch, 150ms hysteresis). Both stamp packets in the shared host-clock
// domain so the receiver measures one-way latency directly, then computes a per-remote
// "rift":
//   rift = mostRecentConfirmedRemoteFrame + oneWayLatencyFrames + 1 - localSimFrame
// (>0 => local behind/speed up; <0 => local ahead/slow down). The smoothed rift, blended
// with GekkoNet's own frame-advantage, feeds the existing frame-pacing sleep -- pacing more
// smoothly than arrival-jitter frame-advantage under high/asymmetric latency.
//
// Modeled on a shipping rollback engine's pacing. Generic naming; no product references.
// Gated by FM2K_HOST_CLOCK. FM2K is 100fps => 1 frame = 10000 µs.
#pragma once
#include <cstdint>

namespace fm2k { namespace hostclock {

// Local monotonic microseconds (QPC-derived).
uint64_t LocalMicros();

// True when FM2K_HOST_CLOCK enables rift pacing (else GekkoNet frame-advantage only).
bool Enabled();

// Role: the host (player 0) is the shared clock reference; the peer syncs to it.
bool IsHost();

// The host-clock "now" as this node sees it: host = LocalMicros(); peer =
// LocalMicros() + estimated offset. Meaningful on the peer once HasAccurateOffset().
uint64_t HostClockMicros();
bool     HasAccurateOffset();

// Stamp an outbound sync-bearing packet with this node's host-clock send time.
uint64_t StampOutbound();

// This node's current battle sim frame, kept current each frame by the pacing hook
// and stamped onto outbound PING/PONG so the peer can project our frame.
void     SetLocalFrame(uint32_t f);
uint32_t LocalFrame();

// Feed an inbound packet's host-clock send stamp + the sender's battle frame at that
// stamp + the local recv time. Updates the offset estimate [peer], the one-way-latency
// measurement [both], and the remote-frame projection sample [both]. peer_frame==0 =>
// sender not in battle / no frame info (projection sample skipped).
void OnInboundTimestamp(uint64_t peer_host_clock_send_us, uint32_t peer_frame,
                        uint64_t local_recv_us);

// One-way latency to the remote in frames (100fps), for the rift's staleness term.
// HONEST BASELINE = RTT/2: absolute per-direction latency is unknowable with two
// clocks under asymmetric routing (fundamental limit shared by NTP and the reference
// engine; the host-clock offset is biased by ~one transit and yields only EXCESS-over-
// minimum, not absolute). The host clock's real value is offset sync + a jitter-free
// projection, NOT beating RTT/2 -- so we do not let its biased excess inflate the rift.
float OneWayLatencyFrames(uint32_t rtt_ms);

// Diagnostic: the host-clock-MEASURED one-way in frames (excess over the windowed-min
// baseline), or -1 if not available. For logging / comparing against RTT/2 under
// emulated asymmetry; not fed into pacing.
float HostClockOneWayFramesDiag();

// Instantaneous rift (see header comment). Positive => local behind.
float RiftFrames(uint32_t remote_confirmed_frame, uint32_t local_sim_frame,
                 uint32_t rtt_ms);

// The value to feed the existing HandleFrameTime/SleepToTarget as `frames_ahead`:
// the jitter-free projected frame-advantage (localSim - projected remote frame) when a
// fresh remote-frame sample exists, else GekkoNet's arrival-based frame-advantage;
// lightly smoothed, clamped, sign so positive => local ahead => slow down. Also keeps
// the outbound-stamped local frame current. Call once per tick (self-gates per frame).
float PacingFramesAhead(uint32_t local_sim_frame, uint32_t rtt_ms,
                        float gekko_frames_ahead);

// Clear all sync/smoothing state (call on session start/teardown).
void Reset();

}}  // namespace fm2k::hostclock
