// FM2K_Updater -- auto-updater glue for the launcher.
//
// Pulls "LatestVersion" from raw.githubusercontent.com/<owner>/<repo>/main,
// compares with kAppVersion (from version_local.h), and if newer
// downloads the matching release zip from
// github.com/<owner>/<repo>/releases/download/v<ver>/fm2k_v<ver>.zip.
// On user confirm, hands the zip to FM2KUpdater.exe (separate target),
// which extracts it after the launcher exits and relaunches.
//
// All HTTP via WinHTTP. Async -- work runs on a background thread, the
// UI polls Snapshot() each frame and renders the pill state.
#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

namespace fm2k::updater {

enum class State {
    Idle,           // boot -- no check yet
    Checking,       // GET LatestVersion in flight
    UpToDate,       // remote ≤ local
    UpdateAvailable,// remote > local, ready for user prompt
    Downloading,    // user clicked Update -- zip download in flight
    Ready,          // zip on disk, ready to invoke FM2KUpdater.exe
    Failed,         // anything went wrong; check error_detail
};

struct Snapshot {
    State        state          = State::Idle;
    std::string  remote_version;        // populated from State::UpdateAvailable onward
    uint32_t     downloaded_bytes = 0;  // populated during Downloading
    uint32_t     total_bytes      = 0;  // populated during Downloading (0 if unknown)
    std::string  error_detail;          // populated on State::Failed
    // Both channels' latest versions, refreshed on every CheckForUpdates.
    // Used by the menu-bar release-channel toggle to show what's available
    // on each side so the user can decide whether flipping is worth it.
    // Empty string = unknown (check hasn't completed or that channel has
    // no releases yet).
    std::string  latest_stable;
    std::string  latest_dev;
    std::string  latest_bleeding;       // newest -bleeding-tagged prerelease
};

// Kick off a non-blocking version check. Safe to call any time;
// no-op if a check or download is already in flight.
void CheckForUpdates();

// Trigger the zip download. Valid only when state is UpdateAvailable;
// no-op otherwise.
void StartDownload();

// Spawn FM2KUpdater.exe and exit the current process. Valid only when
// state is Ready. Returns false if the spawn failed (state stays
// Ready so the user can click again).
bool ApplyUpdateAndExit();

// Snapshot the current state for UI rendering. Cheap.
Snapshot Get();

// True when remote is OLDER than local -- i.e. the channel they're on
// has a version below their installed one. Common case: user is on a
// dev build (0.2.55) and flips channel to stable (latest 0.2.54). UI
// uses this to switch the pill copy from "Update X -> Y" to
// "Switch X -> Y" so the user knows they're going backwards.
bool IsRemoteOlderThanLocal();

// Tear down the worker thread (called from launcher shutdown).
void Shutdown();

}  // namespace fm2k::updater
