// host_binder.h -- the controller binder, host side. The BINDER CORE is the
// shipping one (FM2KHook/src/ui/input_binder{,_gamepads,_profiles,_sample}.cpp,
// compiled into this exe): same profile files, same device identity, same
// defaults, same sampling the injected hook uses in-game. Only the ImGui
// render layer is replaced -- by rows this file publishes and actions it
// applies, rendered by ui/input.slint.
#pragma once
#include <string>

namespace hostgui {

// Start the binder (opens gamepads, loads the profile). `game_stem` selects
// the per-game profile (fm2k_inputs_<stem>.ini) when one exists.
void BinderInit(const std::string& game_stem);
void BinderSetGame(const std::string& game_stem);

// One line per player, TSV:
//   player \t device_label \t device_index \t device_count \t socd_index \t
//   socd_label \t per_game_profile(0/1) \t game_stem \t unmapped_text
// device_index -1 = keyboard only; device_count = number of pads seen.
std::string BinderPlayers();

// One line per (player, bit), TSV:
//   player \t bit \t name \t primary_label \t alt_label \t capturing(0 none,
//   1 primary slot, 2 alt slot) \t required(0/1)
std::string BinderRows();

// Apply one action from the UI. Vocabulary:
//   bind:<p>:<bit>:<slot>   start capturing into slot 0 (primary) / 1 (alt)
//   cancel                  stop capturing
//   clear:<p>:<bit>:<slot>  unbind one slot
//   device:<p>:<idx>        choose the player's pad (-1 = keyboard only)
//   padfill:<p>             fill every button from the chosen pad's defaults
//   stickfill:<p>           put the stick directions into the alt slots
//   defaults:<p>            reset the player to the shipped defaults
//   socd:<p>:<mode>         0 = inherit, 1..6 = mode 0..5
//   profile:<p>:<0|1>       0 shared default profile, 1 per-game profile
// Returns false for an unknown action.
bool BinderAction(const std::string& action);

// Per frame: advance a capture in progress, refresh the pad list on a
// cadence. Returns true when what the UI shows must be republished.
bool BinderTick();

void BinderShutdown();

}  // namespace hostgui
