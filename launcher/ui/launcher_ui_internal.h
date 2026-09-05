#pragma once
// Shared file-scope persistence helpers for the split launcher_ui_*.cpp TUs.
// Pure linkage move out of FM2K_LauncherUI.cpp: definitions live in
// launcher_ui_settings_io.cpp, wrapped in `namespace lui`. Each panel TU does
// `using namespace lui;` so the moved LauncherUI:: method bodies keep calling
// these unqualified, exactly as they did when everything was one file.
#include <string>
#include <vector>
#include <utility>

namespace lui {

// dev_flags.ini (Dev-tools checkboxes) + the auto-upload mirror flag the
// upload-queue poll reads directly.
extern bool g_auto_upload_logs;
std::string AudioIniPath();
std::string DevFlagsIniPath();
bool LoadDevFlag(const char* key, bool default_val);
void SaveDevFlag(const char* key, bool value);
int  LoadDevFlagInt(const char* key, int default_val);
void SaveDevFlagInt(const char* key, int value);

// Per-game patch overrides ini (host-config panel writes, launch path reads).
std::string GamePatchesDir();
std::string GamePatchesIniPath(const std::string& game_id);
std::vector<std::pair<std::string, std::string>> ReadGamePatchesKv(const std::string& path);
void WriteGamePatchesKv(const std::string& path,
                        const std::vector<std::pair<std::string, std::string>>& kv);
// Shipped-distribution default layer, read from
// <exedir>\game_patch_defaults\<game_id>.ini (same flat key=value format as
// the %APPDATA% per-game file, never written back to). Lets a bundled client
// ship a patch ON for its own game without changing the built-in default for
// every other FM2K title. Pass the conservative built-in as `builtin_default`;
// feed the result in as the `dflt` of LoadGamePatchBool so the user's own
// %APPDATA% setting still wins.
std::string GamePatchDistDefaultsPath(const std::string& game_id);
bool GamePatchDistDefaultBool(const std::string& game_id, const char* key,
                              bool builtin_default);
bool LoadGamePatchBool(const std::string& game_id, const char* key, bool dflt);
int  LoadGamePatchInt(const std::string& game_id, const char* key, int dflt);
void SaveGamePatchString(const std::string& game_id, const char* key, const std::string& value);
void SaveGamePatchBool(const std::string& game_id, const char* key, bool value);
void SaveGamePatchInt(const std::string& game_id, const char* key, int value);
std::string GameIdForExePath(const std::string& exe_path_utf8);
void ApplyGamePatchEnvVars(const std::string& game_id);
// Clear every ApplyGamePatchEnvVars-owned env var. MUST be called by the
// non-offline session paths (online / stress / spectate) before spawning:
// the vars are launcher-process-level and inherited by every child, so a
// prior offline launch's per-game patches (team size, damage multiplier,
// training...) would otherwise leak into a netplay match one-sided.
void NeutralizeGamePatchEnvVars();
// Same contract for the offline-only developer launch flags (boot
// strategy, auto title skip, bypass trampoline, T4 patch/probe, skip
// VS-mode patch): call beside NeutralizeGamePatchEnvVars on every
// non-offline path.
void NeutralizeDevLaunchEnvVars();

// settings.ini primitives (notifications, SOCD, random-stage all share it).
std::string NotifySettingsPath();
bool ReadBoolSetting(const std::string& path, const char* key, bool dflt);
int  ReadIntSetting(const std::string& path, const char* key, int dflt);
void WriteIntSetting(const std::string& path, const char* key, int value);
void WriteBoolSetting(const std::string& path, const char* key, bool value);

}  // namespace lui
