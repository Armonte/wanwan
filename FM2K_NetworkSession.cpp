#include "FM2K_Integration.h"
#include "gekkonet.h"
#include <iostream>
#include <cstring>
#include <SDL3/SDL.h>

// NetworkSession Implementation
NetworkSession::NetworkSession() 
    : session_(nullptr)
    , local_player_handle_(-1)
    , game_instance_(nullptr)
    , state_mutex_(nullptr)
    , input_buffer_lock_(nullptr)
    , rollback_thread_(nullptr)
    , network_thread_(nullptr)
{
    // Initialize SDL synchronization primitives
    state_mutex_ = SDL_CreateMutex();
    input_buffer_lock_ = SDL_CreateRWLock();
    
    // Initialize atomic counters for 100 FPS timing
    SDL_SetAtomicInt(&frame_counter_, 0);
    SDL_SetAtomicInt(&rollback_flag_, 0);
    SDL_SetAtomicInt(&running_, 0);
    SDL_SetAtomicInt(&last_confirmed_frame_, 0);
    SDL_SetAtomicInt(&prediction_window_, 2); // Start with 2 frame prediction (20ms at 100 FPS)
    
    // Pre-allocate state buffer (128 frames ? 1.28 seconds at 100fps)
    state_buffer_.resize(STATE_BUFFER_SIZE);
}

NetworkSession::~NetworkSession() {
    Stop();
    
    if (state_mutex_) {
        SDL_DestroyMutex(state_mutex_);
        state_mutex_ = nullptr;
    }
    
    if (input_buffer_lock_) {
        SDL_DestroyRWLock(input_buffer_lock_);
        input_buffer_lock_ = nullptr;
    }
}

bool NetworkSession::Start(const NetworkConfig& config) {
    if (!gekko_create(&session_)) {
        std::cerr << "Failed to create GekkoNet session\n";
        return false;
    }

    // Configure GekkoNet session
    GekkoConfig gekko_config = {};
    gekko_config.num_players = 2;  // 2 players for FM2K
    gekko_config.max_spectators = config.max_spectators;
    gekko_config.input_prediction_window = config.input_delay;
    gekko_config.spectator_delay = 2;  // 2 frame delay for spectators
    gekko_config.input_size = sizeof(uint32_t);  // FM2K uses 32-bit input
    gekko_config.state_size = sizeof(FM2K::GameState);
    gekko_config.limited_saving = false;  // Full state saving
    gekko_config.post_sync_joining = true;  // Allow late joining
    gekko_config.desync_detection = true;  // Enable desync detection

    // Set up network adapter
    GekkoNetAdapter* adapter = gekko_default_adapter(config.local_port);
    if (!adapter) {
        std::cerr << "Failed to create network adapter\n";
        return false;
    }
    gekko_net_adapter_set(session_, adapter);

    // Add local player
    GekkoNetAddress remote_addr = {};
    remote_addr.data = const_cast<char*>(config.remote_address.c_str());
    remote_addr.size = config.remote_address.length() + 1;
    
    local_player_handle_ = gekko_add_actor(session_, LocalPlayer, nullptr);
    if (local_player_handle_ < 0) {
        std::cerr << "Failed to add local player\n";
        return false;
    }

    // Add remote player
    if (gekko_add_actor(session_, RemotePlayer, &remote_addr) < 0) {
        std::cerr << "Failed to add remote player\n";
        return false;
    }

    // Set local player delay
    gekko_set_local_delay(session_, local_player_handle_, config.input_delay);

    // Start the session
    gekko_start(session_, &gekko_config);

    return true;
}

void NetworkSession::Stop() {
    if (!session_) return;
    
    // Stop threads
    SDL_SetAtomicInt(&running_, 0);
    
    if (rollback_thread_) {
        SDL_WaitThread(rollback_thread_, nullptr);
        rollback_thread_ = nullptr;
    }
    
    if (network_thread_) {
        SDL_WaitThread(network_thread_, nullptr);
        network_thread_ = nullptr;
    }
    
    // Cleanup GekkoNet
    gekko_destroy(session_);
    session_ = nullptr;
}

void NetworkSession::Update() {
    if (!session_) return;
    
    // Process any pending events
    int event_count;
    GekkoGameEvent** events = gekko_update_session(session_, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        HandleGameEvent(events[i]);
    }
}

void NetworkSession::AddLocalInput(uint32_t input) {
    if (!session_) return;
    
    // Add input to GekkoNet
    gekko_add_local_input(session_, local_player_handle_, &input);
}

bool NetworkSession::SaveGameState(int frame) {
    if (!game_instance_) return false;

    // Allocate buffer for state
    std::vector<uint8_t> state_buffer(sizeof(FM2K::GameState));
    
    // Save state to buffer
    if (!game_instance_->SaveState(state_buffer.data(), state_buffer.size())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to save game state");
        return false;
    }

    // Store state in history
    saved_states_[frame] = std::move(state_buffer);
    return true;
}

bool NetworkSession::LoadGameState(int frame) {
    if (!game_instance_) return false;

    // Find state in history
    auto it = saved_states_.find(frame);
    if (it == saved_states_.end()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "No saved state found for frame %d", frame);
        return false;
    }

    // Load state from buffer
    if (!game_instance_->LoadState(it->second.data(), it->second.size())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to load game state");
        return false;
    }

    return true;
}

void NetworkSession::HandleGameEvent(GekkoGameEvent* event) {
    if (!game_instance_ || !event) return;

    switch (event->type) {
        case AdvanceEvent: {
            // Extract inputs from event
            uint32_t p1_input = event->data.Advance.inputs[0];
            uint32_t p2_input = event->data.Advance.inputs[1];

            // Inject inputs into game
            game_instance_->InjectInputs(p1_input, p2_input);
            break;
        }

        case SaveEvent: {
            // Save state to buffer
            if (!game_instance_->SaveState(
                event->data.save.state,
                *event->data.save.state_len)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to save state to network buffer");
            }
            break;
        }

        case LoadEvent: {
            // Load state from buffer
            if (!game_instance_->LoadState(
                event->data.load.state,
                event->data.load.state_len)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to load state from network buffer");
            }
            break;
        }

        default:
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Unknown game event type: %d", event->type);
            break;
    }
}

int NetworkSession::RollbackThreadFunction(void* data) {
    NetworkSession* session = static_cast<NetworkSession*>(data);
    
    while (SDL_GetAtomicInt(&session->running_)) {
        // Process rollbacks if needed
        if (SDL_GetAtomicInt(&session->rollback_flag_)) {
            session->ProcessRollback(SDL_GetAtomicInt(&session->last_confirmed_frame_));
            SDL_SetAtomicInt(&session->rollback_flag_, 0);
        }
        
        // Sleep for 1ms (we're running at 100 FPS)
        SDL_Delay(1);
    }
    
    return 0;
}

int NetworkSession::NetworkThreadFunction(void* data) {
    NetworkSession* session = static_cast<NetworkSession*>(data);
    
    while (SDL_GetAtomicInt(&session->running_)) {
        // Process network events
        session->Update();
        
        // Sleep for 1ms (we're running at 100 FPS)
        SDL_Delay(1);
    }
    
    return 0;
}

NetworkSession::NetworkStats NetworkSession::GetStats() const {
    NetworkStats stats;
    
    if (session_) {
        GekkoNetworkStats net_stats;
        gekko_network_stats(session_, 0, &net_stats);
        
        stats.ping = net_stats.last_ping;
        stats.jitter = net_stats.jitter;
        stats.frames_ahead = gekko_frames_ahead(session_);
        stats.connected = true;
    }
    
    return stats;
}

void NetworkSession::ProcessRollback(int target_frame) {
    // Load the last confirmed state
    if (!LoadGameState(target_frame)) {
        return;
    }
    
    // Re-simulate up to current frame
    int current_frame = SDL_GetAtomicInt(&frame_counter_);
    
    SDL_LockRWLockForWriting(input_buffer_lock_);
    for (int frame = target_frame + 1; frame <= current_frame; frame++) {
        // TODO: Re-apply inputs and update game state
        // This will need to use the FM2KGameInstance to:
        // 1. Apply confirmed remote inputs
        // 2. Re-simulate frame
        // 3. Save new state
    }
    SDL_UnlockRWLock(input_buffer_lock_);
}

bool NetworkSession::ShouldRollback(uint32_t remote_input, int frame_number) {
    // Get local input from the input history buffer at the specific frame
    uint32_t local_input;
    uint32_t input_addr = FM2K::P1_INPUT_HISTORY_ADDR + 
                         (local_player_handle_ * sizeof(uint32_t) * 1024) + 
                         ((frame_number & 0x3FF) * sizeof(uint32_t));
    
    if (game_instance_->ReadMemory(input_addr, &local_input)) {
        // Compare the local predicted input with the actual remote input
        return local_input != remote_input;
    }
    return false;
}

void NetworkSession::UpdatePredictionWindow() {
    // TODO: Implement dynamic prediction window adjustment based on:
    // 1. Network latency (ping)
    // 2. Jitter
    // 3. Rollback frequency
    // This helps balance input delay vs rollback frequency
}

void NetworkSession::ProcessEvents(FM2KGameInstance* game) {
    if (!IsActive() || !game) {
        return;
    }
    
    HandleGameEvents(game);
    HandleSessionEvents();
}

void NetworkSession::HandleGameEvents(FM2KGameInstance* game) {
    int event_count = 0;
    GekkoGameEvent** events = gekko_update_session(session_, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        GekkoGameEvent* ev = events[i];
        
        switch (ev->type) {
            case SaveEvent:
                {
                    // Save current FM2K state
                    if (ev->data.save.state && *ev->data.save.state_len >= sizeof(FM2K::GameState)) {
                        game->SaveState(ev->data.save.state, *ev->data.save.state_len);
                    }
                }
                break;
                
            case LoadEvent:
                {
                    // Load saved FM2K state
                    if (ev->data.load.state && 
                        ev->data.load.state_len >= sizeof(FM2K::GameState)) {
                        game->LoadState(ev->data.load.state, ev->data.load.state_len);
                    }
                }
                break;
                
            case AdvanceEvent:
                {
                    // Extract inputs from GekkoNet and inject into FM2K
                    if (ev->data.Advance.input_len >= 2 * sizeof(uint16_t)) {
                        const uint16_t* inputs = reinterpret_cast<const uint16_t*>(ev->data.Advance.inputs);
                        uint32_t p1_input = inputs[0];
                        uint32_t p2_input = inputs[1];
                        
                        // Inject inputs into FM2K
                        game->InjectInputs(p1_input, p2_input);
                    }
                }
                break;
                
            case EmptyGameEvent:
                // Ignore empty events
                break;
        }
    }
}

void NetworkSession::HandleSessionEvents() {
    int event_count = 0;
    GekkoSessionEvent** events = gekko_session_events(session_, &event_count);
    
    for (int i = 0; i < event_count; i++) {
        GekkoSessionEvent* ev = events[i];
        
        switch (ev->type) {
            case PlayerConnected:
                std::cout << "Connected to remote player (handle: " << ev->data.connected.handle << ")" << std::endl;
                cached_stats_.connected = true;
                break;
                
            case PlayerDisconnected:
                std::cout << "Disconnected from remote player (handle: " << ev->data.disconnected.handle << ")" << std::endl;
                cached_stats_.connected = false;
                break;
                
            case PlayerSyncing:
                std::cout << "Player syncing: " << ev->data.syncing.current << "/" << ev->data.syncing.max << std::endl;
                break;
                
            case SessionStarted:
                std::cout << "Session started" << std::endl;
                break;
                
            case SpectatorPaused:
                std::cout << "Spectator paused" << std::endl;
                break;
                
            case SpectatorUnpaused:
                std::cout << "Spectator unpaused" << std::endl;
                break;
                
            case DesyncDetected:
                std::cout << "Desync detected at frame " << ev->data.desynced.frame 
                         << " with player " << ev->data.desynced.remote_handle
                         << " (local=0x" << std::hex << ev->data.desynced.local_checksum
                         << " remote=0x" << ev->data.desynced.remote_checksum << std::dec
                         << ")" << std::endl;
                break;
                
            case EmptySessionEvent:
                // Ignore empty events
                break;
        }
    }
}

// End of implementation 