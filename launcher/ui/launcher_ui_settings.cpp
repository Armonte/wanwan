// launcher_ui_settings.cpp -- LauncherUI settings/host-config/discord/games-folders windows. Split from FM2K_LauncherUI.cpp (pure move).
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

// Native folder-picker async state + worker (settings-only; moved here
// from FM2K_LauncherUI.cpp because RenderGamesFoldersBody owns it).
// Modern native folder picker via Win32 IFileOpenDialog (the Common Item
// Dialog) -- NOT SDL_ShowOpenFolderDialog, whose Windows backend is the
// legacy SHBrowseForFolder tree dialog. FOS_ALLOWMULTISELECT lets the user
// add several folders at once.
//
// It MUST run ASYNC. IFileOpenDialog::Show is modal to the parent window and
// drives a message loop the parent window's owning thread (our render thread)
// has to keep pumping. Blocking the render thread for the result (e.g.
// join()) DEADLOCKS -- the dialog waits on the render thread to service its
// messages, the render thread waits on the dialog to finish. (This is what
// locked up the window.) So: open it on a detached STA worker, keep
// rendering, and the render thread collects the result via the holder below
// on a later frame -- the same shape as SDL's own async dialog API.
namespace {
std::mutex               g_folder_pick_mtx;
std::vector<std::string> g_folder_pick_result;
std::atomic<bool>        g_folder_pick_ready{false};
std::atomic<bool>        g_folder_dialog_open{false};

void OpenGamesFolderDialogAsync(HWND hwnd) {
    if (g_folder_dialog_open.exchange(true)) return;  // one dialog at a time
    std::thread([hwnd]() {
        std::vector<std::string> picked;
        if (SUCCEEDED(CoInitializeEx(nullptr,
                COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
            IFileOpenDialog* dlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                           reinterpret_cast<void**>(&dlg))) && dlg) {
                DWORD opts = 0;
                dlg->GetOptions(&opts);
                dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_ALLOWMULTISELECT |
                                FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                if (SUCCEEDED(dlg->Show(hwnd))) {
                    IShellItemArray* items = nullptr;
                    if (SUCCEEDED(dlg->GetResults(&items)) && items) {
                        DWORD count = 0;
                        items->GetCount(&count);
                        for (DWORD i = 0; i < count; ++i) {
                            IShellItem* item = nullptr;
                            if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
                                PWSTR wpath = nullptr;
                                if (SUCCEEDED(item->GetDisplayName(
                                        SIGDN_FILESYSPATH, &wpath)) && wpath) {
                                    int n = WideCharToMultiByte(CP_UTF8, 0, wpath,
                                                -1, nullptr, 0, nullptr, nullptr);
                                    if (n > 1) {
                                        std::string s(static_cast<size_t>(n - 1), '\0');
                                        WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                                s.data(), n, nullptr, nullptr);
                                        picked.push_back(std::move(s));
                                    }
                                    CoTaskMemFree(wpath);
                                }
                                item->Release();
                            }
                        }
                        items->Release();
                    }
                }
                dlg->Release();
            }
            CoUninitialize();
        }
        {
            std::lock_guard<std::mutex> lk(g_folder_pick_mtx);
            g_folder_pick_result = std::move(picked);
        }
        g_folder_pick_ready.store(true, std::memory_order_release);
        g_folder_dialog_open.store(false, std::memory_order_release);
    }).detach();
}
}  // namespace

// One Settings window with TabBar -- bindings, host config, hub server,
// games folders, recent matches. Floats above the dockspace, can't be
// dragged or resized (popup-style modal feel without actually being a
// modal -- the user can still click around outside it). Replaces the
// five separate floating sub-windows. Each tab calls a body-only
// renderer; click the X to close, settings auto-save on edit.
void LauncherUI::RenderSettingsWindow() {
    if (!show_settings_) return;

    // Center on the viewport at first open. Re-center on subsequent
    // opens so the window is always findable; user can still nudge it
    // via SetWindowPos in their imgui.ini if they really want.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(560.0f, 420.0f);
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f,
               vp->WorkPos.y + (vp->WorkSize.y - size.y) * 0.5f),
        ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking  |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove;

    if (!ImGui::Begin(T("menu_settings"), &show_settings_, flags)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##settings_tabs",
                           ImGuiTabBarFlags_Reorderable)) {
        // Input Bindings -- single tab with a nested P1/P2 sub-tabbar so
        // the player picker doesn't bloat the top-level tabs.
        if (ImGui::BeginTabItem(T("input_bindings"))) {
            if (!input_binder_initialized_) {
                FM2KInputBinder::Init();
                input_binder_initialized_ = true;
            }
            if (ImGui::BeginTabBar("##input_bindings_players")) {
                if (ImGui::BeginTabItem(T("tab_p1"))) {
                    RenderInputBindingsTab(0);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(T("tab_p2"))) {
                    RenderInputBindingsTab(1);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("panel_host_config"))) {
            RenderHostConfigBody();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("menu_hub_server"))) {
            RenderHubServerBody();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("menu_games_folders"))) {
            RenderGamesFoldersBody();
            ImGui::EndTabItem();
        }
        // Display -- every cnc-ddraw [ddraw] setting. Lives here rather
        // than in the Debug & Diagnostics → Renderer tab because it's a
        // permanent config surface, not a dev knob.
        if (ImGui::BeginTabItem("Display")) {
            RenderDisplayBody();
            ImGui::EndTabItem();
        }
        // Recent Matches lives in the Hub panel (collapsing section
        // beside the room list), not Settings -- match-history isn't a
        // configuration concern, it's session data.
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// Settings → Games Folders… window. Multi-root editor for the launcher's
// games-discovery roots. Lives behind a Settings menu entry so the main
// panel stays focused on the games list itself; casual users with a
// single folder almost never need this UI after first-run setup.
// Drain a result from the async folder dialog (set on the worker thread).
// Append each chosen folder, de-duped.
//
// Called from BOTH the Games Folders body and the game-selection panel's
// first-run prompt. It used to live only in the former, which meant a pick
// started anywhere else would sit in the buffer unread until the user
// happened to open Settings.
void LauncherUI::DrainPickedGamesFolders() {
    if (!g_folder_pick_ready.exchange(false, std::memory_order_acq_rel)) return;
    std::vector<std::string> picked;
    {
        std::lock_guard<std::mutex> lk(g_folder_pick_mtx);
        picked.swap(g_folder_pick_result);
    }
    if (picked.empty()) return;
    std::vector<std::string> paths = games_root_paths_;
    for (auto& p : picked) {
        if (std::find(paths.begin(), paths.end(), p) == paths.end())
            paths.push_back(std::move(p));
    }
    games_root_paths_ = paths;
    if (on_games_folders_set) on_games_folders_set(std::move(paths));
}

// Open the modern native picker async (non-blocking); the result is applied
// by DrainPickedGamesFolders on a later frame. The HWND has to be extracted
// on the render thread.
void LauncherUI::OpenGamesFolderPicker() {
    HWND hwnd = window_ ? static_cast<HWND>(SDL_GetPointerProperty(
                    SDL_GetWindowProperties(window_),
                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
                        : nullptr;
    OpenGamesFolderDialogAsync(hwnd);
}

bool LauncherUI::GamesFolderPickerBusy() const {
    return g_folder_dialog_open.load(std::memory_order_acquire);
}

// The folder editor, INLINE, in whatever pane called it.
//
// This used to be reached by a button that opened a second floating window
// on top of the first -- for a list of folders, in a launcher whose whole
// job at that moment is getting one folder out of the user. A collapsing
// section costs nothing, keeps the context that sent you here, and cannot
// be dragged behind the main window and lost.
//
// RenderGamesFoldersBody was already written body-only ("caller owns the
// container") for exactly this; nothing ever needed the window.
void LauncherUI::RenderGamesFoldersInline() {
    if (ImGui::CollapsingHeader(T("panel_games_folders"))) {
        ImGui::Indent();
        RenderGamesFoldersBody();
        ImGui::Unindent();
    }
}

void LauncherUI::RenderGamesFoldersBody() {
    auto commit = [&](std::vector<std::string> paths) {
        games_root_paths_ = paths;
        if (on_games_folders_set) on_games_folders_set(std::move(paths));
    };

    DrainPickedGamesFolders();

    ImGui::TextWrapped("%s", T("hint_games_folders_window"));
    ImGui::Separator();

    ImGui::PushID("GamesFoldersWindow");
    // Each configured root: a Remove button + the path. Folders are ADDED via
    // the native Browse picker below (multi-select supported) -- the list
    // holds as many roots as you want.
    int remove_index = -1;
    for (size_t i = 0; i < games_root_paths_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(T("btn_remove"))) {
            remove_index = static_cast<int>(i);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(games_root_paths_[i].c_str());
        ImGui::PopID();
    }
    if (remove_index >= 0 && remove_index < (int)games_root_paths_.size()) {
        std::vector<std::string> paths = games_root_paths_;
        paths.erase(paths.begin() + remove_index);
        commit(std::move(paths));
    }

    ImGui::Separator();
    const bool dialog_open = g_folder_dialog_open.load(std::memory_order_acquire);
    ImGui::BeginDisabled(dialog_open);
    if (ImGui::Button(dialog_open ? "Choosing folder..." : T("btn_browse_folder"))) {
        OpenGamesFolderPicker();
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void LauncherUI::RenderGamesFoldersWindow() {
    if (!ImGui::Begin(T("menu_games_folders"), &show_games_folders_,
                      ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }
    RenderGamesFoldersBody();
    ImGui::End();
}

// Settings → Sign in with Discord… window. Drives the OAuth pairing
// flow in FM2K_DiscordAuth: kicks off /pair/begin, opens the browser,
// polls /pair/<code> until success/fail. The hub_token is cached in
// %APPDATA%\FM2K_Rollback\discord_auth.json and read by RenderHubPanel
// at Connect time. Patron-only access -- Tester ($5+) tier required
// during testing, mapped via Patreon→Discord role automation.
void LauncherUI::RenderDiscordAuthWindow() {
    using namespace fm2k::discord_auth;
    static std::unique_ptr<Pairing> s_pairing;
    static std::string              s_status;
    static fm2k::discord_auth::CachedAuth s_cached = LoadCached();

    if (!ImGui::Begin(T("hub_signin"), &show_discord_auth_,
                      ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    if (s_cached.valid) {
        // Display the actual Discord global_name; nick is the launcher-
        // local custom override which can be anything.
        const std::string display = s_cached.discord_global_name.empty()
            ? s_cached.nick : s_cached.discord_global_name;
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f),
                           "Signed in as %s", display.c_str());
        ImGui::TextDisabled(T("label_discord_id"), s_cached.discord_user_id.c_str());
        if (ImGui::Button(T("hub_signout"))) {
            ClearCached();
            s_cached = CachedAuth{};
            // Tell the menu-bar pill to flip back to the orange
            // "Sign in with Discord" state on the next render.
            discord_signed_in_ = false;
            discord_nick_.clear();
        }
        ImGui::SameLine();
    }

    const bool busy = s_pairing && s_pairing->status() == Pairing::Status::Pending;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button(s_cached.valid ? T("hub_resignin") : T("hub_signin"))) {
        // Build hub HTTP base URL. Default is HTTPS via the public
        // reverse proxy (hub.2dfm.org -> Caddy -> 127.0.0.1:7700 on the
        // DO droplet).
        //
        // DEFAULT RESTORATION for the ONE retired name (review A8a): only the
        // exact dead literal 2dfm.sytes.net is restored to the default. An
        // unanchored find("sytes.net") would silently redirect a user's own
        // free-dyndns hub -- typed this session, since hub_host_ is never
        // persisted -- to production, together with their Discord OAuth
        // exchange. Same predicate as the WS path in
        // launcher_ui_hub_automation.cpp::HubConnect.
        std::string base;
        std::string host = hub_host_[0] ? hub_host_ : "hub.2dfm.org";
        if (fm2k::hub_endpoint::IsRetiredHubHost(host)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: hub host '%s' is the RETIRED No-IP name (DNS removed) -- "
                "restoring the default hub.2dfm.org.", host.c_str());
            host = "hub.2dfm.org";
        } else if (host.find("sytes.net") != std::string::npos) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: hub host '%s' is not the retired 2dfm.sytes.net -- using "
                "it AS TYPED for the Discord pairing exchange.", host.c_str());
        }
        // Plain http (no TLS front) is for a LOCAL hub only: a loopback
        // spelling, a private-IP host, or an explicit host:port on one of
        // those. Everything else is the public hostname behind Caddy (https).
        //
        // PHASE 4e (review A8c). Two downgrades were reachable for a PUBLIC
        // host and the comment above them claimed neither could be:
        //   * FM2K_HUB_INSECURE was not conditioned on the host at all;
        //   * bare presence-of-a-colon sent ANY "example.com:8443" to plain
        //     http -- and this exchange is HOW the hub_token is obtained, so it
        //     ran the Discord OAuth pairing unencrypted against a public host.
        // Both are now gated on the host actually being private. The colon term
        // survives only for a private host (a local dev hub's 127.0.0.1:7700).
        const char* insecure_env = std::getenv("FM2K_HUB_INSECURE");
        const bool  insecure_req = (insecure_env && std::strcmp(insecure_env, "1") == 0);
        const size_t colon       = host.find(':');
        // "::1" is a loopback spelling, not a host:port -- test it first.
        const bool  loopback_hub = fm2k::hub_endpoint::IsLoopbackHost(host);
        const std::string bare   = (!loopback_hub && colon != std::string::npos)
                                     ? host.substr(0, colon) : host;
        const bool  has_port     = (!loopback_hub && colon != std::string::npos);
        const bool  private_hub  = fm2k::hub_endpoint::IsPrivateHost(bare);
        const bool  hub_insecure = insecure_req && private_hub;
        if (insecure_req && !private_hub) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: REFUSING FM2K_HUB_INSECURE for '%s' -- not a loopback or "
                "private address. The Discord pairing exchange stays on https.",
                host.c_str());
        }
        if (has_port && !private_hub) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: '%s' names a port on a PUBLIC host -- using https, not "
                "plain http. The Discord pairing exchange must not run in the "
                "clear against a routable host.", host.c_str());
        }
        if ((has_port && private_hub) || loopback_hub || hub_insecure) {
            // A bare IPv6 literal needs brackets before a port can be appended
            // (previously "::1" produced the malformed "http://::1").
            const bool v6_bare = (host == "::1");
            base = "http://" + (v6_bare ? std::string("[::1]") : host);
            if (!has_port) base += ":7700";
        } else {
            base = "https://" + host;
        }
        s_pairing.reset(Begin(base));
        s_status = "Browser opened. Click Authorize on Discord and come back.";
    }
    if (busy) ImGui::EndDisabled();

    if (s_pairing) {
        switch (s_pairing->status()) {
            case Pairing::Status::Pending: {
                const std::string url = s_pairing->authorize_url();
                const bool open_failed = s_pairing->browser_open_failed();

                // Auto-copy on the first render after we detect the
                // browser launch failed. Static one-shot keyed by the
                // pairing code so a fresh sign-in click after a prior
                // failure re-arms (the URL changes; the user might
                // try again on a different account etc.).
                static std::string s_auto_copied_for_code;
                if (open_failed && !url.empty()) {
                    const std::string pc = s_pairing->pairing_code();
                    if (s_auto_copied_for_code != pc) {
                        ImGui::SetClipboardText(url.c_str());
                        s_auto_copied_for_code = pc;
                        s_status =
                            "Browser didn't open -- URL has been COPIED to "
                            "your clipboard. Paste it (Ctrl+V) in your "
                            "browser to authorize Discord.";
                    }
                }

                // Status line. Yellow tint when the browser failed so
                // the change in instruction stands out vs. the calm
                // "browser opened, click Authorize" text.
                if (open_failed) {
                    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.30f, 1.0f),
                                       "%s", s_status.c_str());
                } else {
                    ImGui::TextWrapped("%s", s_status.c_str());
                }

                // Always surface the URL while pairing is pending --
                // browser auto-launch can fail silently for many
                // reasons (Admin process / no http handler / AV blocking
                // ShellExecute / other). The Copy + Reopen buttons let
                // the user retry without re-clicking sign-in.
                if (!url.empty()) {
                    ImGui::Spacing();
                    if (open_failed) {
                        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.30f, 1.0f),
                            "Paste this URL in your browser:");
                    } else {
                        ImGui::TextDisabled("If your browser didn't open:");
                    }
                    ImGui::PushStyleColor(ImGuiCol_FrameBg,
                        ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
                    char url_buf[1024];
                    std::snprintf(url_buf, sizeof(url_buf), "%s", url.c_str());
                    ImGui::PushItemWidth(-90);
                    ImGui::InputText("##authorize_url", url_buf,
                                     sizeof(url_buf),
                                     ImGuiInputTextFlags_ReadOnly);
                    ImGui::PopItemWidth();
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (ImGui::Button("Copy")) {
                        ImGui::SetClipboardText(url.c_str());
                        s_status = "URL copied -- paste it in your browser.";
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reopen")) {
                        // Manual retry of the browser launch. Useful
                        // when the first attempt fired before the user
                        // had focus on a browser-capable session.
                        ShellExecuteA(nullptr, "open", url.c_str(),
                                      nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
                break;
            }
            case Pairing::Status::Ok: {
                auto a = s_pairing->result();
                if (SaveCached(a)) s_cached = a;
                // Surface the Discord display name (global_name) on the
                // sign-in confirmation, not the launcher's custom nick --
                // the user is verifying which Discord account they
                // bound, not what nick they'll appear as in lobbies.
                const std::string display = a.discord_global_name.empty()
                    ? a.nick : a.discord_global_name;
                s_status = "Signed in as " + display + ". You can connect to the hub now.";
                s_pairing.reset();
                // Refresh the menu-bar pill so it flips green this frame.
                discord_signed_in_ = true;
                discord_nick_      = display;
                // Auto-close after a brief moment so the user sees the
                // success message but isn't stuck on a modal-feeling
                // window. Closing here is immediate; the
                // confirmation lives on the menu-bar pill which now
                // shows the green "Discord: <nick>" state.
                show_discord_auth_ = false;
                break;
            }
            case Pairing::Status::Expired:
                ImGui::TextColored(ImVec4(0.85f, 0.6f, 0.3f, 1.0f),
                                   "%s", s_pairing->error_detail().c_str());
                if (ImGui::Button(T("btn_dismiss"))) s_pairing.reset();
                break;
            case Pairing::Status::Error:
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.95f, 0.32f, 0.32f, 1.0f));
                ImGui::TextWrapped("%s", s_pairing->error_detail().c_str());
                ImGui::PopStyleColor();
                if (ImGui::Button(T("btn_dismiss"))) s_pairing.reset();
                break;
        }
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "Tester ($5+) tier on Patreon required during testing. Pledge on "
        "Patreon, link your Discord on the Patreon Connections page, then "
        "click Sign in here. Patreon assigns the Discord role automatically; "
        "the hub checks your roles when you sign in.");
    ImGui::End();
}

// Host-side match settings UI. SOCD mode + stage selection for now;
// round count / time limit / game speed are forward-compat fields whose
// addresses aren't mapped per-game yet. Settings are saved to fm2k_host.ini
// next to the launcher and pushed over the control channel via the hook
// DLL's Netplay_BroadcastHostConfig.
void LauncherUI::RenderHostConfigBody() {
    ImGui::TextWrapped(
        "Per-game match settings. Edits here override the game's "
        "default game.ini for THIS launcher; the host's resolved values "
        "get pushed to the client + spectators on challenge (#54). "
        "HitJudge / GameInformation are force-zeroed online -- saved "
        "to disk for offline practice but never applied to a hub match.");
    ImGui::Separator();

    if (selected_game_index_ < 0 ||
        selected_game_index_ >= (int)games_.size())
    {
        ImGui::TextDisabled("%s", T("warn_no_game_selected"));
        return;
    }
    const auto& game = games_[selected_game_index_];
    // Wide-construct so JP-named exes (ＣＰＷ.exe etc.) keep their
    // bytes intact through stem()/parent_path() instead of
    // round-tripping through MinGW's ANSI codepage.
    const std::filesystem::path exe =
        fm2k::utf8path::Utf8ToWide(game.exe_path);
    const std::filesystem::path ini = fm2k::game_ini::PathForExe(exe);

    static int          s_loaded_for = -1;
    static fm2k::game_ini::GamePlayConfig s_defaults;
    static fm2k::game_ini::GamePlayConfig s_override;
    static bool         s_dirty = false;
    if (s_loaded_for != selected_game_index_) {
        s_loaded_for = selected_game_index_;
        s_defaults = {};
        s_override = {};
        fm2k::game_ini::Load(ini, s_defaults);
        fm2k::game_ini::LoadOverride(exe, s_override);
        s_dirty = false;
    }

    // Render via TextUnformatted instead of Text("%s", ...). MinGW's
    // vsnprintf goes through the C locale's narrow conversion and
    // turns non-CP1252 bytes (full-width forms like ＣＰＷ.exe) into
    // '_'. The games list above gets away with TextUnformatted +
    // SameLine; we do the same here for the static prefix and the
    // dynamic name. ImGui itself decodes UTF-8 just fine -- the
    // mangling is exclusively in printf-style format specifiers.
    ImGui::TextUnformatted("Game: ");
    ImGui::SameLine(0.0f, 0.0f);
    {
        // One-shot diagnostic: dump the bytes we're handing to ImGui
        // so we can see whether they survive (vs. games-list which
        // works) or get mangled by some intermediate layer. Logs once
        // per game-selection change.
        static int s_logged_for = -2;
        if (s_logged_for != selected_game_index_) {
            s_logged_for = selected_game_index_;
            const std::string n = game.GetExeName();
            std::string hex;
            char buf[8];
            for (unsigned char c : n) {
                std::snprintf(buf, sizeof(buf), "%02X ", c);
                hex += buf;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "HostConfig: GetExeName='%s' (%zu bytes: %s)",
                n.c_str(), n.size(), hex.c_str());
        }
        ImGui::TextUnformatted(game.GetExeName().c_str());
    }

    {
        std::string ini_display = game.GetExeDir();
        if (!ini_display.empty() && ini_display.back() != '/' &&
            ini_display.back() != '\\') {
            ini_display += '/';
        }
        ini_display += "game.ini";
        // TextDisabled has the same printf trap -- wrap it manually.
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted("game.ini: ");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted(ini_display.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    if (ImGui::BeginTable("##gameplay_overrides", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Setting",   ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Default",   ImGuiTableColumnFlags_WidthFixed,  70.0f);
        ImGui::TableSetupColumn("Override",  ImGuiTableColumnFlags_WidthFixed,  90.0f);
        ImGui::TableSetupColumn("Notes");
        ImGui::TableHeadersRow();

        struct Row {
            const char* label;
            int fm2k::game_ini::GamePlayConfig::* member;
            const char* note;
            int min;
            int max;
        };
        static const Row rows[] = {
            {"Round count (1v1)",   &fm2k::game_ini::GamePlayConfig::vs_single_play, "Editor.TestPlay.VSSinglePlay",      1,  9},
            {"Round count (team)",  &fm2k::game_ini::GamePlayConfig::vs_team_play,   "Editor.TestPlay.VSTeamPlay",        1,  9},
            {"Round timer (s)",     &fm2k::game_ini::GamePlayConfig::time,           "0 = infinite",                      0, 99},
            {"Game speed",          &fm2k::game_ini::GamePlayConfig::game_speed,     "default 10",                        1, 16},
            {"Stage",               &fm2k::game_ini::GamePlayConfig::stage_nb,       "stage index, 0 = first",            0, 99},
            {"Joystick (0=KB,1=Pad)",&fm2k::game_ini::GamePlayConfig::joystick,      "force 0 unless game lags",          0,  1},
            {"P0 CPU",              &fm2k::game_ini::GamePlayConfig::player0_cpu,    "force 0 online (anti-cheat)",       0,  1},
            {"P1 CPU",              &fm2k::game_ini::GamePlayConfig::player1_cpu,    "force 0 online (anti-cheat)",       0,  1},
            {"Hit-judge overlay",   &fm2k::game_ini::GamePlayConfig::hit_judge,      "FORCED 0 online (anti-cheat)",      0,  1},
            {"Damage info overlay", &fm2k::game_ini::GamePlayConfig::game_information,"FORCED 0 online (anti-cheat)",     0,  1},
            {"VS mode",             &fm2k::game_ini::GamePlayConfig::vs_mode,        "Editor.TestPlay.VSMode",            0,  9},
        };

        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.label);

            ImGui::TableSetColumnIndex(1);
            const int def_val = s_defaults.*r.member;
            if (def_val == fm2k::game_ini::kUnset) {
                ImGui::TextDisabled("--");
            } else {
                ImGui::Text("%d", def_val);
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(r.label);
            int  cur     = s_override.*r.member;
            bool enabled = (cur != fm2k::game_ini::kUnset);
            bool prev_enabled = enabled;
            ImGui::Checkbox("##en", &enabled);
            if (enabled != prev_enabled) {
                if (enabled) {
                    s_override.*r.member = (def_val == fm2k::game_ini::kUnset)
                        ? r.min : def_val;
                } else {
                    s_override.*r.member = fm2k::game_ini::kUnset;
                }
                s_dirty = true;
            }
            if (enabled) {
                ImGui::SameLine();
                int v = s_override.*r.member;
                ImGui::SetNextItemWidth(60.0f);
                if (ImGui::InputInt("##v", &v, 0)) {
                    if (v < r.min) v = r.min;
                    if (v > r.max) v = r.max;
                    s_override.*r.member = v;
                    s_dirty = true;
                }
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%s", r.note);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (s_dirty) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                           "Unsaved overrides -- apply or reset.");
    }
    if (ImGui::Button("Apply overrides")) {
        if (fm2k::game_ini::SaveOverride(exe, s_override)) {
            s_dirty = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Host config: saved overrides for %s",
                game.GetExeName().c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) {
        s_override = {};
        fm2k::game_ini::SaveOverride(exe, s_override);
        s_dirty = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        s_loaded_for = -1;
    }

    // ─── Random stage (#56) ──────────────────────────────────────
    // Per-launcher persistence (settings.ini, not per-game) so the
    // user toggles once and it follows them across games. Both peers
    // run the same xorshift sequence from a shared seed so rematches
    // re-roll deterministically without per-match wire traffic.
    ImGui::Spacing();
    ImGui::SeparatorText("Random stage");
    EnsureRandomStageLoaded();  // per-game; reloads when the game changes
    bool prev_enable = random_stage_enable_;
    if (ImGui::Checkbox("Enable random stage", &random_stage_enable_)) {
        SaveRandomStageState();
    }
    if (random_stage_enable_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(rolls a fresh stage each match)");
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputInt("Min stage", &random_stage_min_, 0)) {
            if (random_stage_min_ < 0) random_stage_min_ = 0;
            if (random_stage_min_ > random_stage_max_) random_stage_min_ = random_stage_max_;
            SaveRandomStageState();
        }
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputInt("Max stage", &random_stage_max_, 0)) {
            if (random_stage_max_ < random_stage_min_) random_stage_max_ = random_stage_min_;
            if (random_stage_max_ > 99) random_stage_max_ = 99;
            SaveRandomStageState();
        }
        ImGui::TextWrapped(
            "Inclusive range, saved PER GAME. Set to your game's stage "
            "count - 1 (FM2K indexes from 0); the hook additionally clamps "
            "to the game's real stage list so an oversized range can never "
            "load a missing stage. Both peers' hooks seed an xorshift PRNG "
            "with the host's seed, then advance by one per match -- "
            "deterministic lockstep, no extra wire traffic per rematch.");
    }
    (void)prev_enable;

    // ---------- Per-game experimental patches ----------
    // Hook-side patches that compensate for FM2K engine bugs or expose
    // optional gameplay tweaks. Each setting is per-game (stored in
    // %APPDATA%\FM2K_Rollback\game_patches\<game_id>.ini) so different
    // FM2K games can opt in/out independently. Restart the game after
    // toggling -- env vars are read at hook init.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                       "Per-game experimental patches");
    ImGui::TextWrapped(
        "Hook-side fixes for FM2K engine bugs. Stored per-game in "
        "%%APPDATA%%\\FM2K_Rollback\\game_patches\\%s.ini. Restart "
        "the game after toggling.",
        GameIdForExePath(game.exe_path).c_str());
    ImGui::Spacing();

    // Per-game state cache: reload toggles when the selected game changes.
    static int  s_patches_loaded_for         = -1;
    static bool s_patch_gs_pic_fix           = true;
    static bool s_patch_team_css_dupe        = false;
    static bool s_patch_team_kof_retention   = false;
    static int  s_patch_team_size            = 0;     // 0 = engine default (don't override)
    static int  s_patch_damage_mult_pct      = 100;   // 100 = no scaling
    static bool s_patch_vs_cpu_mode          = false;
    static bool s_patch_cpu_vs_cpu_mode      = false;
    static bool s_patch_training_mode        = false;
    static bool s_patch_option_mode_selector = false;
    if (s_patches_loaded_for != selected_game_index_) {
        s_patches_loaded_for = selected_game_index_;
        const std::string gid = GameIdForExePath(game.exe_path);
        s_patch_gs_pic_fix           = LoadGamePatchBool(gid, "gs_pic_fix", true);
        s_patch_team_css_dupe        = LoadGamePatchBool(gid, "team_css_dupe_lock", false);
        s_patch_team_kof_retention   = LoadGamePatchBool(gid, "team_kof_retention", false);
        s_patch_team_size            = LoadGamePatchInt (gid, "team_size", 0);
        s_patch_damage_mult_pct      = LoadGamePatchInt (gid, "damage_multiplier_pct", 100);
        s_patch_vs_cpu_mode          = LoadGamePatchBool(gid, "vs_cpu_mode", false);
        s_patch_cpu_vs_cpu_mode      = LoadGamePatchBool(gid, "cpu_vs_cpu_mode", false);
        s_patch_training_mode        = LoadGamePatchBool(gid, "training_mode", false);
        s_patch_option_mode_selector = LoadGamePatchBool(gid, "option_mode_selector", false);
    }

    const std::string gid_for_save = GameIdForExePath(game.exe_path);

    // ---- Implemented patches ----
    ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "Bug fixes & engine tweaks");

    if (ImGui::Checkbox("EXPERIMENTAL: speed patch for vanpri (gs != 10 fix)",
                        &s_patch_gs_pic_fix)) {
        SaveGamePatchBool(gid_for_save, "gs_pic_fix", s_patch_gs_pic_fix);
    }
    ImGui::SetItemTooltip(
        "Locks the broken script-wait scaling to its working value (the "
        "value at GameSpeed=10). Speed change still works (chars move "
        "faster) but scripts no longer break.\n\n"
        "Confirmed working on Vanpri.");

    if (ImGui::Checkbox("Team CSS: prevent duplicate character per team",
                        &s_patch_team_css_dupe)) {
        SaveGamePatchBool(gid_for_save, "team_css_dupe_lock", s_patch_team_css_dupe);
    }
    ImGui::SetItemTooltip(
        "Team-mode only. Masks each player's confirm bits if the cursor "
        "would land on a character already locked into one of that "
        "player's earlier team slots.");

    if (ImGui::Checkbox("Team KOF retention: winner keeps HP & meter into next round",
                        &s_patch_team_kof_retention)) {
        SaveGamePatchBool(gid_for_save, "team_kof_retention",
                          s_patch_team_kof_retention);
    }
    ImGui::SetItemTooltip(
        "Team-mode only. When the round ends, the winner's remaining HP "
        "and super meter carry into the next round (loser's incoming "
        "character initializes at full HP / default meter). Vanilla "
        "behavior resets both sides to full each round.");

    ImGui::SetNextItemWidth(160);
    if (ImGui::InputInt("Team size override (0 = engine default)",
                        &s_patch_team_size, 1, 1)) {
        if (s_patch_team_size < 0) s_patch_team_size = 0;
        if (s_patch_team_size > 4) s_patch_team_size = 4;
        SaveGamePatchInt(gid_for_save, "team_size", s_patch_team_size);
    }
    ImGui::SetItemTooltip(
        "Override g_team_round @ 0x430128. Range [2, 4]; 0 leaves the "
        "engine's INI-loaded value alone. Hard ceiling is 4 per side -- "
        "the engine indexes its 8-slot character data pool as "
        "4*player_idx + round_count, so values >4 stomp the opposite "
        "player's slots.");

    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Damage multiplier %%",
                         &s_patch_damage_mult_pct, 1, 500, "%d%%")) {
        SaveGamePatchInt(gid_for_save, "damage_multiplier_pct",
                         s_patch_damage_mult_pct);
    }
    ImGui::SetItemTooltip(
        "Scales the damage argument to health_damage_manager by this "
        "percentage. 100 = no change. 50 = halved damage. 200 = doubled. "
        "Hook only installs when != 100, so default users pay no "
        "trampoline cost.");

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "Modes");
    ImGui::TextWrapped(
        "Two ways to use these:\n"
        "  - Enable ONE individual mode toggle below for a always-on "
        "default on this game.\n"
        "  - Enable OPTION-button selector to cycle Default → VS CPU → "
        "Training → CPU vs CPU at runtime via the OPTION button on title.\n\n"
        "If both are set, OPTION cycle wins (overrides individual flags "
        "at title→CSS).");
    ImGui::Spacing();

    // No mutual exclusion in the UI -- the hook resolves combinations in a
    // documented fixed precedence (option_mode_selector cycle > cpu_vs_cpu
    // > vs_cpu > training for battle behavior; CSS takeover fires if ANY
    // mode is active). Some users want all three individual flags
    // enableable simultaneously for diagnostics, and the OPTION cycle
    // requires only its own toggle -- over-restricting blocked legitimate
    // configurations.

    if (ImGui::Checkbox("VS CPU mode (P1 picks both, AI plays P2)",
                        &s_patch_vs_cpu_mode)) {
        SaveGamePatchBool(gid_for_save, "vs_cpu_mode", s_patch_vs_cpu_mode);
    }
    ImGui::SetItemTooltip(
        "Always-on VS CPU for this game. P1's CSS cursor drives both "
        "character slots; in battle P2's input is zeroed so the engine's "
        "script-driven AI takes over. Skip this toggle if you're using "
        "the OPTION cycle below.");

    if (ImGui::Checkbox("CPU vs CPU (both AI)", &s_patch_cpu_vs_cpu_mode)) {
        SaveGamePatchBool(gid_for_save, "cpu_vs_cpu_mode",
                          s_patch_cpu_vs_cpu_mode);
    }
    ImGui::SetItemTooltip(
        "Always-on CPU vs CPU. CSS uses P1 input for both cursors so you "
        "can pick the matchup; in battle both inputs are zeroed. Skip if "
        "using OPTION cycle.");

    if (ImGui::Checkbox("Training mode (P2 behavior options)",
                        &s_patch_training_mode)) {
        SaveGamePatchBool(gid_for_save, "training_mode",
                          s_patch_training_mode);
    }
    ImGui::SetItemTooltip(
        "Always-on Training. P1 picks both characters via CSS pipe; in "
        "battle P2 behavior cycles through Player / CPU / Imitate / Guard "
        "/ Jump-up via the FN2 hotkey (default F2). Skip if using OPTION "
        "cycle.");

    ImGui::Spacing();

    if (ImGui::Checkbox("OPTION-button mode selector + title overlay",
                        &s_patch_option_mode_selector)) {
        SaveGamePatchBool(gid_for_save, "option_mode_selector",
                          s_patch_option_mode_selector);
    }
    ImGui::SetItemTooltip(
        "Press the OPTION button (default Tab, rebindable via Input "
        "Bindings) on the title screen to cycle through:\n"
        "  Default → VS CPU → Training → CPU vs CPU → Default\n\n"
        "Badge appears in the top-right of the game viewport showing "
        "the queued submode (hidden in Default -- no overlay clutter). "
        "The chosen submode applies at title→CSS transition; P1's CSS "
        "cursor drives both characters for the three non-default modes. "
        "Returning to title clears the selection so you can pick again "
        "next round.\n\n"
        "You don't need to also enable the individual mode toggles "
        "above -- the cycle controls them at runtime. They DO override "
        "Default when the cycle is at slot 0 (so if you want \"VS CPU "
        "unless I explicitly cycle\", enable VS CPU + OPTION together).");
}

void LauncherUI::RenderHostConfigWindow() {
    if (!ImGui::Begin("Host Config", &show_host_config_, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }
    RenderHostConfigBody();
    ImGui::End();
}

