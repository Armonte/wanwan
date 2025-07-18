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
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Environment FM2K_PLAYER_INDEX=%s → player_index=%d", env_player, player_index);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: ERROR - FM2K_PLAYER_INDEX environment variable not set!");
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
    
    // CRITICAL: Store original player index before reassignment
    original_player_index = player_index;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Setting original_player_index=%d", original_player_index);
    
    // EXACT GEKKONET REFERENCE PATTERN: Order-dependent actor addition
    // The reference example shows this is the correct way to do it
    // EXACT WORKING PATTERN from dllmain_orig.cpp - REASSIGN player_index like OnlineSession reference
    if (player_index == 0) {
        // add local player - REASSIGN player_index to handle like OnlineSession line 219
        player_index = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        local_player_handle = player_index;  // HOST: should be handle 0
        // add remote player
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), (unsigned int)remote_address.size() };
        gekko_add_actor(gekko_session, RemotePlayer, &remote);
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: HOST - original_player=%d, LOCAL handle: %d", original_player_index, local_player_handle);
    } else {
        // add remote player
        auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), (unsigned int)remote_address.size() };
        gekko_add_actor(gekko_session, RemotePlayer, &remote);
        // add local player - REASSIGN player_index to handle like OnlineSession line 228
        player_index = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        local_player_handle = player_index;  // CLIENT: should be handle 1
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: CLIENT - original_player=%d, LOCAL handle: %d", original_player_index, local_player_handle);
    }
    
    if (local_player_handle < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to add local player! Handle: %d", local_player_handle);
        gekko_destroy(gekko_session);
        gekko_session = nullptr;
        return false;
    }
    
    gekko_set_local_delay(gekko_session, local_player_handle, 2);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Set input delay to 2 frames for local player handle %d", local_player_handle);
    
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
        // CRITICAL: Increment handshake timeout to prevent infinite blocking
        handshake_timeout_frames++;
        
        // ESCAPE MECHANISM: After 10 seconds (1000 frames at 100fps), force session start
        if (handshake_timeout_frames > 1000) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: TIMEOUT - Forcing session start after %u frames to prevent deadlock", handshake_timeout_frames);
            gekko_session_started = true;
            
            // Enable frame control even on timeout to maintain consistency
            gekko_frame_control_enabled = true;
            can_advance_frame = false;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: FRAME CONTROL ENABLED (timeout) - FM2K now waits for AdvanceEvent");
            
            return true;
        }
        
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
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: No session events received yet - still waiting for network handshake... (attempt %d, timeout in %u frames)", 
                           no_events_counter, 1000 - handshake_timeout_frames);
            }
        }
        
        if (session_started_event_found) {
            gekko_session_started = true;
            handshake_timeout_frames = 0; // Reset timeout on success
            
            // ENABLE THORN'S FRAMESTEP PATTERN - GekkoNet now controls FM2K frame advancement
            gekko_frame_control_enabled = true;
            can_advance_frame = false; // Start blocking immediately
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SESSION STARTED - All players connected and synchronized! (BSNES AllPlayersValid pattern)");
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: FRAME CONTROL ENABLED - FM2K now waits for AdvanceEvent to progress frames");
            return true;
        }
        
        return false;
    }
    
    // Track when players are valid for timeout purposes
    last_valid_players_frame = g_frame_counter;
    return true;
}

void ConfigureNetworkMode(bool online_mode, bool host_mode) {
    is_online_mode = online_mode;
    is_host = host_mode;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network mode configured - Online: %s, Host: %s", 
                online_mode ? "YES" : "NO", host_mode ? "YES" : "NO");
} 