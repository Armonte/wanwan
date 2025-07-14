#include "gekkonet_hooks.h"
#include "globals.h"
#include "logging.h"
#include "gekkonet.h"

bool InitializeGekkoNet() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** INITIALIZING GEKKONET WITH REAL UDP NETWORKING (OnlineSession Style) ***");
    
    is_online_mode = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: FORCING ONLINE MODE FOR TESTING");
    
    uint16_t local_port = 7000;
    std::string remote_address = "127.0.0.1:7001";
    
    char* env_player = getenv("FM2K_PLAYER_INDEX");
    char* env_port = getenv("FM2K_LOCAL_PORT");
    char* env_remote = getenv("FM2K_REMOTE_ADDR");
    
    if (env_player) {
        player_index = static_cast<uint8_t>(atoi(env_player));
    }
    
    ::player_index = player_index;
    ::is_host = (player_index == 0);
    InitializeFileLogging();
    
    char* env_input_recording = getenv("FM2K_INPUT_RECORDING");
    if (env_input_recording && strcmp(env_input_recording, "1") == 0) {
        InitializeInputRecording();
    }
    
    char* env_production_mode = getenv("FM2K_PRODUCTION_MODE");
    if (env_production_mode && strcmp(env_production_mode, "1") == 0) {
        ::production_mode = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Production mode enabled - reduced logging");
    }
    
    if (env_port) {
        local_port = static_cast<uint16_t>(atoi(env_port));
    }
    
    if (env_remote) {
        remote_address = std::string(env_remote);
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network config - Player: %u, Local port: %u, Remote: %s", 
                player_index, local_port, remote_address.c_str());
    
    if (!gekko_create(&gekko_session)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to create GekkoNet session!");
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session created successfully");
    
    GekkoConfig config;
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = 10;
    config.spectator_delay = 0;
    config.input_size = sizeof(uint8_t);
    config.state_size = sizeof(uint32_t);
    config.limited_saving = false;
    config.post_sync_joining = false;
    config.desync_detection = true;
    
    gekko_start(gekko_session, &config);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session configured and started");
    
    gekko_net_adapter_set(gekko_session, gekko_default_adapter(local_port));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Real UDP adapter set on port %u", local_port);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Adding players - Player index: %u", player_index);
    
    // Store the original player identity before it gets overwritten
    uint8_t original_player_index = player_index;
    
    // BSNES DETERMINISTIC PATTERN: Ensure consistent handle-to-player mapping
    // Both clients must agree: Handle 0 = P1, Handle 1 = P2
    int p1_handle, p2_handle;
    
    if (original_player_index == 0) {
        // Host controls P1: Local=P1(handle 0), Remote=P2(handle 1)
        p1_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), (unsigned int)remote_address.size() };
        p2_handle = gekko_add_actor(gekko_session, RemotePlayer, &remote);
        local_player_handle = p1_handle;  // Host sends to P1 handle
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Host - P1 handle: %d (LOCAL), P2 handle: %d (REMOTE)", p1_handle, p2_handle);
    } else {
        // Client controls P2: Remote=P1(handle 0), Local=P2(handle 1)
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), (unsigned int)remote_address.size() };
        p1_handle = gekko_add_actor(gekko_session, RemotePlayer, &remote);
        p2_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        local_player_handle = p2_handle;  // Client sends to P2 handle
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Client - P1 handle: %d (REMOTE), P2 handle: %d (LOCAL)", p1_handle, p2_handle);
    }
    
    // Keep player_index as the original identity (0 or 1) for input logic
    player_index = original_player_index;
    
    if (local_player_handle < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to add local player! Handle: %d", local_player_handle);
        gekko_destroy(gekko_session);
        gekko_session = nullptr;
        return false;
    }
    
    gekko_set_local_delay(gekko_session, local_player_handle, 1);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Set input delay for local player handle %d", local_player_handle);
    
    gekko_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialization complete with real UDP networking!");
    return true;
}

void CleanupGekkoNet() {
    if (gekko_session) {
        gekko_destroy(gekko_session);
        gekko_session = nullptr;
        gekko_initialized = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet session closed");
    }
}

bool AllPlayersValid() {
    if (!gekko_session || !gekko_initialized) {
        return false;
    }
    
    if (!gekko_session_started) {
        gekko_network_poll(gekko_session);
        
        int session_event_count = 0;
        auto session_events = gekko_session_events(gekko_session, &session_event_count);
        
        bool session_started_event_found = false;
        for (int i = 0; i < session_event_count; i++) {
            auto event = session_events[i];
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session Event: %d", event->type);
            
            if (event->type == SessionStarted) {
                session_started_event_found = true;
            } else if (event->type == DesyncDetected) {
                auto desync = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "=== DESYNC DETECTED ===");
                // ... (rest of desync logging)
                GenerateDesyncReport(desync.frame, desync.local_checksum, desync.remote_checksum);
            } else if (event->type == PlayerDisconnected) {
                auto disco = event->data.disconnected;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player disconnected: %d", disco.handle);
            } else if (event->type == PlayerConnected) {
                auto connected = event->data.connected;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player connected: %d", connected.handle);
            }
        }
        
        if (session_event_count == 0) {
            static int no_events_counter = 0;
            if (++no_events_counter % 300 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: No session events received yet - still waiting for network handshake... (attempt %d)", no_events_counter);
            }
        }
        
        if (session_started_event_found) {
            gekko_session_started = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SESSION STARTED - All players connected and synchronized! (BSNES AllPlayersValid pattern)");
            return true;
        }
        
        return false;
    }
    
    return true;
}

void ConfigureNetworkMode(bool online_mode, bool host_mode) {
    is_online_mode = online_mode;
    is_host = host_mode;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network mode configured - Online: %s, Host: %s", 
                online_mode ? "YES" : "NO", host_mode ? "YES" : "NO");
} 