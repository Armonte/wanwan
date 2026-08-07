#pragma once

#include <cstdint>

// Game-data fingerprint sent in HELLO so peers reject mismatched setups
// up front (#57). Approach (a) -- sorted list of all *.player + *.kgt +
// the .exe's stem in the game directory, each entry "lowercase_name|size"
// joined by newlines, hashed with xxhash3-64 truncated to 32 bits.
//
// Catches the common "we have different rosters" desync trigger (different
// .player file count or different filenames). Doesn't catch silent edits
// to existing files -- content hashing would, but at the cost of reading
// every .player byte on every connect; that's a v2 toggle if tournament
// strict mode wants it.
//
// Computed once on first call, cached. Returns 0 if the game directory
// can't be enumerated (the receiver treats hash=0 as "older client, skip
// the check" so the rollout is backwards-compatible).

namespace fm2k::game_hash {

uint32_t Compute();

// Friendly debug string ("3 .player + 1 .kgt + game.exe -- 0xabcd1234").
// Cached alongside the hash. Empty when Compute() has never been called.
const char* DescribeLocal();

// Full canonical manifest (one line per file: "name|size|content_hash"
// for .kgt/.exe, "name|size|-" for .player). Cached on first Compute().
// Used by the HELLO mismatch path so the hook log captures the full
// list of files that fed the hash -- peers can diff their logs to find
// which entry diverges. "(not computed yet)" sentinel when Compute()
// has never been called.
const char* ManifestLocal();

// Per-entry iteration: invoke `cb(name, size, content_hash)` for each
// file that fed the hash, in the canonical sorted order. content_hash
// is 0 for entries hashed by name+size only (.player). Used by the
// on-mismatch dump so the rendered output is byte-equivalent to the
// boot-time per-entry log -- avoids parsing the cached manifest string
// line-by-line, which had a (still-being-investigated) corruption
// case where one entry rendered with a truncated name+missing size.
void ForEachManifestEntry(
    void (*cb)(const char* name, uint64_t size, uint64_t content_hash, void* user),
    void* user);

}  // namespace fm2k::game_hash
