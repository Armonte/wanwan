// launcher_ui_settings_io.cpp -- shared launcher-UI persistence helpers, split
// out of FM2K_LauncherUI.cpp. Holds the dev_flags.ini, per-game patches ini,
// and settings.ini primitives that several panels read/write. Definitions are
// wrapped in `namespace lui` (declared in launcher_ui_internal.h); the panels
// reach them via `using namespace lui;`. Pure move -- top-level `static` was
// stripped so the namespace gives them external linkage across the TUs.
#include "launcher_ui_internal.h"
#include "FM2K_Integration.h"
#include "FM2K_Utf8Path.h"
#include <string>
#include <vector>
#include <utility>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <windows.h>

namespace lui {

bool g_auto_upload_logs = true;

std::string AudioIniPath() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\audio.ini";
}

// Dev-flag persistence. Same flat key=value format as audio.ini. Currently
// just stores `eb_diag=` so the [EB] palette/shake diagnostic toggle
// survives launcher restarts.
std::string DevFlagsIniPath() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\dev_flags.ini";
}

bool LoadDevFlag(const char* key, bool default_val) {
    const std::string path = DevFlagsIniPath();
    if (path.empty()) return default_val;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return default_val;
    char line[128];
    bool result = default_val;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        if (s.substr(0, eq) != key) continue;
        const std::string v = s.substr(eq + 1);
        result = (v == "1" || v == "true" || v == "yes" || v == "on");
    }
    std::fclose(f);
    return result;
}

void SaveDevFlag(const char* key, bool value) {
    const std::string path = DevFlagsIniPath();
    if (path.empty()) return;
    // Read all existing keys, replace this one, write back. Tiny file --
    // a few keys at most -- so brute-force rewrite is fine.
    std::vector<std::pair<std::string, std::string>> kv;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                  s.back() == ' '  || s.back() == '\t')) s.pop_back();
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
        }
        std::fclose(f);
    }
    bool found = false;
    for (auto& p : kv) if (p.first == key) { p.second = value ? "1" : "0"; found = true; }
    if (!found) kv.emplace_back(key, value ? "1" : "0");
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (const auto& p : kv) std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        std::fclose(f);
    }
}

int LoadDevFlagInt(const char* key, int default_val) {
    const std::string path = DevFlagsIniPath();
    if (path.empty()) return default_val;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return default_val;
    char line[128];
    int result = default_val;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        if (s.substr(0, eq) != key) continue;
        result = std::atoi(s.substr(eq + 1).c_str());
    }
    std::fclose(f);
    return result;
}

void SaveDevFlagInt(const char* key, int value) {
    const std::string path = DevFlagsIniPath();
    if (path.empty()) return;
    std::vector<std::pair<std::string, std::string>> kv;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                  s.back() == ' '  || s.back() == '\t')) s.pop_back();
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
        }
        std::fclose(f);
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", value);
    bool found = false;
    for (auto& p : kv) if (p.first == key) { p.second = buf; found = true; }
    if (!found) kv.emplace_back(key, buf);
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (const auto& p : kv) std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        std::fclose(f);
    }
}

// =============================================================================
// PER-GAME PATCH PERSISTENCE
// =============================================================================
//
// Each game has its own INI in %APPDATA%\FM2K_Rollback\game_patches\<game_id>.ini,
// extending the dev_flags.ini pattern but per-game so different FM2K games
// can opt into experimental hook-side patches independently. The launcher
// edits these via the Host Config panel; the game's hook DLL receives the
// settings as env vars (set by ApplyGamePatchEnvVars before each launch).
//
// Hand-editable; missing keys → hardcoded default (recommended-on for fixes).

std::string GamePatchesDir() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    const std::string parent = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(parent.c_str(), nullptr);
    const std::string dir = parent + "\\game_patches";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string GamePatchesIniPath(const std::string& game_id) {
    if (game_id.empty()) return "";
    const std::string dir = GamePatchesDir();
    if (dir.empty()) return "";
    return dir + "\\" + game_id + ".ini";
}

std::vector<std::pair<std::string, std::string>>
ReadGamePatchesKv(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> kv;
    if (path.empty()) return kv;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return kv;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
    }
    std::fclose(f);
    return kv;
}

void WriteGamePatchesKv(const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& kv) {
    if (path.empty()) return;
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fprintf(f, "# Per-game FM2K hook patches. Hand-editable.\n");
        std::fprintf(f, "# Edited via the launcher's Host Config panel.\n");
        for (const auto& p : kv) {
            std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        }
        std::fclose(f);
    }
}

bool LoadGamePatchBool(const std::string& game_id, const char* key,
                              bool default_val) {
    const auto kv = ReadGamePatchesKv(GamePatchesIniPath(game_id));
    for (const auto& p : kv) {
        if (p.first == key) {
            return (p.second == "1" || p.second == "true" ||
                    p.second == "yes" || p.second == "on");
        }
    }
    return default_val;
}

int LoadGamePatchInt(const std::string& game_id, const char* key,
                            int default_val) {
    const auto kv = ReadGamePatchesKv(GamePatchesIniPath(game_id));
    for (const auto& p : kv) {
        if (p.first == key) {
            try { return std::stoi(p.second); }
            catch (...) { return default_val; }
        }
    }
    return default_val;
}

void SaveGamePatchString(const std::string& game_id, const char* key,
                                const std::string& value) {
    const std::string path = GamePatchesIniPath(game_id);
    auto kv = ReadGamePatchesKv(path);
    bool found = false;
    for (auto& p : kv) {
        if (p.first == key) { p.second = value; found = true; break; }
    }
    if (!found) kv.emplace_back(key, value);
    WriteGamePatchesKv(path, kv);
}

void SaveGamePatchBool(const std::string& game_id, const char* key,
                              bool value) {
    SaveGamePatchString(game_id, key, value ? "1" : "0");
}

void SaveGamePatchInt(const std::string& game_id, const char* key,
                             int value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", value);
    SaveGamePatchString(game_id, key, buf);
}

// Compute the canonical game_id from an exe path. Mirrors the
// Python-side registry's exe-stem convention so launcher INIs and
// registry.json line up 1:1.
std::string GameIdForExePath(const std::string& exe_path_utf8) {
    if (exe_path_utf8.empty()) return "";
    std::filesystem::path p(fm2k::utf8path::Utf8ToWide(exe_path_utf8));
    return fm2k::utf8path::StemUtf8(p);
}

// Every env var ApplyGamePatchEnvVars owns. Neutralize and Apply share
// this table so a patch added to one can't silently drift out of the
// other (a stale var surviving Neutralize = the leak described below).
static const char* const kGamePatchEnvVars[] = {
    "FM2K_KEEP_GAMESPEED_PIC",
    "FM2K_TEAM_CSS_DUPE_LOCK",
    "FM2K_TEAM_KOF_RETENTION",
    "FM2K_TEAM_SIZE",
    "FM2K_DAMAGE_MULT_PCT",
    "FM2K_VS_CPU_MODE",
    "FM2K_CPU_VS_CPU_MODE",
    "FM2K_TRAINING_MODE",
    "FM2K_OPTION_MODE_SELECTOR",
};

// Clear every per-game patch env var. MUST run before any NON-offline
// launch (online, stress, spectate): these vars are process-level on
// the launcher and inherited by every spawned game, so without this a
// prior OFFLINE launch's settings (training mode, team size, damage
// multiplier...) leaked into the next netplay match -- sim-affecting
// ones (team_size, damage_mult) only on THIS peer's side, i.e. a
// guaranteed desync; training/vs-cpu leaks zeroed P2-style inputs.
// Online stays "vanilla + hook defaults" by design until per-game
// patches are host-synced. (FM2K_KEEP_GAMESPEED_PIC is opt-out -- the
// hook's render fix defaults ON, so clearing keeps the fix active.)
void NeutralizeGamePatchEnvVars() {
    for (const char* v : kGamePatchEnvVars) {
        ::SetEnvironmentVariableA(v, nullptr);
    }
}

// Apply per-game patch env vars before launching the game OFFLINE (the
// netcfg Play-Offline button). Online / stress / spectate paths call
// NeutralizeGamePatchEnvVars() instead -- per-game patches are local
// house rules and are not synced between peers yet. Each new patch
// added to the per-game INI gets a corresponding env var set here AND
// an entry in kGamePatchEnvVars above.
void ApplyGamePatchEnvVars(const std::string& game_id) {
    // ---- IMPLEMENTED ----

    // gs_pic_fix: OPT-OUT (fix is on by default in the hook).
    const bool gs_pic_fix = LoadGamePatchBool(game_id, "gs_pic_fix", true);
    ::SetEnvironmentVariableA("FM2K_KEEP_GAMESPEED_PIC",
                              gs_pic_fix ? nullptr : "1");

    // team_css_dupe_lock: OPT-IN -- masks confirm bits when the cursor
    // would land on an already-locked team slot.
    const bool team_css_dupe_lock =
        LoadGamePatchBool(game_id, "team_css_dupe_lock", false);
    ::SetEnvironmentVariableA("FM2K_TEAM_CSS_DUPE_LOCK",
                              team_css_dupe_lock ? "1" : nullptr);

    // team_kof_retention: OPT-IN -- winner's HP/meter carries into next
    // round (loser gets fresh char). Team mode only.
    const bool team_kof_retention =
        LoadGamePatchBool(game_id, "team_kof_retention", false);
    ::SetEnvironmentVariableA("FM2K_TEAM_KOF_RETENTION",
                              team_kof_retention ? "1" : nullptr);

    // team_size: int 2..4, default 0 (engine default). Hard ceiling is
    // 4 per side -- the engine's CSS indexes its 8-slot character data
    // pool as 4*player_idx + round_count, so N>4 stomps the opposite
    // player's slots. See per_game_patches.cpp for the full analysis.
    const int team_size = LoadGamePatchInt(game_id, "team_size", 0);
    if (team_size >= 2 && team_size <= 4) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", team_size);
        ::SetEnvironmentVariableA("FM2K_TEAM_SIZE", buf);
    } else {
        ::SetEnvironmentVariableA("FM2K_TEAM_SIZE", nullptr);
    }

    // damage_multiplier_pct: int 1..1000, default 100 (no scaling).
    const int dmg_mult = LoadGamePatchInt(game_id, "damage_multiplier_pct", 100);
    if (dmg_mult != 100 && dmg_mult >= 1 && dmg_mult <= 1000) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", dmg_mult);
        ::SetEnvironmentVariableA("FM2K_DAMAGE_MULT_PCT", buf);
    } else {
        ::SetEnvironmentVariableA("FM2K_DAMAGE_MULT_PCT", nullptr);
    }

    // ---- STUBS (toggles wired through to env vars; hook-side TODO) ----
    // Each of these sets the env var so the hook can pick it up once the
    // implementation lands. Until then the hook just logs a warning when
    // it sees one of these set.

    const bool vs_cpu_mode =
        LoadGamePatchBool(game_id, "vs_cpu_mode", false);
    ::SetEnvironmentVariableA("FM2K_VS_CPU_MODE",
                              vs_cpu_mode ? "1" : nullptr);

    const bool cpu_vs_cpu_mode =
        LoadGamePatchBool(game_id, "cpu_vs_cpu_mode", false);
    ::SetEnvironmentVariableA("FM2K_CPU_VS_CPU_MODE",
                              cpu_vs_cpu_mode ? "1" : nullptr);

    const bool training_mode =
        LoadGamePatchBool(game_id, "training_mode", false);
    ::SetEnvironmentVariableA("FM2K_TRAINING_MODE",
                              training_mode ? "1" : nullptr);

    const bool option_mode_selector =
        LoadGamePatchBool(game_id, "option_mode_selector", false);
    ::SetEnvironmentVariableA("FM2K_OPTION_MODE_SELECTOR",
                              option_mode_selector ? "1" : nullptr);
}

std::string NotifySettingsPath() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\settings.ini";
}

bool ReadBoolSetting(const std::string& path, const char* key, bool dflt) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return dflt;
    char line[256];
    bool out = dflt;
    while (std::fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* k = p;
        size_t klen = std::strlen(k);
        while (klen > 0 && (k[klen-1] == ' ' || k[klen-1] == '\t')) k[--klen] = '\0';
        if (std::strcmp(k, key) != 0) continue;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        size_t vlen = std::strlen(v);
        while (vlen > 0 && (v[vlen-1] == '\n' || v[vlen-1] == '\r' ||
                            v[vlen-1] == ' '  || v[vlen-1] == '\t')) v[--vlen] = '\0';
        out = (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0
            || std::strcmp(v, "yes") == 0 || std::strcmp(v, "on") == 0);
        break;
    }
    std::fclose(f);
    return out;
}

// Tiny int-keyed setting reader/writer -- same flat key=value format as
// the bool helpers; integers like SOCD mode use this. Default returned
// when the key is missing or the value isn't a valid int.
int ReadIntSetting(const std::string& path, const char* key, int dflt) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return dflt;
    char line[256];
    int out = dflt;
    while (std::fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* k = p;
        size_t klen = std::strlen(k);
        while (klen > 0 && (k[klen-1] == ' ' || k[klen-1] == '\t')) k[--klen] = '\0';
        if (std::strcmp(k, key) != 0) continue;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        char* endp = nullptr;
        long parsed = std::strtol(v, &endp, 10);
        if (endp && endp != v) out = (int)parsed;
        break;
    }
    std::fclose(f);
    return out;
}

void WriteIntSetting(const std::string& path, const char* key, int value) {
    std::vector<std::pair<std::string, std::string>> kv;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                  s.back() == ' '  || s.back() == '\t')) s.pop_back();
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
        }
        std::fclose(f);
    }
    bool found = false;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", value);
    for (auto& p : kv) if (p.first == key) { p.second = buf; found = true; }
    if (!found) kv.emplace_back(key, buf);
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (const auto& p : kv) std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        std::fclose(f);
    }
}

void WriteBoolSetting(const std::string& path, const char* key, bool value) {
    // Read all keys, replace ours, rewrite. Tiny file, tiny number of keys --
    // brute force is fine and keeps the format stable.
    std::vector<std::pair<std::string, std::string>> kv;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                  s.back() == ' '  || s.back() == '\t')) s.pop_back();
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
        }
        std::fclose(f);
    }
    bool found = false;
    for (auto& p : kv) if (p.first == key) { p.second = (value ? "1" : "0"); found = true; }
    if (!found) kv.emplace_back(key, value ? "1" : "0");
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (const auto& p : kv) std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        std::fclose(f);
    }
}

}  // namespace lui
