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
#include "../ui/launcher_ui_internal.h"  // lui::NeutralizeGamePatchEnvVars

#include <memory>
#include <string>
#include <cstdlib>
#include <iostream>

void FM2KLauncher::StartOfflineSession() {
    if (selected_game_.exe_path.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot start offline session: no game selected.");
        return;
    }

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
    game_instance_->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "1");  // Enable input recording

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

    // Netplay must not inherit per-game patch envs from a prior OFFLINE
    // launch (they're launcher-process-level): a one-sided team-size or
    // damage-multiplier is a guaranteed desync. Online = vanilla + hook
    // defaults until per-game patches are host-synced.
    lui::NeutralizeGamePatchEnvVars();

    // FM95 rollback runs only under FM95_TRAMPOLINE=1 (the hook rides
    // TrampolineFrameTick inside CPW's WinMain loop). Netplay IS the rollback
    // driver, so a CPW online match must enable it -- same as stress. Offline
    // deliberately does NOT set it (CPW runs natively at title→CSS→battle).
    // Per-instance env; no-op on FM2K.
    if (selected_game_.engine == FM2K::Engine::FM95) {
        game_instance_->SetEnvironmentVariable("FM95_TRAMPOLINE", "1");
    }

    ApplyPendingConfigToInstance(game_instance_.get());

    uint8_t player_index = is_host ? 0 : 1;
    uint16_t local_port = static_cast<uint16_t>(config.local_port);

    // Remote address:
    //   - HUB-DRIVEN: match_start carries the peer's udp_addr in config
    //     for BOTH host and guest. Use it directly.
    //   - JOIN (legacy direct connect): user-pasted "ip:port" in
    //     config.remote_address.
    //   - HOST (legacy direct connect): leave empty so the hook
    //     listens on its socket and learns the peer's address from
    //     the first inbound HELLO. The default "127.0.0.1:7001" from
    //     NetworkConfig's ctor is a UI copy-button placeholder, not
    //     a real peer -- clear it for legacy host.
    std::string remote_addr = config.remote_address;
    if (is_host && remote_addr == "127.0.0.1:7001") {
        remote_addr.clear();   // legacy-host placeholder; let hook learn
    }
    if (remote_addr.find(':') == std::string::npos && !remote_addr.empty()) {
        remote_addr += ":7500";  // fallback if user pasted a bare IP
    }

    game_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX", std::to_string(player_index));
    game_instance_->SetEnvironmentVariable("FM2K_LOCAL_PORT", std::to_string(local_port));
    game_instance_->SetEnvironmentVariable("FM2K_REMOTE_ADDR", remote_addr);

    // Auto-enable parity recorder for spectator-desync diagnosis. Each
    // process writes per-frame state snapshots (RNG, game_timer, render_fc,
    // etc.) to a .pty file. Diff host vs spectator post-run with
    // tools/kgt_diff_pty to find the first divergent frame. Skip the env
    // override if the user already set one (manual diagnostic flow).
    if (!std::getenv("FM2K_PARITY_RECORD_PATH")) {
        const std::string pty_path = "c:/games/2dfm/wanwan/parity_p"
            + std::to_string(player_index + 1) + ".pty";
        game_instance_->SetEnvironmentVariable("FM2K_PARITY_RECORD_PATH", pty_path);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Online session: parity recorder -> %s", pty_path.c_str());
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Online session: P%d port=%d remote=%s",
        player_index + 1, local_port, remote_addr.c_str());

    // Bake the host's resolved [GamePlay] config (defaults + per-game
    // overrides + online anti-cheat clamps) into the game's own
    // game.ini before CreateProcess. The game reads this file at
    // startup; by writing it now both peers boot with the same round
    // count / time / stage / etc. We restore the original ini in
    // StopSession so leaving the launcher doesn't permanently mutate
    // the user's offline settings. is_online=true forces HitJudge +
    // GameInformation = 0 (debug overlays are cheating online).
    fm2k::game_ini::ApplyForLaunch(selected_game_.exe_path,
                                    /*is_online=*/true);

    if (!game_instance_->Launch(selected_game_.exe_path, selected_game_.engine)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch game for online session.");
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
