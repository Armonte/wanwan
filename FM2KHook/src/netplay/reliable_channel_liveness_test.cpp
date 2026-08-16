// reliable_channel_liveness_test.cpp -- tests 14 to 20: the S0-S3 liveness half
// of the ReliableChannel layer.
//
// WHY THIS FILE EXISTS. The adversarial review of the S0-S3 change (2026-08-16)
// found that roughly half of the new design had never executed at ANY level --
// not in a unit test, not in four end-to-end spectator runs. `hold_fresh` fired
// zero times, `fact_repair` zero times, `holding_repair` / `IsRepairingOrdered()`
// zero times, the disarm rule zero times, the hard backstop's own repair zero
// times. The one confirmed defect in the change (the backstop's arm being
// undone by the peer's very next carrier) lived precisely inside that
// unexecuted half, which is the whole argument for these tests: the branch that
// nothing reaches is the branch that is wrong.
//
// Every test below therefore states which branch it reaches and asserts on a
// COUNTER or a STATE FIELD that proves the reach, not merely on an outcome that
// several paths could produce.
//
// Split from reliable_channel_test.cpp for the repo's 1000-line rule. It links
// into the same binary and reports into the same g_fail; the link model and the
// delivery recorder are shared through reliable_channel_test_link.h.
#ifdef RC_STANDALONE_TEST

#include "reliable_channel_internal.h"     // Endpoint/RxChannel/TxChannel + the codec
#include "reliable_channel_test_link.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fm2k::rc;

namespace {

// Build one holding-advertisement section by hand, as a hostile or synthetic
// peer would. Returns the bytes AFTER a (caller-supplied) cumulative block, so
// it can be handed straight to CarrierParseHoldSection.
void PushHoldEntry(std::vector<uint8_t>& v, uint8_t chan, bool holding,
                   uint64_t oldest) {
    v.push_back(chan);
    v.push_back(holding ? 1 : 0);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&oldest);
    v.insert(v.end(), p, p + 8);
}

std::vector<uint8_t> MakeHoldSection(uint8_t hcount_field,
                                     const std::vector<std::tuple<uint8_t, bool, uint64_t>>& e) {
    std::vector<uint8_t> v;
    v.push_back(RC_CARRIER_EXT_MAGIC);
    v.push_back(hcount_field);   // deliberately settable independently of e.size()
    for (auto& [c, h, o] : e) PushHoldEntry(v, c, h, o);
    return v;
}

// Give an endpoint an ORDERED rx cursor on `chan` without running a real
// stream through it. Used by the codec tests, which are about bytes and state,
// not about delivery.
RxChannel& MakeOrderedRx(Endpoint* ep, uint8_t chan, uint64_t next_deliver) {
    RxChannel& rc = ep->rx[chan];
    rc.cls_ordered = true;
    rc.cls_known   = true;
    rc.started     = true;
    rc.next_deliver = next_deliver;
    return rc;
}

// ~900B payloads so reliable.io never coalesces two of them into one datagram
// and the per-message drop filters stay per-message.
void SendBig(Endpoint* ep, uint8_t chan, const char* tag) {
    char buf[960];
    int n = snprintf(buf, sizeof(buf), "%s", tag);
    memset(buf + n, 'x', 900 - n);
    Send(ep, chan, Class::ReliableOrdered, reinterpret_cast<uint8_t*>(buf), 900);
}

bool Delivered(const Recv& r, const char* tag) {
    const size_t n = strlen(tag);
    for (auto& m : r.msgs) if (m.second.compare(0, n, tag) == 0) return true;
    return false;
}

}  // namespace

void RcLivenessTests() {
    // The kill-switch control run (FM2K_RC_LIVENESS=0) exists to prove tests
    // 1-13 restore the PRE-FIX contract byte for byte -- a 7/16 verdict in test
    // 11 identical to a genuine f8e4b67 build. Everything below asserts on the
    // machinery that switch turns off, so running it there would just report
    // that the switch works, loudly, in the middle of a control run. Skip, and
    // say so, rather than emit expected FAILs that a reader has to triage.
    if (!LivenessEnabled()) {
        printf("[14-20] SKIPPED -- FM2K_RC_LIVENESS is OFF; these tests assert "
               "on the machinery that switch disables\n");
        return;
    }

    Recv sink;   // deliveries on the sender side of each pair (nothing expected)

    // ---- Test 14: THE STICKY HARD BACKSTOP, against a peer that never stops
    // claiming to hold our hole. This is the red-proof for the one CONFIRMED
    // defect the adversarial review found, and it is also the only test that
    // drives the ENTIRE positive-hold triangle in one run:
    //   hold_fresh -> defer -> holding_repair -> IsRepairingOrdered() true
    //   -> the RC_ORDERED_STALL_HARD_SEC backstop -> an arm that STICKS
    //   -> the next message adopts it AT DELIVERY TIME.
    //
    // THE LINK MODEL IS THE POINT. The forward path passes the ~40B ack carrier
    // and permanently black-holes exactly one ~900B data message (and every
    // retransmit of it, since a retransmit is the same bytes). That is a
    // path-MTU blackhole or a middlebox eating one flow -- the exact scenario
    // RC_RETIRE_HARD_SEC's own comment cites as its reason for existing. The
    // sender is alive, vocal, honest and still holding; the receiver is pinned.
    //
    // BEFORE THE FIX this test is RED at both assertions: the peer's carrier
    // arrives within one carrier interval of the backstop arming and clears
    // `stall_resync` (the disarm rule), so the arm never survives to be used,
    // the receiver stays pinned, and the message sent after the backstop is
    // buffered instead of adopted. The deferral was bounded at 9s; the REPAIR
    // was bounded at 25s (an honest peer's own RC_RETIRE_HARD_SEC) or not at
    // all (a peer that lies) -- while two in-source comments claimed 9s. ----
    printf("[14] sticky backstop: a peer that keeps claiming to hold cannot pin "
           "us past the bound\n");
    {
        Recv rb; Link a2b, b2a;
        double t = 0.0;
        Endpoint* A = Create(30, &LinkSend, &a2b, &OnDeliver, &sink, t);
        Endpoint* B = Create(31, &LinkSend, &b2a, &OnDeliver, &rb, t);
        a2b.dst = B; b2a.dst = A;
        // FEC OFF: with parity on, the black-holed message is recoverable from
        // a group whose other members (the ack carriers) DO get through, and
        // the outage would not be an outage.
        SetFec(A, false, 4); SetFec(B, false, 4);

        for (int i = 0; i < 4; i++) {
            char tag[8]; snprintf(tag, sizeof(tag), "h-%02d", i);
            SendBig(A, 0, tag);
        }
        for (int tick = 0; tick < 25; tick++) {
            t += 0.02; Update(A, t); Update(B, t); PumpLink(a2b); PumpLink(b2a);
        }
        CHECK(rb.msgs.size() == 4, "sticky: 4-message baseline delivered");
        CHECK(B->rx[0].next_deliver == 4, "sticky: receiver cursor parked at 4");

        // One message dies on the wire forever. A retains it (B is alive and
        // acking) and advertises holding=1, oldest=4 == B's next_deliver, which
        // is both the deferral trigger AND the disarm trigger.
        a2b.drop_if_contains = "h-04";
        SendBig(A, 0, "h-04");

        bool saw_defer_latch = false, saw_is_repairing = false;
        int  arm_tick = -1;
        for (int tick = 0; tick < 525; tick++) {   // 525 * 0.02 = 10.5s
            t += 0.02; Update(A, t); Update(B, t); PumpLink(a2b); PumpLink(b2a);
            if (B->rx[0].holding_repair) {
                saw_defer_latch = true;
                if (IsRepairingOrdered(B)) saw_is_repairing = true;
            }
            if (arm_tick < 0 && B->st_backstop_arms > 0) arm_tick = tick;
        }
        CHECK(B->st_hold_defers >= 1,
              "sticky: the receiver DEFERRED on a fresh positive hold "
              "(hold_fresh reached -- it had never executed anywhere before)");
        CHECK(saw_defer_latch,
              "sticky: holding_repair latched while deferring (the flag S3 reads)");
        CHECK(saw_is_repairing,
              "sticky: IsRepairingOrdered() reported true to the app ladder");
        CHECK(B->st_backstop_arms == 1,
              "sticky: the RC_ORDERED_STALL_HARD_SEC backstop fired exactly once");
        // The load-bearing assertion. On the pre-fix tree the peer's next
        // carrier clears this within ~33ms of the backstop setting it.
        CHECK(B->rx[0].stall_resync,
              "sticky: the backstop's arm SURVIVED the peer's fresh "
              "holding=1/oldest<=cursor carriers (the disarm must not undo the "
              "backstop)");
        CHECK(B->rx[0].stall_resync_backstop,
              "sticky: the arm is marked as a BACKSTOP arm, not a timer arm");

        // ...and the arm is not merely present, it WORKS. No Update(B) between
        // arrival and assertion: adoption must happen at delivery time. Note
        // the carrier A emits in this same Update is pumped FIRST, so this is
        // exactly the adversarial interleaving -- a fresh positive
        // advertisement immediately followed by the successor message.
        SendBig(A, 0, "h-05");
        t += 0.02; Update(A, t); PumpLink(a2b);
        CHECK(Delivered(rb, "h-05"),
              "sticky: the next message was ADOPTED as the cursor on arrival "
              "(pre-fix: the arm was gone, so it buffers behind a hole the "
              "sender will hold for another 15s)");
        CHECK(!B->rx[0].stall_resync && !B->rx[0].stall_resync_backstop,
              "sticky: consuming the arm clears BOTH flags");

        // The channel is healthy from there, and the black-holed message stays
        // gone -- un-anchoring forward never re-delivers history.
        for (int i = 6; i < 9; i++) {
            char tag[8]; snprintf(tag, sizeof(tag), "h-%02d", i);
            SendBig(A, 0, tag);
        }
        for (int tick = 0; tick < 60; tick++) {
            t += 0.02; Update(A, t); Update(B, t); PumpLink(a2b); PumpLink(b2a);
        }
        CHECK(Delivered(rb, "h-08") && !Delivered(rb, "h-04"),
              "sticky: the channel delivers again afterwards and never "
              "re-delivers the skipped message");
        Destroy(A); Destroy(B);
    }

    // ---- Test 15: FACT-DRIVEN REPAIR. The headline "~14x faster than the 3.5s
    // timer" claim of the whole change, and it had zero coverage. The peer
    // advertises an oldest-held seq ABOVE our cursor, which is proof it retired
    // our hole and will never send it again, so the repair must run at
    // RC_ORDERED_FACT_REPAIR_SEC (0.25s) rather than at RC_ORDERED_STALL_SEC.
    // Driven through the REAL CarrierParseHoldSection so the codec, not a
    // transcription of it, is what the assertion covers. ----
    printf("[15] fact repair: 'I retired your hole' un-anchors immediately, not "
           "at the 3.5s timer\n");
    {
        Recv rb; Link a2b, b2a;
        double t = 0.0;
        Endpoint* A = Create(32, &LinkSend, &a2b, &OnDeliver, &sink, t);
        Endpoint* B = Create(33, &LinkSend, &b2a, &OnDeliver, &rb, t);
        a2b.dst = B; b2a.dst = A;
        SetFec(A, false, 4); SetFec(B, false, 4);
        for (int i = 0; i < 4; i++) {
            char tag[8]; snprintf(tag, sizeof(tag), "f-%02d", i);
            SendBig(A, 0, tag);
        }
        for (int tick = 0; tick < 25; tick++) {
            t += 0.02; Update(A, t); Update(B, t); PumpLink(a2b); PumpLink(b2a);
        }
        CHECK(rb.msgs.size() == 4 && B->rx[0].next_deliver == 4,
              "fact: 4-message baseline delivered, cursor at 4");

        // Mute the sender entirely so nothing but our synthetic carrier can
        // influence the receiver's decision.
        a2b.drop_every = 1;
        const double stall_from = t;
        for (int tick = 0; tick < 20; tick++) {   // 0.4s: past the 0.25s fact
            t += 0.02; Update(B, t);              // slack, far below the 3.5s timer
        }
        CHECK(!B->rx[0].stall_resync,
              "fact: nothing has repaired yet at 0.4s of stall (the timer is 3.5s)");

        std::vector<uint8_t> sec = MakeHoldSection(1, {{0, false, 5ull}});
        CarrierParseHoldSection(B, sec.data(), sec.data() + sec.size());
        CHECK(B->rx[0].peer_hold_known && !B->rx[0].peer_hold_active &&
              B->rx[0].peer_hold_oldest == 5ull,
              "fact: the real parser latched the advertisement into rx state");
        t += 0.02; Update(B, t);
        const double at = t - stall_from;
        printf("  repaired after %.2fs of stall (timer horizon is %.1fs)\n",
               at, RC_ORDERED_STALL_SEC);
        CHECK(B->st_unanchor_by_fact == 1 && B->st_unanchor_by_timer == 0,
              "fact: the un-anchor was attributed to the FACT, not the timer");
        CHECK(B->rx[0].stall_resync && at < RC_ORDERED_STALL_SEC,
              "fact: repaired strictly before the 3.5s horizon");
        CHECK(B->st_backstop_arms == 0,
              "fact: a fact repair is NOT a backstop arm (it stays disarmable)");
        Destroy(A); Destroy(B);
    }

    // ---- Test 16: A PEER THAT NEVER ADVERTISES -- the mixed-version path, at
    // the STATE level rather than as a local bool. D1(b): a post-fix receiver
    // paired with a pre-fix sender must keep TODAY's timing exactly, i.e.
    // un-anchor at RC_ORDERED_STALL_SEC and NOT wait out the 9s backstop.
    // A pre-fix sender is modelled by dropping only its small ack carriers, so
    // the advertisement never arrives while the stream itself is untouched. ----
    printf("[16] no advertisement (pre-fix sender): the plain 3.5s timer, "
           "unchanged\n");
    {
        Recv rb; Link a2b, b2a;
        double t = 0.0;
        Endpoint* A = Create(34, &LinkSend, &a2b, &OnDeliver, &sink, t);
        Endpoint* B = Create(35, &LinkSend, &b2a, &OnDeliver, &rb, t);
        a2b.dst = B; b2a.dst = A;
        SetFec(A, false, 4); SetFec(B, false, 4);
        a2b.drop_below_bytes = 200;   // A's ack carriers never reach B
        for (int i = 0; i < 4; i++) {
            char tag[8]; snprintf(tag, sizeof(tag), "n-%02d", i);
            SendBig(A, 0, tag);
        }
        for (int tick = 0; tick < 25; tick++) {
            t += 0.02; Update(A, t); Update(B, t); PumpLink(a2b); PumpLink(b2a);
        }
        CHECK(rb.msgs.size() == 4, "no-advert: 4-message baseline delivered");
        CHECK(!B->rx[0].peer_hold_known,
              "no-advert: peer_hold_known is FALSE -- the receiver inferred "
              "nothing from a carrier-less peer (this is the assertion a "
              "mixed-version field pairing actually depends on)");

        a2b.drop_if_contains = "n-04";
        SendBig(A, 0, "n-04");
        const double stall_from = t;
        double armed_at = -1.0;
        for (int tick = 0; tick < 600 && armed_at < 0.0; tick++) {  // up to 12s
            t += 0.02; Update(A, t); Update(B, t); PumpLink(a2b); PumpLink(b2a);
            if (B->rx[0].stall_resync) armed_at = t - stall_from;
        }
        printf("  un-anchored after %.2fs of stall (3.5s timer, 9.0s backstop)\n",
               armed_at);
        CHECK(armed_at > 0.0 && armed_at < 4.5,
              "no-advert: un-anchored on the 3.5s timer, NOT deferred to the "
              "9s backstop");
        CHECK(B->st_unanchor_by_timer >= 1 && B->st_hold_defers == 0 &&
              B->st_backstop_arms == 0,
              "no-advert: attributed to the timer, with zero deferrals and zero "
              "backstop arms");
        CHECK(!B->rx[0].stall_resync_backstop,
              "no-advert: a timer arm is NOT sticky -- it stays disarmable, "
              "which is the window the disarm rule exists to close");
        Destroy(A); Destroy(B);
    }

    // ---- Test 17: THE BUILDER, not a transcription of its output. Test 13
    // hand-writes an extended body and proves a pre-fix parser ignores the
    // trailing section -- which pins the FORMAT and not the BUILDER. If
    // CarrierAppendHoldSection ever miscomputed its offset, wrote hcount
    // inconsistently with the bytes it emitted, or clobbered body[0], test 13
    // would still pass. So: build through the REAL builder, then read it with
    // the REAL parser AND with the f8e4b67 loop. ----
    printf("[17] builder x pre-fix parser: the real CarrierAppendHoldSection "
           "output is additive\n");
    {
        double t = 0.0;
        Link dummy; Recv dsink;
        Endpoint* E = Create(36, &LinkSend, &dummy, &OnDeliver, &dsink, t);
        Endpoint* R = Create(37, &LinkSend, &dummy, &OnDeliver, &dsink, t);
        // Two ORDERED tx channels -- one holding (a queued-but-unpumped message
        // counts, which is why queued_lowest exists), one idle -- plus one
        // UNORDERED channel that must be skipped entirely.
        TxChannel& t1 = E->tx[1];
        t1.cls_known = true; t1.cls_ordered = true;
        t1.queued_count = 1; t1.queued_lowest = 41; t1.next_msg_seq = 42;
        TxChannel& t2 = E->tx[2];
        t2.cls_known = true; t2.cls_ordered = true; t2.next_msg_seq = 9;
        TxChannel& t3 = E->tx[3];
        t3.cls_known = true; t3.cls_ordered = false; t3.next_msg_seq = 1000;

        // Cumulative block exactly as the carrier builder writes it.
        uint8_t body[1 + RC_CARRIER_MAX_ENTRIES * 9 +
                     2 + RC_CARRIER_MAX_ENTRIES * RC_CARRIER_HOLD_STRIDE];
        const uint8_t  cum_chan[2] = { 1, 2 };
        const uint64_t cum_nexp[2] = { 4242ull, 7ull };
        size_t off = 1;
        for (int i = 0; i < 2; i++) {
            body[off] = cum_chan[i];
            std::memcpy(body + off + 1, &cum_nexp[i], 8);
            off += 9;
        }
        body[0] = 2;
        const size_t cumulative_len = off;
        const size_t total = CarrierAppendHoldSection(E, body, off);

        CHECK(total == cumulative_len + 2 + 2 * RC_CARRIER_HOLD_STRIDE,
              "builder: emitted a header plus exactly 2 entries (the unordered "
              "channel was skipped)");
        CHECK(body[0] == 2,
              "builder: the cumulative count byte was NOT clobbered by the "
              "append");
        CHECK(body[cumulative_len] == RC_CARRIER_EXT_MAGIC &&
              body[cumulative_len + 1] == 2,
              "builder: magic + hcount land exactly where the cumulative block "
              "ended");
        CHECK(E->st_hold_adverts_sent == 1,
              "builder: the advertisement counter moved (the branch was taken)");

        // (a) the f8e4b67 parse loop, transcribed, over the REAL builder output.
        size_t pre_consumed = 0; int pre_entries = 0; uint64_t pre_last = 0;
        {
            const uint8_t* payload = body;
            const int len = static_cast<int>(total);
            uint8_t cnt = payload[0];
            const uint8_t* p = payload + 1;
            const uint8_t* end = payload + len;
            for (uint8_t i = 0; i < cnt && p + 9 <= end; i++) {
                uint64_t n; std::memcpy(&n, p + 1, 8);
                p += 9; pre_entries++; pre_last = n;
            }
            pre_consumed = static_cast<size_t>(p - payload);
        }
        CHECK(pre_consumed == 1 + 9 * 2 && pre_entries == 2 && pre_last == 7ull,
              "builder: a pre-fix parser consumes exactly 1 + 9*cnt bytes of "
              "the REAL builder output and reads both cumulative acks");

        // (b) the real parser reads back exactly what the real builder wrote.
        MakeOrderedRx(R, 1, 0);
        MakeOrderedRx(R, 2, 0);
        CarrierParseHoldSection(R, body + pre_consumed, body + total);
        CHECK(R->rx[1].peer_hold_known && R->rx[1].peer_hold_active &&
              R->rx[1].peer_hold_oldest == 41ull,
              "builder: round-trip -- the holding channel reads back as "
              "holding=1 oldest=41 (the QUEUED seq, not next_msg_seq)");
        CHECK(R->rx[2].peer_hold_known && !R->rx[2].peer_hold_active &&
              R->rx[2].peer_hold_oldest == 9ull,
              "builder: round-trip -- the idle channel reads back as holding=0 "
              "oldest=next_msg_seq");
        CHECK(R->rx.find(3) == R->rx.end(),
              "builder: the unordered channel never reached the wire, so the "
              "receiver never invented an rx cursor for it");

        // (c) a peer with NO ordered tx emits no section at all -- ABSENT, not
        // an empty section, which is what keeps the body byte-identical to a
        // pre-fix carrier on such a peer.
        Endpoint* Q = Create(38, &LinkSend, &dummy, &OnDeliver, &dsink, t);
        Q->tx[7].cls_known = true; Q->tx[7].cls_ordered = false;
        uint8_t qbody[64]; qbody[0] = 0;
        CHECK(CarrierAppendHoldSection(Q, qbody, 1) == 1 &&
              Q->st_hold_adverts_sent == 0,
              "builder: no ordered tx -> the section is ABSENT, not empty");
        Destroy(E); Destroy(R); Destroy(Q);
    }

    // ---- Test 18: THE PARSER against a PRE-FIX BODY, at the state level.
    // D1(b) again, from the other side: test 13(b) re-implements the guard and
    // asserts on a local bool, so nothing anywhere asserted that the receiver's
    // STATE is untouched. ----
    printf("[18] pre-fix carrier x the real parser: no state is invented\n");
    {
        double t = 0.0;
        Link dummy; Recv dsink;
        Endpoint* R = Create(39, &LinkSend, &dummy, &OnDeliver, &dsink, t);
        MakeOrderedRx(R, 0, 12);
        MakeOrderedRx(R, 1, 3);

        // Body ends exactly where the cumulative block does (p == end).
        std::vector<uint8_t> body{2};
        for (uint64_t n : {12ull, 3ull}) {
            body.push_back(static_cast<uint8_t>(n == 12ull ? 0 : 1));
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&n);
            body.insert(body.end(), p, p + 8);
        }
        CarrierParseHoldSection(R, body.data() + body.size(),
                                body.data() + body.size());
        CHECK(!R->rx[0].peer_hold_known && !R->rx[1].peer_hold_known,
              "pre-fix body: peer_hold_known stays FALSE on every channel");
        CHECK(!R->rx[0].peer_hold_active && R->rx[0].peer_hold_time < 0.0,
              "pre-fix body: no freshness stamp, no holding claim");

        // One trailing byte (p + 2 > end) and a non-magic trailing byte are the
        // two other shapes a pre-fix or corrupt carrier can end in.
        const uint8_t one = RC_CARRIER_EXT_MAGIC;
        CarrierParseHoldSection(R, &one, &one + 1);
        const uint8_t two[2] = { 0x5A, 4 };
        CarrierParseHoldSection(R, two, two + 2);
        CHECK(!R->rx[0].peer_hold_known && !R->rx[1].peer_hold_known,
              "pre-fix body: a truncated or non-magic tail is ignored, not "
              "read past");
        Destroy(R);
    }

    // ---- Test 19: HOSTILE CARRIER BODIES. R2 demands "every read
    // bounds-checked against end" and "hcount capped at 32". Both were correct
    // by reading and covered by nothing. The carrier is unauthenticated UDP
    // from an arbitrary source, so these are the inputs to assume. ----
    printf("[19] hostile carrier bodies: oversized hcount, truncated tail, "
           "absurd seqs\n");
    {
        double t = 0.0;
        Link dummy; Recv dsink;
        Endpoint* R = Create(40, &LinkSend, &dummy, &OnDeliver, &dsink, t);
        MakeOrderedRx(R, 0, 100);
        MakeOrderedRx(R, 1, 200);

        // (a) hcount=255 with only 2 entries actually present. The loop must
        // stop at `end`, not at hcount. std::vector gives an exact-size heap
        // buffer so an over-read is a real ASAN finding, not a silent read into
        // adjacent stack.
        std::vector<uint8_t> hostile =
            MakeHoldSection(255, {{0, true, 100ull}, {1, true, 200ull}});
        CarrierParseHoldSection(R, hostile.data(),
                                hostile.data() + hostile.size());
        CHECK(R->rx[0].peer_hold_known && R->rx[1].peer_hold_known &&
              R->rx[0].peer_hold_oldest == 100ull,
              "hostile: hcount=255 with 2 entries reads exactly 2 and stops at "
              "the end of the buffer");

        // (b) magic present, hcount honest, but the LAST entry is chopped short
        // mid-way. The complete entry must be applied and the partial one must
        // be dropped -- the failure mode being guarded is reading the missing
        // 3 bytes off the end of the buffer.
        std::vector<uint8_t> trunc =
            MakeHoldSection(2, {{0, true, 7ull}, {1, true, 999ull}});
        trunc.resize(trunc.size() - 3);
        R->rx[0].peer_hold_oldest = 100ull;
        R->rx[1].peer_hold_oldest = 200ull;
        CarrierParseHoldSection(R, trunc.data(), trunc.data() + trunc.size());
        CHECK(R->rx[0].peer_hold_oldest == 7ull &&
              R->rx[1].peer_hold_oldest == 200ull,
              "hostile: a truncated tail costs exactly the truncated entry, "
              "not a read past the buffer");

        // (c) an entry naming a channel we have no ordered rx cursor for must
        // be dropped WITHOUT creating one -- otherwise a spoofed carrier could
        // grow the rx map at will.
        std::vector<uint8_t> unknown = MakeHoldSection(1, {{200, true, 1ull}});
        CarrierParseHoldSection(R, unknown.data(),
                                unknown.data() + unknown.size());
        CHECK(R->rx.find(200) == R->rx.end(),
              "hostile: an entry for an unknown channel allocates nothing");

        // (d) oldest = UINT64_MAX. This is the "un-anchor immediately" attack.
        // It is NOT new and is NOT fixed here (a hostile peer can already erase
        // a sender's whole backlog through the CUMULATIVE section) -- what is
        // asserted is that it stays confined: no crash, no BACKWARD cursor
        // move, and no effect on any other channel.
        const uint64_t before1 = R->rx[1].next_deliver;
        std::vector<uint8_t> huge =
            MakeHoldSection(1, {{0, false, UINT64_MAX}});
        CarrierParseHoldSection(R, huge.data(), huge.data() + huge.size());
        t += 0.5; Update(R, t);
        t += 4.0; Update(R, t);
        CHECK(R->rx[0].next_deliver == 100ull && R->rx[1].next_deliver == before1,
              "hostile: an absurd advertisement can arm an un-anchor but can "
              "never move a cursor BACKWARD or touch another channel");
        Destroy(R);
    }

    // ---- Test 20: THE UNACKED BYTE CAP. The spec's test 13 was "hard ceiling
    // AND byte cap"; only the ceiling was built, and the review's R3 was
    // therefore claiming a discharged bound it had not tested. The cap is
    // dominated by the cwnd bound at real spectator message sizes (96 x ~180B),
    // so it is unreachable on production traffic and can only be driven with
    // near-envelope messages -- which is exactly what it is insurance against.
    // Pacing is opened up and the resend interval stretched so the test costs
    // two retransmit rounds instead of eighty. ----
    printf("[20] unacked byte cap: an alive-but-deaf peer cannot pin unbounded "
           "bytes\n");
    {
        Recv rb; Link a2b, b2a;
        double t = 0.0;
        Endpoint* A = Create(41, &LinkSend, &a2b, &OnDeliver, &sink, t);
        Endpoint* B = Create(42, &LinkSend, &b2a, &OnDeliver, &rb, t);
        a2b.dst = B; b2a.dst = A;
        SetFec(A, false, 4); SetFec(B, false, 4);
        SetPacing(A, 100000.0, 5000.0, 96);   // the pacer is not what is under test
        SetResendSeconds(A, 5.0);             // 2 retransmit rounds, not ~90

        SendBig(A, 0, "b-00");
        for (int tick = 0; tick < 25; tick++) {
            t += 0.02; Update(A, t); Update(B, t); PumpLink(a2b); PumpLink(b2a);
        }
        CHECK(rb.msgs.size() == 1, "bytecap: baseline delivered, peer is live");

        // Path-MTU blackhole: the ~40B carriers still flow both ways, so the
        // peer stays provably ALIVE and neither the silent arm nor (within this
        // window) the 25s ceiling can fire. Only the byte cap can.
        a2b.drop_above_bytes = 200;
        std::vector<uint8_t> big(60000, 'z');
        for (int i = 0; i < 40; i++) {          // 40 x ~60KB = ~2.4MB > 2MB
            std::memcpy(big.data(), "bulk", 4);
            Send(A, 0, Class::ReliableOrdered, big.data(),
                 static_cast<int>(big.size()));
        }
        for (int tick = 0; tick < 400; tick++) {   // 400 * 0.025 = 10s, past 7s
            t += 0.025; Update(A, t); Update(B, t); PumpLink(a2b); PumpLink(b2a);
        }
        printf("  unacked_bytes=%zu cap=%zu | ret_cap=%llu ret_dead=%llu "
               "ret_hard=%llu\n",
               A->unacked_bytes, RC_MAX_UNACKED_BYTES,
               (unsigned long long)A->st_retire_bytecap,
               (unsigned long long)A->st_retire_dead,
               (unsigned long long)A->st_retire_hard);
        CHECK(A->st_retire_bytecap > 0,
              "bytecap: the 2MB cap retired oldest-first (the branch is "
              "reachable, and now executed)");
        CHECK(A->st_retire_dead == 0 && A->st_retire_hard == 0 &&
              A->st_retire_flat == 0,
              "bytecap: neither the dead-link arm nor the 25s ceiling fired -- "
              "the peer was alive the whole time");
        CHECK(A->unacked_bytes <= RC_MAX_UNACKED_BYTES,
              "bytecap: retention ends up back under the cap");
        Destroy(A); Destroy(B);
    }
}

#endif  // RC_STANDALONE_TEST
