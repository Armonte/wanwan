#include "LocalSession.h"
#include "FM2K_GekkoNetBridge.h"
#include "FM2K_GameInstance.h"
#include <SDL3/SDL.h>

LocalSession::LocalSession()
    : gekko_bridge_(std::make_unique<FM2K::GekkoNetBridge>())
    , game_instance_(nullptr) 
{}

LocalSession::~LocalSession() {
    Stop();
}

bool LocalSession::Start(const NetworkConfig& config) {
    if (!gekko_bridge_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GekkoNet bridge not initialized");
        return false;
    }

    FM2K::FM2KNetworkConfig bridge_config{};
    bridge_config.session_mode = SessionMode::LOCAL;
    bridge_config.input_delay = config.input_delay;

    if (!gekko_bridge_->InitializeLocalSession(bridge_config)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize GekkoNet bridge for local session");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LocalSession started successfully");
    return true;
}

void LocalSession::Stop() {
    if (gekko_bridge_) {
        gekko_bridge_->Shutdown();
    }
}

void LocalSession::Update() {
    // In local mode, the session is driven by input events, not a timed update loop.
    // This function can remain empty.
}

bool LocalSession::IsActive() const {
    return gekko_bridge_ && gekko_bridge_->IsConnected();
}

void LocalSession::AddLocalInput(uint32_t input) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "AddLocalInput called on a LocalSession. Use AddBothInputs instead.");
}

void LocalSession::AddBothInputs(uint32_t p1_input, uint32_t p2_input) {
    if (!gekko_bridge_) return;
    
    FM2K::FM2KInput fm2k_p1_input{}, fm2k_p2_input{};
    fm2k_p1_input.input.value = static_cast<uint16_t>(p1_input & 0xFFFF);
    fm2k_p2_input.input.value = static_cast<uint16_t>(p2_input & 0xFFFF);
    gekko_bridge_->AddBothInputs(fm2k_p1_input, fm2k_p2_input);
}

SessionMode LocalSession::GetSessionMode() const {
    return SessionMode::LOCAL;  // Using the enum from FM2K_Integration.h
}

ISession::NetworkStats LocalSession::GetStats() const {
    // Local sessions don't have network stats like ping or jitter.
    // We can return a default-constructed struct.
    return ISession::NetworkStats();
}

void LocalSession::SetGameInstance(FM2KGameInstance* instance) {
    game_instance_ = instance;
    if (gekko_bridge_) {
        gekko_bridge_->SetGameInstance(instance);
    }
}
