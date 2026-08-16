// reliable_channel_fec.cpp -- the FEC half of the ReliableChannel layer.
//
// Split out of reliable_channel.cpp (1000-line cap); the shared state model
// lives in reliable_channel_internal.h. This TU owns:
//   egress  -- TransmitCb: wrap each outgoing reliable.io datagram in a FEC
//              role header, buffer its coded form, and emit M Reed-Solomon
//              parity datagrams after every K data datagrams. If exactly one
//              data datagram of a group is lost the receiver rebuilds it with
//              NO retransmit round trip, which is what removes the per-loss
//              head-of-line stall an ordered stream otherwise suffers.
//   ingress -- OnDatagram: demux the role header, feed the inner reliable.io
//              packet straight through (never wait on FEC), and reconstruct
//              any missing datagram of a group from survivors + parity.
#include "reliable_channel_internal.h"
#include "rc_reed_solomon.h"

#include <cstring>

namespace fm2k {
namespace rc {

namespace {

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

}  // namespace

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

void OnDatagram(Endpoint* ep, const uint8_t* data, int len) {
    if (!ep || !ep->rel || len <= 0) return;
    ep->rx_since_carrier++;  // received something -> owe the peer an ack carrier
    // caller already stripped the 0xCB tag; first byte is the FEC role.
    uint8_t role = data[0];

    // S1 liveness: a datagram from the peer -- data, parity or a bare ack
    // carrier -- is proof it is alive. Stamped here rather than at delivery on
    // purpose: a peer whose acks reach us while its data does not is exactly
    // the "alive and asking" state the retire test must not read as dead, and
    // waiting for a DELIVERY would make the test fail closed in the one outage
    // it targets.
    //
    // WHAT THIS DELIBERATELY DOES AND DOES NOT PROVE (review D5a, 2026-08-16 --
    // the consequences were understated when this stamp was first written):
    //   * it is a LINK-liveness test, not a peer-correctness test. A peer whose
    //     RC layer emits carriers while acking nothing keeps the sender
    //     retaining and blind-retransmitting for the full RC_RETIRE_HARD_SEC.
    //     RC_RETIRE_HARD_SEC is what bounds that, and it is why it exists.
    //   * routing is by source address alone (Addr_ActorString in
    //     reliable_channel_net.cpp), and UDP source addresses are spoofable, so
    //     an off-path attacker who knows the peer tuple can hold `peer_silent`
    //     false at ~2 packets/s of traffic. Bounded (25s ceiling) and
    //     spectator-only (RC has no other consumer), so this is a recorded
    //     limitation, not a defended boundary. It is strictly cheaper than the
    //     pre-existing forged-cumulative-ack hole, which is also untouched.
    // CHEAP HARDENING TAKEN: the stamp now happens only after the role byte
    // parses to a role this codec knows, so a stream of arbitrary 0xCB-tagged
    // garbage no longer counts as proof of life. rx_since_carrier is left
    // above it on purpose -- owing an ack for a datagram we could not parse is
    // harmless, and moving it would change the carrier cadence for no gain.
    if (role == FEC_RAW || role == FEC_DATA || role == FEC_PARITY)
        ep->last_peer_rx_time = ep->now_time;

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

}  // namespace rc
}  // namespace fm2k
