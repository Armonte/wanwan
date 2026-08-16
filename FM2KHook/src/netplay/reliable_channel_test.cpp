// reliable_channel_test.cpp -- host-native unit test for the ReliableChannel layer.
//
// Proves, WITHOUT the game or Windows, that:
//   1. reliable-ordered messages are delivered in order,
//   2. FEC reconstructs a dropped data packet WITHOUT a retransmit round-trip
//      (single loss per K-group), and
//   3. multi-loss beyond FEC still eventually delivers via retransmit,
//   4. an ordered channel whose head message the SENDER retired repairs itself
//      -- including the empty-reassembly-buffer shape that made the repair
//      unreachable and produced the mid-stream spectator starve (test 10),
//   5. a message is NOT retired while the peer is provably alive, so a one-way
//      outage longer than the TTL costs latency and not content (test 11),
//   6. retention is still bounded -- a peer that is alive but can never receive
//      has its backlog retired at the hard ceiling (test 12),
//   7. the extended ack carrier is readable by a pre-fix parser and a pre-fix
//      carrier infers no advertisement (test 13).
//
// Tests 11-13 map onto the implementation spec's tests 11, 13 and 14. The
// spec's separate test 12 ("the dead-link contract must not regress") is not a
// separate test here: re-authoring test 10 to a both-directions blackout IS
// that test, and adding a duplicate would just be the same assertions twice.
//
// Tests 14-20 live in reliable_channel_liveness_test.cpp (same binary, same
// g_fail): the positive-hold triangle, the sticky hard backstop, the real
// builder against a pre-fix parser, hostile carrier bodies and the unacked byte
// cap. They were added 2026-08-16 after adversarial review found that half of
// the S0-S3 design had never executed at any level. The link model and the
// delivery recorder are shared through reliable_channel_test_link.h -- one
// definition, because both TUs link into one binary.
//
// Build + run on the host (Linux/WSL), not the mingw cross target:
//   g++ -std=c++17 -DRC_STANDALONE_TEST -I vendored/reliable \
//       FM2KHook/src/netplay/reliable_channel_test.cpp \
//       FM2KHook/src/netplay/reliable_channel.cpp \
//       vendored/reliable/reliable.c -o /tmp/rc_test && /tmp/rc_test
#ifdef RC_STANDALONE_TEST

#include "reliable_channel.h"
#include "reliable_channel_test_link.h"   // Link / Recv / PumpLink / CHECK / g_fail

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <string>

using namespace fm2k::rc;

// Tests 14-20: the liveness/holding-advertisement half of the design, in its
// own TU so neither file crosses the repo's 1000-line rule. Returns nothing --
// it reports through the shared g_fail.
void RcLivenessTests();

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
    // the old code the message sits in `ooo` at this instant.)
    //
    // *** DELIBERATE CONTRACT CHANGE, 2026-08-16 (RC liveness fix, S0-S2). ***
    // This test used to black-hole A->B ONLY, leaving B->A clean, and it
    // asserted the permanent loss of msg 5 as the EXPECTED outcome. That is no
    // longer the contract and must not be: with a clean reverse path the peer
    // is provably ALIVE and provably pinned on exactly that message, and
    // retiring it there is the defect that produced the spectator starve --
    // see test 11, which is the same link model with the opposite assertion.
    //
    // The un-anchor itself remains the last-resort repair and still needs a
    // test, so what changed is the LINK MODEL, not the assertions: the
    // blackout is now BOTH DIRECTIONS, which is the genuinely dead link. The
    // peer goes silent, RC_PEER_SILENT_SEC elapses, retirement happens at 7s
    // exactly as it always did, and the receiver un-anchors. This is the test
    // that stops the liveness fix from degenerating into "never give up". ----
    printf("[10] DEAD LINK (both directions): retired hole must not deafen the channel\n");
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

    // Black-hole BOTH directions and send msg 5. B receives nothing at all, so
    // `ooo` stays EMPTY -- the shape the old precondition exempted -- and A
    // hears nothing from B, so its peer-liveness test correctly reads the link
    // as dead. Hold it for 8s: past A's 7s retire (msg 5 can now NEVER be
    // re-sent) and past B's stall horizon (the resync arms).
    a2b10.drop_every = 1;   // (n % 1) == 0 for every n -> drop everything
    b2a10.drop_every = 1;   // dead LINK, not a one-way outage (see the note above)
    {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "s-05");
        Send(A10, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    // 11s, not 8s. The receiver no longer un-anchors on the bare 3.5s timer
    // when it has NO CURRENT INFORMATION about the sender (a silent peer is
    // usually a down link, which is exactly when the sender retains hardest);
    // it waits out RC_ORDERED_STALL_HARD_SEC = 9s first. Past that it un-
    // anchors exactly as before, which is what this test pins. Test 11 is the
    // complement: an outage INSIDE the 9s window must cost nothing at all.
    for (int tick = 0; tick < 550; tick++) {   // 550 * 20ms = 11.0s
        t10 += 0.02;
        Update(A10, t10); Update(B10, t10);
        PumpLink(a2b10); PumpLink(b2a10);
    }
    CHECK(rb10.msgs.size() == 5, "dead link: the black-holed msg 5 never arrived");

    // Link comes back. The very next message is msg 6, one PAST the dead hole.
    a2b10.drop_every = 0;
    b2a10.drop_every = 0;
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

    // ---- Test 11: ONE-WAY OUTAGE LONGER THAN THE TTL, REVERSE PATH CLEAN.
    // THE RED PROOF for the RC liveness fix (S0 + S1). This is the field shape
    // behind the spectator starve: a host that is reachable the whole time
    // (its JOIN_ACKs keep answering the viewer's pulls on a 500ms cadence)
    // whose forward stream is wedged. Two independent pre-fix defects combine:
    //
    //   * the receiver only emitted an ack carrier when it had RECEIVED
    //     something, so during a forward outage it went silent -- making a
    //     perfectly healthy, still-asking peer indistinguishable from a dead
    //     one; and
    //   * the sender retired every unacked message at a flat 7s measured from
    //     FIRST SEND, with no reference to whether the peer was alive. A live
    //     stream sends in dense runs, so a whole burst shares a first_time and
    //     EXPIRES AS A BLOCK -- which is why the field log said "14 message(s)
    //     permanently lost" as one number rather than one message at a time.
    //
    // On pre-fix code this test is RED: the messages first sent during the
    // outage are gone permanently and the delivered sequence has a gap. It is
    // the same link model as the OLD test 10, with the opposite assertion,
    // which is exactly the contract change documented there. ----
    printf("[11] one-way outage past the TTL, reverse path clean: NO permanent loss\n");
    Recv rb11; Link a2b11, b2a11;
    double t11 = 0.0;
    Endpoint* A11 = Create(20, &LinkSend, &a2b11, &OnDeliver, &ra, t11);
    Endpoint* B11 = Create(21, &LinkSend, &b2a11, &OnDeliver, &rb11, t11);
    a2b11.dst = B11; b2a11.dst = A11;
    // ~900B each so Nagle never coalesces two of them into one datagram (the
    // trick test 6 uses) -- otherwise "the outage" would not be per-message.
    const int M11_BASE = 6, M11_OUTAGE = 10;
    auto send11 = [&](int i) {
        char buf[960];
        int n = snprintf(buf, sizeof(buf), "w-%02d", i);
        memset(buf + n, 'x', 900 - n);
        Send(A11, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), 900);
    };
    for (int i = 0; i < M11_BASE; i++) send11(i);
    for (int tick = 0; tick < 25; tick++) {
        t11 += 0.02;
        Update(A11, t11); Update(B11, t11);
        PumpLink(a2b11); PumpLink(b2a11);
    }
    CHECK(rb11.msgs.size() == (size_t)M11_BASE, "one-way: 6-message baseline delivered");

    // Blackhole A->B ONLY. B->A stays clean, so B keeps acking (the carrier
    // keepalive is what makes it keep acking with nothing to ack) and A can
    // see that its peer is alive.
    a2b11.drop_every = 1;
    const int dg_before11 = a2b11.counter;
    int sent11 = 0;
    // THE WINDOW IS DELIBERATE, and the arithmetic is the point of the test:
    // 7.5s is PAST the sender's 7.0s retire horizon (so a flat TTL loses the
    // burst) and INSIDE the receiver's 9.0s hard backstop (so the receiver has
    // not given up either). Both margins are ~1s, and the baseline pump above
    // is kept short so the stall clock -- which runs from the last DELIVERY,
    // not from the blackout -- stays inside the backstop too.
    for (int tick = 0; tick < 375; tick++) {   // 375 * 20ms = 7.5s
        t11 += 0.02;
        if (tick % 20 == 0 && sent11 < M11_OUTAGE) send11(M11_BASE + sent11++);
        Update(A11, t11); Update(B11, t11);
        PumpLink(a2b11); PumpLink(b2a11);
    }
    const int dg_outage11 = a2b11.counter - dg_before11;
    CHECK(sent11 == M11_OUTAGE, "one-way: all 10 outage messages were offered");
    CHECK(rb11.msgs.size() == (size_t)M11_BASE,
          "one-way: nothing arrived during the 7.5s blackout (the outage is real)");

    // Heal. Everything A retained must now flow, in order, exactly once.
    a2b11.drop_every = 0;
    for (int tick = 0; tick < 150; tick++) {
        t11 += 0.02;
        Update(A11, t11); Update(B11, t11);
        PumpLink(a2b11); PumpLink(b2a11);
    }
    printf("  delivered after heal: %zu / %d (A emitted %d datagrams during the outage)\n",
           rb11.msgs.size(), M11_BASE + M11_OUTAGE, dg_outage11);
    CHECK(rb11.msgs.size() == (size_t)(M11_BASE + M11_OUTAGE),
          "one-way: ALL 16 delivered after the heal -- the peer was alive and "
          "asking the whole time, so nothing was retired");
    bool ordered11 = (rb11.msgs.size() == (size_t)(M11_BASE + M11_OUTAGE));
    for (int i = 0; i < (int)rb11.msgs.size(); i++) {
        char want[16]; snprintf(want, sizeof(want), "w-%02d", i);
        if (rb11.msgs[i].second.compare(0, strlen(want), want) != 0) ordered11 = false;
    }
    CHECK(ordered11, "one-way: delivered strictly in order, no gap and no duplicates");
    // Retention must not have turned into a flood. Bound: cwnd(96) messages
    // over the outage at the 80ms retransmit floor, plus FEC parity and the
    // ack carriers, is comfortably under this.
    CHECK(dg_outage11 > 0 && dg_outage11 < 5000,
          "one-way: the retained backlog kept retransmitting at a BOUNDED rate");

    // ---- Test 12: THE HARD CEILING. A peer that is alive and asking but can
    // NEVER receive (path-MTU blackhole, a middlebox eating one flow) would
    // otherwise pin the sender's backlog forever, because liveness stays true
    // by construction. RC_RETIRE_HARD_SEC is what answers the "infinite
    // retention" attack, and without a test for it that answer is a promise.
    //
    // Read together with test 11 this brackets the ceiling from both sides:
    // healed at 8s -> everything delivered (retained past the 7s TTL); held to
    // 30s -> the backlog IS gone (retired at the 25s ceiling) and the channel
    // still recovers afterwards. ----
    printf("[12] hard ceiling: alive-but-deaf peer still sheds its backlog\n");
    Recv rb12; Link a2b12, b2a12;
    double t12 = 0.0;
    Endpoint* A12 = Create(22, &LinkSend, &a2b12, &OnDeliver, &ra, t12);
    Endpoint* B12 = Create(23, &LinkSend, &b2a12, &OnDeliver, &rb12, t12);
    a2b12.dst = B12; b2a12.dst = A12;
    for (int i = 0; i < 4; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "c-%02d", i);
        Send(A12, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    for (int tick = 0; tick < 60; tick++) {
        t12 += 0.02;
        Update(A12, t12); Update(B12, t12);
        PumpLink(a2b12); PumpLink(b2a12);
    }
    CHECK(rb12.msgs.size() == 4, "hard ceiling: 4-message baseline delivered");
    a2b12.drop_every = 1;                       // forward path dead, reverse clean
    for (int i = 4; i < 8; i++) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "c-%02d", i);
        Send(A12, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    for (int tick = 0; tick < 750; tick++) {   // 750 * 40ms = 30.0s, past the 25s ceiling
        t12 += 0.04;
        Update(A12, t12); Update(B12, t12);
        PumpLink(a2b12); PumpLink(b2a12);
    }
    a2b12.drop_every = 0;
    {   // one NEW message: the channel must still work after the shed
        char buf[32]; int n = snprintf(buf, sizeof(buf), "c-99");
        Send(A12, 0, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), n);
    }
    for (int tick = 0; tick < 150; tick++) {
        t12 += 0.02;
        Update(A12, t12); Update(B12, t12);
        PumpLink(a2b12); PumpLink(b2a12);
    }
    bool shed12 = true, recovered12 = false;
    for (auto& m : rb12.msgs) {
        if (m.second == "c-04" || m.second == "c-05" ||
            m.second == "c-06" || m.second == "c-07") shed12 = false;
        if (m.second == "c-99") recovered12 = true;
    }
    CHECK(shed12, "hard ceiling: the 30s backlog WAS retired -- retention is bounded, "
                  "not infinite (this is what stops the fix becoming 'never give up')");
    CHECK(recovered12, "hard ceiling: the channel delivers again after the shed");

    // ---- Test 13: WIRE COMPATIBILITY of the holding advertisement. The
    // section is appended AFTER the cumulative ack block, and the claim that
    // it is invisible to a pre-fix peer rests entirely on that parser reading
    // exactly `count` fixed-stride entries and then returning. Both parse
    // shapes are transcribed here rather than called, because the pre-fix one
    // no longer exists in the tree -- so this pins the FORMAT claim, which is
    // the one a mixed-version field pairing depends on. ----
    printf("[13] wire compat: extended carrier body vs the pre-fix parse loop\n");
    {
        // Build a body exactly as the carrier builder does: 2 cumulative
        // entries, then [0xA5][hcount=2] and 2 holding entries.
        std::vector<uint8_t> body;
        body.push_back(2);
        const uint8_t  chans[2] = { 1, 2 };
        const uint64_t nexp[2]  = { 4242ull, 7ull };
        for (int i = 0; i < 2; i++) {
            body.push_back(chans[i]);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&nexp[i]);
            body.insert(body.end(), p, p + 8);
        }
        const size_t cumulative_len = body.size();
        body.push_back(0xA5);   // RC_CARRIER_EXT_MAGIC
        body.push_back(2);
        const uint64_t oldest[2]  = { 4242ull, 9ull };
        const uint8_t  holding[2] = { 1, 0 };   // 10-byte stride: chan|holding|u64
        for (int i = 0; i < 2; i++) {
            body.push_back(chans[i]);
            body.push_back(holding[i]);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&oldest[i]);
            body.insert(body.end(), p, p + 8);
        }

        // (a) THE PRE-FIX PARSER, transcribed verbatim from the shape at
        // reliable_channel.cpp:181-197 as it stood before this change.
        int      pre_entries = 0;
        uint64_t pre_last    = 0;
        size_t   pre_consumed = 0;
        {
            const uint8_t* payload = body.data();
            int len = (int)body.size();
            if (len >= 1) {
                uint8_t cnt = payload[0];
                const uint8_t* p = payload + 1;
                const uint8_t* end = payload + len;
                for (uint8_t i = 0; i < cnt && p + 9 <= end; i++) {
                    uint64_t n; memcpy(&n, p + 1, 8);
                    p += 9; pre_entries++; pre_last = n;
                }
                pre_consumed = (size_t)(p - payload);
            }
            // ...and then it RETURNS. Everything after `p` is unreachable.
        }
        CHECK(pre_entries == 2 && pre_last == 7ull,
              "wire compat: a pre-fix parser reads both cumulative acks correctly");
        CHECK(pre_consumed == cumulative_len,
              "wire compat: a pre-fix parser stops exactly at the end of the "
              "cumulative block and never touches the trailing section");

        // (b) THE NEW PARSER against a PRE-FIX body (no trailing section):
        // it must infer no advertisement at all rather than read past the end.
        std::vector<uint8_t> old_body(body.begin(), body.begin() + cumulative_len);
        bool inferred = false;
        {
            const uint8_t* payload = old_body.data();
            int len = (int)old_body.size();
            uint8_t cnt = payload[0];
            const uint8_t* p = payload + 1;
            const uint8_t* end = payload + len;
            for (uint8_t i = 0; i < cnt && p + 9 <= end; i++) p += 9;
            if (p + 2 <= end && p[0] == 0xA5) inferred = true;
        }
        CHECK(!inferred,
              "wire compat: the new parser infers NO advertisement from a "
              "pre-fix carrier (mixed-version degrades to today's behaviour)");

        // (c) The full carrier must stay ONE datagram with both sections full,
        // or the ack path starts fragmenting and a lost fragment silently
        // costs an ack. 1 + 32*9 + 2 + 32*10 body + 12 header + 4 CRC.
        const size_t worst = 1 + 32 * 9 + 2 + 32 * 10 + 12 + 4;
        CHECK(worst <= 1024,
              "wire compat: worst-case carrier stays under RC_FRAGMENT_ABOVE "
              "(one datagram, never fragmented)");
    }

    // Tests 14-20 (sibling TU): the positive-hold triangle, the sticky
    // backstop, builder-vs-format wire compat, hostile carrier bodies and the
    // unacked byte cap. Everything the adversarial review of 2026-08-16 found
    // to be unexecuted code lives there.
    RcLivenessTests();

    Destroy(A11); Destroy(B11); Destroy(A12); Destroy(B12);
    Destroy(A); Destroy(B); Destroy(A2); Destroy(B2); Destroy(A3); Destroy(B3);
    Destroy(A4); Destroy(B4); Destroy(A5); Destroy(B5);
    Destroy(A6); Destroy(B6); Destroy(A8b); Destroy(B8);
    Destroy(A9); Destroy(B9); Destroy(A10); Destroy(B10);
    printf("%s\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED");
    return g_fail == 0 ? 0 : 1;
}

#endif  // RC_STANDALONE_TEST
