// Snapshot-blob coverage set (spec_snapshot_coverage.h) -- the idempotence
// property the snapshot inbox depends on.
//
// This exercises the REAL header the hook compiles, not a mirror: the header is
// deliberately free of Windows/SDL/engine dependencies for exactly this reason.
//
// The bug being pinned: the inbox used to track progress as
// `bytes_received += n` per chunk and finalize on `bytes_received ==
// wire_bytes`. RC dedups only within one (endpoint, channel, msg_seq) space, so
// a host re-ship carries the SAME byte offsets under FRESH seqs -- the counter
// double-counted, overshot the exact equality test, and the transfer could then
// NEVER finalize. A re-ship, which is the only repair the app layer has, was
// therefore guaranteed to make things worse rather than better.
#include "doctest.h"
#include "../FM2KHook/src/netplay/spec_snapshot_coverage.h"

#include <cstdint>
#include <vector>

using fm2k::specsnap::CoverageSet;
using fm2k::specsnap::CoverRange;
using fm2k::specsnap::FullyCovered;

namespace {

// Sum of a coverage set, the way the inbox's bytes_received accumulates it.
size_t Span(const CoverageSet& cov) {
    size_t n = 0;
    for (const auto& kv : cov) n += static_cast<size_t>(kv.second - kv.first);
    return n;
}

}  // namespace

TEST_CASE("snapshot coverage: sequential chunks merge to one range") {
    CoverageSet cov;
    size_t total = 0;
    const uint32_t kChunk = 960;
    const uint32_t kWire  = kChunk * 10;
    for (uint32_t off = 0; off < kWire; off += kChunk)
        total += CoverRange(cov, off, kChunk);
    CHECK(total == kWire);
    CHECK(cov.size() == 1);
    CHECK(FullyCovered(cov, kWire));
    CHECK(Span(cov) == total);
}

TEST_CASE("snapshot coverage: a re-ship contributes zero and cannot overshoot") {
    CoverageSet cov;
    const uint32_t kChunk = 960;
    const uint32_t kWire  = kChunk * 8;
    size_t total = 0;
    for (uint32_t off = 0; off < kWire; off += kChunk)
        total += CoverRange(cov, off, kChunk);
    REQUIRE(total == kWire);
    REQUIRE(FullyCovered(cov, kWire));

    // Host re-ships the identical snapshot: same offsets, fresh RC msg_seqs.
    // Under the old counter this pushed bytes_received to 2 * kWire and the
    // `== wire_bytes` finalize test could never pass again.
    for (uint32_t off = 0; off < kWire; off += kChunk) {
        CHECK(CoverRange(cov, off, kChunk) == 0);
    }
    CHECK(Span(cov) == kWire);
    CHECK(cov.size() == 1);
    CHECK(FullyCovered(cov, kWire));
}

TEST_CASE("snapshot coverage: out-of-order arrival with a hole") {
    CoverageSet cov;
    const uint32_t kChunk = 960;
    const uint32_t kWire  = kChunk * 5;
    // Everything except chunk 2 (the UNORDERED blob channel delivers in any
    // order, and a lost chunk delays only itself).
    CHECK(CoverRange(cov, kChunk * 4, kChunk) == kChunk);
    CHECK(CoverRange(cov, kChunk * 1, kChunk) == kChunk);
    CHECK(CoverRange(cov, kChunk * 0, kChunk) == kChunk);
    CHECK(CoverRange(cov, kChunk * 3, kChunk) == kChunk);
    CHECK(Span(cov) == kChunk * 4);
    CHECK_FALSE(FullyCovered(cov, kWire));
    CHECK(cov.size() == 2);   // [0,2c) and [3c,5c)

    // The straggler (or its re-ship) fills the hole and everything coalesces.
    CHECK(CoverRange(cov, kChunk * 2, kChunk) == kChunk);
    CHECK(cov.size() == 1);
    CHECK(FullyCovered(cov, kWire));
}

TEST_CASE("snapshot coverage: partial overlap counts only the new bytes") {
    CoverageSet cov;
    CHECK(CoverRange(cov, 100, 100) == 100);   // [100,200)
    CHECK(CoverRange(cov, 150, 100) == 50);    // [150,250) -> 50 new
    CHECK(Span(cov) == 150);
    CHECK(cov.size() == 1);
    CHECK(CoverRange(cov, 0, 100) == 100);     // adjacent from the left
    CHECK(cov.size() == 1);
    CHECK(FullyCovered(cov, 250));
    // A range wholly inside an existing one is a pure duplicate.
    CHECK(CoverRange(cov, 10, 5) == 0);
    CHECK(CoverRange(cov, 0, 250) == 0);
    CHECK(Span(cov) == 250);
}

TEST_CASE("snapshot coverage: a spanning range absorbs many islands") {
    CoverageSet cov;
    for (uint32_t i = 0; i < 10; i++) CoverRange(cov, i * 100, 10);
    REQUIRE(cov.size() == 10);
    REQUIRE(Span(cov) == 100);
    // One big range covering all of them: 1000 total minus the 100 already held.
    CHECK(CoverRange(cov, 0, 1000) == 900);
    CHECK(cov.size() == 1);
    CHECK(FullyCovered(cov, 1000));
}

TEST_CASE("snapshot coverage: zero-length and empty-set edges") {
    CoverageSet cov;
    CHECK(CoverRange(cov, 0, 0) == 0);
    CHECK(cov.empty());
    CHECK_FALSE(FullyCovered(cov, 0));      // nothing received != complete
    CHECK_FALSE(FullyCovered(cov, 1024));
    CHECK(CoverRange(cov, 5, 1) == 1);
    CHECK_FALSE(FullyCovered(cov, 6));      // starts at 5, not 0
    CHECK(CoverRange(cov, 0, 5) == 5);
    CHECK(FullyCovered(cov, 6));
}
