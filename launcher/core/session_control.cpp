// session_control.cpp -- FM2KLauncher session lifecycle (offline / stress /
// online start + stop), split out of FM2K_RollbackClient.cpp. Pure move:
// these are FM2KLauncher member functions declared in FM2K_Integration.h, so
// no internal-state header is needed -- they touch only `this->` members plus
// the header-declared game.ini helpers. Each Start* spawns the game instance
// with the right env vars and transitions launcher state; StopSession tears
// the instance down and restores the game's pristine ini.

#include "SDL3/SDL.h"
#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include "FM2K_GameIni.h"
#include "FM2K_OnlineLaunch.h"   // the shared online launch
#include "../game/FM2K_InstallHealth.h"  // Program Files / VirtualStore split detector
#include "FM2K_Locale.h"                 // T() -- the online-refusal status line
#include "../ui/launcher_ui_internal.h"  // lui::NeutralizeGamePatchEnvVars

#include <filesystem>   // the "is game.ini actually gone" test on the refusal path
#include <memory>
#include <string>
#include <cstdlib>
#include <iostream>
#include <system_error>

// The Program Files / UAC VirtualStore probe (LogInstallHealth) and the whole
// online launch sequence live in launcher/session/FM2K_OnlineLaunch.cpp since
// 2026-09-02, shared with the Slint launcher.

void FM2KLauncher::StartOfflineSession() {
    if (selected_game_.exe_path.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot start offline session: no game selected.");
        return;
    }

    fm2k::online_launch::LogInstallHealth(selected_game_.exe_path);

    // Terminate existing game if running
    if (game_instance_ && game_instance_->IsRunning()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Terminating existing game instance before new launch");
        game_instance_->Terminate();
    }

    // Create new game instance
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating new FM2KGameInstance for offline session");
    game_instance_ = std::make_unique<FM2KGameInstance>();

    // Set environment variables for true offline mode
    game_instance_->SetEnvironmentVariable("FM2K_TRUE_OFFLINE", "1");
    game_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX", "0");  // Always P1 for offline
    game_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");  // Debug mode for now

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Set FM2K_TRUE_OFFLINE=1 for pure offline session");

    if (!game_instance_->Launch(selected_game_.exe_path, selected_game_.engine)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch game for offline session.");
        game_instance_.reset();
        return;
    }

    NetworkConfig local_config;
    local_config.session_mode = SessionMode::LOCAL;

    // Configure DLL for offline mode - shared memory enabled for debugging features
    if (game_instance_) {
        game_instance_->SetNetworkConfig(false, false);
    }

    SetState(LauncherState::InGame);
    std::cout << "? LOCAL session started (offline mode)\n";
}

// Launch a single game instance with GekkoStressSession enabled.
// No second instance, no networking. The hook DLL detects FM2K_STRESS_MODE=1
// and creates a GekkoStressSession with both players local; GekkoNet then
// artificially rewinds and re-simulates on a check_distance cadence, flagging
// any sim nondeterminism via the normal DESYNC event path.
// If the game survives a match without DESYNC firing, the save/load/tick
// pipeline is deterministic. If it fires, we have a pure local repro.
void FM2KLauncher::StartStressSession() {
    if (selected_game_.exe_path.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot start stress session: no game selected.");
        return;
    }

    if (game_instance_ && game_instance_->IsRunning()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Terminating existing game instance before stress launch");
        game_instance_->Terminate();
    }

    game_instance_ = std::make_unique<FM2KGameInstance>();

    // Stress is a determinism harness -- a leaked offline per-game patch
    // (damage mult, team size...) would perturb the baseline sim.
    lui::NeutralizeGamePatchEnvVars();
    lui::NeutralizeDevLaunchEnvVars();

    // Env vars: stress mode ON, true-offline OFF (we still need GekkoNet running)
    game_instance_->SetEnvironmentVariable("FM2K_TRUE_OFFLINE", "0");
    game_instance_->SetEnvironmentVariable("FM2K_STRESS_MODE", "1");
    game_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX", "0");  // irrelevant in stress mode but keep consistent
    game_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");  // verbose logging so we see desync diagnostics

    // FM95's rollback driver only runs under FM95_TRAMPOLINE=1 (the hook
    // rides TrampolineFrameTick inside CPW's native WinMain loop). Stress
    // mode IS the rollback driver, so a CPW stress session must enable it --
    // otherwise there's no Save/Load/Advance and the (protection-less,
    // dedup-less) Hook_RenderGame path runs instead. Per-instance env, so no
    // leakage to other spawns. No-op on FM2K.
    if (selected_game_.engine == FM2K::Engine::FM95) {
        game_instance_->SetEnvironmentVariable("FM95_TRAMPOLINE", "1");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Starting STRESS session: GekkoStressSession will force rollbacks "
        "every check_distance frames. Any DESYNC is a local determinism bug.");

    if (!game_instance_->Launch(selected_game_.exe_path, selected_game_.engine)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch game for stress session.");
        game_instance_.reset();
        return;
    }

    SetState(LauncherState::InGame);
    std::cout << "? STRESS session started (determinism check, single instance)\n";
}

void FM2KLauncher::StartOnlineSession(const NetworkConfig& config, bool is_host) {
    if (selected_game_.exe_path.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot start online session: no game selected.");
        return;
    }

    // Terminate existing game if running
    if (game_instance_ && game_instance_->IsRunning()) {
        game_instance_->Terminate();
    }

    // Create new instance and set env vars BEFORE launch
    game_instance_ = std::make_unique<FM2KGameInstance>();
    ApplyPendingConfigToInstance(game_instance_.get());

    // The environment, the online game.ini apply (anti-cheat clamps, one
    // retry, deleted-ini rescue) and the launch: FM2K_OnlineLaunch.cpp,
    // shared with the Slint launcher.
    using fm2k::online_launch::StartResult;
    const StartResult r = fm2k::online_launch::StartOnlineGame(
        *game_instance_, selected_game_.exe_path, selected_game_.engine,
        config, is_host);
    if (r == StartResult::IniRefused) {
        game_instance_.reset();
        // SURFACE IT (Wave-2 review B6). This refusal happens AFTER the hub's
        // match_start has already been accepted, so silence here means: the
        // local user sees a launcher that did nothing, the OPPONENT launches and
        // waits forever for a peer that will never connect, and both users' hub
        // status sticks at "in_match" until someone reconnects. The sibling
        // abort path in launcher_ui_hub_events_match.cpp does status_line +
        // MatchEnded(); do the same, and return the launcher to a usable state.
        if (ui_) {
            ui_->NotifyHubMatchAborted(T("hub_status_online_ini_refused"));
        }
        SetState(LauncherState::GameSelection);
        return;
    }
    if (r == StartResult::LaunchFailed) {
        game_instance_.reset();
        return;
    }

    network_config_ = config;
    SetState(LauncherState::Connecting);
    std::cout << "? ONLINE session started (" << (is_host ? "Hosting" : "Joining") << ")\n";
}

void FM2KLauncher::StopSession() {
    // DLL handles GekkoNet directly - no launcher-side session needed
    std::cout << "? Session stopped\n";
    // Tell the hub the match ended BEFORE we tear the local instance
    // down. Hub flips both peers' status back to "idle" and
    // broadcasts user_status to the rest of the room -- without this
    // the lobby sticks at "in_match" and Challenge stays disabled
    // until the user reconnects.
    if (ui_) {
        ui_->NotifyHubMatchEnded();
    }
    if (game_instance_) {
        game_instance_->Terminate();
        game_instance_.reset();
    }
    // Restore the game's pristine game.ini from the .fm2krollback_bak
    // backup ApplyForLaunch made. No-op when there's no backup (we
    // never overrode anything for this game). Done after Terminate so
    // we don't race the game holding its own ini open.
    if (!selected_game_.exe_path.empty()) {
        fm2k::game_ini::RestoreFromBackup(selected_game_.exe_path);
    }
    SetState(LauncherState::GameSelection);
}
