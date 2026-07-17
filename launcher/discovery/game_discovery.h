#pragma once
// Game discovery subsystem: filesystem scan for FM2K/FM95 games, exe/engine
// sniffing, the on-disk games cache, and the async discovery worker thread.
// Extracted verbatim from FM2K_RollbackClient.cpp (pure move, no behavior
// change) to shrink that file. The implementation lives in game_discovery.cpp;
// this header is just the narrow surface the rest of the launcher calls.

#include <string>
#include <vector>
#include <SDL3/SDL.h>            // Uint32
#include "FM2K_Integration.h"   // FM2K::FM2KGameInfo, FM2KLauncher

// Discovery-completion SDL event type. Registered in the FM2KLauncher ctor,
// read in FM2KLauncher::HandleEvent, and written by the discovery worker thread
// (DiscoveryThreadFunc, in game_discovery.cpp). Defined in game_discovery.cpp.
extern Uint32 g_event_discovery_complete;

// The handful of discovery helpers called from code that stays in
// FM2K_RollbackClient.cpp (config + cache load/save). The rest of the Utils
// namespace is internal to game_discovery.cpp.
namespace Utils {
    std::vector<std::string> LoadGamesRootPaths();
    void SaveGamesRootPaths(const std::vector<std::string>& paths);
    std::vector<FM2K::FM2KGameInfo> LoadGameCache();
    void SaveGameCache(const std::vector<FM2K::FM2KGameInfo>& games);
    // Engine identification for a single exe (known-hash registry -> string
    // sniff -> size heuristic). For launch paths that bypass the discovery
    // scan (CLI direct-path launches) -- the FM2KGameInfo engine default is
    // FM2K, which would inject the wrong hook DLL into an FM95 exe.
    FM2K::Engine DetectEngineForExe(const std::string& exe_path);
}
