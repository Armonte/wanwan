#include "FM2K_Integration.h"
#include <iostream>
#include <algorithm>
#include "vendored/imgui/imgui.h" 
// LauncherUI Implementation
LauncherUI::LauncherUI() 
    : games_{}
    , network_config_{} // Zero-initialize network config
    , network_stats_{}  // Zero-initialize network stats
    , launcher_state_(LauncherState::GameSelection)
    , renderer_(nullptr)
    , window_(nullptr)
    , scanning_games_(false)
{
    // Initialize callbacks to nullptr
    on_game_selected = nullptr;
    on_network_start = nullptr;
    on_network_stop = nullptr;
    on_exit = nullptr;
}

LauncherUI::~LauncherUI() {
    Shutdown();
}

bool LauncherUI::Initialize(SDL_Window* window, SDL_Renderer* renderer) {
    if (!window || !renderer) {
        std::cerr << "Invalid SDL window or renderer" << std::endl;
        return false;
    }
    renderer_ = renderer;
    window_ = window;
    
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    
    // Enable features
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Setup scaling
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);        // Bake a fixed style scale
    style.FontScaleDpi = main_scale;        // Set initial font scale
    
    // Load font
    const float font_size = 16.0f * main_scale;
    const char* font_paths[] = {
        "vendored/imgui/misc/fonts/DroidSans.ttf",
        "C:/Windows/Fonts/segoeui.ttf"  // Fallback to Segoe UI on Windows
    };
    
    bool font_loaded = false;
    for (const char* font_path : font_paths) {
        if (SDL_GetPathInfo(font_path, nullptr)) {
            ImFont* font = io.Fonts->AddFontFromFileTTF(font_path, font_size);
            if (font != nullptr) {
                font_loaded = true;
                break;
            }
        }
    }
    
    if (!font_loaded) {
        io.Fonts->AddFontDefault();
    }
    
    // Setup Platform/Renderer backends
    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
        std::cerr << "Failed to initialize ImGui SDL3 backend" << std::endl;
        return false;
    }
    if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
        std::cerr << "Failed to initialize ImGui SDL3 Renderer backend" << std::endl;
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    
    return true;
}

void LauncherUI::Shutdown() {
    // Cleanup ImGui
    if (ImGui::GetCurrentContext()) {
        // Make sure we finish any pending viewport operations
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    
    std::cout << "LauncherUI shutdown" << std::endl;
}

void LauncherUI::NewFrame() {
    // Start the Dear ImGui frame
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void LauncherUI::Render() {
    if (!renderer_) {
        std::cerr << "Renderer is null in LauncherUI::Render!" << std::endl;
        return;
    }

    // Render UI elements
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

    // Render ImGui
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
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
            ImGui::Text("Ping: %ums", network_stats_.ping);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Disconnected");
        }
        
        ImGui::EndMainMenuBar();
    }
}

void LauncherUI::RenderGameSelection() {
    ImGui::Begin("Game Selection");
    
    // Show games root directory ("games" folder)
    const char* cwd_c = SDL_GetCurrentDirectory();
    std::string games_root = cwd_c ? cwd_c : "";
    if (cwd_c) SDL_free(const_cast<char*>(cwd_c));
    if (!games_root.empty() && games_root.back() != '/' && games_root.back() != '\\') games_root.push_back('/');
    games_root += "games";
    static char path_buf[512] = {0};
    static bool initialised_path = false;
    if (!initialised_path) {
        SDL_strlcpy(path_buf, games_root.c_str(), sizeof(path_buf));
        initialised_path = true;
    }

    ImGui::InputText("Games Folder", path_buf, sizeof(path_buf));
    ImGui::SameLine();
    if (ImGui::Button("Set##GamesFolder")) {
        if (on_games_folder_set) {
            on_games_folder_set(std::string(path_buf));
        }
    }
    
    // Game list ? size to remaining content height so window can be freely resized
    ImVec2 list_size = ImGui::GetContentRegionAvail();
    if (ImGui::BeginListBox("##GameList", list_size)) {
        if (games_.empty()) {
            if (scanning_games_) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Scanning for FM2K games...");
                static float prog = 0.0f;
                prog += ImGui::GetIO().DeltaTime * 0.5f; // Loop every 2s
                if (prog > 1.0f) prog = 0.0f;
                ImGui::ProgressBar(prog, ImVec2(-1, 0));
            } else {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "No FM2K games found!");
                ImGui::TextWrapped("Add your FM2K games (each with matching .exe and .kgt) inside sub-folders under the 'games' directory.");
            }
        } else {
            for (size_t idx = 0; idx < games_.size(); ++idx) {
                const auto& game = games_[idx];
                // Push a unique identifier to avoid ImGui ID conflicts even when display names repeat
                ImGui::PushID(static_cast<int>(idx));
                // Derive display name by stripping directory and extension
                std::string display_name = game.exe_path;
                // Remove directory
                size_t slash = display_name.find_last_of("/\\");
                if (slash != std::string::npos) display_name.erase(0, slash + 1);
                // Remove extension
                size_t dot = display_name.find_last_of('.');
                if (dot != std::string::npos) display_name.erase(dot);

                if (ImGui::Selectable(display_name.c_str())) {
                    if (on_game_selected) {
                        on_game_selected(game);
                    }
                }
                
                if (ImGui::IsItemHovered()) {
                    ShowGameValidationStatus(game);
                }

                ImGui::PopID(); // Pop per-item ID
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
            ImGui::Text("Ping: %u ms", network_stats_.ping);
            ImGui::Text("Jitter: %u ms", network_stats_.jitter);
            ImGui::Text("Frames Ahead: %u", network_stats_.frames_ahead);
            ImGui::Text("Rollbacks/sec: %u", network_stats_.rollbacks_per_second);
            
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

void LauncherUI::ShowGameValidationStatus(const FM2K::FM2KGameInfo& game) {
    if (game.is_host) {
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
        ImGui::Text("Ping: %u ms", network_stats_.ping);
        ImGui::Text("Jitter: %u ms", network_stats_.jitter);
        ImGui::Text("Frames Ahead: %u", network_stats_.frames_ahead);
        
        // Rollback information
        ImGui::Separator();
        ImGui::Text("Rollback Stats:");
        ImGui::Text("Rollbacks/sec: %u", network_stats_.rollbacks_per_second);
        
        // Frame timing visualization
        if (ImGui::CollapsingHeader("Frame Timeline")) {
            ImGui::Text("Last 60 frames:");
            
            // Simple frame timeline visualization
            for (int i = 0; i < 60; i++) {
                if (i > 0) ImGui::SameLine();

                // Mock rollback data ? replace with real tracking in production
                bool was_rollback = (i % 17) == 0;

                // Give each miniature button a unique ID to avoid conflicts
                ImGui::PushID(i);

                ImGui::PushStyleColor(ImGuiCol_Button,
                                     was_rollback ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                                                  : ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                ImGui::Button("##frame", ImVec2(4, 20));
                ImGui::PopStyleColor();

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Frame %d: %s", i, was_rollback ? "Rollback" : "Normal");
                }

                ImGui::PopID();
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
void LauncherUI::SetGames(const std::vector<FM2K::FM2KGameInfo>& games) {
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

void LauncherUI::SetScanning(bool scanning) {
    scanning_games_ = scanning;
} 