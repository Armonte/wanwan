#pragma once
// input_binder.cpp shared state + cross-TU helpers, externed so the split
// input_binder_*.cpp TUs (gamepads / profiles / sample / ui) can share them.
// Pure linkage move from the original single-file anonymous namespace --
// definitions live in input_binder.cpp (the core). The ImGui render layer
// (input_binder_ui.cpp) is the ONLY TU a future Slint rewrite replaces;
// everything declared here is render-agnostic.
#include "input_binder.h"          // FM2KInputBinder::Bit / Binding / PlayerBindings
#include <SDL3/SDL.h>              // SDL_JoystickID / SDL_Gamepad
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FM2KInputBinder {

// ---- compile-time constants (were file-static constexpr in the anon ns) ----
inline constexpr int   kPlayers           = 2;
inline constexpr float kAxisBindThreshold = 0.45f;
inline constexpr int   kAxisSampleThreshold = 16384;  // ~50% of int16 range

// ---- shared runtime state (defined in input_binder.cpp) ----
extern PlayerBindings g_players[kPlayers];

// SDL gamepad handles we've opened, keyed by joystick instance id, plus the
// "examined and NOT a gamepad" set (so RefreshGamepadList doesn't re-open a
// vJoy stick every refresh -- the v0.2.46 1s-cadence frame hitch, #63).
extern std::vector<SDL_JoystickID> g_gamepad_ids;
extern std::unordered_map<SDL_JoystickID, SDL_Gamepad*> g_gamepad_handles;
extern std::unordered_set<SDL_JoystickID> g_non_gamepad_ids;

// Currently-active profile path (default or per-game). Set by Init/Save/Load.
extern std::string g_config_path;

// Init-once guard (core Init/Shutdown) -- also read by Sample*/RenderBody to
// no-op before the binder is warmed up. External so all TUs see it.
extern bool g_initialized;

// Active per-game profile basename ("" = default). Owned by the profiles TU;
// the ui reads it for the "use override for this game" checkbox state.
extern std::string g_active_game;

// INI key table, one per FM2K bit. Shared by profiles (Save/Load) + ui
// (BindingLabel), so it lives at external linkage rather than in either TU.
extern const char* kBitNames[(size_t)Bit::COUNT];

// ---- cross-TU helpers (were anon-ns; promoted to external) ----
void         RefreshGamepadList();        // input_binder_gamepads.cpp
const char*  GamepadNameAt(int idx);      // input_binder_gamepads.cpp
SDL_Gamepad* GamepadAt(int idx);          // input_binder_gamepads.cpp (Sample + ui)
std::string  DefaultConfigPath();         // input_binder_profiles.cpp

// ---- stable per-player device identity (input_binder_gamepads.cpp) ----
// Each player's identity-resolved pad, as an SDL joystick instance id
// (0 = unresolved / no identity). Instance ids are stable for as long as
// the device stays connected, which is what makes mid-session resolution
// sticky even between two identical pads.
extern SDL_JoystickID g_player_pad_jid[kPlayers];
// "<GUID string>[#<serial>]" for a connected instance id.
std::string  DeviceIdentityOfJid(SDL_JoystickID jid);
// Re-resolve g_player_pad_jid from each player's device_id. Keeps a
// still-connected resolution sticky; claims prevent both players from
// resolving to the same physical pad. Runs at the end of every
// RefreshGamepadList() and after Load()/SetPlayerDevice().
void         ResolvePlayerPads();
// Handle for the player's resolved pad (nullptr = none). Only meaningful
// when that player's device_id is set; legacy configs sample through
// GamepadAt(binding.gamepad_index) instead.
SDL_Gamepad* ResolvedPadForPlayer(int player_slot);

// Defaults (input_binder.cpp). ApplyDefaults seeds a player's full binding set;
// the Fill* helpers are called directly by the ui "set this pad as primary /
// stick-as-dpad" buttons, so they're external too.
void ApplyDefaults(int player);
void FillPrimaryAsGamepad(PlayerBindings& pb, int gp_idx);
void FillAltAsStickDirections(PlayerBindings& pb, int gp_idx);

}  // namespace FM2KInputBinder
