// host_settings.h -- the Config view's data, read from and written to THE
// LAUNCHER'S REAL STORES. No spike-private settings file: every row here is
// a key in a file the shipping ImGui launcher (or the hook) already reads,
// so the two launchers cannot drift and a user switching between them keeps
// their settings. The stores (docs/dev/settings_audit_and_config_ia_2026-09-02.md):
//
//   %APPDATA%\FM2K_Rollback\settings.ini    language, notifications, delay
//   %APPDATA%\FM2K_Rollback\audio.ini       bgm_muted, se_muted (hook polls it)
//   %APPDATA%\FM2K_Rollback\dev_flags.ini   developer flags, channel, stealth,
//                                            diagnostics upload
//   %APPDATA%\FM2K_Rollback\game_configs\<stem>.ini   per-game match settings
//   %APPDATA%\FM2K_Rollback\game_patches\<stem>.ini   random stage + patches
//   %APPDATA%\FM2K_Rollback\fm2k_inputs.ini per-player SOCD (binder profile)
//   <FM2K_DDRAW_DIR | launcher dir>\cnc-ddraw\ddraw.ini   all 60 cnc-ddraw keys
//
// The UI never learns where a setting lives. It renders rows and reports
// "key changed to value"; this file owns the mapping in both directions.
#pragma once
#include <string>

namespace hostgui {

// Every row of the Config view as TSV, one row per line:
//   tab \t key \t kind \t label \t hint \t value \t value_text \t count \t advanced \t enabled
// tab: 0 display 1 input 2 audio 3 network 4 hub 5 game 6 developer.
// kind: 0 toggle, 1 int, 2 choice, 3 text, 4 vkey, 5 action, 6 note,
//       7 group header, 9 "show advanced" toggle.
// `game_stem` names the selected game (exe stem == game id); empty means
// the GAME tab says so instead of guessing.
std::string SettingsRows(const std::string& game_stem);

// Write one UI edit to its store. Returns false when no writer exists for
// the key -- the caller routes that through HOST_TODO so it is loud.
// The direct-IP address typed on the DEVELOPER tab (tool.direct_addr).
std::string ToolsDirectAddr();

bool ApplySetting(const std::string& key, const std::string& value,
                  const std::string& game_stem);

// The spike's developer-tools gate: dev_flags.ini `developer_ui`.
bool DevToolsEnabled();

// A small integer that changes whenever the cnc-ddraw installer's state or
// progress changes; the host republishes the rows when it does, so the
// DISPLAY tab shows a download as it happens.
int InstallerStamp();

// True when the Discord pairing state moved this frame (see host_discord.h).
bool SettingsTick();

// Stop the installer's worker thread (before SDL_Quit).
void SettingsShutdown();

}  // namespace hostgui
