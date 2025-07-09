#include "OnlineSession.h"
#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <SDL3/SDL.h>

// OnlineSession Implementation
OnlineSession::OnlineSession() 
    : game_instance_(nullptr)
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
    // DLL handles GekkoNet directly - no launcher-side session needed
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "OnlineSession: DLL handles GekkoNet directly");
    return true;
}

void OnlineSession::Stop() {
    // DLL handles GekkoNet directly - no launcher-side cleanup needed
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "OnlineSession: DLL handles GekkoNet directly");
}

void OnlineSession::Update() {
    // DLL handles GekkoNet directly - no launcher-side updates needed
}

void OnlineSession::AddLocalInput(uint32_t input) {
    // DLL handles input capture and GekkoNet directly
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
    // DLL handles GekkoNet directly - always return false for launcher
    return false;
}

void OnlineSession::SetGameInstance(FM2KGameInstance* instance) {
    game_instance_ = instance;
    // DLL handles GekkoNet directly - no bridge connection needed
}

ISession::NetworkStats OnlineSession::GetStats() const {
    NetworkStats stats;
    // DLL handles GekkoNet directly - no stats available from launcher
    stats.connected = false;
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
    // DLL handles rollback logic directly
    return false; // DLL handles rollback decisions automatically
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
    
    // DLL handles event processing directly
}

void OnlineSession::HandleGameEvents(FM2KGameInstance* game) {
    // DLL handles game events directly
}

void OnlineSession::HandleSessionEvents() {
    // DLL handles session events directly
}

// End of implementation 