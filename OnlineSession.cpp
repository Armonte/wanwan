#include "OnlineSession.h"
#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include "FM2K_GekkoNetBridge.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <SDL3/SDL.h>

// OnlineSession Implementation
OnlineSession::OnlineSession() 
    : gekko_bridge_(std::make_unique<FM2K::GekkoNetBridge>())
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

OnlineSession::~OnlineSession() {
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

bool OnlineSession::Start(const NetworkConfig& config) {
    if (!gekko_bridge_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet bridge not initialized");
        return false;
    }
    
    // Convert NetworkConfig to FM2KNetworkConfig
    FM2K::FM2KNetworkConfig bridge_config{};
    bridge_config.session_mode = SessionMode::ONLINE;
    bridge_config.local_port = config.local_port;
    bridge_config.remote_address = config.remote_address;
    bridge_config.input_delay = config.input_delay;
    bridge_config.desync_detection = true;
    
    // Initialize the bridge for either a host or client session
    bool success = false;
    if (config.is_host) {
        success = gekko_bridge_->InitializeHostSession(bridge_config);
    } else {
        success = gekko_bridge_->InitializeClientSession(bridge_config);
    }

    if (!success) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize GekkoNet bridge for online session");
        return false;
    }
    
    // Game instance connection handled in SetGameInstance() - no need to connect here
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "OnlineSession started successfully");
    return true;
}

void OnlineSession::Stop() {
    if (!gekko_bridge_) return;
    
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
    
    // Shutdown GekkoNet bridge
    gekko_bridge_->Shutdown();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "NetworkSession stopped successfully");
}

void OnlineSession::Update() {
    if (!gekko_bridge_) return;
    
    // Update the bridge with frame timing
    static auto last_update = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float delta_time = std::chrono::duration<float>(now - last_update).count();
    last_update = now;
    
    // Let the bridge handle all GekkoNet events
    gekko_bridge_->Update(delta_time);
}

void OnlineSession::AddLocalInput(uint32_t input) {
    if (!gekko_bridge_) return;
    
    // Convert to FM2K input format and add to bridge
    FM2K::FM2KInput fm2k_input{};
    fm2k_input.input.value = static_cast<uint16_t>(input & 0xFFFF);
    gekko_bridge_->AddLocalInput(fm2k_input);
}

void OnlineSession::AddBothInputs(uint32_t p1_input, uint32_t p2_input) {
    // This method is for local sessions and should not be called in an OnlineSession.
    // We'll log an error if it's ever called.
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "AddBothInputs called on an OnlineSession, which is invalid.");
}

SessionMode OnlineSession::GetSessionMode() const {
    return SessionMode::ONLINE;  // Using the enum from FM2K_Integration.h
}

bool OnlineSession::SaveGameState(int frame) {
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

bool OnlineSession::LoadGameState(int frame) {
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

void OnlineSession::HandleGameEvent(GekkoGameEvent* event) {
    if (!game_instance_ || !event) return;

    switch (event->type) {
        case AdvanceEvent: {
            if (event->data.adv.inputs) {
                // GekkoNet provides a pointer to the input array for this frame
                const uint32_t* inputs = reinterpret_cast<const uint32_t*>(event->data.adv.inputs);
                uint32_t p1_input = inputs[0];
                uint32_t p2_input = inputs[1];
                game_instance_->InjectInputs(p1_input, p2_input);
            }
            break;
        }

        case SaveEvent: {
            auto& save = event->data.save;
            // Save state to buffer supplied by GekkoNet
            if (save.state && save.state_len) {
                if (!game_instance_->SaveState(save.state, *save.state_len)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to save state to network buffer");
                }
            }
            break;
        }

        case LoadEvent: {
            auto& load = event->data.load;
            // Load state from buffer
            if (load.state) {
                if (!game_instance_->LoadState(load.state, load.state_len)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load state from network buffer");
                }
            }
            break;
        }

        default:
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Unknown game event type: %d", event->type);
            break;
    }
}

int OnlineSession::RollbackThreadFunction(void* data) {
    OnlineSession* session = static_cast<OnlineSession*>(data);
    
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

int OnlineSession::NetworkThreadFunction(void* data) {
    OnlineSession* session = static_cast<OnlineSession*>(data);
    
    while (SDL_GetAtomicInt(&session->running_)) {
        // Process network events
        session->Update();
        
        // Sleep for 1ms (we're running at 100 FPS)
        SDL_Delay(1);
    }
    
    return 0;
}

bool OnlineSession::IsActive() const {
    return gekko_bridge_ && gekko_bridge_->IsConnected();
}

void OnlineSession::SetGameInstance(FM2KGameInstance* instance) {
    game_instance_ = instance;
    
    // Forward to GekkoNet bridge for state management
    if (gekko_bridge_) {
        gekko_bridge_->SetGameInstance(instance);
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
            "Game instance connected to NetworkSession and GekkoNet bridge");
    }
}

ISession::NetworkStats OnlineSession::GetStats() const {
    NetworkStats stats;
    
    if (gekko_bridge_ && gekko_bridge_->IsConnected()) {
        auto bridge_stats = gekko_bridge_->GetNetworkStats();
        
        stats.ping = bridge_stats.ping_ms;
        stats.jitter = bridge_stats.jitter_ms;
        stats.frames_ahead = static_cast<uint32_t>(bridge_stats.frames_ahead);
        stats.rollbacks_per_second = bridge_stats.rollback_count;
        stats.connected = true;
    }
    
    return stats;
}

void OnlineSession::ProcessRollback(int target_frame) {
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

bool OnlineSession::ShouldRollback(uint32_t remote_input, int frame_number) {
    // Rollback logic is now handled internally by GekkoNetBridge
    // This method is kept for compatibility but delegates to the bridge
    return false; // Bridge handles rollback decisions automatically
}

void OnlineSession::UpdatePredictionWindow() {
    // TODO: Implement dynamic prediction window adjustment based on:
    // 1. Network latency (ping)
    // 2. Jitter
    // 3. Rollback frequency
    // This helps balance input delay vs rollback frequency
}

void OnlineSession::ProcessEvents(FM2KGameInstance* game) {
    if (!IsActive() || !game) {
        return;
    }
    
    // Event processing is now handled internally by GekkoNetBridge::Update()
    // This method is kept for compatibility but no longer needed
}

void OnlineSession::HandleGameEvents(FM2KGameInstance* game) {
    // Game event handling is now done internally by GekkoNetBridge
    // This method is kept for compatibility but no longer used
    // The bridge handles SaveEvent, LoadEvent, and AdvanceEvent directly
}

void OnlineSession::HandleSessionEvents() {
    // Session event handling is now done internally by GekkoNetBridge
    // This method is kept for compatibility but no longer used
    // The bridge handles connection events, desyncs, etc. directly
}

// End of implementation 