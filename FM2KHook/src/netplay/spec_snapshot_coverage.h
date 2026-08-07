#pragma once
// Snapshot-blob coverage set: which byte ranges of a SNAPSHOT_BEGIN/CHUNK/END
// transfer have actually landed.
//
// This replaces the old `inbox.bytes_received += n` counter, which was the
// reason a re-shipped snapshot could never finalize. RC dedups only within one
// (endpoint, channel, msg_seq) space, so a host re-ship carries the SAME byte
// offsets under FRESH seqs: the counter double-counted, overshot the exact
// `== wire_bytes` finalize test, and the transfer wedged forever -- logging
// "END seen but inbox incomplete -- N/M" at 1Hz with N sometimes greater
// than M. A coverage set cannot be satisfied by overshoot, only by real
// coverage, so re-delivery of any chunk (host re-ship, pre-BEGIN stash replay
// landing on already-applied bytes, a future selective NACK) is a no-op.
//
// Deliberately dependency-free (no Windows, no SDL, no engine headers) so
// tests/ exercises THIS code host-native rather than a mirror of it.
#include <cstddef>
#include <cstdint>
#include <map>

namespace fm2k {
namespace specsnap {

// start -> end (exclusive). Invariant: entries are disjoint, non-empty, and
// non-adjacent (adjacent ranges are merged on insert), so a complete transfer
// is exactly one entry.
using CoverageSet = std::map<uint32_t, uint32_t>;

// Mark [off, off+n) covered. Returns the count of NEWLY covered bytes -- 0 for
// a pure duplicate, and only the non-overlapping part for a partial overlap.
inline size_t CoverRange(CoverageSet& cov, uint32_t off, uint32_t n) {
    if (n == 0) return 0;
    uint32_t start = off;
    uint32_t end   = off + n;
    // First entry that can touch [start, end): the one at or before `start` if
    // it reaches start (overlap or adjacency), else the first strictly after.
    auto it = cov.upper_bound(start);
    if (it != cov.begin()) {
        auto prev = it; --prev;
        if (prev->second >= start) it = prev;
    }
    size_t already = 0;
    while (it != cov.end() && it->first <= end) {
        already += static_cast<size_t>(it->second - it->first);
        if (it->first  < start) start = it->first;
        if (it->second > end)   end   = it->second;
        it = cov.erase(it);
    }
    cov[start] = end;
    // Every absorbed entry lies inside the merged span, so the span minus what
    // those entries already held is exactly the new coverage.
    return static_cast<size_t>(end - start) - already;
}

// True once [0, total) is covered by a single range. Unlike a byte counter this
// is not satisfiable by overshoot.
inline bool FullyCovered(const CoverageSet& cov, uint32_t total) {
    return cov.size() == 1 && cov.begin()->first == 0u &&
           cov.begin()->second == total;
}

}  // namespace specsnap
}  // namespace fm2k
