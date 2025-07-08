#include "FM2K_Integration.h"
#include <iostream>
#include <algorithm>
#include "vendored/imgui/imgui.h"
#include "imgui_internal.h"

// LauncherUI Implementation
LauncherUI::LauncherUI() 
    : games_{}
    , network_config_{} // Zero-initialize network config
    , network_stats_{}  // Zero-initialize network stats
    , launcher_state_(LauncherState::GameSelection)
    , renderer_(nullptr)
    , window_(nullptr)
    , scanning_games_(false)
    , games_root_path_("")
    , current_theme_(UITheme::System)
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
    
    // Setup Dear ImGui style based on theme
    SetTheme(current_theme_);
    
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
    
    // Use a dockspace to create a flexible layout
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();

    // For simplicity, we'll use a single layout, but this could be expanded
    // to allow user-customizable layouts.
    static bool first_time = true;
    if (first_time) {
        first_time = false;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

        ImGuiID dock_main_id = dockspace_id;
        ImGuiID dock_left_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.4f, nullptr, &dock_main_id);
        ImGuiID dock_right_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.6f, nullptr, &dock_main_id);
        
        ImGui::DockBuilderDockWindow("Game Selection", dock_left_id);
        ImGui::DockBuilderDockWindow("Network Configuration", dock_right_id);
        ImGui::DockBuilderDockWindow("Connection Status", dock_right_id);
        ImGui::DockBuilderDockWindow("Rollback Diagnostics", dock_right_id);
        ImGui::DockBuilderFinish(dockspace_id);
    }
    
    // Always render the game selection panel.
    RenderGameSelection();
    
    switch (launcher_state_) {
        case LauncherState::GameSelection:
            // In selection mode, the network config panel shows a placeholder.
            RenderNetworkConfig();
            break;
        case LauncherState::Configuration:
            // In config mode, the network panel is active.
            RenderNetworkConfig();
            break;
        case LauncherState::Connecting:
        case LauncherState::InGame:
            RenderConnectionStatus();
            RenderInGameUI();
            break;
        case LauncherState::Disconnected:
            RenderConnectionStatus();
            RenderNetworkConfig(); // Show config again on disconnect
            break;
    }

    // Render ImGui
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);

    // Update and Render additional Platform Windows
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
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
        
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Dark Theme", nullptr, current_theme_ == UITheme::Dark)) {
                SetTheme(UITheme::Dark);
            }
            if (ImGui::MenuItem("Light Theme", nullptr, current_theme_ == UITheme::Light)) {
                SetTheme(UITheme::Light);
            }
            if (ImGui::MenuItem("Use System Theme", nullptr, current_theme_ == UITheme::System)) {
                SetTheme(UITheme::System);
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
    
    // Show games root directory
    static char path_buf[512] = {0};
    static bool initialised_path = false;
    if (!initialised_path) {
        SDL_strlcpy(path_buf, games_root_path_.c_str(), sizeof(path_buf));
        initialised_path = true;
    }

    ImGui::InputText("Games Folder", path_buf, sizeof(path_buf));
    ImGui::SameLine();
    if (ImGui::Button("Set##GamesFolder")) {
        if (on_games_folder_set) {
            on_games_folder_set(path_buf);
            SDL_strlcpy(path_buf, games_root_path_.c_str(), sizeof(path_buf));
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
                const char* exe_path = game.exe_path.c_str();
                char display_name[256] = {0};
                SDL_strlcpy(display_name, exe_path, sizeof(display_name));
                
                // Remove directory
                char* last_slash = SDL_strrchr(display_name, '/');
                if (!last_slash) last_slash = SDL_strrchr(display_name, '\\');
                if (last_slash) {
                    size_t offset = last_slash - display_name + 1;
                    SDL_memmove(display_name, last_slash + 1, SDL_strlen(last_slash + 1) + 1);
                }
                
                // Remove extension
                char* last_dot = SDL_strrchr(display_name, '.');
                if (last_dot) *last_dot = '\0';

                if (ImGui::Selectable(display_name)) {
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
    // We now dock this window, so remove fixed positioning
    //ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    //ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Network Configuration", nullptr, ImGuiWindowFlags_NoCollapse)) {
        // Only show if a game has been selected
        if (launcher_state_ == LauncherState::GameSelection) {
            ImGui::TextWrapped("Select a game from the list to configure network settings.");
        } else {
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
            static char remote_addr_buffer[256];
            SDL_strlcpy(remote_addr_buffer, network_config_.remote_address.c_str(), sizeof(remote_addr_buffer));
            
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
    }
    ImGui::End();
}

void LauncherUI::RenderConnectionStatus() {
    // We now dock this window, so remove fixed positioning
    //ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
    //ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
    
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
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "c Connecting...");
            
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
    // We now dock this window, so remove fixed positioning
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

void LauncherUI::SetGamesRootPath(const std::string& path) {
    games_root_path_ = path;
}

// NOTE: This is the correctly scoped implementation for the SetTheme method
void LauncherUI::SetTheme(UITheme theme) {
    current_theme_ = theme;
    
    UITheme theme_to_apply = theme;
    if (theme_to_apply == UITheme::System) {
        // Detect system theme
        if (SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK) {
            theme_to_apply = UITheme::Dark;
        } else {
            theme_to_apply = UITheme::Light;
        }
    }
    
    if (theme_to_apply == UITheme::Dark) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
    }
} 