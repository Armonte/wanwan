// launcher_callbacks.cpp -- FM2KLauncher::WireUICallbacks(), split out of
// FM2K_RollbackClient.cpp. Pure move: this is the block of ui_->on_* lambda
// assignments that connect the LauncherUI's callbacks to launcher logic
// (session start/stop, spectate, replay, debug save-states, hub spectator
// punch, spec-relay inbound, etc.). It's a FM2KLauncher member function so it
// touches only `this->` members + header-declared helpers -- no internal
// header needed. Called once from FM2KLauncher::Initialize().

#include "SDL3/SDL.h"
#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/netplay/spec_relay_queue.h"
#include "FM2KHook/src/netplay/content_tag.h"  // replay content gate

#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

void FM2KLauncher::WireUICallbacks() {
    ui_->on_game_selected = [this](const FM2K::FM2KGameInfo& game) {
        SetSelectedGame(game);
    };
    ui_->on_offline_session_start = [this]() {
        StartOfflineSession();
    };
    ui_->on_online_session_start = [this](const NetworkConfig& config) {
        StartOnlineSession(config, config.is_host);
    };
    ui_->on_stress_session_start = [this]() {
        StartStressSession();
    };
    ui_->on_session_stop = [this]() {
        StopSession();
    };
    ui_->on_spectator_punch_target = [this](const std::string& spec_udp_ip,
                                            int                spec_udp_port,
                                            const std::string& spec_user_id) {
        // Hub forwarded a spectator's external UDP addr (we're the host of
        // an active match). Write into our running game instance's shared
        // mem so the hook's TickHostMaintenance polls the seq bump and
        // fires a UDP heartbeat burst toward it, opening our NAT for the
        // spectator's SPEC_JOIN_REQ and the reliable-channel stream that
        // follows on the same UDP socket.
        DWORD target_pid = 0;
        if (game_instance_ && game_instance_->IsRunning()) {
            target_pid = game_instance_->GetProcessId();
        } else if (client1_instance_ && client1_instance_->IsRunning()) {
            // Dev-mode dual-clients fallback -- local-test spectator path.
            target_pid = client1_instance_->GetProcessId();
        }
        if (target_pid == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: spectator_incoming with no running game instance "
                "to deliver punch target to (addr=%s:%d) -- dropping",
                spec_udp_ip.c_str(), spec_udp_port);
            return;
        }
        // Resolve dotted IPv4 -> network-byte-order u32 for StartPunch.
        IN_ADDR addr_bin{};
        if (inet_pton(AF_INET, spec_udp_ip.c_str(), &addr_bin) != 1 ||
            spec_udp_port <= 0 || spec_udp_port > 0xFFFF) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: spectator_incoming bad addr %s:%d -- dropping",
                spec_udp_ip.c_str(), spec_udp_port);
            return;
        }
        const std::string mapping_name =
            "FM2K_SharedMem_" + std::to_string((unsigned)target_pid);
        HANDLE h = OpenFileMappingA(FILE_MAP_WRITE, FALSE,
                                    mapping_name.c_str());
        if (!h) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: spectator_incoming: OpenFileMapping('%s') failed: %lu",
                mapping_name.c_str(), GetLastError());
            return;
        }
        FM2KSharedMemData* shm = static_cast<FM2KSharedMemData*>(
            MapViewOfFile(h, FILE_MAP_WRITE, 0, 0,
                          sizeof(FM2KSharedMemData)));
        if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC) {
            shm->spectator_punch_ip_be    = addr_bin.S_un.S_addr;
            shm->spectator_punch_port     = (uint16_t)spec_udp_port;
            // Phase 2c: also write spec_user_id (relay-mode addressing).
            // Truncate to fit (32-byte buffer, 31 chars + NUL). Empty
            // when the hub doesn't include spec_user_id (older hub);
            // hook treats absent user_id as "no relay routing" and
            // addresses the sub by its UDP tuple instead.
            std::memset(shm->spectator_punch_user_id, 0,
                        sizeof(shm->spectator_punch_user_id));
            const size_t uid_max = sizeof(shm->spectator_punch_user_id) - 1;
            const size_t uid_n   = std::min<size_t>(spec_user_id.size(), uid_max);
            if (uid_n > 0) {
                std::memcpy(shm->spectator_punch_user_id,
                            spec_user_id.data(), uid_n);
            }
            // Bump seq AFTER the addr writes -- hook's poll reads addr
            // only when seq advances, so a torn write is harmless.
            shm->spectator_punch_seq  += 1;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: queued spectator-punch target %s udp:%d "
                "user_id=%s to game pid %lu (seq=%u)",
                spec_udp_ip.c_str(), spec_udp_port,
                spec_user_id.empty() ? "(none)" : spec_user_id.c_str(),
                (unsigned long)target_pid,
                (unsigned)shm->spectator_punch_seq);
        }
        if (shm) UnmapViewOfFile(shm);
        CloseHandle(h);
    };

    // Phase 3: spec hub-relay inbound. Hub forwarded SpecDataHeader-
    // prefixed wire bytes for our spec game; write into its inbound
    // shared-mem ring. Lazy-open the ring keyed by game pid (similar
    // pattern to the outbound drain in Update). The hook's TickHealth
    // drains the ring and dispatches each Slot through HandleSpecData.
    ui_->on_spec_relay_bytes = [this](const std::vector<uint8_t>& bytes) {
        // Spectator slot FIRST -- inbound relay bytes are spec data for a
        // VIEWER, and a viewer's game is spawned into spectator_instance_,
        // never into game_instance_. Resolving player-slots-only here (the
        // pre-fix code) meant target_pid stayed 0 on every viewer launcher
        // and EVERY inbound frame was dropped before the ring was opened,
        // i.e. relay spectate could not work at all. Shared with the
        // outbound drain via SpecRelayInboundPid() so the two can't drift.
        const uint32_t target_pid = SpecRelayInboundPid();
        if (target_pid == 0) {
            // Game not running -- nothing to deliver to. Frames arriving
            // before spec game boots are normal at the very start; drop.
            return;
        }
        // Inbound ring cached as class member (spec_relay_in_ring_) so
        // the status pill in the menu bar can read its counters. Was
        // lambda-static; promotion required for menu-bar visibility.
        auto* in_ring_ptr =
            static_cast<fm2k::spec_relay::Ring*>(spec_relay_in_ring_);
        if (target_pid != spec_relay_in_pid_) {
            if (in_ring_ptr) {
                fm2k::spec_relay::Close(in_ring_ptr);
                in_ring_ptr = nullptr;
                spec_relay_in_ring_ = nullptr;
            }
            spec_relay_in_pid_ = target_pid;
        }
        // Retry open until success. Hook's mapping creation races our
        // first WS-binary delivery; the cache would otherwise stick at
        // nullptr if the first open happens before the hook is ready.
        if (!in_ring_ptr) {
            in_ring_ptr = fm2k::spec_relay::OpenInboundFor(target_pid);
            spec_relay_in_ring_ = in_ring_ptr;
            if (in_ring_ptr) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpecRelay: opened inbound ring for spec game pid %lu",
                    (unsigned long)target_pid);
            }
        }
        if (!in_ring_ptr) {
            // Mapping still not available (hook still booting, or hook
            // not in relay mode). Drop this WS frame; next one retries.
            return;
        }
        // The bytes are exactly the payload the hook hands to
        // SpectatorNode_HandleSpecData. Enqueue with TARGET_BROADCAST
        // (kind isn't load-bearing on the inbound side; we set it for
        // consistency) and zero header metadata.
        fm2k::spec_relay::Enqueue(
            in_ring_ptr,
            fm2k::spec_relay::TARGET_BROADCAST,
            /*spec_user_id=*/nullptr,
            /*spec_data_type=*/0,
            /*frame_count=*/0,
            /*spec_data_flags=*/0,
            bytes.data(), static_cast<uint32_t>(bytes.size()));
    };

    ui_->on_spectate_match = [this](const std::string& host_ip, int host_port,
                                    const std::string& session_kind,
                                    const std::string& spec_transport) {
        // Need an installed game to point the spectator at; reuse whatever
        // the launcher currently has selected.
        if (selected_game_.exe_path.empty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Spectate: no game selected -- pick one before clicking Spectate");
            return;
        }
        // Phase 4: tell the user-facing log clearly which mode they're
        // about to enter. Relay-mode hosts route spec data through the
        // hub which costs ~30-50 ms extra latency but works behind any
        // NAT class. TCP-mode hosts use direct P2P (faster but blocked
        // by symmetric NAT). The auto-derivation already set the env;
        // this log is purely informational so testers know which path
        // is active.
        if (spec_transport == "relay") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spectate: host advertises spec_transport=relay -- spec "
                "will receive data via hub WS binary frames. Watch the "
                "RELAY pill in the menu bar for ring counters; drops "
                "(red) indicate snapshot or event corruption.");
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spectate: direct P2P mode (reliable channel over UDP). "
                "If this fails behind symmetric NAT, ask host to set "
                "FM2K_SPEC_TRANSPORT=relay and retry.");
        }
        // Pick a free local UDP port for the spectator's bind. 7002 by
        // convention (above the host's 7000 and the client's 7001).
        // If a spectator is already running, LaunchRemoteSpectator returns
        // false; user can stop it from the multi-client tools first.
        constexpr int SPEC_LOCAL_PORT = 7002;
        // ARGUMENT ORDER. LaunchRemoteSpectator is
        //   (game, port, host_ip, host_port, session_kind, spec_transport)
        // and this call used to pass spec_transport into a since-deleted
        // `mode` slot, leaving spec_transport at its "tcp" default. So the hub
        // path -- the ONLY path that negotiates a transport -- pinned
        // FM2K_SPEC_TRANSPORT=tcp on every spectator no matter what the grant
        // said, and relay-mode spectating could never work for a real user.
        // The defaults are gone so a wrong-arity call is a compile error now.
        LaunchRemoteSpectator(selected_game_.exe_path, SPEC_LOCAL_PORT,
                              host_ip, host_port, session_kind, spec_transport);
    };
    ui_->on_exit = [this]() {
        running_ = false;
    };

    // C11 -- Replay browser dispatch. Resolve the game .exe from the
    // replay file's grandparent directory (replays/<file>.fm2krep is
    // always under <game_dir>/replays/) -- same logic as the --replay
    // CLI flag. Then call LaunchReplayPlayer to spawn the game with
    // FM2K_REPLAY_FILE set.
    ui_->on_replay_play = [this](const std::string& replay_path) {
        std::error_code ec;
        std::filesystem::path replay_fs = std::filesystem::u8path(replay_path);
        std::filesystem::path canon =
            std::filesystem::weakly_canonical(replay_fs, ec);
        if (ec) canon = replay_fs;
        std::filesystem::path game_dir =
            canon.parent_path().parent_path();

        std::string game_exe_path;
        if (!game_dir.empty() &&
            std::filesystem::is_directory(game_dir, ec)) {
            std::filesystem::path kgt_stem;
            for (const auto& entry :
                 std::filesystem::directory_iterator(game_dir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
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
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Replay browser: could not locate game .exe under %s",
                game_dir.string().c_str());
            return;
        }

        // Content gate (second line of defence -- the browser already disables
        // the button for a mismatched row, this covers any other dispatcher).
        // A replay recorded against a DIFFERENT install of the same game plays
        // without a single error and garbles from the first frame, because the
        // recorded input stream drives a sim whose character data no longer
        // matches. Both-nonzero-and-different only: 0 on either side is
        // "unknown" and must not block, since every pre-provenance file is 0.
        {
            std::ifstream hf(replay_fs, std::ios::binary);
            uint8_t hbuf[256] = {};
            if (hf) hf.read(reinterpret_cast<char*>(hbuf), sizeof(hbuf));
            uint32_t magic = 0, recorded_tag = 0;
            uint16_t ver = 0;
            if (hf && hf.gcount() == (std::streamsize)sizeof(hbuf)) {
                std::memcpy(&magic, hbuf + 0, 4);
                std::memcpy(&ver,   hbuf + 4, 2);
                std::memcpy(&recorded_tag, hbuf + 200, 4);   // game_content_tag
            }
            if (magic == 0x53534D46u && ver == 2 && recorded_tag != 0) {
                // Name the exe through the SHARED resolver rather than reusing
                // game_exe_path's filename, so this gate and the browser's
                // cannot drift apart in what they hash. It resolves to the same
                // <kgt stem>.exe we just verified above; if it ever cannot, it
                // returns empty and ComputeForDir gives 0 = unknown, which
                // leaves the gate open rather than blocking on a guess.
                const std::wstring exe_name =
                    fm2k::content_tag::ResolveGameExeName(
                        game_dir.wstring().c_str());
                const uint32_t local_tag = fm2k::content_tag::ComputeForDir(
                    game_dir.wstring().c_str(), exe_name.c_str());
                if (local_tag != 0 && local_tag != recorded_tag) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "Replay browser: REFUSING %s -- recorded against a "
                        "different install of this game (file tag 0x%08X, "
                        "installed 0x%08X). Playing it would desync from the "
                        "first frame with no on-screen sign; the .player / "
                        ".kgt / .exe set under %s has changed since it was "
                        "recorded.",
                        replay_path.c_str(), recorded_tag, local_tag,
                        game_dir.string().c_str());
                    return;
                }
            }
        }

        LaunchReplayPlayer(game_exe_path, replay_path);
    };

    ui_->on_games_folders_set = [this](const std::vector<std::string>& folders) {
        SetGamesRootPaths(folders);
    };
    
    // Connect debug state callbacks
    ui_->on_debug_save_state = [this]() -> bool {
        if (game_instance_) {
            return game_instance_->TriggerManualSaveState();
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for debug save state");
        return false;
    };
    
    ui_->on_debug_load_state = [this]() -> bool {
        if (game_instance_) {
            return game_instance_->TriggerManualLoadState();
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for debug load state");
        return false;
    };
    
    ui_->on_debug_force_rollback = [this](uint32_t frames) -> bool {
        if (game_instance_) {
            return game_instance_->TriggerForceRollback(frames);
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for debug rollback");
        return false;
    };
    
    // Connect frame stepping callbacks
    ui_->on_frame_step_pause = [this](bool pause) {
        if (game_instance_) {
            game_instance_->SetFrameStepPause(pause);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for frame stepping");
        }
    };
    
    ui_->on_frame_step_single = [this]() {
        if (game_instance_) {
            game_instance_->StepSingleFrame();
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for frame stepping");
        }
    };
    
    ui_->on_frame_step_multi = [this](uint32_t frames) {
        if (game_instance_) {
            game_instance_->StepMultipleFrames(frames);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for frame stepping");
        }
    };
    
    // Connect slot-based save/load callbacks
    ui_->on_debug_save_to_slot = [this](uint32_t slot) -> bool {
        if (game_instance_) {
            return game_instance_->TriggerSaveToSlot(slot);
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for save to slot");
        return false;
    };
    
    ui_->on_debug_load_from_slot = [this](uint32_t slot) -> bool {
        if (game_instance_) {
            return game_instance_->TriggerLoadFromSlot(slot);
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for load from slot");
        return false;
    };
    
    ui_->on_debug_auto_save_config = [this](bool enabled, uint32_t interval_frames) -> bool {
        if (game_instance_) {
            game_instance_->SetAutoSaveEnabled(enabled);
            return game_instance_->SetAutoSaveInterval(interval_frames);
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for auto-save config");
        return false;
    };
    
    // Connect auto-save config reading callback
    ui_->on_get_auto_save_config = [this](LauncherUI::AutoSaveConfig& config) -> bool {
        if (game_instance_) {
            FM2KGameInstance::AutoSaveConfig game_config;
            if (game_instance_->GetAutoSaveConfig(game_config)) {
                config.enabled = game_config.enabled;
                config.interval_frames = game_config.interval_frames;
                return true;
            }
        }
        return false;
    };
    
    // Connect debug and testing configuration callbacks
    ui_->on_set_production_mode = [this](bool enabled) -> bool {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "UI callback: SetProductionMode(%s)", enabled ? "true" : "false");
        bool success = false;
        bool deferred = false;
        
        if (game_instance_) {
            success = game_instance_->SetProductionMode(enabled);
            if (!success) {
                deferred = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Production mode config deferred - will be applied when shared memory is ready");
            }
        }
        if (client1_instance_) {
            if (client1_instance_->SetProductionMode(enabled)) {
                success = true;
            }
        }
        if (client2_instance_) {
            if (client2_instance_->SetProductionMode(enabled)) {
                success = true;
            }
        }
        
        return success || deferred;
    };
    
    ui_->on_set_input_recording = [this](bool enabled) -> bool {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "UI callback: SetInputRecording(%s)", enabled ? "true" : "false");
        bool success = false;
        bool deferred = false;
        
        if (game_instance_) {
            success = game_instance_->SetInputRecording(enabled);
            if (!success) {
                deferred = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Input recording config deferred - will be applied when shared memory is ready");
            }
        }
        if (client1_instance_) {
            if (client1_instance_->SetInputRecording(enabled)) {
                success = true;
            }
        }
        if (client2_instance_) {
            if (client2_instance_->SetInputRecording(enabled)) {
                success = true;
            }
        }
        
        return success || deferred;
    };
    
    ui_->on_set_minimal_gamestate_testing = [this](bool enabled) -> bool {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "UI callback: SetMinimalGameStateTesting(%s)", enabled ? "true" : "false");
        bool success = false;
        bool deferred = false;
        
        // Check if any game instances exist
        if (game_instance_ || client1_instance_ || client2_instance_) {
            // Apply to existing instances
            if (game_instance_) {
                success = game_instance_->SetMinimalGameStateTesting(enabled);
                if (!success) {
                    deferred = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "MinimalGameState testing config deferred - will be applied when shared memory is ready");
                }
            }
            if (client1_instance_) {
                if (client1_instance_->SetMinimalGameStateTesting(enabled)) {
                    success = true;
                }
            }
            if (client2_instance_) {
                if (client2_instance_->SetMinimalGameStateTesting(enabled)) {
                    success = true;
                }
            }
        } else {
            // No instances exist yet - store as pending configuration
            pending_config_.has_minimal_gamestate_testing = true;
            pending_config_.minimal_gamestate_testing_value = enabled;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "No game instances exist yet - storing MinimalGameState testing config as pending: %s", enabled ? "enabled" : "disabled");
            return true;  // Successfully stored as pending
        }
        
        return success || deferred;  // Return true if either applied or deferred
    };
    
    // Save profile callback removed - now using optimized FastGameState system
    
    // Connect slot status callback
    ui_->on_get_slot_status = [this](uint32_t slot, LauncherUI::SlotStatusInfo& status) -> bool {
        if (game_instance_) {
            FM2KGameInstance::SlotStatus game_status;
            if (game_instance_->GetSlotStatus(slot, game_status)) {
                status.occupied = game_status.occupied;
                status.frame_number = game_status.frame_number;
                status.timestamp_ms = game_status.timestamp_ms;
                status.checksum = game_status.checksum;
                status.state_size_kb = game_status.state_size_kb;
                status.save_time_us = game_status.save_time_us;
                status.load_time_us = game_status.load_time_us;
                status.active_object_count = game_status.active_object_count;
                return true;
            }
        }
        return false;
    };
    
    // Enhanced actions removed from shared memory - no longer available
    ui_->on_get_enhanced_actions = [this]() -> std::vector<LauncherUI::EnhancedActionInfo> {
        std::vector<LauncherUI::EnhancedActionInfo> enhanced_actions;
        return enhanced_actions;
    };
    
    // Connect multi-client testing callbacks
    ui_->on_launch_local_client1 = [this](const std::string& game_path) -> bool {
        return LaunchLocalClient(game_path, true, 7000);  // Host on port 7000
    };
    
    ui_->on_launch_local_client2 = [this](const std::string& game_path) -> bool {
        return LaunchLocalClient(game_path, false, 7001);  // Guest on port 7001
    };

    ui_->on_launch_local_spectator = [this](const std::string& game_path) -> bool {
        // Spectator subscribes to client1 (host on 7000), itself bound on 7002.
        return LaunchLocalSpectator(game_path, /*spectator_port=*/7002, /*host_port=*/7000);
    };

    ui_->on_launch_local_spectator2 = [this](const std::string& game_path) -> bool {
        // Daisy-chain: spectator 2 subscribes to spectator 1 (port 7002),
        // bound on 7003. Validates the relay-forward path.
        return LaunchLocalSpectator2(game_path, /*spectator_port=*/7003, /*upstream_port=*/7002);
    };
    
    ui_->on_terminate_all_clients = [this]() -> bool {
        bool success = TerminateAllClients();
        return success;
    };
    
    ui_->on_get_client_status = [this](uint32_t& client1_pid, uint32_t& client2_pid) -> bool {
        bool client1_running = (client1_instance_ && client1_instance_->IsRunning());
        bool client2_running = (client2_instance_ && client2_instance_->IsRunning());

        client1_pid = client1_running ? client1_instance_->GetProcessId() : 0;
        client2_pid = client2_running ? client2_instance_->GetProcessId() : 0;

        // Online (single-game) sessions live on game_instance_, not the
        // dev-mode dual slots. Surface that PID through the same channel
        // so the W/L/D shared-mem poll has something to read.
        if (client1_pid == 0 && game_instance_ && game_instance_->IsRunning()) {
            client1_pid = game_instance_->GetProcessId();
            client1_running = true;
        }

        return client1_running || client2_running;
    };

    ui_->on_resolve_stage_name = [this](const std::string& game_id,
                                        uint32_t stage_id) -> std::string {
        if (stage_id == 0xFFFFFFFFu) return {};
        const fm2k::KgtSummary* k = FindKgtByGameId(game_id);
        if (!k) return {};
        const std::string& n = k->StageName((int)stage_id);
        return n;  // empty string if slot empty / out-of-range
    };

    ui_->on_resolve_char_name = [this](const std::string& game_id,
                                       uint32_t char_id) -> std::string {
        if (char_id == 0xFFFFFFFFu) return {};
        const fm2k::KgtSummary* k = FindKgtByGameId(game_id);
        if (!k) return {};
        const std::string& n = k->PlayerName((int)char_id);
        return n;
    };

    // Network simulation callbacks removed - not connected to LocalNetworkAdapter
    
    ui_->on_get_rollback_stats = [this](RollbackStats& stats) -> bool {
        // Read real rollback statistics from hook DLL shared memory
        return ReadRollbackStatsFromSharedMemory(stats);
    };
}
