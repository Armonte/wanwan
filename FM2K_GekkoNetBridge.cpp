#include "FM2K_GekkoNetBridge.h"
#include "FM2K_Integration.h"
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
    , p2_player_handle_(-1)
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
    
    // Route to appropriate initialization method based on session mode
    if (config_.session_mode == ::SessionMode::LOCAL) {
        return InitializeLocalSession(config);
    } else {
        return InitializeOnlineSession(config);
    }
}

bool GekkoNetBridge::InitializeLocalSession(const FM2KNetworkConfig& config) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "Initializing GekkoNet LOCAL session: input_delay=%d", config.input_delay);
    
    // Create GekkoNet session
    gekko_create(&session_);
    if (!session_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create GekkoNet session");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet session created successfully");
    
    // Configure session for LOCAL MODE (following LocalSession.cpp pattern)
    GekkoConfig conf{};
    conf.num_players = 2;                           // FM2K is 2-player
    conf.input_size = sizeof(uint8_t);              // Use 8-bit inputs like LocalSession.cpp
    conf.state_size = 0;                            // No state saving for local testing
    conf.max_spectators = 0;                        // No spectators for local testing
    conf.input_prediction_window = 0;               // No prediction needed for local testing
    conf.desync_detection = false;                  // No desync detection for local testing
    
    gekko_start(session_, &conf);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet session started successfully");
    
    // NO network configuration for local mode
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LOCAL SESSION: Skipping network configuration (both players local)");
    
    // Add two local players (following LocalSession.cpp pattern)
    local_player_handle_ = gekko_add_actor(session_, LocalPlayer, nullptr);  // P1
    p2_player_handle_ = gekko_add_actor(session_, LocalPlayer, nullptr);      // P2
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "Local session: P1 handle=%d, P2 handle=%d", 
        local_player_handle_, p2_player_handle_);
    
    // Set input delay for both players
    gekko_set_local_delay(session_, local_player_handle_, config_.input_delay);
    gekko_set_local_delay(session_, p2_player_handle_, config_.input_delay);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "LOCAL session initialized: P1 handle %d, P2 handle %d, input delay %d frames",
        local_player_handle_, p2_player_handle_, config_.input_delay);
    
    return true;
}

bool GekkoNetBridge::InitializeOnlineSession(const FM2KNetworkConfig& config) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "Initializing GekkoNet ONLINE session: player %d, port %d, remote %s",
        config.local_player, config.local_port, config.remote_address.c_str());
    
    // Create GekkoNet session
    gekko_create(&session_);
    if (!session_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create GekkoNet session");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet session created successfully");
    
    // Configure session for ONLINE MODE (following OnlineSession.cpp pattern)
    GekkoConfig conf{};
    conf.num_players = 2;                           // FM2K is 2-player
    conf.input_size = sizeof(uint8_t);              // Use 8-bit inputs like OnlineSession.cpp
    conf.state_size = sizeof(State::CoreGameState); // Our state structure for rollback
    conf.max_spectators = 0;                        // No spectators for now
    conf.input_prediction_window = config.max_prediction_window; // Enable prediction for online
    conf.desync_detection = config.desync_detection; // Enable desync detection for online
    
    gekko_start(session_, &conf);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet session started successfully");
    
    // Set up network adapter for online mode
    auto adapter = gekko_default_adapter(config.local_port);
    gekko_net_adapter_set(session_, adapter);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Network adapter set for port %d", config.local_port);
    
    // Add players in correct order (following OnlineSession.cpp pattern)
    if (config.local_player == 0) {
        // Add local player first, then remote
        local_player_handle_ = gekko_add_actor(session_, LocalPlayer, nullptr);
        
        GekkoNetAddress remote_addr{ 
            const_cast<void*>(static_cast<const void*>(config.remote_address.c_str())), 
            static_cast<unsigned int>(config.remote_address.length()) 
        };
        p2_player_handle_ = gekko_add_actor(session_, RemotePlayer, &remote_addr);
    } else {
        // Add remote player first, then local
        GekkoNetAddress remote_addr{ 
            const_cast<void*>(static_cast<const void*>(config.remote_address.c_str())), 
            static_cast<unsigned int>(config.remote_address.length()) 
        };
        p2_player_handle_ = gekko_add_actor(session_, RemotePlayer, &remote_addr);
        
        local_player_handle_ = gekko_add_actor(session_, LocalPlayer, nullptr);
    }
    
    // Set input delay for local player
    gekko_set_local_delay(session_, local_player_handle_, config_.input_delay);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "ONLINE session initialized: local player %d (handle %d), remote handle %d, input delay %d frames",
        config.local_player, local_player_handle_, p2_player_handle_, config_.input_delay);
    
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


void GekkoNetBridge::Update(float delta_time) {
    if (!session_) return;
    
    // For LOCAL mode, inputs drive updates directly in AddBothInputs()
    // For ONLINE mode, we use timing-based updates
    if (config_.session_mode == ::SessionMode::LOCAL) {
        // Only process session events (disconnections, etc.)
        ProcessGekkoEvents();
        return;
    }
    
    // ONLINE mode: timing-based processing
    accumulator_ += delta_time;
    
    float frames_ahead = gekko_frames_ahead(session_);
    target_frame_time_ = GetFrameTime(frames_ahead);
    
    // Poll network for incoming data
    gekko_network_poll(session_);
    
    // Process frames when enough time has accumulated
    while (accumulator_ >= target_frame_time_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "ONLINE: Processing frame: accumulator=%.3f, target_frame_time=%.3f", 
            accumulator_, target_frame_time_);
        
        ProcessGekkoEvents();
        ProcessGameUpdates();
        accumulator_ -= target_frame_time_;
    }
}

// Convert FM2K 16-bit input to GekkoNet 8-bit format
uint8_t GekkoNetBridge::ConvertInputToGekkoFormat(const FM2KInput& input) {
    uint8_t gekko_input = 0;
    
    // Map FM2K input bits to 8-bit GekkoNet format
    // Directions (4 bits) + buttons (4 bits)
    if (input.input.bits.left)    gekko_input |= 0x01;  // bit 0
    if (input.input.bits.right)   gekko_input |= 0x02;  // bit 1
    if (input.input.bits.up)      gekko_input |= 0x04;  // bit 2
    if (input.input.bits.down)    gekko_input |= 0x08;  // bit 3
    if (input.input.bits.button1) gekko_input |= 0x10;  // bit 4
    if (input.input.bits.button2) gekko_input |= 0x20;  // bit 5
    if (input.input.bits.button3) gekko_input |= 0x40;  // bit 6
    if (input.input.bits.button4) gekko_input |= 0x80;  // bit 7
    
    return gekko_input;
}

void GekkoNetBridge::AddLocalInput(const FM2KInput& input) {
    if (!session_ || local_player_handle_ < 0) return;
    
    // Convert FM2K input to GekkoNet 8-bit format
    uint8_t gekko_input = ConvertInputToGekkoFormat(input);
    
    // Add input to GekkoNet session
    gekko_add_local_input(session_, local_player_handle_, &gekko_input);
}

void GekkoNetBridge::AddBothInputs(const FM2KInput& p1_input, const FM2KInput& p2_input) {
    if (!session_ || local_player_handle_ < 0 || p2_player_handle_ < 0) return;
    
    // Convert both inputs to GekkoNet 8-bit format
    uint8_t p1_gekko = ConvertInputToGekkoFormat(p1_input);
    uint8_t p2_gekko = ConvertInputToGekkoFormat(p2_input);
    
    // Add both P1 and P2 inputs to GekkoNet session (LocalSession pattern)
    gekko_add_local_input(session_, local_player_handle_, &p1_gekko);
    gekko_add_local_input(session_, p2_player_handle_, &p2_gekko);
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
        "Added inputs: P1=%04x, P2=%04x", p1_input.input.value, p2_input.input.value);
    
    // CRITICAL: Process GekkoNet updates immediately after adding inputs (LocalSession pattern)
    ProcessGameUpdates();
}

SessionMode GekkoNetBridge::GetSessionMode() const {
    return config_.session_mode;
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
    if (!session_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ProcessGameUpdates: session_ is null");
        return;
    }
    
    // Get game update events from GekkoNet
    int update_count = 0;
    auto updates = gekko_update_session(session_, &update_count);
    
    // Debug: Always log the update count
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "ProcessGameUpdates: gekko_update_session returned %d events", update_count);
    
    for (int i = 0; i < update_count; i++) {
        auto event = updates[i];
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            "Processing GekkoNet event %d: type=%d", i, event->type);
        
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
    // LOCAL mode with state_size=0 should not trigger save events
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SaveState called in LOCAL mode - this should not happen");
    
    // For LOCAL mode, just return without saving state
    if (event && event->data.save.state_len) {
        *event->data.save.state_len = 0;
    }
    if (event && event->data.save.checksum) {
        *event->data.save.checksum = 0;
    }
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
    
    // Extract inputs for both players (converted back from 8-bit to 16-bit for logging)
    uint8_t p1_gekko = event->data.adv.inputs[0];
    uint8_t p2_gekko = event->data.adv.inputs[1];
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "GekkoNet AdvanceEvent: Frame %d with inputs P1:0x%02x P2:0x%02x", 
        event->data.adv.frame, p1_gekko, p2_gekko);
    
    // For FM2K integration, the game drives itself through the hook
    // GekkoNet provides confirmed inputs via AdvanceEvent
    // The hook should use these inputs when the game asks for them
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