#pragma once
// NOTE: included ONLY via FM2K_Integration.h (umbrella). Relies on that
// header's includes + the namespace FM2K layout / config structs / the
// `class LauncherUI;` forward-decl being in scope -- not standalone.

// Game instance management - see FM2K_GameInstance.h for full definition

// Modern ImGui launcher interface
class LauncherUI {
public:
    LauncherUI();
    ~LauncherUI();
    
    bool Initialize(SDL_Window* window, SDL_Renderer* renderer);
    void Shutdown();
    
    void NewFrame();
    void Render();

    // True while a continuous animation is on screen that the event-driven
    // idle path must keep repainting: the background-game scan spinner, or an
    // input-binder window (which shows live analog-stick state). Everything
    // else (Discord-pill pulse, transient modal dots) degrades gracefully to
    // the 250ms safety-net repaint when idle -- we deliberately do NOT keep
    // the CPU spinning for those, since pegging weak CPUs at idle is the whole
    // bug we're fixing.
    bool WantsContinuousRedraw() const;

    // UI state callbacks
    std::function<void(const FM2K::FM2KGameInfo&)> on_game_selected;
    std::function<void()> on_offline_session_start;
    std::function<void(const NetworkConfig&)> on_online_session_start;
    std::function<void()> on_stress_session_start;  // Single-instance GekkoStressSession determinism test
    // Click-to-spectate. host_ip:host_port comes from the hub's
    // spectate_grant. Launcher should boot a local FM2K spectator instance
    // pointing at that addr (LaunchRemoteSpectator). session_kind is
    // "menu" / "css" / "battle" -- the host's current game phase, used
    // to decide whether to /F-boot-to-battle (host in battle) or walk
    // the normal title→CSS path (host in lobby/CSS).
    //
    // spec_transport ("tcp" or "relay") -- Phase 4. Echoes the host's
    // advertised transport so the spec launcher sets FM2K_SPEC_TRANSPORT
    // on the spec game spawn matching the host's mode. Removes the
    // "user must set env on both peers" requirement.
    std::function<void(const std::string& host_ip, int host_port,
                       const std::string& session_kind,
                       const std::string& spec_transport)> on_spectate_match;
    // Hub fired a spectator_incoming event -- we're the host of an active
    // match and a spectator wants in. Their external UDP addr is passed
    // so we can fire an outbound NAT-punch packet to open the inbound
    // mapping before their first JOIN_REQ arrives at our NAT.
    std::function<void(const std::string& spec_udp_ip,
                       int                spec_udp_port,
                       int                spec_tcp_port,
                       const std::string& spec_user_id)> on_spectator_punch_target;
    // Phase 3: hub forwarded spec data bytes (we're the spec). Launcher
    // writes the bytes into the inbound shared-mem ring of the running
    // spec game so the hook can drain + dispatch. Bytes are a fully-
    // formed SpecDataHeader-prefixed wire frame.
    std::function<void(const std::vector<uint8_t>& bytes)> on_spec_relay_bytes;
    std::function<void()> on_session_stop;
    std::function<void()> on_exit;
    // C11 -- replay browser dispatch. Called when the user clicks a row in
    // the Replays panel; should call FM2KLauncher::LaunchReplayPlayer with
    // the absolute path to a .fm2krep / .fm2kset file. Game .exe is
    // resolved from the file's grandparent directory (replays/<f>.fm2krep
    // is always under <game_dir>/replays/) -- matches the same logic the
    // CLI --replay flag uses.
    std::function<void(const std::string& replay_path)> on_replay_play;
    // Fired when the user adds, removes, or otherwise reorders the
    // configured games-root list. The full new list is passed by value so
    // the launcher can persist it atomically; the UI keeps its own copy
    // in games_root_paths_ for rendering.
    std::function<void(const std::vector<std::string>&)> on_games_folders_set;
    
    // Debug state callbacks
    std::function<bool()> on_debug_save_state;
    std::function<bool()> on_debug_load_state;
    std::function<bool(uint32_t)> on_debug_force_rollback;
    
    // Frame stepping controls
    std::function<void(bool)> on_frame_step_pause;      // Pause/resume game execution
    std::function<void()> on_frame_step_single;         // Step one frame
    std::function<void(uint32_t)> on_frame_step_multi;   // Step multiple frames
    
    // Slot-based save/load callbacks
    std::function<bool(uint32_t)> on_debug_save_to_slot;
    std::function<bool(uint32_t)> on_debug_load_from_slot;
    std::function<bool(bool, uint32_t)> on_debug_auto_save_config;  // (enabled, interval_frames)
    
    // Slot status callback
    struct SlotStatusInfo {
        bool occupied;
        uint32_t frame_number;
        uint64_t timestamp_ms;
        uint32_t checksum;
        uint32_t state_size_kb;
        uint32_t save_time_us;
        uint32_t load_time_us;
        uint32_t active_object_count;
    };
    std::function<bool(uint32_t, SlotStatusInfo&)> on_get_slot_status;  // (slot, status_out)
    
    // Auto-save configuration callbacks
    struct AutoSaveConfig {
        bool enabled;
        uint32_t interval_frames;
    };
    std::function<bool(AutoSaveConfig&)> on_get_auto_save_config;  // Get current auto-save settings
    
    // Enhanced action inspection data structure (FM2K "objects" are actually "actions")
    struct EnhancedActionInfo {
        // Core action data from DetailedObject
        uint16_t slot_index;
        uint32_t type;
        uint32_t id;
        uint32_t position_x, position_y;
        uint32_t velocity_x, velocity_y;
        uint32_t animation_state;
        uint32_t health_damage;
        uint32_t state_flags;
        uint32_t timer_counter;
        
        // 2DFM script integration
        std::string type_name;              // Human readable action type name
        std::string action_name;            // Current action being performed
        uint32_t script_id;                 // Associated script ID
        uint32_t animation_frame;           // Current animation frame
        
        // Character-specific data (for CHARACTER actions)
        std::string character_name;         // Character performing the action
        std::string current_move;           // Current move/technique name
        uint32_t facing_direction;          // 0=left, 1=right
        uint32_t combo_count;               // Hit combo counter
        
        // Raw memory for deep inspection
        uint8_t raw_data[382];              // Complete action data
        
        // Analysis helpers
        bool IsCharacter() const { return type == 4; }
        bool IsProjectile() const { return type == 5; }
        bool IsEffect() const { return type == 6; }
        bool IsSystem() const { return type == 1; }
        bool HasMovement() const { return velocity_x != 0 || velocity_y != 0; }
    };
    
    // Action inspection callback - returns current active actions with enhanced data
    std::function<std::vector<EnhancedActionInfo>()> on_get_enhanced_actions;
    
    // Debug and testing configuration callbacks
    std::function<bool(bool)> on_set_production_mode;              // Set production mode (reduced logging)
    std::function<bool(bool)> on_set_input_recording;              // Set input recording
    std::function<bool(bool)> on_set_minimal_gamestate_testing;    // Set MinimalGameState testing
    
    // Multi-client testing data structures
    // NetworkStats struct removed - network stats handled by LocalNetworkAdapter
    
    // RollbackStats is now defined at global scope
    
    // Save state profile removed - now using optimized FastGameState system
    
    // ======= Multi-Client Testing Infrastructure =======
    
    // Multi-client process management  
    std::function<bool(const std::string&)> on_launch_local_client1;     // Launch first client as host
    std::function<bool(const std::string&)> on_launch_local_client2;     // Launch second client as guest
    std::function<bool(const std::string&)> on_launch_local_spectator;   // Launch spectator subscribing to client1
    std::function<bool(const std::string&)> on_launch_local_spectator2;  // Launch second spectator subscribing to first (daisy-chain test)
    std::function<bool()> on_terminate_all_clients;                      // Kill all launched clients
    std::function<bool(uint32_t&, uint32_t&)> on_get_client_status;      // Get client process IDs (client1_pid, client2_pid)

    // Resolve a stage_id into a UTF-8 name from the locally-discovered
    // game's parsed KGT. Returns empty string if game_id isn't installed
    // locally or the slot is empty / out-of-range. Used at match_result
    // bake time to ship a human-readable stage name to the hub so other
    // players viewing recent matches see "公園" not "Stage #2".
    std::function<std::string(const std::string&, uint32_t)> on_resolve_stage_name;
    // Same shape, parallel hook for char_id → name lookup. Used by the
    // live-matches lobby panel for rows where the wire payload didn't
    // carry char_name (older client / KGT not installed on the sender).
    // Returns empty string when game isn't installed locally.
    std::function<std::string(const std::string&, uint32_t)> on_resolve_char_name;
    
    // Network simulation callbacks removed - not connected to LocalNetworkAdapter
    
    // Rollback monitoring  
    std::function<bool(RollbackStats&)> on_get_rollback_stats;           // Get rollback performance data
    
    // Data binding
    void SetGames(const std::vector<FM2K::FM2KGameInfo>& games);
    void SetNetworkConfig(const NetworkConfig& config);
    void SetLauncherState(LauncherState state);
    void SetFramesAhead(float frames_ahead);
    // Update scanning progress (0-1). Only meaningful while scanning flag is true.
    void SetScanning(bool scanning);
    void SetGamesRootPaths(const std::vector<std::string>& paths);

    // Forward an external TCP addr (learned by the spec hook via TCP-STUN
    // against the hub) to the hub via a `tcp_addr` WS message. Called
    // from FM2KLauncher::Update on tcp_stun_seq SharedMem bumps.
    void SendHubTcpAddr(uint32_t ip_be, uint16_t port);

    // Phase 4: status-bar surface for spec hub-relay ring counters.
    // FM2KLauncher::Update calls every tick with the latest values
    // pulled from spec_relay_out / spec_relay_in. RenderMenuBar shows
    // a small "RELAY out=enq/drop in=enq/drop" widget when active.
    // out_active / in_active flag which rings are currently open.
    struct SpecRelayStatus {
        bool     out_active   = false;
        bool     in_active    = false;
        uint64_t out_enqueued = 0;
        uint64_t out_dropped  = 0;
        uint64_t out_dequeued = 0;
        uint64_t in_enqueued  = 0;
        uint64_t in_dropped   = 0;
        uint64_t in_dequeued  = 0;
        // Frames the HubClient refused after we popped them off the ring:
        // no hub connection, outbox over its byte cap, or discarded at
        // reconnect. The ring counters above cannot see these -- from the
        // ring's point of view the frame was dequeued successfully -- so
        // without this the pill stays green while nothing reaches a viewer.
        uint64_t out_hub_dropped = 0;
    };
    void SetSpecRelayStatus(const SpecRelayStatus& st);

    // HubClient::SpecRelayDropped() passthrough (the hub client type is
    // only visible inside the UI TUs). 0 when no hub state exists.
    uint64_t HubSpecRelayDropped() const;

    // Forward the hook's current session_kind (menu/CSS/battle) to the
    // hub via a `session_kind` WS message. Called from FM2KLauncher::Update
    // on session_kind_seq SharedMem bumps. Hub stores per-user and
    // includes in spectate_grant so spec launchers can decide /F.
    void SendHubSessionKind(uint8_t kind);

    // Forward a packed SpecDataBinary frame to the hub (Phase 2c of v0.3
    // spec rebuild). Called from FM2KLauncher::Update's drain of the
    // hook's outbound spec-relay shared-mem ring. The bytes match
    // hub.py:handle_spec_relay_frame's expected SPDB wire shape; hub
    // fans out to subscribed specs.
    void SendHubSpecRelayFrame(std::vector<uint8_t> frame);

private:
    // Logging
    void AddLog(const char* message);
    void ClearLog();
    static void SDLCustomLogOutput(void* userdata, int category, SDL_LogPriority priority, const char* message);

    // UI state
    std::vector<FM2K::FM2KGameInfo> games_;
    NetworkConfig network_config_;
    float frames_ahead_;
    LauncherState launcher_state_;
    SpecRelayStatus spec_relay_status_{};
    SDL_Renderer* renderer_;
    SDL_Window* window_;
    std::vector<std::string> games_root_paths_;  // Configured games root directories
    int selected_game_index_ = -1; // -1 means no selection
    bool scanning_games_ = false;  // True while background discovery is running

    // Challenge notification toggles. Defaults to all-on so players never
    // miss an incoming challenge while tabbed out. Persists in
    // %APPDATA%\FM2K_Rollback\settings.ini under keys notify_flash,
    // notify_sound, notify_toast. Loaded on first menu-bar render and
    // saved whenever the user toggles a checkbox in Settings →
    // Notifications.
    bool notify_flash_ = true;
    bool notify_sound_ = true;
    bool notify_toast_ = true;
    bool notify_state_loaded_ = false;
    
    // Console Log
    ImGuiTextBuffer log_buffer_;
    SDL_Mutex* log_buffer_mutex_;
    bool scroll_to_bottom_;
    SDL_LogOutputFunction original_log_function_;
    void* original_log_userdata_;

    // UI components
    void RenderGameSelection();
    void RenderNetworkConfig();
    void RenderConnectionStatus();
    void RenderInGameUI();
    void RenderMenuBar();
    void RenderSessionControls();
    void RenderDebugTools();
    void RenderMultiClientTools();
    void RenderNetworkTools();
    void RenderConsoleLog();
    void RenderObjectAnalysis();        // Stub
    void RenderSlotInspectionWindow();  // Stub
    void RenderHubPanel();              // Fightcade-style lobby
    void HandleHubEvent(const fm2k::HubEvent& ev);  // hub WS event dispatch (split into launcher_ui_hub_events.cpp)
    void HandleMatchStartEvent(const fm2k::HubEvent& ev);  // K::MatchStart handler (launcher_ui_hub_events_match.cpp)
    void RenderHostConfigWindow();      // Match-settings UI (SOCD, stage, etc.)
    void RenderHubServerWindow();       // Legacy floating window -- kept for hot-reload paths; new path is the Settings tab.
    void RenderDiscordAuthWindow();     // Stays separate -- OAuth pairing flow has its own state machine.
    void RenderGamesFoldersWindow();    // Legacy.
    void RenderRecentMatchesWindow();   // Legacy.
    void RenderDirectSpecInline();      // "Spectate by IP" -- hub-less spec, rendered in Debug → Network tab

    // Single consolidated Settings window with tabs. Replaces the
    // five floating Settings sub-windows. Floats but is non-movable
    // and non-dockable so it stays a popup-style modal -- open it from
    // Settings → Settings…, do your thing, close it. Tabs: Input P1,
    // Input P2, Host Config, Hub Server, Games Folders, Recent Matches.
    void RenderSettingsWindow();
    // Per-tab body renderers (no Begin/End -- caller owns the container).
    // Reused by both the legacy floating windows and the new Settings tabs.
    void RenderHubServerBody();
    void RenderHostConfigBody();
    void RenderGamesFoldersBody();
    void RenderRecentMatchesBody();
    // Hub-panel "Live Matches" -- shows every InFlightMatch the hub knows
    // about. Char/stage names render via fm2k::FormatCharLabel /
    // FormatStageLabel so names appear when the viewer has the game
    // installed locally OR when a peer baked the names into the hub
    // payload. Refreshes from MatchInProgress* events; no per-render hub
    // round-trip.
    void RenderInProgressMatchesBody();
    // Settings → Display tab. Edits <install_dir>\ddraw.ini for the
    // bundled cnc-ddraw build. State cached in `ddraw_cfg_`; loaded
    // lazily on first tab render via `LoadDDrawCfgIfNeeded`. Per-widget
    // changes write back through fm2k::cnc_ddraw::Save* helpers, which
    // hit the ini through Win32 WritePrivateProfileString -- preserves
    // unknown keys + per-game `[<exe>]` blocks the user might have.
    void RenderDisplayBody();
    void LoadDDrawCfgIfNeeded();
    // Per-player SOCD picker rendered above/below each player's
    // binding tab. SOCD is a local input filter applied before the
    // 11-bit mask hits the wire, so different modes on the two peers
    // do NOT cause desyncs -- each peer's slot has its own setting.
    // Persisted to settings.ini (`socd_mode_p1`, `socd_mode_p2`).
    void RenderInputBindingsTab(int player_slot);
    // Per-launcher SOCD state. Loaded from settings.ini on first menu
    // render; written back when the user changes the picker. Pushed to
    // the spawned game's hook via FM2K_SOCD_MODE env at launch time.
    int  socd_mode_[2] = {1, 1};   // tournament-default (Hitbox SOCD)
    bool socd_state_loaded_ = false;
    void LoadSocdState();
    void SaveSocdState();
    // Random-stage host preference (#56). Persisted PER GAME in the
    // game-patches ini (Patrick, 2026-06-11: "stage range isn't game
    // specific" -- a range tuned for one game's stage list carried into
    // every other game; the legacy global settings.ini values serve as
    // first-load defaults). Consumed at challenge time to populate
    // MatchSettings::random_seed/min/max. random_state_loaded_for_
    // tracks WHICH game the members hold so a game switch reloads.
    bool random_stage_enable_       = false;
    int  random_stage_min_          = 0;
    int  random_stage_max_          = 7;
    int  random_state_loaded_for_   = -2;   // selected_game_index_ tag
    void LoadRandomStageState();
    void SaveRandomStageState();
    void EnsureRandomStageLoaded();
    // Publish FM2K_STAGE_RANDOM_SEED/_MIN/_MAX to the environment so offline
    // launches inherit them (online is re-applied by the hub match-start path).
    void PublishRandomStageEnv();

    // Refresh the SDL window title to "FM2K Rollback Launcher -- <nick> (W-L-D)"
    // any time the user's record changes. No-op if we don't have a record
    // yet (record-fetch races with first lobby render). Called from the
    // K::RecordReceived handler.
    void UpdateWindowTitleWithRecord();

    // Push the current overall + vs-peer W/L/D and the peer/my nick into
    // the running game's FM2KSharedMemData. The hook reads these to
    // render the in-game titlebar (and eventually the in-game overlay)
    // so the player sees their record without alt-tabbing. Called any
    // time the cached record or current peer changes (K::RecordReceived,
    // K::MatchStart). Cheap no-op when there is no active game process.
    void PushStatsToHook();

    // Push a system-message to the in-game HUD (centered overlay
    // with TTL fade). Mirrors PushStatsToHook's PID-resolution path:
    // writes into both running clients' shared-mem mappings if any.
    // No-op when no game is running. Used for netplay events the
    // user benefits from seeing without alt-tabbing (peer dropped,
    // hub-side state change, match starting).
    void PushHudSystemMessage(const char* text_utf8, uint32_t ttl_ms);

    // Append one row to %APPDATA%\FM2K_Rollback\results.csv (#42). Writes
    // a UTF-8 BOM on first creation so Excel renders JP/accented names
    // correctly. Invoked from PollMatchOutcome alongside the hub send so
    // the local log captures the match even when the hub roundtrip fails.
    // outcome_str is the same string we send to the hub (self_won /
    // peer_won / draw / disconnect).
    void AppendResultsCsvRow(const char* outcome_str,
                             uint32_t p1_char_id, uint32_t p2_char_id,
                             const std::string& p1_char_name,
                             const std::string& p2_char_name);
    void LoadAudioMuteState();          // Read %APPDATA%\FM2K_Rollback\audio.ini
    void SaveAudioMuteState();          // Write same file (hook re-reads it)
    void LoadNotifyState();             // Read notify_* keys from settings.ini
    void SaveNotifyState();             // Write notify_* keys to settings.ini
    // Fire all enabled challenge notifications: taskbar flash + sound chirp
    // + Windows toast. Each piece is independently togglable in Settings.
    // Called from the K::ChallengeReceived event handler.
    void FireChallengeNotification(const std::string& from_nick);

    // Fire a generic notification (taskbar flash + sound + toast) with a
    // caller-provided title and body. Used for the peer-disconnect path
    // where the challenge-specific copy doesn't apply, but we still want
    // the user to notice the launcher is taking back focus + closing the
    // match. UTF-8 strings (converted to UTF-16 for Shell_NotifyIconW).
    void FireSystemNotification(const std::string& title_utf8,
                                const std::string& body_utf8);

    // Poll FM2KSharedMemData on every running game PID. When the hook
    // bumps `match_outcome_seq`, read the new outcome enum, map it to a
    // hub `match_result` string, and send. Idempotent across frames --
    // a per-PID last-seen-seq prevents re-sends of the same bump. No-op
    // when no hub match is active or the launcher isn't connected. Also
    // detects the FM2K_MATCH_OUTCOME_DISCONNECT case and asks the local
    // game to close so the surviving instance doesn't stay open after
    // the peer drops.
    void PollMatchOutcome();

    // Drain at most one hook-produced upload manifest per tick. Calls
    // fm2k::upload_queue::Process() against the selected game's
    // upload_queue/ directory, gated on the launcher's
    // "Auto-upload diagnostics" dev checkbox. No-op when no game is
    // selected, when the checkbox is off, or when the queue is empty.
    void PollUploadQueue();

    // Developer mode toggle. End-user UI hides the offline-bisect
    // checkboxes, dual-client launcher, stress test, and spectator
    // chain test. Enabled via FM2K_DEV_MODE=1 env var on launch or
    // via View → Developer Mode in the menu bar.
    bool developer_mode_ = false;

    // Settings windows toggled from the menu bar. input_binder_initialized_
    // gates Init() to a single call (gamepad subsystem startup) the first
    // time the user opens either binder window.
    bool show_settings_        = false;     // Single tabbed Settings window
    bool show_discord_auth_    = false;     // Sign in with Discord -- OAuth flow is its own window
    // Legacy per-section flags kept for any path that still toggles them.
    // The unified Settings window is the user-facing surface now.
    bool show_input_binder_p1_ = false;
    bool show_input_binder_p2_ = false;
    bool show_host_config_     = false;
    bool show_hub_server_      = false;
    bool show_games_folders_   = false;
    bool show_recent_matches_  = false;
    bool show_replay_browser_  = false;
    char direct_spec_addr_[64] = {};        // "Spectate by IP" addr buffer (Debug → Network tab)
    bool input_binder_initialized_ = false;

    // Settings → Display state. `ddraw_cfg_` is loaded once on first
    // open of the Display tab; subsequent edits save back per-key
    // through fm2k::cnc_ddraw::Save* and refresh the cached value
    // immediately so the widget reflects the new state.
    fm2k::cnc_ddraw::IniConfig ddraw_cfg_{};
    bool ddraw_cfg_loaded_ = false;

    // Audio mute toggles (Settings menu). Persisted to
    // %APPDATA%\FM2K_Rollback\audio.ini; the hook DLL re-reads that file
    // ~once per second from inside the dispatcher so changes propagate
    // mid-game without IPC. Booted from the file on first menu render.
    bool mute_bgm_ = false;
    bool mute_se_  = false;
    bool mute_state_loaded_ = false;

    // Host-config staged values (committed on Apply → fm2k_host.ini + env var).
    int      host_config_socd_mode_ = 1;          // tournament default
    uint32_t host_config_stage_     = 0xFFFFFFFFu;// 0xFFFFFFFF = unset
    bool     host_config_dirty_     = false;

    // Hub server hostname / IP. Edited from Settings → Hub Server…
    // (used to live in the Hub panel; moved out so casual users don't
    // see it by default). Persisted via FM2K_HUB_HOST env var on
    // connect. Empty = use FM2K_HUB_HOST env var or hub.2dfm.org.
    char     hub_host_[128] = {};
    bool     hub_host_initialized_ = false;

    // Discord OAuth status, surfaced in the menu bar's sign-in pill.
    // Refreshed on startup, on sign-in completion, and on sign-out;
    // never read every frame so we don't hammer the .json file.
    bool     discord_signed_in_  = false;
    std::string discord_nick_;
    bool     discord_state_loaded_ = false;

    // Hub client + per-frame drained state. Owned by the launcher
    // (forward-declared in LauncherUI scope to avoid pulling
    // FM2K_HubClient.h into the header). Defined out-of-line in
    // FM2K_LauncherUI.cpp.
    struct HubState;
    std::unique_ptr<HubState> hub_state_;

    // UPnP port mapper (Phase 1 NAT reachability). One instance per
    // launcher; StartAsync fires at the hub Connected event for ONLINE
    // sessions only, mapping the game's UDP port on the router so peers can
    // reach us directly. Polled each frame in RenderHubPanel; on the
    // transition into Mapped we re-send udp_addr carrying the external
    // endpoint. Stop() on session teardown / launcher exit. unique_ptr so
    // the miniupnpc-dependent type stays fully out of this header.
    std::unique_ptr<fm2k::PortMapper> port_mapper_;
    // Last PortMapper state we acted on, so the udp_addr re-send fires
    // EXACTLY once per state transition (the Snapshot() is polled every
    // frame; without this we'd spam the hub). int mirror of
    // fm2k::PortMapper::State; -1 = "never polled yet".
    int port_mapper_last_state_ = -1;

public:
    // Visibility-INDEPENDENT tick, driven from FM2KLauncher::Update(). Drains
    // hub events and reports match outcomes. These must keep running while the
    // launcher is minimized/hidden -- Render() is skipped entirely then, which
    // used to silently drop every match_result for the session. Never touches
    // ImGui, which is what makes it safe outside the paint path.
    void TickAlways();

    // Tell the hub the current match (if any) has ended. Called by
    // FM2KLauncher::StopSession on both the user-initiated stop and
    // the game-process-died path. No-op when not in a hub-driven
    // session (the hub will silently treat the match_ended for an
    // already-idle user as a noop).
    void NotifyHubMatchEnded();

    // Helper methods
    void ShowGameValidationStatus(const FM2K::FM2KGameInfo& game);
    void ShowNetworkDiagnostics();      // Stub
    bool ValidateNetworkConfig();
    
    // Simplified theme - always Dark
    enum class UITheme { Dark };
    void SetTheme(UITheme theme);
    UITheme current_theme_;

    // Save state inspection
    int selected_inspection_slot_ = -1;
    bool show_slot_inspection_ = false;

    // C11 -- Replay browser. Lazily-populated cache of .fm2krep / .fm2kset
    // files found across configured games-root paths. Each entry mirrors
    // the FM2KSessionFileHeader fields the tree UI displays. Sessions
    // group entries by session_id; matches inside each session order by
    // match_index_in_session.
    struct ReplayMeta {
        std::string path;             // absolute path
        bool        is_battle_slice;  // .fm2krep (true) vs .fm2kset (false)
        uint64_t    started_at_unix;
        uint64_t    finished_at_unix;
        uint32_t    event_count;
        uint32_t    input_count;
        char        game_id[32];      // null-padded
        char        p1_nick[32];
        char        p2_nick[32];
        uint8_t     p1_char_id;
        uint8_t     p2_char_id;
        uint8_t     rounds_won_p1;
        uint8_t     rounds_won_p2;
        uint8_t     match_count;
        uint8_t     match_index;
        uint64_t    session_id;
        uint8_t     round_count;

        // --- format + provenance (see FM2KSessionFileHeader) --------------
        // file_version != 2 => the hook's loader hard-refuses it, so the row
        // is listed DISABLED rather than dropped from the scan: silently
        // skipping unreadable files is what produced "my replays vanished".
        uint16_t    file_version;
        bool        playable;             // file_version == 2
        uint32_t    producer_version;     // packed maj/min/patch, 0 = pre-provenance
        char        producer_version_s[16];
        uint32_t    game_content_tag;     // install fingerprint at record time, 0 = absent
        uint32_t    local_content_tag;    // same, recomputed now for this file's game dir
        uint32_t    game_speed_pct;       // 0 = not recorded
        uint8_t     socd_mode_plus1;      // 0 = not recorded
    };
    std::vector<ReplayMeta> replays_cache_;
    bool                    replays_cache_dirty_ = true;  // first-render rescan

    // Build replays_cache_ from games_root_paths_. Walks each root for
    // <game>/replays/*.fm2krep -- sniffs the 256-byte FM2KSessionFileHeader
    // off the front of each file. Cheap (<200ms for ~1000 files); called
    // lazily on Replay panel open or after a refresh button.
    void ScanReplays();

    // ImGui body for the Replays window. Renders Session → Match tree.
    // Click a row → on_replay_play(path).
    void RenderReplayBrowser();

    // True when the file records an install fingerprint that disagrees with
    // the one this machine has NOW for that game folder. Both-nonzero-and-
    // different only: 0 on either side means "unknown", which must allow
    // playback (every pre-provenance file is 0). A mismatch is refused rather
    // than played, because the sim garbles from frame 0 with no diagnostic.
    static bool ReplayContentMismatch(const ReplayMeta& m);
};
