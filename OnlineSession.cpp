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
    , frame_counter_(0)
{
    // Minimal initialization - DLL handles everything
}

OnlineSession::~OnlineSession() {
    Stop();
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
    // DLL handles GekkoNet directly - just increment frame counter
    frame_counter_++;
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


bool OnlineSession::IsActive() const {
    // DLL handles GekkoNet directly - always return false for launcher
    return false;
}

void OnlineSession::SetGameInstance(FM2KGameInstance* instance) {
    game_instance_ = instance;
    // DLL handles GekkoNet directly - no bridge connection needed
}

ISession::NetworkStats OnlineSession::GetStats() const {
    NetworkStats stats = {};
    // DLL handles everything - minimal stats
    stats.connected = false;  // DLL manages connection
    return stats;
}


// End of implementation 