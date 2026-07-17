// launcher_cli.cpp -- command-line parse + launch-mode application, split out
// of FM2K_RollbackClient.cpp's SDL_AppInit. The two big verbatim blocks (the
// argv parse loop and the replay/stress/offline/direct launch modes) live here
// behind small wrapper functions; reference aliases let the bodies move
// unchanged. The --upnp-test / --nat-test probes stay inline in SDL_AppInit
// (they exit the process). See launcher_cli.h for the staging contract.
#include "SDL3/SDL.h"
#include <SDL3_image/SDL_image.h>
#include "app_icon_data.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "MinHook.h"
#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include "game_discovery.h"  // game scan/cache/sniff + async discovery (moved out of this file)
#include "FM2K_GameIni.h"
#include "FM2K_Utf8Path.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/ui/input_binder.h"  // RefreshGamepads() on SDL hot-plug events
#include "FM2KHook/src/netplay/spec_relay_queue.h"  // hub-relay outbound drain (Phase 2c)
#define XXH_INLINE_ALL
#include "vendored/xxhash/xxhash.h"
#include "LocalSession.h"
#include "OnlineSession.h"
#include "FM2K_PortMapper.h"  // --upnp-test self-contained router validation

#include <chrono>
#include <string>
#include <cstring>
#include <cstdlib>  // std::getenv for FM2K_FULL_CRCS perf-run override
#include <optional>
#include <vector>
#include <iostream>
#include <thread>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <system_error>
#include "launcher_cli.h"

SDL_AppResult LauncherCli_ParseArgs(int argc, char** argv, LauncherCliArgs& out) {
    // Aliases so the original parse loop body moves verbatim.
    NetworkConfig& config             = out.config;
    bool& direct_mode                 = out.direct_mode;
    bool& spectate_mode               = out.spectate_mode;
    bool& stress_mode_cli             = out.stress_mode_cli;
    std::string& stress_game_filter   = out.stress_game_filter;
    bool& offline_mode_cli            = out.offline_mode_cli;
    std::string& offline_game_filter  = out.offline_game_filter;
    std::string& direct_game_filter   = out.direct_game_filter;
    std::string& spectate_target_addr = out.spectate_target_addr;
    std::string& spectate_join_mode   = out.spectate_join_mode;
    std::string& spectate_session_kind= out.spectate_session_kind;
    int& spectate_player_index        = out.spectate_player_index;
    std::string& replay_file_path     = out.replay_file_path;
    bool& upnp_test_cli               = out.upnp_test_cli;
    uint16_t& upnp_test_port          = out.upnp_test_port;
    bool& nat_test_cli                = out.nat_test_cli;
    std::string& nat_test_host        = out.nat_test_host;
    uint16_t& nat_test_port           = out.nat_test_port;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" || arg == "-h") {
            config.is_host = true;
            direct_mode = true;
            // Optional next arg: game-name substring OR absolute path to
            // game .exe (same convention as --stress). Picks the specific
            // game instead of "first discovered" — the test harness needs
            // this to target WonderfulWorld instead of whichever game
            // ends up alphabetically first in the launcher registry.
            if (i + 1 < argc && argv[i+1][0] != '-') {
                direct_game_filter = argv[++i];
            }
        } else if (arg == "--connect" || arg == "-c") {
            if (i + 1 < argc) {
                config.remote_address = argv[++i];
                config.is_host = false;
                direct_mode = true;
                // Optional second arg: game filter, same as --host.
                if (i + 1 < argc && argv[i+1][0] != '-') {
                    direct_game_filter = argv[++i];
                }
            } else {
                std::cerr << "Error: --connect requires an address\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--spectate-mode") {
            // --spectate-mode {current,full}
            //   current = CURRENT_MATCH (default; CCCaster-style snapshot join)
            //   full    = FULL_SESSION  (replay from frame 0)
            if (i + 1 < argc) {
                std::string m = argv[++i];
                if (m == "current" || m == "full") {
                    spectate_join_mode = m;
                } else {
                    std::cerr << "Error: --spectate-mode must be 'current' or 'full', got: " << m << "\n";
                    return SDL_APP_FAILURE;
                }
            } else {
                std::cerr << "Error: --spectate-mode requires {current,full}\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--spectate-session-kind") {
            // --spectate-session-kind {menu,css,battle}
            // Override the host's session_kind for CLI --spectate (no
            // hub to read it from). "battle" sets FM2K_BOOT_TO_BATTLE
            // for instant join; "menu"/"css" walks the title→CSS path.
            if (i + 1 < argc) {
                std::string k = argv[++i];
                if (k == "menu" || k == "css" || k == "battle") {
                    spectate_session_kind = k;
                } else {
                    std::cerr << "Error: --spectate-session-kind must be "
                                 "{menu,css,battle}, got: " << k << "\n";
                    return SDL_APP_FAILURE;
                }
            } else {
                std::cerr << "Error: --spectate-session-kind requires "
                             "{menu,css,battle}\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--replay") {
            // --replay <path-to-.fm2krep-or-.fm2kset>
            // Launches the game as a spectator instance with no network
            // connection; the hook reads FM2K_REPLAY_FILE on init and
            // SpectatorNode_LoadSessionFile populates pb_queue from the
            // file. Trampoline's RunSpectatorTick consumes events and
            // drives the sim forward, identical to a live spectator.
            if (i + 1 < argc) {
                replay_file_path = argv[++i];
            } else {
                std::cerr << "Error: --replay requires <path>\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--spectate" || arg == "--spec") {
            // Spectate a remote host: --spectate <ip:port>
            // Skips netplay handshake and stands the game up as a passive
            // viewer dialing the host's spectator listener. Matches the UI
            // path's LaunchRemoteSpectator + the FM2K_SPECTATOR_MODE=1 hook
            // gate.
            if (i + 1 < argc) {
                spectate_target_addr = argv[++i];
                spectate_mode = true;
            } else {
                std::cerr << "Error: --spectate requires <host_ip:host_port>\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--player-index") {
            // Distinct index per concurrent spectator -> distinct
            // FM2K_P{N+1}_Debug.log + parity. Default 2 (=> P3).
            if (i + 1 < argc) {
                spectate_player_index = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: --player-index requires N\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--port" || arg == "-p") {
            if (i + 1 < argc) {
                config.local_port = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: --port requires a port number\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--remote" || arg == "-r") {
            // Override the remote peer address regardless of host/guest
            // role. --connect already sets remote_address for the guest;
            // --host leaves it at the placeholder (the hook then learns
            // the peer from the first inbound HELLO). --remote lets the
            // HOST be pointed at a specific peer/blackhole too, which the
            // relay self-test needs: pointing both peers at a TEST-NET-1
            // blackhole makes the direct punch fail on BOTH sides so the
            // hub relay engages. StartOnlineSession only clears the host
            // remote when it equals the "127.0.0.1:7001" placeholder, so
            // any other value (e.g. 192.0.2.1:6001) flows through to the
            // hook's StartPunch.
            if (i + 1 < argc) {
                config.remote_address = argv[++i];
            } else {
                std::cerr << "Error: --remote requires an address\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--delay" || arg == "-d") {
            if (i + 1 < argc) {
                config.input_delay = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: --delay requires a frame count\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--games") {
            if (i + 1 < argc) {
                // Treat --games as "set the games-roots list to a single
                // path" for parity with the legacy command line. Users who
                // want multiple roots can configure them in the UI.
                Utils::SaveGamesRootPaths({ argv[++i] });
            }
        } else if (arg == "--stress") {
            // Auto-launches GekkoStressSession determinism test on the
            // first discovered game (or the first match of a
            // case-insensitive substring filter passed as the next arg)
            // and exits the launcher when the test game terminates.
            // Forces a rollback every check_distance=10 frames and
            // fires GekkoDesyncDetected on any forward-vs-replay
            // mismatch — exactly the test we need to validate
            // Phase F (#23) fixes.
            stress_mode_cli = true;
            // Optional next arg: game-name substring (e.g. "wanwan"
            // or "pkmncc"). The first arg that DOESN'T start with `--`
            // is the filter; lets us pick a specific known-active game
            // instead of "first alphabetical" (HHRTFG idles too much
            // for the RNG-determinism check to fire).
            if (i + 1 < argc && argv[i+1][0] != '-') {
                stress_game_filter = argv[++i];
            }
        } else if (arg == "--offline") {
            // Auto-launches the pure offline native path (FM2K_TRUE_OFFLINE)
            // on a matched game and skips the launcher UI -- the path Yamada's
            // Robot Heroes slowdown lives on. Unlike --stress this runs NO
            // GekkoNet/rollback, so the [OFFLINE-SECT]/[FRAMETIME] perf
            // instruments (FM2K_PERF_PROFILE=1) measure the real offline
            // per-frame cost. Optional next arg = game substring or direct
            // .exe path (same convention as --stress).
            offline_mode_cli = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                offline_game_filter = argv[++i];
            }
        } else if (arg == "--upnp-test") {
            // Manual router validation for the Phase 1 UPnP port mapper.
            // Runs PortMapper::StartAsync on a test port, waits a few
            // seconds for the off-thread discovery+map to land, prints the
            // full Status, tears the mapping down, and exits -- without
            // ever spawning the launcher UI or a game. This is how the user
            // checks UPnP against their real router. Optional next arg = the
            // UDP port to map (default 7000), so the user can match the port
            // their game actually binds.
            upnp_test_cli = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                upnp_test_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            }
        } else if (arg == "--nat-test") {
            // Manual / harness validation for the Phase 2a NAT classifier.
            // Runs ONLY fm2k::LauncherStunClassify against a given hub
            // (UDP-STUN on :7711, classification reflector on :7714),
            // prints nat_type + the two reflexive ports, and exits -- no
            // launcher UI, no game, no WS. Drives the same probe the lobby
            // uses at the Connected event. On a local hub (127.0.0.1) this
            // expects "cone" (same external port from both responders).
            // Args: --nat-test <hub_host> [local_port]. local_port defaults
            // to 7000 (the game's default bind).
            nat_test_cli = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                nat_test_host = argv[++i];
            }
            if (i + 1 < argc && argv[i+1][0] != '-') {
                nat_test_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            }
        }
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult LauncherCli_ApplyLaunchMode(FM2KLauncher* launcher, LauncherCliArgs& args) {
    // Aliases so the original replay/stress/offline/direct blocks move verbatim.
    FM2KLauncher* g_launcher          = launcher;
    NetworkConfig& config             = args.config;
    bool& direct_mode                 = args.direct_mode;
    bool& spectate_mode               = args.spectate_mode;
    bool& stress_mode_cli             = args.stress_mode_cli;
    std::string& stress_game_filter   = args.stress_game_filter;
    bool& offline_mode_cli            = args.offline_mode_cli;
    std::string& offline_game_filter  = args.offline_game_filter;
    std::string& direct_game_filter   = args.direct_game_filter;
    std::string& spectate_target_addr = args.spectate_target_addr;
    std::string& spectate_join_mode   = args.spectate_join_mode;
    std::string& spectate_session_kind= args.spectate_session_kind;
    std::string& replay_file_path     = args.replay_file_path;

    // Replay mode (offline file playback). Skip UI, no network, no peer.
    // Launches a spectator instance with FM2K_REPLAY_FILE pointing at
    // the .fm2krep / .fm2kset; the hook's Netplay_InitAsSpectator reads
    // the env var at init and SpectatorNode_LoadSessionFile drains the
    // file's events into pb_queue.
    //
    // Game-exe lookup: the replay file is always written to
    // <game_dir>/replays/<timestamp>.fm2krep (see WriteCurrentBattleFile),
    // so game_dir is the replay's grandparent. We scan that directory
    // for the .exe directly — no dependency on the async discovery
    // thread, which hasn't populated yet at this point in startup.
    if (!replay_file_path.empty()) {
        std::error_code ec;
        std::filesystem::path replay_fs = std::filesystem::u8path(replay_file_path);
        std::filesystem::path canon = std::filesystem::weakly_canonical(replay_fs, ec);
        if (ec) canon = replay_fs;
        std::filesystem::path game_dir = canon.parent_path().parent_path();

        // FM2K games always ship with <project>.kgt next to <project>.exe.
        // Find the .kgt's stem and use it to pick the matching .exe — this
        // skips bundled helper binaries like antimicrox.exe that are 64-bit
        // and would fail injection.
        std::string game_exe_path;
        if (!game_dir.empty() && std::filesystem::is_directory(game_dir, ec)) {
            std::filesystem::path kgt_stem;
            for (const auto& entry : std::filesystem::directory_iterator(game_dir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                if (ext == ".kgt") {
                    kgt_stem = entry.path().stem();
                    break;
                }
            }
            if (!kgt_stem.empty()) {
                std::filesystem::path candidate = game_dir / kgt_stem;
                candidate += ".exe";
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    game_exe_path = candidate.string();
                }
            }
        }
        if (game_exe_path.empty()) {
            std::cerr << "Replay: could not locate game .exe under "
                      << game_dir.string() << "\n";
            return SDL_APP_FAILURE;
        }

        if (!g_launcher->LaunchReplayPlayer(game_exe_path, replay_file_path)) {
            std::cerr << "Replay: launch failed\n";
            return SDL_APP_FAILURE;
        }
        g_launcher->SetExitOnGameEnd(true);  // no lobby: quit when replay ends
        std::cout << "Replay mode: playing " << replay_file_path
                  << " (game=" << game_exe_path << ")\n";
        // Fall through to the launcher's headless main loop so the
        // replay-instance lifetime is managed normally.
    }

    // --stress mode: drive Phase F determinism test from the CLI so we
    // don't need to UI-click. Launches the first discovered game with
    // FM2K_STRESS_MODE=1; the hook creates a GekkoStressSession that
    // forces a rollback every check_distance=10 frames and fires
    // GekkoDesyncDetected on any forward-vs-replay mismatch. Watch
    // <game_dir>/logs/FM2K_P1_Debug.log for DESYNC events.
    if (stress_mode_cli) {
        // Direct-path bypass: if --stress <filter> looks like a literal
        // path to a .exe (contains a separator, ends in .exe, file
        // exists), skip the launcher.cfg-rooted DiscoverGames scan
        // entirely and just launch that EXE. Lets the replay-selftest
        // harness target a specific game without waiting on a
        // multi-thousand-file recursive scan of D:\Games\fm2k\.
        std::vector<FM2K::FM2KGameInfo> games;
        const bool is_direct_path = !stress_game_filter.empty()
            && (stress_game_filter.find('/')  != std::string::npos ||
                stress_game_filter.find('\\') != std::string::npos);
        if (is_direct_path) {
            FM2K::FM2KGameInfo info{};
            info.exe_path = stress_game_filter;
            // Discovery normally sniffs the engine; a direct-path launch
            // bypasses the scan and the FM2KGameInfo default (FM2K) would
            // inject FM2KHook.dll into an FM95 exe. Run the same cascade
            // (known-hash -> string sniff -> size heuristic) here. Other
            // fields (clean_label, etc) keep their defaults -- the launcher
            // only needs exe_path + engine to drive StartStressSession.
            info.engine = Utils::DetectEngineForExe(info.exe_path);
            games.push_back(info);
            stress_game_filter.clear();  // suppress the substring matcher below
        } else {
            // Force a SYNCHRONOUS scan here so we have a games list to
            // pick from. The launcher's async discovery thread will still
            // run later but won't change the selection we made.
            games = g_launcher->DiscoverGames();
        }
        if (games.empty()) {
            std::cerr << "No FM2K games found for --stress (scanned launcher.cfg roots)\n";
            return SDL_APP_FAILURE;
        }
        // Boot the game directly into battle so GekkoStressSession
        // actually exercises rollback during the test run. Without
        // these vars the game sits at title screen waiting for input
        // and the determinism check never fires.
        ::SetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE", "1");
        ::SetEnvironmentVariableA("FM2K_AUTO_TITLE_SKIP", "1");
        ::SetEnvironmentVariableA("FM2K_PARITY_AUTOPLAY", "1");
        // Phase F autonomous stress: keep mashing buttons during battle
        // so RNG-consuming actions (hits, scripts, projectiles) fire
        // and we exercise the active-gameplay code path that user-
        // reported desyncs cluster on. Idle stress passed 21k+ frames
        // clean. The --stress harness implies "actually try to break
        // things," not "let the engine idle."
        ::SetEnvironmentVariableA("FM2K_PARITY_AUTOPLAY_BATTLE", "1");
        // Force per-save full per-region CRC so the desync diagnostic
        // dump can attribute first-divergent region (vs the default
        // 1/sec throttle that shows everything as 0x0). Respect an
        // explicit FM2K_FULL_CRCS=0 from the environment so a perf run
        // (FM2K_PERF_PROFILE=1) can measure the TRUE production save cost
        // without the ~1.2ms/save diagnostic hash that stress mode adds.
        if (const char* fc = std::getenv("FM2K_FULL_CRCS"); !(fc && fc[0] == '0')) {
            ::SetEnvironmentVariableA("FM2K_FULL_CRCS", "1");
        }
        // Apply optional --stress <filter> game-name substring match
        // (case-insensitive). Default = first discovered.
        size_t pick = 0;
        if (!stress_game_filter.empty()) {
            auto lowercase = [](std::string s) {
                for (auto& c : s) c = (char)std::tolower((unsigned char)c);
                return s;
            };
            std::string needle = lowercase(stress_game_filter);
            bool found = false;
            for (size_t i = 0; i < games.size(); ++i) {
                if (lowercase(games[i].exe_path).find(needle) != std::string::npos) {
                    pick = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "--stress: no game matched filter '"
                          << stress_game_filter << "'; falling back to first\n";
            }
        }
        const auto& game_to_launch = games[pick];
        g_launcher->SetSelectedGame(game_to_launch);
        g_launcher->StartStressSession();
        g_launcher->SetState(LauncherState::InGame);
        g_launcher->SetExitOnGameEnd(true);  // no lobby: quit when game ends
        std::cout << "Stress mode: GekkoStressSession started for "
                  << game_to_launch.exe_path
                  << " — watch logs/FM2K_P1_Debug.log\n";
    }

    if (offline_mode_cli) {
        // Mirror of the --stress resolution, but launches the pure offline
        // native path for perf profiling. Boots straight to battle (skips the
        // CSS, which crashes on some games like Robot Heroes) and idles --
        // NO autoplay-mash, so the per-frame cost is stage/object dominated
        // and stable across runs. Stage is pinned via FM2K_BTB_STAGE (passed
        // through from the environment); FM2K_PERF_PROFILE / FM2K_DUMP_STAGES
        // likewise flow through to the spawned game.
        std::vector<FM2K::FM2KGameInfo> games;
        const bool is_direct_path = !offline_game_filter.empty()
            && (offline_game_filter.find('/')  != std::string::npos ||
                offline_game_filter.find('\\') != std::string::npos);
        if (is_direct_path) {
            FM2K::FM2KGameInfo info{};
            info.exe_path = offline_game_filter;
            // Direct-path launch bypasses discovery's engine sniff; the
            // default (FM2K) would inject the wrong hook DLL into an FM95
            // exe. Same fix as --stress above.
            info.engine = Utils::DetectEngineForExe(info.exe_path);
            games.push_back(info);
            offline_game_filter.clear();
        } else {
            games = g_launcher->DiscoverGames();
        }
        if (games.empty()) {
            std::cerr << "No FM2K games found for --offline\n";
            return SDL_APP_FAILURE;
        }
        // --offline defaults to boot-straight-to-battle so perf profiling
        // lands in gameplay, but RESPECT an explicit FM2K_BOOT_TO_BATTLE
        // from the environment (a CSS test batch sets =0 to walk the normal
        // title->CSS path for character-select churn testing). Same opt-out
        // pattern as FM2K_PARITY_RECORD_PATH below. GetEnvironmentVariableA
        // returns 0 only when the var is unset; any explicit value wins.
        if (::GetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE", nullptr, 0) == 0) {
            ::SetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE", "1");
        }
        ::SetEnvironmentVariableA("FM2K_AUTO_TITLE_SKIP", "1");
        size_t pick = 0;
        if (!offline_game_filter.empty()) {
            auto lowercase = [](std::string s) {
                for (auto& c : s) c = (char)std::tolower((unsigned char)c);
                return s;
            };
            std::string needle = lowercase(offline_game_filter);
            for (size_t i = 0; i < games.size(); ++i) {
                if (lowercase(games[i].exe_path).find(needle) != std::string::npos) {
                    pick = i;
                    break;
                }
            }
        }
        g_launcher->SetSelectedGame(games[pick]);
        g_launcher->StartOfflineSession();
        g_launcher->SetState(LauncherState::InGame);
        g_launcher->SetExitOnGameEnd(true);  // no lobby: quit when game ends
        std::cout << "Offline mode: native session started for "
                  << games[pick].exe_path
                  << " — watch logs/FM2K_P1_Debug.log [OFFLINE-SECT]/[FRAMETIME]\n";
    }

    // If direct mode, skip UI and go straight to game launch + network
    if (direct_mode || spectate_mode) {
        // Direct-path bypass: if --host/--connect <filter> is a literal
        // absolute path, build a one-element discovered-games list and
        // launch that — skips the registry-scan entirely (matches --stress
        // semantics for harness use). Otherwise treat as substring filter
        // against the discovered games list.
        const bool is_direct_path = !direct_game_filter.empty()
            && (direct_game_filter.find('/')  != std::string::npos ||
                direct_game_filter.find('\\') != std::string::npos);

        FM2K::FM2KGameInfo selected{};
        if (is_direct_path) {
            selected.exe_path = direct_game_filter;
            // Direct-path launch bypasses discovery's engine sniff; the
            // default (FM2K) would inject the wrong hook DLL into an FM95
            // exe. Same fix as --stress above.
            selected.engine = Utils::DetectEngineForExe(selected.exe_path);
        } else {
            if (g_launcher->GetDiscoveredGames().empty()) {
                std::cerr << "No FM2K games found for direct mode\n";
                return SDL_APP_FAILURE;
            }
            if (!direct_game_filter.empty()) {
                auto lowercase = [](std::string s) {
                    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
                    return s;
                };
                std::string needle = lowercase(direct_game_filter);
                const auto& games = g_launcher->GetDiscoveredGames();
                bool matched = false;
                for (size_t i = 0; i < games.size(); ++i) {
                    if (lowercase(games[i].exe_path).find(needle) != std::string::npos) {
                        selected = games[i];
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    std::cerr << "--host/--connect: no game matched filter '"
                              << direct_game_filter << "'; falling back to first\n";
                    selected = games[0];
                }
            } else {
                selected = g_launcher->GetDiscoveredGames()[0];
            }
        }
        const auto& game_to_launch = selected;

        // Manually set the selected game for the launcher
        g_launcher->SetSelectedGame(game_to_launch);

        if (spectate_mode) {
            // Parse "host_ip:host_port" target.
            const size_t colon = spectate_target_addr.find(':');
            if (colon == std::string::npos) {
                std::cerr << "Error: --spectate target must be <ip:port>, got: "
                          << spectate_target_addr << "\n";
                return SDL_APP_FAILURE;
            }
            const std::string host_ip = spectate_target_addr.substr(0, colon);
            const int host_port       = std::stoi(spectate_target_addr.substr(colon + 1));
            const int spectator_port  = (config.local_port > 0) ? config.local_port : 7702;

            // CLI --spectate has no hub context, so session_kind isn't
            // forwarded — assume "battle" (the typical e2e use case is
            // joining an already-running match). To test the CSS-walk
            // path from CLI, pass `--spectate-session-kind menu`.
            if (!g_launcher->LaunchRemoteSpectator(game_to_launch.exe_path,
                                                    spectator_port,
                                                    host_ip, host_port,
                                                    spectate_session_kind,
                                                    spectate_join_mode,
                                                    "tcp",
                                                    args.spectate_player_index)) {
                std::cerr << "Spectate: launch failed\n";
                return SDL_APP_FAILURE;
            }
            std::cout << "Direct spectate mode: dialing " << host_ip << ":" << host_port
                      << " on local port " << spectator_port
                      << " (mode=" << spectate_join_mode << ")\n";
        } else {
            // Start regular host/client network session
            NetworkConfig online_config = config;
            online_config.session_mode = SessionMode::ONLINE;
            g_launcher->StartOnlineSession(online_config, config.is_host);
            std::cout << "Direct mode: game launched + network started\n";
        }

        g_launcher->SetState(LauncherState::InGame);
        // Direct CLI launch (--host/--connect/--spectate): no interactive lobby
        // to return to, so quit the launcher when the hosted game exits instead
        // of zombie-ing the window (the harness "launcher didnt kill" hang).
        g_launcher->SetExitOnGameEnd(true);
    }

    return SDL_APP_CONTINUE;
}
