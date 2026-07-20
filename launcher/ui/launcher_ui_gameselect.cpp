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

// "Spectate by IP" — hub-less spec entry. Renders inline in the dev
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
                // Default session_kind="battle" — same convention as
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

// C11 — Replay browser. Walks configured games-root paths once
// (replays_cache_dirty_ → ScanReplays), then renders Session → Match tree.
// Click a row to dispatch via on_replay_play. Future iterations: filter
// chips (game/date/nick), right-click context menu (open file location,
// export round-as-standalone, copy share link), round-level seek.
void LauncherUI::ScanReplays() {
    replays_cache_.clear();
    replays_cache_dirty_ = false;

    // 256-byte FM2KSessionFileHeader (mirrors spectator_node.cpp's struct).
    // We read off the front of each file by offset rather than declaring
    // the struct here so the launcher and hook stay decoupled — a hook
    // schema bump would break the launcher's struct cast otherwise.
    constexpr uint32_t MAGIC_FMSS = 0x53534D46;  // 'FMSS' little-endian
    constexpr uint16_t VERSION_V2 = 2;
    constexpr size_t   HEADER_SIZE = 256;

    std::error_code ec;
    auto try_load_file = [&](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return;
        uint8_t buf[HEADER_SIZE] = {};
        f.read(reinterpret_cast<char*>(buf), HEADER_SIZE);
        if (!f || f.gcount() < (std::streamsize)HEADER_SIZE) return;

        uint32_t magic;   std::memcpy(&magic,   buf + 0, 4);
        uint16_t version; std::memcpy(&version, buf + 4, 2);
        uint16_t flags;   std::memcpy(&flags,   buf + 6, 2);
        if (magic != MAGIC_FMSS || version != VERSION_V2) return;

        ReplayMeta m{};
        m.path = p.string();
        m.is_battle_slice = (flags & 0x0001) != 0;
        std::memcpy(&m.started_at_unix,  buf + 8,  8);
        std::memcpy(&m.finished_at_unix, buf + 16, 8);
        std::memcpy(&m.event_count,      buf + 24, 4);
        std::memcpy(&m.input_count,      buf + 28, 4);
        std::memcpy(m.game_id,           buf + 32, 32);
        std::memcpy(m.p1_nick,           buf + 64, 32);
        std::memcpy(m.p2_nick,           buf + 96, 32);
        m.p1_char_id    = buf[128];
        m.p2_char_id    = buf[129];
        // colors at 130/131 — not displayed in the tree
        m.rounds_won_p1 = buf[132];
        m.rounds_won_p2 = buf[133];
        m.match_count   = buf[134];
        m.match_index   = buf[135];
        std::memcpy(&m.session_id,       buf + 136, 8);
        m.round_count   = buf[144];
        replays_cache_.push_back(std::move(m));
    };

    for (const auto& root : games_root_paths_) {
        if (root.empty()) continue;
        std::filesystem::path root_fs = std::filesystem::u8path(root);
        if (!std::filesystem::is_directory(root_fs, ec)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "ReplayBrowser: skipping root '%s' — not a directory (ec=%s)",
                root.c_str(), ec ? ec.message().c_str() : "ok");
            continue;
        }
        // Each game lives directly under root; replays/ is one level deeper.
        // Use recursive_directory_iterator with a depth cap so we don't walk
        // user-installed game subdirs unnecessarily.
        size_t hits_before = replays_cache_.size();
        size_t walked      = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root_fs,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec);
             it != std::filesystem::recursive_directory_iterator{};
             it.increment(ec))
        {
            if (ec) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ReplayBrowser: walk error under '%s': %s",
                    root.c_str(), ec.message().c_str());
                break;
            }
            if (it.depth() > 5) { it.disable_recursion_pending(); continue; }
            const auto& entry = *it;
            ++walked;
            if (!entry.is_regular_file(ec)) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (ext == ".fm2krep" || ext == ".fm2kset") {
                try_load_file(entry.path());
            }
        }
        const size_t found = replays_cache_.size() - hits_before;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "ReplayBrowser: root '%s' walked %zu entries, accepted %zu replay file(s)",
            root.c_str(), walked, found);
    }

    // Sort: newest finished first.
    std::sort(replays_cache_.begin(), replays_cache_.end(),
        [](const ReplayMeta& a, const ReplayMeta& b) {
            return a.finished_at_unix > b.finished_at_unix;
        });
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "ReplayBrowser: scanned %zu replay file(s) across %zu games root(s)",
        replays_cache_.size(), games_root_paths_.size());
}

void LauncherUI::RenderReplayBrowser() {
    if (replays_cache_dirty_) ScanReplays();

    if (ImGui::Button("Refresh##replay_browser")) {
        replays_cache_dirty_ = true;
    }
    ImGui::SameLine();
    // #61 (FlippySpatula): the old "newest first" claim read as a promise
    // about EVERY row, but matches inside a set list in play order (match
    // 1 -> N) by design -- Melan's study flow depends on it. Say what the
    // ordering actually is instead of half of it.
    ImGui::TextDisabled("%zu file(s) — sets newest first, matches in play order",
                        replays_cache_.size());

    // Show the configured games-root paths so the user can verify what's
    // being scanned. Common gotcha: launcher in C:\games but games on D:\,
    // and the games-root config still points at the legacy C:\ path —
    // recursive walk silently scans nothing relevant. Surfacing the list
    // means the user can spot it immediately instead of debugging blind.
    if (games_root_paths_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
            "No games-root paths configured — add one via Settings → Games "
            "Folders.");
    } else {
        ImGui::TextDisabled("Scanned roots:");
        for (const auto& root : games_root_paths_) {
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", root.c_str());
        }
    }
    ImGui::Separator();

    if (replays_cache_.empty()) {
        ImGui::TextWrapped("No .fm2krep / .fm2kset files found under any "
                           "configured games-root path. Play a netplay "
                           "match to create one (replays auto-save to "
                           "<game>/replays/<timestamp>.fm2krep). If you "
                           "have replays elsewhere, add the parent folder "
                           "via Settings → Games Folders.");
        return;
    }

    // Group by session_id. Sessions with session_id==0 are legacy files
    // (pre-C7 headers) — render them as standalone rows under a synthetic
    // "Legacy (no session id)" header.
    struct SessionGroup {
        uint64_t session_id;
        std::vector<size_t> indices;  // into replays_cache_
        std::string         p1_nick, p2_nick, game_id;
        uint64_t            latest_finished;
    };
    std::vector<SessionGroup> groups;
    {
        std::unordered_map<uint64_t, size_t> group_by_sid;
        for (size_t i = 0; i < replays_cache_.size(); ++i) {
            const auto& r = replays_cache_[i];
            uint64_t sid = r.session_id;
            auto [it, inserted] = group_by_sid.try_emplace(sid, groups.size());
            if (inserted) {
                groups.push_back({});
                auto& g = groups.back();
                g.session_id      = sid;
                g.p1_nick         = r.p1_nick;
                g.p2_nick         = r.p2_nick;
                g.game_id         = r.game_id;
                g.latest_finished = r.finished_at_unix;
            }
            auto& g = groups[it->second];
            g.indices.push_back(i);
            g.latest_finished =
                std::max(g.latest_finished, r.finished_at_unix);
        }
        std::sort(groups.begin(), groups.end(),
            [](const SessionGroup& a, const SessionGroup& b) {
                return a.latest_finished > b.latest_finished;
            });
        // Order indices within each group by match_index ascending so the
        // tree shows match 1 → match N in temporal order.
        for (auto& g : groups) {
            std::sort(g.indices.begin(), g.indices.end(),
                [&](size_t i, size_t j) {
                    const auto& a = replays_cache_[i];
                    const auto& b = replays_cache_[j];
                    if (a.match_index != b.match_index)
                        return a.match_index < b.match_index;
                    return a.finished_at_unix < b.finished_at_unix;
                });
        }
    }

    auto fmt_unix = [](uint64_t t) -> std::string {
        if (t == 0) return "?";
        time_t tt = static_cast<time_t>(t);
        std::tm lt = {};
        localtime_s(&lt, &tt);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
        return buf;
    };

    for (const auto& g : groups) {
        char hdr[256];
        if (g.session_id == 0) {
            std::snprintf(hdr, sizeof(hdr),
                "Legacy (no session id) — %zu file%s###leg_%p",
                g.indices.size(),
                g.indices.size() == 1 ? "" : "s",
                (void*)&g);
        } else {
            std::snprintf(hdr, sizeof(hdr),
                "%s vs %s — %s — %s — %zu match%s###sid_%016llx",
                g.p1_nick[0] ? g.p1_nick.c_str() : "?",
                g.p2_nick[0] ? g.p2_nick.c_str() : "?",
                fmt_unix(g.latest_finished).c_str(),
                g.game_id[0] ? g.game_id.c_str() : "?",
                g.indices.size(),
                g.indices.size() == 1 ? "" : "es",
                (unsigned long long)g.session_id);
        }
        if (ImGui::TreeNodeEx(hdr,
                              ImGuiTreeNodeFlags_DefaultOpen)) {
            // Set-playback support (#63): does this session group carry a
            // whole-set file? Per-match rows then offer "watch from here"
            // (seek into the set at that match) alongside their own slice.
            const ReplayMeta* set_file = nullptr;
            for (size_t idx : g.indices) {
                if (!replays_cache_[idx].is_battle_slice) {
                    set_file = &replays_cache_[idx];
                    break;
                }
            }
            for (size_t idx : g.indices) {
                const auto& r = replays_cache_[idx];
                char row[512];
                if (r.is_battle_slice) {
                    std::snprintf(row, sizeof(row),
                        "Match %u — char %u vs %u — wins %u-%u — %u INPUTs — %s",
                        (unsigned)r.match_index,
                        (unsigned)r.p1_char_id, (unsigned)r.p2_char_id,
                        (unsigned)r.rounds_won_p1, (unsigned)r.rounds_won_p2,
                        (unsigned)r.input_count,
                        fmt_unix(r.finished_at_unix).c_str());
                } else {
                    std::snprintf(row, sizeof(row),
                        "Whole set — %u match%s — %u INPUTs — %s",
                        (unsigned)r.match_count,
                        r.match_count == 1 ? "" : "es",
                        (unsigned)r.input_count,
                        fmt_unix(r.finished_at_unix).c_str());
                }
                ImGui::PushID(static_cast<int>(idx));
                ImGui::Bullet();
                ImGui::TextUnformatted(row);
                ImGui::SameLine();
                if (ImGui::SmallButton("Watch")) {
                    // Set-or-clear: a prior "From here" click must not leak
                    // its seek into an unrelated watch (same discipline as
                    // the relay-cred envs).
                    ::SetEnvironmentVariableA("FM2K_REPLAY_SEEK_MATCH", nullptr);
                    if (on_replay_play) on_replay_play(r.path);
                }
                if (r.is_battle_slice && set_file &&
                    r.match_index > 0) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Watch set from here")) {
                        char n[8];
                        std::snprintf(n, sizeof(n), "%u",
                                      (unsigned)r.match_index);
                        ::SetEnvironmentVariableA("FM2K_REPLAY_SEEK_MATCH", n);
                        if (on_replay_play) on_replay_play(set_file->path);
                    }
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
}

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
                    ImGui::SetTooltip("Packed with %s — may not run with rollback hooks until unpacked.\n"
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

