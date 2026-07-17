// input_binder.cpp -- CORE: shared state + defaults + lifecycle.
// Split from the original monolith; the binding store + gamepad handle
// tables + INI-key table live here at external linkage (see
// input_binder_internal.h). ENGINE-AGNOSTIC, render-agnostic.
#include "input_binder.h"
#include "input_binder_internal.h"
#include <SDL3/SDL.h>
#include <cstddef>
#include <string>

namespace FM2KInputBinder {

// ---- shared state (external linkage; declared in input_binder_internal.h) ----
PlayerBindings g_players[kPlayers];
std::vector<SDL_JoystickID> g_gamepad_ids;
std::unordered_map<SDL_JoystickID, SDL_Gamepad*> g_gamepad_handles;
std::unordered_set<SDL_JoystickID> g_non_gamepad_ids;
std::string g_config_path;
bool g_initialized = false;

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

// Wire / display name per FM2K bit. These names also serve as the INI keys
// in fm2k_inputs.ini, so renaming them is a wire-format break — old configs
// with "BTN5" / "BTN6" / "BTN7" lines will silently fail to load and fall
// back to defaults. That's an acceptable one-time cost for matching the
// 2DFM convention; the binder ships in v1 so very few configs exist.
const char* kBitNames[(size_t)Bit::COUNT] = {
    "LEFT", "RIGHT", "UP", "DOWN",
    "A", "B", "C", "D", "E", "F",
    "START", "OPTION", "FN1", "FN2"
};


// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

void ApplyDefaultsP1(PlayerBindings& pb) {
    auto kb = [&](Bit b, SDL_Scancode sc) {
        Binding& s = pb.bits[(size_t)b];
        s.source = Binding::Source::KEYBOARD;
        s.code = (int)sc;
        s.axis_dir = 0;
        s.gamepad_index = -1;
    };
    kb(Bit::LEFT,  SDL_SCANCODE_LEFT);
    kb(Bit::RIGHT, SDL_SCANCODE_RIGHT);
    kb(Bit::UP,    SDL_SCANCODE_UP);
    kb(Bit::DOWN,  SDL_SCANCODE_DOWN);
    // 6 attack buttons + Start, matching 2D Fighter Maker 2002's KEYINPUT
    // layout convention. Z X C V on the lower row, A S D on the upper row,
    // Enter for Start — same defaults LilithPort and most FM2K-era setups
    // shipped with so existing players don't have to relearn their bind.
    kb(Bit::A,     SDL_SCANCODE_Z);
    kb(Bit::B,     SDL_SCANCODE_X);
    kb(Bit::C,     SDL_SCANCODE_C);
    kb(Bit::D,     SDL_SCANCODE_A);
    kb(Bit::E,     SDL_SCANCODE_S);
    kb(Bit::F,     SDL_SCANCODE_D);
    kb(Bit::START, SDL_SCANCODE_RETURN);
    // Meta-buttons. Tab = OPTION (title submode cycle, doesn't conflict
    // with START's confirm on title). F1/F2 reserved for hook features
    // (training-mode behavior cycle migrates here from the GetKeyState
    // fallback once the binder's Sample feeds FN2 into the cycle handler).
    kb(Bit::OPTION, SDL_SCANCODE_TAB);
    kb(Bit::FN1,    SDL_SCANCODE_F1);
    kb(Bit::FN2,    SDL_SCANCODE_F2);
}

void ApplyDefaultsP2(PlayerBindings& pb) {
    // P2 defaults: numpad layout so two players can share a single keyboard
    // without conflicts. Same shape as P1 (left/right/up/down + 6 attack
    // buttons + start) but mapped to the right side of a 104-key layout.
    //
    // If a second gamepad is plugged in, the user can rebind P2's slots
    // to it via the binder UI; the gamepad_index field defaults to 1
    // here so the FIRST gamepad-bound row a user adds picks up gamepad
    // index 1 (P1 picks 0). This matches the user-facing convention:
    // P1 = primary device, P2 = second device / keyboard fallback.
    auto kb = [&](Bit b, SDL_Scancode sc) {
        Binding& s = pb.bits[(size_t)b];
        s.source = Binding::Source::KEYBOARD;
        s.code = (int)sc;
        s.axis_dir = 0;
        s.gamepad_index = 1;  // default routes future gamepad bindings to pad #2
    };
    kb(Bit::LEFT,  SDL_SCANCODE_KP_4);
    kb(Bit::RIGHT, SDL_SCANCODE_KP_6);
    kb(Bit::UP,    SDL_SCANCODE_KP_8);
    kb(Bit::DOWN,  SDL_SCANCODE_KP_2);
    // 6 attack buttons + Start. UIOJKL is the canonical "right hand"
    // layout for P2 keyboard share — mirrors P1's ZXCV/ASD (Z X C
    // lower row, A S D upper row) on the home-row keys around U/J.
    kb(Bit::A,     SDL_SCANCODE_J);
    kb(Bit::B,     SDL_SCANCODE_K);
    kb(Bit::C,     SDL_SCANCODE_L);
    kb(Bit::D,     SDL_SCANCODE_U);
    kb(Bit::E,     SDL_SCANCODE_I);
    kb(Bit::F,     SDL_SCANCODE_O);
    kb(Bit::START, SDL_SCANCODE_KP_ENTER);
}

// Fill the alt slot with the LEFT STICK axis bindings ONLY for the four
// directionals. Buttons stay empty in alt. Pairs with FillPrimaryAsGamepad
// to give the CXL pattern (dpad in primary + stick in alt, both move the
// character) without duplicating face buttons across both slots — pressing
// A on the pad would otherwise fire the A bit twice through the same
// physical button. Empty alt buttons keep the binding model honest:
// "one device, one button, one bit" except where the user explicitly
// wants two physical inputs to share a bit (directionals).
void FillAltAsStickDirections(PlayerBindings& pb, int gp_idx) {
    auto axis = [&](Bit b, SDL_GamepadAxis a, int dir) {
        Binding& s = pb.bits_alt[(size_t)b];
        s.source = Binding::Source::GAMEPAD_AXIS;
        s.code = (int)a;
        s.axis_dir = dir;
        s.gamepad_index = gp_idx;
    };
    axis(Bit::LEFT,  SDL_GAMEPAD_AXIS_LEFTX, -1);
    axis(Bit::RIGHT, SDL_GAMEPAD_AXIS_LEFTX, +1);
    axis(Bit::UP,    SDL_GAMEPAD_AXIS_LEFTY, -1);
    axis(Bit::DOWN,  SDL_GAMEPAD_AXIS_LEFTY, +1);
    // Buttons (A-F + START) intentionally NOT touched — leaving them
    // empty keeps each face button mapped exactly once.
}

// Fill the primary slot with the standard XInput-style gamepad layout
// (dpad + face/shoulder buttons). Inverse of FillAltAsGamepad — used
// when the user picks a gamepad as their primary device.
void FillPrimaryAsGamepad(PlayerBindings& pb, int gp_idx) {
    auto btn = [&](Bit b, SDL_GamepadButton gb) {
        Binding& s = pb.bits[(size_t)b];
        s.source = Binding::Source::GAMEPAD_BUTTON;
        s.code = (int)gb;
        s.axis_dir = 0;
        s.gamepad_index = gp_idx;
    };
    btn(Bit::LEFT,  SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    btn(Bit::RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    btn(Bit::UP,    SDL_GAMEPAD_BUTTON_DPAD_UP);
    btn(Bit::DOWN,  SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    btn(Bit::A,     SDL_GAMEPAD_BUTTON_SOUTH);
    btn(Bit::B,     SDL_GAMEPAD_BUTTON_EAST);
    btn(Bit::C,     SDL_GAMEPAD_BUTTON_WEST);
    btn(Bit::D,     SDL_GAMEPAD_BUTTON_NORTH);
    btn(Bit::E,     SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    btn(Bit::F,     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    btn(Bit::START, SDL_GAMEPAD_BUTTON_START);
}

void ApplyDefaults(int player) {
    PlayerBindings& pb = g_players[player];
    for (auto& s : pb.bits)     s = Binding{};
    for (auto& s : pb.bits_alt) s = Binding{};
    // Defaults are KEYBOARD-ONLY. One device per player. Users who want a
    // gamepad pick it via the device dropdown — that swaps primary to
    // dpad+face-button layout AND fills alt with stick+face-button layout
    // (CXL stick+dpad pattern). Never auto-bind a second device behind
    // the user's back: dual-device bindings are how Muffin's parry
    // moved across slots when v0.2.16 stomped his alt with XInput
    // defaults while his primary kept his keyboard layout.
    if (player == 0)      ApplyDefaultsP1(pb);
    else if (player == 1) ApplyDefaultsP2(pb);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

PlayerBindings& Bindings(int player_slot) {
    if (player_slot < 0) player_slot = 0;
    if (player_slot >= kPlayers) player_slot = kPlayers - 1;
    return g_players[player_slot];
}

void Init() {
    if (g_initialized) return;
    g_initialized = true;
    g_config_path = DefaultConfigPath();

    // Hints — must be set BEFORE SDL_InitSubSystem(GAMEPAD).
    // HIDAPI brings the first-party DS4/DS3/Switch-Pro drivers in
    // SDL3, which handle PS4 sticks (Qanba Obsidian in PS4 mode is a
    // re-labelled DS4) and PS3 sticks correctly. RAWINPUT gives us
    // XInput-style controllers (Qanba Obsidian in PC mode shows up as
    // an XInput device). Without these, sticks fall back to platform-
    // generic joystick paths that often don't ship with a SDL gamepad
    // mapping → SDL_GetGamepads() returns nothing and the binder UI
    // shows zero devices.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI,         "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH,  "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT,       "1");

    // Make sure SDL gamepad subsystem is up. No-op if already initialized.
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    // Critical: without this the polled state behind SDL_GetGamepadButton
    // never refreshes — clicking Bind and pressing a button looked like
    // the binder was ignoring controller inputs entirely. Enabled
    // unconditionally; mirrors revolve_input_sdl3 (sdl3_gamepad_manager
    // line 128).
    SDL_SetGamepadEventsEnabled(true);

    // Built-in mapping fallbacks for sticks SDL3 doesn't ship a
    // mapping for. PS3 controllers in particular: HIDAPI driver can't
    // always identify the controller flavor (Sony first-party vs
    // clone) and falls through to a generic HID joystick — which has
    // axes/buttons but no gamepad mapping, so SDL_GetGamepads()
    // doesn't list it. Adding mappings ahead of time covers that.
    // Lifted verbatim from revolve_input_sdl3 (BBBR's input layer).
    static const char* kBuiltinMappings[] = {
        "030000004c0500006802000000010000,PS3 Controller,a:b14,b:b13,y:b12,x:b15,back:b0,guide:b16,start:b3,leftstick:b1,rightstick:b2,leftshoulder:b10,rightshoulder:b11,lefttrigger:b8,righttrigger:b9,leftx:a0,lefty:a1,rightx:a2,righty:a3,dpdown:b6,dpleft:b7,dpright:b5,dpup:b4,",
        "030000004c0500006802000000000000,PS3 Controller,a:b14,b:b13,y:b12,x:b15,back:b0,guide:b16,start:b3,leftstick:b1,rightstick:b2,leftshoulder:b10,rightshoulder:b11,lefttrigger:b8,righttrigger:b9,leftx:a0,lefty:a1,rightx:a2,righty:a3,dpdown:b6,dpleft:b7,dpright:b5,dpup:b4,",
        "030000004c0500006802000000020000,PS3 Controller,a:b14,b:b13,y:b12,x:b15,back:b0,guide:b16,start:b3,leftstick:b1,rightstick:b2,leftshoulder:b10,rightshoulder:b11,lefttrigger:b8,righttrigger:b9,leftx:a0,lefty:a1,rightx:a2,righty:a3,dpdown:b6,dpleft:b7,dpright:b5,dpup:b4,",
    };
    for (const char* m : kBuiltinMappings) SDL_AddGamepadMapping(m);

    RefreshGamepadList();
    // Defaults first, then overlay with file contents if present.
    for (int p = 0; p < kPlayers; ++p) ApplyDefaults(p);
    Load();
}

void Shutdown() {
    for (auto& kv : g_gamepad_handles) {
        if (kv.second) SDL_CloseGamepad(kv.second);
    }
    g_gamepad_handles.clear();
    g_gamepad_ids.clear();
    g_initialized = false;
}

// Public wrapper for hot-plug refresh. See input_binder.h.
void RefreshGamepads() {
    RefreshGamepadList();
}
}  // namespace FM2KInputBinder
