#pragma once

#define GEKKONET_STATIC
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include "FM2KHook/src/state_manager.h"
#include "FM2KHook/src/ipc.h"
#include <SDL3/SDL.h>
#include <memory>

// Forward declaration
class FM2KGameInstance;

namespace FM2K {

// FM2K Input structure (matches game's 11-bit input mask)
struct FM2KInput {
    union {
        struct {
            uint16_t left     : 1;   // 0x001
            uint16_t right    : 1;   // 0x002
            uint16_t up       : 1;   // 0x004
            uint16_t down     : 1;   // 0x008
            uint16_t button1  : 1;   // 0x010
            uint16_t button2  : 1;   // 0x020
            uint16_t button3  : 1;   // 0x040
            uint16_t button4  : 1;   // 0x080
            uint16_t button5  : 1;   // 0x100
            uint16_t button6  : 1;   // 0x200
            uint16_t button7  : 1;   // 0x400
            uint16_t reserved : 5;   // Unused bits
        } bits;
        uint16_t value;
    } input;
};

// Configuration for FM2K rollback session
struct FM2KNetworkConfig {
    int local_player;           // 0 or 1
    int local_port;             // Network port for this player
    std::string remote_address; // IP address of remote player
    int input_delay;            // Local input delay frames
    int max_prediction_window;  // Maximum prediction window
    bool desync_detection;      // Enable checksum validation
};

// Network statistics
struct FM2KNetworkStats {
    int ping_ms;
    int avg_ping_ms;
    int jitter_ms;
    float frames_ahead;
    int rollback_count;
    int prediction_errors;
};

// Main GekkoNet integration class
class GekkoNetBridge {
public:
    GekkoNetBridge();
    ~GekkoNetBridge();
    
    // Session management
    bool Initialize(const FM2KNetworkConfig& config);
    void Shutdown();
    bool IsConnected() const;
    
    // Game loop integration
    void Update(float delta_time);
    void AddLocalInput(const FM2KInput& input);
    
    // Network stats
    FM2KNetworkStats GetNetworkStats() const;
    
    // Event callbacks (called by GekkoNet)
    void OnSaveState(GekkoGameEvent* event);
    void OnLoadState(GekkoGameEvent* event);
    void OnAdvanceFrame(GekkoGameEvent* event);
    
    // Game instance connection
    void SetGameInstance(FM2KGameInstance* game_instance);

private:
    // GekkoNet session
    GekkoSession* session_;
    int local_player_handle_;
    FM2KGameInstance* game_instance_;
    
    // Configuration
    FM2KNetworkConfig config_;
    
    // Game state buffer for GekkoNet
    std::unique_ptr<State::GameState> current_state_;
    
    // Timing for frame rate control
    float accumulator_;
    float target_frame_time_;
    
    // Internal methods
    void ProcessGekkoEvents();
    void ProcessGameUpdates();
    float GetFrameTime(float frames_ahead) const;
    
    // Helper functions
    bool InitializeGekkoSession();
    void ConfigureNetworking();
    void AddPlayers();
};

} // namespace FM2K