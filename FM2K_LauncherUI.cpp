#include "FM2K_Integration.h"
#include <iostream>
#include <algorithm>
#include "vendored/imgui/imgui.h"
#include "imgui_internal.h"
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include <chrono>
#include <ctime>

// LauncherUI Implementation
LauncherUI::LauncherUI() 
    : games_{}
    , network_config_{} // Zero-initialize network config
    , network_stats_{}  // Zero-initialize network stats
    , frames_ahead_(0.0f)
    , launcher_state_(LauncherState::GameSelection)
    , renderer_(nullptr)
    , window_(nullptr)
    , scanning_games_(false)
    , games_root_path_("")
    , selected_game_index_(-1)
    , current_theme_(UITheme::System)
    , scroll_to_bottom_(true)
    , original_log_function_(nullptr)
    , original_log_userdata_(nullptr)
{
    // Initialize callbacks to null
    on_game_selected = nullptr;
    on_offline_session_start = nullptr;
    on_online_session_start = nullptr;
    on_session_stop = nullptr;
    on_exit = nullptr;
    on_games_folder_set = nullptr;
    on_debug_save_state = nullptr;
    on_debug_load_state = nullptr;
    on_debug_force_rollback = nullptr;
    on_debug_save_to_slot = nullptr;
    on_debug_load_from_slot = nullptr;
    on_debug_auto_save_config = nullptr;
    on_get_slot_status = nullptr;
    on_get_auto_save_config = nullptr;
    on_set_save_profile = nullptr;

    log_buffer_mutex_ = SDL_CreateMutex();
}

LauncherUI::~LauncherUI() {
    if (log_buffer_mutex_) {
        SDL_DestroyMutex(log_buffer_mutex_);
        log_buffer_mutex_ = nullptr;
    }
    Shutdown();
}

bool LauncherUI::Initialize(SDL_Window* window, SDL_Renderer* renderer) {
    if (!window || !renderer) {
        std::cerr << "Invalid SDL window or renderer" << std::endl;
        return false;
    }
    renderer_ = renderer;
    window_ = window;
    
    // NUCLEAR: Exact copy of official SDL3 renderer example initialization
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Setup scaling - THIS IS CRITICAL FOR FONT STACK
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    
    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    
    // Setup our logging to capture SDL logs
    SDL_GetLogOutputFunction(&original_log_function_, &original_log_userdata_);
    SDL_SetLogOutputFunction(SDLCustomLogOutput, this);

    SDL_Log("Launcher UI Initialized");
    
    return true;
}

void LauncherUI::Shutdown() {
    // Restore original logger
    SDL_SetLogOutputFunction(original_log_function_, original_log_userdata_);

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
    // Render menu bar at application level first
    RenderMenuBar();
    
    // Create a dockspace to allow for flexible panel arrangement
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    window_flags |= ImGuiWindowFlags_NoBackground;
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DockSpace", nullptr, window_flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoCollapse;
    
    if (ImGui::Begin("Games & Configuration", nullptr, panel_flags)) {
        RenderGameSelection();
        ImGui::Separator();
        RenderNetworkConfig();
        ImGui::Separator();
        RenderSessionControls();
    }
    ImGui::End();
    
    if (ImGui::Begin("Debug & Diagnostics", nullptr, panel_flags)) {
        RenderDebugTools();
    }
    ImGui::End();
    
    ImGui::End(); // End DockSpace

    // Render connection status popups
    RenderConnectionStatus();
}

void LauncherUI::RenderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
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
            // Theme menu temporarily disabled to keep things simple initially
            if (ImGui::BeginMenu("Theme")) {
                if (ImGui::MenuItem("Dark")) SetTheme(UITheme::Dark);
                if (ImGui::MenuItem("Light")) SetTheme(UITheme::Light);
                if (ImGui::MenuItem("Dark Cyan")) SetTheme(UITheme::DarkCyan);
                if (ImGui::MenuItem("System")) SetTheme(UITheme::System);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void LauncherUI::RenderGameSelection() {
    ImGui::Text("Games Folder");
    static char path_buf[512] = {0};
    if (games_root_path_.c_str() != path_buf) {
        SDL_strlcpy(path_buf, games_root_path_.c_str(), sizeof(path_buf));
    }
    
    // Create a focus scope for the input group
    ImGui::PushID("GamesFolder");
    ImGui::InputText("##GamesFolder", path_buf, sizeof(path_buf));
    ImGui::SameLine();
    if (ImGui::Button("Set")) {
        if (on_games_folder_set) {
            on_games_folder_set(path_buf);
        }
    }
    ImGui::PopID();
    
    ImGui::Separator();
    ImGui::Text("Available FM2K Games");
    ImGui::Separator();

    if (scanning_games_) {
        ImGui::Text("Scanning for games...");
    } else if (games_.empty()) {
        ImGui::Text("No games found in the specified directory.");
        ImGui::Text("Please select a valid games folder.");
    } else {
        // Simple list without child window to avoid focus scope conflicts
        for (size_t i = 0; i < games_.size(); ++i) {
            const auto& game = games_[i];
            if (!game.is_host) {
                continue; // Skip invalid entries
            }
            
            bool is_selected = (static_cast<int>(i) == selected_game_index_);
            
            // Use PushID with integer to avoid string pointer issues
            ImGui::PushID(static_cast<int>(i));
            
            if (ImGui::Selectable(game.GetExeName().c_str(), is_selected)) {
                selected_game_index_ = static_cast<int>(i);
                if (on_game_selected) {
                    on_game_selected(game);
                }
            }
            
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
            
            // Tooltips restored - font stack issue is fixed
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("EXE: %s\nKGT: %s", game.exe_path.c_str(), game.dll_path.c_str());
            }
            
            ImGui::PopID();
        }
    }
}

void LauncherUI::RenderNetworkConfig() {
    if (ImGui::CollapsingHeader("Network Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        
        // Session Type (Host/Join)
        static int session_type = 0; // 0: Host, 1: Join
        ImGui::RadioButton("Host", &session_type, 0); ImGui::SameLine();
        ImGui::RadioButton("Join", &session_type, 1);
        
        network_config_.is_host = (session_type == 0);

        // Port Configuration
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("Port", &network_config_.local_port, 0, 0, ImGuiInputTextFlags_CharsDecimal);

        if (network_config_.is_host) {
            // Host-specific UI
            char local_ip[64] = "127.0.0.1"; // In a real app, get this dynamically
            ImGui::InputText("Your IP", local_ip, sizeof(local_ip), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Copy")) {
                char address_with_port[128];
                snprintf(address_with_port, sizeof(address_with_port), "%s:%d", local_ip, network_config_.local_port);
                SDL_SetClipboardText(address_with_port);
            }
        } else {
            // Client-specific UI
            char remote_addr_buf[128];
            strncpy(remote_addr_buf, network_config_.remote_address.c_str(), sizeof(remote_addr_buf) - 1);
            remote_addr_buf[sizeof(remote_addr_buf) - 1] = '\0';
            
            if (ImGui::InputText("Host Address", remote_addr_buf, sizeof(remote_addr_buf))) {
                network_config_.remote_address = remote_addr_buf;
            }
        }

        // Input Delay
        ImGui::SetNextItemWidth(100);
        ImGui::SliderInt("Input Delay (frames)", &network_config_.input_delay, 0, 10);
        
        ImGui::Unindent();
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

// Note: ShowGameValidationStatus removed ? UI simplified
/* void LauncherUI::ShowGameValidationStatus(const FM2K::FM2KGameInfo& game) {} */

void LauncherUI::ShowNetworkDiagnostics() {
    // Remove window creation - this is now rendered inline within the debug tools panel
    ImGui::Text("Network Performance:");
    
    // Network quality indicator based on average ping
    float quality = std::max(0.0f, std::min(1.0f, (100.0f - network_stats_.avg_ping) / 100.0f));
    ImVec4 quality_color = ImVec4(1.0f - quality, quality, 0.0f, 1.0f);
    
    ImGui::Text("Connection Quality:");
    ImGui::SameLine();
    ImGui::TextColored(quality_color, "%.0f%%", quality * 100.0f);
    
    ImGui::Separator();
    
    // Detailed stats from GekkoNetworkStats
    ImGui::Text("Avg Ping: %.2f ms", network_stats_.avg_ping);
    ImGui::Spacing();
    ImGui::Text("Last Ping: %u ms", network_stats_.last_ping);
    ImGui::Spacing();
    ImGui::Text("Jitter: %.2f ms", network_stats_.jitter);
    ImGui::Spacing();
    ImGui::Text("Frames Ahead: %.2f", frames_ahead_);
    
    // Rollback information
    ImGui::Separator();
    ImGui::Text("Rollback Stats:");
    
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

            // Tooltip restored - font stack issue is fixed
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

void LauncherUI::SetNetworkStats(const GekkoNetworkStats& stats) {
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

void LauncherUI::SetFramesAhead(float frames_ahead) {
    frames_ahead_ = frames_ahead;
}

// NOTE: This is the correctly scoped implementation for the SetTheme method
void LauncherUI::SetTheme(UITheme theme) {
    if (current_theme_ == theme && theme != UITheme::System) {
        return; // No change needed
    }
    
    current_theme_ = theme;
    
    UITheme theme_to_apply = theme;
    if (theme_to_apply == UITheme::System) {
        if (SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK) {
            theme_to_apply = UITheme::Dark;
        } else {
            theme_to_apply = UITheme::Light;
        }
    }
    
    switch (theme_to_apply) {
        case UITheme::Dark:
            ImGui::StyleColorsDark();
            break;
        case UITheme::Light:
            ImGui::StyleColorsLight();
            break;
        case UITheme::DarkCyan:
            ApplyDarkCyanThemeStyle();
            break;
        default:
            ImGui::StyleColorsDark(); // Default fallback
            break;
    }
}

void LauncherUI::ApplyDarkCyanThemeStyle()
{
	// Comfortable Dark Cyan style by SouthCraftX from ImThemes
	ImGuiStyle& style = ImGui::GetStyle();
	
	style.Alpha = 1.0f;
	style.DisabledAlpha = 1.0f;
	style.WindowPadding = ImVec2(20.0f, 20.0f);
	style.WindowRounding = 11.5f;
	style.WindowBorderSize = 0.0f;
	style.WindowMinSize = ImVec2(20.0f, 20.0f);
	style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
	style.WindowMenuButtonPosition = ImGuiDir_None;
	style.ChildRounding = 20.0f;
	style.ChildBorderSize = 1.0f;
	style.PopupRounding = 17.39999961853027f;
	style.PopupBorderSize = 1.0f;
	style.FramePadding = ImVec2(20.0f, 3.400000095367432f);
	style.FrameRounding = 11.89999961853027f;
	style.FrameBorderSize = 0.0f;
	style.ItemSpacing = ImVec2(8.899999618530273f, 13.39999961853027f);
	style.ItemInnerSpacing = ImVec2(7.099999904632568f, 1.799999952316284f);
	style.CellPadding = ImVec2(12.10000038146973f, 9.199999809265137f);
	style.IndentSpacing = 0.0f;
	style.ColumnsMinSpacing = 8.699999809265137f;
	style.ScrollbarSize = 11.60000038146973f;
	style.ScrollbarRounding = 15.89999961853027f;
	style.GrabMinSize = 3.700000047683716f;
	style.GrabRounding = 20.0f;
	style.TabRounding = 9.800000190734863f;
	style.TabBorderSize = 0.0f;
	style.TabCloseButtonMinWidthUnselected = 0.0f;
	style.ColorButtonPosition = ImGuiDir_Right;
	style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
	style.SelectableTextAlign = ImVec2(0.0f, 0.0f);
	
	style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.2745098173618317f, 0.3176470696926117f, 0.4509803950786591f, 1.0f);
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0784313753247261f, 0.08627451211214066f, 0.1019607856869698f, 1.0f);
	style.Colors[ImGuiCol_ChildBg] = ImVec4(0.09411764889955521f, 0.1019607856869698f, 0.1176470592617989f, 1.0f);
	style.Colors[ImGuiCol_PopupBg] = ImVec4(0.0784313753247261f, 0.08627451211214066f, 0.1019607856869698f, 1.0f);
	style.Colors[ImGuiCol_Border] = ImVec4(0.1568627506494522f, 0.168627455830574f, 0.1921568661928177f, 1.0f);
	style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0784313753247261f, 0.08627451211214066f, 0.1019607856869698f, 1.0f);
	style.Colors[ImGuiCol_FrameBg] = ImVec4(0.1137254908680916f, 0.125490203499794f, 0.1529411822557449f, 1.0f);
	style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.1568627506494522f, 0.168627455830574f, 0.1921568661928177f, 1.0f);
	style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.1568627506494522f, 0.168627455830574f, 0.1921568661928177f, 1.0f);
	style.Colors[ImGuiCol_TitleBg] = ImVec4(0.0470588244497776f, 0.05490196123719215f, 0.07058823853731155f, 1.0f);
	style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.0470588244497776f, 0.05490196123719215f, 0.07058823853731155f, 1.0f);
	style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0784313753247261f, 0.08627451211214066f, 0.1019607856869698f, 1.0f);
	style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.09803921729326248f, 0.105882354080677f, 0.1215686276555061f, 1.0f);
	style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0470588244497776f, 0.05490196123719215f, 0.07058823853731155f, 1.0f);
	style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.1176470592617989f, 0.1333333402872086f, 0.1490196138620377f, 1.0f);
	style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.1568627506494522f, 0.168627455830574f, 0.1921568661928177f, 1.0f);
	style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.1176470592617989f, 0.1333333402872086f, 0.1490196138620377f, 1.0f);
	style.Colors[ImGuiCol_CheckMark] = ImVec4(0.0313725508749485f, 0.9490196108818054f, 0.843137264251709f, 1.0f);
	style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.0313725508749485f, 0.9490196108818054f, 0.843137264251709f, 1.0f);
	style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.6000000238418579f, 0.9647058844566345f, 0.0313725508749485f, 1.0f);
	style.Colors[ImGuiCol_Button] = ImVec4(0.1176470592617989f, 0.1333333402872086f, 0.1490196138620377f, 1.0f);
	style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.1803921610116959f, 0.1882352977991104f, 0.196078434586525f, 1.0f);
	style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.1529411822557449f, 0.1529411822557449f, 0.1529411822557449f, 1.0f);
	style.Colors[ImGuiCol_Header] = ImVec4(0.1411764770746231f, 0.1647058874368668f, 0.2078431397676468f, 1.0f);
	style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.105882354080677f, 0.105882354080677f, 0.105882354080677f, 1.0f);
	style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.0784313753247261f, 0.08627451211214066f, 0.1019607856869698f, 1.0f);
	style.Colors[ImGuiCol_Separator] = ImVec4(0.1294117718935013f, 0.1490196138620377f, 0.1921568661928177f, 1.0f);
	style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.1568627506494522f, 0.1843137294054031f, 0.250980406999588f, 1.0f);
	style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.1568627506494522f, 0.1843137294054031f, 0.250980406999588f, 1.0f);
	style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1450980454683304f, 1.0f);
	style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.0313725508749485f, 0.9490196108818054f, 0.843137264251709f, 1.0f);
	style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	style.Colors[ImGuiCol_Tab] = ImVec4(0.0784313753247261f, 0.08627451211214066f, 0.1019607856869698f, 1.0f);
	style.Colors[ImGuiCol_TabHovered] = ImVec4(0.1176470592617989f, 0.1333333402872086f, 0.1490196138620377f, 1.0f);
	style.Colors[ImGuiCol_TabActive] = ImVec4(0.1176470592617989f, 0.1333333402872086f, 0.1490196138620377f, 1.0f);
	style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.0784313753247261f, 0.08627451211214066f, 0.1019607856869698f, 1.0f);
	style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.125490203499794f, 0.2745098173618317f, 0.572549045085907f, 1.0f);
	style.Colors[ImGuiCol_PlotLines] = ImVec4(0.5215686559677124f, 0.6000000238418579f, 0.7019608020782471f, 1.0f);
	style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.03921568766236305f, 0.9803921580314636f, 0.9803921580314636f, 1.0f);
	style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.0313725508749485f, 0.9490196108818054f, 0.843137264251709f, 1.0f);
	style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.1568627506494522f, 0.1843137294054031f, 0.250980406999588f, 1.0f);
	style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.0470588244497776f, 0.05490196123719215f, 0.07058823853731155f, 1.0f);
	style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.0470588244497776f, 0.05490196123719215f, 0.07058823853731155f, 1.0f);
	style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
	style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.1176470592617989f, 0.1333333402872086f, 0.1490196138620377f, 1.0f);
	style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.09803921729326248f, 0.105882354080677f, 0.1215686276555061f, 1.0f);
	style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.9372549057006836f, 0.9372549057006836f, 0.9372549057006836f, 1.0f);
	style.Colors[ImGuiCol_DragDropTarget] = ImVec4(0.4980392158031464f, 0.5137255191802979f, 1.0f, 1.0f);
	style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.2666666805744171f, 0.2901960909366608f, 1.0f, 1.0f);
	style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.4980392158031464f, 0.5137255191802979f, 1.0f, 1.0f);
	style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.196078434586525f, 0.1764705926179886f, 0.5450980663299561f, 0.501960813999176f);
	style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.196078434586525f, 0.1764705926179886f, 0.5450980663299561f, 0.501960813999176f);
}

void LauncherUI::RenderSessionControls() {
    if (ImGui::CollapsingHeader("Session Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();

        // Disable buttons if no game is selected
        bool game_selected = selected_game_index_ >= 0 && selected_game_index_ < games_.size();
        if (!game_selected) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Start Offline Session", ImVec2(-1, 0))) {
            if (on_offline_session_start) {
                on_offline_session_start();
            }
        }
        ImGui::SetItemTooltip("Launch the selected game for local offline play");

        if (ImGui::Button("Start Online Session", ImVec2(-1, 0))) {
            if (on_online_session_start) {
                on_online_session_start(network_config_);
            }
        }
        ImGui::SetItemTooltip("Launch an online session using the configuration below");

        if (ImGui::Button("Stop Session", ImVec2(-1, 0))) {
            if (on_session_stop) {
                on_session_stop();
            }
        }
        ImGui::SetItemTooltip("Terminate the currently running game session");

        if (!game_selected) {
            ImGui::EndDisabled();
        }
        
        ImGui::Unindent();
    }
}

void LauncherUI::RenderDebugTools() {
    ImGui::Text("Rollback & State Management");
    ImGui::Separator();
    
    // Performance Statistics
    if (ImGui::CollapsingHeader("Performance Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (on_get_slot_status) {
            // Get performance data from slot 0 (auto-save)
            SlotStatusInfo dummy_status;
            on_get_slot_status(0, dummy_status); // Just to trigger data sync
            
            // Show overall statistics (would need additional callback for perf stats)
            ImGui::Text("State Analysis:");
            ImGui::BulletText("Current size per save: ~850 KB");
            ImGui::BulletText("Player Data: 459 KB (54%%)");
            ImGui::BulletText("Object Pool: 391 KB (46%%)");
            ImGui::BulletText("Core State: ~8 KB (<1%%)");
            
            ImGui::Separator();
            ImGui::Text("Memory Usage:");
            ImGui::BulletText("8 save slots: ~6.8 MB total");
            ImGui::BulletText("Rollback buffer: ~850 KB");
            ImGui::BulletText("Total allocation: ~7.6 MB");
        } else {
            ImGui::TextDisabled("Performance data unavailable");
        }
    }
    
    ImGui::Separator();
    
    // Auto-save controls
    if (ImGui::CollapsingHeader("Auto-Save", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Get current auto-save settings from hook DLL
        bool auto_save_enabled = true;  // default fallback
        int auto_save_interval = 120;   // default fallback
        bool settings_available = false;
        
        if (on_get_auto_save_config) {
            AutoSaveConfig current_config;
            if (on_get_auto_save_config(current_config)) {
                auto_save_enabled = current_config.enabled;
                auto_save_interval = (int)current_config.interval_frames;
                settings_available = true;
            }
        }
        
        if (!settings_available) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "⚠ Auto-save settings unavailable");
        }
        
        bool enabled_changed = false;
        bool interval_changed = false;
        
        if (ImGui::Checkbox("Enable Auto-Save (Slot 0)", &auto_save_enabled)) {
            enabled_changed = true;
        }
        
        ImGui::SetNextItemWidth(150);
        if (ImGui::SliderInt("Interval (frames)", &auto_save_interval, 30, 600)) {
            interval_changed = true;
        }
        ImGui::SameLine();
        ImGui::Text("(%.1fs)", auto_save_interval / 100.0f);
        
        // Only send updates when something actually changed
        if ((enabled_changed || interval_changed) && on_debug_auto_save_config) {
            bool success = on_debug_auto_save_config(auto_save_enabled, auto_save_interval);
            if (success) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Auto-save config updated: %s, %d frames", 
                            auto_save_enabled ? "enabled" : "disabled", auto_save_interval);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to update auto-save config");
            }
        }
        
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "ℹ Auto-save uses Slot 0");
        if (auto_save_enabled && settings_available) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "✓ Auto-save active every %.1fs", auto_save_interval / 100.0f);
        } else if (!auto_save_enabled && settings_available) {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "○ Auto-save disabled");
        }
        
        // Save State Profile Selection
        ImGui::Separator();
        ImGui::Text("Save State Profile");
        
        static int profile_selection = 1;  // Default to STANDARD (index 1)
        const char* profile_items[] = { 
            "MINIMAL (~50KB)", 
            "STANDARD (~200KB)", 
            "COMPLETE (~850KB)" 
        };
        
        if (ImGui::Combo("Profile", &profile_selection, profile_items, IM_ARRAYSIZE(profile_items))) {
            // Update save profile in shared memory
            if (on_set_save_profile) {
                SaveStateProfile new_profile = static_cast<SaveStateProfile>(profile_selection);
                bool success = on_set_save_profile(new_profile);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Save profile changed to: %s (%s)", 
                           profile_items[profile_selection], success ? "success" : "failed");
            }
        }
        
        // Profile descriptions
        switch (profile_selection) {
            case 0: // MINIMAL
                ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "⚡ Fastest - Core state + active objects only");
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "   Good for: High-frequency auto-saves, rollback netcode");
                break;
            case 1: // STANDARD  
                ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "⚖ Balanced - Essential runtime state");
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "   Good for: Manual saves, most use cases");
                break;
            case 2: // COMPLETE
                ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "🔒 Complete - Everything for perfect restoration");
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "   Good for: Analysis, debugging, archival");
                break;
        }
    }
    
    ImGui::Separator();
    
    // Save Slots
    if (ImGui::CollapsingHeader("Save Slots", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Columns(4, "SaveSlots", true);
        ImGui::Text("Slot");
        ImGui::NextColumn();
        ImGui::Text("Status");
        ImGui::NextColumn();
        ImGui::Text("Save");
        ImGui::NextColumn();
        ImGui::Text("Load");
        ImGui::NextColumn();
        ImGui::Separator();
        
        for (int slot = 0; slot < 8; slot++) {
            ImGui::PushID(slot);
            
            // Slot number with clearer auto-save indication
            if (slot == 0) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%d (AUTO)", slot);
                ImGui::SetItemTooltip("Auto-save slot - automatically saves at configured intervals");
            } else {
                ImGui::Text("%d", slot);
                ImGui::SetItemTooltip("Manual save slot");
            }
            ImGui::NextColumn();
            
            // Status - get real status from hook DLL
            if (on_get_slot_status) {
                SlotStatusInfo status;
                if (on_get_slot_status(slot, status)) {
                    if (status.occupied) {
                        // Calculate time ago
                        uint64_t current_time = SDL_GetTicks();
                        uint64_t time_diff = current_time - status.timestamp_ms;
                        
                        if (time_diff < 1000) {
                            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "F%u (now)", status.frame_number);
                        } else if (time_diff < 60000) {
                            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "F%u (%.1fs ago)", status.frame_number, time_diff / 1000.0f);
                        } else {
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), "F%u (%llus ago)", status.frame_number, time_diff / 1000);
                        }
                        
                        // Enhanced tooltip with performance data
                        std::string tooltip = "Frame " + std::to_string(status.frame_number) +
                                            "\nChecksum: 0x" + std::to_string(status.checksum) +
                                            "\nSaved " + std::to_string(time_diff / 1000.0f) + " seconds ago";
                        
                        if (status.state_size_kb > 0) {
                            tooltip += "\nSize: " + std::to_string(status.state_size_kb) + " KB";
                        }
                        if (status.save_time_us > 0) {
                            tooltip += "\nSave time: " + std::to_string(status.save_time_us) + " μs";
                        }
                        if (status.load_time_us > 0) {
                            tooltip += "\nLast load: " + std::to_string(status.load_time_us) + " μs";
                        }
                        
                        ImGui::SetItemTooltip("%s", tooltip.c_str());
                    } else {
                        ImGui::TextDisabled("Empty");
                    }
                } else {
                    ImGui::TextDisabled("Error");
                }
            } else {
                ImGui::TextDisabled("Unknown");
            }
            ImGui::NextColumn();
            
            // Save button
            if (ImGui::Button("Save")) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "UI: Save button clicked for slot %d", slot);
                if (on_debug_save_to_slot) {
                    bool success = on_debug_save_to_slot(slot);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "UI: Save to slot %d %s", slot, success ? "triggered" : "failed");
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "UI: on_debug_save_to_slot callback is null!");
                }
            }
            ImGui::NextColumn();
            
            // Load button
            if (ImGui::Button("Load")) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "UI: Load button clicked for slot %d", slot);
                if (on_debug_load_from_slot) {
                    bool success = on_debug_load_from_slot(slot);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "UI: Load from slot %d %s", slot, success ? "triggered" : "failed");
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "UI: on_debug_load_from_slot callback is null!");
                }
            }
            ImGui::NextColumn();
            
            ImGui::PopID();
        }
        
        ImGui::Columns(1);
    }
    
    ImGui::Separator();
    
    // Quick save/load (legacy)
    ImGui::Text("Quick Actions");
    if (ImGui::Button("Quick Save")) {
        if (on_debug_save_state) {
            bool success = on_debug_save_state();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Quick save %s", success ? "triggered" : "failed");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Quick Load")) {
        if (on_debug_load_state) {
            bool success = on_debug_load_state();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Quick load %s", success ? "triggered" : "failed");
        }
    }

    static int rollback_frames = 3;  // Default to 3 frames
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("Force Rollback Frames", &rollback_frames);
    if (rollback_frames < 0) rollback_frames = 0;
    if (rollback_frames > 60) rollback_frames = 60;  // Reasonable limit
    
    ImGui::SameLine();
    if (ImGui::Button("Force")) {
        if (on_debug_force_rollback && rollback_frames > 0) {
            bool success = on_debug_force_rollback(static_cast<uint32_t>(rollback_frames));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Force rollback of %d frames %s", rollback_frames, success ? "triggered" : "failed");
        }
    }
    
    ImGui::Separator();
    
    ShowNetworkDiagnostics();

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Console Log", ImGuiTreeNodeFlags_DefaultOpen)) {
        SDL_LockMutex(log_buffer_mutex_);

        if (ImGui::Button("Clear")) {
            ClearLog();
        }
        
        ImGui::Separator();
        
        ImGui::BeginChild("LogScrollingRegion", ImVec2(0, 200), false, ImGuiWindowFlags_HorizontalScrollbar);
        
        // Use InputTextMultiline to make the log selectable.
        ImGui::InputTextMultiline("##console", (char*)log_buffer_.c_str(), log_buffer_.size(), ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
        
        if (scroll_to_bottom_) {
            ImGui::SetScrollHereY(1.0f);
            scroll_to_bottom_ = false;
        }
        
        ImGui::EndChild();

        SDL_UnlockMutex(log_buffer_mutex_);
    }
} 

// Custom log capture implementation
void LauncherUI::SDLCustomLogOutput(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    LauncherUI* ui = static_cast<LauncherUI*>(userdata);
    if (!ui) {
        return;
    }

    // Chain to the original logger to keep console output
    if (ui->original_log_function_) {
        ui->original_log_function_(ui->original_log_userdata_, category, priority, message);
    }
    
    // Add to our internal buffer for the UI
    ui->AddLog(message);
}

void LauncherUI::AddLog(const char* message) {
    if (!log_buffer_mutex_) {
        return;
    }

    SDL_LockMutex(log_buffer_mutex_);
    
    log_buffer_.appendf("%s\n", message);
    scroll_to_bottom_ = true;

    SDL_UnlockMutex(log_buffer_mutex_);
}

void LauncherUI::ClearLog() {
    if (!log_buffer_mutex_) {
        return;
    }

    SDL_LockMutex(log_buffer_mutex_);
    log_buffer_.clear();
    SDL_UnlockMutex(log_buffer_mutex_);
} 