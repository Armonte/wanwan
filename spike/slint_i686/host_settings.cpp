// host_settings.cpp -- see host_settings.h. Store access is deliberately
// tiny and dependency-free (the launcher's own helpers live in TUs that
// drag ImGui / winhttp / miniz along), but the FORMATS are the launcher's:
// flat key=value for the three ini files, [GamePlay] Editor.TestPlay.* for
// the per-game override, [ddraw] via the Win32 profile API for cnc-ddraw,
// (the binder profile is the binder core's own, see host_binder.cpp).
#include "host_settings.h"

// The launcher's own cnc-ddraw installer / updater and ini path (one
// resolver: FM2K_DDRAW_DIR, else <exe dir>\cnc-ddraw). Compiled into the
// spike so "check and install" and "reset to defaults" are the SAME code
// the shipping launcher runs, not a second implementation.
#include "FM2K_CncDDraw.h"
#include "host_discord.h"
#include "FM2K_Updater.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hostgui {
namespace {

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

std::string AppDataDir() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string SubDir(const char* name) {
    const std::string base = AppDataDir();
    if (base.empty()) return "";
    const std::string dir = base + "\\" + name;
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string SettingsIni() { const auto d = AppDataDir(); return d.empty() ? "" : d + "\\settings.ini"; }
std::string AudioIni()    { const auto d = AppDataDir(); return d.empty() ? "" : d + "\\audio.ini"; }
std::string DevFlagsIni() { const auto d = AppDataDir(); return d.empty() ? "" : d + "\\dev_flags.ini"; }

std::string GameConfigIni(const std::string& stem) {
    if (stem.empty()) return "";
    const auto d = SubDir("game_configs");
    return d.empty() ? "" : d + "\\" + stem + ".ini";
}
std::string GamePatchesIni(const std::string& stem) {
    if (stem.empty()) return "";
    const auto d = SubDir("game_patches");
    return d.empty() ? "" : d + "\\" + stem + ".ini";
}

// THE one cnc-ddraw dir rule lives in the launcher (FM2K_DDrawRedirect's
// ResolveCncDdrawDir, which cnc_ddraw::InstallDir wraps).
std::string DdrawIni() { return fm2k::cnc_ddraw::IniPath(); }

const char* InstallStateName(fm2k::cnc_ddraw::State s) {
    using S = fm2k::cnc_ddraw::State;
    switch (s) {
        case S::Idle:            return "not checked yet";
        case S::Checking:        return "checking";
        case S::NotInstalled:    return "NOT INSTALLED";
        case S::UpToDate:        return "installed, up to date";
        case S::UpdateAvailable: return "update available";
        case S::Downloading:     return "downloading";
        case S::Extracting:      return "extracting";
        case S::Ready:           return "installed";
        case S::Failed:          return "FAILED";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Flat key=value files (settings.ini / audio.ini / dev_flags.ini / patches)
// ---------------------------------------------------------------------------

using Kv = std::vector<std::pair<std::string, std::string>>;

void Rtrim(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t')) s.pop_back();
}

Kv ReadKv(const std::string& path) {
    Kv kv;
    if (path.empty()) return kv;
    std::ifstream f(path);
    std::string s;
    while (std::getline(f, s)) {
        Rtrim(s);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
    }
    return kv;
}

std::string KvGet(const Kv& kv, const char* key, const std::string& def) {
    std::string out = def;
    for (const auto& p : kv) if (p.first == key) out = p.second;  // last wins, like the launcher
    return out;
}

bool KvBool(const Kv& kv, const char* key, bool def) {
    const std::string v = KvGet(kv, key, def ? "1" : "0");
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

int KvInt(const Kv& kv, const char* key, int def) {
    const std::string v = KvGet(kv, key, "");
    if (v.empty()) return def;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    return (end && *end == '\0') ? (int)n : def;
}

// Rewrite one key, preserving every other key. Comments are dropped, which
// is what the launcher's own WriteIntSetting does too.
bool KvSet(const std::string& path, const std::string& key, const std::string& value,
           const char* header = nullptr) {
    if (path.empty()) return false;
    Kv kv = ReadKv(path);
    bool found = false;
    for (auto& p : kv) if (p.first == key) { p.second = value; found = true; }
    if (!found) kv.emplace_back(key, value);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    if (header) std::fprintf(f, "%s", header);
    for (const auto& p : kv) std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
    std::fclose(f);
    return true;
}

bool KvErase(const std::string& path, const std::string& key) {
    if (path.empty()) return false;
    Kv kv = ReadKv(path);
    Kv keep;
    for (const auto& p : kv) if (p.first != key) keep.push_back(p);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    for (const auto& p : keep) std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// cnc-ddraw ddraw.ini via the profile API (what cnc-ddraw itself uses)
// ---------------------------------------------------------------------------

std::string DdrawGet(const char* key, const char* def) {
    char buf[512] = {};
    GetPrivateProfileStringA("ddraw", key, def, buf, sizeof(buf), DdrawIni().c_str());
    return buf;
}
bool DdrawBool(const char* key, bool def) {
    const std::string v = DdrawGet(key, def ? "true" : "false");
    return v == "true" || v == "1" || v == "yes";
}
int DdrawInt(const char* key, int def) {
    const std::string v = DdrawGet(key, "");
    if (v.empty()) return def;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 0);  // base 0: hotkeys are 0xNN
    return (end && *end == '\0') ? (int)n : def;
}
bool DdrawSet(const char* key, const std::string& v) {
    return WritePrivateProfileStringA("ddraw", key, v.c_str(), DdrawIni().c_str()) != 0;
}

// ---------------------------------------------------------------------------
// Per-game override ([GamePlay] Editor.TestPlay.*), the launcher's format
// ---------------------------------------------------------------------------

struct CfgKey { const char* ui; const char* ini; };
const CfgKey kCfgKeys[] = {
    {"vs_single_play",   "Editor.TestPlay.VSSinglePlay"},
    {"vs_team_play",     "Editor.TestPlay.VSTeamPlay"},
    {"time",             "Editor.TestPlay.time"},
    {"game_speed",       "Editor.TestPlay.GameSpeed"},
    {"stage_nb",         "Editor.TestPlay.StageNb"},
    {"joystick",         "Editor.TestPlay.JoyStick"},
    {"player0_cpu",      "Editor.TestPlay.Player0.cpu"},
    {"player1_cpu",      "Editor.TestPlay.Player1.cpu"},
    {"hit_judge",        "Editor.TestPlay.HitJudge"},
    {"game_information", "Editor.TestPlay.GameInformation"},
    {"vs_mode",          "Editor.TestPlay.VSMode"},
};

const char* CfgIniKey(const std::string& ui) {
    for (const auto& k : kCfgKeys) if (ui == k.ui) return k.ini;
    return nullptr;
}

// -1 = unset (the launcher's kUnset).
int CfgGet(const std::string& stem, const char* ui) {
    const char* ini = CfgIniKey(ui);
    const std::string path = GameConfigIni(stem);
    if (!ini || path.empty()) return -1;
    std::ifstream f(path);
    std::string s;
    bool in_gameplay = false;
    int out = -1;
    while (std::getline(f, s)) {
        Rtrim(s);
        if (s.empty()) continue;
        if (s.front() == '[') { in_gameplay = (s == "[GamePlay]"); continue; }
        if (!in_gameplay || s[0] == '#' || s[0] == ';') continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        if (s.substr(0, eq) == ini) out = std::atoi(s.c_str() + eq + 1);
    }
    return out;
}

// Mirrors game_ini::Save: keep every non-managed line, drop the managed
// keys inside [GamePlay], re-insert the set ones after the header, and
// delete the file when nothing is set (== "reset to defaults").
bool CfgSet(const std::string& stem, const char* ui, int value) {
    const char* ini = CfgIniKey(ui);
    const std::string path = GameConfigIni(stem);
    if (!ini || path.empty()) return false;
    std::vector<std::string> kept;
    Kv managed;  // ini key -> value, current file state
    {
        std::ifstream f(path);
        std::string s;
        std::string section;
        while (std::getline(f, s)) {
            Rtrim(s);
            if (!s.empty() && s.front() == '[') { section = s; kept.push_back(s); continue; }
            if (section == "[GamePlay]") {
                const size_t eq = s.find('=');
                if (eq != std::string::npos && !s.empty() && s[0] != '#' && s[0] != ';') {
                    const std::string k = s.substr(0, eq);
                    bool is_managed = false;
                    for (const auto& c : kCfgKeys) if (k == c.ini) is_managed = true;
                    if (is_managed) { managed.emplace_back(k, s.substr(eq + 1)); continue; }
                }
            }
            kept.push_back(s);
        }
    }
    bool found = false;
    for (auto& p : managed) if (p.first == ini) { p.second = std::to_string(value); found = true; }
    if (!found) managed.emplace_back(ini, std::to_string(value));
    Kv body;
    for (const auto& p : managed) if (std::atoi(p.second.c_str()) != -1) body.push_back(p);
    if (body.empty()) { DeleteFileA(path.c_str()); return true; }
    size_t insert_at = kept.size();
    bool had = false;
    for (size_t i = 0; i < kept.size(); ++i) if (kept[i] == "[GamePlay]") { insert_at = i + 1; had = true; }
    if (!had) { kept.push_back("[GamePlay]"); insert_at = kept.size(); }
    std::vector<std::string> lines;
    for (const auto& p : body) lines.push_back(p.first + "=" + p.second);
    kept.insert(kept.begin() + (long)insert_at, lines.begin(), lines.end());
    std::ofstream o(path, std::ios::trunc | std::ios::binary);
    if (!o) return false;
    for (const auto& l : kept) { o << l << '\n'; }
    return true;
}

// ---------------------------------------------------------------------------
// Row builder
// ---------------------------------------------------------------------------

enum Tab { DISPLAY = 0, INPUT, AUDIO, NETWORK, HUB, GAME, DEVELOPER };
enum Kind { TOGGLE = 0, INT = 1, CHOICE = 2, TEXT = 3, VKEY = 4, ACTION = 5, NOTE = 6, GROUP = 7, ADV = 9 };

std::string Clean(std::string s) {
    for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string g_direct_addr;   // tool.direct_addr, this run only

struct Rows {
    std::string out;
    bool adv = false;  // rows added while true are the advanced layer
    bool onboard = false;  // rows added while true also show in first-run setup
    void add(int tab, int kind, const std::string& key, const std::string& label,
             const std::string& hint, const std::string& value,
             const std::string& value_text = "", int count = 0, bool enabled = true) {
        out += std::to_string(tab) + '\t' + Clean(key) + '\t' + std::to_string(kind) + '\t' +
               Clean(label) + '\t' + Clean(hint) + '\t' + Clean(value) + '\t' +
               Clean(value_text) + '\t' + std::to_string(count) + '\t' +
               (adv ? "1" : "0") + '\t' + (enabled ? "1" : "0") + '\t' +
               (onboard ? "1" : "0") + '\n';
    }
    void group(int tab, const std::string& name) { add(tab, GROUP, "", name, "", ""); }
    void note(int tab, const std::string& label, const std::string& hint) { add(tab, NOTE, "", label, hint, ""); }
    void toggle(int tab, const std::string& key, const std::string& label, const std::string& hint, bool on) {
        add(tab, TOGGLE, key, label, hint, on ? "1" : "0");
    }
    void integer(int tab, const std::string& key, const std::string& label, const std::string& hint, int v) {
        add(tab, INT, key, label, hint, std::to_string(v));
    }
    void text(int tab, const std::string& key, const std::string& label, const std::string& hint, const std::string& v) {
        add(tab, TEXT, key, label, hint, v);
    }
    void choice(int tab, const std::string& key, const std::string& label, const std::string& hint,
                int idx, const std::vector<std::string>& items) {
        if (idx < 0 || idx >= (int)items.size()) idx = 0;
        add(tab, CHOICE, key, label, hint, std::to_string(idx), items[(size_t)idx], (int)items.size());
    }
    void vkey(int tab, const std::string& key, const std::string& label, int vk) {
        char hex[16]; std::snprintf(hex, sizeof(hex), "0x%02X", vk & 0xFF);
        add(tab, VKEY, key, label, "Virtual-key code, hex. 0x00 = unbound.", hex, hex);
    }
    void action(int tab, const std::string& key, const std::string& label, const std::string& hint, bool enabled = true) {
        add(tab, ACTION, key, label, hint, "", "", 0, enabled);
    }
    void adv_toggle(int tab, const std::string& label) { add(tab, ADV, "", label, "", ""); }
};

std::vector<std::string> DelayItems() {
    std::vector<std::string> v = {"computed (avg ping)", "computed (peak ping)"};
    for (int i = 0; i <= 16; ++i) v.push_back("manual " + std::to_string(i) + " frames");
    return v;
}

int WindowModeIndex() {
    const bool fs = DdrawBool("fullscreen", false), win = DdrawBool("windowed", true);
    if (win && !fs) return 0;   // windowed
    if (win && fs)  return 1;   // borderless (windowed fullscreen)
    return 2;                   // fullscreen upscaled
}

void DisplayRows(Rows& r) {
    const int T = DISPLAY;
    r.note(T, "Edits apply to the next game launch.", "Editing " + DdrawIni());
    r.group(T, "Window");
    r.onboard = true;
    r.choice(T, "ddraw.mode", "Window mode", "Borderless is a windowed fullscreen; Fullscreen is exclusive upscaled.",
             WindowModeIndex(), {"Windowed", "Borderless (windowed fullscreen)", "Fullscreen upscaled"});
    r.onboard = false;
    r.integer(T, "ddraw.width",  "Width",  "0 = use the game's own resolution.", DdrawInt("width", 0));
    r.integer(T, "ddraw.height", "Height", "0 = use the game's own resolution.", DdrawInt("height", 0));
    r.toggle(T, "ddraw.maintas", "Keep aspect ratio", "", DdrawBool("maintas", false));
    r.toggle(T, "ddraw.boxing", "Integer scaling", "Pixel-exact multiples; leaves black borders.", DdrawBool("boxing", false));
    r.text(T, "ddraw.aspect_ratio", "Aspect ratio override", "e.g. 4:3 or 16:9; blank = automatic.", DdrawGet("aspect_ratio", ""));
    r.toggle(T, "ddraw.border", "Window border", "Windowed mode only.", DdrawBool("border", true));
    r.toggle(T, "ddraw.resizable", "Resizable window", "", DdrawBool("resizable", true));
    r.group(T, "Scaling and sync");
    r.onboard = true;
    r.choice(T, "ddraw.d3d9_filter", "Scaling filter", "Bicubic and Lanczos need a 16 or 32-bit game.",
             DdrawInt("d3d9_filter", 0), {"Nearest neighbour", "Bilinear", "Bicubic", "Lanczos"});
    r.text(T, "ddraw.shader", "Shader", "Path or name of a GLSL shader; blank = none.", DdrawGet("shader", ""));
    r.toggle(T, "ddraw.vsync", "VSync", "Off can lower input latency at the cost of tearing.", DdrawBool("vsync", false));
    r.onboard = false;
    r.note(T, "Renderer: " + DdrawGet("renderer", "direct3d9") + (DdrawGet("renderer", "direct3d9") == "direct3d9" ? "  (managed by the launcher)" : "  -- the launcher resets this to direct3d9 at start"),
           "The in-game overlay needs Direct3D 9, so the launcher pins it once per start.");
    r.group(T, "Alt+Enter and hotkeys");
    r.toggle(T, "ddraw.toggle_borderless", "Alt+Enter toggles borderless instead of fullscreen", "", DdrawBool("toggle_borderless", false));
    r.toggle(T, "ddraw.toggle_upscaled", "Alt+Enter toggles upscaled instead", "", DdrawBool("toggle_upscaled", false));
    r.toggle(T, "ddraw.devmode", "Do not lock the mouse cursor to the window", "", DdrawBool("devmode", true));
    r.vkey(T, "ddraw.keytogglefullscreen",  "Toggle fullscreen (Alt + key)", DdrawInt("keytogglefullscreen", 0x0D));
    r.vkey(T, "ddraw.keytogglefullscreen2", "Toggle fullscreen (single key)", DdrawInt("keytogglefullscreen2", 0x00));
    r.vkey(T, "ddraw.keytogglemaximize",    "Maximize window (Alt + key)",    DdrawInt("keytogglemaximize", 0x22));
    r.vkey(T, "ddraw.keytogglemaximize2",   "Maximize window (single key)",   DdrawInt("keytogglemaximize2", 0x00));
    r.vkey(T, "ddraw.keyunlockcursor1",     "Unlock cursor (Ctrl + key)",     DdrawInt("keyunlockcursor1", 0x09));
    r.vkey(T, "ddraw.keyunlockcursor2",     "Unlock cursor (Right Alt + key)", DdrawInt("keyunlockcursor2", 0xA3));
    r.vkey(T, "ddraw.keyscreenshot",        "Screenshot",                     DdrawInt("keyscreenshot", 0x2C));
    r.group(T, "cnc-ddraw install");
    {
        const auto snap = fm2k::cnc_ddraw::Get();
        std::string line = std::string("cnc-ddraw: ") + InstallStateName(snap.state);
        if (!snap.local_version.empty())  line += ", local " + snap.local_version;
        if (!snap.remote_version.empty()) line += ", pinned " + snap.remote_version;
        if (snap.state == fm2k::cnc_ddraw::State::Downloading && snap.total_bytes)
            line += ", " + std::to_string(snap.downloaded_bytes / 1024) + " / " + std::to_string(snap.total_bytes / 1024) + " KB";
        r.onboard = true;
        r.note(T, line, snap.error_detail.empty() ? "Installed at " + fm2k::cnc_ddraw::InstallDir() : snap.error_detail);
        const bool busy = snap.state == fm2k::cnc_ddraw::State::Checking ||
                          snap.state == fm2k::cnc_ddraw::State::Downloading ||
                          snap.state == fm2k::cnc_ddraw::State::Extracting;
        r.action(T, "ddraw.install", "Check and install cnc-ddraw", "Installs or updates the pinned cnc-ddraw release into the directory above.", !busy);
        r.onboard = false;
        r.action(T, "ddraw.reinstall", "Force reinstall cnc-ddraw", "Re-downloads even when the version matches.", !busy);
        r.action(T, "ddraw.reset", "Reset ddraw.ini to launcher defaults", "Also wipes any per-game [exe] blocks.", !busy);
    }

    r.adv_toggle(T, "cnc-ddraw advanced settings (rarely needed)");
    r.adv = true;
    r.group(T, "Window position");
    r.choice(T, "ddraw.center_window", "Center on resolution change", "", DdrawInt("center_window", 1),
             {"Never center", "Automatic", "Always center"});
    r.integer(T, "ddraw.posX", "Window X", "-32000 = centered.", DdrawInt("posX", -32000));
    r.integer(T, "ddraw.posY", "Window Y", "-32000 = centered.", DdrawInt("posY", -32000));
    r.choice(T, "ddraw.savesettings", "Save window position and size on exit", "", DdrawInt("savesettings", 1),
             {"Do not save", "Save to [ddraw]", "Save per game [exe]"});
    r.group(T, "Performance");
    r.choice(T, "ddraw.maxfps_mode", "Frame cap", "", DdrawInt("maxfps", 0) < 0 ? 0 : DdrawInt("maxfps", 0) == 0 ? 1 : 2,
             {"Screen refresh rate", "Unlimited", "Custom cap (value below)"});
    r.integer(T, "ddraw.maxfps", "maxfps", "-1 = screen, 0 = unlimited, n = cap.", DdrawInt("maxfps", 0));
    r.integer(T, "ddraw.maxgameticks", "maxgameticks", "-1 = disabled, -2 = refresh rate, 0 = 60 Hz vblank.", DdrawInt("maxgameticks", -1));
    r.integer(T, "ddraw.minfps", "minfps", "0 = disabled, -1 = use maxfps, -2 = force redraw.", DdrawInt("minfps", 0));
    r.choice(T, "ddraw.limiter_type", "Limiter type", "", DdrawInt("limiter_type", 0),
             {"Automatic", "TestCooperativeLevel", "BltFast", "Unlock", "PeekMessage"});
    r.toggle(T, "ddraw.nonexclusive", "Disable fullscreen-exclusive mode", "", DdrawBool("nonexclusive", false));
    r.text(T, "ddraw.inject_resolution", "Inject resolution", "e.g. 960x540; blank = off.", DdrawGet("inject_resolution", ""));
    r.toggle(T, "ddraw.vhack", "vhack", "High-res patches for C&C, RA1, Worms 2, KKND Xtreme.", DdrawBool("vhack", false));
    r.group(T, "Input");
    r.toggle(T, "ddraw.adjmouse", "adjmouse", "Auto mouse-sensitivity scaling.", DdrawBool("adjmouse", false));
    r.toggle(T, "ddraw.hook_peekmessage", "hook_peekmessage", "Cursor-lock fix on upscaling.", DdrawBool("hook_peekmessage", false));
    r.group(T, "Compatibility");
    r.toggle(T, "ddraw.noactivateapp", "noactivateapp", "Hide WM_ACTIVATEAPP / NCACTIVATE on alt-tab.", DdrawBool("noactivateapp", false));
    r.toggle(T, "ddraw.singlecpu", "singlecpu", "Force CPU0 affinity.", DdrawBool("singlecpu", false));
    r.choice(T, "ddraw.resolutions", "Available display resolutions", "", DdrawInt("resolutions", 0),
             {"Small list", "Very small list", "Full list"});
    r.choice(T, "ddraw.fixchilds", "fixchilds", "Child window handling.", DdrawInt("fixchilds", 2),
             {"Disabled", "Display top-left", "Display top-left + repaint", "Hide", "Hide + repaint"});
    r.integer(T, "ddraw.anti_aliased_fonts_min_size", "anti_aliased_fonts_min_size", "", DdrawInt("anti_aliased_fonts_min_size", 13));
    r.integer(T, "ddraw.min_font_size", "min_font_size", "", DdrawInt("min_font_size", 0));
    r.text(T, "ddraw.screenshotdir", "Screenshot directory", "", DdrawGet("screenshotdir", ".\\Screenshots\\"));
    r.group(T, "Undocumented (key names are cnc-ddraw's own)");
    for (const char* k : {"fix_alt_key_stuck", "game_handles_close", "fix_not_responding", "no_compat_warning",
                          "lock_surfaces", "flipclear", "rgb555", "no_dinput_hook", "center_cursor_fix",
                          "lock_mouse_top_left", "limit_gdi_handles", "remove_menu"}) {
        r.toggle(T, std::string("ddraw.") + k, k, "", DdrawBool(k, false));
    }
    r.integer(T, "ddraw.guard_lines", "guard_lines", "", DdrawInt("guard_lines", 200));
    r.integer(T, "ddraw.max_resolutions", "max_resolutions", "", DdrawInt("max_resolutions", 0));
    r.integer(T, "ddraw.hook", "hook", "Hook mode 1-4; default 4.", DdrawInt("hook", 4));
    r.integer(T, "ddraw.refresh_rate", "refresh_rate", "0 = monitor default.", DdrawInt("refresh_rate", 0));
    r.adv = false;
}

// INPUT has no stored rows: the tab is the binder itself (host_binder.cpp
// over the shipping core), which owns bindings, device and SOCD per player.
void InputRows(Rows&) {}

void AudioRows(Rows& r) {
    const int T = AUDIO;
    const Kv audio = ReadKv(AudioIni());
    const Kv st = ReadKv(SettingsIni());
    r.group(T, "Game audio");
    r.toggle(T, "audio.bgm_muted", "Mute music", "The running game picks this up within a second.", KvBool(audio, "bgm_muted", false));
    r.toggle(T, "audio.se_muted", "Mute sound effects", "", KvBool(audio, "se_muted", false));
    r.group(T, "Alerts");
    r.toggle(T, "settings.notify_flash", "Flash the taskbar on a challenge", "", KvBool(st, "notify_flash", true));
    r.toggle(T, "settings.notify_sound", "Play a sound on a challenge", "", KvBool(st, "notify_sound", true));
    r.toggle(T, "settings.notify_toast", "Windows notification on a challenge", "", KvBool(st, "notify_toast", true));
}

void NetworkRows(Rows& r) {
    const int T = NETWORK;
    const Kv st = ReadKv(SettingsIni());
    r.group(T, "Input delay");
    r.choice(T, "settings.delay_mode", "Delay", "Computed modes size the delay to your ping; a manual value is never raised to the opponent's.",
             KvInt(st, "delay_mode", 0), DelayItems());
    r.note(T, "Rollback window: 16 frames, fixed.",
           "Every match runs the same prediction window; delay above is the only knob. "
           "(Developers: FM2K_PREDICTION_WINDOW overrides it for testing.)");
}

void HubRows(Rows& r) {
    const int T = HUB;
    const Kv st = ReadKv(SettingsIni());
    const Kv dev = ReadKv(DevFlagsIni());
    r.group(T, "Account");
    r.onboard = true;
    {
        const DiscordState d = DiscordGet();
        if (d.signed_in) {
            r.note(T, "Signed in as " + d.display + ".", "The hub shows this name; sign out to use a different Discord account.");
            r.toggle(T, "hub.use_discord_name", "Use my Discord name in the lobby", "", d.use_discord_name);
            r.action(T, "hub.signout", "Sign out", "");
        } else if (d.pending) {
            r.note(T, d.browser_open_failed ? "Your browser did not open. Paste this link in it:" : "Browser opened. Click Authorize on Discord and come back.",
                   d.authorize_url);
            r.action(T, "hub.copy_url", "Copy the link", "");
            r.action(T, "hub.reopen", "Open it again", "");
            r.action(T, "hub.cancel", "Cancel", "");
        } else {
            if (!d.problem.empty()) r.note(T, "Sign-in did not complete: " + d.problem, "");
            r.note(T, "Playing online needs no account: the lobby takes your display name.",
                   "Signing in with Discord is optional. It adds online replays, stats and rankings, and your Patreon perks.");
            r.action(T, "hub.discord", "Sign in with Discord (optional)", "Opens Discord in your browser.");
        }
    }
    r.onboard = false;
    r.toggle(T, "dev.stealth_mode", "Stealth mode", "Hide my match and characters from the lobby.", KvBool(dev, "stealth_mode", false));
    r.group(T, "Launcher");
    const std::string lang = KvGet(st, "language", "en");
    r.choice(T, "settings.language", "Language", "", lang == "ja" ? 1 : lang == "es" ? 2 : 0, {"English", "日本語", "Español"});
    r.choice(T, "dev.update_channel", "Release channel", "", KvInt(dev, "update_channel", 0), {"Stable", "Dev", "Bleeding"});
    {
        using US = fm2k::updater::State;
        const auto u = fm2k::updater::Get();
        std::string line, hint;
        switch (u.state) {
            case US::Idle:            line = "Updates: not checked yet."; break;
            case US::Checking:        line = "Updates: checking..."; break;
            case US::UpToDate:        line = "Updates: this is the newest build on your channel."; break;
            case US::UpdateAvailable: line = "Update available: " + u.remote_version; break;
            case US::Downloading:     line = "Downloading " + u.remote_version + (u.total_bytes ? " (" + std::to_string(u.downloaded_bytes / 1024) + " / " + std::to_string(u.total_bytes / 1024) + " KB)" : ""); break;
            case US::Ready:           line = "Update " + u.remote_version + " downloaded."; break;
            case US::Failed:          line = "Update check failed."; hint = u.error_detail; break;
        }
        if (!u.latest_stable.empty() && hint.empty())
            hint = "Newest: stable " + u.latest_stable + (u.latest_dev.empty() ? "" : ", dev " + u.latest_dev) + (u.latest_bleeding.empty() ? "" : ", bleeding " + u.latest_bleeding);
        r.note(T, line, hint);
        const bool busy = u.state == US::Checking || u.state == US::Downloading;
        r.action(T, "hub.check_updates", "Check for updates", "", !busy);
        if (u.state == US::UpdateAvailable) r.action(T, "hub.download_update", "Download " + u.remote_version, "", true);
        if (u.state == US::Ready)
            r.note(T, "Applying an update from here is not wired: the updater replaces the shipping launcher's files, and this build is a separate exe.", "");
    }
    r.toggle(T, "dev.auto_upload_logs", "Send crash and desync diagnostics",
             "On a crash or desync, the diagnostic bundle (log tail, desync diff, RNG trace) goes to the 2DFM hub. Personal data is scrubbed first. Default on.",
             KvBool(dev, "auto_upload_logs", true));
    r.toggle(T, "dev.developer_ui", "Show developer tools", "Adds the DEVELOPER tab.", KvBool(dev, "developer_ui", false));
}

std::string OrDefault(int v) { return v < 0 ? "game default" : std::to_string(v); }

void GameRows(Rows& r, const std::string& stem) {
    const int T = GAME;
    if (stem.empty()) {
        r.note(T, "Pick a game in BROWSE to edit its match settings.", "");
        return;
    }
    r.note(T, stem, "Match settings write " + GameConfigIni(stem) + "; patches write " + GamePatchesIni(stem) + ".");
    r.group(T, "Match settings (-1 = the game's own default)");
    r.integer(T, "cfg.vs_single_play", "Rounds to win (1v1)", "1-9", CfgGet(stem, "vs_single_play"));
    r.integer(T, "cfg.vs_team_play",   "Rounds to win (team)", "1-9", CfgGet(stem, "vs_team_play"));
    r.integer(T, "cfg.time",           "Round timer (seconds)", "0 = infinite, -1 = game default.", CfgGet(stem, "time"));
    r.integer(T, "cfg.game_speed",     "Game speed", "1-16, the engine default is 10.", CfgGet(stem, "game_speed"));
    r.integer(T, "cfg.stage_nb",       "Stage", "Stage index, 0 = first.", CfgGet(stem, "stage_nb"));
    const Kv p = ReadKv(GamePatchesIni(stem));
    r.group(T, "Random stage");
    r.toggle(T, "patch.random_stage_enable", "Random stage each match", "", KvBool(p, "random_stage_enable", false));
    r.integer(T, "patch.random_stage_min", "Lowest stage index", "", KvInt(p, "random_stage_min", 0));
    r.integer(T, "patch.random_stage_max", "Highest stage index", "", KvInt(p, "random_stage_max", 0));
    r.group(T, "Experimental patches (offline launches only)");
    r.toggle(T, "patch.gs_pic_fix", "Speed patch for vanpri (game speed != 10 fix)", "Locks the broken script-wait scaling to the GameSpeed=10 value.", KvBool(p, "gs_pic_fix", true));
    r.toggle(T, "patch.team_css_dupe_lock", "Team select: no duplicate character per team", "", KvBool(p, "team_css_dupe_lock", false));
    r.toggle(T, "patch.team_kof_retention", "Team KOF retention", "The winner keeps HP and meter into the next round.", KvBool(p, "team_kof_retention", false));
    r.integer(T, "patch.team_size", "Team size override", "0 = engine default, 2-4 otherwise.", KvInt(p, "team_size", 0));
    r.integer(T, "patch.damage_multiplier_pct", "Damage multiplier (percent)", "100 = unchanged.", KvInt(p, "damage_multiplier_pct", 100));
    r.toggle(T, "patch.vs_cpu_mode", "VS CPU mode", "P1 picks both characters, the AI plays P2.", KvBool(p, "vs_cpu_mode", false));
    r.toggle(T, "patch.cpu_vs_cpu_mode", "CPU vs CPU", "Both sides AI.", KvBool(p, "cpu_vs_cpu_mode", false));
    r.toggle(T, "patch.training_mode", "Training mode", "P2 behaviour options.", KvBool(p, "training_mode", false));
    r.toggle(T, "patch.option_mode_selector", "OPTION-button mode selector and title overlay", "", KvBool(p, "option_mode_selector", false));
}

void DeveloperRows(Rows& r, const std::string& stem) {
    const int T = DEVELOPER;
    const Kv d = ReadKv(DevFlagsIni());
    r.note(T, "Everything here is saved in dev_flags.ini and applied on every launch the shipping launcher makes.", "");
    r.group(T, "Boot");
    r.choice(T, "dev.boot_strategy", "Boot strategy", "Fast skips the title screen; breaks StudioS Fighters / Strip Fighter Zero.",
             KvInt(d, "boot_strategy", 0), {"Safe (title screen, auto-mash)", "Fast (straight to character select)"});
    r.toggle(T, "dev.auto_title_skip", "Auto title skip", "", KvBool(d, "auto_title_skip", true));
    r.toggle(T, "dev.boot_to_battle", "Boot straight to battle (/F)", "Engine debug boot; VS 1v1 only.", KvBool(d, "boot_to_battle", false));
    r.integer(T, "dev.btb_p1_char", "Boot-to-battle P1 character", "", KvInt(d, "btb_p1_char", 0));
    r.integer(T, "dev.btb_p2_char", "Boot-to-battle P2 character", "", KvInt(d, "btb_p2_char", 0));
    r.integer(T, "dev.btb_stage", "Boot-to-battle stage", "", KvInt(d, "btb_stage", 0));
    r.integer(T, "dev.btb_p1_meter", "Boot-to-battle P1 meter", "", KvInt(d, "btb_p1_meter", 0));
    r.integer(T, "dev.btb_p2_meter", "Boot-to-battle P2 meter", "", KvInt(d, "btb_p2_meter", 0));
    r.group(T, "Hook");
    r.toggle(T, "dev.dev_mode", "Experimental hook features", "Currently the spectator .player OS-cache pre-warmer.", KvBool(d, "dev_mode", false));
    r.toggle(T, "dev.eb_diag", "EB diagnostics", "", KvBool(d, "eb_diag", false));
    r.toggle(T, "dev.fast_player_load", "Fast .player load", "", KvBool(d, "fast_player_load", false));
    r.toggle(T, "dev.bypass_trampoline", "Bypass trampoline", "Offline only.", KvBool(d, "bypass_trampoline", false));
    r.toggle(T, "dev.skip_vs_mode_patch", "Skip VS-mode patch", "", KvBool(d, "skip_vs_mode_patch", false));
    r.toggle(T, "dev.force_t4_patch", "Force T4 patch", "", KvBool(d, "force_t4_patch", false));
    r.toggle(T, "dev.t4_probe", "T4 probe", "", KvBool(d, "t4_probe", false));
    r.toggle(T, "dev.fm95_trampoline", "FM95 trampoline-driven loop", "", KvBool(d, "fm95_trampoline", false));
    if (!stem.empty()) {
        r.group(T, "Forced online (per game, offline only)");
        r.integer(T, "cfg.joystick", "Joystick (0 = keyboard, 1 = pad)", "", CfgGet(stem, "joystick"));
        r.integer(T, "cfg.player0_cpu", "P1 is CPU", "", CfgGet(stem, "player0_cpu"));
        r.integer(T, "cfg.player1_cpu", "P2 is CPU", "", CfgGet(stem, "player1_cpu"));
        r.integer(T, "cfg.hit_judge", "Hit-judge overlay", "", CfgGet(stem, "hit_judge"));
        r.integer(T, "cfg.game_information", "Damage info overlay", "", CfgGet(stem, "game_information"));
        r.integer(T, "cfg.vs_mode", "VS mode", "", CfgGet(stem, "vs_mode"));
    }
    r.group(T, "Tools (act on the selected game)");
    r.action(T, "tool.dual", "Launch dual clients", "Two copies on this machine over loopback (7000/7001), fixed rng seed. Local test only.");
    r.action(T, "tool.spectator1", "Launch spectator 1", "Joins the dual host on 7000 from port 7002.");
    r.action(T, "tool.spectator2", "Launch spectator 2", "Joins the dual host on 7000 from port 7003.");
    r.action(T, "tool.stress", "Stress test", "One instance; GekkoNet forces rollbacks every few frames. A desync here is a local determinism bug.");
    r.text(T, "tool.direct_addr", "Direct IP: peer address", "ip:port of the host to join (legacy direct connect).", g_direct_addr);
    r.action(T, "tool.direct_host", "Direct IP: host", "Listen on 7000; the peer's address is learned from HELLO.");
    r.action(T, "tool.direct_join", "Direct IP: join", "Dial the address above from port 7001.");
    r.action(T, "tool.terminate", "Terminate all clients", "Stops every instance this launcher started.");
}

bool ToInt(const std::string& v, int* out) {
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 0);
    if (!end || *end != '\0' || v.empty()) return false;
    *out = (int)n;
    return true;
}

}  // namespace

int InstallerStamp() {
    const auto s = fm2k::cnc_ddraw::Get();
    const auto u = fm2k::updater::Get();
    return (int)s.state * 1000003 + (int)(s.downloaded_bytes / 65536) + (s.error_detail.empty() ? 0 : 7)
         + (int)u.state * 10007 + (int)(u.downloaded_bytes / 65536) * 3;
}

bool SettingsTick() { return DiscordTick(); }

void SettingsShutdown() {
    fm2k::cnc_ddraw::Shutdown();
    fm2k::updater::Shutdown();
}

bool DevToolsEnabled() {
    return KvBool(ReadKv(DevFlagsIni()), "developer_ui", false);
}

std::string SettingsRows(const std::string& game_stem) {
    Rows r;
    DisplayRows(r);
    InputRows(r);
    AudioRows(r);
    NetworkRows(r);
    HubRows(r);
    GameRows(r, game_stem);
    if (DevToolsEnabled()) DeveloperRows(r, game_stem);
    return r.out;
}

std::string ToolsDirectAddr() { return g_direct_addr; }

bool ApplySetting(const std::string& key, const std::string& value,
                  const std::string& game_stem) {
    const auto starts = [&](const char* p) { return key.rfind(p, 0) == 0; };
    int n = 0;

    // ---- cnc-ddraw ----
    if (key == "ddraw.mode") {
        if (!ToInt(value, &n)) return false;
        return DdrawSet("windowed", n == 2 ? "false" : "true") &&
               DdrawSet("fullscreen", n == 0 ? "false" : "true");
    }
    if (key == "ddraw.maxfps_mode") {
        if (!ToInt(value, &n)) return false;
        if (n == 0) return DdrawSet("maxfps", "-1");
        if (n == 1) return DdrawSet("maxfps", "0");
        return true;  // custom: the maxfps row below carries the number
    }
    if (key == "ddraw.install")   { fm2k::cnc_ddraw::EnsureInstalled(); return true; }
    if (key == "ddraw.reinstall") { fm2k::cnc_ddraw::ForceReinstall();  return true; }
    if (key == "ddraw.reset")     { return fm2k::cnc_ddraw::ResetIniToDefault(); }
    if (starts("ddraw.")) {
        const std::string k = key.substr(6);
        // toggles arrive as 0/1; cnc-ddraw wants true/false for its bools.
        static const char* const kBools[] = {
            "maintas", "boxing", "border", "resizable", "vsync", "toggle_borderless",
            "toggle_upscaled", "devmode", "nonexclusive", "vhack", "adjmouse",
            "hook_peekmessage", "noactivateapp", "singlecpu", "fix_alt_key_stuck",
            "game_handles_close", "fix_not_responding", "no_compat_warning",
            "lock_surfaces", "flipclear", "rgb555", "no_dinput_hook",
            "center_cursor_fix", "lock_mouse_top_left", "limit_gdi_handles", "remove_menu"};
        for (const char* b : kBools) {
            if (k == b) return DdrawSet(b, (value == "1" || value == "true") ? "true" : "false");
        }
        if (k.rfind("key", 0) == 0) {  // hotkeys: store as 0xNN
            if (!ToInt(value, &n)) return false;
            char hex[16]; std::snprintf(hex, sizeof(hex), "0x%02X", n & 0xFF);
            return DdrawSet(k.c_str(), hex);
        }
        return DdrawSet(k.c_str(), value);  // ints and strings verbatim
    }

    // ---- flat stores ----
    if (starts("audio.")) {
        return KvSet(AudioIni(), key.substr(6), value == "1" ? "1" : "0",
                     "; FM2K Rollback audio mute state\n; the hook DLL re-reads this file ~once per second.\n");
    }
    if (key == "settings.language") {
        if (!ToInt(value, &n)) return false;
        return KvSet(SettingsIni(), "language", n == 1 ? "ja" : n == 2 ? "es" : "en");
    }
    if (key == "settings.delay_mode") {
        if (!ToInt(value, &n)) return false;
        // Same env contract as the shipping launcher's Delay combo.
        if (n >= 2) {
            ::SetEnvironmentVariableA("FM2K_LOCAL_DELAY", std::to_string(n - 2).c_str());
            ::SetEnvironmentVariableA("FM2K_DELAY_MODE", nullptr);
        } else {
            ::SetEnvironmentVariableA("FM2K_LOCAL_DELAY", nullptr);
            ::SetEnvironmentVariableA("FM2K_DELAY_MODE", n == 1 ? "1" : "0");
        }
        return KvSet(SettingsIni(), "delay_mode", std::to_string(n));
    }
    if (starts("settings.")) return KvSet(SettingsIni(), key.substr(9), value);
    if (key == "dev.boot_strategy" || starts("dev.btb_") || key == "dev.update_channel") {
        if (!ToInt(value, &n)) return false;
        return KvSet(DevFlagsIni(), key.substr(4), std::to_string(n));
    }
    if (starts("dev.")) return KvSet(DevFlagsIni(), key.substr(4), value == "1" ? "1" : "0");

    // ---- per game ----
    if (starts("cfg.")) {
        if (game_stem.empty() || !ToInt(value, &n)) return false;
        return CfgSet(game_stem, key.substr(4).c_str(), n);
    }
    if (starts("patch.")) {
        if (game_stem.empty()) return false;
        const std::string k = key.substr(6);
        const bool is_int = (k == "random_stage_min" || k == "random_stage_max" ||
                             k == "team_size" || k == "damage_multiplier_pct");
        if (is_int && !ToInt(value, &n)) return false;
        return KvSet(GamePatchesIni(game_stem), k,
                     is_int ? std::to_string(n) : (value == "1" ? "1" : "0"),
                     "# Per-game FM2K hook patches. Hand-editable.\n");
    }

    // ---- Discord ----
    if (key == "hub.discord")    return DiscordAction("signin", "");  // host set by DiscordSetHubHost
    if (key == "hub.signout")    return DiscordAction("signout", "");
    if (key == "hub.cancel")     return DiscordAction("cancel", "");
    if (key == "hub.copy_url")   return DiscordAction("copy_url", "");
    if (key == "hub.reopen")     return DiscordAction("reopen", "");
    if (key == "hub.use_discord_name") return DiscordAction(std::string("use_discord_name:") + (value == "1" ? "1" : "0"), "");

    if (key == "hub.check_updates")   { fm2k::updater::CheckForUpdates(); return true; }
    if (key == "hub.download_update") { fm2k::updater::StartDownload(); return true; }

    if (key == "tool.direct_addr") { g_direct_addr = value; return true; }
    return false;  // tool.<action>: the host runs these (host.cpp), it has the exe
}

}  // namespace hostgui
