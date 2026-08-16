#pragma once
// reliable_channel_test_link.h -- the scriptable lossy link + delivery recorder
// shared by the ReliableChannel host-native tests.
//
// Extracted 2026-08-16 when the liveness tests grew their own TU
// (reliable_channel_liveness_test.cpp) rather than push reliable_channel_test.cpp
// past the repo's 1000-line rule. Both TUs link into one binary, so these MUST
// be one definition: two identical-looking copies of `struct Link` in two TUs
// is an ODR violation the linker will not diagnose.
//
// Everything here is test-only. It is never compiled into the hook.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "reliable_channel.h"

// A one-way lossy link: datagrams queued here, optionally dropped, then pumped
// into the destination endpoint.
struct Link {
    std::deque<std::vector<uint8_t>> q;
    fm2k::rc::Endpoint* dst = nullptr;
    // drop predicate: called per datagram with a running counter.
    int counter = 0;
    int drop_every = 0;      // 0 = no drop; N = drop every Nth datagram
    int extra_drop_at = -1;  // one specific counter value to also drop (burst sim)
    // PATH-MTU-BLACKHOLE MODEL (added for the liveness tests): drop every
    // datagram strictly larger than this, pass everything at or below it. 0 =
    // off. This is the shape the ack-carrier keepalive and the holding
    // advertisement are designed for and the shape the sticky-backstop defect
    // lived in -- the ~40B carrier gets through while the ~900B data datagram
    // does not, so the peer looks alive and vocal while delivering nothing.
    int drop_above_bytes = 0;
    // The inverse, and it models a PRE-FIX SENDER seen from the receiver: drop
    // every datagram strictly smaller than this. Ack carriers are ~40B and data
    // messages are ~900B, so this suppresses the holding advertisement while
    // leaving the stream intact -- which is the only way to test "a post-fix
    // receiver paired with a peer that never advertises" without a second
    // build. 0 = off.
    int drop_below_bytes = 0;
    // Permanently black-hole any datagram containing this byte string. Lets a
    // test kill exactly ONE message (and every retransmit of it, since a
    // retransmit is the same bytes) while the rest of the stream flows, which
    // is how a receiver is parked on a hole its peer genuinely still holds.
    std::string drop_if_contains;
    int dropped = 0;
};

inline void LinkSend(void* ctx, const uint8_t* data, int len) {
    Link* L = static_cast<Link*>(ctx);
    int n = L->counter++;
    bool drop = (L->drop_every > 0 && (n % L->drop_every) == (L->drop_every - 1));
    if (n == L->extra_drop_at) drop = true;
    if (L->drop_above_bytes > 0 && len > L->drop_above_bytes) drop = true;
    if (L->drop_below_bytes > 0 && len < L->drop_below_bytes) drop = true;
    if (!L->drop_if_contains.empty() &&
        len >= static_cast<int>(L->drop_if_contains.size())) {
        const std::string hay(reinterpret_cast<const char*>(data),
                              static_cast<size_t>(len));
        if (hay.find(L->drop_if_contains) != std::string::npos) drop = true;
    }
    if (drop) { L->dropped++; return; }
    L->q.emplace_back(data, data + len);
}

// Deliver record on the receiver.
struct Recv {
    std::vector<std::pair<uint8_t, std::string>> msgs;  // (channel, payload)
};

inline void OnDeliver(void* ctx, uint8_t chan, const uint8_t* data, int len) {
    Recv* R = static_cast<Recv*>(ctx);
    R->msgs.emplace_back(chan, std::string(reinterpret_cast<const char*>(data),
                                           static_cast<size_t>(len)));
}

// Pump a link's queued datagrams into its destination endpoint. d[0] is the
// 0xCB wire tag that TransmitCb produced; OnDatagram wants the body after it.
inline void PumpLink(Link& L) {
    while (!L.q.empty()) {
        std::vector<uint8_t> d = std::move(L.q.front());
        L.q.pop_front();
        if (d.size() >= 1)
            fm2k::rc::OnDatagram(L.dst, d.data() + 1, static_cast<int>(d.size()) - 1);
    }
}

// Shared failure counter: both test TUs report into it and main() returns it.
inline int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; } \
                              else { printf("  ok:   %s\n", msg); } } while (0)
