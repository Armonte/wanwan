#pragma once

// FM2K_CncDDraw — bundled cnc-ddraw installer / updater.
//
// The launcher ships next to a `cnc-ddraw/` subdirectory that contains a
// renamed cnc-ddraw build (`2DFMD.dll`) plus its `ddraw.ini` and
// `Shaders/`. `FM2K_DDrawRedirect` patches the suspended game's IAT to
// look for that name and prepends this folder to the child PATH. This
// module owns getting the bits onto disk: downloading the PINNED GitHub
// release (`kPinnedTag` in the .cpp — the hook DLL's fullscreen-toggle
// offsets are validated against that exact build), extracting it, and
// renaming `ddraw.dll` → `2DFMD.dll`.
//
// Strict download-only — no embedded fallback. First run requires
// network. Subsequent launches skip the work if `version.txt` matches
// the pinned release (no network needed once installed).
//
// Mirrors `FM2K_Updater`'s state-machine + worker-thread shape so the UI
// pill can be rendered identically. All HTTP via WinHTTP, all archive
// reading via the vendored miniz (SDL_image's copy, included with our
// own defines so we get stdio APIs).

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

namespace fm2k::cnc_ddraw {

enum class State {
    Idle,             // boot — no check yet
    Checking,         // comparing local version.txt against the pinned tag
    NotInstalled,     // checked but no local install + offline / failed
    UpToDate,         // local version == pinned release
    UpdateAvailable,  // local < remote (or no local)
    Downloading,      // zip download in flight
    Extracting,       // unzipping + renaming ddraw.dll -> 2DFMD.dll
    Ready,            // install done, redirect can be enabled
    Failed,           // anything went wrong; check error_detail
};

struct Snapshot {
    State        state            = State::Idle;
    std::string  local_version;    // from <install_dir>/version.txt; "" if missing
    std::string  remote_version;   // GitHub `tag_name`, leading 'v' stripped
    uint32_t     downloaded_bytes = 0;
    uint32_t     total_bytes      = 0;
    std::string  error_detail;
};

// Resolve <launcher_exe_dir>/cnc-ddraw (UTF-8). Same logical location as
// `FM2K_DDrawRedirect::ResolveCncDdrawDir`, but exposed in UTF-8 here to
// match the rest of this module's WinHTTP / stdio surface. No trailing
// separator. Returns empty string if GetModuleFileName failed.
std::string InstallDir();

// Path to `<install_dir>/version.txt` (UTF-8). The file holds the
// installed cnc-ddraw `tag_name` minus the leading 'v'. Written last
// during install so partial extracts are detectable (no file = treat
// as not installed).
std::string VersionFilePath();

// Read the locally-installed version. Empty string if `version.txt`
// is missing, empty, or unreadable.
std::string ReadLocalVersion();

// Path to the canonical renamed dll, `<install_dir>/2DFMD.dll` (UTF-8).
// Existence of this file alongside `version.txt` is the integrity check
// the launcher uses to decide "install is good."
std::string DllPath();

// Background "make sure cnc-ddraw is installed at the pinned version."
// If the local version differs from the pin (or no install present),
// downloads and extracts — newer local installs are intentionally
// snapped BACK to the pin. Also migrates managed ddraw.ini keys in
// place (see MigrateManagedIniKeys in the .cpp). Idempotent: no-op
// if a worker is already running. UI polls Get() each frame.
void EnsureInstalled();

// Force a fresh download even if local matches remote — for the
// "Reinstall cnc-ddraw" debug button. Same worker pipeline.
void ForceReinstall();

// Snapshot for UI rendering.
Snapshot Get();

// Cancel any in-flight worker and join. Called from launcher shutdown.
void Shutdown();

// ─── ddraw.ini editing surface ─────────────────────────────────────────
//
// Phase-E launcher UI exposes every cnc-ddraw [ddraw] setting under
// Settings → Display. State lives in `IniConfig`; the UI reads via
// LoadIni() once on tab open and writes per-key on every widget change
// using the Save* helpers below. Writes go through Win32
// `WritePrivateProfileString` (same API cnc-ddraw uses internally at
// config.c:147), so unknown keys + per-game `[<exe_name>]` blocks the
// user might add are preserved across our edits.

// Path to <install_dir>\ddraw.ini (UTF-8).
std::string IniPath();

// Overwrite <install_dir>\ddraw.ini with the launcher's baked-in
// `kDefaultIni`. Wipes user tuning AND any per-game [<exe>] blocks —
// warning is in the UI, not the function.
bool ResetIniToDefault();

// Mirror of every [ddraw] key cnc-ddraw recognizes. Fields default to
// the same fallbacks cnc-ddraw uses when keys are missing.
struct IniConfig {
    // ── Display / window ─────────────────────────────────────────────
    int  width = 0;
    int  height = 0;
    bool fullscreen = false;
    bool windowed = true;
    bool maintas = false;
    std::string aspect_ratio;
    bool boxing = false;
    int  maxfps = 0;
    bool vsync = false;
    bool adjmouse = false;
    std::string shader = "Shaders\\interpolation\\catmull-rom-bilinear.glsl";
    int  posX = -32000;
    int  posY = -32000;
    std::string renderer = "direct3d9";
    bool devmode = true;
    bool border = true;
    int  savesettings = 1;
    bool resizable = true;
    int  d3d9_filter = 0;
    int  anti_aliased_fonts_min_size = 13;
    int  min_font_size = 0;
    int  center_window = 1;
    std::string inject_resolution;
    bool vhack = false;
    std::string screenshotdir = ".\\Screenshots\\";
    bool toggle_borderless = false;
    bool toggle_upscaled = false;

    // ── Compatibility ────────────────────────────────────────────────
    bool noactivateapp = false;
    int  maxgameticks = -1;
    int  limiter_type = 0;
    int  minfps = 0;
    bool nonexclusive = false;
    bool singlecpu = false;
    int  resolutions = 0;
    int  fixchilds = 2;
    bool hook_peekmessage = false;

    // ── Undocumented / advanced ──────────────────────────────────────
    bool fix_alt_key_stuck = false;
    bool game_handles_close = false;
    bool fix_not_responding = false;
    bool no_compat_warning = false;
    int  guard_lines = 200;
    int  max_resolutions = 0;
    bool lock_surfaces = false;
    bool flipclear = false;
    bool rgb555 = false;
    bool no_dinput_hook = false;
    bool center_cursor_fix = false;
    bool lock_mouse_top_left = false;
    int  hook = 4;
    bool limit_gdi_handles = false;
    bool remove_menu = false;
    int  refresh_rate = 0;

    // ── Hotkeys (Virtual-Key codes; 0 = disabled) ────────────────────
    int  keytogglefullscreen = 0x0D;
    int  keytogglefullscreen2 = 0x00;
    int  keytogglemaximize = 0x22;
    int  keytogglemaximize2 = 0x00;
    int  keyunlockcursor1 = 0x09;
    int  keyunlockcursor2 = 0xA3;
    int  keyscreenshot = 0x2C;
};

// Read every [ddraw] key into `out`. Missing keys retain field defaults.
// Accepts both "true"/"false" and "1"/"0" for booleans.
void LoadIni(IniConfig& out);

// Per-key write helpers. Each writes one [ddraw] key. SaveBool emits
// "true"/"false" (cnc-ddraw idiom), SaveHex emits "0xNN" for VK codes.
void SaveBool(const char* key, bool val);
void SaveInt(const char* key, int val);
void SaveHex(const char* key, int val);
void SaveString(const char* key, const std::string& val);

} // namespace fm2k::cnc_ddraw
