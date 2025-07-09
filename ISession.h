#pragma once

#include <cstdint>
#include <memory>

// Forward declarations
class FM2KGameInstance;
struct NetworkConfig;

// Session mode enum to identify the type of session
enum class SessionMode {
    LOCAL,   // Both players local (offline testing)
    ONLINE   // One local + one remote player (network play)
};

// Abstract base class for all session types
class ISession {
public:
    virtual ~ISession() = default;

    // Network statistics structure (remains the same for all session types)
    struct NetworkStats {
        uint32_t ping;
        uint32_t jitter;
        uint32_t frames_ahead;
        uint32_t rollbacks_per_second;
        bool connected;
        
        NetworkStats()
            : ping(0)
            , jitter(0)
            , frames_ahead(0)
            , rollbacks_per_second(0)
            , connected(false)
        {}
    };

    // Pure virtual functions that all concrete session classes must implement
    virtual bool Start(const NetworkConfig& config) = 0;
    virtual void Stop() = 0;
    virtual void Update() = 0;
    virtual bool IsActive() const = 0;
    
    virtual void AddLocalInput(uint32_t input) = 0;
    virtual void AddBothInputs(uint32_t p1_input, uint32_t p2_input) = 0;
    virtual SessionMode GetSessionMode() const = 0;
    virtual NetworkStats GetStats() const = 0;
    
    virtual void SetGameInstance(FM2KGameInstance* instance) = 0;
}; 