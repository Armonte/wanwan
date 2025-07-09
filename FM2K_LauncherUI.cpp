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
    , selected_game_index_(-1)
    , current_theme_(UITheme::System)
{
    // Initialize callbacks to nullptr
    on_game_selected = nullptr;
    on_offline_session_start = nullptr;
    on_online_session_start = nullptr;
    on_session_stop = nullptr;
    on_exit = nullptr;
    on_games_folder_set = nullptr;
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
    // Set theme before rendering
    SetTheme(current_theme_);
    
    // Dockspace setup
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("MainDockspace", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // Render components into docked windows
    RenderMenuBar();

    // Left Panel: Game Selection and Controls
    if (ImGui::Begin("Games")) {
        RenderGameSelection();
        ImGui::Separator();
        RenderNetworkConfig();
        ImGui::Separator();
        RenderSessionControls();
    }
    ImGui::End();

    // Right Panel: Debug and Diagnostics
    if (ImGui::Begin("Debug Tools")) {
        RenderDebugTools();
    }
    ImGui::End();
    
    ImGui::End(); // End MainDockspace
}

void LauncherUI::RenderMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Select Games Folder...")) {
                // ... folder selection logic ...
            }
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                if (on_exit) on_exit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Session")) {
            if (launcher_state_ == LauncherState::InGame || launcher_state_ == LauncherState::Connecting) {
                if (ImGui::MenuItem("Disconnect")) {
                    if (on_session_stop) on_session_stop();
                }
            } else {
                ImGui::MenuItem("Disconnect", nullptr, false, false); // Disabled
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::BeginMenu("Theme")) {
                if (ImGui::MenuItem("Dark")) SetTheme(UITheme::Dark);
                if (ImGui::MenuItem("Light")) SetTheme(UITheme::Light);
                if (ImGui::MenuItem("System")) SetTheme(UITheme::System);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void LauncherUI::RenderGameSelection() {
    ImGui::Text("Games Folder");
    static char path_buf[512] = {0};
    if (games_root_path_.c_str() != path_buf) {
        SDL_strlcpy(path_buf, games_root_path_.c_str(), sizeof(path_buf));
    }
    ImGui::InputText("##GamesFolder", path_buf, sizeof(path_buf));
    ImGui::SameLine();
    if (ImGui::Button("Set")) {
        if (on_games_folder_set) {
            on_games_folder_set(path_buf);
        }
    }
    ImGui::Separator();

    ImGui::Text("Available FM2K Games");
    ImGui::Separator();

    ImGui::BeginChild("GameList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2)); // Reserve space for controls
    if (scanning_games_) {
        ImGui::Text("Scanning for games...");
    } else if (games_.empty()) {
        ImGui::Text("No games found in the specified directory.");
        ImGui::Text("Please select a valid games folder.");
    } else {
        for (size_t i = 0; i < games_.size(); ++i) {
            const auto& game = games_[i];
            bool is_selected = (static_cast<int>(i) == selected_game_index_);

            if (ImGui::Selectable(game.GetExeName().c_str(), is_selected)) {
                selected_game_index_ = static_cast<int>(i);
                if (on_game_selected) {
                    on_game_selected(game);
                }
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
    }
    ImGui::EndChild();
}

void LauncherUI::RenderNetworkConfig() {
    ImGui::Text("Network Configuration");
    ImGui::Separator();
    
    // Make network config always visible, removing state checks
    ImGui::InputText("Local Address", &network_config_.local_address[0], network_config_.local_address.capacity());
    ImGui::InputInt("Local Port", &network_config_.local_port);
    ImGui::InputText("Remote Address", &network_config_.remote_address[0], network_config_.remote_address.capacity());
    ImGui::InputInt("Local Player (0 or 1)", &network_config_.local_player);
    ImGui::SliderInt("Input Delay", &network_config_.input_delay, 0, 10);
    ImGui::Checkbox("Enable Spectators", &network_config_.enable_spectators);
    if (network_config_.enable_spectators) {
        ImGui::InputInt("Max Spectators", &network_config_.max_spectators);
    }
}

void LauncherUI::RenderConnectionStatus() {
    if (launcher_state_ == LauncherState::Connecting) {
        ImGui::OpenPopup("Connecting...");
    }

    if (ImGui::BeginPopupModal("Connecting...", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Establishing connection, please wait...");
        if (launcher_state_ != LauncherState::Connecting) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    if (launcher_state_ == LauncherState::Disconnected) {
        ImGui::OpenPopup("Disconnected");
    }

    if (ImGui::BeginPopupModal("Disconnected", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The network connection was lost.");
        if (ImGui::Button("OK")) {
            if (on_session_stop) on_session_stop();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void LauncherUI::RenderInGameUI() {
    // Only show during active gameplay
    if (launcher_state_ != LauncherState::InGame) {
        return;
    }
    
    // Network diagnostics are now shown in the debug tools panel
    // This function is no longer needed but kept for backwards compatibility
}

void LauncherUI::ShowGameValidationStatus(const FM2K::FM2KGameInfo& game) {
    ImGui::PushID(&game);
    if (game.is_host) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "  - Valid");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "  - Invalid");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("EXE: %s\nKGT: %s", game.exe_path.c_str(), game.dll_path.c_str());
    }
    ImGui::PopID();
}

void LauncherUI::ShowNetworkDiagnostics() {
    // Remove window creation - this is now rendered inline within the debug tools panel
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

void LauncherUI::RenderSessionControls() {
    ImGui::Text("Session Controls");
    ImGui::Separator();

    // "Start Offline" button is always enabled if a game is selected
    if (ImGui::Button("Start Offline Session", ImVec2(-1, 0))) {
        if (selected_game_index_ != -1 && on_offline_session_start) {
            on_offline_session_start();
        }
    }
    if (selected_game_index_ == -1) {
        ImGui::SameLine();
        ImGui::TextDisabled("(select a game)");
    }

    // "Start Online" button
    if (ImGui::Button("Start Online Session", ImVec2(-1, 0))) {
        if (selected_game_index_ != -1 && on_online_session_start) {
            // TODO: Add validation for network config
            on_online_session_start(network_config_);
        }
    }
    if (selected_game_index_ == -1) {
        ImGui::SameLine();
        ImGui::TextDisabled("(select a game)");
    }
    
    // "Stop Session" button
    if (launcher_state_ == LauncherState::InGame || launcher_state_ == LauncherState::Connecting) {
        if (ImGui::Button("Stop Session", ImVec2(-1, 0))) {
            if (on_session_stop) {
                on_session_stop();
            }
        }
    }
}

void LauncherUI::RenderDebugTools() {
    ImGui::Text("Debugging & Diagnostics");
    ImGui::Separator();

    if (launcher_state_ == LauncherState::InGame) {
        ShowNetworkDiagnostics();
    } else {
        ImGui::Text("No active session.");
    }
    
    // Future additions:
    // - Memory viewer
    // - State save/load buttons
    // - Rollback controls
} 