#pragma once
// NOTE: included ONLY via FM2K_Integration.h (umbrella). Relies on that
// header's includes + the namespace FM2K layout / config structs / the
// `class LauncherUI;` forward-decl being in scope -- not standalone.

// Main launcher class
class FM2KLauncher {
public:
    FM2KLauncher();
    ~FM2KLauncher();
    
    bool Initialize();
    void Shutdown();
    void Update(float delta_time);
    void Render();
    void HandleEvent(SDL_Event* event);
    
    bool LaunchGame(const FM2K::FM2KGameInfo& game);
    void TerminateGame();

    void StartOfflineSession();
    void StartOnlineSession(const NetworkConfig& config, bool is_host);
    void StartStressSession();  // GekkoStressSession determinism test (single instance)
    void StopSession();
    
    std::vector<FM2K::FM2KGameInfo> DiscoverGames();
    const std::vector<FM2K::FM2KGameInfo>& GetDiscoveredGames() const { return discovered_games_; }

    // Resolve a hub-style game_id (exe stem, e.g. "WonderfulWorld_ver_0946")
    // to its parsed .kgt summary. Returns nullptr if the game isn't
    // installed locally or its KGT failed to parse -- UI callers should
    // pass the result straight into fm2k::FormatCharLabel /
    // FormatStageLabel which fall back to "Char #N" / "Stage #N".
    const fm2k::KgtSummary* FindKgtByGameId(const std::string& game_id) const;
    
    void SetState(LauncherState state);
    LauncherState GetState() const { return current_state_; }
    bool IsRunning() const { return running_; }
    void SetRunning(bool running) { running_ = running; }
    // CLI direct-mode (--host/--connect/--spectate/--replay): there is no
    // interactive lobby to return to, so when the hosted game process exits
    // (clean end, desync-terminate, crash, or the spectator host-gone
    // watchdog) the launcher must QUIT rather than zombie at the selection
    // screen waiting for input that never comes (the "launcher didnt kill"
    // stuck-window under the test harness). Default false = interactive lobby
    // returns to selection on game end.
    void SetExitOnGameEnd(bool v) { exit_on_game_end_ = v; }

    // True when the UI has a live animation (background-discovery spinner or an
    // open input-binder window showing live pad state) that must keep painting
    // even while the user is idle. SDL_AppIterate ORs this into its repaint
    // decision so the event-driven idle path doesn't freeze an animation.
    // Forwards to LauncherUI::WantsContinuousRedraw (defined out-of-line
    // because LauncherUI is only forward-declared here).
    bool UiWantsContinuousRedraw() const;
    
    // Games directory management
    const std::vector<std::string>& GetGamesRootPaths() const { return games_root_paths_; }
    void SetGamesRootPaths(const std::vector<std::string>& paths);
    void SetSelectedGame(const FM2K::FM2KGameInfo& game);
    
    // ----- Asynchronous game discovery -----
    SDL_Thread* discovery_thread_ = nullptr; // Worker thread handle
    bool discovery_in_progress_ = false;     // Flag so we don't launch multiple scans

    // Starts a background SDL thread that will run DiscoverGames() and notify the main
    // thread when done. Implemented in FM2K_RollbackClient.cpp.
    //
    // `show_spinner` toggles the UI's "Scanning for games…" indicator. Pass
    // false when the cache already populated the games list -- the user
    // shouldn't see a spinner if the displayed list is already correct;
    // the background walk is just an "anything new?" check at that point.
    void StartAsyncDiscovery(bool show_spinner = true);
    
    // Scan progress accessors for UI
    void SetScanning(bool scanning);

    SDL_Window* GetWindow() const { return window_; }
    bool IsVsyncAvailable() const { return vsync_available_; }

private:
    bool InitializeSDL();
    bool InitializeImGui();
    void WireUICallbacks();  // ui_->on_* lambda wiring (split into launcher_callbacks.cpp)
    
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::unique_ptr<LauncherUI> ui_;
    std::unique_ptr<FM2KGameInstance> game_instance_;
    std::vector<FM2K::FM2KGameInfo> discovered_games_;
    
    // Multi-client testing instances
    std::unique_ptr<FM2KGameInstance> client1_instance_;
    std::unique_ptr<FM2KGameInstance> client2_instance_;
    // Local spectator instance -- subscribes to client1 (host) on its
    // multiplexed UDP port and replays the input stream. Used by the
    // launcher's "Launch Spectator" button so we can validate the
    // spectator pipeline against a live local dual-client session.
    std::unique_ptr<FM2KGameInstance> spectator_instance_;
    // Second local spectator that subscribes to spectator_instance_ rather
    // than the host -- exercises the daisy-chain relay (host → spec1 → spec2).
    // Validates that a relay node correctly forwards confirmed-input frames
    // it received from upstream to its own subscribers.
    std::unique_ptr<FM2KGameInstance> spectator2_instance_;
    // Phase 4: spec hub-relay ring caches. Promoted from lambda-local
    // statics so the menu-bar status pill can read live counters from
    // BOTH directions. Lifetimes: opened lazily when a game with a
    // relay-mode hook exists; closed on pid change.
    void*    spec_relay_out_ring_ = nullptr;  // fm2k::spec_relay::Ring*
    void*    spec_relay_in_ring_  = nullptr;
    uint32_t spec_relay_out_pid_  = 0;
    uint32_t spec_relay_in_pid_   = 0;
    FM2K::FM2KGameInfo selected_game_;
    NetworkConfig network_config_;
    LauncherState current_state_;
    bool running_;
    // See SetExitOnGameEnd: quit instead of returning to the lobby when the
    // hosted game process terminates (CLI direct-mode / harness).
    bool exit_on_game_end_ = false;
    // True when SDL_SetRenderVSync(renderer_, 1) reported success AND
    // SDL_GetRenderVSync reads back enabled. When false (e.g. RDP /
    // software fallback / driver refused), SDL_AppIterate falls back to
    // a software 60 fps cap so the launcher doesn't spin uncapped and
    // burn CPU/GPU at idle.
    bool vsync_available_ = false;
    
    // Timing
    std::chrono::steady_clock::time_point last_frame_time_;
    
    // Game discovery helpers
    bool ValidateGameFiles(FM2K::FM2KGameInfo& game);
    std::string DetectGameVersion(const std::string& exe_path);
    
    // Multi-client testing helpers
    bool LaunchLocalClient(const std::string& game_path, bool is_host, int port);
    // Launch a local spectator pointing at the host (client1) on host_port.
    // Spectator-mode hook will SPEC_JOIN_REQ the host and start replaying
    // the streamed input history (CSS + battle).
    // mode: "current" (default; CCCaster-style snapshot join -- v0.2.42's
    // Phases C+D+E made this the preferred path: /F boots spec straight
    // to battle for mid-battle joiners, per-round refresh keeps mid-set
    // joiners fresh, CSS snapshot covers mid-CSS joiners) or "full"
    // (legacy replay-from-session-start input log; falls back here when
    // CURRENT_MATCH can't apply -- version mismatch, snapshot transfer
    // interrupted, etc.). Default flipped from "full" → "current" on
    // 2026-05-13 after the vanpri sim-determinism leak in the replay
    // path made FULL_SESSION untrustworthy past ~4000 frames.
    bool LaunchLocalSpectator(const std::string& game_path,
                              int spectator_port,
                              int host_port,
                              const std::string& mode = "current");
    // Daisy-chain test: launches a second spectator that subscribes to the
    // first spectator instead of the host. Verifies relay-node forwarding.
    bool LaunchLocalSpectator2(const std::string& game_path,
                               int spectator_port,
                               int upstream_port);
public:
    // Launch a spectator pointing at an arbitrary remote host (typically
    // received via hub spectate_grant). Used by the lobby UI's "click an
    // active match to watch it" path AND the --spectate CLI flag for e2e
    // testing. spectator_port is local UDP bind; host_ip:host_port is
    // where the spectator's FM2K_REMOTE_ADDR points and SpectatorNode
    // JOIN_REQ is sent. mode default "current" -- see LaunchLocalSpectator
    // for the 2026-05-13 v0.2.42 flip rationale.
    // spec_transport ("tcp" or "relay") -- Phase 4. If "relay", the
    // launcher sets FM2K_SPEC_TRANSPORT=relay on the spec game spawn
    // so the hook enters relay mode without the user setting an env.
    // Default "tcp" preserves legacy P2P TCP spec data plane.
    bool LaunchRemoteSpectator(const std::string& game_path,
                               int spectator_port,
                               const std::string& host_ip,
                               int host_port,
                               const std::string& session_kind = "menu",
                               const std::string& mode = "current",
                               const std::string& spec_transport = "tcp",
                               int player_index = 2);

    // Offline replay player. Launches the game with FM2K_SPECTATOR_MODE=1
    // + FM2K_REPLAY_FILE=<replay_path>; the hook reads the env var in
    // Netplay_InitAsSpectator, calls SpectatorNode_LoadSessionFile to
    // populate pb_queue, and the trampoline's RunSpectatorTick drives
    // playback. No network, no peer, no STUN -- just the file.
    bool LaunchReplayPlayer(const std::string& game_path,
                            const std::string& replay_path);
private:
    bool TerminateAllClients();
    
    
    // Multi-client testing
    uint32_t client1_process_id_;
    uint32_t client2_process_id_;
    
    
    // Games directories (one or more roots where FM2K games are located).
    // Persisted as one path per line in launcher.cfg; the historical
    // single-string format migrates transparently because that file is
    // already line-delimited.
    std::vector<std::string> games_root_paths_;
    
    // Pending configuration (set before instances are created)
    struct PendingConfig {
        bool has_minimal_gamestate_testing = false;
        bool minimal_gamestate_testing_value = false;
        bool has_production_mode = false;
        bool production_mode_value = false;
        bool has_input_recording = false;
        bool input_recording_value = false;
    } pending_config_;
    
    // Helper method to read rollback statistics from hook shared memory  
    bool ReadRollbackStatsFromSharedMemory(RollbackStats& stats);
    
    // Apply pending configuration to game instances
    void ApplyPendingConfigToInstance(FM2KGameInstance* instance);
};

