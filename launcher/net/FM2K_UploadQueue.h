#pragma once

// Launcher-side uploader for hook-generated crash/desync diagnostic
// bundles. See FM2KHook/src/netplay/upload_queue.h for the hook's
// matching enqueue helper.
//
// Workflow:
//   Hook → <game_dir>/upload_queue/<unix_ms>_<kind>_p<n>_<sid>.json
//   Launcher (this code) -- polls the dir every tick, processes one
//     manifest per tick to avoid blocking the UI:
//     1. Read JSON, validate fields.
//     2. Read each referenced file (truncating Debug.log tail to 2 MB
//        so a long match doesn't blow the 50 MB body cap).
//     3. POST multipart/form-data to https://hub.2dfm.org/logs/upload
//        with the X-FM2K-Log-Secret header.
//     4. On 200: move manifest to <game_dir>/upload_queue/done/.
//        On 4xx: move to <game_dir>/upload_queue/quarantine/ (broken
//        manifest, don't retry).
//        On network failure / 5xx: leave in place, retry next tick.
//
// Threading model: each Process() call performs at most one upload and
// blocks the calling thread for the duration. Caller (LauncherUI tick)
// is expected to invoke once per render frame; if more than one
// manifest is pending, each gets serviced on a separate tick. Network
// I/O happens on the UI thread but with a 15-second WinHTTP timeout
// so a slow VPS can't pin the launcher.

#include <string>

namespace fm2k::upload_queue {

// Configuration sourced from baked-at-build values + dev_flags.ini.
// LauncherUI builds one of these and passes it on every Process()
// call. Cheap to construct.
struct ProcessorConfig {
    std::string game_dir;       // absolute path; contains upload_queue/
    std::string upload_url;     // e.g. "https://hub.2dfm.org/logs/upload"
    std::string secret;         // X-FM2K-Log-Secret value
    bool        enabled = true; // launcher dev checkbox state
};

// Service at most one manifest. Returns true if it touched a file
// (uploaded or moved to quarantine), false if the queue was empty or
// the network attempt failed and the manifest stays for retry.
//
// Safe to call from the UI tick. No-op when cfg.enabled is false.
bool Process(const ProcessorConfig& cfg);

}  // namespace fm2k::upload_queue
