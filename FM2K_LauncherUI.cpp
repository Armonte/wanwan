#include "FM2K_Integration.h"
#include <iostream>
#include <algorithm>

// LauncherUI Implementation
LauncherUI::LauncherUI() 
    : launcher_state_(LauncherState::GameSelection)
{
}

LauncherUI::~LauncherUI() {
    Shutdown();
}

bool LauncherUI::Initialize(SDL_Window* window, SDL_Renderer* renderer) {
    if (!window || !renderer) {
        std::cerr << "Invalid SDL window or renderer" << std::endl;
        return false;
    }
    
    std::cout << "? LauncherUI initialized" << std::endl;
    return true;
}

void LauncherUI::Shutdown() {
    std::cout << "? LauncherUI shutdown" << std::endl;
}

void LauncherUI::NewFrame() {
    // This is handled by the main launcher
}

void LauncherUI::Render() {
    RenderMenuBar();
    
    switch (launcher_state_) {
        case LauncherState::GameSelection:
            RenderGameSelection();
            break;
            
        case LauncherState::Configuration:
            RenderNetworkConfig();
            break;
            
        case LauncherState::Connecting:
        case LauncherState::InGame:
            RenderConnectionStatus();
            RenderInGameUI();
            break;
            
        case LauncherState::Disconnected:
            RenderConnectionStatus();
            RenderNetworkConfig();
            break;
    }
}

void LauncherUI::RenderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Refresh Games")) {
                // Trigger game discovery refresh
                // This would be handled by the launcher
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                if (on_exit) on_exit();
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Network")) {
            if (ImGui::MenuItem("Disconnect", nullptr, false, network_stats_.connected)) {
                if (on_network_stop) on_network_stop();
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                // Show about dialog
            }
            ImGui::EndMenu();
        }
        
        // Show connection status in menu bar
        ImGui::SameLine(ImGui::GetWindowWidth() - 200);
        if (network_stats_.connected) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected");
            ImGui::SameLine();
            ImGui::Text("Ping: %.0fms", network_stats_.ping);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Disconnected");
        }
        
        ImGui::EndMainMenuBar();
    }
}

void LauncherUI::RenderGameSelection() {
    ImGui::Begin("Game Selection");
    
    // Show current directory
    ImGui::Text("Game Directory: %s", std::filesystem::absolute(".").string().c_str());
    
    // Game list
    if (ImGui::BeginListBox("##GameList", ImVec2(-1, 200))) {
        for (const auto& game : games_) {
            if (ImGui::Selectable(game.name.c_str())) {
                if (on_game_selected) {
                    on_game_selected(game);
                }
            }
            
            if (ImGui::IsItemHovered()) {
                ShowGameValidationStatus(game);
            }
        }
        ImGui::EndListBox();
    }
    
    ImGui::End();
}

void LauncherUI::RenderNetworkConfig() {
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Network Configuration", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Configure network settings for online play:");
        ImGui::Separator();
        
        // Local player selection
        ImGui::Text("Local Player:");
        ImGui::RadioButton("Player 1", &network_config_.local_player, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Player 2", &network_config_.local_player, 1);
        
        ImGui::Separator();
        
        // Network settings
        ImGui::Text("Network Settings:");
        
        // Local port
        ImGui::InputInt("Local Port", &network_config_.local_port);
        if (network_config_.local_port < 1024) network_config_.local_port = 1024;
        if (network_config_.local_port > 65535) network_config_.local_port = 65535;
        
        // Remote address
        char remote_addr_buffer[256];
        strncpy(remote_addr_buffer, network_config_.remote_address.c_str(), sizeof(remote_addr_buffer) - 1);
        remote_addr_buffer[sizeof(remote_addr_buffer) - 1] = '\0';
        
        if (ImGui::InputText("Remote Address", remote_addr_buffer, sizeof(remote_addr_buffer))) {
            network_config_.remote_address = remote_addr_buffer;
        }
        
        // Input delay
        ImGui::SliderInt("Input Delay (frames)", &network_config_.input_delay, 0, 10);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Higher values reduce rollbacks but increase input lag");
        }
        
        ImGui::Separator();
        
        // Spectator settings
        ImGui::Text("Spectator Settings:");
        ImGui::Checkbox("Enable Spectators", &network_config_.enable_spectators);
        
        if (network_config_.enable_spectators) {
            ImGui::SliderInt("Max Spectators", &network_config_.max_spectators, 1, 16);
        }
        
        ImGui::Separator();
        
        // Validation and start button
        bool config_valid = ValidateNetworkConfig();
        
        if (!config_valid) {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Invalid configuration");
        }
        
        if (ImGui::Button("Start Network Session", ImVec2(-1, 0))) {
            if (config_valid && on_network_start) {
                on_network_start(network_config_);
            }
        }
        
        if (launcher_state_ != LauncherState::GameSelection) {
            if (ImGui::Button("Back to Game Selection", ImVec2(-1, 0))) {
                // This would be handled by the launcher state management
            }
        }
    }
    ImGui::End();
}

void LauncherUI::RenderConnectionStatus() {
    ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Connection Status", nullptr, ImGuiWindowFlags_NoCollapse)) {
        if (network_stats_.connected) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "? Connected");
            
            ImGui::Separator();
            ImGui::Text("Network Statistics:");
            ImGui::Text("Ping: %.1f ms", network_stats_.ping);
            ImGui::Text("Jitter: %.1f ms", network_stats_.jitter);
            ImGui::Text("Frames Ahead: %.1f", network_stats_.frames_ahead);
            ImGui::Text("Rollbacks/sec: %d", network_stats_.rollbacks_per_second);
            
        } else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "? Connecting...");
            
            // Simple connecting animation
            static float progress = 0.0f;
            progress += ImGui::GetIO().DeltaTime;
            if (progress > 1.0f) progress = 0.0f;
            
            ImGui::ProgressBar(progress, ImVec2(-1, 0), "Establishing connection...");
        }
        
        ImGui::Separator();
        
        if (ImGui::Button("Disconnect")) {
            if (on_network_stop) on_network_stop();
        }
    }
    ImGui::End();
}

void LauncherUI::RenderInGameUI() {
    // Only show during active gameplay
    if (launcher_state_ != LauncherState::InGame) {
        return;
    }
    
    ShowNetworkDiagnostics();
}

void LauncherUI::ShowGameValidationStatus(const FM2KGameInfo& game) {
    if (game.validated) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Valid");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Invalid");
    }
}

void LauncherUI::ShowNetworkDiagnostics() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 350, 50), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 250), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Rollback Diagnostics", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Network Performance:");
        
        // Network quality indicator
        float quality = std::max(0.0f, std::min(1.0f, (100.0f - network_stats_.ping) / 100.0f));
        ImVec4 quality_color = ImVec4(1.0f - quality, quality, 0.0f, 1.0f);
        
        ImGui::Text("Connection Quality:");
        ImGui::SameLine();
        ImGui::TextColored(quality_color, "%.0f%%", quality * 100.0f);
        
        ImGui::Separator();
        
        // Detailed stats
        ImGui::Text("Ping: %.1f ms", network_stats_.ping);
        ImGui::Text("Jitter: %.1f ms", network_stats_.jitter);
        ImGui::Text("Frames Ahead: %.1f", network_stats_.frames_ahead);
        
        // Rollback information
        ImGui::Separator();
        ImGui::Text("Rollback Stats:");
        ImGui::Text("Rollbacks/sec: %d", network_stats_.rollbacks_per_second);
        
        // Frame timing visualization
        if (ImGui::CollapsingHeader("Frame Timeline")) {
            ImGui::Text("Last 60 frames:");
            
            // Simple frame timeline visualization
            for (int i = 0; i < 60; i++) {
                if (i > 0) ImGui::SameLine();
                
                // Mock rollback data - in real implementation this would track actual rollbacks
                bool was_rollback = (i % 17) == 0;  // Mock some rollbacks
                
                ImGui::PushStyleColor(ImGuiCol_Button, 
                                     was_rollback ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) 
                                                  : ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                ImGui::Button("##frame", ImVec2(4, 20));
                ImGui::PopStyleColor();
                
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Frame %d: %s", i, was_rollback ? "Rollback" : "Normal");
                }
            }
        }
    }
    ImGui::End();
}

bool LauncherUI::ValidateNetworkConfig() {
    // Check if remote address is valid format
    if (network_config_.remote_address.empty()) {
        return false;
    }
    
    // Check if port is in valid range
    if (network_config_.local_port < 1024 || network_config_.local_port > 65535) {
        return false;
    }
    
    // Basic IP:port format check
    size_t colon_pos = network_config_.remote_address.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }
    
    return true;
}

// Data binding methods
void LauncherUI::SetGames(const std::vector<FM2KGameInfo>& games) {
    games_ = games;
}

void LauncherUI::SetNetworkConfig(const NetworkConfig& config) {
    network_config_ = config;
}

void LauncherUI::SetNetworkStats(const NetworkSession::NetworkStats& stats) {
    network_stats_ = stats;
}

void LauncherUI::SetLauncherState(LauncherState state) {
    launcher_state_ = state;
} 