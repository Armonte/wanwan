#pragma once
// reliable_channel_internal.h -- state model shared by the ReliableChannel TUs.
//
// The layer is split by concern (1000-line cap): reliable_channel.cpp owns
// framing, ordering, acks, pacing and retransmit; reliable_channel_fec.cpp owns
// the FEC codec (parity encode on egress, group reassembly + RS recovery on
// ingress) and the wire demux that feeds it. Both operate on the one Endpoint
// declared here.
//
// NOTE (trap this repo has hit before): none of these may also live in an
// anonymous namespace in either TU -- anon-namespace members stay visible at
// the enclosing scope, so a same-named local copy makes the name AMBIGUOUS.
#include "reliable_channel.h"

extern "C" {
#include "reliable.h"
}

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace fm2k {
namespace rc {

constexpr uint8_t  RC_WIRE_TAG   = 0xCB;   // first byte on the wire (RawReceive demux)
constexpr size_t   RC_CRC_BYTES  = 4;      // CRC32 trailer per framed message
constexpr double   RESEND_AFTER  = 0.10;   // legacy fixed timeout; default is now
                                           // AUTO (0 => 2x RTT). Kept for callers
                                           // that want a fixed value via SetResendSeconds.
// Cap on the per-channel out-of-order reassembly buffer. A reliable-ordered
// channel only buffers a few messages ahead while a gap fills; a persistent
// large gap means the stream is stuck (lost/un-retransmitted packet, or a
// malicious peer flooding high sequence numbers). Bound the memory: beyond
// this we stop buffering (the stall is then detected by the consumer's
// gap-fill / reconnect watchdog, which re-syncs the channel).
constexpr size_t   RC_OOO_MAX    = 1024;
constexpr size_t   RC_HDR_BYTES  = 1 + 1 + 8 + 2;  // chan + class + msg_seq + len
constexpr uint8_t  RC_ACK_CARRIER_CHANNEL = 0xFF;  // reserved carrier; never delivered to app
constexpr double   RC_ACK_FLUSH_INTERVAL  = 0.033; // ~3 frames @100fps

// ---- S0: ack-carrier KEEPALIVE ---------------------------------------------
// The carrier used to be emitted ONLY when something had been received since
// the last one (rx_since_carrier > 0). That makes a healthy receiver
// indistinguishable from a dead one during a one-way FORWARD outage: it
// receives nothing, so it says nothing, so the sender's only evidence about
// its peer is silence. Every liveness test below would then fail closed in
// exactly the failure they exist for.
//
// Second, independent bug it removes: while the receiver is silent the sender
// cannot clear its unacked map even for messages the receiver ALREADY HAS, so
// InFlight() stays pinned and cwnd stalls new sends on EVERY channel of that
// endpoint (InFlight is per-endpoint, not per-channel).
//
// 2 datagrams/s per endpoint next to the existing up-to-30/s receive-driven
// path, which is unchanged.
constexpr double   RC_ACK_KEEPALIVE_INTERVAL = 0.5;

// ---- S2: holding advertisement (trailing carrier section) ------------------
// The carrier body is [u8 count]{u8 chan, u64 next_expected} * count. The
// parser reads exactly `count` fixed-stride entries and then RETURNS, so
// anything appended after them is invisible to a pre-fix peer -- which is what
// makes this section additive on the wire in both directions. Appended form:
//
//   [u8 RC_CARRIER_EXT_MAGIC][u8 hcount]
//   { u8 chan, u8 holding, u64 oldest_unacked } * hcount
//
// `oldest_unacked` is the lowest msg_seq this sender still holds for that
// ORDERED channel (queued or unacked); when it holds nothing it is
// next_msg_seq, i.e. "everything below this is gone, stop waiting for it".
// The receiver uses it to un-anchor on a FACT instead of on a timer, and to
// KEEP waiting while the sender is provably still retransmitting the hole.
//
// THE `holding` FLAG IS NOT REDUNDANT, and leaving it out was a live defect
// caught in the first vanpri run. An IDLE, fully in-sync ordered channel has
// oldest_unacked == next_msg_seq == the receiver's next_deliver -- byte-for-
// byte identical to "I am holding exactly the message you are missing". The
// receiver would then DEFER forever on a channel with nothing outstanding, and
// (worse) report itself mid-repair to the app's starve ladder. Observed on
// RC_CHAN_SPEC_SNAPSHOT, which goes permanently quiet after a join backfill:
// 23 spurious deferrals in one spectator session. One byte settles it.
constexpr uint8_t  RC_CARRIER_EXT_MAGIC   = 0xA5;
constexpr uint8_t  RC_CARRIER_MAX_ENTRIES = 32;   // per section (both sections)
constexpr size_t   RC_CARRIER_HOLD_STRIDE = 10;   // chan + holding + u64

// reliable.io envelope. Mirrored into the reliable_config_t in Create(); hoisted
// here because the PACER must know them -- a packet at or below fragment_above
// is one datagram, anything larger becomes ceil(bytes / fragment_size) fragment
// datagrams (reliable.c:784-830), and the token bucket has to charge for what
// actually hits the wire.
constexpr size_t   RC_MAX_PACKET_BYTES = 64 * 1024;
constexpr size_t   RC_FRAGMENT_ABOVE   = 1024;
constexpr size_t   RC_FRAGMENT_SIZE    = 1024;

// Ordered-channel stall horizon: how long a reliable-ordered channel may
// deliver NOTHING before the receiver repairs itself. See the stall sweep in
// Update().
//
// WAS 8.0, AND THAT MADE THE REPAIR UNREACHABLE. The old value was chosen to
// sit ABOVE the 7s unacked retire below, on the reasoning that a message the
// sender has not yet retired might still arrive. True, but irrelevant: the
// only consumer of this layer is the spectator stream, and a viewer's
// host-gone watchdog (FM2K_SPEC_HOST_GONE_MS, trampoline_spectator.cpp) kills
// the PROCESS on the same clock. At 8.0 vs the old 8000ms budget the repair
// and the suicide landed on the same tick -- a coin flip -- and the lab's
// 5000ms override made it a guaranteed loss. `[RC] ordered chan=... SKIPPING`
// had never executed once, anywhere, at the time this was changed.
//
// 3.5s is now the FIRST rung of the spectator recovery ladder and every rung
// above it is spaced off this number:
//   3.5s  RC ordered-stall repair (here)
//   4.0s  gap-fill escalation -> resume-suppressed re-JOIN + RC endpoint reset
//   8.0s  second escalation
//  12.0s  host-gone give-up (FM2K_SPEC_HOST_GONE_MS default)
// Firing below the 7s retire is deliberate: the hole may still be in flight,
// but at a <=750ms retransmit cap it has already had 5-40 attempts.
//
// CORRECTION (was: "the repair NEVER discards a message that later arrives").
// That is true of the EMPTY-ooo branch, which only un-anchors (see
// stall_resync) so a late retransmit of the hole itself still delivers
// normally. It is FALSE of the ooo-non-empty branch: that one advances
// next_deliver PAST a hole the sender may still be retransmitting, after which
// the real message arrives and is dropped as a duplicate. So the timer-driven
// repair really can throw away a message that was in flight and about to
// arrive. S2's holding advertisement is what fixes it: while the sender says
// it is still holding our hole we do NOT repair at all, and when it says it
// has retired the hole we repair IMMEDIATELY rather than guessing at 3.5s.
constexpr double   RC_ORDERED_STALL_SEC = 3.5;

// ---- S1/S2: liveness-conditional give-up ------------------------------------
// The old contract was two independent, uncoordinated guesses: the SENDER gave
// up at a flat 7s from FIRST send with no reference to whether the peer was
// alive or still asking, and the RECEIVER gave up at 3.5s with no reference to
// whether the sender was still holding the hole. Neither guess can tell a bad
// link from a dead one, and because a live stream sends in dense runs the
// senders' `first_time` values are near-identical -- so a burst expires AS A
// BLOCK ("14 message(s) permanently lost" is one retire sweep, not 14 events).
//
// RC_RETIRE_SEC is unchanged and still the horizon. What changed is that
// reaching it is no longer sufficient: one of three additional conditions must
// hold (see the retire loop in Update()).
//
//   RC_PEER_SILENT_SEC  the dead-link case. Behaviour here is bit-identical to
//                       the old flat TTL, which is what keeps a genuinely dead
//                       link shedding its backlog instead of retransmitting
//                       forever. Do NOT replace this with a TTL measured from
//                       `sent_time`: that is refreshed on every retransmit, so
//                       such a TTL never expires while the resend loop runs.
//   RC_RETIRE_HARD_SEC  a peer that is alive and asking but can NEVER receive
//                       (path-MTU blackhole, a middlebox eating one flow).
//                       25s sits below the spectator subscriber expiry (30s,
//                       spectator_node_internal.h) and below RC_PEER_IDLE_SEC
//                       (30s, reliable_channel_net.cpp) so an endpoint never
//                       outlives its own app-level owner. It is load-bearing,
//                       not decorative: the 30s idle sweep does NOT bound this
//                       because GetOrCreate refreshes last_activity on SENDS
//                       as well as receives, so a host talking to a dead
//                       viewer is never evicted.
//   RC_MAX_UNACKED_BYTES belt and braces. Retention is ALREADY bounded by cwnd
//                       (~96 messages endpoint-wide, PumpSendQueue stops
//                       adding at inflight >= cwnd), so what the TTL actually
//                       bounds is the retransmit WIRE RATE on a dead link, not
//                       memory -- the old in-source claim that a longer TTL
//                       "would keep 1000+ messages resident" was wrong. 2MB is
//                       still cheap insurance against a future cwnd change.
//                       BE HONEST ABOUT ITS REACH (review D4b): the cap is
//                       PER-ENDPOINT, so the process bound is RC_MAX_PEERS(64)
//                       x 2MB = 128MB on top of the pre-existing 64 x
//                       RC_MAX_QUEUED_BYTES; and with cwnd at 96 it can only
//                       fire above ~21.8KB per message. Spectator live batches
//                       are ~180B (17KB resident) and snapshot chunks are
//                       <=16KB (1.5MB), so on the ONLY traffic RC carries today
//                       this arm is unreachable. It is not a discharged bound
//                       on production traffic; it is a guard on a future one,
//                       and unit test 20 has to use near-envelope messages to
//                       reach it at all.
constexpr double   RC_RETIRE_SEC         = 7.0;
constexpr double   RC_PEER_SILENT_SEC    = 2.0;
constexpr double   RC_RETIRE_HARD_SEC    = 25.0;
constexpr size_t   RC_MAX_UNACKED_BYTES  = 2u * 1024 * 1024;

// Receiver-side backstop for the S2 holding branch: however loudly the peer
// claims it is still holding our hole, repair anyway once the channel has
// delivered nothing for this long. Sits ABOVE the spectator ladder's second
// escalation (8000ms) and strictly BELOW its host-gone budget (12000ms), so a
// viewer that waits on a hold can never wait itself to death.
constexpr double   RC_ORDERED_STALL_HARD_SEC = 9.0;
// A holding advertisement older than this is not evidence of anything: the
// peer has stopped sending carriers, so its last claim is stale. Matches
// RC_PEER_SILENT_SEC on purpose -- same "is this peer alive" question, asked
// from the other end of the link.
constexpr double   RC_HOLD_ADVERT_FRESH_SEC  = 2.0;
// Slack before a FACT-driven repair (peer advertises it retired our hole) may
// fire. It exists so a repair is never taken on an advertisement that crossed
// the very delivery it describes, and so an endpoint restart has a moment to
// settle through the RC_RESTART_BACKJUMP path first. Still ~14x faster than
// the 3.5s timer it replaces.
constexpr double   RC_ORDERED_FACT_REPAIR_SEC = 0.25;

// A genuine duplicate is always a retransmit of something RECENT: cwnd caps
// in-flight at ~96 messages and the 7s TTL retires the rest. A msg_seq this far
// BELOW our rx cursor can therefore only be a peer whose endpoint was destroyed
// and recreated (eviction is per-side; see RC_PEER_IDLE_SEC in
// reliable_channel_net.cpp), restarting its msg_seq at 0.
constexpr uint64_t RC_RESTART_BACKJUMP = 256;

// FEC suppression thresholds -- see the block in Update() for why this is now
// byte-based AND sustained rather than "more than 20 queued messages".
constexpr size_t   RC_FEC_SUPPRESS_BYTES       = 256 * 1024;
constexpr double   RC_FEC_SUPPRESS_SUSTAIN_SEC = 1.0;
constexpr double   RC_FEC_SUPPRESS_SEC         = 1.0;

// Per-endpoint send-queue memory bound. A host serving N spectators queues a
// full snapshot per viewer, and a viewer whose acks are black-holed stalls its
// endpoint on cwnd while re-ships keep arriving -- unbounded before this.
constexpr size_t   RC_MAX_QUEUED_BYTES = 12u * 1024 * 1024;

// ---- FEC (XOR parity over groups of K packets) -----------------------------
// Each outgoing reliable.io datagram is wrapped with a 1-byte FEC role. Data
// datagrams stream immediately at real size; after every K data datagrams a
// single PARITY datagram is emitted = XOR of the group's "coded forms"
// ([u16 inner_len][inner], zero-padded to the group max). If exactly ONE of the
// K data datagrams is lost, the receiver reconstructs it from parity + the K-1
// survivors WITHOUT a retransmit round-trip -- this is what removes the per-loss
// head-of-line stall an ordered stream otherwise suffers. >1 loss in a group
// falls back to reliable.io retransmit (only that group stalls, not the tail).
// XOR/1-parity is the starting codec; Reed-Solomon (K+M) is the burst-loss
// upgrade and slots into the same encode/decode seam.
enum FecRole : uint8_t { FEC_DATA = 0, FEC_PARITY = 1, FEC_RAW = 2 };
constexpr uint8_t  FEC_GROUP_K   = 8;      // data packets per group
constexpr uint8_t  FEC_GROUP_M   = 4;      // parity packets per group (recovers up to M losses instantly).
                                           // M=4 (33% parity) keeps the live stream bit-exact + live to
                                           // ~15-20% loss as the SOLE input path; M=2 was marginal at 15%.
constexpr size_t   FEC_RX_GROUPS = 64;     // bounded reassembly window
// wire headers after the 0xCB tag:
//   data  : [role u8][group u16][index u8][k u8][m u8][inner_len u16][inner...]
//   parity: [role u8][group u16][j u8][k u8][m u8][pad_len u16][parity...]
//   raw   : [role u8][inner...]
constexpr size_t   FEC_DATA_HDR   = 1 + 2 + 1 + 1 + 1 + 2;
constexpr size_t   FEC_PARITY_HDR = 1 + 2 + 1 + 1 + 1 + 2;
constexpr size_t   FEC_RAW_HDR    = 1;

struct FecEncoder {
    uint16_t group = 0;
    uint8_t  index = 0;
    std::vector<std::vector<uint8_t>> coded;  // buffered coded forms for the current group
};

// "coded form" of a data packet = [u16 inner_len][inner], zero-padded to L (the
// group's max coded length). RS operates on these fixed-length blocks.
struct FecGroupRx {
    uint8_t  k = 0, m = 0;
    bool     params_known = false;
    std::map<uint8_t, std::vector<uint8_t>> data;    // index -> inner reliable.io packet
    std::map<uint8_t, std::vector<uint8_t>> parity;  // j -> parity block (length L)
    uint16_t L = 0;                                   // padded coded-form length
    bool     recovered = false;
};

struct FecDecoder {
    std::map<uint16_t, FecGroupRx> groups;
};

struct Unacked {
    std::vector<uint8_t> framed;  // full framed message (chan..payload)
    double   sent_time;
    double   first_time = -1.0;   // first send; used for the 7s stuck-connection retire
    uint16_t pkt_seq;             // reliable.io packet seq that last carried it
};

struct TxChannel {
    uint64_t next_msg_seq = 0;
    std::map<uint64_t, Unacked> unacked;  // reliable msgs awaiting ack, by msg_seq
    // Delivery class actually used on this channel. Only ORDERED channels can
    // pin a receiver, so only they advertise a holding seq (S2).
    bool     cls_ordered = false;
    bool     cls_known   = false;
    // Messages allocated a msg_seq but not yet pumped into `unacked`. They are
    // held too, and the advertisement must say so -- advertising next_msg_seq
    // while a queued message sits below it would tell the receiver to skip a
    // message we are about to send. Within a channel the queue is FIFO and
    // seqs are allocated in push order (rejections happen BEFORE allocation),
    // so the lowest queued seq is the front one and popping seq S leaves S+1.
    size_t   queued_count  = 0;
    uint64_t queued_lowest = 0;
};

struct RxChannel {
    bool     started = false;
    uint64_t next_deliver = 0;                    // ordered: next msg_seq to hand up
    // Last time next_deliver advanced (or the channel started). Drives the
    // ordered-stall sweep: a hole the sender has already retired can never
    // fill, and without this the channel pinned forever with no log output.
    double   last_progress_time = -1.0;
    // Armed by the ordered-stall sweep when the channel has delivered nothing
    // for RC_ORDERED_STALL_SEC AND `ooo` is EMPTY -- i.e. there is no buffered
    // successor to skip the hole to, because the sender retired the hole and
    // its whole tail (a pinned cumulative ack stops the sender clearing
    // ANYTHING, so nothing behind the hole is ever re-sent either). While
    // armed, the next message to arrive with seq > next_deliver is ADOPTED as
    // the new cursor instead of being buffered behind a hole that can never
    // fill. Never adopts BACKWARD: a seq below next_deliver still takes the
    // ordinary duplicate path, so a late retransmit cannot re-deliver history.
    bool     stall_resync = false;
    // ...and whether THAT arm was set by the HARD BACKSTOP
    // (RC_ORDERED_STALL_HARD_SEC) rather than by the ordinary 3.5s timer or by
    // a fact-driven repair. A backstop arm is STICKY: the disarm-on-fresh-
    // positive rule in CarrierParseHoldSection must not clear it.
    //
    // WHY (found in adversarial review, 2026-08-16, and it was a real defect):
    // the empty-`ooo` "repair" delivers nothing -- it only ARMS the next
    // arrival to adopt itself as the cursor. A peer that keeps advertising
    // `holding=1, oldest <= next_deliver` sends its next carrier within one
    // carrier interval (33ms receive-driven, 0.5s on the S0 keepalive) and, by
    // the un-stickied disarm rule, cleared the arm the backstop had just set --
    // before any message could adopt it. The deferral was bounded at 9s; the
    // REPAIR was not bounded at all. Real cost: 25s against an honest peer (it
    // takes RC_RETIRE_HARD_SEC for the sender's own advertisement to move above
    // our cursor and trigger fact_repair, whose arm the disarm cannot touch
    // because it requires `oldest <= next_deliver`), and UNBOUNDED against a
    // peer that lies -- while two in-source comments and the risk register all
    // claimed a 9s bound. Reachable shape: a forward path that passes the ~40B
    // carrier and drops the ~900B data datagram, i.e. exactly the path-MTU
    // blackhole RC_RETIRE_HARD_SEC's own comment cites as its reason to exist.
    //
    // Stickiness is safe because an arm only ever adopts FORWARD (DeliverOrdered
    // takes the duplicate path below the cursor) and is consumed by the first
    // delivery, which clears both flags. On an IDLE channel the arm is a no-op:
    // the next message arrives at exactly next_deliver and takes the ordinary
    // in-order path. Only the BACKSTOP arm is sticky -- an arm set by the plain
    // 3.5s no-information timer stays disarmable, which is the window the
    // disarm rule was added to close in the first place.
    bool     stall_resync_backstop = false;
    // S2: the peer's latest holding advertisement for THIS channel (its tx
    // channel of the same number). `peer_hold_oldest` is the lowest msg_seq it
    // still holds; `peer_hold_time` is when we last heard it, so a stale claim
    // can be ignored (RC_HOLD_ADVERT_FRESH_SEC). Cleared on the endpoint-
    // restart back-jump: a carrier built by the PREVIOUS generation of the
    // peer's endpoint describes seqs that no longer exist.
    uint64_t peer_hold_oldest = 0;
    bool     peer_hold_known  = false;
    // Does the peer actually HOLD anything on this channel, or is
    // peer_hold_oldest merely its next_msg_seq on an idle channel? Without
    // this the two are indistinguishable -- see RC_CARRIER_EXT_MAGIC above.
    bool     peer_hold_active = false;
    double   peer_hold_time   = -1.0;
    // Latched while the stall sweep is DEFERRING repair because the peer says
    // it is still holding our hole. Read by IsRepairingOrdered() so the app's
    // starve ladder does not destroy the endpoint mid-repair (S3).
    bool     holding_repair   = false;
    bool     defer_logged     = false;   // one deferral line per stall episode
    std::map<uint64_t, std::vector<uint8_t>> ooo; // ordered: buffered out-of-order
    // Reliable-UNORDERED: deliver each msg once on first receipt, but ACK the
    // CONTIGUOUS received prefix (un_next_ack) so a gap is never falsely acked
    // (which would stop the sender retransmitting a lost msg). SACK-style.
    uint64_t un_next_ack = 0;                     // lowest seq not yet received
    std::set<uint64_t> un_recv_above;             // received seqs above un_next_ack (holes filled)
    bool     cls_ordered = true;                  // which ack model this channel uses
    bool     cls_known = false;
};

// A framed message waiting to be paced onto the wire. Pacing exists so a bulk
// burst (e.g. a ~1MB snapshot = hundreds of datagrams) can't flood the UDP path
// and self-inflict loss + head-of-line stall. Send() enqueues; Update() drains
// under a token-bucket rate + a congestion window (max reliable msgs in flight).
struct PendingSend {
    std::vector<uint8_t> framed;
    uint8_t  chan;
    uint8_t  cls;   // fm2k::rc::Class as int
    uint64_t msg_seq;
};

struct Endpoint {
    reliable_endpoint_t* rel = nullptr;
    RawSendFn raw_send = nullptr;  void* raw_send_ctx = nullptr;
    DeliverFn deliver  = nullptr;  void* deliver_ctx  = nullptr;
    uint64_t id = 0;               // endpoint id (peer key) -- [RC-STATS] tag
    double   last_stats_log = 0.0; // FM2K_RC_STATS 1Hz throttle
    double   last_retire_log = -1.0e9;  // retire-line 1Hz throttle, PER ENDPOINT
    std::unordered_map<uint8_t, TxChannel> tx;
    std::unordered_map<uint8_t, RxChannel> rx;
    std::vector<uint8_t> scratch;  // reused wire buffer (0xCB + reliable packet)
    FecEncoder fec_enc;
    FecDecoder fec_dec;
    bool    fec_enabled = true;
    uint8_t fec_k = FEC_GROUP_K;
    uint8_t fec_m = FEC_GROUP_M;
    // Adaptive-M: scale parity to measured loss (reliable.io packet_loss %) so low
    // loss is efficient and high loss stays live. reference-style bandwidth thrift.
    bool    fec_adaptive = true;
    double  last_adapt_time = -1.0;
    // FEC suppression under send-queue backpressure (the reference engine: suppress FEC on
    // LimitExceeded / send-queue > ~20 for 3s) -- during a bulk backlog, parity is
    // wasted bandwidth; drop it so the actual data drains, restore after.
    bool    fec_suppressed = false;
    double  fec_suppress_until = 0.0;
    double  fec_backlog_since  = -1.0;  // when the byte backlog first went deep (-1 = not deep)
    // Monotonic seconds, refreshed by Update(). DeliverOrdered runs from the
    // datagram path which has no clock of its own, and needs one to timestamp
    // delivery progress.
    double  now_time = 0.0;
    // Default to AUTO retransmit timing (0 => clamp(2x measured RTT, 80ms,
    // 750ms) in Update). The old fixed 100ms default retransmitted BEFORE the
    // ack could arrive on any cross-region link (RTT > 100ms), multiplying
    // bandwidth exactly when the link is worst. SetResendSeconds still overrides.
    double  resend_after = 0.0;
    // Ack-carrier: reliable.io piggybacks acks on outgoing packets only, so on a
    // one-way stream the receiver must periodically send SOMETHING or the sender
    // resends forever. Track receives-since-last-carrier and flush a tiny
    // unreliable packet (header carries the acks) on a cadence.
    uint32_t rx_since_carrier = 0;
    double   last_carrier_time = 0.0;
    // S1 liveness: when we last received ANY datagram from this peer (stamped
    // in OnDatagram). -1 = never heard from. The retire test reads this, and
    // the S0 keepalive is what keeps it meaningful during a one-way outage.
    double   last_peer_rx_time = -1.0;
    // Sum of framed sizes currently held in every tx channel's `unacked` map.
    // Maintained incrementally (emplace / ack-erase / retire-erase) so the
    // byte cap costs nothing per Update.
    size_t   unacked_bytes = 0;
    // [RC-STATS] counters for the liveness machinery. Never logged per message.
    uint64_t st_retire_dead = 0, st_retire_hard = 0, st_retire_bytecap = 0;
    uint64_t st_retire_flat = 0;      // kill-switch / liveness-off path
    uint64_t st_hold_adverts_sent = 0;
    uint64_t st_unanchor_by_fact = 0, st_unanchor_by_timer = 0;
    uint64_t st_hold_defers = 0;
    // Arms taken by the HARD backstop (RC_ORDERED_STALL_HARD_SEC) rather than by
    // the 3.5s timer or a fact. Non-zero means a peer kept us deferring all the
    // way to the bound; it is the counter that makes the sticky-arm path (see
    // RxChannel::stall_resync_backstop) visible in the field instead of
    // inferred, and the one a unit test asserts on to prove the branch is
    // reached at all.
    uint64_t st_backstop_arms = 0;
    // Send pacing (token bucket) + congestion window (max reliable in-flight).
    // Defaults let small live batches send immediately (burst covers them) while
    // a bulk burst drains at rate_pps. cwnd provides backpressure under loss.
    std::deque<PendingSend> send_queue;
    size_t   send_queue_bytes = 0;  // sum of framed sizes in send_queue (FEC + cap decisions)
    double   tokens = 0.0;
    double   last_pump = -1.0;
    // Paced DATAGRAMS/sec (not messages -- see WireCost). 3000 was nominal but
    // fictional: the bucket charged 1 token per RC message while reliable.io
    // fragmented a 16KB message into 17 datagrams downstream, so the real
    // offered rate was ~51,000 datagrams/s (~53 MB/s) and the pacer did
    // nothing. With honest accounting the number finally means something, so
    // it is set to what a home uplink can actually absorb: 1000 datagrams/s at
    // ~1KB is ~8 Mbit/s INCLUDING FEC parity, which still lands a compressed
    // ~100KB snapshot in ~0.2s and the 1.08MB uncompressed worst case in
    // ~2.3s -- both inside RC's 7s per-message retire. FM2K_RC_RATE_PPS
    // overrides (reliable_channel_net.cpp) for sweeps.
    double   rate_pps = 1000.0;
    double   burst_max = 24.0;    // token-bucket cap (immediate burst allowance)
    int      cwnd = 96;           // max reliable msgs awaiting ack before we stall new sends
};

// ---- reliable_channel_fec.cpp ----------------------------------------------
// reliable.io egress hook: FEC-encodes each outgoing datagram and puts it on
// the wire. Installed as reliable_config_t::transmit_packet_function in
// Create(), which is why it needs external linkage.
void TransmitCb(void* context, uint64_t id, uint16_t seq, uint8_t* data, int bytes);

// ---- reliable_channel_liveness.cpp -----------------------------------------
// The holding-advertisement codec. `CarrierAppendHoldSection` appends the
// trailing section to a carrier body that already holds its cumulative block
// and returns the new offset (unchanged when there is nothing to advertise);
// `CarrierParseHoldSection` reads one from the bytes remaining after a peer's
// cumulative block. `LivenessEnabled()` and `IsRepairingOrdered()` live in the
// same TU and are declared in reliable_channel.h.
size_t CarrierAppendHoldSection(Endpoint* ep, uint8_t* body, size_t off);
void   CarrierParseHoldSection(Endpoint* ep, const uint8_t* p, const uint8_t* end);

}  // namespace rc
}  // namespace fm2k
