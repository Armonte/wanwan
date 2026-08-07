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

// reliable.io envelope. Mirrored into the reliable_config_t in Create(); hoisted
// here because the PACER must know them -- a packet at or below fragment_above
// is one datagram, anything larger becomes ceil(bytes / fragment_size) fragment
// datagrams (reliable.c:784-830), and the token bucket has to charge for what
// actually hits the wire.
constexpr size_t   RC_MAX_PACKET_BYTES = 64 * 1024;
constexpr size_t   RC_FRAGMENT_ABOVE   = 1024;
constexpr size_t   RC_FRAGMENT_SIZE    = 1024;

// Ordered-channel stall horizon. Must sit ABOVE the 7s unacked retire below:
// once the sender has retired a message it will never be sent again, so a
// receiver that keeps waiting for it waits forever. See the stall sweep in
// Update().
constexpr double   RC_ORDERED_STALL_SEC = 8.0;

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
};

struct RxChannel {
    bool     started = false;
    uint64_t next_deliver = 0;                    // ordered: next msg_seq to hand up
    // Last time next_deliver advanced (or the channel started). Drives the
    // ordered-stall sweep: a hole the sender has already retired can never
    // fill, and without this the channel pinned forever with no log output.
    double   last_progress_time = -1.0;
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

}  // namespace rc
}  // namespace fm2k
