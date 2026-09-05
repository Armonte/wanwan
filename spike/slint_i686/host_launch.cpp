// host_launch.cpp -- see host_launch.h.
#include "host_launch.h"

#include "FM2K_GameInstance.h"
#include "FM2K_GameIni.h"
#include "FM2K_HubClient.h"        // fm2k::HubEvent
#include "FM2K_Integration.h"
#include "FM2K_OnlineLaunch.h"     // the shared online launch
#include "FM2K_Utf8Path.h"        // fm2k::utf8path::Utf8ToWide
#include "launcher_ui_internal.h"   // lui::ApplyGamePatchEnvVars, LoadDevFlag*

#include <SDL3/SDL.h>
#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>

namespace hostgui {
namespace {

std::unique_ptr<FM2KGameInstance> g_game;
// Developer-tool instances: two loopback clients and two spectators.
std::unique_ptr<FM2KGameInstance> g_dev[4];
std::string g_game_exe;      // for RestoreFromBackup when it exits
Uint64 g_started_ms = 0;
bool   g_online = false;     // the current/last launch was a hub match

// The developer launch flags the ImGui launcher's Play Offline button
// exports (launcher_ui_netcfg.cpp), read from the same dev_flags.ini keys.
// Offline only by contract: the online paths clear these
// (NeutralizeDevLaunchEnvVars), and so does this file when the game exits.
void ExportDevLaunchFlags() {
    const bool fast_css   = lui::LoadDevFlagInt("boot_strategy", 0) == 1;
    const bool title_skip = lui::LoadDevFlag("auto_title_skip", true);
    ::SetEnvironmentVariableA("FM2K_BOOT_TO_CSS_DIRECT", fast_css ? "1" : nullptr);
    ::SetEnvironmentVariableA("FM2K_AUTO_TITLE_SKIP",    title_skip ? nullptr : "0");
    ::SetEnvironmentVariableA("FM2K_BYPASS_TRAMPOLINE",  lui::LoadDevFlag("bypass_trampoline", false) ? "1" : nullptr);
    ::SetEnvironmentVariableA("FM2K_FORCE_T4_PATCH",     lui::LoadDevFlag("force_t4_patch", false) ? "1" : nullptr);
    ::SetEnvironmentVariableA("FM2K_SKIP_VS_MODE_PATCH", lui::LoadDevFlag("skip_vs_mode_patch", false) ? "1" : nullptr);
    ::SetEnvironmentVariableA("FM2K_T4_PROBE",           lui::LoadDevFlag("t4_probe", false) ? "1" : nullptr);
    ::SetEnvironmentVariableA("FM2K_EB_DIAG",            lui::LoadDevFlag("eb_diag", false) ? "1" : nullptr);
    ::SetEnvironmentVariableA("FM95_TRAMPOLINE",         lui::LoadDevFlag("fm95_trampoline", false) ? "1" : nullptr);
    ::SetEnvironmentVariableA("FM2K_FAST_PLAYER_LOAD",   lui::LoadDevFlag("fast_player_load", false) ? "1" : nullptr);
    ::SetEnvironmentVariableA("FM2K_DEV_MODE",           lui::LoadDevFlag("dev_mode", false) ? "1" : nullptr);
    const bool btb = lui::LoadDevFlag("boot_to_battle", false);
    ::SetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE", btb ? "1" : nullptr);
    const char* keys[] = {"btb_p1_char", "btb_p2_char", "btb_stage", "btb_p1_meter", "btb_p2_meter"};
    const char* envs[] = {"FM2K_BTB_P1_CHAR", "FM2K_BTB_P2_CHAR", "FM2K_BTB_STAGE", "FM2K_BTB_P1_METER", "FM2K_BTB_P2_METER"};
    for (int i = 0; i < 5; ++i) {
        if (btb) ::SetEnvironmentVariableA(envs[i], std::to_string(lui::LoadDevFlagInt(keys[i], 0)).c_str());
        else     ::SetEnvironmentVariableA(envs[i], nullptr);
    }
}

}  // namespace

bool LaunchOffline(const std::string& exe_path_utf8, const std::string& engine,
                   const std::string& game_stem, std::string* err) {
    if (exe_path_utf8.empty()) { if (err) *err = "no game selected"; return false; }
    if (g_game && g_game->IsRunning()) g_game->Terminate();
    g_game = std::make_unique<FM2KGameInstance>();

    // Same contract as FM2KLauncher::StartOfflineSession.
    g_game->SetEnvironmentVariable("FM2K_TRUE_OFFLINE", "1");
    g_game->SetEnvironmentVariable("FM2K_PLAYER_INDEX", "0");
    g_game->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    g_game->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "1");
    ExportDevLaunchFlags();
    lui::ApplyGamePatchEnvVars(game_stem);

    // Per-game match settings (game_configs/<stem>.ini) onto the game's own
    // game.ini, backed up first; restored when the game exits.
    const std::filesystem::path exe = fm2k::utf8path::Utf8ToWide(exe_path_utf8);
    if (!fm2k::game_ini::ApplyForLaunch(exe, /*is_online=*/false)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Launch: ApplyForLaunch failed for %s (launching with the game's own ini)",
                    exe_path_utf8.c_str());
    }
    const FM2K::Engine eng = (engine == "FM95") ? FM2K::Engine::FM95 : FM2K::Engine::FM2K;
    if (!g_game->Launch(exe_path_utf8, eng)) {
        fm2k::game_ini::RestoreFromBackup(exe);
        g_game.reset();
        if (err) *err = "the game did not start -- see the log";
        return false;
    }
    g_game->SetNetworkConfig(false, false);
    g_game_exe = exe_path_utf8;
    g_started_ms = SDL_GetTicks();
    g_online = false;
    return true;
}

bool LaunchOnline(const std::string& exe_path_utf8, const std::string& engine,
                  const std::string& game_stem, const fm2k::HubEvent& ev,
                  const std::string& my_id, std::string* err) {
    if (exe_path_utf8.empty()) { if (err) *err = "game not in your library"; return false; }
    if (g_game && g_game->IsRunning()) g_game->Terminate();
    (void)game_stem;

    // Same contract as the ImGui launcher's K::MatchStart handler followed
    // by FM2KLauncher::StartOnlineSession, through the one shared module.
    NetworkConfig cfg;                       // local_port 7000, as the launcher's default
    cfg.session_mode = SessionMode::ONLINE;
    cfg.is_host = (ev.match.role == "host");
    const auto plan = fm2k::online_launch::ProbePeer(
        static_cast<uint16_t>(cfg.local_port), ev);
    cfg.remote_address = plan.peer_ip + ":" + std::to_string(plan.peer_port);

    // This player's SOCD mode: host is P1, guest is P2. Same settings.ini
    // keys the ImGui launcher reads (LoadSocdState); the binder's per-player
    // mode, when set, still wins inside the hook.
    const int local_slot = cfg.is_host ? 0 : 1;
    int socd = lui::ReadIntSetting(lui::NotifySettingsPath(),
                                   local_slot == 0 ? "socd_mode_p1" : "socd_mode_p2", 1);
    if (socd < 0 || socd > 5) socd = 1;
    fm2k::online_launch::ExportMatchEnv(ev, my_id, socd);

    g_game = std::make_unique<FM2KGameInstance>();
    // NOT PORTED YET: the match outcome the hook reports through shared
    // memory (the ImGui launcher's PollMatchOutcome -> match_result). A
    // match played from here reaches the hub as started and ended, with no
    // winner recorded. Loud so nobody mistakes a missing record for a bug
    // elsewhere.
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Launch: online match from the Slint launcher -- match_result "
                "reporting is not ported, the hub will record no winner");
    using fm2k::online_launch::StartResult;
    const FM2K::Engine eng = (engine == "FM95") ? FM2K::Engine::FM95 : FM2K::Engine::FM2K;
    const StartResult r = fm2k::online_launch::StartOnlineGame(
        *g_game, exe_path_utf8, eng, cfg, cfg.is_host);
    if (r != StartResult::Ok) {
        g_game.reset();
        if (err) *err = (r == StartResult::IniRefused)
            ? "could not write the game's settings for an online match (is the game under Program Files?)"
            : "the game did not start -- see the log";
        return false;
    }
    g_game_exe = exe_path_utf8;
    g_started_ms = SDL_GetTicks();
    g_online = true;
    return true;
}

bool LaunchWasOnline() { return g_online; }

namespace {
FM2K::Engine EngineOf(const std::string& engine) {
    return (engine == "FM95") ? FM2K::Engine::FM95 : FM2K::Engine::FM2K;
}
bool DevStart(std::unique_ptr<FM2KGameInstance>& slot, const std::string& exe,
              const std::string& engine, std::string* err) {
    if (!slot->Launch(exe, EngineOf(engine))) {
        slot.reset();
        if (err) *err = "the game did not start -- see the log";
        return false;
    }
    SDL_Delay(100);
    if (!slot->IsRunning()) {
        slot.reset();
        if (err) *err = "the game exited immediately after launch";
        return false;
    }
    return true;
}
}  // namespace

bool DevLaunchDual(const std::string& exe, const std::string& engine, std::string* err) {
    if (exe.empty()) { if (err) *err = "no game selected"; return false; }
    for (int i = 0; i < 2; ++i) {
        if (g_dev[i] && g_dev[i]->IsRunning()) { if (err) *err = "dual clients already running"; return false; }
        g_dev[i] = std::make_unique<FM2KGameInstance>();
        auto& c = *g_dev[i];
        const bool host = (i == 0);
        c.SetEnvironmentVariable("FM2K_PLAYER_INDEX", host ? "0" : "1");
        c.SetEnvironmentVariable("FM2K_LOCAL_PORT", host ? "7000" : "7001");
        // Loopback + a fixed seed: LOCAL TEST ONLY (process_manager.cpp
        // says why; the matchmade path derives its seed from the match).
        c.SetEnvironmentVariable("FM2K_REMOTE_ADDR", host ? "127.0.0.1:7001" : "127.0.0.1:7000");
        c.SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
        c.SetEnvironmentVariable("FM2K_INPUT_RECORDING", "1");
        c.SetEnvironmentVariable("FM2K_FORCE_RNG_SEED", "12345678");
        if (!DevStart(g_dev[i], exe, engine, err)) return false;
    }
    return true;
}

bool DevLaunchSpectator(int which, const std::string& exe, const std::string& engine, std::string* err) {
    if (exe.empty()) { if (err) *err = "no game selected"; return false; }
    const int i = (which == 2) ? 3 : 2;
    if (g_dev[i] && g_dev[i]->IsRunning()) { if (err) *err = "that spectator is already running"; return false; }
    g_dev[i] = std::make_unique<FM2KGameInstance>();
    auto& s = *g_dev[i];
    // Player index 2 is the spectator sentinel; the hook sends
    // SPEC_JOIN_REQ to the host instead of HELLO.
    s.SetEnvironmentVariable("FM2K_PLAYER_INDEX", "2");
    s.SetEnvironmentVariable("FM2K_LOCAL_PORT", (which == 2) ? "7003" : "7002");
    s.SetEnvironmentVariable("FM2K_REMOTE_ADDR", "127.0.0.1:7000");
    s.SetEnvironmentVariable("FM2K_SPECTATOR_MODE", "1");
    s.SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    s.SetEnvironmentVariable("FM2K_INPUT_RECORDING", "0");
    s.SetEnvironmentVariable("FM2K_FORCE_RNG_SEED", "12345678");
    return DevStart(g_dev[i], exe, engine, err);
}

bool DevLaunchStress(const std::string& exe, const std::string& engine, std::string* err) {
    if (exe.empty()) { if (err) *err = "no game selected"; return false; }
    if (g_game && g_game->IsRunning()) g_game->Terminate();
    g_game = std::make_unique<FM2KGameInstance>();
    // A determinism harness: no leaked offline patches or dev flags.
    lui::NeutralizeGamePatchEnvVars();
    lui::NeutralizeDevLaunchEnvVars();
    g_game->SetEnvironmentVariable("FM2K_TRUE_OFFLINE", "0");
    g_game->SetEnvironmentVariable("FM2K_STRESS_MODE", "1");
    g_game->SetEnvironmentVariable("FM2K_PLAYER_INDEX", "0");
    g_game->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    if (engine == "FM95") g_game->SetEnvironmentVariable("FM95_TRAMPOLINE", "1");
    if (!DevStart(g_game, exe, engine, err)) return false;
    g_game_exe = exe;
    g_started_ms = SDL_GetTicks();
    g_online = false;
    return true;
}

bool DevLaunchDirect(const std::string& exe, const std::string& engine, bool host,
                     const std::string& remote, std::string* err) {
    if (exe.empty()) { if (err) *err = "no game selected"; return false; }
    if (!host && remote.empty()) { if (err) *err = "enter the host's ip:port first"; return false; }
    if (g_game && g_game->IsRunning()) g_game->Terminate();
    g_game = std::make_unique<FM2KGameInstance>();
    NetworkConfig cfg;
    cfg.session_mode = SessionMode::ONLINE;
    cfg.is_host = host;
    cfg.local_port = host ? 7000 : 7001;
    // The host learns its peer from HELLO (empty remote, see
    // FM2K_OnlineLaunch.cpp); the joiner dials what was typed.
    cfg.remote_address = host ? "" : remote;
    using fm2k::online_launch::StartResult;
    const StartResult r = fm2k::online_launch::StartOnlineGame(*g_game, exe, EngineOf(engine), cfg, host);
    if (r != StartResult::Ok) {
        g_game.reset();
        if (err) *err = (r == StartResult::IniRefused)
            ? "could not write the game's settings for an online match"
            : "the game did not start -- see the log";
        return false;
    }
    g_game_exe = exe;
    g_started_ms = SDL_GetTicks();
    g_online = false;   // no hub match to report on
    return true;
}

void DevTerminateAll() {
    for (auto& d : g_dev) { if (d && d->IsRunning()) d->Terminate(); d.reset(); }
    LaunchTerminate();
}

uint32_t LaunchPid() {
    return (g_game && g_game->IsRunning()) ? (uint32_t)g_game->GetProcessId() : 0u;
}

int LaunchTick() {
    if (!g_game) return 0;
    if (g_game->IsRunning()) return 1;
    // It exited: put game.ini back and clear the offline-only flags so a
    // later online launch cannot inherit them.
    fm2k::game_ini::RestoreFromBackup(fm2k::utf8path::Utf8ToWide(g_game_exe));
    lui::NeutralizeGamePatchEnvVars();
    lui::NeutralizeDevLaunchEnvVars();
    g_game.reset();
    g_started_ms = 0;
    return 0;
}

int LaunchElapsedSeconds() {
    return g_started_ms ? (int)((SDL_GetTicks() - g_started_ms) / 1000) : 0;
}

void LaunchTerminate() {
    if (g_game && g_game->IsRunning()) g_game->Terminate();
    LaunchTick();
}

void LaunchShutdown() { LaunchTerminate(); }

}  // namespace hostgui
