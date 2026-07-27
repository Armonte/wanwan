// launcher_ui_input.cpp -- LauncherUI input-bindings tab + SOCD/random-stage state. Split from FM2K_LauncherUI.cpp (pure move).
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
#include <cstdio>   // std::snprintf for the random-stage env publish
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

void LauncherUI::LoadSocdState() {
    const std::string path = NotifySettingsPath();   // shared settings.ini
    if (path.empty()) return;
    socd_mode_[0] = ReadIntSetting(path, "socd_mode_p1", 1);  // 1 = Hitbox SOCD
    socd_mode_[1] = ReadIntSetting(path, "socd_mode_p2", 1);
    // Clamp to known range so a hand-edited bad value can't blow up
    // the hook's switch statement.
    for (int i = 0; i < 2; ++i) {
        if (socd_mode_[i] < 0 || socd_mode_[i] > 5) socd_mode_[i] = 1;
    }
    // Publish the local slot's setting to the Win32 environment so
    // EVERY launch path (offline, dev dual-client P1, stress) inherits
    // it via CreateProcess. Online K::MatchStart re-applies the
    // role-resolved value on top. Pre-v0.2.45 only the online path
    // set this, so offline launches always used the compiled-in
    // default — bug Froglet reported.
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", socd_mode_[0]);
    ::SetEnvironmentVariableA("FM2K_SOCD_MODE", buf);
}

void LauncherUI::SaveSocdState() {
    const std::string path = NotifySettingsPath();
    if (path.empty()) return;
    WriteIntSetting(path, "socd_mode_p1", socd_mode_[0]);
    WriteIntSetting(path, "socd_mode_p2", socd_mode_[1]);
}

void LauncherUI::LoadRandomStageState() {
    // Per-game (Patrick, 2026-06-11: "stage range isn't game specific").
    // A range tuned for one game's stage list followed the user into
    // every other game; out-of-range indices make LoadStageFile throw a
    // modal "GameStage Open error" box mid-match. The legacy GLOBAL
    // settings.ini values are kept as first-load defaults so existing
    // users keep their range for the game they already play.
    int g_enable = 0, g_min = 0, g_max = 7;
    {
        const std::string path = NotifySettingsPath();
        if (!path.empty()) {
            g_enable = ReadIntSetting(path, "random_stage_enable", 0);
            g_min    = ReadIntSetting(path, "random_stage_min", 0);
            g_max    = ReadIntSetting(path, "random_stage_max", 7);
        }
    }
    std::string gid;
    if (selected_game_index_ >= 0 &&
        selected_game_index_ < (int)games_.size()) {
        gid = GameIdForExePath(games_[selected_game_index_].exe_path);
    }
    if (!gid.empty()) {
        random_stage_enable_ =
            LoadGamePatchBool(gid, "random_stage_enable", g_enable != 0);
        random_stage_min_ = LoadGamePatchInt(gid, "random_stage_min", g_min);
        random_stage_max_ = LoadGamePatchInt(gid, "random_stage_max", g_max);
    } else {
        random_stage_enable_ = (g_enable != 0);
        random_stage_min_    = g_min;
        random_stage_max_    = g_max;
    }
    if (random_stage_min_ < 0)   random_stage_min_ = 0;
    if (random_stage_max_ > 99)  random_stage_max_ = 99;
    if (random_stage_max_ < random_stage_min_) random_stage_max_ = random_stage_min_;

    PublishRandomStageEnv();
}

// Publish the random-stage settings to the Win32 environment so EVERY launch
// path (offline, practice, dev dual-client) inherits them via CreateProcess --
// the same fix SOCD got in v0.2.45. Before this, the vars were set in exactly
// one place (the hub match-start handler), so random stage only ever worked
// online; StartOfflineSession never set them and nothing rolled offline.
//
// ONLINE AUTHORITY IS PRESERVED, and depends on an invariant worth stating:
// the hub match-start handler sets all three vars when the host enables random
// stage and explicitly CLEARS all three when it doesn't. So for an online match
// the host's seed always overwrites whatever we publish here, and a host who
// disabled random stage clears it. Do not make that clear-path conditional.
//
// Offline gets a fresh per-launch seed -- a fixed one would make every offline
// session roll the identical stage sequence.
void LauncherUI::PublishRandomStageEnv() {
    if (!random_stage_enable_) {
        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_SEED", nullptr);
        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_MIN",  nullptr);
        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_MAX",  nullptr);
        return;
    }
    // Seed 0 means "disabled" to the hook, so never hand it one.
    unsigned seed = (unsigned)(::GetTickCount() ^ (::GetCurrentProcessId() << 16));
    if (seed == 0) seed = 1;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", seed);
    ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_SEED", buf);
    std::snprintf(buf, sizeof(buf), "%d", random_stage_min_);
    ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_MIN", buf);
    std::snprintf(buf, sizeof(buf), "%d", random_stage_max_);
    ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_MAX", buf);
}

void LauncherUI::SaveRandomStageState() {
    // Re-publish on every change so a toggle in the Host Config panel takes
    // effect on the next offline launch without a launcher restart.
    PublishRandomStageEnv();
    std::string gid;
    if (selected_game_index_ >= 0 &&
        selected_game_index_ < (int)games_.size()) {
        gid = GameIdForExePath(games_[selected_game_index_].exe_path);
    }
    if (!gid.empty()) {
        SaveGamePatchBool(gid, "random_stage_enable", random_stage_enable_);
        SaveGamePatchInt (gid, "random_stage_min",    random_stage_min_);
        SaveGamePatchInt (gid, "random_stage_max",    random_stage_max_);
        return;
    }
    // No game selected (shouldn't happen from the host UI) -- legacy global.
    const std::string path = NotifySettingsPath();
    if (path.empty()) return;
    WriteIntSetting(path, "random_stage_enable", random_stage_enable_ ? 1 : 0);
    WriteIntSetting(path, "random_stage_min",    random_stage_min_);
    WriteIntSetting(path, "random_stage_max",    random_stage_max_);
}

void LauncherUI::EnsureRandomStageLoaded() {
    if (random_state_loaded_for_ != selected_game_index_) {
        random_state_loaded_for_ = selected_game_index_;
        LoadRandomStageState();
    }
}

void LauncherUI::RenderInputBindingsTab(int player_slot) {
    if (player_slot < 0 || player_slot > 1) return;

    if (!socd_state_loaded_) {
        socd_state_loaded_ = true;
        LoadSocdState();
    }

    // SOCD picker — purely local. Each P1/P2 slot keeps its own mode
    // because dual-local dev mode runs both slots from one launcher
    // and wants each child process configured independently. Online
    // mode applies socd_mode_[g_player_index] to the spawned game's
    // FM2K_SOCD_MODE env var at launch.
    static const char* kSocdLabels[6] = {
        "0 — Default        (R wins L+R, U wins U+D)",
        "1 — Hitbox SOCD    (L+R neutral, U wins U+D)  [tournament default]",
        "2 — U/D Cancel     (R wins L+R, U+D neutral)",
        "3 — Both Cancel    (L+R neutral, U+D neutral)",
        "4 — Up Bias        (R wins L+R, U wins U+D)",
        "5 — Hitbox + UpBias",
    };
    ImGui::TextDisabled(
        "SOCD is local — applied before inputs hit the wire, so peers "
        "running different modes do NOT desync.");
    ImGui::SetNextItemWidth(380);
    char combo_id[32];
    std::snprintf(combo_id, sizeof(combo_id), "##socd_p%d", player_slot + 1);
    if (ImGui::Combo(combo_id, &socd_mode_[player_slot], kSocdLabels, 6)) {
        SaveSocdState();
        // Live-update the env so a freshly-spawned game picks up the
        // new mode; running games don't reload (hook caches on first
        // GetSOCDMode call) — they get the new value next launch.
        //
        // Bug Froglet reported: the previous code wrote
        // FM2K_SOCD_MODE_P1 / _P2 via _putenv_s, but
        //   (a) the hook reads FM2K_SOCD_MODE (no suffix), and
        //   (b) _putenv_s writes CRT env which is a DIFFERENT table
        //       from Win32 env, and CreateProcess only inherits Win32
        //       env into child processes.
        // So the setting silently never reached the spawned game and
        // every launch ran at the compiled-in default (mode 1).
        //
        // For player_slot==0 (local game) we publish to FM2K_SOCD_MODE
        // so the next offline / online launch inherits it. Online
        // play's K::MatchStart handler still re-applies the
        // role-resolved slot value on top, so host/guest each pick
        // their own setting — slot 1's value only matters there (kept
        // on disk via SaveSocdState).
        if (player_slot == 0) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", socd_mode_[0]);
            ::SetEnvironmentVariableA("FM2K_SOCD_MODE", buf);
        }
    }
    ImGui::Separator();

    // Bindings body — inherits the existing per-player binding UI.
    if (FM2KInputBinder::RenderBody(player_slot)) FM2KInputBinder::Save();
}

