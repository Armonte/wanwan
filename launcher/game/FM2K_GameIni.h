#pragma once

// Per-game [GamePlay] config in FM2K's `game.ini` (lives next to the
// .exe). The launcher reads this to surface match settings to the host
// (#53), syncs the host's resolved values to the client on challenge
// (#54), and applies per-game user overrides on launch (#55).
//
// Format (from refgame/game.ini and CPW.exe samples):
//
//   [GamePlay]
//   Editor.TestPlay.Player0.cpu=0
//   Editor.TestPlay.Player1.cpu=0
//   Editor.TestPlay.GameSpeed=10
//   Editor.TestPlay.HitJudge=0
//   Editor.TestPlay.GameInformation=0
//   Editor.TestPlay.StageNb=0
//   Editor.TestPlay.JoyStick=1
//   Editor.TestPlay.time=60
//   Editor.TestPlay.exit=0
//   Editor.TestPlay.VSMode=0
//   Editor.TestPlay.VSSinglePlay=3
//   Editor.TestPlay.VSTeamPlay=3
//   ... (KEY / JOY input mappings + window geometry follow but are
//        not our concern -- we read+rewrite only the [GamePlay] keys)
//
// `Save` preserves every other section + every non-[GamePlay] line in
// place; the FM2K editor / game writes its own keys (keyboard maps with
// CP932 byte sequences, window geometry) and we don't disturb them.

#include <filesystem>
#include <string>

namespace fm2k::game_ini {

// Sentinel for "field absent in the source file" so callers can tell
// "user explicitly set this to 0" from "key wasn't in the INI." The hub
// sync path (#54) only includes fields the host actually has set; the
// override editor (#55) shows blank for unset fields rather than 0.
constexpr int kUnset = -1;

struct GamePlayConfig {
    int player0_cpu       = kUnset;   // 0/1
    int player1_cpu       = kUnset;
    int game_speed        = kUnset;   // 1..16, default 10
    int hit_judge         = kUnset;   // hit-box debug overlay (anti-cheat: force 0 online)
    int game_information  = kUnset;   // damage debug overlay (anti-cheat: force 0 online)
    int stage_nb          = kUnset;   // stage index, 0 = first
    int joystick          = kUnset;   // 0 = keyboard, 1 = pad
    int time              = kUnset;   // round timer in seconds, 0 = infinite
    int exit_flag         = kUnset;
    int vs_mode           = kUnset;
    int vs_single_play    = kUnset;   // round count for 1v1
    int vs_team_play      = kUnset;   // round count for team
    // Top-level [GamePlay] keys (no Editor.TestPlay. prefix). FM2K reads
    // GameScreenMode at launch: 0 = windowed, 1 = fullscreen. We pin it
    // to 1 when cnc-ddraw is in front of the game so cnc-ddraw owns the
    // window mode (its `windowmode` ini key picks borderless / windowed /
    // exclusive). The GameWindow* keys are managed by the FM2K editor
    // and we leave them alone.
    int game_screen_mode  = kUnset;   // 0/1

    bool any_set() const {
        return player0_cpu != kUnset || player1_cpu != kUnset ||
               game_speed != kUnset || hit_judge != kUnset ||
               game_information != kUnset || stage_nb != kUnset ||
               joystick != kUnset || time != kUnset ||
               exit_flag != kUnset || vs_mode != kUnset ||
               vs_single_play != kUnset || vs_team_play != kUnset ||
               game_screen_mode != kUnset;
    }
};

// Locate the game's `game.ini` next to the given .exe. Returns the
// resolved path or empty if no candidate exists. We check both
// "<dir>/game.ini" (most FM2K games) and "<dir>/2dfm.ini" (some
// distributions ship with the editor's name).
std::filesystem::path PathForExe(const std::filesystem::path& exe_path);

// Parse [GamePlay]. Returns true on success (file existed + was
// readable). Missing fields stay at kUnset; the file is allowed to
// contain only some of the keys.
bool Load(const std::filesystem::path& ini_path, GamePlayConfig& out);

// Rewrite [GamePlay] in place, preserving every other section and every
// unrelated [GamePlay] line that we don't manage (forward-compatible
// with future FM2K keys). Fields at kUnset are removed from the file
// rather than written as -1.
//
// Atomic: writes to <ini>.tmp then renames over the original, so a
// crashed write doesn't leave a half-truncated game.ini.
bool Save(const std::filesystem::path& ini_path, const GamePlayConfig& cfg);

// Apply the anti-cheat clamps for online play. HitJudge + GameInformation
// are debug overlays that show hit-boxes and damage numbers; these stay
// useful for offline practice but are cheating in online matches. The
// host calls this on the resolved config before sending to the peer
// (#54) and before launching the game (#55), so neither side ever sees
// the overlays mid-match. JoyStick is left alone -- it's a local input
// preference, not a competitive concern.
void ForceOnlineClamps(GamePlayConfig& cfg);

// ─── Per-game override store (#55) ───────────────────────────────────
//
// Per-game user overrides live at %APPDATA%\FM2K_Rollback\game_configs\
// <exe_stem>.ini in the same [GamePlay] format as the game's own
// game.ini. Loading is two-tier:
//   1. game.ini next to the exe (defaults from the game author)
//   2. our override INI (user's per-game tweaks)
// Values from (2) win where they're set (!= kUnset).
//
// Apply happens at launch time: the resolved (defaults + overrides +
// online clamps) config is written into the GAME's game.ini before
// FM2K_GameInstance::Launch fires CreateProcess, since the game reads
// its own ini at startup. We restore the original game.ini on session
// stop so the user's untouched local install isn't permanently muted
// by our online clamps.

// Build the per-game override path. Empty string if APPDATA is unset.
// Caller-friendly: takes any exe path (absolute, with or without ext)
// and emits the canonical override file.
std::filesystem::path OverridePathForExe(const std::filesystem::path& exe_path);

// Resolve effective config for a given exe by layering override on
// defaults. `out` ends up with kUnset for any key neither file sets.
// `defaults_out` (optional) gets the game's untouched defaults -- useful
// for the editor UI to show "default" vs "override" distinctly.
bool LoadResolved(const std::filesystem::path& exe_path,
                  GamePlayConfig& out,
                  GamePlayConfig* defaults_out = nullptr);

// Convenience: load just the override file (no merge). For the editor.
bool LoadOverride(const std::filesystem::path& exe_path,
                  GamePlayConfig& out);

// Persist the override file. Empty config → file removed (so "delete
// override" is just SaveOverride with an empty struct).
bool SaveOverride(const std::filesystem::path& exe_path,
                  const GamePlayConfig& cfg);

// Apply the resolved (defaults + overrides + optional online clamps)
// config to the game's own game.ini just before spawning it. Backs up
// the original to <game.ini>.fm2krollback_bak on first call so we
// don't permanently mangle the user's local install. is_online=true
// triggers ForceOnlineClamps() before write so anti-cheat overlays
// get zeroed for hub matches but stay editable offline.
//
// Returns true on success. No-op when the override file doesn't exist
// AND online clamps wouldn't change anything (saves an unnecessary
// rewrite on every launch).
bool ApplyForLaunch(const std::filesystem::path& exe_path, bool is_online);

// Force `[GamePlay] GameScreenMode=1` (FM2K's fullscreen mode) in the
// game's ini before launch. Used when the cnc-ddraw redirect is active --
// pinning FM2K to fullscreen lets cnc-ddraw own actual window-mode
// presentation (its own `windowmode` ini key picks borderless / windowed /
// exclusive). Without this pin a windowed FM2K would compose against
// cnc-ddraw's own windowmode and the result is racy.
//
// Idempotent if already 1. Reuses the same `.fm2krollback_bak` backup
// file that `ApplyForLaunch` creates, so the two can coexist within a
// single session and `RestoreFromBackup` unwinds either or both.
bool ForceFullscreenForLaunch(const std::filesystem::path& exe_path);

// Restore the pristine game.ini from the backup made by ApplyForLaunch.
// Called from FM2KLauncher::StopSession so leaving the launcher doesn't
// leave the user's game permanently configured for online play. Safe
// to call when no backup exists (no-op).
bool RestoreFromBackup(const std::filesystem::path& exe_path);

}  // namespace fm2k::game_ini
