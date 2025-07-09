#include "LocalSession.h"
#include <SDL3/SDL.h>

LocalSession::LocalSession()
    : game_instance_(nullptr) 
{}

LocalSession::~LocalSession() {
    Stop();
}

bool LocalSession::Start(const NetworkConfig& config) {
    (void)config; // Suppress unused parameter warning
    // DLL handles GekkoNet directly - no launcher-side session needed
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LocalSession: DLL handles GekkoNet directly");
    return true;
}

void LocalSession::Stop() {
    // DLL handles GekkoNet directly - no launcher-side cleanup needed
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LocalSession: DLL handles GekkoNet directly");
}

void LocalSession::Update() {
    // In local mode, the session is driven by input events, not a timed update loop.
    // This function can remain empty.
}

bool LocalSession::IsActive() const {
    // DLL handles GekkoNet directly - always return false for launcher
    return false;
}

void LocalSession::AddLocalInput(uint32_t input) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "AddLocalInput called on a LocalSession. Use AddBothInputs instead.");
}

void LocalSession::AddBothInputs(uint32_t p1_input, uint32_t p2_input) {
    // DLL handles input capture and GekkoNet directly
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
        "LocalSession: Input P1=0x%04X, P2=0x%04X handled by DLL", p1_input, p2_input);
}

SessionMode LocalSession::GetSessionMode() const {
    return SessionMode::LOCAL;  // Using the enum from FM2K_Integration.h
}



ISession::NetworkStats LocalSession::GetStats() const {
    NetworkStats stats;
    // DLL handles GekkoNet directly - no stats available from launcher
    stats.connected = false;
    return stats;
}

void LocalSession::SetGameInstance(FM2KGameInstance* instance) {
    game_instance_ = instance;
    // DLL handles GekkoNet directly - no bridge connection needed
}
