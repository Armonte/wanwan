// launcher_ui_menubar.cpp -- LauncherUI top menu bar. Split from FM2K_LauncherUI.cpp (pure move).
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

void LauncherUI::RenderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(T("menu_file"))) {
            // "Select Games Folder" used to live here but it duplicates
            // Settings → Games Folders, so it's been folded into that
            // tab. Exit is the only thing left in File since it's the
            // canonical place users look for "quit the app."
            if (ImGui::MenuItem(T("menu_exit"), "Alt+F4")) {
                if (on_exit) on_exit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("menu_session"))) {
            if (launcher_state_ == LauncherState::InGame || launcher_state_ == LauncherState::Connecting) {
                if (ImGui::MenuItem(T("hub_disconnect"))) {
                    if (on_session_stop) on_session_stop();
                }
            } else {
                ImGui::MenuItem(T("hub_disconnect"), nullptr, false, false); // Disabled
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("menu_view"))) {
            if (ImGui::MenuItem(T("menu_developer_mode"), nullptr, developer_mode_)) {
                developer_mode_ = !developer_mode_;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("menu_settings"))) {
            // Single Settings… entry opens a tabbed window; everything
            // else (bindings, host config, hub server, games folders,
            // recent matches) lives as tabs inside that window. Discord
            // sign-in stays separate because its OAuth flow has its own
            // pending/error/success state machine.
            if (ImGui::MenuItem(T("menu_settings_window"))) {
                show_settings_ = true;
            }
            if (ImGui::MenuItem(T("hub_signin_ellipsis"), nullptr, show_discord_auth_)) {
                show_discord_auth_ = !show_discord_auth_;
            }
            if (ImGui::MenuItem("Replays…", nullptr, show_replay_browser_)) {
                show_replay_browser_ = !show_replay_browser_;
                if (show_replay_browser_) replays_cache_dirty_ = true;
            }
            ImGui::Separator();
            // Audio mutes — write to %APPDATA%\FM2K_Rollback\audio.ini.
            // The hook DLL re-reads it every ~1s from inside the audio
            // dispatcher, so the toggle takes effect within a second
            // without needing the game to restart.
            if (!mute_state_loaded_) {
                mute_state_loaded_ = true;
                LoadAudioMuteState();
            }
            if (ImGui::MenuItem(T("audio_mute_music"), nullptr, mute_bgm_)) {
                mute_bgm_ = !mute_bgm_;
                SaveAudioMuteState();
            }
            if (ImGui::MenuItem(T("audio_mute_se"), nullptr, mute_se_)) {
                mute_se_ = !mute_se_;
                SaveAudioMuteState();
            }
            ImGui::Separator();
            // Notification toggles. Lazy-loaded once on first menu render
            // so the read of settings.ini doesn't happen until the user
            // actually opens the Settings menu.
            if (!notify_state_loaded_) {
                LoadNotifyState();
                notify_state_loaded_ = true;
            }
            if (ImGui::BeginMenu(T("menu_notifications"))) {
                if (ImGui::MenuItem(T("notify_taskbar_flash"), nullptr, notify_flash_)) {
                    notify_flash_ = !notify_flash_;
                    SaveNotifyState();
                }
                if (ImGui::MenuItem(T("notify_sound"), nullptr, notify_sound_)) {
                    notify_sound_ = !notify_sound_;
                    SaveNotifyState();
                }
                if (ImGui::MenuItem(T("notify_toast"), nullptr, notify_toast_)) {
                    notify_toast_ = !notify_toast_;
                    SaveNotifyState();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(T("menu_check_for_updates"))) {
                fm2k::updater::CheckForUpdates();
            }
            ImGui::TextDisabled(T("label_version_rev"), fm2k::kAppVersion, fm2k::kAppRevision);
            ImGui::EndMenu();
        }

        // Language menu — top-level so users don't have to dig into
        // Settings to switch. Each entry labels itself in its own native
        // script so anyone can recognize their language regardless of what
        // the launcher is currently set to. Toggling persists the choice to
        // %APPDATA%\FM2K_Rollback\settings.ini and applies on the next
        // frame (no restart needed — the font atlas has every glyph range
        // loaded once at boot).
        if (ImGui::BeginMenu(T("menu_language"))) {
            const fm2k::Lang current = fm2k::Locale::Current();
            for (fm2k::Lang lang : fm2k::Locale::All()) {
                bool selected = (lang == current);
                if (ImGui::MenuItem(fm2k::Locale::DisplayNameForLang(lang),
                                    nullptr, selected)) {
                    fm2k::Locale::Set(lang);
                }
            }
            ImGui::EndMenu();
        }

        // Release channel toggle — RIGHT in the menu bar, always visible.
        // Persisted to dev_flags.ini under "update_channel" (0=stable,
        // 1=dev, 2=bleeding); fm2k::updater::ReadUpdateChannel reads the
        // same key. Switching auto-fires CheckForUpdates because that's the
        // only reason anyone would flip it. Each MenuItem label includes
        // the latest known version on that channel so the user can decide
        // whether flipping is worth it without poking around — they see
        // "Stable(0.2.53)  Dev(0.2.54)  Bleeding(0.2.58-bleeding)" inline.
        // Tiers nest: dev shows stable+dev, bleeding shows everything.
        {
            static int s_channel = LoadDevFlagInt("update_channel", 0);
            const auto upd_snap = fm2k::updater::Get();
            ImGui::TextDisabled("RELEASE:");

            auto channel_label = [](const char* name, const std::string& ver, char* buf, size_t cap) {
                if (ver.empty()) std::snprintf(buf, cap, "%s##bar_channel", name);
                else             std::snprintf(buf, cap, "%s(%s)##bar_channel",
                                                name, ver.c_str());
            };
            char lbl_stable[64], lbl_dev[64], lbl_bleeding[64];
            channel_label("Stable",   upd_snap.latest_stable,   lbl_stable,   sizeof(lbl_stable));
            channel_label("Dev",      upd_snap.latest_dev,      lbl_dev,      sizeof(lbl_dev));
            channel_label("Bleeding", upd_snap.latest_bleeding, lbl_bleeding, sizeof(lbl_bleeding));

            auto select_channel = [&](int ch) {
                if (s_channel != ch) {
                    s_channel = ch;
                    SaveDevFlagInt("update_channel", ch);
                    fm2k::updater::CheckForUpdates();
                }
            };

            if (ImGui::MenuItem(lbl_stable,   nullptr, s_channel == 0)) select_channel(0);
            if (ImGui::MenuItem(lbl_dev,      nullptr, s_channel == 1)) select_channel(1);
            if (ImGui::MenuItem(lbl_bleeding, nullptr, s_channel == 2)) select_channel(2);
        }

        // Lazy-load auth state on first menu-bar render. File is only
        // touched when the auth window saves/clears, so the read is
        // cheap and we don't need to refresh per-frame.
        if (!discord_state_loaded_) {
            discord_state_loaded_ = true;
            const auto a = fm2k::discord_auth::LoadCached();
            discord_signed_in_ = a.valid;
            // Show the actual Discord display name in the top-bar pill,
            // not the launcher's custom in-app nick — those can be
            // arbitrary strings and confuse users about which account
            // they're signed in to. Falls back to nick / "signed in" if
            // the cache is missing the new field (older auth.json).
            discord_nick_      = a.discord_global_name.empty()
                                 ? a.nick
                                 : a.discord_global_name;
        }

        // Kick off the version check exactly once on first menu-bar
        // render. Async; pill below shows the result whenever it lands.
        static bool s_did_update_check = false;
        if (!s_did_update_check) {
            s_did_update_check = true;
            fm2k::updater::CheckForUpdates();
            // Same first-frame slot also kicks the cnc-ddraw bundled
            // installer. Idempotent; version is PINNED so an up-to-date
            // install bails with no network at all (and managed ini
            // keys get migrated in the same pass). New installs get
            // auto-fetched without any user click.
            fm2k::cnc_ddraw::EnsureInstalled();
        }

        // Build BOTH pills (update + Discord) and right-align them
        // together so they don't drift around as state changes.
        const auto upd = fm2k::updater::Get();

        char update_pill[80] = {};
        bool show_update_pill = false;
        ImVec4 update_col{};
        switch (upd.state) {
            case fm2k::updater::State::UpdateAvailable:
                // Downgrade case (local > remote): user just flipped to
                // a channel where the latest is BELOW their installed
                // build. Wording shifts from "Update" to "Switch" so
                // they know they're going backwards intentionally.
                if (fm2k::updater::IsRemoteOlderThanLocal()) {
                    std::snprintf(update_pill, sizeof(update_pill),
                                  "  Switch %s -> %s  ",
                                  fm2k::kAppVersion, upd.remote_version.c_str());
                    update_col      = ImVec4(0.95f, 0.70f, 0.40f, 1.0f);
                } else {
                    std::snprintf(update_pill, sizeof(update_pill),
                                  "  Update %s -> %s  ",
                                  fm2k::kAppVersion, upd.remote_version.c_str());
                    update_col      = ImVec4(0.40f, 0.65f, 0.95f, 1.0f);
                }
                show_update_pill = true;
                break;
            case fm2k::updater::State::Downloading: {
                int pct = (upd.total_bytes > 0)
                    ? (int)(((uint64_t)upd.downloaded_bytes * 100) / upd.total_bytes)
                    : 0;
                std::snprintf(update_pill, sizeof(update_pill),
                              "  Downloading %d%%  ", pct);
                update_col      = ImVec4(0.40f, 0.65f, 0.95f, 1.0f);
                show_update_pill = true;
                break;
            }
            case fm2k::updater::State::Ready:
                std::snprintf(update_pill, sizeof(update_pill),
                              "  Apply %s — Restart  ", upd.remote_version.c_str());
                update_col      = ImVec4(0.40f, 0.85f, 0.50f, 1.0f);
                show_update_pill = true;
                break;
            case fm2k::updater::State::Failed:
                // Surface the failure quietly — clickable so the user
                // can re-trigger via the menu, but not blinking. Most
                // common failure is "fm2ktest repo doesn't exist yet";
                // logging will say so too.
                std::snprintf(update_pill, sizeof(update_pill),
                              "  Update check failed  ");
                update_col      = ImVec4(0.95f, 0.40f, 0.40f, 0.85f);
                show_update_pill = true;
                break;
            default:
                show_update_pill = false;
                break;
        }

        char discord_pill[64];
        if (discord_signed_in_) {
            std::snprintf(discord_pill, sizeof(discord_pill), "  Discord: %s  ",
                          discord_nick_.empty() ? "signed in" : discord_nick_.c_str());
        } else {
            std::snprintf(discord_pill, sizeof(discord_pill), "  Sign in with Discord  ");
        }

        // Phase 4: spec hub-relay status. Only shown when a relay ring
        // is active (mode flipped on for this session). Compact format
        // so it fits in the menu bar alongside the existing pills:
        //   RELAY out:E/D in:E/D
        //   where E = total enqueued (k-suffixed when > 999), D = total
        //   dropped (red if non-zero). Drops are what testers watch
        //   for; they indicate a ring overflow or oversize payload.
        char relay_pill[80] = {};
        bool show_relay_pill = false;
        ImVec4 relay_col(0.55f, 0.85f, 0.55f, 1.0f);  // muted green = healthy
        if (spec_relay_status_.out_active || spec_relay_status_.in_active) {
            show_relay_pill = true;
            auto fmt_k = [](uint64_t v, char* buf, size_t n) {
                if (v >= 10000) std::snprintf(buf, n, "%llu.%lluk",
                    (unsigned long long)(v / 1000),
                    (unsigned long long)((v / 100) % 10));
                else std::snprintf(buf, n, "%llu", (unsigned long long)v);
            };
            char oe[16], od[16], ie[16], id[16];
            fmt_k(spec_relay_status_.out_enqueued, oe, sizeof(oe));
            fmt_k(spec_relay_status_.out_dropped,  od, sizeof(od));
            fmt_k(spec_relay_status_.in_enqueued,  ie, sizeof(ie));
            fmt_k(spec_relay_status_.in_dropped,   id, sizeof(id));
            std::snprintf(relay_pill, sizeof(relay_pill),
                "  RELAY out:%s/%s in:%s/%s  ", oe, od, ie, id);
            // Any drops flip color to red. Even one drop in a real
            // match means snapshot/event corruption -- worth investigating
            // immediately.
            if (spec_relay_status_.out_dropped > 0 ||
                spec_relay_status_.in_dropped  > 0) {
                relay_col = ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
            }
        }

        const float discord_w = ImGui::CalcTextSize(discord_pill).x +
                                ImGui::GetStyle().ItemSpacing.x * 2.0f;
        const float update_w  = show_update_pill
            ? ImGui::CalcTextSize(update_pill).x +
              ImGui::GetStyle().ItemSpacing.x * 2.0f
            : 0.0f;
        const float relay_w   = show_relay_pill
            ? ImGui::CalcTextSize(relay_pill).x +
              ImGui::GetStyle().ItemSpacing.x * 2.0f
            : 0.0f;
        const float total_w   = discord_w + update_w + relay_w;
        const float bar_w     = ImGui::GetContentRegionAvail().x;
        if (total_w < bar_w) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (bar_w - total_w));
        }

        // RELAY pill — informational; clicking does nothing (no
        // associated action). MenuItem is the cheapest readonly text
        // element that respects the menu-bar style.
        if (show_relay_pill) {
            ImGui::PushStyleColor(ImGuiCol_Text, relay_col);
            ImGui::MenuItem(relay_pill, nullptr, false, false);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Spec hub-relay status (Phase 4 surface).\n"
                    "out: hook -> launcher -> hub (host shipping spec data)\n"
                    "in:  hub -> launcher -> hook (spec receiving data)\n"
                    "Format: enqueued/dropped. Red = drops happened "
                    "(snapshot or event corruption -- investigate).");
            }
            ImGui::PopStyleColor();
        }

        // Update pill (left of the Discord one) — clickable, advances
        // through the state machine: UpdateAvailable → start download
        // → Ready → spawn FM2KUpdater.exe and exit.
        if (show_update_pill) {
            ImGui::PushStyleColor(ImGuiCol_Text, update_col);
            if (ImGui::MenuItem(update_pill)) {
                switch (upd.state) {
                    case fm2k::updater::State::UpdateAvailable:
                        fm2k::updater::StartDownload();
                        break;
                    case fm2k::updater::State::Ready:
                        fm2k::updater::ApplyUpdateAndExit();
                        break;
                    case fm2k::updater::State::Failed:
                        fm2k::updater::CheckForUpdates();
                        break;
                    default:
                        break;
                }
            }
            // Tooltip carries the actual error / status detail. Pill
            // text is intentionally short so it fits in the menu bar;
            // hovering reveals the diagnostic.
            if (ImGui::IsItemHovered()) {
                if (upd.state == fm2k::updater::State::Failed) {
                    ImGui::SetTooltip("%s\nClick to retry.",
                        upd.error_detail.empty()
                            ? "Update check failed."
                            : upd.error_detail.c_str());
                } else if (upd.state == fm2k::updater::State::UpdateAvailable) {
                    ImGui::SetTooltip("Click to download v%s.",
                        upd.remote_version.c_str());
                } else if (upd.state == fm2k::updater::State::Ready) {
                    ImGui::SetTooltip("Click to apply v%s — launcher will restart.",
                        upd.remote_version.c_str());
                } else if (upd.state == fm2k::updater::State::Downloading) {
                    if (upd.total_bytes > 0) {
                        ImGui::SetTooltip("Downloading %u / %u bytes.",
                            (unsigned)upd.downloaded_bytes,
                            (unsigned)upd.total_bytes);
                    } else {
                        ImGui::SetTooltip("Downloading %u bytes.",
                            (unsigned)upd.downloaded_bytes);
                    }
                }
            }
            ImGui::PopStyleColor();
        }

        // Discord pill (right edge).
        ImVec4 col;
        if (discord_signed_in_) {
            col = ImVec4(0.3f, 0.85f, 0.45f, 1.0f);  // green = good to go
        } else {
            const float t = (float)ImGui::GetTime();
            const float a = 0.55f + 0.35f * (float)std::sin(t * 3.0f);
            col = ImVec4(0.95f, 0.65f, 0.20f, a);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        if (ImGui::MenuItem(discord_pill)) {
            show_discord_auth_ = !show_discord_auth_;
        }
        ImGui::PopStyleColor();
        ImGui::EndMainMenuBar();
    }

    // Input-binder windows. RenderWindow returns true on any change → autosave.
    if (show_input_binder_p1_) {
        if (FM2KInputBinder::RenderWindow(0, &show_input_binder_p1_)) {
            FM2KInputBinder::Save();
        }
    }
    if (show_input_binder_p2_) {
        if (FM2KInputBinder::RenderWindow(1, &show_input_binder_p2_)) {
            FM2KInputBinder::Save();
        }
    }
    // Single tabbed Settings window (replaces the five separate
    // floating settings sub-windows).
    RenderSettingsWindow();
    // Discord auth stays as its own window — OAuth pairing has its
    // own pending/error/success state machine that doesn't fit in
    // a tab next to the other static editors.
    if (show_discord_auth_) {
        RenderDiscordAuthWindow();
    }
    // Legacy floating-window paths kept for any code that still
    // toggles the per-section flags (none after the menu cleanup,
    // but defensive — opens nothing unless someone flips a flag).
    if (show_host_config_)    RenderHostConfigWindow();
    if (show_hub_server_)     RenderHubServerWindow();
    if (show_games_folders_)  RenderGamesFoldersWindow();
    if (show_recent_matches_) RenderRecentMatchesWindow();
    if (show_replay_browser_) {
        if (ImGui::Begin("Replays", &show_replay_browser_)) {
            RenderReplayBrowser();
        }
        ImGui::End();
    }
}

