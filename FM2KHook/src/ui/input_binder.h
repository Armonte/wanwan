// input_binder.h
// Minimal ImGui-based input binder for FM2K's 11-bit input format.
// Drop-in for either the launcher (SDL3 + ImGui) or the hook DLL's overlay.
// Depends only on <SDL3/SDL.h>, <imgui.h>, and the C++ standard library.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace FM2KInputBinder {

// FM2K's vanilla 11-bit input layout (engine mask 0x7FF) plus 3 additional
// meta-bits (0x800-0x2000) used only by FM2KHook for its own features --
// the engine itself doesn't see those bits (we mask them off before
// passing input through).
//
// Bits 0-10  : standard 2DFM layout (d-pad + 6 attack buttons + Start)
// Bits 11-13 : meta-buttons (OPTION/FN1/FN2) the hook reserves for itself
//   * OPTION drives the title-screen submode cycle (VS 2P / VS CPU /
//     CPU vs CPU / Training) when option_mode_selector is enabled
//   * FN1 / FN2 are reserved for in-game features (e.g. training-mode
//     P2 behavior cycle, future overlay toggles). Unbound by default.
enum class Bit : uint8_t {
    LEFT   = 0,   // 0x0001
    RIGHT  = 1,   // 0x0002
    UP     = 2,   // 0x0004
    DOWN   = 3,   // 0x0008
    A      = 4,   // 0x0010 (button1)
    B      = 5,   // 0x0020 (button2)
    C      = 6,   // 0x0040 (button3)
    D      = 7,   // 0x0080 (button4)
    E      = 8,   // 0x0100 (button5)
    F      = 9,   // 0x0200 (button6)
    START  = 10,  // 0x0400 (button7 / start) -- engine-visible
    OPTION = 11,  // 0x0800 (FM2KHook meta -- submode cycle on title)
    FN1    = 12,  // 0x1000 (FM2KHook meta -- reserved)
    FN2    = 13,  // 0x2000 (FM2KHook meta -- reserved)
    COUNT
};

// Bitmask of engine-visible bits. Hook layer ANDs incoming input with
// this before passing to the engine's get_player_input substitute so
// meta-bits don't leak into game state.
constexpr uint16_t kEngineInputMask = 0x07FFu;

// Bitmask of all bits (engine + meta) the binder produces.
constexpr uint16_t kFullInputMask = 0x3FFFu;

// One slot per FM2K bit. A slot is either keyboard, gamepad button,
// gamepad axis, or empty -- mutually exclusive for simplicity.
struct Binding {
    enum class Source : uint8_t { NONE, KEYBOARD, GAMEPAD_BUTTON, GAMEPAD_AXIS };

    Source source        = Source::NONE;
    int    code          = 0;   // SDL_Scancode | SDL_GamepadButton | SDL_GamepadAxis
    int    axis_dir      = 0;   // +1 / -1 for GAMEPAD_AXIS, ignored otherwise
    int    gamepad_index = -1;  // index into SDL_GetGamepads(); -1 = first connected
};

struct PlayerBindings {
    // Two parallel slots per FM2K bit. Sample() OR's them together,
    // so a single bit can fire from EITHER source -- typical use cases:
    //   primary = keyboard, alt = gamepad (covers most users)
    //   primary = stick axis, alt = dpad button (CXL-style: both
    //   gamepad inputs route to the same in-game direction)
    //   primary = keyboard, alt = NONE (no gamepad / single-source)
    // Save/Load round-trips both via "<bit>" and "<bit>.alt" INI keys
    // so old configs still load (alt defaults to NONE if missing).
    Binding bits    [static_cast<size_t>(Bit::COUNT)];
    Binding bits_alt[static_cast<size_t>(Bit::COUNT)];

    // Stable device identity for this player's gamepad bindings --
    // "<SDL GUID string>[#<serial>]". When set, sampling resolves this
    // player's pad BY IDENTITY (ResolvePlayerPads) instead of through
    // the per-binding SDL list index: unplugging the OTHER player's pad
    // reorders the list, which used to shift/steal devices between
    // players mid-session. Empty = legacy index-based routing (old
    // configs). Persisted as "device_id" / "device_name" per [PlayerN].
    std::string device_id;
    std::string device_name;  // display label when the pad is disconnected
};

// Initialize: opens any visible gamepads and loads bindings from
// $FM2K_INPUT_CONFIG_PATH (or "fm2k_inputs.ini" in CWD). Falls back to
// defaults silently if no file exists. Safe to call once at startup.
void Init();

// Tear down opened gamepad handles. Optional.
void Shutdown();

// Re-enumerate connected gamepads. Picks up devices plugged in AFTER
// Init() and closes handles for devices that have been removed. Cheap
// (SDL_GetGamepads + a couple of SDL_PumpEvents); safe to call on a
// 1 s cadence from the launcher's tick + the hook's periodic poll so
// users don't have to restart the session after hot-plugging a pad
// (Suicidal Muffin's bug report). No effect if nothing changed.
// Also re-resolves each player's identity-bound pad (ResolvePlayerPads).
void RefreshGamepads();

// Stamp player_slot's device identity from the pad at SDL-list index
// `list_index` (the index shown in the device dropdown). Negative or
// out-of-range clears the identity (keyboard-only / legacy routing).
// Called by the binder UI's device dropdown and by gamepad bind-capture,
// then persisted by Save().
void SetPlayerDevice(int player_slot, int list_index);

// Current SDL-list index of the player's identity-resolved pad, or -1
// when no identity is set / the device is disconnected. UI preview.
int ResolvedPadListIndex(int player_slot);

// Render the binder window for one player slot (0 or 1). Returns true if
// any binding was modified this frame (caller may auto-Save).
bool RenderWindow(int player_slot, bool* p_open = nullptr);

// Body-only variant -- caller owns the ImGui window/tab/child container.
// Use this to embed the binder inside the launcher's consolidated
// Settings tab pane. Returns true on any binding change for the frame.
bool RenderBody(int player_slot);

// Sample current state of all bindings for player_slot, returning the
// 11-bit FM2K input mask. Call once per frame from your input source.
//
// Sample()      uses SDL3 -- for the LAUNCHER (SDL3 event pump runs).
// Sample_Win32() uses GetKeyboardState + XInput -- for the HOOK DLL where
//                 SDL3 isn't event-pumped. Both honor the same Bindings()
//                 config so launcher-bound keys work identically in-game.
//                 Focus-correct by construction: returns 0 unless this
//                 process owns the foreground window, and keyboard state
//                 is queue-synced (never GetAsyncKeyState -- that reads
//                 the desktop-global key state).
uint16_t Sample(int player_slot);
uint16_t Sample_Win32(int player_slot);

// Persistence.
// - Save() writes to the currently-active profile (default or per-game).
// - Load() reads the per-game profile if SetGameProfile() has been called
//   AND a per-game .ini exists; otherwise falls back to the default file.
bool Save();
bool Load();

// Per-game overrides. Pass the exe basename (e.g. "WonderfulWorld_ver_0946")
// to switch the active profile to fm2k_inputs_<basename>.ini under
// %APPDATA%\FM2K_Rollback\. Pass nullptr or "" to revert to the default
// profile (fm2k_inputs.ini). The new profile takes effect on the next
// Load(); current in-memory bindings stay until the caller chooses to
// reload. Idempotent -- re-setting the same name is a no-op.
void SetGameProfile(const char* exe_basename);

// Returns true if a per-game override is currently active (i.e.
// SetGameProfile was called with a non-empty name AND that file exists
// on disk). Useful for "Use default for this game" toggle UI.
bool HasGameProfile();

// Forks the default profile into the current per-game profile so the
// user can edit them independently. Requires SetGameProfile() to have
// already routed to a per-game name. Returns false if no per-game name
// is active (or copy failed). On success the per-game .ini exists on
// disk and HasGameProfile() returns true.
bool ForkDefaultToGameProfile();

// Deletes the per-game profile file and reverts the active profile to
// default. Use case: user clicks "Use default for this game" -- drops
// the per-game file and re-Loads from default. Returns false if no
// per-game profile was active.
bool DeleteGameProfile();

// Path of the currently-active profile (for diagnostic UI / logging).
const char* CurrentConfigPath();

// Direct access (for diagnostic UI, etc.).
PlayerBindings& Bindings(int player_slot);

}  // namespace FM2KInputBinder
