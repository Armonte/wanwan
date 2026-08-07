// launcher_ui_notify.cpp -- LauncherUI notifications + audio-mute state. Split from FM2K_LauncherUI.cpp (pure move).
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

void LauncherUI::LoadAudioMuteState() {
    const std::string path = AudioIniPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = s.substr(0, eq);
        const std::string v = s.substr(eq + 1);
        const bool truthy = (v == "1" || v == "true" || v == "yes" || v == "on");
        if      (k == "bgm_muted") mute_bgm_ = truthy;
        else if (k == "se_muted")  mute_se_  = truthy;
    }
    std::fclose(f);
}

void LauncherUI::SaveAudioMuteState() {
    const std::string path = AudioIniPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "; FM2K Rollback audio mute state\n");
    std::fprintf(f, "; rewritten on each toggle from the launcher's Settings menu.\n");
    std::fprintf(f, "; the hook DLL re-reads this file ~once per second from inside\n");
    std::fprintf(f, "; the audio dispatcher, so changes propagate without restarting.\n");
    std::fprintf(f, "bgm_muted=%d\n", mute_bgm_ ? 1 : 0);
    std::fprintf(f, "se_muted=%d\n",  mute_se_  ? 1 : 0);
    std::fclose(f);
}

void LauncherUI::LoadNotifyState() {
    const std::string path = NotifySettingsPath();
    if (path.empty()) return;
    notify_flash_ = ReadBoolSetting(path, "notify_flash", true);
    notify_sound_ = ReadBoolSetting(path, "notify_sound", true);
    notify_toast_ = ReadBoolSetting(path, "notify_toast", true);
}

void LauncherUI::SaveNotifyState() {
    const std::string path = NotifySettingsPath();
    if (path.empty()) return;
    WriteBoolSetting(path, "notify_flash", notify_flash_);
    WriteBoolSetting(path, "notify_sound", notify_sound_);
    WriteBoolSetting(path, "notify_toast", notify_toast_);
}

void LauncherUI::FireChallengeNotification(const std::string& from_nick) {
    // Resolve the launcher's HWND once. SDL3 stores it on the window's
    // properties under SDL_PROP_WINDOW_WIN32_HWND_POINTER. If we can't get
    // it (e.g., SDL backend changed), every Win32-flavored notification
    // silently no-ops -- sound still works.
    HWND hwnd = nullptr;
    if (window_) {
        hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
                                            SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                            nullptr);
    }

    // 1) Taskbar flash. Only if the launcher isn't currently the foreground
    // window -- flashing while focused is annoying. FLASHW_ALL flashes both
    // window caption AND taskbar button. FLASHW_TIMERNOFG keeps flashing
    // until the user focuses the window. Cancels automatically on focus.
    if (notify_flash_ && hwnd && hwnd != GetForegroundWindow()) {
        FLASHWINFO fi = { sizeof(fi), hwnd,
                          FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&fi);
    }

    // 2) Sound: MessageBeep is the cheapest "make a noise" path on Windows.
    // No assets to ship; the Windows default-event sound is what users
    // already recognize as a notification chirp. MB_ICONINFORMATION maps
    // to SystemAsterisk -- a short, non-jarring ding.
    if (notify_sound_) {
        MessageBeep(MB_ICONINFORMATION);
    }

    // 3) Windows toast / balloon notification via Shell_NotifyIconW
    // (wide-string variant). The W variant is critical so non-ASCII nicks
    // (Armonté, テスト, español) render correctly -- Shell_NotifyIconA
    // would interpret UTF-8 bytes through the system codepage (CP1252 on
    // most US installs), turning "é" (`C3 A9`) into "Ã©" garbage.
    //
    // Single-balloon protocol:
    //   NIM_ADD    with NIF_ICON | NIF_TIP only      -- register, no toast
    //   NIM_MODIFY with NIF_INFO + content fields    -- fires exactly 1 toast
    //   NIM_DELETE                                   -- cleanup
    // Earlier impl set NIF_INFO on both ADD and MODIFY which fired TWO
    // balloons (one per call) -- fixed by splitting the flag set.
    if (notify_toast_ && hwnd) {
        char body_utf8[256];
        std::snprintf(body_utf8, sizeof(body_utf8),
                      T("modal_incoming_challenge_body"), from_nick.c_str());

        NOTIFYICONDATAW nid{};
        nid.cbSize  = sizeof(nid);
        nid.hWnd    = hwnd;
        nid.uID     = 1;
        nid.hIcon   = LoadIcon(nullptr, IDI_APPLICATION);

        auto utf8_to_wide = [](const char* in, wchar_t* out, int out_len) {
            if (!in || !out || out_len <= 0) return;
            int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, out_len);
            if (n <= 0) out[0] = L'\0';
        };
        utf8_to_wide("FM2K Rollback Launcher", nid.szTip,
                     (int)(sizeof(nid.szTip)/sizeof(WCHAR)));

        // Step 1: register the icon (no NIF_INFO yet → no balloon).
        nid.uFlags = NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_ADD, &nid);

        // Step 2: modify with the balloon info (this fires the one toast).
        nid.uFlags     = NIF_INFO | NIF_ICON | NIF_TIP;
        nid.dwInfoFlags = NIIF_INFO;
        utf8_to_wide(T("modal_incoming_challenge_title"), nid.szInfoTitle,
                     (int)(sizeof(nid.szInfoTitle)/sizeof(WCHAR)));
        utf8_to_wide(body_utf8, nid.szInfo,
                     (int)(sizeof(nid.szInfo)/sizeof(WCHAR)));
        Shell_NotifyIconW(NIM_MODIFY, &nid);

        // Step 3: cleanup. Windows captures the balloon info before
        // releasing the icon slot, so the toast still appears in Action
        // Center even after this Delete returns.
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
}

void LauncherUI::FireSystemNotification(const std::string& title_utf8,
                                        const std::string& body_utf8) {
    HWND hwnd = nullptr;
    if (window_) {
        hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
                                            SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                            nullptr);
    }
    if (notify_flash_ && hwnd && hwnd != GetForegroundWindow()) {
        FLASHWINFO fi = { sizeof(fi), hwnd,
                          FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&fi);
    }
    if (notify_sound_) {
        MessageBeep(MB_ICONWARNING);
    }
    if (notify_toast_ && hwnd) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = hwnd;
        nid.uID    = 1;
        nid.hIcon  = LoadIcon(nullptr, IDI_APPLICATION);

        auto utf8_to_wide = [](const char* in, wchar_t* out, int out_len) {
            if (!in || !out || out_len <= 0) return;
            int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, out_len);
            if (n <= 0) out[0] = L'\0';
        };
        utf8_to_wide("FM2K Rollback Launcher", nid.szTip,
                     (int)(sizeof(nid.szTip) / sizeof(WCHAR)));

        nid.uFlags = NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_ADD, &nid);

        nid.uFlags      = NIF_INFO | NIF_ICON | NIF_TIP;
        nid.dwInfoFlags = NIIF_WARNING;
        utf8_to_wide(title_utf8.c_str(), nid.szInfoTitle,
                     (int)(sizeof(nid.szInfoTitle) / sizeof(WCHAR)));
        utf8_to_wide(body_utf8.c_str(),  nid.szInfo,
                     (int)(sizeof(nid.szInfo) / sizeof(WCHAR)));
        Shell_NotifyIconW(NIM_MODIFY, &nid);

        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
}

void LauncherUI::NotifyHubMatchEnded() {
    if (!hub_state_) return;
    if (!hub_state_->client.IsConnected()) return;
    hub_state_->client.MatchEnded();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: signaled match_ended (game terminated)");
}

