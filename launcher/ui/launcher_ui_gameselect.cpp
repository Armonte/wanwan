// launcher_ui_gameselect.cpp -- LauncherUI game picker + replay browser + direct-spectate. Split from FM2K_LauncherUI.cpp (pure move).
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

// "Spectate by IP" -- hub-less spec entry. Renders inline in the dev
// panel's Network tab. The hook's SPEC_JOIN_REQ → JOIN_ACK protocol
// works without any hub coordination; this just exposes the existing
// direct-spec CLI path through a UI. Cross-Patreon-tier scenarios:
// patron watching a non-patron friend's match, or two non-patrons
// spec'ing each other in dev mode. Both cases require the host to
// share their public addr out-of-band (Discord etc) since there's no
// hub matchmaking. Works for port-forwarded hosts + full-cone NATs;
// doesn't work across symmetric NAT (that needs Patreon-tier hub
// spec coordination).
void LauncherUI::RenderDirectSpecInline() {
    ImGui::TextWrapped(
        "Spectate by IP \xE2\x80\x94 hub-free spec for cross-tier viewing. "
        "Host must share their public addr out-of-band (e.g. Discord). "
        "Spawns a spec instance of whichever game is currently selected.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("Host addr (ip:port)##direct_spec_addr",
                             "1.2.3.4:7000",
                             direct_spec_addr_, sizeof(direct_spec_addr_));

    const bool can_connect = std::strchr(direct_spec_addr_, ':') != nullptr;
    ImGui::SameLine();
    if (!can_connect) ImGui::BeginDisabled();
    if (ImGui::Button("Spectate##direct_spec_go")) {
        std::string addr_str(direct_spec_addr_);
        const auto colon = addr_str.find_last_of(':');
        if (colon != std::string::npos && colon + 1 < addr_str.size()) {
            const std::string host_ip = addr_str.substr(0, colon);
            const int host_port = std::atoi(addr_str.c_str() + colon + 1);
            if (host_port > 0 && host_port <= 0xFFFF) {
                // Default session_kind="battle" -- same convention as
                // CLI --spectate. User is presumably joining a live
                // match; /F-boots straight to battle and applies host's
                // snapshot. on_spectate_match (on the launcher side)
                // validates a game is selected and warns if not.
                if (on_spectate_match) {
                    // Manual "spectate by IP" dev path -- we don't know
                    // the host's spec_transport here. Default to "tcp"
                    // so the launcher uses the legacy P2P path; the
                    // user can override by setting
                    // FM2K_SPEC_TRANSPORT=relay before launching.
                    on_spectate_match(host_ip, host_port, "battle", "tcp");
                }
            }
        }
    }
    if (!can_connect) ImGui::EndDisabled();
}

// The C11 replay browser (ScanReplays + RenderReplayBrowser) moved to the
// sibling launcher_ui_replaybrowser.cpp when it grew a real classifier,
// provenance badges and a per-row context menu. Same class, same callbacks.

void LauncherUI::RenderGameSelection() {
    // Games-folder list editor lives in Settings → Games Folders… The
    // main panel just shows the current root count + a button to open
    // the editor, so the games list itself dominates the panel.
    {
        // FlippySpatula's bug: when the column is narrow, the long
        // path string previously consumed the whole row and pushed
        // the Edit button off-screen with no way to recover. Render
        // the button FIRST so it's always reachable; the path text
        // wraps onto the next line(s) below if it doesn't fit.
        const size_t n = games_root_paths_.size();
        if (ImGui::SmallButton(T("btn_edit_games_folders"))) {
            show_games_folders_ = true;
        }
        ImGui::SameLine();
        if (n == 0) {
            ImGui::TextDisabled("%s", T("status_invalid_games_folder"));
        } else if (n == 1) {
            // TextWrapped instead of TextDisabled so long absolute
            // paths wrap into a second line instead of clipping at
            // the column edge. Keeps the disabled-color styling via
            // PushStyleColor since TextWrapped doesn't have a
            // "Disabled" variant.
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s: %s", T("panel_games_folder"),
                               games_root_paths_[0].c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("%s: %u", T("panel_games_folders"),
                                static_cast<unsigned>(n));
        }
    }

    ImGui::Separator();
    ImGui::Text("%s", T("panel_available_games"));
    ImGui::Separator();

    if (scanning_games_) {
        ImGui::Text("%s", T("status_scanning_for_games"));
    } else if (games_.empty()) {
        ImGui::Text("%s", T("status_no_games_found"));
        ImGui::Text("%s", T("status_invalid_games_folder"));
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

            // Compact two-tone row, engine tag on the LEFT:
            //   [2K] wanwan.exe       (tag dim gray, name normal)
            //   [95] CPW.exe          ditto for FM95
            //   [2K] AOB.exe          (both yellow when packer detected)
            // The Selectable owns the click + selection highlight; we overlay
            // the two-color text on top so tag and name carry independent
            // colors. ImGuiSelectableFlags_AllowItemOverlap lets the text
            // sit above the click region without blocking it.
            const bool packed = !game.packer_label.empty();
            const char* engine_tag = (game.engine == FM2K::Engine::FM95) ? "[95]" : "[2K]";

            ImVec2 cursor = ImGui::GetCursorScreenPos();
            const float row_h = ImGui::GetTextLineHeightWithSpacing();
            const float row_w = ImGui::GetContentRegionAvail().x;

            bool clicked = ImGui::Selectable("##row_sel", is_selected,
                                             ImGuiSelectableFlags_AllowItemOverlap,
                                             ImVec2(row_w, row_h));
            const bool hovered = ImGui::IsItemHovered();

            // Overlay the text on top of the (now invisible-labeled) Selectable.
            ImGui::SetCursorScreenPos(cursor);
            const ImVec4 yellow = ImVec4(0.92f, 0.78f, 0.30f, 1.0f);
            const ImVec4 dim    = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);

            ImGui::PushStyleColor(ImGuiCol_Text, packed ? yellow : dim);
            ImGui::TextUnformatted(engine_tag);
            ImGui::PopStyleColor();
            ImGui::SameLine();

            if (packed) ImGui::PushStyleColor(ImGuiCol_Text, yellow);
            ImGui::TextUnformatted(game.GetExeName().c_str());
            if (packed) ImGui::PopStyleColor();

            if (hovered) {
                if (packed) {
                    ImGui::SetTooltip("Packed with %s -- may not run with rollback hooks until unpacked.\n"
                                      "Hash: 0x%016llx",
                                      game.packer_label.c_str(),
                                      (unsigned long long)game.xxh64);
                } else if (!game.clean_label.empty()) {
                    ImGui::SetTooltip("%s\nHash: 0x%016llx (registered)",
                                      game.clean_label.c_str(),
                                      (unsigned long long)game.xxh64);
                } else {
                    ImGui::SetTooltip("Hash: 0x%016llx",
                                      (unsigned long long)game.xxh64);
                }
            }
            if (clicked) {
                selected_game_index_ = static_cast<int>(i);
                if (on_game_selected) {
                    on_game_selected(game);
                }
                // Route the input binder to this game's per-game profile
                // (creates fm2k_inputs_<basename>.ini lookup; reads
                // default if no override exists). Strip .exe suffix.
                // Construct path from wide so stem() preserves JP bytes.
                std::filesystem::path p(
                    fm2k::utf8path::Utf8ToWide(game.exe_path));
                std::string stem = fm2k::utf8path::StemUtf8(p);
                FM2KInputBinder::SetGameProfile(stem.c_str());
                if (input_binder_initialized_) {
                    FM2KInputBinder::Load();
                }
            }
            
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
            
            // Tooltips restored - font stack issue is fixed
            if (ImGui::IsItemHovered()) {
                if (game.kgt.valid) {
                    int p = 0, s = 0, d = 0;
                    for (const auto& n : game.kgt.player_names) if (!n.empty()) ++p;
                    for (const auto& n : game.kgt.stage_names)  if (!n.empty()) ++s;
                    for (const auto& n : game.kgt.demo_names)   if (!n.empty()) ++d;
                    ImGui::SetTooltip(
                        "EXE: %s\nKGT: %s\nProject: %s\n%d chars / %d stages / %d demos",
                        game.exe_path.c_str(), game.dll_path.c_str(),
                        game.kgt.project_name.empty() ? "(unnamed)" : game.kgt.project_name.c_str(),
                        p, s, d);
                } else {
                    ImGui::SetTooltip("EXE: %s\nKGT: %s", game.exe_path.c_str(), game.dll_path.c_str());
                }
            }
            
            ImGui::PopID();
        }
    }
}

