#include "FM2K_Integration.h"
#include <iostream>
#include <cstring>

// NetworkSession Implementation
NetworkSession::NetworkSession() 
    : session_(nullptr)
    , local_player_handle_(-1)
    , last_stats_update_(std::chrono::steady_clock::now())
{
}

NetworkSession::~NetworkSession() {
    Stop();
}

bool NetworkSession::Start(const NetworkConfig& config) {
    if (IsActive()) {
        std::cerr << "Network session already active" << std::endl;
        return false;
    }
    
    config_ = config;
    
    std::cout << "Starting GekkoNet session..." << std::endl;
    std::cout << "  Local player: " << config.local_player << std::endl;
    std::cout << "  Local port: " << config.local_port << std::endl;
    std::cout << "  Remote address: " << config.remote_address << std::endl;
    std::cout << "  Input delay: " << config.input_delay << " frames" << std::endl;
    
    // Create GekkoNet session
    if (!gekko_create(&session_)) {
        SDL_Log("Failed to create GekkoNet session");
        return false;
    }
    
    // Configure GekkoNet
    GekkoConfig gekko_config = {
        .num_players = 2,
        .input_size = sizeof(FM2K::Input),
        .state_size = sizeof(FM2K::GameState),
        .max_spectators = static_cast<unsigned char>(config.max_spectators),
        .input_prediction_window = 8,
        .spectator_delay = 2,
        .limited_saving = false,
        .post_sync_joining = false,
        .desync_detection = true
    };
    
    if (gekko_start(session_, &gekko_config) != GekkoResult::Ok) {
        SDL_Log("Failed to start GekkoNet session");
        return false;
    }
    
    // Set up network adapter
    GekkoNetAdapter adapter = gekko_default_adapter(config.local_port);
    if (gekko_net_adapter_set(session_, &adapter) != GekkoResult::Ok) {
        SDL_Log("Failed to set network adapter");
        return false;
    }
    
    // Add local player
    GekkoNetAddress local_addr = { nullptr, 0 };  // Local player doesn't need address
    local_player_handle_ = gekko_add_actor(session_, GekkoPlayerType::LocalPlayer, &local_addr);
    
    // Add remote player
    size_t colon_pos = config.remote_address.find(':');
    if (colon_pos != std::string::npos) {
        std::string host = config.remote_address.substr(0, colon_pos);
        uint16_t port = static_cast<uint16_t>(std::stoi(config.remote_address.substr(colon_pos + 1)));
        
        GekkoNetAddress remote_addr = { 
            const_cast<char*>(host.c_str()),
            static_cast<unsigned int>(host.length())
        };
        gekko_add_actor(session_, GekkoPlayerType::RemotePlayer, &remote_addr);
    }
    
    // Set input delay
    gekko_set_local_delay(session_, local_player_handle_, config.input_delay);
    
    std::cout << "? GekkoNet session started successfully" << std::endl;
    std::cout << "? Local player handle: " << local_player_handle_ << std::endl;
    
    return true;
}

void NetworkSession::Stop() {
    if (!IsActive()) {
        return;
    }
    
    std::cout << "Stopping GekkoNet session..." << std::endl;
    
    gekko_destroy(session_);
    session_ = nullptr;
    local_player_handle_ = -1;
    
    std::cout << "? GekkoNet session stopped" << std::endl;
}

void NetworkSession::Update() {
    if (!session_) return;
    
    // Poll network events
    gekko_network_poll(session_);
    
    // Process game events
    int event_count = 0;
    GekkoGameEvent** events = gekko_update_session(session_, &event_count);
    
    // Process session events
    int session_event_count = 0;
    GekkoSessionEvent** session_events = gekko_session_events(session_, &session_event_count);
    
    // Update network stats
    GekkoNetworkStats stats;
    if (gekko_network_stats(session_, 0, &stats) == GekkoResult::Ok) {
        cached_stats_.ping = stats.last_ping;
        cached_stats_.jitter = stats.jitter;
        cached_stats_.frames_ahead = gekko_frames_ahead(session_);
        cached_stats_.connected = true;
    }
}

void NetworkSession::AddLocalInput(uint32_t input) {
    if (!IsActive() || local_player_handle_ < 0) {
        return;
    }
    
    // Convert to 16-bit for FM2K
    uint16_t fm2k_input = static_cast<uint16_t>(input & 0xFFFF);
    
    gekko_add_local_input(session_, local_player_handle_, &fm2k_input);
}

void NetworkSession::ProcessEvents(FM2KGameInstance* game) {
    if (!IsActive() || !game) {
        return;
    }
    
    HandleGameEvents(game);
}

void NetworkSession::HandleSessionEvents() {
    int event_count = 0;
    auto events = gekko_session_events(session_, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        switch (event->type) {
            case PlayerConnected:
                std::cout << "? Player connected: Handle " << event->data.connected.handle << std::endl;
                break;
                
            case PlayerDisconnected:
                std::cout << "? Player disconnected: Handle " << event->data.disconnected.handle << std::endl;
                cached_stats_.connected = false;
                break;
                
            case DesyncDetected:
                std::cout << "? Desync detected at frame " << event->data.desynced.frame 
                         << ": local=" << std::hex << event->data.desynced.local_checksum
                         << " remote=" << std::hex << event->data.desynced.remote_checksum 
                         << std::dec << std::endl;
                break;
                
            default:
                // Handle other session events as needed
                break;
        }
    }
}

void NetworkSession::HandleGameEvents(FM2KGameInstance* game) {
    int event_count = 0;
    auto updates = gekko_update_session(session_, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        auto ev = updates[i];
        
        switch (ev->type) {
            case SaveEvent:
                {
                    // Save current FM2K state
                    FM2K::GameState state;
                    if (game->SaveState(&state, sizeof(state))) {
                        *ev->data.save.state_len = sizeof(state);
                        *ev->data.save.checksum = state.CalculateChecksum();
                        std::memcpy(ev->data.save.state, &state, sizeof(state));
                    }
                }
                break;
                
            case LoadEvent:
                {
                    // Restore FM2K state
                    if (ev->data.load.state_len >= sizeof(FM2K::GameState)) {
                        game->LoadState(ev->data.load.state, ev->data.load.state_len);
                    }
                }
                break;
                
            case AdvanceEvent:
                {
                    // Extract inputs from GekkoNet and inject into FM2K
                    if (ev->data.adv.input_len >= 2 * sizeof(uint16_t)) {
                        const uint16_t* inputs = static_cast<const uint16_t*>(ev->data.adv.inputs);
                        uint32_t p1_input = inputs[0];
                        uint32_t p2_input = inputs[1];
                        
                        // Inject inputs into FM2K
                        game->InjectInputs(p1_input, p2_input);
                        
                        // Let FM2K process one frame
                        // The actual game logic runs in the FM2K process
                        // Note: In a complete implementation, we'd need to synchronize
                        // with FM2K's frame timing here
                    }
                }
                break;
                
            default:
                std::cout << "Unknown GekkoNet event: " << ev->type << std::endl;
                break;
        }
    }
}

NetworkSession::NetworkStats NetworkSession::GetStats() const {
    return cached_stats_;
} 