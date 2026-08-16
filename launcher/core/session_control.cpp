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
#include "FM2K_Locale.h"                 // T() -- the online-refusal status line
#include "../ui/launcher_ui_internal.h"  // lui::NeutralizeGamePatchEnvVars

#include <filesystem>   // the "is game.ini actually gone" test on the refusal path
#include <memory>
#include <string>
#include <cstdlib>
#include <iostream>
#include <system_error>

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
    //
    // THE RETURN VALUE IS LOAD-BEARING ON THIS PATH AND WAS BEING DISCARDED.
    // ApplyForLaunch returns false when it could not take its one-time backup
    // of game.ini (read-only directory, a Program Files / VirtualStore split,
    // the file locked by another process) and therefore refused to write, to
    // avoid clobbering the user's settings with no way back. Ignoring that
    // meant the game launched ONLINE with game.ini untouched -- i.e. without
    // ForceOnlineClamps, so HitJudge and GameInformation debug overlays go
    // live in a rated match and the round config is whatever was on disk. The
    // only trace was a WARN in launcher.log, which under exactly the
    // VirtualStore split that causes this is not where the user is looking.
    //
    // This is the anti-cheat path, so refusing to start is the defensible
    // behaviour: same shape as the Launch failure immediately below.
    //
    // ONE RETRY before refusing. The only failure this has ever been observed
    // to take in practice is a same-install collision between two launcher
    // processes (measured 2026-08-15: the hub-brokered E2E harness runs three
    // launchers against one game directory). Both known collision points are
    // fixed at the source -- the concurrent-backup carve-out and the per-pid
    // temp file in FM2K_GameIni.cpp -- so this retry is belt-and-braces against
    // any remaining transient sharing violation, not the fix for one. A genuine
    // permissions failure fails both attempts in a few hundred microseconds.
    bool ini_ok = fm2k::game_ini::ApplyForLaunch(selected_game_.exe_path,
                                                 /*is_online=*/true);
    if (!ini_ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "GameIni: apply-for-launch failed; retrying once in 250ms before "
            "refusing the online session");
        ::Sleep(250);
        ini_ok = fm2k::game_ini::ApplyForLaunch(selected_game_.exe_path,
                                                /*is_online=*/true);
    }
    if (!ini_ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Online session ABORTED: could not apply game.ini for launch, so the "
            "online anti-cheat clamps (HitJudge/GameInformation = 0) and the "
            "shared round config were NOT written. Most likely cause: the game "
            "directory is not writable (Program Files / UAC VirtualStore, or the "
            "file is open elsewhere). Move the game out of Program Files or fix "
            "the permissions on '%s' and try again.",
            selected_game_.exe_path.c_str());
        // RESCUE A DELETED game.ini (Wave-2 review B3b). ApplyForLaunch can
        // fail INSIDE Save(), whose overwrite fallback is `remove(game.ini)` +
        // `rename(tmp, game.ini)`: if that second rename also fails, game.ini is
        // GONE and the temp has been removed too. Returning from here without
        // putting it back would leave the user with no game.ini at all, because
        // the only RestoreFromBackup call is in StopSession and this path never
        // reaches it (the session never starts).
        //
        // CONDITIONAL ON THE FILE BEING MISSING, and that is load-bearing.
        // RestoreFromBackup is `remove(ini) + rename(bak, ini)`: it CONSUMES
        // the backup. On a shared install (two launchers, one game directory --
        // the case that produced this whole family of races) an unconditional
        // restore here would revert the OTHER launcher's freshly applied
        // game.ini while its game is booting AND eat the backup its own
        // StopSession needs. Restoring only when the file has actually
        // disappeared fixes the destructive case and cannot touch a healthy one.
        if (!selected_game_.exe_path.empty()) {
            std::error_code ini_ec;
            const auto ini_path =
                fm2k::game_ini::PathForExe(selected_game_.exe_path);
            if (!std::filesystem::exists(ini_path, ini_ec)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameIni: game.ini is MISSING after the failed apply -- "
                    "restoring it from the backup before refusing the session");
                fm2k::game_ini::RestoreFromBackup(selected_game_.exe_path);
            }
        }
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
