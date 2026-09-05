// host_scan.h -- game discovery and its on-disk caches.
//
// Split out of host.cpp when that file crossed the repo's 1000-line
// ceiling. The seam is by CONCERN, not by line count: everything here
// answers "what games are installed and what do they look like", and none
// of it knows the UI exists. host.cpp keeps the window, the event loop and
// the FFI calls.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Published by the scan thread, drained by the main loop. g_scan_done is
// an exchange-once latch: reading it clears it.
extern std::mutex g_scan_mutex;
extern std::string g_scan_result;
extern std::atomic<bool> g_scan_done;
extern std::atomic<bool> g_scanning;
extern std::atomic<int> g_scan_seen;
extern std::atomic<int> g_scan_hits;
extern std::atomic<int> g_scan_cached;

void LoadCache();
void SaveCache();
void LoadThumbIndex();
void StartScan(std::vector<std::string> roots);
// The library as the cache last saw it -- published before the walk so the
// rail is up on the first frame; the finished scan replaces it.
std::string RowsFromCache();
std::vector<std::string> LoadRoots(bool* onboarded);
void SaveRoots(const std::vector<std::string>& roots, bool onboarded);

// User preferences: a settings bitfield (see slint_ui_set_config) and the
// hub display name. Same ini as the roots, since it is already the spike's
// own config file and a second one would just be another thing to find.
void LoadPrefs(int* bits, std::string* nick);
void SavePrefs(int bits, const std::string& nick);

// Most-recently-played game titles, newest first. Persisted so the rail
// still shows your games after a restart, which is the only way a "recent"
// list is worth anything.
// The hub the user last connected to. Stored so the Network tab comes back
// pointing where they left it.
std::string LoadHubHost();
void SaveHubHost(const std::string& host);

std::vector<std::string> LoadMru();
void SaveMru(const std::vector<std::string>& mru);
