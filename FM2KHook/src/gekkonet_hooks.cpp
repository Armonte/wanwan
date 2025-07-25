#include "gekkonet_hooks.h"
#include "globals.h"
#include "logging.h"
#include "input_handler.h"
#include "gekkonet.h"
#include "state_manager.h"  // For GameState
#include <SDL3/SDL_log.h>
#include <cstdint>
#include <memory>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <string>

bool InitializeGekkoNet() {
    // Set network session flag in the game state machine

    // Logging for network session initialization
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: *** REIMPLEMENTING FM2K MAIN LOOP WITH GEKKONET CONTROL ***");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Initializing GgekkoNet...");
    // CSS filtering removed for simplified input handling
    
    static uint16_t local_port = 7000;
    static std::string remote_address = "127.0.0.1:7001";
    
    // Player index and is_host should already be set by dllmain.cpp
    // Just read the environment variables for network configuration
    char* env_port = getenv("FM2K_LOCAL_PORT");
    char* env_remote = getenv("FM2K_REMOTE_ADDR");
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Using player_index=%d (already set by DllMain)", ::player_index);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Environment variables - FM2K_LOCAL_PORT=%s, FM2K_REMOTE_ADDR=%s", 
                env_port ? env_port : "NOT SET", env_remote ? env_remote : "NOT SET");
    
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
    } else {
        // Fallback: Auto-configure ports for dual client testing
        // Player 0 (host) uses port 7000, Player 1 (client) uses port 7001
        local_port = 7000 + ::player_index;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Auto-configured local port %u for player %d", 
                    local_port, ::player_index);
    }
    
    if (env_remote) {
        remote_address = std::string(env_remote);
    } else {
        // Fallback: Auto-configure remote address for dual client testing
        // Host connects to client port, client connects to host port
        uint16_t remote_port = 7000 + (1 - ::player_index); // Opposite player's port
        remote_address = "127.0.0.1:" + std::to_string(remote_port);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Auto-configured remote address %s for player %d", 
                    remote_address.c_str(), ::player_index);
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: Network config - Player: %u, Local port: %u, Remote: %s", 
                player_index, local_port, remote_address.c_str());
    
    // LIKE BSNES-NETPLAY: Proper player setup
    GekkoConfig config;
    config.num_players = 2;
    config.max_spectators = 0;
    config.input_prediction_window = 10;  // ROLLBACK mode
    config.input_size = sizeof(uint16_t); // 2 bytes per input
    config.state_size = sizeof(int32_t); // 4 bytes for network state (exactly like bsnes)
    config.desync_detection = true;
    config.limited_saving = false;
    config.post_sync_joining = false;
    config.spectator_delay = 0;
    
    // Create session like bsnes-netplay
    gekko_create(&gekko_session);
    gekko_start(gekko_session, &config);
    
    // Set network adapter like bsnes-netplay
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Setting up network adapter on port %u", local_port);
    
    auto adapter = gekko_default_adapter(local_port);
    if (!adapter) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Failed to create network adapter on port %u", local_port);
        return false;
    }
    
    gekko_net_adapter_set(gekko_session, adapter);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Network adapter configured successfully");
    
    // FIX: Both players should be in online mode when connecting to each other
    // Check if we have a remote address - if so, we're in online mode
    bool is_online_session = !remote_address.empty();
    
    if (is_online_session) {
        // Online session: Ensure Player 1 gets handle 0, Player 2 gets handle 1
        // This matches BSNES approach where inputs[0]=P1, inputs[1]=P2
        
        GekkoNetAddress remote_addr;
        remote_addr.data = (void*)remote_address.c_str();
        remote_addr.size = remote_address.length();
        
        if (::player_index == 0) {
            // Player 1 (host): Add self first (gets handle 0), then remote (gets handle 1)
            local_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
            int remote_handle = gekko_add_actor(gekko_session, RemotePlayer, &remote_addr);
            
            if (local_player_handle == -1 || remote_handle == -1) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: P1 failed to add players - Local: %d, Remote: %d", 
                            local_player_handle, remote_handle);
                return false;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: P1 added - local_handle=%d (P1), remote_handle=%d (P2)", 
                        local_player_handle, remote_handle);
        } else {
            // Player 2 (client): Add remote first (gets handle 0), then self (gets handle 1) 
            int remote_handle = gekko_add_actor(gekko_session, RemotePlayer, &remote_addr);
            local_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
            
            if (local_player_handle == -1 || remote_handle == -1) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: P2 failed to add players - Remote: %d, Local: %d", 
                            remote_handle, local_player_handle);
                return false;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: P2 added - remote_handle=%d (P1), local_handle=%d (P2)", 
                        remote_handle, local_player_handle);
        }
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player %d controls handle %d", 
                    ::player_index == 0 ? 1 : 2, local_player_handle);
        
        // Set online mode flags
        is_online_mode = true;
        is_local_session = false;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Online session detected - connecting to %s", remote_address.c_str());
    } else {
        // Local session: Add both players as local
        p1_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        p2_player_handle = gekko_add_actor(gekko_session, LocalPlayer, nullptr);
        
        if (p1_player_handle == -1 || p2_player_handle == -1) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Failed to add local players");
            return false;
        }
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Added local players P1=%d, P2=%d", p1_player_handle, p2_player_handle);
        
        // Set local mode flags
        is_online_mode = false;
        is_local_session = true;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Local session detected");
    }
    
    gekko_initialized = true;
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
    // CRITICAL: Network polling MUST happen before checking events (like BSNES)
    gekko_network_poll(gekko_session);
    
    int update_count_check = 0;
    auto updates_check = gekko_update_session(gekko_session, &update_count_check);

    // Debug: Log all events we're receiving during handshake
    static uint32_t debug_attempts = 0;
    debug_attempts++;
    if (debug_attempts <= 10 || debug_attempts % 300 == 0) { // Log first 10 attempts, then every 5 seconds
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Handshake attempt %d - received %d events", 
                    debug_attempts, update_count_check);
        for (int i = 0; i < update_count_check; i++) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Event %d: type=%d", i, updates_check[i]->type);
        }
    }

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
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Connection established successfully!");
            connection_logged = true;
        }
        
        gekko_session_started = true;
        gekko_frame_control_enabled = true;
        can_advance_frame = false;
        return true;
    }

    // Add timeout detection for failed connections
    static uint32_t connection_attempts = 0;
    connection_attempts++;
    
    if (connection_attempts % 600 == 0) { // Every 10 seconds at 60fps
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Still waiting for connection after %d attempts", connection_attempts);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Local port: %u, Remote: %s", GetGekkoLocalPort(), GetGekkoRemoteIP());
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session state - initialized: %s, started: %s", 
                    gekko_initialized ? "YES" : "NO", gekko_session_started ? "YES" : "NO");
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

// Process GekkoNet frame - extracted from Hook_ProcessGameInputs for reuse
void ProcessGekkoNetFrame() {
    // STEP 1: Always capture real inputs (equivalent to BSNES netplayPollLocalInput)
    CaptureRealInputs();
    
    // STEP 2: Always send inputs to GekkoNet (equivalent to BSNES gekko_add_local_input)
    // This must happen regardless of session state to establish the connection
    if (is_local_session) {
        // Local session: Send both players' inputs
        uint16_t p1_input = (uint16_t)(live_p1_input & 0x7FF);
        uint16_t p2_input = (uint16_t)(live_p2_input & 0x7FF);
        gekko_add_local_input(gekko_session, p1_player_handle, &p1_input);
        gekko_add_local_input(gekko_session, p2_player_handle, &p2_input);
    } else {
        // Online session: Each client sends only their player's input
        if (::player_index == 0) {
            uint16_t p1_input = (uint16_t)(live_p1_input & 0x7FF);
            gekko_add_local_input(gekko_session, local_player_handle, &p1_input);
            
            static uint32_t input_log_counter = 0;
            if (++input_log_counter % 300 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: P1 sending input 0x%04X (live_p1_input=0x%08X)", 
                           p1_input, live_p1_input);
            }
        } else {
            uint16_t p2_input = (uint16_t)(live_p2_input & 0x7FF);
            gekko_add_local_input(gekko_session, local_player_handle, &p2_input);
            
            static uint32_t input_log_counter = 0;
            if (++input_log_counter % 300 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: P2 sending input 0x%04X (live_p2_input=0x%08X)", 
                           p2_input, live_p2_input);
            }
        }
    }
    
    // STEP 3: Always process connection events (equivalent to BSNES gekko_session_events)
    int event_count = 0;
    auto events = gekko_session_events(gekko_session, &event_count);
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        if (event->type == PlayerConnected) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player Connected - handle %d", event->data.connected.handle);
        } else if (event->type == PlayerDisconnected) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Player Disconnected - handle %d", event->data.disconnected.handle);
        } else if (event->type == SessionStarted) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: Session Started!");
            gekko_session_started = true;
            gekko_frame_control_enabled = true;
        }
    }
    
    // STEP 4: Always process updates (SaveEvent, LoadEvent, AdvanceEvent)
    // This is the core of BSNES gekko_update_session processing
    gekko_network_poll(gekko_session);
    int update_count = 0;
    auto updates = gekko_update_session(gekko_session, &update_count);
    
    // Reset frame advance flag - will be set by AdvanceEvent if we should advance
    can_advance_frame = false;
    use_networked_inputs = false;
    
    for (int i = 0; i < update_count; i++) {
        auto update = updates[i];
        switch (update->type) {
            case SaveEvent:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: SaveEvent frame %d", update->data.save.frame);
                // Minimal state like BSNES - just 4 bytes
                *update->data.save.checksum = 0;
                *update->data.save.state_len = sizeof(int32_t);
                memcpy(update->data.save.state, &update->data.save.frame, sizeof(int32_t));
                break;
                
            case LoadEvent:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: LoadEvent frame %d", update->data.load.frame);
                // TODO: Implement rollback state loading
                break;
                
            case AdvanceEvent:
                // This is the key - only advance when GekkoNet says so (like BSNES emulator->run())
                can_advance_frame = true;
                use_networked_inputs = true;
                gekko_frame_control_enabled = true;
                
                // Copy networked inputs from GekkoNet (like BSNES memcpy)
                if (update->data.adv.inputs && update->data.adv.input_len >= sizeof(uint16_t) * 2) {
                    uint16_t* networked_inputs = (uint16_t*)update->data.adv.inputs;
                    
                    // Store synchronized inputs for use in FM2K_ProcessGameInputs_GekkoNet
                    networked_p1_input = networked_inputs[0];
                    networked_p2_input = networked_inputs[1];
                    
                    static uint32_t advance_counter = 0;
                    if (++advance_counter % 300 == 0) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet: AdvanceEvent #%d - P1=0x%04X P2=0x%04X", 
                                   advance_counter, networked_p1_input, networked_p2_input);
                    }
                }
                break;
        }
    }
} 