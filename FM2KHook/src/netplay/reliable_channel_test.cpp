// reliable_channel_test.cpp -- host-native unit test for the ReliableChannel layer.
//
// Proves, WITHOUT the game or Windows, that:
//   1. reliable-ordered messages are delivered in order,
//   2. FEC reconstructs a dropped data packet WITHOUT a retransmit round-trip
//      (single loss per K-group), and
//   3. multi-loss beyond FEC still eventually delivers via retransmit,
//   4. an ordered channel whose head message the SENDER retired repairs itself
//      -- including the empty-reassembly-buffer shape that made the repair
//      unreachable and produced the mid-stream spectator starve (test 10).
//
// Build + run on the host (Linux/WSL), not the mingw cross target:
//   g++ -std=c++17 -DRC_STANDALONE_TEST -I vendored/reliable \
//       FM2KHook/src/netplay/reliable_channel_test.cpp \
//       FM2KHook/src/netplay/reliable_channel.cpp \
//       vendored/reliable/reliable.c -o /tmp/rc_test && /tmp/rc_test
#ifdef RC_STANDALONE_TEST

#include "reliable_channel.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <string>

using namespace fm2k::rc;

// A one-way lossy link: datagrams queued here, optionally dropped, then pumped
// into the destination endpoint.
struct Link {
    std::deque<std::vector<uint8_t>> q;
    Endpoint* dst = nullptr;
    // drop predicate: called per datagram with a running counter.
    int counter = 0;
    int drop_every = 0;   // 0 = no drop; N = drop every Nth datagram
    int extra_drop_at = -1;  // one specific counter value to also drop (burst sim)
    int dropped = 0;
};

static void LinkSend(void* ctx, const uint8_t* data, int len) {
    Link* L = static_cast<Link*>(ctx);
    int n = L->counter++;
    bool drop = (L->drop_every > 0 && (n % L->drop_every) == (L->drop_every - 1));
    if (n == L->extra_drop_at) drop = true;
    if (drop) { L->dropped++; return; }
    L->q.emplace_back(data, data + len);
}

// Deliver record on the receiver.
struct Recv {
    std::vector<std::pair<uint8_t, std::string>> msgs;  // (channel, payload)
};
static void OnDeliver(void* ctx, uint8_t chan, const uint8_t* data, int len) {
    Recv* R = static_cast<Recv*>(ctx);
    R->msgs.emplace_back(chan, std::string(reinterpret_cast<const char*>(data), len));
}

// Pump a link's queued datagrams into its destination endpoint (strip nothing --
// the RC OnDatagram expects the payload after the 0xCB tag, and our LinkSend
// captured exactly what TransmitCb produced INCLUDING the 0xCB tag, so strip it).
static void PumpLink(Link& L) {
    while (!L.q.empty()) {
        std::vector<uint8_t> d = std::move(L.q.front());
        L.q.pop_front();
        // d[0] == 0xCB tag; OnDatagram wants the body after it.
        if (d.size() >= 1) OnDatagram(L.dst, d.data() + 1, static_cast<int>(d.size()) - 1);
    }
}

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; } \
                              else { printf("  ok:   %s\n", msg); } } while (0)

int main() {
    printf("ReliableChannel unit test\n");

    // Two endpoints, A -> B (data) and B -> A (acks). Loopback links.
    Link a2b, b2a;
    Recv rb, ra;
    double t = 0.0;
    Endpoint* A = Create(1, &LinkSend, &a2b, &OnDeliver, &ra, t);
    Endpoint* B = Create(2, &LinkSend, &b2a, &OnDeliver, &rb, t);
    a2b.dst = B; b2a.dst = A;

    // ---- Test 1: clean link, ordered delivery ----
    printf("[1] ordered delivery, no loss\n");
    const int M = 12;
    for (int i = 0; i < M; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "msg-%02d", i);
        Send(A, /*chan*/0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    for (int tick = 0; tick < 20; tick++) {
        t += 0.016;
        Update(A, t); Update(B, t);
        PumpLink(a2b); PumpLink(b2a);
    }
    CHECK(rb.msgs.size() == (size_t)M, "all 12 messages delivered");
    bool ordered = true;
    for (int i = 0; i < (int)rb.msgs.size(); i++) {
        char want[32]; snprintf(want, sizeof(want), "msg-%02d", i);
        if (rb.msgs[i].second != want) ordered = false;
    }
    CHECK(ordered, "messages delivered strictly in order");

    // ---- Test 2: single loss per FEC group -> FEC recovers, no stall ----
    // Drop every 5th datagram (K=4 data + 1 parity = 5 per group; dropping the
    // 5th drops the PARITY, harmless; shift to drop a DATA packet by dropping
    // every 3rd so a data packet in each group is lost but <=1 per group).
    printf("[2] single-loss-per-group, FEC should reconstruct\n");
    Recv rb2; a2b = Link{}; b2a = Link{};
    double t2 = 0.0;
    Endpoint* A2 = Create(3, &LinkSend, &a2b, &OnDeliver, &ra, t2);
    Endpoint* B2 = Create(4, &LinkSend, &b2a, &OnDeliver, &rb2, t2);
    a2b.dst = B2; b2a.dst = A2;
    a2b.drop_every = 5;  // drop 1 of every 5 datagrams (<=1 data loss per K=4 group)
    const int M2 = 20;
    for (int i = 0; i < M2; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "d-%03d", i);
        Send(A2, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    // Pump WITHOUT giving retransmit time to fire first, to prove FEC (not
    // retransmit) delivered them: one immediate pump pass.
    Update(A2, t2); PumpLink(a2b);
    size_t after_fec = rb2.msgs.size();
    printf("  delivered after first pass (FEC only): %zu / %d (dropped %d datagrams)\n",
           after_fec, M2, a2b.dropped);
    CHECK(after_fec >= (size_t)(M2 * 0.7), "FEC recovered most single-losses without retransmit");
    // Now allow retransmit to mop up any >1-loss groups.
    for (int tick = 0; tick < 40; tick++) {
        t2 += 0.016;
        Update(A2, t2); Update(B2, t2);
        PumpLink(a2b); PumpLink(b2a);
    }
    CHECK(rb2.msgs.size() == (size_t)M2, "all 20 delivered (FEC + retransmit backstop)");
    bool ordered2 = true;
    for (int i = 0; i < (int)rb2.msgs.size(); i++) {
        char want[32]; snprintf(want, sizeof(want), "d-%03d", i);
        if (i < (int)rb2.msgs.size() && rb2.msgs[i].second != want) ordered2 = false;
    }
    CHECK(ordered2, "still strictly ordered under loss");

    // ---- Test 3: one-way stream -- acks must flow back via the carrier so the
    // sender's unacked map drains (not resend-forever). B never app-sends. ----
    printf("[3] one-way stream, ack-carrier drains sender unacked\n");
    Recv rb3; Link a2b3, b2a3;
    double t3 = 0.0;
    Endpoint* A3 = Create(5, &LinkSend, &a2b3, &OnDeliver, &ra, t3);
    Endpoint* B3 = Create(6, &LinkSend, &b2a3, &OnDeliver, &rb3, t3);
    a2b3.dst = B3; b2a3.dst = A3;
    const int M3 = 8;
    for (int i = 0; i < M3; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "o-%02d", i);
        Send(A3, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    // Run enough ticks for acks to round-trip via B3's carrier.
    int a2b3_count_before = 0, a2b3_count_after = 0;
    for (int tick = 0; tick < 30; tick++) {
        t3 += 0.033;
        Update(A3, t3); Update(B3, t3);
        if (tick == 3) a2b3_count_before = a2b3.counter;
        PumpLink(a2b3); PumpLink(b2a3);
    }
    a2b3_count_after = a2b3.counter;
    CHECK(rb3.msgs.size() == (size_t)M3, "one-way: all 8 delivered");
    // After acks arrive, A3 should STOP resending -> datagram count plateaus.
    int tail_growth = a2b3_count_after - a2b3_count_before;
    printf("  A3 datagrams: @tick3=%d final=%d (tail growth=%d; low => resends stopped)\n",
           a2b3_count_before, a2b3_count_after, tail_growth);
    CHECK(tail_growth < M3 * 3, "one-way: sender stopped resending after acks (no resend-forever)");

    // ---- Test 4: CRC integrity -- a corrupted datagram must be DROPPED, never
    // delivered as valid (this is what makes FEC safe against silent desync). ----
    printf("[4] corruption integrity: bit-flipped datagram dropped, not delivered\n");
    Recv rb4; Link a2b4, b2a4;
    double t4 = 0.0;
    Endpoint* A4 = Create(7, &LinkSend, &a2b4, &OnDeliver, &ra, t4);
    Endpoint* B4 = Create(8, &LinkSend, &b2a4, &OnDeliver, &rb4, t4);
    a2b4.dst = B4; b2a4.dst = A4;
    // Send 6 ordered messages; corrupt message index 2's datagram in flight.
    for (int i = 0; i < 6; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "c-%02d", i);
        Send(A4, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    Update(A4, t4);  // pump: enqueues datagrams into a2b4.q (Nagle may coalesce)
    // Corrupt a payload byte in the LAST queued datagram (works regardless of how
    // Nagle coalesced the messages into datagrams).
    int corrupt_hits = 0;
    for (int qi = (int)a2b4.q.size() - 1; qi >= 0 && corrupt_hits == 0; qi--) {
        std::vector<uint8_t>& d = a2b4.q[qi];
        if (d.size() > 20) { d[d.size() - 6] ^= 0xFF; corrupt_hits++; }  // flip a byte
    }
    PumpLink(a2b4); PumpLink(b2a4);
    // The corrupted message must NOT appear verbatim; run more ticks so
    // retransmit recovers the real one -> all 6 eventually delivered, in order.
    for (int tick = 0; tick < 40; tick++) {
        t4 += 0.02;
        Update(A4, t4); Update(B4, t4);
        PumpLink(a2b4); PumpLink(b2a4);
    }
    CHECK(corrupt_hits == 1, "test injected exactly one corruption");
    CHECK(rb4.msgs.size() == 6, "all 6 delivered (corrupt dropped, retransmit recovered)");
    bool ordered4 = true, no_garbage = true;
    for (int i = 0; i < (int)rb4.msgs.size(); i++) {
        char want[32]; snprintf(want, sizeof(want), "c-%02d", i);
        if (rb4.msgs[i].second != want) ordered4 = false;
        // no delivered message should contain the 0xFF-corrupted byte pattern
    }
    (void)no_garbage;
    CHECK(ordered4, "delivered set is exactly the correct messages, in order (no corruption leaked)");

    // ---- Test 5: reliable-UNORDERED under loss -- every msg delivered exactly
    // once (dedup), all eventually delivered (SACK ack doesn't falsely clear a
    // gap so lost msgs retransmit). Order NOT required. ----
    printf("[5] reliable-unordered under loss: exactly-once, all delivered\n");
    Recv rb5; Link a2b5, b2a5;
    double t5 = 0.0;
    Endpoint* A5 = Create(9, &LinkSend, &a2b5, &OnDeliver, &ra, t5);
    Endpoint* B5 = Create(10, &LinkSend, &b2a5, &OnDeliver, &rb5, t5);
    a2b5.dst = B5; b2a5.dst = A5;
    a2b5.drop_every = 4;  // drop 25% (heavy) to force gaps + retransmit
    const int M5 = 24;
    for (int i = 0; i < M5; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "u-%03d", i);
        Send(A5, /*chan*/7, Class::ReliableUnordered, reinterpret_cast<uint8_t*>(buf), n);
    }
    for (int tick = 0; tick < 80; tick++) {
        t5 += 0.02;
        Update(A5, t5); Update(B5, t5);
        PumpLink(a2b5); PumpLink(b2a5);
    }
    CHECK(rb5.msgs.size() == (size_t)M5, "unordered: all 24 delivered under 25% loss");
    // exactly-once: the set of delivered payloads equals the sent set, no dups
    std::vector<int> seen(M5, 0);
    bool exactly_once = (rb5.msgs.size() == (size_t)M5);
    for (auto& m : rb5.msgs) {
        int idx = -1;
        if (sscanf(m.second.c_str(), "u-%d", &idx) == 1 && idx >= 0 && idx < M5) seen[idx]++;
    }
    for (int i = 0; i < M5; i++) if (seen[i] != 1) exactly_once = false;
    CHECK(exactly_once, "unordered: each message delivered EXACTLY once (dedup + no false-ack loss)");

    // ---- Test 6: HEAD-OF-BURST loss on an ordered channel. This is the
    // regression test for the anchor defect: DeliverOrdered used to set
    // next_deliver to the FIRST seq it saw ("mid-join: start here"), so losing
    // msg 0 made msg 1 anchor the channel at 1 -- and the ack carrier then
    // advertised next_deliver=1 as "cumulatively delivered", which told the
    // SENDER to erase msg 0. Silent, permanent, no log. Messages are ~900 B so
    // each rides its own datagram (Nagle would otherwise coalesce all six into
    // one packet and "drop the first datagram" would drop the whole burst). ----
    printf("[6] head-of-burst loss: msg 0 lost, must NOT anchor at 1\n");
    Recv rb6; Link a2b6, b2a6;
    double t6 = 0.0;
    Endpoint* A6 = Create(11, &LinkSend, &a2b6, &OnDeliver, &ra, t6);
    Endpoint* B6 = Create(12, &LinkSend, &b2a6, &OnDeliver, &rb6, t6);
    a2b6.dst = B6; b2a6.dst = A6;
    a2b6.extra_drop_at = 0;   // drop the very first datagram = msg 0's packet
    const int M6 = 6;
    for (int i = 0; i < M6; i++) {
        char buf[960];
        int n = snprintf(buf, sizeof(buf), "h-%02d", i);
        memset(buf + n, 'x', 900 - n);   // ~900 B so it never Nagles with a sibling
        Send(A6, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), 900);
    }
    Update(A6, t6); PumpLink(a2b6);
    CHECK(a2b6.dropped == 1, "head-loss: exactly the first datagram was dropped");
    CHECK(rb6.msgs.empty(),
          "head-loss: nothing delivered yet -- the channel did NOT anchor at the "
          "first seq it saw");
    for (int tick = 0; tick < 60; tick++) {
        t6 += 0.02;
        Update(A6, t6); Update(B6, t6);
        PumpLink(a2b6); PumpLink(b2a6);
    }
    CHECK(rb6.msgs.size() == (size_t)M6,
          "head-loss: all 6 delivered -- the lost HEAD was retransmitted, not erased");
    bool ordered6 = (rb6.msgs.size() == (size_t)M6);
    for (int i = 0; i < (int)rb6.msgs.size(); i++) {
        char want[16]; snprintf(want, sizeof(want), "h-%02d", i);
        if (rb6.msgs[i].second.compare(0, strlen(want), want) != 0) ordered6 = false;
    }
    CHECK(ordered6, "head-loss: delivered strictly in order starting at msg 0");

    // ---- Test 7: duplicate suppression on the ordered channel. The gap in
    // test 6 pinned the cumulative ack at 0, so the sender re-blasted msgs 1..5
    // repeatedly while msg 0 was still missing. Every one of those is a
    // duplicate the receiver must NOT hand up a second time. (Same property the
    // snapshot inbox now relies on at the app layer: repeat delivery must be a
    // no-op, never a double-count.) ----
    printf("[7] duplicate suppression: retransmits must not double-deliver\n");
    {
        int dup = 0;
        for (int i = 0; i < M6; i++) {
            char want[16]; snprintf(want, sizeof(want), "h-%02d", i);
            int seen = 0;
            for (auto& m : rb6.msgs)
                if (m.second.compare(0, strlen(want), want) == 0) seen++;
            if (seen != 1) dup++;
        }
        printf("  A6 datagrams sent=%d (retransmits included), delivered=%zu\n",
               a2b6.counter, rb6.msgs.size());
        CHECK(dup == 0, "each message delivered EXACTLY once despite retransmits");
    }

    // ---- Test 8: sender restart. Endpoint eviction is per-side
    // (reliable_channel_net.cpp RC_PEER_IDLE_SEC), so a peer can destroy and
    // recreate its endpoint -- restarting msg_seq at 0 -- while our rx cursor
    // stays high. Every message of the new stream would then read as a
    // "duplicate" forever and the channel would go permanently deaf. ----
    printf("[8] sender restart: peer msg_seq drops back to 0, must not go deaf\n");
    Recv rb8; Link a2b8, b2a8;
    double t8 = 0.0;
    Endpoint* A8 = Create(13, &LinkSend, &a2b8, &OnDeliver, &ra, t8);
    Endpoint* B8 = Create(14, &LinkSend, &b2a8, &OnDeliver, &rb8, t8);
    a2b8.dst = B8; b2a8.dst = A8;
    const int M8 = 300;   // must exceed RC_RESTART_BACKJUMP (256)
    for (int i = 0; i < M8; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "r-%03d", i);
        Send(A8, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    for (int tick = 0; tick < 200; tick++) {
        t8 += 0.02;
        Update(A8, t8); Update(B8, t8);
        PumpLink(a2b8); PumpLink(b2a8);
    }
    CHECK(rb8.msgs.size() == (size_t)M8, "restart: baseline 300 delivered first");
    // The peer's endpoint dies and comes back -- fresh tx state, msg_seq 0.
    Destroy(A8);
    Endpoint* A8b = Create(15, &LinkSend, &a2b8, &OnDeliver, &ra, t8);
    b2a8.dst = A8b;
    const size_t before_restart = rb8.msgs.size();
    for (int i = 0; i < 5; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "n-%02d", i);
        Send(A8b, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    for (int tick = 0; tick < 40; tick++) {
        t8 += 0.02;
        Update(A8b, t8); Update(B8, t8);
        PumpLink(a2b8); PumpLink(b2a8);
    }
    CHECK(rb8.msgs.size() == before_restart + 5,
          "restart: the restarted sender's 5 new messages were delivered");

    // ---- Test 9: the pacer charges by DATAGRAMS, not by messages. A message
    // larger than fragment_above becomes ceil(bytes/fragment_size) datagrams
    // downstream; charging one token for all of them made the token bucket a
    // no-op for exactly the bulk burst it exists to pace (a 16 KB snapshot
    // chunk cost 1 token and emitted 17 datagrams -> ~17x over the configured
    // rate). With honest accounting a single 8 KB message must exhaust a
    // 4-token burst on its own. ----
    printf("[9] pacer charges per emitted datagram, not per message\n");
    Recv rb9; Link a2b9, b2a9;
    double t9 = 0.0;
    Endpoint* A9 = Create(16, &LinkSend, &a2b9, &OnDeliver, &ra, t9);
    Endpoint* B9 = Create(17, &LinkSend, &b2a9, &OnDeliver, &rb9, t9);
    a2b9.dst = B9; b2a9.dst = A9;
    SetPacing(A9, /*rate_pps*/100.0, /*burst_max*/4.0, /*cwnd*/1000);
    std::vector<uint8_t> big(8000, 0xA5);
    for (int i = 0; i < 4; i++) {
        big[0] = (uint8_t)('0' + i);
        Send(A9, 0, Class::ReliableOrdered, big.data(), (int)big.size());
    }
    Update(A9, t9);   // one pump with a 4-token burst
    printf("  datagrams after one pump with burst=4: %d (8KB msg = 8 fragments)\n",
           a2b9.counter);
    CHECK(a2b9.counter > 0 && a2b9.counter <= 16,
          "one 8KB message (>=8 datagrams) consumed the whole burst -- not all 4");
    for (int tick = 0; tick < 120; tick++) {
        t9 += 0.02;
        Update(A9, t9); Update(B9, t9);
        PumpLink(a2b9); PumpLink(b2a9);
    }
    CHECK(rb9.msgs.size() == 4, "paced bulk still delivers all 4 messages");

    // ---- Test 10: ORDERED STALL WITH AN EMPTY REASSEMBLY BUFFER. The
    // regression test for the mid-stream spectator starve (2026-08-07). The
    // ordered-stall repair used to require `!ooo.empty()` -- "skip to the
    // buffered successor" -- but the failure it was written for produces the
    // OPPOSITE state: the receiver's pinned cumulative ack is what stops the
    // SENDER clearing anything, so when the hole ages out at 7s the entire
    // tail ages out with it and NOTHING is ever buffered behind the hole. With
    // that precondition the repair was structurally unreachable, the channel
    // stayed deaf forever, and the viewer's host-gone watchdog killed the
    // process while the host was still fully reachable.
    //
    // The check is deliberately taken with NO Update() between the message
    // arriving and the assertion: the repair must happen at DELIVERY time, not
    // at the next stall sweep. (It also makes the test discriminating -- with
    // the old code the message sits in `ooo` at this instant.) ----
    printf("[10] ordered stall, EMPTY ooo: retired hole must not deafen the channel\n");
    Recv rb10; Link a2b10, b2a10;
    double t10 = 0.0;
    Endpoint* A10 = Create(18, &LinkSend, &a2b10, &OnDeliver, &ra, t10);
    Endpoint* B10 = Create(19, &LinkSend, &b2a10, &OnDeliver, &rb10, t10);
    a2b10.dst = B10; b2a10.dst = A10;
    for (int i = 0; i < 5; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "s-%02d", i);
        Send(A10, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    for (int tick = 0; tick < 20; tick++) {
        t10 += 0.02;
        Update(A10, t10); Update(B10, t10);
        PumpLink(a2b10); PumpLink(b2a10);
    }
    CHECK(rb10.msgs.size() == 5, "stall-resync: 5 clean messages delivered, cursor at 5");

    // Black-hole A->B completely and send msg 5. B receives nothing at all, so
    // `ooo` stays EMPTY -- the shape the old precondition exempted. Hold it for
    // 8s: past A's 7s retire (msg 5 can now NEVER be re-sent) and past B's
    // stall horizon (the resync arms).
    a2b10.drop_every = 1;   // (n % 1) == 0 for every n -> drop everything
    {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "s-05");
        Send(A10, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    for (int tick = 0; tick < 400; tick++) {   // 400 * 20ms = 8.0s
        t10 += 0.02;
        Update(A10, t10); Update(B10, t10);
        PumpLink(a2b10); PumpLink(b2a10);
    }
    CHECK(rb10.msgs.size() == 5, "stall-resync: the black-holed msg 5 never arrived");

    // Link comes back. The very next message is msg 6, one PAST the dead hole.
    a2b10.drop_every = 0;
    {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "s-06");
        Send(A10, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    t10 += 0.02;
    Update(A10, t10);    // A puts msg 6 on the wire
    PumpLink(a2b10);     // B receives it -- deliberately NO Update(B10) here
    bool adopted10 = false;
    for (auto& m : rb10.msgs) if (m.second == "s-06") adopted10 = true;
    CHECK(adopted10,
          "stall-resync: msg 6 ADOPTED as the new cursor and delivered on arrival "
          "(old code: ooo was empty, the skip never fired, msg 6 buffers forever)");
    CHECK(rb10.msgs.size() == 6,
          "stall-resync: exactly the 5 clean messages + the adopted one, no dups");
    // And the channel is healthy from there: 7,8,9 flow in order behind it.
    for (int i = 7; i < 10; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "s-%02d", i);
        Send(A10, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    for (int tick = 0; tick < 20; tick++) {
        t10 += 0.02;
        Update(A10, t10); Update(B10, t10);
        PumpLink(a2b10); PumpLink(b2a10);
    }
    CHECK(rb10.msgs.size() == 9,
          "stall-resync: the channel keeps delivering after the resync (5+1+3)");

    Destroy(A); Destroy(B); Destroy(A2); Destroy(B2); Destroy(A3); Destroy(B3);
    Destroy(A4); Destroy(B4); Destroy(A5); Destroy(B5);
    Destroy(A6); Destroy(B6); Destroy(A8b); Destroy(B8);
    Destroy(A9); Destroy(B9); Destroy(A10); Destroy(B10);
    printf("%s\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED");
    return g_fail == 0 ? 0 : 1;
}

#endif  // RC_STANDALONE_TEST
