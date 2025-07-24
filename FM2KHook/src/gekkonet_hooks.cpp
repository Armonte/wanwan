#include "gekkonet_hooks.h"
#include "globals.h"
#include "logging.h"
#include "gekkonet.h"
#include "game_state_machine.h"
#include "state_manager.h"  // For GameState
#include <SDL3/SDL_log.h>
#include <cstdint>
#include <memory>
#include <vector>
#include <cstring>
#include <cstdlib>

bool InitializeGekkoNet() {
    // Set network session flag in the game state machine
    FM2K::State::g_game_state_machine.SetNetworkSession(true);

    // Logging for network session initialization
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** REIMPLEMENTING FM2K MAIN LOOP WITH GEKKONET CONTROL ***");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing GgekkoNet...");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** INITIALIZING GEKKONET WITH ROLLBACK NETCODE (3-Frame Prediction) ***");
    // CSS filtering removed for simplified input handling
    
    static uint16_t local_port = 7000;
    static std::string remote_address = "127.0.0.1:7001";
    
    // Player index and is_host should already be set by dllmain.cpp
    // Just read the environment variables for network configuration
    char* env_port = getenv("FM2K_LOCAL_PORT");
    char* env_remote = getenv("FM2K_REMOTE_ADDR");
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Using player_index=%d (already set by DllMain)", ::player_index);
    
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
    
    // Store config globally for deferred start
    static GekkoConfig stored_config;
    stored_config.num_players = 2;
    stored_config.max_spectators = 0;
    stored_config.input_prediction_window = 10;  // ROLLBACK mode - test CSS with prediction frames
    // Previously was 0 (lockstep) - now testing rollback compatibility
    stored_config.spectator_delay = 0;
    stored_config.input_size = sizeof(uint16_t);
    stored_config.state_size = sizeof(FM2K::State::GameState);  // Use full GameState for proper save states
    stored_config.limited_saving = false;
    stored_config.post_sync_joining = false;
    stored_config.desync_detection = true;
    
    // Check for true offline mode (no networking at all)
    char* env_offline = getenv("FM2K_TRUE_OFFLINE");
    bool is_true_offline = (env_offline && strcmp(env_offline, "1") == 0);
    
    // Set network adapter BEFORE adding players (following OnlineSession example)
    if (!is_true_offline) {
        auto adapter = gekko_default_adapter(local_port);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Creating UDP adapter for port %u - adapter ptr: %p", local_port, adapter);
        gekko_net_adapter_set(gekko_session, adapter);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Real UDP adapter set on port %u", local_port);
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: TRUE OFFLINE - No network adapter set");
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Adding players - Player index: %u", player_index);
    
    // CRITICAL: Store original player index before reassignment
    original_player_index = player_index;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Setting original_player_index=%d", original_player_index);
    
    if (is_true_offline) {
        // TRUE OFFLINE MODE: Both players LocalPlayer, no network adapter
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Setting up TRUE OFFLINE session (no networking)");
        
        p1_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        p2_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        
        // No network adapter for true offline
        is_local_session = true;
        // For offline mode, set local_player_handle based on original_player_index
        local_player_handle = (original_player_index == 0) ? p1_player_handle : p2_player_handle;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: TRUE OFFLINE - P1 handle: %d, P2 handle: %d, local_player_handle: %d", p1_player_handle, p2_player_handle, local_player_handle);
    } else {
        // ONLINE SESSION PATTERN: One LocalPlayer, one RemotePlayer (includes localhost testing)
        bool is_localhost_test = (remote_address.find("127.0.0.1") != std::string::npos);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Setting up %s session", 
                   is_localhost_test ? "LOCALHOST TESTING" : "ONLINE");
        
        if (player_index == 0) {
            // HOST: add local player first (gets handle 0)
            local_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
            p1_player_handle = local_player_handle;  // HOST controls P1
            
            // add remote player (gets handle 1)
            auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), (unsigned int)remote_address.size() };
            p2_player_handle = gekko_add_actor(gekko_session, RemotePlayer, &remote);
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: HOST - player_index=%d, LOCAL handle: %d (P1), REMOTE handle: %d (P2)", 
                       player_index, local_player_handle, p2_player_handle);
        } else {
            // CLIENT: add remote player first (gets handle 0)
            auto remote = GekkoNetAddress{ (void*)remote_address.c_str(), (unsigned int)remote_address.size() };
            p1_player_handle = gekko_add_actor(gekko_session, RemotePlayer, &remote);
            
            // add local player (gets handle 1)
            local_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
            p2_player_handle = local_player_handle;  // CLIENT controls P2
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: CLIENT - player_index=%d, REMOTE handle: %d (P1), LOCAL handle: %d (P2)", 
                       player_index, p1_player_handle, local_player_handle);
        }
    }
    
    if (local_player_handle < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Failed to add local player! Handle: %d", local_player_handle);
        gekko_destroy(gekko_session);
        gekko_session = nullptr;
        return false;
    }
    
    // Only set delay for online sessions, not true offline mode
    if (!is_true_offline) {
        gekko_set_local_delay(gekko_session, local_player_handle, 1);  // Match launcher: 1 frame delay
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Set input delay to 1 frame for local player handle %d", local_player_handle);
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: TRUE OFFLINE - No input delay set (both players equal)");
    }
    
    gekko_initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet initialization complete - ready for synchronized start!");
    
    // For online mode, defer start until proper synchronization
    if (!is_true_offline) {
        gekko_needs_synchronized_start = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Online mode - deferring GekkoNet start for synchronization");
    } else {
        // For true offline mode, start immediately since no sync needed
        gekko_start(gekko_session, &stored_config);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: GekkoNet started immediately (offline mode)");
    }
    
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

    // Handle deferred start for online sessions
    if (gekko_needs_synchronized_start) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Starting deferred session with frame synchronization");
        
        // Reset frame counters to 0 before starting GekkoNet
        extern uint32_t g_frame_counter;
        g_frame_counter = 0;
        
        uint32_t* continuous_frame_ptr = (uint32_t*)0x004456FC;  // g_negate_interpolation_value_frame_counter
        if (!IsBadWritePtr(continuous_frame_ptr, sizeof(uint32_t))) {
            *continuous_frame_ptr = 0;
        }
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Frame counters reset to 0, starting session NOW");
        
        // Get the stored config
        static GekkoConfig stored_config;
        stored_config.num_players = 2;
        stored_config.max_spectators = 0;
        stored_config.input_prediction_window = 10;
        stored_config.spectator_delay = 0;
        stored_config.input_size = sizeof(uint16_t);
        stored_config.state_size = sizeof(FM2K::State::GameState);
        stored_config.limited_saving = false;
        stored_config.post_sync_joining = false;
        stored_config.desync_detection = true;
        
        gekko_start(gekko_session, &stored_config);
        gekko_needs_synchronized_start = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Synchronized start complete - both clients should start at F0");
    }

    // If session is already started, we're good.
    if (gekko_session_started) {
        return true;
    }

    // For TRUE OFFLINE sessions with local players, immediately mark as valid
    if (is_local_session) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: TRUE OFFLINE mode - both players are local, no handshake needed");
        gekko_session_started = true;
        gekko_frame_control_enabled = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: FRAME CONTROL ENABLED (offline mode)");
        return true;
    }

    // For online sessions, check for AdvanceEvents to confirm connection.
    // This is the real handshake.
    int update_count_check = 0;
    auto updates_check = gekko_update_session(gekko_session, &update_count_check);

    bool got_advance_events = false;
    for (int i = 0; i < update_count_check; i++) {
        if (updates_check[i]->type == AdvanceEvent) {
            got_advance_events = true;
            break;
        }
    }

    if (got_advance_events) {
        static bool connection_logged = false;
        if (!connection_logged) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: First AdvanceEvent received. All players are now valid.");
            connection_logged = true;
        }
        
        gekko_session_started = true;
        gekko_frame_control_enabled = true;
        can_advance_frame = false;
        return true;
    }

    // Not yet connected and validated.
    return false;
}

void ConfigureNetworkMode(bool online_mode, bool host_mode) {
    is_online_mode = online_mode;
    is_host = host_mode;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network mode configured - Online: %s, Host: %s", 
                online_mode ? "YES" : "NO", host_mode ? "YES" : "NO");
}

uint16_t GetGekkoLocalPort() {
    static uint16_t cached_port = 0;
    static bool port_initialized = false;
    
    if (!port_initialized) {
        cached_port = 7000;  // Default
        char* env_port = getenv("FM2K_LOCAL_PORT");
        if (env_port) {
            cached_port = static_cast<uint16_t>(atoi(env_port));
        }
        port_initialized = true;
    }
    
    return cached_port;
}

const char* GetGekkoRemoteIP() {
    static std::string cached_ip;
    static bool ip_initialized = false;
    
    if (!ip_initialized) {
        cached_ip = "127.0.0.1";  // Default
        char* env_remote = getenv("FM2K_REMOTE_ADDR");
        if (env_remote) {
            std::string remote_addr = std::string(env_remote);
            // Extract IP from "IP:PORT" format
            size_t colon_pos = remote_addr.find(':');
            if (colon_pos != std::string::npos) {
                cached_ip = remote_addr.substr(0, colon_pos);
            } else {
                cached_ip = remote_addr;
            }
        }
        ip_initialized = true;
    }
    
    return cached_ip.c_str();
} 