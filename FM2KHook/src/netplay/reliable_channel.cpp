// reliable_channel.cpp -- see reliable_channel.h.
//
// Layering:
//   Send(channel,class,msg) -> frame [chan|class|msg_seq|len|payload]
//                           -> paced send queue (token bucket + cwnd)
//                           -> reliable_endpoint_send_packet (seq/ack/fragment)
//                           -> TransmitCb -> FEC wrap -> 0xCB -> RawSend -> wire
//   wire -> OnDatagram(strip 0xCB) -> FEC demux/recover
//                                  -> reliable_endpoint_receive_packet (reassemble)
//                                  -> ProcessCb -> parse frame -> ordered deliver
//   Update() -> reliable_endpoint_update + ack-carrier + ordered-stall sweep
//               + paced drain + ack-driven retransmit of unacked.
//
// This TU owns framing, ordering/dedup, the RC-level cumulative ack, pacing and
// retransmit. The FEC codec (parity encode on egress, group reassembly and RS
// recovery on ingress) lives in the sibling reliable_channel_fec.cpp; the state
// model both share is reliable_channel_internal.h.
#include "reliable_channel_internal.h"

#include <SDL3/SDL_log.h>   // ooo-buffer-full + stall/retire diagnostics
#include <cstring>
#include <cstdint>
#include <cstdlib>   // std::getenv -- FM2K_RC_STATS gate
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

void EnsureReliableInit() {
    static bool inited = false;
    if (!inited) { reliable_init(); inited = true; }
}

void DeliverOrdered(Endpoint* ep, uint8_t chan, RxChannel& rc,
                    uint64_t seq, const uint8_t* payload, int plen) {
    if (!rc.started) {
        // A fresh channel anchors at ZERO -- NOT at whatever seq happens to
        // arrive first. The old "mid-join: start here" anchor silently ate the
        // head of every burst: send msg 0 and msg 1 back to back, lose 0, and
        // msg 1 anchors the channel at 1. The ack carrier then advertises
        // next_deliver=1 as "cumulative delivered", which tells the SENDER to
        // erase msg 0 from its unacked map (DeliverOne's ack path) -- so the
        // retransmit that would have healed it never happens. Permanent,
        // silent, zero log output; for a join backfill that is PIN_RNG /
        // MATCH_START / RESET_INPUT_STATE / SOUND_INIT and the first ~1600
        // INPUTs gone while the viewer happily runs a battle it never got a
        // MATCH_START for.
        // The two cases ARE separable, and cwnd is what separates them: the
        // sender stalls at ~96 unacked messages, so it cannot be 256+ messages
        // into a stream with ZERO of them delivered. A first-seen seq below
        // that is therefore a stream at its beginning with a lost head (anchor
        // at 0 and let retransmit fill it); a first-seen seq at or above it can
        // only be a genuine mid-join -- OUR endpoint was recreated while the
        // peer's survived -- where anchoring at 0 would strand the channel
        // until the RC_ORDERED_STALL_SEC sweep.
        rc.started = true;
        rc.next_deliver = (seq >= RC_RESTART_BACKJUMP) ? seq : 0;
        rc.last_progress_time = ep->now_time;
    }
    if (seq < rc.next_deliver) {
        if (rc.next_deliver - seq < RC_RESTART_BACKJUMP) return;  // true duplicate
        // Sender restart: its endpoint was destroyed and recreated, so its
        // msg_seq restarted at 0 while our cursor stayed high. Every message of
        // the new stream would otherwise read as a duplicate, forever.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "ReliableChannel: chan=%u peer msg_seq jumped back %llu -> %llu "
            "(sender endpoint restarted) -- resetting ordered rx cursor",
            (unsigned)chan, (unsigned long long)rc.next_deliver,
            (unsigned long long)seq);
        rc.next_deliver = seq;
        rc.ooo.clear();
    }
    if (seq > rc.next_deliver && rc.stall_resync) {
        // The stall sweep armed a resync: this channel delivered nothing for
        // RC_ORDERED_STALL_SEC with an EMPTY reassembly buffer, so there was
        // no buffered successor to skip the hole to and the sweep could only
        // un-anchor. THIS message is the successor. Adopt it as the cursor
        // rather than buffer it behind a hole the sender has already retired
        // and will never send again. Only ever forward (seq > next_deliver);
        // a lower seq fell into the duplicate path above, so a late
        // retransmit can never rewind the stream.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[RC] ordered chan=%u RESYNC -- adopting msg_seq=%llu as the new "
            "cursor (was %llu; %llu message(s) permanently lost). The sender "
            "retired the hole and its tail, so nothing behind it was ever "
            "coming; consumer must resync from the app-level gap",
            (unsigned)chan, (unsigned long long)seq,
            (unsigned long long)rc.next_deliver,
            (unsigned long long)(seq - rc.next_deliver));
        rc.next_deliver = seq;
    }
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
        rc.last_progress_time = ep->now_time;
        rc.stall_resync       = false;   // progress -> the channel is healthy again
    } else if (rc.ooo.size() < RC_OOO_MAX) {
        rc.ooo.emplace(seq, std::vector<uint8_t>(payload, payload + plen));  // buffer, in order
    } else {
        // Reassembly buffer full: the gap at next_deliver isn't filling. Stop
        // hoarding memory -- drop this future message. reliable.io already
        // acked the packet, so the channel will now stall at the gap; the
        // consumer's stall watchdog re-JOINs and re-syncs. Bounded, not OOM.
        static uint32_t s_ooo_drop_log = 0;
        if ((s_ooo_drop_log++ & 0x3F) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "ReliableChannel: ooo buffer full (%zu) at next=%llu, dropping "
                "seq=%llu -- channel stalled, awaiting reconnect",
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
        // in order) -- NOT merely transport-received. This is why a corrupt packet
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
        // Sender restart, same reasoning as DeliverOrdered: a real duplicate is
        // always recent, so a far-below seq means the peer's endpoint was
        // recreated and its msg_seq restarted at 0. Without this the whole new
        // stream reads as duplicates and the blob channel goes permanently deaf.
        if (rc.un_next_ack > seq && rc.un_next_ack - seq >= RC_RESTART_BACKJUMP) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "ReliableChannel: chan=%u (unordered) peer msg_seq jumped back "
                "%llu -> %llu (sender endpoint restarted) -- resetting rx cursor",
                (unsigned)chan, (unsigned long long)rc.un_next_ack,
                (unsigned long long)seq);
            rc.un_next_ack = seq;
            rc.un_recv_above.clear();
        }
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

// How many UDP datagrams one reliable.io packet actually becomes, including FEC
// parity. THE PACER'S OLD "1 token per packet" WAS THE BUG: a 16KB snapshot
// chunk cost one token and emitted 17 fragment datagrams, so the configured
// 3000 pps was really ~51,000 datagrams/s (~53 MB/s offered) and the token
// bucket -- the entire reason a bulk burst can't self-inflict loss -- was a
// no-op for exactly the burst it existed to pace.
double WireCost(const Endpoint* ep, size_t packet_bytes) {
    double dgrams = 1.0;
    if (packet_bytes > RC_FRAGMENT_ABOVE) {
        dgrams = static_cast<double>((packet_bytes + RC_FRAGMENT_SIZE - 1) /
                                     RC_FRAGMENT_SIZE);
    }
    // FEC emits M parity datagrams per K data datagrams. They are real wire
    // cost and must be paced like everything else.
    if (ep->fec_enabled && !ep->fec_suppressed && ep->fec_k > 0)
        dgrams *= 1.0 + static_cast<double>(ep->fec_m) / static_cast<double>(ep->fec_k);
    return dgrams;
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
            const size_t framed_bytes = ps.framed.size();
            pkt.insert(pkt.end(), ps.framed.begin(), ps.framed.end());  // copy into packet
            if (reliable) {
                Unacked u; u.framed = std::move(ps.framed); u.sent_time = now;
                u.first_time = now; u.pkt_seq = 0;
                ep->tx[ps.chan].unacked.emplace(ps.msg_seq, std::move(u));  // per-msg retransmit
                inflight++;
            }
            ep->send_queue_bytes = (ep->send_queue_bytes >= framed_bytes)
                                       ? ep->send_queue_bytes - framed_bytes : 0;
            ep->send_queue.pop_front();
            if (pkt.size() >= RC_NAGLE_MAX) break;  // approx full (a lone big msg goes alone)
        }
        if (pkt.empty()) break;
        SendFramedNow(ep, pkt);  // one reliable.io packet (may fragment if > MTU)
        // Charge what actually goes on the wire: fragments + FEC parity. Tokens
        // may dip negative for a large packet; that is the point -- the next
        // pump waits it off rather than pretending the packet was free.
        ep->tokens -= WireCost(ep, pkt.size());
    }
}

}  // namespace

Endpoint* Create(uint64_t id, RawSendFn raw_send, void* raw_send_ctx,
                 DeliverFn deliver, void* deliver_ctx, double now) {
    EnsureReliableInit();
    Endpoint* ep = new Endpoint();
    ep->raw_send = raw_send; ep->raw_send_ctx = raw_send_ctx;
    ep->deliver = deliver;   ep->deliver_ctx = deliver_ctx;
    ep->id = id;
    reliable_config_t cfg;
    reliable_default_config(&cfg);
    reliable_copy_string(cfg.name, "rc", sizeof(cfg.name));
    // task #55 root cause: the DEFAULT envelope (max_packet_size 16KB,
    // max_fragments 16 x 1024B = 16KB) is SMALLER than a lone spectator
    // snapshot chunk (16384B payload + SpecDataHeader + RC framing).
    // reliable_endpoint_send_packet rejects oversize with a void return --
    // the chunk failed on the initial send AND on every retransmit, then
    // the 7s retirement silently erased it: the snapshot never completed
    // and the viewer simmed placeholder chars (RC-STATS TOO_LARGE=15 on
    // the failing run; intermittent because zero-RLE chunk sizes depend on
    // the captured battle state). 64KB headroom fits any framed message
    // this codebase produces with 4x margin.
    cfg.max_packet_size = static_cast<int>(RC_MAX_PACKET_BYTES);
    cfg.fragment_above  = static_cast<int>(RC_FRAGMENT_ABOVE);
    cfg.max_fragments   = 64;
    cfg.fragment_size   = static_cast<int>(RC_FRAGMENT_SIZE);
    cfg.fragment_reassembly_buffer_size = 256;
    cfg.context = ep;
    cfg.id = id;
    cfg.transmit_packet_function = &TransmitCb;
    cfg.process_packet_function  = &ProcessCb;
    ep->now_time = now;
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
    const size_t framed_bytes = RC_HDR_BYTES + static_cast<size_t>(len) + RC_CRC_BYTES;
    // Both rejections below happen BEFORE a msg_seq is allocated. The old code
    // burned the seq first and then returned, which punched a hole the peer's
    // ordered channel would wait on forever -- the very "silent permanent
    // stream gap" the guard was added to prevent.
    //
    // Envelope guard: reliable.io rejects oversize with a VOID return and our
    // retransmit would spin on it until the 7s retire (the task-#55 snapshot
    // bug). Never let an oversize message enter the pipeline quietly.
    if (framed_bytes > RC_MAX_PACKET_BYTES) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[RC] framed msg %zu B exceeds the 64KB envelope (chan=%u cls=%u) "
            "-- reliable.io would drop it silently; REJECTING at Send. "
            "Fragment the payload at the app layer.",
            framed_bytes, (unsigned)chan, (unsigned)cls);
        return;
    }
    // Memory bound: a host serving N spectators queues a whole snapshot per
    // viewer, and a viewer whose acks are black-holed stalls its endpoint on
    // cwnd while re-ships keep arriving. Loud, and recoverable -- the app-level
    // repair for a missing snapshot is a re-ship, not a transport retransmit.
    if (ep->send_queue_bytes + framed_bytes > RC_MAX_QUEUED_BYTES) {
        static double s_last_full_log = -1.0;
        if (s_last_full_log < 0.0 || ep->now_time - s_last_full_log >= 1.0) {
            s_last_full_log = ep->now_time;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[RC] send queue full: %zu B queued (%zu msgs), rejecting a %zu B "
                "msg on chan=%u -- peer is not draining; app-level re-ship must repair",
                ep->send_queue_bytes, ep->send_queue.size(), framed_bytes,
                (unsigned)chan);
        }
        return;
    }
    TxChannel& tc = ep->tx[chan];
    PendingSend ps;
    ps.chan = chan;
    ps.cls = static_cast<uint8_t>(cls);
    ps.msg_seq = tc.next_msg_seq++;
    BuildFramed(chan, cls, ps.msg_seq, data, len, ps.framed);
    ep->send_queue_bytes += ps.framed.size();
    ep->send_queue.push_back(std::move(ps));  // paced out in Update()
}

void Update(Endpoint* ep, double now) {
    if (!ep || !ep->rel) return;
    ep->now_time = now;   // DeliverOrdered timestamps progress off this
    reliable_endpoint_update(ep->rel, now);

    // FM2K_RC_STATS=1: 1Hz per-endpoint stats dump (task #55 snapshot-over-RC
    // diagnosis). Shows whether a stalled bulk transfer is a SEND-side drop
    // (too_large counter), a fragment casualty (frags_invalid), or a
    // retransmit livelock (unacked count pinned + oldest age growing).
    {
        static const bool s_rc_stats = []{
            const char* v = std::getenv("FM2K_RC_STATS");
            return v && v[0] == '1';
        }();
        if (s_rc_stats && now - ep->last_stats_log >= 1.0) {
            ep->last_stats_log = now;
            size_t unacked_total = 0, queue = ep->send_queue.size();
            double oldest = 0.0;
            for (auto& [chan, tc] : ep->tx) {
                unacked_total += tc.unacked.size();
                for (auto& [seq, u] : tc.unacked) {
                    if (now - u.first_time > oldest) oldest = now - u.first_time;
                }
            }
            const uint64_t* c = reliable_endpoint_counters(ep->rel);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[RC-STATS] ep=%llu q=%zu qb=%zu fec_supp=%d unacked=%zu oldest=%.1fs | "
                "sent=%llu recv=%llu acked=%llu stale=%llu invalid=%llu "
                "TOO_LARGE=%llu frag_sent=%llu frag_recv=%llu FRAG_INVALID=%llu",
                (unsigned long long)ep->id, queue, ep->send_queue_bytes,
                (int)ep->fec_suppressed, unacked_total, oldest,
                (unsigned long long)c[0], (unsigned long long)c[1],
                (unsigned long long)c[2], (unsigned long long)c[3],
                (unsigned long long)c[4], (unsigned long long)c[5],
                (unsigned long long)c[7], (unsigned long long)c[8],
                (unsigned long long)c[9]);
        }
    }

    // 0) ack-carrier: periodically send our RC-level cumulative ack table so the
    // peer can clear/retransmit precisely on what we've DELIVERED. Sent DIRECTLY
    // (not queued) so acks always flow -- never blocked by pacing or a cwnd-stalled
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
    // ProcessCb), NOT reliable.io transport acks -- because reliable.io acks a
    // corrupt-but-received packet, which must NOT count as delivered. We still
    // drain reliable.io's ack list so it doesn't leak (it feeds RTT internally).
    int num_acks = 0;
    (void)reliable_endpoint_get_acks(ep->rel, &num_acks);
    if (num_acks > 0) reliable_endpoint_clear_acks(ep->rel);

    // 1a) FEC suppression under SUSTAINED send-queue backpressure.
    //
    // This was `if (send_queue.size() > 20) suppress for 3s` -- a MESSAGE COUNT
    // threshold, inherited from a reference engine whose messages are uniformly
    // small. It disabled parity for exactly the one payload on this link that
    // needs erasure coding: a snapshot has always been 100+ queued messages
    // (and is now ~1130 of them, one datagram each), so parity was off for the
    // whole transfer plus 3s of hangover, every time.
    //
    // Two corrections:
    //   * measure BYTES -- a count is meaningless once message sizes vary by
    //     16x, and bytes are what the link actually cares about;
    //   * require the backlog to PERSIST. A bulk burst is offered all at once
    //     and drains at the paced rate in a fraction of a second; that is
    //     healthy and must keep its parity. Only a queue that stays deep (the
    //     peer has stopped acking, so cwnd has stalled the drain) is the
    //     "parity is wasted bandwidth" case suppression exists for -- and in
    //     that state parity is not what is failing anyway.
    if (ep->send_queue_bytes > RC_FEC_SUPPRESS_BYTES) {
        if (ep->fec_backlog_since < 0.0) ep->fec_backlog_since = now;
        if (now - ep->fec_backlog_since >= RC_FEC_SUPPRESS_SUSTAIN_SEC)
            ep->fec_suppress_until = now + RC_FEC_SUPPRESS_SEC;
    } else {
        ep->fec_backlog_since = -1.0;
    }
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

    // 1a3) ordered-channel stall recovery. A message the sender retired at 7s
    // (below) will NEVER be sent again, so a receiver waiting on that hole
    // waits forever: next_deliver pins, every later message piles into `ooo`
    // until RC_OOO_MAX and is then dropped, and RC state is per-endpoint so an
    // app-level re-JOIN repairs it only if that re-JOIN explicitly resets the
    // endpoint (ReliableChannel_ResetPeer -- nothing did until the spectator
    // starve of 2026-08-07 proved the retire log's "permanent gap until
    // re-JOIN" was a promise re-JOIN could not keep). Past the stall horizon,
    // repair LOUDLY -- a visible gap the consumer can resync from beats a
    // silently dead channel.
    //
    // TWO SHAPES, and requiring `!ooo.empty()` made the repair unreachable in
    // exactly the one that motivated it:
    //   * BUFFERED TAIL -- something behind the hole did arrive, so we know
    //     where the stream resumes: skip to it and drain.
    //   * EMPTY TAIL -- nothing behind the hole is buffered. This is NOT the
    //     rare case: the receiver's pinned cumulative ack is what stops the
    //     SENDER clearing anything, so once the hole ages out the whole tail
    //     ages out with it and never re-ships. There is no successor to name,
    //     so arm stall_resync and let the next message to arrive adopt itself
    //     as the cursor (DeliverOrdered). Also reached by a merely IDLE
    //     ordered channel, where the arm is a no-op: the next message arrives
    //     at exactly next_deliver, takes the ordinary in-order path, and
    //     clears the arm without ever adopting.
    for (auto& [rxchan, rc] : ep->rx) {
        if (!rc.cls_ordered || !rc.started) continue;
        if (rc.last_progress_time < 0.0) { rc.last_progress_time = now; continue; }
        if (now - rc.last_progress_time < RC_ORDERED_STALL_SEC) continue;
        if (rc.ooo.empty()) {
            // One line per stall episode: the arm latches until a delivery
            // clears it, and last_progress_time is refreshed so a channel
            // that stays quiet does not re-log every horizon.
            if (!rc.stall_resync) {
                rc.stall_resync = true;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[RC] ordered chan=%u delivered nothing for %.1fs at "
                    "msg_seq=%llu with NOTHING buffered -- the sender retired "
                    "the hole AND its tail (or the channel is simply idle). "
                    "UN-ANCHORING: the next message to arrive adopts itself as "
                    "the cursor instead of waiting on a hole that can never "
                    "fill",
                    (unsigned)rxchan, now - rc.last_progress_time,
                    (unsigned long long)rc.next_deliver);
            }
            rc.last_progress_time = now;
            continue;
        }
        const uint64_t skip_to = rc.ooo.begin()->first;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[RC] ordered chan=%u stalled %.1fs at msg_seq=%llu with %zu buffered "
            "-- the sender has retired that message; SKIPPING to %llu "
            "(permanent gap, consumer must resync)",
            (unsigned)rxchan, now - rc.last_progress_time,
            (unsigned long long)rc.next_deliver, rc.ooo.size(),
            (unsigned long long)skip_to);
        rc.next_deliver = skip_to;
        rc.last_progress_time = now;
        rc.stall_resync       = false;   // we found the successor ourselves
        auto it = rc.ooo.find(rc.next_deliver);
        while (it != rc.ooo.end()) {
            if (ep->deliver) ep->deliver(ep->deliver_ctx, rxchan,
                                         it->second.data(),
                                         static_cast<int>(it->second.size()));
            rc.ooo.erase(it);
            rc.next_deliver++;
            it = rc.ooo.find(rc.next_deliver);
        }
    }

    // 1b) drain the paced send queue (token bucket rate + cwnd backpressure).
    PumpSendQueue(ep, now);

    // 2) retransmit reliable msgs unacked for too long. Timeout = clamp(2*RTT,
    // 80ms, 750ms) (matches the reference engine's reference reliable peer). Messages unacked for
    // 7s are retired (connection stuck) -- the consumer's stall watchdog re-JOINs.
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
            if (now - u.first_time >= 7.0) {
                // Retiring a RELIABLE message is a delivery-contract break. It
                // is survivable because both sides now have a repair: an
                // ORDERED channel skips the hole after RC_ORDERED_STALL_SEC
                // (see 1a3) instead of pinning forever, and a retired snapshot
                // chunk on the UNORDERED blob channel is re-supplied by the
                // host's rate-limited re-ship, which the viewer's BEGIN-continue
                // rule now folds into the transfer already in progress instead
                // of restarting it.
                // The TTL itself is deliberately NOT channel-aware: at one
                // datagram per chunk, 7s is ~9+ independent retransmit attempts
                // at ~80% each, so a chunk essentially never reaches the retire
                // horizon unless the link is genuinely dead -- and extending the
                // TTL for a dead link would keep 1000+ messages resident and
                // retransmitting, which is the congestion collapse this whole
                // change set exists to remove.
                // It must never be silent again (the silent retire hid the
                // oversize-chunk drop for weeks).
                static double s_last_retire_log = 0.0;
                if (now - s_last_retire_log >= 1.0) {
                    s_last_retire_log = now;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[RC] RETIRED reliable msg seq=%llu chan=%u after 7s "
                        "undelivered (%zu bytes) -- permanent gap. The RECEIVER "
                        "repairs it at %.1fs (ordered-stall skip/resync); a bare "
                        "app-level re-JOIN does NOT, because RC state is "
                        "per-endpoint and survives it -- only "
                        "ReliableChannel_ResetPeer clears this endpoint",
                        (unsigned long long)it->first, (unsigned)tkv.first,
                        u.framed.size(), RC_ORDERED_STALL_SEC);
                }
                it = un.erase(it);
                continue;
            }
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
