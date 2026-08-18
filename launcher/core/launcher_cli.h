// launcher_cli.h -- command-line handling for the launcher, split out of
// FM2K_RollbackClient.cpp's SDL_AppInit. Three stages:
//   1. LauncherCli_ParseArgs       -- argv -> LauncherCliArgs
//   2. LauncherCli_RunHeadlessProbe-- --upnp-test / --nat-test (run before the
//      launcher/SDL exists, then exit the process)
//   3. LauncherCli_ApplyLaunchMode -- after Initialize(), drive replay / stress
//      / offline / direct(host,connect,spectate) launch modes
#pragma once

#include "SDL3/SDL.h"          // SDL_AppResult
#include "FM2K_Integration.h"  // NetworkConfig, FM2KLauncher
#include <string>
#include <cstdint>

// Mirror of the locals SDL_AppInit used to declare inline; defaults match the
// originals so the parse loop body moves verbatim.
struct LauncherCliArgs {
    NetworkConfig config;
    bool direct_mode = false;
    bool spectate_mode = false;
    bool stress_mode_cli = false;        // --stress: auto-launches stress determinism test on first game in scan
    std::string stress_game_filter;       // --stress <name>: filter discovered games by substring (case-insensitive)
    bool offline_mode_cli = false;       // --offline: auto-launches the pure FM2K_TRUE_OFFLINE native path (no rollback) for perf profiling
    std::string offline_game_filter;      // --offline <name|path>: substring filter or direct .exe path
    std::string direct_game_filter;       // --host/--connect <name-or-path>: pick a specific game instead of "first discovered"
    std::string spectate_target_addr;     // "host_ip:host_port" for --spectate
    std::string spectate_session_kind = "battle";  // --spectate-session-kind {menu,css,battle}
    int spectate_player_index = 2;        // --player-index N: distinct FM2K_P{N+1}_Debug.log per concurrent spectator
    std::string replay_file_path;         // "--replay <path>" -- offline .fm2krep playback
    bool upnp_test_cli = false;           // --upnp-test: discover->map->report->unmap then exit
    uint16_t upnp_test_port = 7000;       // --upnp-test [port]
    bool nat_test_cli = false;            // --nat-test <hub_host> [port]
    std::string nat_test_host;            // --nat-test <hub_host>
    uint16_t nat_test_port = 7000;        // --nat-test <hub_host> [port]
};

// Parse argv. Returns SDL_APP_CONTINUE on success, SDL_APP_FAILURE on a bad arg
// (after printing the error to stderr).
SDL_AppResult LauncherCli_ParseArgs(int argc, char** argv, LauncherCliArgs& out);

// Self-contained headless probes (--upnp-test / --nat-test) that run before the
// launcher/SDL is created and exit the process. Returns true if one ran (caller
// returns *result); false to continue normal startup.
bool LauncherCli_RunHeadlessProbe(const LauncherCliArgs& args, SDL_AppResult* result);

// After launcher Initialize(), apply replay / stress / offline / direct launch
// mode. Returns SDL_APP_CONTINUE normally (modes fall through to the headless
// main loop), SDL_APP_FAILURE on a fatal error.
SDL_AppResult LauncherCli_ApplyLaunchMode(FM2KLauncher* launcher, LauncherCliArgs& args);
