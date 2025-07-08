#include "FM2K_GekkoNetBridge.h"
#include "FM2K_GameInstance.h"
#include <chrono>
#include <cstring>

namespace FM2K {

// Timing constants (FM2K runs at 100 FPS)
using fm2k_frame = std::chrono::duration<unsigned int, std::ratio<1, 100>>;
using slow_frame = std::chrono::duration<unsigned int, std::ratio<1, 99>>;
using normal_frame = std::chrono::duration<unsigned int, std::ratio<1, 100>>;
using fast_frame = std::chrono::duration<unsigned int, std::ratio<1, 101>>;

GekkoNetBridge::GekkoNetBridge()
    : session_(nullptr)
    , local_player_handle_(-1)
    , game_instance_(nullptr)
    , current_state_(std::make_unique<State::GameState>())
    , accumulator_(0.0f)
    , target_frame_time_(0.01f) // 100 FPS = 10ms per frame
{
}

GekkoNetBridge::~GekkoNetBridge() {
    Shutdown();
}

bool GekkoNetBridge::Initialize(const FM2KNetworkConfig& config) {
    config_ = config;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "Initializing GekkoNet bridge: player %d, port %d, remote %s",
        config.local_player, config.local_port, config.remote_address.c_str());
    
    if (!InitializeGekkoSession()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize GekkoNet session");
        return false;
    }
    
    ConfigureNetworking();
    AddPlayers();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet bridge initialized successfully");
    return true;
}

void GekkoNetBridge::Shutdown() {
    if (session_) {
        gekko_destroy(session_);
        session_ = nullptr;
    }
    local_player_handle_ = -1;
    game_instance_ = nullptr;
}

bool GekkoNetBridge::IsConnected() const {
    return session_ != nullptr;
}

void GekkoNetBridge::SetGameInstance(FM2KGameInstance* game_instance) {
    game_instance_ = game_instance;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game instance connected to GekkoNet bridge");
}

bool GekkoNetBridge::InitializeGekkoSession() {
    // Create GekkoNet session
    if (gekko_create(&session_) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create GekkoNet session");
        return false;
    }
    
    // Configure session
    GekkoConfig conf{};
    conf.num_players = 2;                           // FM2K is 2-player
    conf.input_size = sizeof(uint16_t);             // FM2K uses 16-bit input mask
    conf.state_size = sizeof(State::CoreGameState); // Our state structure
    conf.max_spectators = 0;                        // No spectators for now
    conf.input_prediction_window = config_.max_prediction_window;
    conf.desync_detection = config_.desync_detection;
    
    gekko_start(session_, &conf);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet session started successfully");
    
    return true;
}

void GekkoNetBridge::ConfigureNetworking() {
    // Set up network adapter
    gekko_net_adapter_set(session_, gekko_default_adapter(config_.local_port));
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "GekkoNet networking configured on port %d", config_.local_port);
}

void GekkoNetBridge::AddPlayers() {
    // Player order is important for deterministic behavior
    if (config_.local_player == 0) {
        // Add local player first, then remote
        local_player_handle_ = gekko_add_actor(session_, LocalPlayer, nullptr);
        
        auto remote = GekkoNetAddress{ 
            (void*)config_.remote_address.c_str(), 
            (unsigned int)config_.remote_address.size() 
        };
        gekko_add_actor(session_, RemotePlayer, &remote);
    } else {
        // Add remote player first, then local
        auto remote = GekkoNetAddress{ 
            (void*)config_.remote_address.c_str(), 
            (unsigned int)config_.remote_address.size() 
        };
        gekko_add_actor(session_, RemotePlayer, &remote);
        
        local_player_handle_ = gekko_add_actor(session_, LocalPlayer, nullptr);
    }
    
    // Set input delay
    gekko_set_local_delay(session_, local_player_handle_, config_.input_delay);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "Players added: local handle %d, input delay %d frames",
        local_player_handle_, config_.input_delay);
}

void GekkoNetBridge::Update(float delta_time) {
    if (!session_) return;
    
    accumulator_ += delta_time;
    
    // Get adaptive frame timing based on network conditions
    float frames_ahead = gekko_frames_ahead(session_);
    target_frame_time_ = GetFrameTime(frames_ahead);
    
    // Poll network for incoming data
    gekko_network_poll(session_);
    
    // Process frames
    while (accumulator_ >= target_frame_time_) {
        ProcessGekkoEvents();
        ProcessGameUpdates();
        accumulator_ -= target_frame_time_;
    }
}

void GekkoNetBridge::AddLocalInput(const FM2KInput& input) {
    if (!session_ || local_player_handle_ < 0) return;
    
    // Add input to GekkoNet session
    gekko_add_local_input(session_, local_player_handle_, const_cast<void*>(static_cast<const void*>(&input.input.value)));
}

void GekkoNetBridge::ProcessGekkoEvents() {
    if (!session_) return;
    
    // Process network events (disconnections, desyncs, etc.)
    int event_count = 0;
    auto events = gekko_session_events(session_, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        auto event = events[i];
        
        switch (event->type) {
            case DesyncDetected: {
                auto desync = event->data.desynced;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "DESYNC DETECTED: frame %d, remote_handle %d, local_checksum %08x, remote_checksum %08x",
                    desync.frame, desync.remote_handle, desync.local_checksum, desync.remote_checksum);
                break;
            }
            
            case PlayerDisconnected: {
                auto disco = event->data.disconnected;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Player disconnected: handle %d", disco.handle);
                break;
            }
            
            default:
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
                    "GekkoNet event: %d", event->type);
                break;
        }
    }
}

void GekkoNetBridge::ProcessGameUpdates() {
    if (!session_) return;
    
    // Get game update events from GekkoNet
    int update_count = 0;
    auto updates = gekko_update_session(session_, &update_count);
    
    for (int i = 0; i < update_count; i++) {
        auto event = updates[i];
        
        switch (event->type) {
            case SaveEvent:
                OnSaveState(event);
                break;
                
            case LoadEvent:
                OnLoadState(event);
                break;
                
            case AdvanceEvent:
                OnAdvanceFrame(event);
                break;
                
            default:
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                    "Unknown GekkoNet update event: %d", event->type);
                break;
        }
    }
}

void GekkoNetBridge::OnSaveState(GekkoGameEvent* event) {
    if (!game_instance_ || !current_state_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot save state - no game instance");
        return;
    }
    
    // Save current game state using our state manager
    if (!State::SaveCoreState(&current_state_->core)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to save core game state");
        return;
    }
    
    // Calculate checksum
    current_state_->checksum = State::CalculateCoreStateChecksum(&current_state_->core);
    current_state_->frame_number = event->data.save.frame;
    current_state_->timestamp_ms = SDL_GetTicks();
    
    // Provide data to GekkoNet
    *event->data.save.state_len = sizeof(State::CoreGameState);
    *event->data.save.checksum = current_state_->checksum;
    std::memcpy(event->data.save.state, &current_state_->core, sizeof(State::CoreGameState));
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "Saved state for frame %d, checksum %08x", 
        event->data.save.frame, current_state_->checksum);
}

void GekkoNetBridge::OnLoadState(GekkoGameEvent* event) {
    if (!game_instance_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot load state - no game instance");
        return;
    }
    
    // Copy state from GekkoNet
    std::memcpy(&current_state_->core, event->data.load.state, sizeof(State::CoreGameState));
    current_state_->frame_number = event->data.load.frame;
    current_state_->timestamp_ms = SDL_GetTicks();
    
    // Load state into game using our state manager
    if (!State::LoadCoreState(&current_state_->core)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load core game state");
        return;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "Loaded state for frame %d", event->data.load.frame);
}

void GekkoNetBridge::OnAdvanceFrame(GekkoGameEvent* event) {
    if (!game_instance_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot advance frame - no game instance");
        return;
    }
    
    // Extract inputs for both players
    uint16_t p1_input = event->data.adv.inputs[0];
    uint16_t p2_input = event->data.adv.inputs[1];
    
    // Inject inputs into game
    game_instance_->InjectInputs(p1_input, p2_input);
    
    // Advance the game by one frame
    if (!game_instance_->AdvanceFrame()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to advance game frame");
        return;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "Advanced frame %d with inputs P1:%04x P2:%04x", 
        event->data.adv.frame, p1_input, p2_input);
}

float GekkoNetBridge::GetFrameTime(float frames_ahead) const {
    // Adaptive frame timing based on network conditions
    if (frames_ahead >= 0.75f) {
        // Running behind - slow down slightly
        return std::chrono::duration<float>(slow_frame(1)).count();
    } else {
        // Normal timing
        return std::chrono::duration<float>(normal_frame(1)).count();
    }
}

FM2KNetworkStats GekkoNetBridge::GetNetworkStats() const {
    FM2KNetworkStats stats{};
    
    if (!session_) return stats;
    
    // Get stats from GekkoNet
    GekkoNetworkStats gekko_stats{};
    int remote_handle = (local_player_handle_ == 0) ? 1 : 0;
    gekko_network_stats(session_, remote_handle, &gekko_stats);
    
    // Convert to our format
    stats.ping_ms = gekko_stats.last_ping;
    stats.avg_ping_ms = gekko_stats.avg_ping;
    stats.jitter_ms = gekko_stats.jitter;
    stats.frames_ahead = gekko_frames_ahead(session_);
    
    return stats;
}

} // namespace FM2K