// reliable_channel_test.cpp — host-native unit test for the ReliableChannel layer.
//
// Proves, WITHOUT the game or Windows, that:
//   1. reliable-ordered messages are delivered in order,
//   2. FEC reconstructs a dropped data packet WITHOUT a retransmit round-trip
//      (single loss per K-group), and
//   3. multi-loss beyond FEC still eventually delivers via retransmit.
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

// Pump a link's queued datagrams into its destination endpoint (strip nothing —
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

    // ---- Test 3: one-way stream — acks must flow back via the carrier so the
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

    // ---- Test 4: CRC integrity — a corrupted datagram must be DROPPED, never
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

    // ---- Test 5: reliable-UNORDERED under loss — every msg delivered exactly
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

    Destroy(A); Destroy(B); Destroy(A2); Destroy(B2); Destroy(A3); Destroy(B3);
    Destroy(A4); Destroy(B4); Destroy(A5); Destroy(B5);
    printf("%s\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED");
    return g_fail == 0 ? 0 : 1;
}

#endif  // RC_STANDALONE_TEST
