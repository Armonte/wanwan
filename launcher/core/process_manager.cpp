// process_manager.cpp -- FM2KLauncher process spawn/terminate + per-instance
// config, split out of FM2K_RollbackClient.cpp. Pure move: these are
// FM2KLauncher member functions declared in FM2K_Integration.h, so no
// internal-state header is needed -- they touch only `this->` members and
// header-declared helpers. Owns the launch of every game instance the
// launcher spawns (main game, local test clients, spectators, replay) plus
// the shared-memory stats read and pending-config apply.

#include "SDL3/SDL.h"
#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "../ui/launcher_ui_internal.h"  // lui::NeutralizeGamePatchEnvVars

#include <memory>
#include <string>
#include <cstdlib>
#include <iostream>
#include <windows.h>

bool FM2KLauncher::LaunchGame(const FM2K::FM2KGameInfo& game) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Attempting to launch game: %s", game.exe_path.c_str());

    if (!game.is_host) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot launch invalid game - is_host flag is false");
        return false;
    }

    // Terminate existing game if running
    if (game_instance_ && game_instance_->IsRunning()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Terminating existing game instance before new launch");
        game_instance_->Terminate();
    }

    // Create new game instance
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating new FM2KGameInstance");
    game_instance_ = std::make_unique<FM2KGameInstance>();

    // Apply any pending configuration before launching
    ApplyPendingConfigToInstance(game_instance_.get());

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Launching game with EXE: %s, KGT: %s, engine=%s",
                 game.exe_path.c_str(), game.dll_path.c_str(),
                 FM2K::EngineName(game.engine));

    if (!game_instance_->Launch(game.exe_path, game.engine)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch game: %s", game.exe_path.c_str());
        game_instance_.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game launched successfully: %s", game.exe_path.c_str());

    // Wait a moment and check if process is still running
    SDL_Delay(100);
    if (!game_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Game process terminated immediately after launch!");
        game_instance_.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game process confirmed running after 100ms");

    return true;
}

void FM2KLauncher::TerminateGame() {
    if (game_instance_) {
        game_instance_->Terminate();
        game_instance_.reset();
        std::cout << "? Game terminated\n";
    }
}

// Multi-client testing implementation
bool FM2KLauncher::LaunchLocalClient(const std::string& game_path, bool is_host, int port) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launching local client: %s (Host: %s, Port: %d)",
                game_path.c_str(), is_host ? "Yes" : "No", port);

    // Check if game instance is already running
    std::unique_ptr<FM2KGameInstance>* target_instance = is_host ? &client1_instance_ : &client2_instance_;
    if (*target_instance && (*target_instance)->IsRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Client %d already running", is_host ? 1 : 2);
        return false;
    }

    // Use the provided game path directly (user has manually set up wanwan2 if needed)
    std::string actual_game_path = game_path;

    // Create new game instance
    *target_instance = std::make_unique<FM2KGameInstance>();

    // Apply any pending configuration before launching
    ApplyPendingConfigToInstance(target_instance->get());

    // Configure GekkoNet session coordination for this client
    uint8_t player_index = is_host ? 0 : 1;  // Host = Player 0, Guest = Player 1

    // FIXED: Use correct networking configuration while keeping non-network variables identical
    // Set environment variables BEFORE launching process (OnlineSession style)
    (*target_instance)->SetEnvironmentVariable("FM2K_PLAYER_INDEX", std::to_string(player_index));  // Host=0, Guest=1
    (*target_instance)->SetEnvironmentVariable("FM2K_LOCAL_PORT", std::to_string(port));  // Keep port different (required for networking)
    // LOCAL-TEST PATH ONLY. LaunchLocalClient spawns two instances on this
    // machine over loopback for hands-on testing -- hence the hardcoded
    // 127.0.0.1 remote and the FIXED RNG seed (both local instances must agree
    // on the seed). The real matchmaked/networked path is session_control.cpp,
    // which sets FM2K_REMOTE_ADDR from the actual peer and NEVER sets
    // FM2K_FORCE_RNG_SEED (real matches derive their seed from the match). Do
    // NOT route production matches through here.
    (*target_instance)->SetEnvironmentVariable("FM2K_REMOTE_ADDR", "127.0.0.1:" + std::to_string(is_host ? 7001 : 7000));  // loopback (local test)

    // FM2K_PRODUCTION_MODE=0 keeps verbose desync diagnostics on during the
    // debug->production transition. LAUNCH CHECKLIST: flip to "1" for release
    // builds (here AND in session_control.cpp) so shipped clients aren't in
    // debug mode.
    (*target_instance)->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    (*target_instance)->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "1");  // Enable input recording by default

    // Fixed seed -- SAFE here (local loopback test, both instances identical).
    // Never set on the matchmaked path.
    (*target_instance)->SetEnvironmentVariable("FM2K_FORCE_RNG_SEED", "12345678");

    // Launch clients simultaneously - no delay needed
    // The GekkoNet synchronization will handle timing differences
    if (!is_host) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Guest client launching immediately (no delay)");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launching FM2K game with OnlineSession-style config: %s", actual_game_path.c_str());

    // Launch the actual FM2K game process with hook injection
    if (!(*target_instance)->Launch(actual_game_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch FM2K game: %s", actual_game_path.c_str());
        target_instance->reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client launched successfully - Player %u, Port %d", player_index, port);

    // Wait a moment and check if process is still running
    SDL_Delay(100);
    if (!(*target_instance)->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K game process terminated immediately after launch!");
        target_instance->reset();
        return false;
    }



    // Store process ID for status tracking
    uint32_t* target_pid = is_host ? &client1_process_id_ : &client2_process_id_;
    *target_pid = (*target_instance)->GetProcessId();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client %d (FM2K game) launched successfully (PID: %u)",
                is_host ? 1 : 2, *target_pid);

    return true;
}

bool FM2KLauncher::LaunchLocalSpectator(const std::string& game_path,
                                        int spectator_port,
                                        int host_port)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Launching local spectator: %s (port=%d -> host_port=%d)",
                game_path.c_str(), spectator_port, host_port);

    if (spectator_instance_ && spectator_instance_->IsRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Spectator already running");
        return false;
    }

    spectator_instance_ = std::make_unique<FM2KGameInstance>();
    ApplyPendingConfigToInstance(spectator_instance_.get());

    // Spectator-mode env vars. The hook reads FM2K_SPECTATOR_MODE=1 to skip
    // the normal HELLO/HELLO_ACK flow and instead send SPEC_JOIN_REQ to
    // FM2K_REMOTE_ADDR after the socket is up. Player index 2 is just a
    // sentinel -- spectators don't claim a player slot.
    spectator_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX",   "2");
    spectator_instance_->SetEnvironmentVariable("FM2K_LOCAL_PORT",     std::to_string(spectator_port));
    spectator_instance_->SetEnvironmentVariable("FM2K_REMOTE_ADDR",    "127.0.0.1:" + std::to_string(host_port));
    spectator_instance_->SetEnvironmentVariable("FM2K_SPECTATOR_MODE", "1");
    spectator_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    spectator_instance_->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "0");
    spectator_instance_->SetEnvironmentVariable("FM2K_FORCE_RNG_SEED",  "12345678");
    // Parity recorder is OPT-IN: an unset FM2K_PARITY_RECORD_PATH means no
    // recording, not a default path. See the note in session_control.cpp --
    // the old default named a developer's absolute drive path and failed
    // silently everywhere else. An explicitly-set value is inherited as-is.

    if (!spectator_instance_->Launch(game_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch spectator: %s", game_path.c_str());
        spectator_instance_.reset();
        return false;
    }

    SDL_Delay(100);
    if (!spectator_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Spectator process terminated immediately!");
        spectator_instance_.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spectator launched successfully (PID: %u, port=%d -> host=127.0.0.1:%d)",
                spectator_instance_->GetProcessId(), spectator_port, host_port);
    return true;
}

bool FM2KLauncher::LaunchRemoteSpectator(const std::string& game_path,
                                         int spectator_port,
                                         const std::string& host_ip,
                                         int host_port,
                                         const std::string& session_kind,
                                         const std::string& spec_transport,
                                         int player_index)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Launching remote spectator: %s (port=%d -> %s:%d, "
                "session_kind=%s, transport=%s)",
                game_path.c_str(), spectator_port, host_ip.c_str(), host_port,
                session_kind.c_str(), spec_transport.c_str());

    if (spectator_instance_ && spectator_instance_->IsRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Spectator already running");
        return false;
    }

    spectator_instance_ = std::make_unique<FM2KGameInstance>();

    // Spectators re-sim the host's match -- a leaked offline per-game
    // patch env (team size / damage mult) would fork their sim from the
    // stream. Same neutralize the online session start does.
    lui::NeutralizeGamePatchEnvVars();

    ApplyPendingConfigToInstance(spectator_instance_.get());

    const std::string remote_addr = host_ip + ":" + std::to_string(host_port);
    // Per-spectator index so N concurrent spectators write distinct
    // FM2K_P{index+1}_Debug.log + distinct parity (default 2 => P3, unchanged
    // for the single-spectator UI/CLI callers).
    spectator_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX",    std::to_string(player_index));
    spectator_instance_->SetEnvironmentVariable("FM2K_LOCAL_PORT",      std::to_string(spectator_port));
    spectator_instance_->SetEnvironmentVariable("FM2K_REMOTE_ADDR",     remote_addr);
    spectator_instance_->SetEnvironmentVariable("FM2K_SPECTATOR_MODE",  "1");
    spectator_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    spectator_instance_->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "0");
    spectator_instance_->SetEnvironmentVariable("FM2K_FORCE_RNG_SEED",  "12345678");
    // Auto-derived spec transport from the host's spectate_grant. "relay"
    // enables the hub-relay data plane in the spec hook; "direct" (and the
    // legacy spelling "tcp" an older hub still sends for the non-relay case)
    // explicitly clears any inherited relay env from a previous spec session.
    // The user never sets this by hand -- it is negotiated via the hub.
    if (spec_transport == "direct" || spec_transport == "tcp" ||
        spec_transport == "relay") {
        spectator_instance_->SetEnvironmentVariable("FM2K_SPEC_TRANSPORT", spec_transport);
    }
    {
        // /F boot-to-battle for spectators -- conditional on host's
        // current session_kind (forwarded by the hub in spectate_grant,
        // sourced from the host hook's published game_mode transitions
        // via SharedMem). Two cases:
        //
        //  - host in "battle": set /F so the spec engine's slot-0
        //    dispatcher fires `create_game_object(14, 127, 0, 0)`
        //    straight into battle (skips CSS). SpectatorNode then
        //    overlays the host's snapshot -- chars, positions, RNG,
        //    everything -- and sim-forwards inputs to live. ~1s join
        //    instead of ~5s title→CSS→battle walk.
        //
        //  - host in "menu" / "css": do NOT set /F. Spec walks the
        //    normal title → CSS path; the CSS-snapshot mid-CSS-join
        //    handshake (Phase E) syncs chars/cursor state. /F here
        //    would land spec in battle with placeholder chars and
        //    no battle-state snapshot to apply, crashing the engine
        //    when the eventual mode 2000→3000 transition fails.
        //
        // Older hubs / pre-session_kind clients default to "menu"
        // (no /F). This is the safe default -- worst case the spec
        // joins via title walk instead of boot-to-battle (slower
        // join, never a crash).
        const bool boot_to_battle = (session_kind == "battle");
        if (boot_to_battle) {
            spectator_instance_->SetEnvironmentVariable("FM2K_BOOT_TO_BATTLE", "1");
            // Placeholder chars / stage for the /F slot-0 dispatcher's
            // battle init. The snapshot apply overlays the real values
            // from the host's saved blob, so the placeholders only
            // affect the engine's INITIAL battle frame state (which
            // SaveState_LoadFromBytes immediately overwrites).
            spectator_instance_->SetEnvironmentVariable("FM2K_BTB_P1_CHAR", "0");
            spectator_instance_->SetEnvironmentVariable("FM2K_BTB_P2_CHAR", "0");
            spectator_instance_->SetEnvironmentVariable("FM2K_BTB_STAGE",   "0");
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spec: host in battle -- set FM2K_BOOT_TO_BATTLE=1");
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spec: host in %s -- walking normal title→CSS path (no /F)",
                session_kind.c_str());
        }
    }
    // Default spectator parity path -- but respect an explicit
    // FM2K_PARITY_RECORD_PATH from the environment (the spec_selftest
    // harness routes the .pty into its own workspace; the unconditional
    // override silently sent it to parity_p3.pty instead).
    // Parity recorder is OPT-IN: an unset FM2K_PARITY_RECORD_PATH means no
    // recording, not a default path. See the note in session_control.cpp --
    // the old default named a developer's absolute drive path and failed
    // silently everywhere else. An explicitly-set value is inherited as-is.

    if (!spectator_instance_->Launch(game_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch remote spectator: %s",
                     game_path.c_str());
        spectator_instance_.reset();
        return false;
    }

    SDL_Delay(100);
    if (!spectator_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Remote spectator process terminated immediately!");
        spectator_instance_.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Remote spectator launched (PID: %u, port=%d -> host=%s)",
                spectator_instance_->GetProcessId(), spectator_port, remote_addr.c_str());
    return true;
}

bool FM2KLauncher::LaunchReplayPlayer(const std::string& game_path,
                                      const std::string& replay_path)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Launching replay player: %s (replay=%s)",
                game_path.c_str(), replay_path.c_str());

    if (spectator_instance_ && spectator_instance_->IsRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Replay player already running");
        return false;
    }

    spectator_instance_ = std::make_unique<FM2KGameInstance>();
    ApplyPendingConfigToInstance(spectator_instance_.get());

    // Replay-mode env vars. Hook reads FM2K_REPLAY_FILE in
    // Netplay_InitAsSpectator and short-circuits the network setup,
    // calling SpectatorNode_LoadSessionFile to populate pb_queue from
    // disk. Trampoline's RunSpectatorTick drains it the same way it
    // drains live-wire events. PLAYER_INDEX=2 is the spectator sentinel.
    spectator_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX",    "2");
    spectator_instance_->SetEnvironmentVariable("FM2K_SPECTATOR_MODE",  "1");
    spectator_instance_->SetEnvironmentVariable("FM2K_REPLAY_FILE",     replay_path);
    spectator_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    spectator_instance_->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "0");

    if (!spectator_instance_->Launch(game_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch replay player: %s",
                     game_path.c_str());
        spectator_instance_.reset();
        return false;
    }

    SDL_Delay(100);
    if (!spectator_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Replay player process terminated immediately!");
        spectator_instance_.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Replay player launched (PID: %u, replay=%s)",
                spectator_instance_->GetProcessId(), replay_path.c_str());
    return true;
}

bool FM2KLauncher::LaunchLocalSpectator2(const std::string& game_path,
                                         int spectator_port,
                                         int upstream_port)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Launching local spectator 2 (chain): %s (port=%d -> upstream_port=%d)",
                game_path.c_str(), spectator_port, upstream_port);

    if (spectator2_instance_ && spectator2_instance_->IsRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Spectator 2 already running");
        return false;
    }

    spectator2_instance_ = std::make_unique<FM2KGameInstance>();
    ApplyPendingConfigToInstance(spectator2_instance_.get());

    // Same env-var shape as the first spectator. The only difference is
    // FM2K_REMOTE_ADDR points at spectator 1's port (7002) instead of the
    // host's (7000). Spectator 1 acts as upstream; on JOIN_REQ it accepts
    // and starts shipping its session_history (which it has been
    // accumulating from its OWN relay path) to spectator 2.
    spectator2_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX",   "3");
    spectator2_instance_->SetEnvironmentVariable("FM2K_LOCAL_PORT",     std::to_string(spectator_port));
    spectator2_instance_->SetEnvironmentVariable("FM2K_REMOTE_ADDR",    "127.0.0.1:" + std::to_string(upstream_port));
    spectator2_instance_->SetEnvironmentVariable("FM2K_SPECTATOR_MODE", "1");
    spectator2_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    spectator2_instance_->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "0");
    spectator2_instance_->SetEnvironmentVariable("FM2K_FORCE_RNG_SEED",  "12345678");

    if (!spectator2_instance_->Launch(game_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to launch spectator 2: %s", game_path.c_str());
        spectator2_instance_.reset();
        return false;
    }

    SDL_Delay(100);
    if (!spectator2_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Spectator 2 process terminated immediately!");
        spectator2_instance_.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spectator 2 launched (PID: %u, port=%d -> upstream=127.0.0.1:%d)",
                spectator2_instance_->GetProcessId(), spectator_port, upstream_port);
    return true;
}

bool FM2KLauncher::TerminateAllClients() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminating all local clients");

    bool success = true;

    // Terminate client 1
    if (client1_instance_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminating Client 1 (PID: %u)", client1_instance_->GetProcessId());
        client1_instance_->Terminate();
        client1_instance_.reset();
        client1_process_id_ = 0;
    }

    // Terminate client 2
    if (client2_instance_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminating Client 2 (PID: %u)", client2_instance_->GetProcessId());
        client2_instance_->Terminate();
        client2_instance_.reset();
        client2_process_id_ = 0;
    }

    // Terminate spectator
    if (spectator_instance_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminating Spectator (PID: %u)", spectator_instance_->GetProcessId());
        spectator_instance_->Terminate();
        spectator_instance_.reset();
    }

    // Terminate spectator 2 (daisy-chain)
    if (spectator2_instance_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminating Spectator 2 (PID: %u)", spectator2_instance_->GetProcessId());
        spectator2_instance_->Terminate();
        spectator2_instance_.reset();
    }

    return success;
}


bool FM2KLauncher::ReadRollbackStatsFromSharedMemory(RollbackStats& stats) {
    // Try to read from both client processes (prioritize the first active one)
    auto try_read_stats = [&stats](DWORD process_id) -> bool {
        if (process_id == 0) return false;

        std::string shared_memory_name = "FM2K_SharedMem_" + std::to_string(process_id);
        HANDLE shared_memory_handle = OpenFileMappingA(FILE_MAP_READ, FALSE, shared_memory_name.c_str());
        if (!shared_memory_handle) return false;

        FM2KSharedMemData* shared_data = static_cast<FM2KSharedMemData*>(
            MapViewOfFile(shared_memory_handle, FILE_MAP_READ, 0, 0, sizeof(FM2KSharedMemData))
        );

        bool ok = false;
        if (shared_data && shared_data->magic == FM2K_SHARED_MEM_MAGIC) {
            stats.rollbacks_per_second = 0;                        // not available in new struct
            stats.max_rollback_frames = 0;                         // not available
            stats.avg_rollback_frames = 0;                         // not available
            stats.frame_advantage = shared_data->frames_ahead;
            stats.input_delay_frames = 2;                          // placeholder
            stats.confirmed_frames = shared_data->frame_number;
            stats.speculative_frames = shared_data->rollback_count;
            ok = true;
        }

        if (shared_data) UnmapViewOfFile(shared_data);
        CloseHandle(shared_memory_handle);
        return ok;
    };

    // Check Client 1 first, then Client 2
    if (try_read_stats(client1_process_id_)) return true;
    if (try_read_stats(client2_process_id_)) return true;
    return false;
}

// Apply pending configuration to a game instance
void FM2KLauncher::ApplyPendingConfigToInstance(FM2KGameInstance* instance) {
    if (!instance) {
        return;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Applying pending configuration to game instance");

    // Apply MinimalGameState testing config
    if (pending_config_.has_minimal_gamestate_testing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Applying pending MinimalGameState testing: %s",
                   pending_config_.minimal_gamestate_testing_value ? "enabled" : "disabled");
        instance->SetMinimalGameStateTesting(pending_config_.minimal_gamestate_testing_value);
    }

    // Apply production mode config
    if (pending_config_.has_production_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Applying pending production mode: %s",
                   pending_config_.production_mode_value ? "enabled" : "disabled");
        instance->SetProductionMode(pending_config_.production_mode_value);
    }

    // Apply input recording config
    if (pending_config_.has_input_recording) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Applying pending input recording: %s",
                   pending_config_.input_recording_value ? "enabled" : "disabled");
        instance->SetInputRecording(pending_config_.input_recording_value);
    }
}
