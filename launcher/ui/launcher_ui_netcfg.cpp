// launcher_ui_netcfg.cpp -- LauncherUI network config + connection status + session controls. Split from FM2K_LauncherUI.cpp (pure move).
#include "FM2K_Integration.h"
#include "launcher_ui_hubstate.h"  // LauncherUI::HubState full def
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

void LauncherUI::RenderNetworkConfig() {
    if (ImGui::CollapsingHeader(T("panel_network_config"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();

        // Session Type (Host/Join)
        static int session_type = 0; // 0: Host, 1: Join
        ImGui::RadioButton(T("netcfg_host"), &session_type, 0); ImGui::SameLine();
        ImGui::RadioButton(T("netcfg_join"), &session_type, 1);

        network_config_.is_host = (session_type == 0);

        if (network_config_.is_host) {
            // HOST: Show ip:port and copy to clipboard on click
            // Get actual local IP from first non-loopback adapter
            // Get external IP via HTTP (same approach as CCCaster).
            // Queries checkip.amazonaws.com which returns just the IP as text.
            static char local_ip[64] = "Resolving...";
            static bool ip_resolved = false;
            if (!ip_resolved) {
                ip_resolved = true;
                HINTERNET hInternet = InternetOpenA("FM2K", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
                if (hInternet) {
                    // Try multiple services like CCCaster does
                    const char* services[] = {
                        "http://checkip.amazonaws.com",
                        "http://ipv4.icanhazip.com",
                        "http://ifcfg.net",
                    };
                    for (const char* url : services) {
                        HINTERNET hUrl = InternetOpenUrlA(hInternet, url, NULL, 0,
                            INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD, 0);
                        if (hUrl) {
                            char buf[256] = {};
                            DWORD bytesRead = 0;
                            if (InternetReadFile(hUrl, buf, sizeof(buf) - 1, &bytesRead) && bytesRead >= 7) {
                                buf[bytesRead] = '\0';
                                // Trim whitespace/newlines
                                char* p = buf;
                                while (*p == ' ' || *p == '\r' || *p == '\n') p++;
                                char* end = p + strlen(p) - 1;
                                while (end > p && (*end == ' ' || *end == '\r' || *end == '\n')) *end-- = '\0';
                                strncpy(local_ip, p, sizeof(local_ip) - 1);
                                InternetCloseHandle(hUrl);
                                break;
                            }
                            InternetCloseHandle(hUrl);
                        }
                    }
                    InternetCloseHandle(hInternet);
                }
                if (strcmp(local_ip, "Resolving...") == 0) {
                    strncpy(local_ip, "Could not resolve", sizeof(local_ip));
                }
            }
            char address_with_port[128];
            snprintf(address_with_port, sizeof(address_with_port), "%s:%d", local_ip, network_config_.local_port);

            ImGui::Text("%s", T("label_your_address"));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", address_with_port);
            ImGui::SameLine();
            if (ImGui::Button(T("btn_copy"))) {
                SDL_SetClipboardText(address_with_port);
            }

            // Port (editable for host)
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("Port", &network_config_.local_port, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        } else {
            // JOIN: Single paste field for ip:port
            static char paste_buf[128] = "";
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputTextWithHint("##join_addr", T("label_paste_host_addr"), paste_buf, sizeof(paste_buf))) {
                network_config_.remote_address = paste_buf;
            }
            ImGui::SameLine();
            if (ImGui::Button(T("btn_paste"))) {
                const char* clipboard = SDL_GetClipboardText();
                if (clipboard && clipboard[0]) {
                    strncpy(paste_buf, clipboard, sizeof(paste_buf) - 1);
                    paste_buf[sizeof(paste_buf) - 1] = '\0';
                    network_config_.remote_address = paste_buf;
                }
            }

            // Local port (editable for client — required to avoid collisions when both peers run on localhost)
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("Local Port", &network_config_.local_port, 0, 0, ImGuiInputTextFlags_CharsDecimal);
        }

        // Input Delay
        ImGui::SetNextItemWidth(100);
        ImGui::SliderInt("Input Delay (frames)", &network_config_.input_delay, 0, 10);

        ImGui::Unindent();
    }
}

void LauncherUI::RenderConnectionStatus() {
    // ImGui popup IDs are hashed from the title string. Keep the ID stable
    // across language switches by appending a `##` suffix — text after `##`
    // is treated as ID-only and never displayed. This way the visible
    // title localizes but the popup keeps its identity if someone changes
    // language while a popup is open.
    if (launcher_state_ == LauncherState::Connecting) {
        ImGui::OpenPopup("##connecting_modal");
    }

    if (ImGui::BeginPopupModal("##connecting_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", T("modal_connecting_body"));
        if (launcher_state_ != LauncherState::Connecting) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (launcher_state_ == LauncherState::Disconnected) {
        ImGui::OpenPopup("##disconnected_modal");
    }

    if (ImGui::BeginPopupModal("##disconnected_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", T("hub_status_connection_lost"));
        if (ImGui::Button(T("btn_ok"))) {
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

void LauncherUI::ShowNetworkDiagnostics() {}

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

void LauncherUI::RenderSessionControls() {
    if (ImGui::CollapsingHeader(T("panel_session_controls"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();

        // Disable buttons if no game is selected
        bool game_selected = selected_game_index_ >= 0 && selected_game_index_ < games_.size();
        if (!game_selected) {
            ImGui::BeginDisabled();
        }

        // Boot/auto-mash defaults — applied to every offline launch, both
        // dev and end-user paths. End users never see these toggles.
        static int  s_boot_strategy     = 0;     // 0=safe, 1=fast
        static bool s_auto_title_skip   = true;
        static bool s_bypass_trampoline = false;
        static bool s_force_t4_patch    = false;
        static bool s_skip_vs_mode_patch= false;
        static bool s_t4_probe          = false;
        // FM95-specific opt-in: drive the trampoline tick from
        // Hook_UpdateGameState instead of the (skipped) RUN_GAME_LOOP
        // detour, so FM95 reaches rollback parity with FM2K.
        static bool s_fm95_trampoline   = false;
        // Persisted across launcher restarts via %APPDATA%\FM2K_Rollback\dev_flags.ini.
        // First-frame init reads the saved value; toggling the checkbox writes back.
        static bool s_eb_diag = []() {
            bool v = LoadDevFlag("eb_diag", false);
            // Apply immediately on launcher start so any auto-launched session
            // (offline / online / hub) inherits the env var.
            ::SetEnvironmentVariableA("FM2K_EB_DIAG", v ? "1" : nullptr);
            return v;
        }();
        // Fast .player loader — collapses ~30ms of per-sound ReadFile syscall
        // overhead per CSS-cursor flick into one big slurp + RAM memcpy.
        // OFF by default until validated; flipping it persists + applies to
        // every launch path.
        static bool s_fast_player_load = []() {
            bool v = LoadDevFlag("fast_player_load", false);
            ::SetEnvironmentVariableA("FM2K_FAST_PLAYER_LOAD",
                                      v ? "1" : nullptr);
            return v;
        }();
        // FM2K_DEV_MODE master flag for experimental hook features that
        // aren't ready for default-on yet (current users: spectator-side
        // .player OS-cache pre-warmer in the trampoline). Off by default;
        // flipping persists + applies to every launch path.
        static bool s_dev_mode = []() {
            bool v = LoadDevFlag("dev_mode", false);
            ::SetEnvironmentVariableA("FM2K_DEV_MODE", v ? "1" : nullptr);
            return v;
        }();
        // FM2K_BOOT_TO_BATTLE dev shortcut: append /F to the game's
        // cmdline (engine's built-in debug-boot path) and prime
        // g_iniFile_nameOverride from the hook so its kgt loader works
        // on shipped binaries. Skips splash/title/CSS entirely; uses
        // the kgt's TestPlay-section preset chars for the matchup.
        // VS 1v1 only — engine hardcodes g_game_mode_flag=1 in that
        // branch. Off by default; persists across launcher restarts.
        static bool s_dev_boot_to_battle = []() {
            bool v = LoadDevFlag("boot_to_battle", false);
            ::SetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE",
                                      v ? "1" : nullptr);
            return v;
        }();
        // Auto-upload crash + desync diagnostics to the hub. Default ON
        // now that the META is PII-scrubbed before transmit (fm2k::pii::Scrub
        // in FM2K_UploadQueue) and log contents are scrubbed at write time —
        // so we actually get crash telemetry from the playerbase instead of
        // chasing manual log sends. Users can still opt OUT via the dev
        // checkbox (the saved flag overrides this default once they touch it).
        // No env var: the launcher reads g_auto_upload_logs directly
        // in PollUploadQueue and the hook side doesn't need to know.
        // File-scope (not function-static) so PollUploadQueue can read.
        static bool s_auto_upload_logs_loaded = []() {
            g_auto_upload_logs = LoadDevFlag("auto_upload_logs", true);
            return true;
        }();
        (void)s_auto_upload_logs_loaded;
        bool& s_dev_auto_upload_logs = g_auto_upload_logs;
        // Boot-to-battle char/stage/meter overrides. Written into the
        // engine's g_config_value1/3 (chars), wParam (stage), and
        // g_config_value2/4 (meter init) by the hook's MinHook detour on
        // InitializeGameFromCommandLine. Persisted across launcher runs
        // so quick-iteration testing keeps the last matchup.
        static int s_btb_p1_char  = LoadDevFlagInt("btb_p1_char",  0);
        static int s_btb_p2_char  = LoadDevFlagInt("btb_p2_char",  0);
        static int s_btb_stage    = LoadDevFlagInt("btb_stage",    0);
        static int s_btb_p1_meter = LoadDevFlagInt("btb_p1_meter", 0);
        static int s_btb_p2_meter = LoadDevFlagInt("btb_p2_meter", 0);
        // gs_pic_fix has been migrated to per-game settings (see
        // LoadGamePatchBool / ApplyGamePatchEnvVars); host-config panel
        // owns the toggle and writes to %APPDATA%\FM2K_Rollback\
        // game_patches\<game_id>.ini.

        // ---------- USER-FACING SECTION ----------
        ImGui::Text("%s", T("label_play"));
        ImGui::Separator();

        if (ImGui::Button(T("btn_play_online"), ImVec2(-1, 0))) {
            // Move the user to the Hub panel and, if they've already
            // got a game selected and a hub connection, drop them into
            // a per-game lobby on demand. Hub creates rooms lazily on
            // join_room — no master room list needed; the room id is
            // the exe stem so two players on the same game converge.
            ImGui::SetWindowFocus("Hub");
            const bool game_selected =
                selected_game_index_ >= 0 &&
                selected_game_index_ < (int)games_.size();
            if (game_selected && hub_state_ && hub_state_->client.IsConnected()) {
                std::filesystem::path exe(
                    fm2k::utf8path::Utf8ToWide(games_[selected_game_index_].exe_path));
                const std::string game_id = fm2k::utf8path::StemUtf8(exe);
                if (hub_state_->current_room_id != game_id) {
                    hub_state_->client.JoinRoom(game_id, game_id);
                }
            }
        }
        ImGui::SetItemTooltip(
            "Switch to the Hub panel. If a game is selected and you're "
            "connected, joins (or creates) a lobby for that game.");

        if (ImGui::Button(T("btn_play_offline"), ImVec2(-1, 0))) {
            ::SetEnvironmentVariableA("FM2K_BOOT_TO_CSS_DIRECT",
                                      s_boot_strategy == 1 ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE",
                                      s_dev_boot_to_battle ? "1" : nullptr);
            // Boot-to-battle char/stage/meter overrides. Only export
            // when the checkbox is on; hook ignores unset env vars and
            // keeps the kgt-default value.
            {
                auto put_int = [](const char* name, int v) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", v);
                    ::SetEnvironmentVariableA(name, buf);
                };
                if (s_dev_boot_to_battle) {
                    put_int("FM2K_BTB_P1_CHAR",  s_btb_p1_char);
                    put_int("FM2K_BTB_P2_CHAR",  s_btb_p2_char);
                    put_int("FM2K_BTB_STAGE",    s_btb_stage);
                    put_int("FM2K_BTB_P1_METER", s_btb_p1_meter);
                    put_int("FM2K_BTB_P2_METER", s_btb_p2_meter);
                } else {
                    ::SetEnvironmentVariableA("FM2K_BTB_P1_CHAR",  nullptr);
                    ::SetEnvironmentVariableA("FM2K_BTB_P2_CHAR",  nullptr);
                    ::SetEnvironmentVariableA("FM2K_BTB_STAGE",    nullptr);
                    ::SetEnvironmentVariableA("FM2K_BTB_P1_METER", nullptr);
                    ::SetEnvironmentVariableA("FM2K_BTB_P2_METER", nullptr);
                }
            }
            ::SetEnvironmentVariableA("FM2K_AUTO_TITLE_SKIP",
                                      s_auto_title_skip ? nullptr : "0");
            ::SetEnvironmentVariableA("FM2K_BYPASS_TRAMPOLINE",
                                      s_bypass_trampoline ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM2K_FORCE_T4_PATCH",
                                      s_force_t4_patch ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM2K_SKIP_VS_MODE_PATCH",
                                      s_skip_vs_mode_patch ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM2K_T4_PROBE",
                                      s_t4_probe ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM2K_EB_DIAG",
                                      s_eb_diag ? "1" : nullptr);
            ::SetEnvironmentVariableA("FM95_TRAMPOLINE",
                                      s_fm95_trampoline ? "1" : nullptr);
            // Per-game patches override global flags for the selected game.
            // Each per-game INI key gets a matching env var; hook DLL reads
            // them at DLL_PROCESS_ATTACH.
            if (selected_game_index_ >= 0 &&
                selected_game_index_ < (int)games_.size()) {
                ApplyGamePatchEnvVars(GameIdForExePath(
                    games_[selected_game_index_].exe_path));
            }
            if (on_offline_session_start) {
                on_offline_session_start();
            }
        }
        ImGui::SetItemTooltip("%s", T("btn_play_offline_tooltip"));

        // ---------- DEVELOPER SECTION ----------
        if (developer_mode_) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", T("dev_section_header"));
            ImGui::Separator();

            ImGui::Text("%s", T("dev_boot_strategy"));
            ImGui::RadioButton(T("dev_boot_safe"), &s_boot_strategy, 0);
            ImGui::SetItemTooltip(
                "Boots to title_screen_manager (skips intro cutscene). The "
                "hook auto-mashes button A with cursor pre-set to VS Player. "
                "Works on every game — adds ~10 frames to boot.");
            ImGui::RadioButton(T("dev_boot_fast"), &s_boot_strategy, 1);
            ImGui::SetItemTooltip(
                "Skips title screen entirely. WORKS on WW. BREAKS StudioS "
                "Fighters / Strip Fighter Zero — characters self-damage on "
                "frame 0. Only enable per-game once verified safe.");

            ImGui::Checkbox(T("dev_auto_title_skip"), &s_auto_title_skip);
            ImGui::SetItemTooltip(
                "Default ON. Disable to walk title screen manually with your "
                "own inputs.");

            // Engine's built-in /F debug-boot path. Bypasses the
            // boot_strategy radio above (which fights the title-screen
            // state machine post-launch) by going through the slot-0
            // boot dispatcher's debug_mode==3 branch directly. Hook
            // primes g_iniFile_nameOverride @ 0x43012c at dispatcher
            // entry so the kgt loader finds <exe_basename>.kgt.
            if (ImGui::Checkbox("Boot straight to battle (/F, dev)",
                                &s_dev_boot_to_battle)) {
                ::SetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE",
                                          s_dev_boot_to_battle ? "1" : nullptr);
                SaveDevFlag("boot_to_battle", s_dev_boot_to_battle);
            }
            ImGui::SetItemTooltip(
                "Append /F to the game's command line. The engine's WinMain "
                "sets g_debug_mode=3 and its slot-0 boot dispatcher creates "
                "a battle-init object instead of splash/title/CSS — no hook-"
                "side input mashing involved.\n\n"
                "VS 1v1 only (engine hardcodes the mode flag in the /F "
                "branch). Use the char/stage inputs below to pick the "
                "matchup; values map directly to the CSS grid index.\n\n"
                "Needs <exe_basename>.kgt sitting next to the .exe (the "
                "standard layout — works on WonderfulWorld, vanpri, etc.).");

            // Char / stage / meter inputs — only meaningful when the
            // checkbox above is on. Hook ignores the env vars if BTB is
            // off, but we hide the controls to reduce visual noise.
            if (s_dev_boot_to_battle) {
                ImGui::Indent();
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputInt("P1 char##btb", &s_btb_p1_char)) {
                    if (s_btb_p1_char < 0)  s_btb_p1_char = 0;
                    if (s_btb_p1_char > 49) s_btb_p1_char = 49;
                    SaveDevFlagInt("btb_p1_char", s_btb_p1_char);
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputInt("P2 char##btb", &s_btb_p2_char)) {
                    if (s_btb_p2_char < 0)  s_btb_p2_char = 0;
                    if (s_btb_p2_char > 49) s_btb_p2_char = 49;
                    SaveDevFlagInt("btb_p2_char", s_btb_p2_char);
                }
                ImGui::SetItemTooltip(
                    "CSS grid index (0-49). Char 0 is whichever character "
                    "occupies the top-left cell of the kgt's character grid.");

                ImGui::SetNextItemWidth(80);
                if (ImGui::InputInt("Stage##btb", &s_btb_stage)) {
                    if (s_btb_stage < 0)  s_btb_stage = 0;
                    if (s_btb_stage > 49) s_btb_stage = 49;
                    SaveDevFlagInt("btb_stage", s_btb_stage);
                }
                ImGui::SetItemTooltip(
                    "Stage index (0-49). Maps to wParam/g_fm2k_game_mode — "
                    "vs_round_function reads it on battle init.");

                ImGui::SetNextItemWidth(80);
                if (ImGui::InputInt("P1 meter##btb", &s_btb_p1_meter)) {
                    SaveDevFlagInt("btb_p1_meter", s_btb_p1_meter);
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputInt("P2 meter##btb", &s_btb_p2_meter)) {
                    SaveDevFlagInt("btb_p2_meter", s_btb_p2_meter);
                }
                ImGui::SetItemTooltip(
                    "Super meter starting value. 0 = empty (vanilla); the "
                    "engine treats non-zero as a pre-charged init.");
                ImGui::Unindent();
            }

            ImGui::Spacing();

            // FM2K_DEV_MODE master flag — gates experimental hook features
            // that aren't ready for production default-on. Current users:
            //   * .player OS-cache pre-warmer in the spectator trampoline
            //     (CSS replay performance).
            // Persists across launcher restarts via dev_flags.ini, applies
            // to all launch paths through the env var.
            if (ImGui::Checkbox("FM2K dev mode (experimental hooks)", &s_dev_mode)) {
                ::SetEnvironmentVariableA("FM2K_DEV_MODE",
                                          s_dev_mode ? "1" : nullptr);
                SaveDevFlag("dev_mode", s_dev_mode);
            }
            ImGui::SetItemTooltip(
                "Enables experimental hook-side optimisations that haven't "
                "been validated as production-default yet. Currently:\n"
                "  - Spectator .player OS-cache pre-warmer (faster CSS replay "
                "after the spectator's first session start).\n"
                "Safe to leave off; required if you're testing spectator perf.");

            // Auto-upload crash + desync diagnostic bundles to the hub
            // so we can pull them down for debugging. OFF by default
            // (user must explicitly opt in — game logs are sensitive
            // even with PII-scrubbed IP addresses).
            const bool secret_baked =
                fm2k::kLogUploadSecret && fm2k::kLogUploadSecret[0] != '\0';
            ImGui::BeginDisabled(!secret_baked);
            if (ImGui::Checkbox("Auto-upload crash/desync diagnostics",
                                &s_dev_auto_upload_logs)) {
                SaveDevFlag("auto_upload_logs", s_dev_auto_upload_logs);
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(
                "On crash or desync, the hook drops a manifest in the "
                "game's upload_queue/ directory. The launcher then POSTs "
                "the bundle (debug.log tail + desync diff + RNG trace) to "
                "%s.\n\n"
                "%s",
                fm2k::kLogUploadUrl,
                secret_baked
                    ? "Logs are tagged with session_id + match_id + "
                      "client_version + game_id + hook-DLL SHA1, indexed "
                      "for pull-down by the dev tool."
                    : "Disabled: this build wasn't compiled with a "
                      "FM2K_LOG_UPLOAD_SECRET — feature would have no "
                      "endpoint to authenticate against.");

            ImGui::Spacing();

            // ---------- FM2K diagnostics (collapsed by default) ----------
            // FM2K-engine-specific toggles — most users never touch these
            // outside of debugging desync repros.
            if (ImGui::CollapsingHeader("FM2K diagnostics")) {
                ImGui::Indent();

                ImGui::Checkbox(T("dev_bypass_trampoline"), &s_bypass_trampoline);
                ImGui::SetItemTooltip(
                    "Routes Hook_RunGameLoop to vanilla. Other hooks still fire. "
                    "Offline only — netplay/spectator require the trampoline.");

                ImGui::Checkbox(T("dev_skip_vs_mode_patch"), &s_skip_vs_mode_patch);
                ImGui::SetItemTooltip("%s", T("dev_skip_vs_mode_tooltip"));

                ImGui::Checkbox(T("dev_force_t4_patch"), &s_force_t4_patch);
                ImGui::SetItemTooltip(
                    "Re-enables the case-200 t4-walk neuter patch (0x408EC5).");

                ImGui::Checkbox(T("dev_t4_probe"), &s_t4_probe);
                ImGui::SetItemTooltip("%s", T("dev_t4_probe_tooltip"));

                if (ImGui::Checkbox(T("dev_eb_diag"), &s_eb_diag)) {
                    // Apply immediately so EVERY launch path (offline, online,
                    // hub, dual-clients, spectator) inherits the env var.
                    // Persist to dev_flags.ini so the toggle survives launcher
                    // restarts — otherwise the static-bool default loses your
                    // setting every time you close the launcher.
                    ::SetEnvironmentVariableA("FM2K_EB_DIAG",
                                              s_eb_diag ? "1" : nullptr);
                    SaveDevFlag("eb_diag", s_eb_diag);
                }
                ImGui::SetItemTooltip(
                    "Logs shake-effect timer values at PRE-SAVE / PRE-RENDER / "
                    "POST-RENDER / POST-RESTORE around the trampoline render "
                    "boundary. Use to track [EB] palette-flash and screen-shake "
                    "duration loss. Output goes to FM2K_eb_diag_pid<PID>.log "
                    "in the game folder (NOT the main launcher log). Repro: "
                    "pkmncc Bewear 624B, slither wing 6A landing, URORFG Loader "
                    "5B / walking, Breloom 6a6a6b. Persists across launcher "
                    "restarts.");

                if (ImGui::Checkbox("Fast .player load (FM2K_FAST_PLAYER_LOAD)",
                                    &s_fast_player_load)) {
                    ::SetEnvironmentVariableA("FM2K_FAST_PLAYER_LOAD",
                                              s_fast_player_load ? "1" : nullptr);
                    SaveDevFlag("fast_player_load", s_fast_player_load);
                }
                ImGui::SetItemTooltip(
                    "Slurp full .player file content on open + serve "
                    "subsequent ReadFile/SetFilePointer calls from RAM. "
                    "Collapses the ~200 syscalls FM2K's character_data_loader "
                    "issues per character (1 per sound × 100-150 sounds × "
                    "~150µs kernel ping-pong) into one big read + memcpy. "
                    "Net: CSS cursor-flick stall drops from 30-60ms to ~5ms "
                    "cold (NVMe disk read only) or <1ms warm (OS page cache). "
                    "Hooks ReadFile/SetFilePointer/SetFilePointerEx/CloseHandle "
                    "globally; non-.player handles fall through unchanged. "
                    "Off by default until validated. Restart the game after "
                    "toggling — env var is read at hook init.");

                ImGui::Unindent();
            }

            // (Experimental patches moved to Host Config panel — they're
            // per-game now, stored in %APPDATA%\FM2K_Rollback\game_patches\
            // <game_id>.ini, edited via the Host Config tab when a game
            // is selected.)

            // ---------- FM95 / CPW (collapsed by default) ----------
            // Engine-specific to FM95Hook.dll-injected games. Won't fire
            // on FM2K builds — environment vars get set anyway, and
            // FM2KHook just ignores them.
            if (ImGui::CollapsingHeader("FM95 (CPW etc.)")) {
                ImGui::Indent();

                ImGui::Checkbox("Trampoline-driven loop (FM95_TRAMPOLINE)",
                                &s_fm95_trampoline);
                ImGui::SetItemTooltip(
                    "FM95's RUN_GAME_LOOP is _WinMain (no separate driver), so the "
                    "trampoline can't replace it like on FM2K. With this enabled, "
                    "Hook_UpdateGameState calls TrampolineFrameTick() and Hook_"
                    "RenderGame skips the host's natural render — the trampoline's "
                    "RenderFrameWithSnapshot drives one render per frame. Required "
                    "for FM95 rollback parity. OFF = current working baseline (no "
                    "rollback driver, host runs CPW natively). Toggle off if you "
                    "see regressions.");

                ImGui::Unindent();
            }

            ImGui::Spacing();
            if (ImGui::Button(T("dev_online_legacy"), ImVec2(-1, 0))) {
                if (on_online_session_start) {
                    on_online_session_start(network_config_);
                }
            }
            ImGui::SetItemTooltip("%s", T("dev_online_legacy_tip"));

            if (ImGui::Button(T("dev_stress_test"), ImVec2(-1, 0))) {
                if (on_stress_session_start) {
                    on_stress_session_start();
                }
            }
            ImGui::SetItemTooltip(
                "GekkoStressSession with a single instance. Forces rollback every 10 frames "
                "and compares save hashes — any DESYNC = local determinism bug.");

            ImGui::Spacing();
            ImGui::Text("%s", T("dev_local_testing"));
            ImGui::Separator();
        
        // Get client status for dual client button
        uint32_t client1_pid = 0, client2_pid = 0;
        bool clients_running = false;
        if (on_get_client_status) {
            clients_running = on_get_client_status(client1_pid, client2_pid);
        }
        
        if (clients_running) {
            ImGui::BeginDisabled();
        }
        
        if (ImGui::Button(T("dev_launch_dual"), ImVec2(-1, 0))) {
            if (on_launch_local_client1 && on_launch_local_client2 && game_selected) {
                const auto& selected_game = games_[selected_game_index_];

                // Both clients launch from the same folder. The hook's
                // BypassMultiInstanceCheck patch disables FM2K's own
                // FindWindow("KGT2KGAME") guard, and shared memory is
                // PID-namespaced. Mutable file collisions (.ini, save data)
                // are tolerable for testing; if they become a problem,
                // revisit with per-instance shadow folders.
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Starting dual clients...");
                bool success1 = on_launch_local_client1(selected_game.exe_path);
                if (success1) {
                    bool success2 = on_launch_local_client2(selected_game.exe_path);
                    if (!success2) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch Client 2");
                    }
                }
            }
        }
        
        if (clients_running) {
            ImGui::EndDisabled();
        }

        ImGui::SetItemTooltip("%s", T("dev_launch_dual_tip"));

        // "Launch Spectator" — spawns a third local instance that subscribes
        // to client1 (host on 7000) for replay-streamed spectating. Only
        // makes sense after Launch Dual Clients has the host running.
        bool can_spectate = on_launch_local_spectator && game_selected && client1_pid != 0;
        if (!can_spectate) ImGui::BeginDisabled();
        if (ImGui::Button(T("dev_launch_spectator"), ImVec2(-1, 0))) {
            if (can_spectate) {
                const auto& selected_game = games_[selected_game_index_];
                bool ok = on_launch_local_spectator(selected_game.exe_path);
                if (!ok) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch spectator");
                }
            }
        }
        if (!can_spectate) ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", T("dev_launch_spectator_tip"));

        // "Launch Spectator 2 (chain)" — daisy-chain test. Subscribes to
        // spectator 1 (port 7002) instead of the host. Validates that
        // spectator 1 correctly relays its received frames to its own
        // subscribers. Disabled until both dual clients + spectator 1 are
        // running.
        bool can_spectate2 = on_launch_local_spectator2 && game_selected && client1_pid != 0;
        if (!can_spectate2) ImGui::BeginDisabled();
        if (ImGui::Button(T("dev_launch_spectator2"), ImVec2(-1, 0))) {
            if (can_spectate2) {
                const auto& selected_game = games_[selected_game_index_];
                bool ok = on_launch_local_spectator2(selected_game.exe_path);
                if (!ok) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch spectator 2");
                }
            }
        }
        if (!can_spectate2) ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", T("dev_launch_spectator2_tip"));

        if (clients_running) {
            ImGui::Text(T("clients_running"), client1_pid, client2_pid);
            if (ImGui::Button(T("dev_terminate_clients"), ImVec2(-1, 0))) {
                if (on_terminate_all_clients) {
                    on_terminate_all_clients();
                }
            }
        }

        // Hub-free Spectate-by-IP. Sits at the bottom of the dev section
        // so it's grouped with the other "test the netcode directly"
        // controls. Cross-Patreon-tier viewing — patron specs a non-
        // patron friend, or two non-patrons in dev mode. Requires host
        // to share their public addr OOB (Discord etc).
        ImGui::Spacing();
        RenderDirectSpecInline();
        }  // end if (developer_mode_)

        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::Button(T("btn_stop_session"), ImVec2(-1, 0))) {
            if (on_session_stop) {
                on_session_stop();
            }
        }
        ImGui::SetItemTooltip("%s", T("btn_stop_session_tooltip"));

        if (!game_selected) {
            ImGui::EndDisabled();
        }

        ImGui::Unindent();
    }
}

