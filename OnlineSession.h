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
    // Minimal state - DLL handles everything
    FM2KGameInstance* game_instance_;
    uint32_t frame_counter_;  // Simple frame counter like bsnes
}; 