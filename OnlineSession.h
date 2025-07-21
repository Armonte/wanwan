#pragma once

#include "ISession.h"
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <SDL3/SDL.h>

// Forward declarations
struct NetworkConfig;
namespace FM2K {
    
}

class OnlineSession : public ISession {
public:
    OnlineSession();
    ~OnlineSession();
    
    // ISession interface implementation
    bool Start(const NetworkConfig& config) override;
    void Stop() override;
    void Update() override;
    bool IsActive() const override;
    
    void AddLocalInput(uint32_t input) override;
    void AddBothInputs(uint32_t p1_input, uint32_t p2_input) override;
    SessionMode GetSessionMode() const override;
    NetworkStats GetStats() const override;
    
    void SetGameInstance(FM2KGameInstance* instance) override;

private:
    // GekkoNet bridge (replaces direct GekkoNet usage)
    // GekkoNetBridge removed - DLL handles GekkoNet directly
    FM2KGameInstance* game_instance_;
    
    // Synchronization
    SDL_Mutex* state_mutex_;
    SDL_RWLock* input_buffer_lock_;
    SDL_Thread* rollback_thread_;
    SDL_Thread* network_thread_;
    
    // State tracking
    SDL_AtomicInt frame_counter_;
    SDL_AtomicInt rollback_flag_;
    SDL_AtomicInt running_;
    SDL_AtomicInt last_confirmed_frame_;
    SDL_AtomicInt prediction_window_;
    NetworkStats cached_stats_;
    
    // State buffer for rollbacks
    static constexpr size_t STATE_BUFFER_SIZE = 128;
    std::vector<uint8_t> state_buffer_;
    std::unordered_map<int, std::vector<uint8_t>> saved_states_;  // Frame number -> State data
    
    // Thread functions
    static int RollbackThreadFunction(void* data);
    static int NetworkThreadFunction(void* data);
    
    // Internal methods
    void HandleSessionEvents();
    void HandleGameEvents(FM2KGameInstance* game);
    void HandleGameEvent(GekkoGameEvent* ev);
    void ProcessEvents(FM2KGameInstance* game);
    
    // Rollback management
    bool SaveGameState(int frame_number);
    bool LoadGameState(int frame_number);
    void ProcessRollback(int target_frame);
    
    // Frame management
    void UpdatePredictionWindow();
    bool ShouldRollback(uint32_t remote_input, int frame_number);
}; 