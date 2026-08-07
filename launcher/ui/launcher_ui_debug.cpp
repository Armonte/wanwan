// launcher_ui_debug.cpp -- LauncherUI debug/dev tools, multi-client + network test tools, console log + logging sink. Split from FM2K_LauncherUI.cpp (pure member-fn move).
#include "FM2K_Integration.h"
#include "launcher_ui_internal.h"  // shared persistence helpers (namespace lui)
#include "FM2K_HubClient.h"
#include "FM2K_PortMapper.h"  // UPnP port mapper member of LauncherUI (Phase 1)
#include "FM2K_DiscordAuth.h"
#include "FM2K_Locale.h"
#include "FM2K_Updater.h"
#include "version_local.h"
#include "auto_upload_secret.h"
#include "FM2K_UploadQueue.h"
#include "FM2KHook/src/ui/input_binder.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/util/pii_scrub.h"
#include "FM2K_GameIni.h"
#include "FM2K_DDrawRedirect.h"
#include "FM2K_CncDDraw.h"
#include "FM2K_Utf8Path.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <shellapi.h>  // Shell_NotifyIcon for challenge toast
#include <shobjidl.h>  // IFileOpenDialog (modern native folder picker)
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include "vendored/imgui/imgui.h"
#include "imgui_internal.h"
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>

using namespace lui;  // shared persistence helpers (launcher_ui_internal.h)

void LauncherUI::RenderDebugTools() {
    if (ImGui::BeginTabBar("DebugTabs", ImGuiTabBarFlags_None)) {

        if (ImGui::BeginTabItem("Multi-Client")) {
            RenderMultiClientTools();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Network")) {
            RenderNetworkTools();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Renderer")) {
            // Redirect is on-by-default. Toggle is presented as a
            // "Disable" knob -- when checked, the next launch skips the
            // IAT patch + PATH prepend and the game loads stock
            // KnownDlls ddraw.dll. Useful for diagnosing whether a
            // problem is rendering-related or game-code-related.
            bool disabled = !FM2K::ddraw_redirect::GetForceRedirect();
            if (ImGui::Checkbox("Disable cnc-ddraw renderer (debug)", &disabled)) {
                FM2K::ddraw_redirect::SetForceRedirect(!disabled);
            }
            ImGui::TextWrapped(
                "When unchecked (default): patches DDRAW.dll -> 2DFMD.dll in "
                "the game's IAT before resume and prepends the cnc-ddraw dir "
                "onto the child PATH. The cnc-ddraw dir is FM2K_DDRAW_DIR if "
                "set, otherwise <launcher>\\cnc-ddraw.");
            std::wstring resolved = FM2K::ddraw_redirect::ResolveCncDdrawDir();
            std::string resolved_utf8;
            if (!resolved.empty()) {
                int n = WideCharToMultiByte(CP_UTF8, 0, resolved.data(),
                                            (int)resolved.size(),
                                            nullptr, 0, nullptr, nullptr);
                if (n > 0) {
                    resolved_utf8.assign((size_t)n, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, resolved.data(),
                                        (int)resolved.size(),
                                        resolved_utf8.data(), n,
                                        nullptr, nullptr);
                }
            }
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Resolved cnc-ddraw dir: %s",
                resolved_utf8.empty() ? "(unresolved)" : resolved_utf8.c_str());

            ImGui::Separator();
            ImGui::Text("cnc-ddraw install");

            // Phase C: bundled cnc-ddraw downloader/updater. Status
            // pill mirrors FM2K_Updater's idiom -- labels pulled from
            // a single switch on the snapshot's State.
            const auto snap = fm2k::cnc_ddraw::Get();
            const char* state_label = "?";
            ImVec4 state_color(0.7f, 0.7f, 0.7f, 1.0f);
            switch (snap.state) {
                case fm2k::cnc_ddraw::State::Idle:
                    state_label = "Idle (not checked)"; break;
                case fm2k::cnc_ddraw::State::Checking:
                    state_label = "Checking GitHub...";
                    state_color = ImVec4(0.7f, 0.85f, 1.0f, 1.0f); break;
                case fm2k::cnc_ddraw::State::NotInstalled:
                    state_label = "Not installed";
                    state_color = ImVec4(1.0f, 0.7f, 0.4f, 1.0f); break;
                case fm2k::cnc_ddraw::State::UpToDate:
                    state_label = "Up to date";
                    state_color = ImVec4(0.5f, 0.9f, 0.5f, 1.0f); break;
                case fm2k::cnc_ddraw::State::UpdateAvailable:
                    state_label = "Update available";
                    state_color = ImVec4(1.0f, 0.85f, 0.4f, 1.0f); break;
                case fm2k::cnc_ddraw::State::Downloading: {
                    static char dl[64];
                    if (snap.total_bytes > 0) {
                        std::snprintf(dl, sizeof(dl),
                            "Downloading %u / %u KB",
                            snap.downloaded_bytes / 1024,
                            snap.total_bytes / 1024);
                    } else {
                        std::snprintf(dl, sizeof(dl),
                            "Downloading %u KB", snap.downloaded_bytes / 1024);
                    }
                    state_label = dl;
                    state_color = ImVec4(0.7f, 0.85f, 1.0f, 1.0f);
                    break;
                }
                case fm2k::cnc_ddraw::State::Extracting:
                    state_label = "Extracting...";
                    state_color = ImVec4(0.7f, 0.85f, 1.0f, 1.0f); break;
                case fm2k::cnc_ddraw::State::Ready:
                    state_label = "Installed";
                    state_color = ImVec4(0.5f, 0.9f, 0.5f, 1.0f); break;
                case fm2k::cnc_ddraw::State::Failed:
                    state_label = "Failed";
                    state_color = ImVec4(1.0f, 0.5f, 0.5f, 1.0f); break;
            }
            ImGui::TextColored(state_color, "Status: %s", state_label);
            if (!snap.local_version.empty() || !snap.remote_version.empty()) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "Local: %s   Remote: %s",
                    snap.local_version.empty()  ? "(none)" : snap.local_version.c_str(),
                    snap.remote_version.empty() ? "(?)"   : snap.remote_version.c_str());
            }
            if (snap.state == fm2k::cnc_ddraw::State::Failed && !snap.error_detail.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                    "Error: %s", snap.error_detail.c_str());
            }

            const bool busy = snap.state == fm2k::cnc_ddraw::State::Checking
                           || snap.state == fm2k::cnc_ddraw::State::Downloading
                           || snap.state == fm2k::cnc_ddraw::State::Extracting;
            if (busy) ImGui::BeginDisabled();
            if (ImGui::Button("Check & install")) {
                fm2k::cnc_ddraw::EnsureInstalled();
            }
            ImGui::SameLine();
            if (ImGui::Button("Force reinstall")) {
                fm2k::cnc_ddraw::ForceReinstall();
            }
            if (busy) ImGui::EndDisabled();

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Log")) {
            RenderConsoleLog();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void LauncherUI::RenderConsoleLog() {
    SDL_LockMutex(log_buffer_mutex_);

    if (ImGui::Button(T("btn_clear"))) {
        ClearLog();
    }

    ImGui::Separator();

    ImGui::BeginChild("LogScrollingRegion", ImVec2(0, -1), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::InputTextMultiline("##console", (char*)log_buffer_.c_str(), log_buffer_.size(),
        ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);

    if (scroll_to_bottom_) {
        ImGui::SetScrollHereY(1.0f);
        scroll_to_bottom_ = false;
    }

    ImGui::EndChild();
    SDL_UnlockMutex(log_buffer_mutex_);
}

void LauncherUI::RenderMultiClientTools() {
    ImGui::Text("%s", T("dev_local_multi"));
    ImGui::Separator();

    // Client Launch Controls
    if (ImGui::CollapsingHeader(T("panel_launch_control"), ImGuiTreeNodeFlags_DefaultOpen)) {
        // Get client status
        uint32_t client1_pid = 0, client2_pid = 0;
        bool status_available = false;
        if (on_get_client_status) {
            status_available = on_get_client_status(client1_pid, client2_pid);
        }
        
        // Selected game display
        if (!games_.empty() && selected_game_index_ >= 0 && selected_game_index_ < (int)games_.size()) {
            const auto& selected_game = games_[selected_game_index_];
            ImGui::Text(T("label_selected_game"), selected_game.GetExeName().c_str());
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), T("label_path"), selected_game.exe_path.c_str());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", T("warn_no_game_selected"));
        }
        
        ImGui::Separator();
        
        // Launch buttons
        bool can_launch = !games_.empty() && selected_game_index_ >= 0 && selected_game_index_ < (int)games_.size();
        bool clients_running = (client1_pid != 0 || client2_pid != 0);
        
        if (!can_launch) {
            ImGui::BeginDisabled();
        }
        
        // Dual client launch button
        if (ImGui::Button(T("dev_launch_dual_short"), ImVec2(200, 30))) {
            if (on_launch_local_client1 && on_launch_local_client2 && can_launch) {
                const auto& selected_game = games_[selected_game_index_];

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Launching dual clients for: %s", selected_game.exe_path.c_str());

                // Both clients launch from the same folder -- multi-instance
                // window check is patched and shared memory is PID-namespaced.
                bool success1 = on_launch_local_client1(selected_game.exe_path);
                if (success1) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 1 launched, starting client 2...");
                    bool success2 = on_launch_local_client2(selected_game.exe_path);
                    if (success2) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 2 (Guest) launched successfully");
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch Client 2 (Guest)");
                    }
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch Client 1 (Host)");
                }
            }
        }
        
        ImGui::SameLine();
        
        // Stop all clients button
        if (!clients_running) {
            ImGui::BeginDisabled();
        }
        
        if (ImGui::Button(T("btn_stop_all_clients"), ImVec2(150, 30))) {
            if (on_terminate_all_clients) {
                bool success = on_terminate_all_clients();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminate all clients: %s", success ? "success" : "failed");
            }
        }

        if (!clients_running) {
            ImGui::EndDisabled();
        }

        // "Launch Spectator" -- third local instance subscribing to client1
        // (host on 7000) for replay-streamed spectator validation. Only
        // enabled once Launch Dual Clients has the host alive.
        ImGui::SameLine();
        bool can_spectate2 = on_launch_local_spectator && can_launch && client1_pid != 0;
        if (!can_spectate2) ImGui::BeginDisabled();
        if (ImGui::Button(T("btn_launch_spectator_short"), ImVec2(160, 30))) {
            if (can_spectate2) {
                const auto& selected_game = games_[selected_game_index_];
                bool ok = on_launch_local_spectator(selected_game.exe_path);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Launch spectator: %s", ok ? "success" : "failed");
            }
        }
        if (!can_spectate2) ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", T("btn_launch_spectator_short_tip"));

        if (!can_launch) {
            ImGui::EndDisabled();
        }

        ImGui::Separator();

        // Client status display
        ImGui::Text("%s", T("label_client_status"));

        if (status_available) {
            // Client 1 status
            if (client1_pid != 0) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", T("client1_online_host"));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), T("label_pid"), client1_pid);
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", T("client1_offline"));
            }
            
            // Client 2 status
            if (client2_pid != 0) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", T("client2_online_guest"));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), T("label_pid"), client2_pid);
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", T("client2_offline"));
            }

            // Connection status
            if (client1_pid != 0 && client2_pid != 0) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", T("network_status_connected"));
            } else if (client1_pid != 0 || client2_pid != 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", T("network_status_waiting"));
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", T("network_status_no_clients"));
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", T("client_status_unavailable"));
        }
    }

    ImGui::Separator();

    // Client Debug Logs
    if (ImGui::CollapsingHeader(T("debug_log_panel"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%s", T("debug_log_realtime"));
        ImGui::Separator();

        // Client 1 Log Section
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", T("client1_log_host"));
        ImGui::SameLine();
        if (ImGui::Button(T("debug_log_copy_c1"))) {
            // Read Client 1 log file and copy to clipboard
            std::ifstream log_file("FM2K_P1_Debug.log");
            if (log_file.is_open()) {
                std::stringstream buffer;
                buffer << log_file.rdbuf();
                log_file.close();
                
                std::string log_content = buffer.str();
                if (!log_content.empty()) {
                    SDL_SetClipboardText(log_content.c_str());
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 1 log copied to clipboard (%zu characters)", log_content.length());
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Client 1 log file is empty");
                }
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Client 1 log file not found");
            }
        }
        
        // Display last few lines of Client 1 log
        {
            std::ifstream log_file("FM2K_P1_Debug.log");
            if (log_file.is_open()) {
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(log_file, line)) {
                    lines.push_back(line);
                }
                log_file.close();
                
                // Show last 10 lines
                size_t start_idx = lines.size() > 10 ? lines.size() - 10 : 0;
                
                ImGui::BeginChild("Client1Log", ImVec2(0, 120), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (size_t i = start_idx; i < lines.size(); ++i) {
                    // Color-code different log levels
                    if (lines[i].find("ERROR") != std::string::npos) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", lines[i].c_str());
                    } else if (lines[i].find("WARN") != std::string::npos) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", lines[i].c_str());
                    } else if (lines[i].find("INPUT") != std::string::npos) {
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.8f, 1.0f), "%s", lines[i].c_str());
                    } else {
                        ImGui::Text("%s", lines[i].c_str());
                    }
                }
                
                // Auto-scroll to bottom
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                    ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", T("debug_log_no_file"));
            }
        }
        
        ImGui::Separator();
        
        // Client 2 Log Section
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", T("client2_log_guest"));
        ImGui::SameLine();
        if (ImGui::Button(T("debug_log_copy_c2"))) {
            // Read Client 2 log file and copy to clipboard
            std::ifstream log_file("FM2K_P2_Debug.log");
            if (log_file.is_open()) {
                std::stringstream buffer;
                buffer << log_file.rdbuf();
                log_file.close();
                
                std::string log_content = buffer.str();
                if (!log_content.empty()) {
                    SDL_SetClipboardText(log_content.c_str());
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client 2 log copied to clipboard (%zu characters)", log_content.length());
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Client 2 log file is empty");
                }
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Client 2 log file not found");
            }
        }
        
        // Display last few lines of Client 2 log
        {
            std::ifstream log_file("FM2K_P2_Debug.log");
            if (log_file.is_open()) {
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(log_file, line)) {
                    lines.push_back(line);
                }
                log_file.close();
                
                // Show last 10 lines
                size_t start_idx = lines.size() > 10 ? lines.size() - 10 : 0;
                
                ImGui::BeginChild("Client2Log", ImVec2(0, 120), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (size_t i = start_idx; i < lines.size(); ++i) {
                    // Color-code different log levels
                    if (lines[i].find("ERROR") != std::string::npos) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", lines[i].c_str());
                    } else if (lines[i].find("WARN") != std::string::npos) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", lines[i].c_str());
                    } else if (lines[i].find("INPUT") != std::string::npos) {
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.8f, 1.0f), "%s", lines[i].c_str());
                    } else {
                        ImGui::Text("%s", lines[i].c_str());
                    }
                }
                
                // Auto-scroll to bottom
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                    ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", T("debug_log_no_file"));
            }
        }
        
        ImGui::Separator();
        
        // Log Management
        ImGui::Text("%s", T("debug_log_management"));
        if (ImGui::Button(T("debug_log_clear_all"))) {
            // Clear both log files
            std::ofstream("FM2K_P1_Debug.log", std::ios::trunc).close();
            std::ofstream("FM2K_P2_Debug.log", std::ios::trunc).close();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "All debug logs cleared");
        }

        ImGui::SameLine();
        if (ImGui::Button(T("debug_log_open_dir"))) {
            // Open the current directory in file explorer
            system("explorer .");
        }
        
    }
}

void LauncherUI::RenderNetworkTools() {
    RollbackStats stats = {};
    bool stats_available = false;

    if (on_get_rollback_stats) {
        stats_available = on_get_rollback_stats(stats);
    }

    if (!stats_available) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", T("status_no_active_session"));
        return;
    }

    ImGui::Text(T("label_frame"), stats.confirmed_frames);
    ImGui::Text(T("label_rollbacks"), stats.speculative_frames);
    ImGui::Text(T("label_frame_advantage"), stats.frame_advantage);

    if (stats.speculative_frames == 0) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", T("status_no_desyncs"));
    }
}

// RenderPerformanceStats removed - was just static info

// Custom log capture implementation
void LauncherUI::SDLCustomLogOutput(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    LauncherUI* ui = static_cast<LauncherUI*>(userdata);
    if (!ui) {
        return;
    }

    // Run the message through the PII scrubber so anything copied out
    // of the launcher's log window (or written to console / a future
    // disk log) is already redacted. The scrubbed buffer is what gets
    // both chained to the original SDL logger AND added to the in-UI
    // buffer -- so any attempt the user makes to share it (right-click
    // copy, "save log" button, screenshot) sees the redacted form.
    char scrubbed[2048];
    fm2k::pii::ScrubInto(message ? message : "",
                         scrubbed, sizeof(scrubbed));

    // Chain to the original logger to keep console output
    if (ui->original_log_function_) {
        ui->original_log_function_(ui->original_log_userdata_, category, priority, scrubbed);
    }

    // Add to our internal buffer for the UI
    ui->AddLog(scrubbed);
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

void LauncherUI::RenderObjectAnalysis() {}

void LauncherUI::RenderSlotInspectionWindow() {}
