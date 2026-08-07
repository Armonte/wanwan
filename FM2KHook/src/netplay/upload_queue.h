#pragma once

// upload_queue -- drop a manifest JSON into <game_dir>/upload_queue/ that
// the launcher uploads to the hub. Used for crash + desync diagnostic
// auto-collection.
//
// Design: hook only does fopen/fwrite/fclose. No threading, no network,
// no allocator-sensitive work -- safe to call from a desync terminate
// path or an unhandled exception filter where the process is about to
// die.
//
// The launcher polls <game_dir>/upload_queue/*.json from its main tick,
// reads each manifest, gzips referenced files, POSTs multipart to
// https://hub.2dfm.org/logs/upload with X-FM2K-Log-Secret, then moves
// processed manifests to <game_dir>/upload_queue/done/.

#include <cstdint>
#include <vector>
#include <string>

namespace fm2k::upload_queue {

// Snapshot of session state to embed in the manifest. Caller fills in
// whatever it knows; missing fields are written as null/empty so the
// receiver-side schema check still passes. session_id and match_id may
// be zero/empty for crashes that happen outside an active match (CSS,
// title, boot) -- that's fine, just makes the upload search-by harder.
struct Manifest {
    const char* kind            = "unknown";  // "desync" | "crash" | "exception_recovered"
    int32_t     frame           = -1;          // engine frame at the event; -1 = unknown
    uint64_t    session_id      = 0;           // 0 = unknown / no active session
    const char* match_id        = "";          // empty = unknown
    int         player_index    = 0;           // 0 or 1
    const char* game_id         = "";          // exe basename, e.g. "WonderfulWorld_ver_0946"
    // Optional context. Pass empty/zero if not known at the call site.
    uint32_t    rng_seed        = 0;
    const char* peer_ip         = "";          // already partially redacted in logs
    // Absolute paths to the files we want uploaded with this report.
    // Caller is responsible for ensuring they exist before calling.
    std::vector<std::string> file_paths;
};

// Write the manifest to disk. Returns true on success. The launcher
// will discover and process the file on its next tick. Safe to call
// from crash handlers -- uses only fopen/fwrite/fclose, no allocations
// that would block on a corrupt heap.
//
// On success, writes:
//   <game_dir>/upload_queue/<unix_ms>_<kind>_p<n>_<sid>.json
bool Enqueue(const Manifest& m);

// UTF-8 versions of GetCurrentDirectory + GetModuleFileName. Use these
// when populating Manifest paths/game_id so the resulting JSON is
// valid UTF-8 (required by spec; the launcher quarantines manifests
// with non-UTF-8 bytes after v0.2.44). The ANSI variants return raw
// Shift-JIS / Windows-932 bytes on Japanese-locale systems with
// Japanese game directories, which crashed the launcher in v0.2.41 →
// std::filesystem::path on MinGW threw on those bytes from inside
// PollUploadQueue on the first render frame.
//
// Returns true on success. On failure leaves `out` empty.
bool GetCurrentDirectoryUtf8(std::string& out);
bool GetModuleFileNameUtf8(std::string& out);

}  // namespace fm2k::upload_queue
