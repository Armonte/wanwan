// reliable_channel.cpp — see reliable_channel.h.
//
// Layering:
//   Send(channel,class,msg) -> frame [chan|class|msg_seq|len|payload]
//                           -> reliable_endpoint_send_packet (seq/ack/fragment)
//                           -> transmit cb -> prefix 0xCB -> RawSend -> wire
//   wire -> OnDatagram(strip 0xCB) -> reliable_endpoint_receive_packet (reassemble)
//                                  -> process cb -> parse frame -> ordered deliver
//   Update() -> reliable_endpoint_update + ack-driven retransmit of unacked.
//
// FEC (parity / Reed-Solomon over K-packet groups) is the next layer and slots
// in around send_packet/process (see design doc). Not present yet: this gives
// reliable-ordered-over-UDP, which already avoids TCP's congestion collapse under
// loss; FEC removes the remaining per-loss HOL stall.
#include "reliable_channel.h"

extern "C" {
#include "reliable.h"
}
#include "rc_reed_solomon.h"

#include <SDL3/SDL_log.h>   // ooo-buffer-full diagnostic (bounded, ~1/64)
#include <cstring>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace fm2k {
namespace rc {

namespace {

// CRC32 (IEEE) over an RC message frame. CRITICAL: FEC reconstruction can
// produce a byte-wrong packet that still parses as a valid-looking RC message;
// without this, that corruption would be DELIVERED and silently desync the
// consumer (observed: 15% loss, one seed, spectator diverged from frame 5). With
// the CRC, a corrupt/mis-reconstructed message fails verification and is dropped,
// degrading gracefully to stall+retransmit instead of silent desync.
uint32_t Crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

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

// ---- FEC (XOR parity over groups of K packets) -----------------------------
// Each outgoing reliable.io datagram is wrapped with a 1-byte FEC role. Data
// datagrams stream immediately at real size; after every K data datagrams a
// single PARITY datagram is emitted = XOR of the group's "coded forms"
// ([u16 inner_len][inner], zero-padded to the group max). If exactly ONE of the
// K data datagrams is lost, the receiver reconstructs it from parity + the K-1
// survivors WITHOUT a retransmit round-trip — this is what removes the per-loss
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

}  // namespace

struct Endpoint {
    reliable_endpoint_t* rel = nullptr;
    RawSendFn raw_send = nullptr;  void* raw_send_ctx = nullptr;
    DeliverFn deliver  = nullptr;  void* deliver_ctx  = nullptr;
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
    // LimitExceeded / send-queue > ~20 for 3s) — during a bulk backlog, parity is
    // wasted bandwidth; drop it so the actual data drains, restore after.
    bool    fec_suppressed = false;
    double  fec_suppress_until = 0.0;
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
    double   tokens = 0.0;
    double   last_pump = -1.0;
    double   rate_pps = 3000.0;   // paced datagrams/sec
    double   burst_max = 24.0;    // token-bucket cap (immediate burst allowance)
    int      cwnd = 96;           // max reliable msgs awaiting ack before we stall new sends
};

namespace {

void EnsureReliableInit() {
    static bool inited = false;
    if (!inited) { reliable_init(); inited = true; }
}

// Emit one already-built datagram body (FEC header + payload) to the wire,
// prefixing the 0xCB demux tag.
void SendWire(Endpoint* ep, const uint8_t* body, size_t body_len) {
    if (!ep->raw_send || body_len == 0) return;
    ep->scratch.clear();
    ep->scratch.reserve(body_len + 1);
    ep->scratch.push_back(RC_WIRE_TAG);
    ep->scratch.insert(ep->scratch.end(), body, body + body_len);
    ep->raw_send(ep->raw_send_ctx, ep->scratch.data(), static_cast<int>(ep->scratch.size()));
}

// reliable.io -> wire: FEC-encode each outgoing datagram.
void TransmitCb(void* context, uint64_t /*id*/, uint16_t /*seq*/, uint8_t* data, int bytes) {
    Endpoint* ep = static_cast<Endpoint*>(context);
    if (!ep->raw_send || bytes <= 0) return;

    if (!ep->fec_enabled || ep->fec_suppressed) {
        // No parity: abandon any partial group so it can't resume with stale state.
        if (ep->fec_enc.index != 0) { ep->fec_enc.index = 0; ep->fec_enc.coded.clear(); }
        std::vector<uint8_t> body; body.reserve(1 + bytes);
        body.push_back(FEC_RAW);
        body.insert(body.end(), data, data + bytes);
        SendWire(ep, body.data(), body.size());
        return;
    }

    FecEncoder& e = ep->fec_enc;
    uint8_t K = ep->fec_k, M = ep->fec_m;
    uint16_t inner_len = static_cast<uint16_t>(bytes);
    // DATA datagram (streams immediately at real size).
    std::vector<uint8_t> body; body.reserve(FEC_DATA_HDR + bytes);
    body.push_back(FEC_DATA);
    body.push_back(static_cast<uint8_t>(e.group & 0xFF));
    body.push_back(static_cast<uint8_t>(e.group >> 8));
    body.push_back(e.index);
    body.push_back(K);
    body.push_back(M);
    body.push_back(static_cast<uint8_t>(inner_len & 0xFF));
    body.push_back(static_cast<uint8_t>(inner_len >> 8));
    body.insert(body.end(), data, data + bytes);
    SendWire(ep, body.data(), body.size());

    // buffer the coded form [u16 inner_len][inner] for RS parity computation.
    std::vector<uint8_t> coded; coded.reserve(2 + bytes);
    coded.push_back(static_cast<uint8_t>(inner_len & 0xFF));
    coded.push_back(static_cast<uint8_t>(inner_len >> 8));
    coded.insert(coded.end(), data, data + bytes);
    e.coded.push_back(std::move(coded));

    if (++e.index >= K) {
        // Group complete: pad all coded forms to L=max, RS-encode M parity blocks.
        size_t L = 0;
        for (auto& c : e.coded) if (c.size() > L) L = c.size();
        std::vector<std::vector<uint8_t>> dblk(K);
        std::vector<const uint8_t*> dptr(K);
        for (int i = 0; i < K; i++) {
            dblk[i] = e.coded[i];
            dblk[i].resize(L, 0);
            dptr[i] = dblk[i].data();
        }
        std::vector<std::vector<uint8_t>> pblk(M, std::vector<uint8_t>(L, 0));
        std::vector<uint8_t*> pptr(M);
        for (int j = 0; j < M; j++) pptr[j] = pblk[j].data();
        rs::encode(K, M, static_cast<int>(L), dptr.data(), pptr.data());
        uint16_t L16 = static_cast<uint16_t>(L);
        for (int j = 0; j < M; j++) {
            std::vector<uint8_t> pbody; pbody.reserve(FEC_PARITY_HDR + L);
            pbody.push_back(FEC_PARITY);
            pbody.push_back(static_cast<uint8_t>(e.group & 0xFF));
            pbody.push_back(static_cast<uint8_t>(e.group >> 8));
            pbody.push_back(static_cast<uint8_t>(j));
            pbody.push_back(K);
            pbody.push_back(M);
            pbody.push_back(static_cast<uint8_t>(L16 & 0xFF));
            pbody.push_back(static_cast<uint8_t>(L16 >> 8));
            pbody.insert(pbody.end(), pblk[j].begin(), pblk[j].end());
            SendWire(ep, pbody.data(), pbody.size());
        }
        e.group++;
        e.index = 0;
        e.coded.clear();
    }
}

// Feed a recovered/received inner reliable.io packet into the endpoint.
void FeedInner(Endpoint* ep, const uint8_t* inner, int inner_len) {
    if (inner_len > 0)
        reliable_endpoint_receive_packet(ep->rel, const_cast<uint8_t*>(inner), inner_len);
}

// Try to RS-reconstruct a group's missing data packets from survivors + parity.
void FecTryRecover(Endpoint* ep, uint16_t group_id, FecGroupRx& g) {
    if (g.recovered || !g.params_known || g.L == 0) return;
    int K = g.k, M = g.m, L = g.L;
    // Need all K data present to skip; otherwise need >= K of (data+parity).
    if (static_cast<int>(g.data.size()) >= K) { g.recovered = true; return; }
    if (static_cast<int>(g.data.size() + g.parity.size()) < K) return;  // not enough yet
    if (static_cast<int>(g.data.size()) == K) { g.recovered = true; return; }

    // Build the K present rows/blocks (data rows first, then parity), each length L.
    std::vector<int> rows;
    std::vector<std::vector<uint8_t>> blkstore;
    std::vector<const uint8_t*> blks;
    blkstore.reserve(K);
    for (auto& kv : g.data) {
        if (static_cast<int>(rows.size()) >= K) break;
        // coded form [u16 len][inner] padded to L
        std::vector<uint8_t> coded; coded.reserve(L);
        uint16_t il = static_cast<uint16_t>(kv.second.size());
        coded.push_back(static_cast<uint8_t>(il & 0xFF));
        coded.push_back(static_cast<uint8_t>(il >> 8));
        coded.insert(coded.end(), kv.second.begin(), kv.second.end());
        coded.resize(L, 0);
        rows.push_back(kv.first);
        blkstore.push_back(std::move(coded));
    }
    for (auto& kv : g.parity) {
        if (static_cast<int>(rows.size()) >= K) break;
        std::vector<uint8_t> pb = kv.second; pb.resize(L, 0);
        rows.push_back(K + kv.first);
        blkstore.push_back(std::move(pb));
    }
    if (static_cast<int>(rows.size()) < K) return;
    for (auto& b : blkstore) blks.push_back(b.data());

    std::vector<std::vector<uint8_t>> out(K, std::vector<uint8_t>(L, 0));
    std::vector<uint8_t*> optr(K);
    for (int i = 0; i < K; i++) optr[i] = out[i].data();
    if (!rs::decode(K, M, L, rows, blks, optr.data())) { g.recovered = true; return; }

    // Feed any data index that was missing (recovered coded form -> inner).
    for (int i = 0; i < K; i++) {
        if (g.data.count(static_cast<uint8_t>(i))) continue;
        const std::vector<uint8_t>& c = out[i];
        if (c.size() < 2) continue;
        uint16_t mlen = static_cast<uint16_t>(c[0]) | (static_cast<uint16_t>(c[1]) << 8);
        if (2u + mlen <= c.size()) FeedInner(ep, c.data() + 2, mlen);
    }
    g.recovered = true;
    (void)group_id;
}

void FecGc(FecDecoder& dec) {
    while (dec.groups.size() > FEC_RX_GROUPS) dec.groups.erase(dec.groups.begin());
}

void DeliverOrdered(Endpoint* ep, uint8_t chan, RxChannel& rc,
                    uint64_t seq, const uint8_t* payload, int plen) {
    if (!rc.started) { rc.started = true; rc.next_deliver = seq; }  // mid-join: start here
    if (seq < rc.next_deliver) return;                              // already delivered
    if (seq == rc.next_deliver) {
        if (ep->deliver) ep->deliver(ep->deliver_ctx, chan, payload, plen);
        rc.next_deliver++;
        // drain any contiguous buffered successors
        auto it = rc.ooo.find(rc.next_deliver);
        while (it != rc.ooo.end()) {
            if (ep->deliver) ep->deliver(ep->deliver_ctx, chan,
                                         it->second.data(), static_cast<int>(it->second.size()));
            rc.ooo.erase(it);
            rc.next_deliver++;
            it = rc.ooo.find(rc.next_deliver);
        }
    } else if (rc.ooo.size() < RC_OOO_MAX) {
        rc.ooo.emplace(seq, std::vector<uint8_t>(payload, payload + plen));  // buffer, in order
    } else {
        // Reassembly buffer full: the gap at next_deliver isn't filling. Stop
        // hoarding memory — drop this future message. reliable.io already
        // acked the packet, so the channel will now stall at the gap; the
        // consumer's stall watchdog re-JOINs and re-syncs. Bounded, not OOM.
        static uint32_t s_ooo_drop_log = 0;
        if ((s_ooo_drop_log++ & 0x3F) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "ReliableChannel: ooo buffer full (%zu) at next=%llu, dropping "
                "seq=%llu — channel stalled, awaiting reconnect",
                rc.ooo.size(), (unsigned long long)rc.next_deliver,
                (unsigned long long)seq);
        }
    }
}

// Deliver one already-CRC-verified message to the app/ordered queue.
void DeliverOne(Endpoint* ep, uint8_t chan, uint8_t cls, uint64_t seq,
                const uint8_t* payload, uint16_t len);

// reliable.io -> us: a reassembled packet carrying ONE OR MORE framed messages
// (Nagle-coalesced). Loop-parse; each message is independently CRC-gated. On a
// CRC failure the message length is untrustworthy, so we stop parsing the rest of
// the packet (those messages retransmit) rather than risk mis-advancing.
int ProcessCb(void* context, uint64_t /*id*/, uint16_t /*seq*/, uint8_t* data, int bytes) {
    Endpoint* ep = static_cast<Endpoint*>(context);
    int off = 0;
    while (off + static_cast<int>(RC_HDR_BYTES + RC_CRC_BYTES) <= bytes) {
        const uint8_t* m = data + off;
        uint16_t len; std::memcpy(&len, m + 10, 2);
        if (off + static_cast<int>(RC_HDR_BYTES) + len + static_cast<int>(RC_CRC_BYTES) > bytes) break;  // truncated
        uint32_t want; std::memcpy(&want, m + RC_HDR_BYTES + len, 4);
        if (Crc32(m, RC_HDR_BYTES + len) != want) break;  // corrupt -> stop (rest retransmits)
        uint8_t  chan = m[0];
        uint8_t  cls  = m[1];
        uint64_t seq; std::memcpy(&seq, m + 2, 8);
        DeliverOne(ep, chan, cls, seq, m + RC_HDR_BYTES, len);
        off += static_cast<int>(RC_HDR_BYTES) + len + static_cast<int>(RC_CRC_BYTES);
    }
    return 1;
}

void DeliverOne(Endpoint* ep, uint8_t chan, uint8_t cls, uint64_t seq,
                const uint8_t* payload, uint16_t len) {
    if (chan == RC_ACK_CARRIER_CHANNEL) {
        // RC-level cumulative ack table: [u8 count]{ u8 chan, u64 next_expected }.
        // Clears our unacked for msgs the peer has actually DELIVERED (CRC-verified,
        // in order) — NOT merely transport-received. This is why a corrupt packet
        // (reliable.io-acked but CRC-dropped) still gets retransmitted.
        if (len >= 1) {
            uint8_t cnt = payload[0];
            const uint8_t* p = payload + 1;
            const uint8_t* end = payload + len;
            for (uint8_t i = 0; i < cnt && p + 9 <= end; i++) {
                uint8_t  ackchan = p[0];
                uint64_t nexp;  std::memcpy(&nexp, p + 1, 8);
                p += 9;
                auto it = ep->tx.find(ackchan);
                if (it != ep->tx.end()) {
                    auto& un = it->second.unacked;
                    for (auto uit = un.begin(); uit != un.end();)
                        if (uit->first < nexp) uit = un.erase(uit); else ++uit;
                }
            }
        }
        return;
    }

    if (cls == static_cast<uint8_t>(Class::ReliableOrdered)) {
        RxChannel& rc = ep->rx[chan];
        rc.cls_known = true; rc.cls_ordered = true;
        DeliverOrdered(ep, chan, rc, seq, payload, len);
    } else if (cls == static_cast<uint8_t>(Class::ReliableUnordered)) {
        RxChannel& rc = ep->rx[chan];
        rc.cls_known = true; rc.cls_ordered = false;
        if (seq < rc.un_next_ack || rc.un_recv_above.count(seq)) return;  // duplicate
        if (ep->deliver) ep->deliver(ep->deliver_ctx, chan, payload, len);  // deliver first receipt
        if (seq == rc.un_next_ack) {
            rc.un_next_ack++;
            while (rc.un_recv_above.count(rc.un_next_ack)) { rc.un_recv_above.erase(rc.un_next_ack); rc.un_next_ack++; }
        } else {
            rc.un_recv_above.insert(seq);
        }
    } else {  // Unreliable
        if (ep->deliver) ep->deliver(ep->deliver_ctx, chan, payload, len);
    }
}

// Build the RC message frame (chan|cls|msg_seq|len|payload|crc32); does NOT send.
void BuildFramed(uint8_t chan, Class cls, uint64_t seq,
                 const uint8_t* data, int len, std::vector<uint8_t>& out) {
    out.resize(RC_HDR_BYTES + static_cast<size_t>(len) + RC_CRC_BYTES);
    out[0] = chan;
    out[1] = static_cast<uint8_t>(cls);
    std::memcpy(out.data() + 2, &seq, 8);
    uint16_t l16 = static_cast<uint16_t>(len);
    std::memcpy(out.data() + 10, &l16, 2);
    if (len > 0) std::memcpy(out.data() + RC_HDR_BYTES, data, len);
    uint32_t crc = Crc32(out.data(), RC_HDR_BYTES + static_cast<size_t>(len));
    std::memcpy(out.data() + RC_HDR_BYTES + len, &crc, 4);
}

// Push a framed message onto the reliable.io endpoint; returns its packet seq.
uint16_t SendFramedNow(Endpoint* ep, const std::vector<uint8_t>& framed) {
    uint16_t pkt_seq = reliable_endpoint_next_packet_sequence(ep->rel);
    reliable_endpoint_send_packet(ep->rel, const_cast<uint8_t*>(framed.data()),
                                  static_cast<int>(framed.size()));
    return pkt_seq;
}

size_t InFlight(Endpoint* ep) {
    size_t n = 0;
    for (auto& kv : ep->tx) n += kv.second.unacked.size();
    return n;
}

// Drain the paced send queue: token-bucket rate + congestion-window backpressure.
void PumpSendQueue(Endpoint* ep, double now) {
    if (ep->last_pump < 0.0) {
        ep->tokens = ep->burst_max;  // first pump: full burst allowance
    } else {
        double dt = now - ep->last_pump;
        if (dt < 0.0) dt = 0.0;
        ep->tokens += ep->rate_pps * dt;
        if (ep->tokens > ep->burst_max) ep->tokens = ep->burst_max;
    }
    ep->last_pump = now;

    constexpr size_t RC_NAGLE_MAX = 953;  // coalesce up to ~950B into one packet (reference-style)
    size_t inflight = InFlight(ep);
    while (!ep->send_queue.empty() && ep->tokens >= 1.0) {
        {   // cwnd backpressure check on the ordered head
            PendingSend& f = ep->send_queue.front();
            bool r = f.cls != static_cast<uint8_t>(Class::Unreliable);
            if (r && inflight >= static_cast<size_t>(ep->cwnd)) break;
        }
        // Nagle: pack consecutive queued messages into one reliable.io packet.
        std::vector<uint8_t> pkt;
        while (!ep->send_queue.empty()) {
            PendingSend& ps = ep->send_queue.front();
            bool reliable = ps.cls != static_cast<uint8_t>(Class::Unreliable);
            if (reliable && inflight >= static_cast<size_t>(ep->cwnd)) break;      // cwnd cap
            if (!pkt.empty() && pkt.size() + ps.framed.size() > RC_NAGLE_MAX) break; // packet full
            pkt.insert(pkt.end(), ps.framed.begin(), ps.framed.end());  // copy into packet
            if (reliable) {
                Unacked u; u.framed = std::move(ps.framed); u.sent_time = now;
                u.first_time = now; u.pkt_seq = 0;
                ep->tx[ps.chan].unacked.emplace(ps.msg_seq, std::move(u));  // per-msg retransmit
                inflight++;
            }
            ep->send_queue.pop_front();
            if (pkt.size() >= RC_NAGLE_MAX) break;  // approx full (a lone big msg goes alone)
        }
        if (pkt.empty()) break;
        SendFramedNow(ep, pkt);  // one reliable.io packet (may fragment if > MTU)
        ep->tokens -= 1.0;       // 1 token per packet
    }
}

}  // namespace

Endpoint* Create(uint64_t id, RawSendFn raw_send, void* raw_send_ctx,
                 DeliverFn deliver, void* deliver_ctx, double now) {
    EnsureReliableInit();
    Endpoint* ep = new Endpoint();
    ep->raw_send = raw_send; ep->raw_send_ctx = raw_send_ctx;
    ep->deliver = deliver;   ep->deliver_ctx = deliver_ctx;
    reliable_config_t cfg;
    reliable_default_config(&cfg);
    reliable_copy_string(cfg.name, "rc", sizeof(cfg.name));
    cfg.context = ep;
    cfg.id = id;
    cfg.transmit_packet_function = &TransmitCb;
    cfg.process_packet_function  = &ProcessCb;
    ep->rel = reliable_endpoint_create(&cfg, now);
    if (!ep->rel) { delete ep; return nullptr; }
    return ep;
}

void Destroy(Endpoint* ep) {
    if (!ep) return;
    if (ep->rel) reliable_endpoint_destroy(ep->rel);
    delete ep;
}

void Send(Endpoint* ep, uint8_t chan, Class cls, const uint8_t* data, int len) {
    if (!ep || !ep->rel || len < 0) return;
    TxChannel& tc = ep->tx[chan];
    PendingSend ps;
    ps.chan = chan;
    ps.cls = static_cast<uint8_t>(cls);
    ps.msg_seq = tc.next_msg_seq++;
    BuildFramed(chan, cls, ps.msg_seq, data, len, ps.framed);
    ep->send_queue.push_back(std::move(ps));  // paced out in Update()
}

void OnDatagram(Endpoint* ep, const uint8_t* data, int len) {
    if (!ep || !ep->rel || len <= 0) return;
    ep->rx_since_carrier++;  // received something -> owe the peer an ack carrier
    // caller already stripped the 0xCB tag; first byte is the FEC role.
    uint8_t role = data[0];

    if (role == FEC_RAW) {
        FeedInner(ep, data + FEC_RAW_HDR, len - static_cast<int>(FEC_RAW_HDR));
        return;
    }

    if (role == FEC_DATA) {
        if (len < static_cast<int>(FEC_DATA_HDR)) return;
        uint16_t group = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);
        uint8_t  index = data[3];
        uint8_t  k     = data[4];
        uint8_t  m     = data[5];
        uint16_t ilen  = static_cast<uint16_t>(data[6]) | (static_cast<uint16_t>(data[7]) << 8);
        if (FEC_DATA_HDR + ilen > static_cast<size_t>(len)) return;
        const uint8_t* inner = data + FEC_DATA_HDR;
        FeedInner(ep, inner, ilen);  // deliver immediately; don't wait on FEC
        FecGroupRx& g = ep->fec_dec.groups[group];
        g.k = k; g.m = m; g.params_known = true;
        g.data.emplace(index, std::vector<uint8_t>(inner, inner + ilen));
        FecTryRecover(ep, group, g);
        FecGc(ep->fec_dec);
        return;
    }

    if (role == FEC_PARITY) {
        if (len < static_cast<int>(FEC_PARITY_HDR)) return;
        uint16_t group   = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);
        uint8_t  j       = data[3];
        uint8_t  k       = data[4];
        uint8_t  m       = data[5];
        uint16_t Llen    = static_cast<uint16_t>(data[6]) | (static_cast<uint16_t>(data[7]) << 8);
        if (FEC_PARITY_HDR + Llen > static_cast<size_t>(len)) return;
        FecGroupRx& g = ep->fec_dec.groups[group];
        g.k = k; g.m = m; g.params_known = true; g.L = Llen;
        g.parity.emplace(j, std::vector<uint8_t>(data + FEC_PARITY_HDR, data + FEC_PARITY_HDR + Llen));
        FecTryRecover(ep, group, g);
        FecGc(ep->fec_dec);
        return;
    }
}

void Update(Endpoint* ep, double now) {
    if (!ep || !ep->rel) return;
    reliable_endpoint_update(ep->rel, now);

    // 0) ack-carrier: periodically send our RC-level cumulative ack table so the
    // peer can clear/retransmit precisely on what we've DELIVERED. Sent DIRECTLY
    // (not queued) so acks always flow — never blocked by pacing or a cwnd-stalled
    // reliable head (which would otherwise deadlock recovery).
    if (ep->rx_since_carrier > 0 && now - ep->last_carrier_time >= RC_ACK_FLUSH_INTERVAL) {
        // body = [u8 count]{ u8 chan, u64 next_expected } for each started rx channel.
        uint8_t body[1 + 32 * 9];
        uint8_t cnt = 0;
        size_t off = 1;
        for (auto& kv : ep->rx) {
            if (!kv.second.cls_known) continue;
            if (cnt >= 32) break;
            // Cumulative next-expected: ordered -> next_deliver; unordered -> the
            // contiguous received prefix (un_next_ack). Either way the sender may
            // clear all msgs with seq < this and must retransmit the rest.
            uint64_t nexp = kv.second.cls_ordered ? kv.second.next_deliver
                                                  : kv.second.un_next_ack;
            body[off] = kv.first;
            std::memcpy(body + off + 1, &nexp, 8);
            off += 9; cnt++;
        }
        body[0] = cnt;
        std::vector<uint8_t> framed;
        BuildFramed(RC_ACK_CARRIER_CHANNEL, Class::Unreliable, 0, body, static_cast<int>(off), framed);
        SendFramedNow(ep, framed);
        ep->rx_since_carrier = 0;
        ep->last_carrier_time = now;
    }

    // 1) unacked clearing is driven by the peer's RC-level cumulative acks (in
    // ProcessCb), NOT reliable.io transport acks — because reliable.io acks a
    // corrupt-but-received packet, which must NOT count as delivered. We still
    // drain reliable.io's ack list so it doesn't leak (it feeds RTT internally).
    int num_acks = 0;
    (void)reliable_endpoint_get_acks(ep->rel, &num_acks);
    if (num_acks > 0) reliable_endpoint_clear_acks(ep->rel);

    // 1a) FEC suppression under send-queue backpressure (the reference engine: >~20 backlog
    // or transport LimitExceeded → suppress FEC 3s). During a bulk backlog parity
    // is wasted bandwidth; drop it so the actual data drains, restore after.
    if (ep->send_queue.size() > 20) ep->fec_suppress_until = now + 3.0;
    ep->fec_suppressed = (now < ep->fec_suppress_until);

    // 1a2) Adaptive-M: retune parity to measured loss every ~0.5s. Low loss →
    // M=2 (efficient); high loss → up to M=6 (stays live). Only affects the NEXT
    // group (each group's header carries its own K,M; decoder reads per-group).
    if (ep->fec_adaptive && (ep->last_adapt_time < 0.0 || now - ep->last_adapt_time >= 0.5)) {
        ep->last_adapt_time = now;
        float loss = reliable_endpoint_packet_loss(ep->rel);  // percent, smoothed
        uint8_t m;
        if      (loss < 3.0f)  m = 2;
        else if (loss < 7.0f)  m = 3;
        else if (loss < 13.0f) m = 4;
        else if (loss < 20.0f) m = 5;
        else                   m = 6;
        // never exceed the RS field capacity (K+M <= 255) nor the configured floor
        if (m < 2) m = 2;
        ep->fec_m = m;
    }

    // 1b) drain the paced send queue (token bucket rate + cwnd backpressure).
    PumpSendQueue(ep, now);

    // 2) retransmit reliable msgs unacked for too long. Timeout = clamp(2*RTT,
    // 80ms, 750ms) (matches the reference engine's reference reliable peer). Messages unacked for
    // 7s are retired (connection stuck) — the consumer's stall watchdog re-JOINs.
    double resend = ep->resend_after;
    if (resend <= 0.0) {  // auto
        float rtt = reliable_endpoint_rtt(ep->rel) / 1000.0f;  // reliable.io RTT is ms
        resend = rtt > 0.0f ? (rtt * 2.0) : 0.08;
        if (resend < 0.08) resend = 0.08;
        if (resend > 0.75) resend = 0.75;
    }
    for (auto& tkv : ep->tx) {
        auto& un = tkv.second.unacked;
        for (auto it = un.begin(); it != un.end();) {
            Unacked& u = it->second;
            if (u.first_time < 0.0) u.first_time = now;
            if (now - u.first_time >= 7.0) { it = un.erase(it); continue; }  // stuck -> retire
            if (u.sent_time == 0.0) { u.sent_time = now; ++it; continue; }
            if (now - u.sent_time >= resend) {
                u.pkt_seq = reliable_endpoint_next_packet_sequence(ep->rel);
                reliable_endpoint_send_packet(ep->rel, u.framed.data(),
                                              static_cast<int>(u.framed.size()));
                u.sent_time = now;
            }
            ++it;
        }
    }
}

void SetFec(Endpoint* ep, bool enabled, uint8_t k, uint8_t m) {
    if (!ep) return;
    ep->fec_enabled = enabled;
    if (k >= 1) ep->fec_k = k;
    if (m >= 1) ep->fec_m = m;
}

void SetResendSeconds(Endpoint* ep, double seconds) {
    if (ep) ep->resend_after = seconds;
}

void SetFecAdaptive(Endpoint* ep, bool on) {
    if (ep) ep->fec_adaptive = on;
}

void SetPacing(Endpoint* ep, double rate_pps, double burst_max, int cwnd) {
    if (!ep) return;
    if (rate_pps > 0.0)  ep->rate_pps = rate_pps;
    if (burst_max > 0.0) ep->burst_max = burst_max;
    if (cwnd > 0)        ep->cwnd = cwnd;
}

float Rtt(Endpoint* ep)        { return (ep && ep->rel) ? reliable_endpoint_rtt(ep->rel) : 0.0f; }
float PacketLoss(Endpoint* ep) { return (ep && ep->rel) ? reliable_endpoint_packet_loss(ep->rel) : 0.0f; }

}  // namespace rc
}  // namespace fm2k
