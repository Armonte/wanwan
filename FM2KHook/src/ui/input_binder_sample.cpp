// input_binder_sample.cpp -- engine-facing input read (NOT ImGui).
// Sample() (SDL3, launcher) + Sample_Win32() (GetKeyboardState + XInput,
// hook DLL). Static helpers keep internal linkage.
#include "input_binder.h"
#include "input_binder_internal.h"
#include <SDL3/SDL.h>
#include <cstring>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <xinput.h>
#endif

namespace FM2KInputBinder {
// Single-binding sampler used by Sample() — pulled out of the per-bit
// switch so the caller can OR primary + alt slots through the same code
// path. Empty / NONE bindings return false (no contribution to mask).
// `identity_pad` / `has_identity`: when the player carries a device
// identity, EVERY gamepad binding reads that resolved pad (nullptr while
// it's disconnected = silent); legacy identity-less configs keep the
// per-binding SDL list index via GamepadAt.
static bool SampleOne_SDL(const Binding& b, const bool* ks, int nkeys,
                          SDL_Gamepad* identity_pad, bool has_identity) {
    switch (b.source) {
        case Binding::Source::NONE:
            return false;
        case Binding::Source::KEYBOARD:
            return ks && b.code >= 0 && b.code < nkeys && ks[b.code];
        case Binding::Source::GAMEPAD_BUTTON: {
            SDL_Gamepad* gp = has_identity ? identity_pad
                                           : GamepadAt(b.gamepad_index);
            return gp && SDL_GetGamepadButton(gp, (SDL_GamepadButton)b.code);
        }
        case Binding::Source::GAMEPAD_AXIS: {
            SDL_Gamepad* gp = has_identity ? identity_pad
                                           : GamepadAt(b.gamepad_index);
            if (!gp) return false;
            Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)b.code);
            return (b.axis_dir < 0) ? (v < -kAxisSampleThreshold)
                                    : (v >  kAxisSampleThreshold);
        }
    }
    return false;
}

uint16_t Sample(int player_slot) {
    if (player_slot < 0 || player_slot >= kPlayers) return 0;
    const PlayerBindings& pb = g_players[player_slot];

    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);

    const bool has_identity = !pb.device_id.empty();
    SDL_Gamepad* const identity_pad =
        has_identity ? ResolvedPadForPlayer(player_slot) : nullptr;

    uint16_t mask = 0;
    for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
        const bool pressed =
            SampleOne_SDL(pb.bits[i],     ks, nkeys, identity_pad, has_identity)
         || SampleOne_SDL(pb.bits_alt[i], ks, nkeys, identity_pad, has_identity);
        if (pressed) mask |= (uint16_t)(1u << i);
    }
    return mask & kFullInputMask;
}

// SDL3 scancode → Windows VK lookup. Covers the keys fighting-game players
// actually bind: letters, digits, arrows, modifiers, F-keys, space, enter,
// tab, escape, backspace, basic punctuation. Anything else returns 0.
//
// SDL3 scancodes are USB HID position codes. We hand-roll a switch instead
// of MapVirtualKey because MapVirtualKey wants Windows scancodes (BIOS set 1)
// not HID scancodes — same-shape mapping for letters but diverges on
// extended keys (arrows etc.).
#ifdef _WIN32
static int Sdl3ScancodeToVk(int sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return 'A' + (sc - SDL_SCANCODE_A);
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        return '1' + (sc - SDL_SCANCODE_1);
    }
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        return VK_F1 + (sc - SDL_SCANCODE_F1);
    }
    switch (sc) {
        case SDL_SCANCODE_RETURN:    return VK_RETURN;
        case SDL_SCANCODE_ESCAPE:    return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB:       return VK_TAB;
        case SDL_SCANCODE_SPACE:     return VK_SPACE;
        case SDL_SCANCODE_LEFT:      return VK_LEFT;
        case SDL_SCANCODE_RIGHT:     return VK_RIGHT;
        case SDL_SCANCODE_UP:        return VK_UP;
        case SDL_SCANCODE_DOWN:      return VK_DOWN;
        case SDL_SCANCODE_LCTRL:     return VK_LCONTROL;
        case SDL_SCANCODE_RCTRL:     return VK_RCONTROL;
        case SDL_SCANCODE_LSHIFT:    return VK_LSHIFT;
        case SDL_SCANCODE_RSHIFT:    return VK_RSHIFT;
        case SDL_SCANCODE_LALT:      return VK_LMENU;
        case SDL_SCANCODE_RALT:      return VK_RMENU;
        case SDL_SCANCODE_INSERT:    return VK_INSERT;
        case SDL_SCANCODE_DELETE:    return VK_DELETE;
        case SDL_SCANCODE_HOME:      return VK_HOME;
        case SDL_SCANCODE_END:       return VK_END;
        case SDL_SCANCODE_PAGEUP:    return VK_PRIOR;
        case SDL_SCANCODE_PAGEDOWN:  return VK_NEXT;
        case SDL_SCANCODE_GRAVE:     return VK_OEM_3;
        case SDL_SCANCODE_MINUS:     return VK_OEM_MINUS;
        case SDL_SCANCODE_EQUALS:    return VK_OEM_PLUS;
        case SDL_SCANCODE_LEFTBRACKET:  return VK_OEM_4;
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
        case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
        case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
        case SDL_SCANCODE_APOSTROPHE:return VK_OEM_7;
        case SDL_SCANCODE_COMMA:     return VK_OEM_COMMA;
        case SDL_SCANCODE_PERIOD:    return VK_OEM_PERIOD;
        case SDL_SCANCODE_SLASH:     return VK_OEM_2;
        case SDL_SCANCODE_KP_0:      return VK_NUMPAD0;
        case SDL_SCANCODE_KP_1:      return VK_NUMPAD1;
        case SDL_SCANCODE_KP_2:      return VK_NUMPAD2;
        case SDL_SCANCODE_KP_3:      return VK_NUMPAD3;
        case SDL_SCANCODE_KP_4:      return VK_NUMPAD4;
        case SDL_SCANCODE_KP_5:      return VK_NUMPAD5;
        case SDL_SCANCODE_KP_6:      return VK_NUMPAD6;
        case SDL_SCANCODE_KP_7:      return VK_NUMPAD7;
        case SDL_SCANCODE_KP_8:      return VK_NUMPAD8;
        case SDL_SCANCODE_KP_9:      return VK_NUMPAD9;
        case SDL_SCANCODE_KP_ENTER:  return VK_RETURN;
        case SDL_SCANCODE_KP_PLUS:   return VK_ADD;
        case SDL_SCANCODE_KP_MINUS:  return VK_SUBTRACT;
        case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
        case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
        default:                     return 0;
    }
}
#endif

// SDL3 gamepad button enum → XInput button bit. We bind via SDL3 names
// in the launcher (so the user picks "South" / "East"), then resolve
// to XInput at sample-time inside the hook DLL. Covers the standard
// 360/X1 button layout.
#ifdef _WIN32
static WORD SdlGamepadButtonToXInputBit(int b) {
    switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH:           return XINPUT_GAMEPAD_A;
        case SDL_GAMEPAD_BUTTON_EAST:            return XINPUT_GAMEPAD_B;
        case SDL_GAMEPAD_BUTTON_WEST:            return XINPUT_GAMEPAD_X;
        case SDL_GAMEPAD_BUTTON_NORTH:           return XINPUT_GAMEPAD_Y;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:   return XINPUT_GAMEPAD_LEFT_SHOULDER;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:  return XINPUT_GAMEPAD_RIGHT_SHOULDER;
        case SDL_GAMEPAD_BUTTON_BACK:            return XINPUT_GAMEPAD_BACK;
        case SDL_GAMEPAD_BUTTON_START:           return XINPUT_GAMEPAD_START;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:      return XINPUT_GAMEPAD_LEFT_THUMB;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:     return XINPUT_GAMEPAD_RIGHT_THUMB;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:         return XINPUT_GAMEPAD_DPAD_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:       return XINPUT_GAMEPAD_DPAD_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:       return XINPUT_GAMEPAD_DPAD_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:      return XINPUT_GAMEPAD_DPAD_RIGHT;
        default:                                 return 0;
    }
}
#endif

// True when the foreground window belongs to THIS process. Local (pid-
// based) so this file has no dependency on the hook's window-finder —
// it also compiles into the launcher — and so it works for both game
// window classes (KGT2KGAME / KGT95GAME) without a lookup.
#ifdef _WIN32
static bool ProcessIsForeground() {
    DWORD fg_pid = 0;
    const HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);
    return fg_pid == GetCurrentProcessId();
}
#endif

// Win32-native sampler — for use INSIDE the hook DLL where SDL3 isn't
// event-pumped. Uses GetKeyboardState (keyboard) + XInputGetState
// (gamepad). Honors the same g_players bindings the launcher's Sample()
// reads, so launcher-bound keys behave identically in-game.
//
// Focus-correct BY CONSTRUCTION — callers need no gating:
//   * Whole sample returns 0 unless this process owns the foreground
//     window. Covers the desktop-global device reads (XInput, SDL
//     gamepads) and kills the stale-held-key case (a key held across
//     an alt-tab must stop driving the game).
//   * Keyboard reads go through GetKeyboardState — synchronized to
//     THIS thread's message queue, exactly the API the vanilla engine's
//     input pipeline uses (process_game_inputs @ 0x4146D0 snapshots it
//     into KeyState[0x424D20]). Keys typed into other windows never
//     enter our queue. NEVER use GetAsyncKeyState here: it reads the
//     physical keyboard desktop-wide, which is how the binder used to
//     keep playing the game while the user typed into Discord.
uint16_t Sample_Win32(int player_slot) {
#ifndef _WIN32
    (void)player_slot;
    return 0;
#else
    if (player_slot < 0 || player_slot >= kPlayers) return 0;
    if (!ProcessIsForeground()) return 0;
    const PlayerBindings& pb = g_players[player_slot];

    // One queue-synced keyboard snapshot per sample (same shape the
    // vanilla engine uses). All KEYBOARD bindings read from this; a
    // failed call leaves it zeroed = no keys down.
    BYTE ks[256] = {};
    GetKeyboardState(ks);

    // SDL3 gamepad polling for ALL stick types — Init()'s RAWINPUT +
    // HIDAPI hints make SDL enumerate XInput pads, DS3/DS4/DS5, Switch
    // Pro, and generic HID sticks natively. Init() opens the devices;
    // we just need to pump events here so the polled state behind
    // SDL_GetGamepadButton stays fresh inside the hooked game process
    // (the game's main loop doesn't call SDL_PollEvent on our behalf).
    if (g_initialized && !g_gamepad_handles.empty()) {
        SDL_PumpEvents();
    }

    // Device resolution rules (ODK's "buttons go to both players" bug):
    //   * Identity routing first: a player with a device_id reads its
    //     identity-resolved pad for EVERY gamepad binding (silent while
    //     disconnected — never borrows the other player's pad). Legacy
    //     identity-less configs go through GamepadAt(b.gamepad_index):
    //     -1 = first connected, out-of-range = nullptr. (The old local
    //     lambda here aliased ANY bad index to the FIRST pad, so P2's
    //     configured-but-absent pad silently read P1's controller.)
    //   * When SDL has a handle for the pad, its read is AUTHORITATIVE —
    //     no fall-through. The old code fell through from "SDL says not
    //     pressed" to raw XInputGetState using the SDL LIST INDEX as an
    //     XInput USER SLOT. Those orderings are unrelated: with P1 on a
    //     DInput/HID pad (occupies no XInput slot) and P2 on an XInput
    //     pad (slot 0), P1's bindings read P2's pad through the
    //     fall-through — both players moved.
    //   * Raw XInput survives ONLY as a last-resort path for "SDL has
    //     ZERO pads open" (subsystem dead / no mapping). With no SDL
    //     list to resolve against, the PLAYER SLOT maps to the XInput
    //     user slot (P1 -> 0, P2 -> 1) — the sanest approximation, and
    //     one that can't cross-wire the two players.
    const bool has_identity = !pb.device_id.empty();
    SDL_Gamepad* const identity_pad =
        has_identity ? ResolvedPadForPlayer(player_slot) : nullptr;
    const bool xinput_fallback = g_gamepad_handles.empty();

    // XInput is checked once per call; fetched lazily by slot.
    XINPUT_STATE xs[4];
    bool         xs_ok[4] = {false, false, false, false};
    auto get_xinput = [&](int idx) -> const XINPUT_STATE* {
        if (idx < 0 || idx >= 4) return nullptr;
        if (!xs_ok[idx]) {
            ZeroMemory(&xs[idx], sizeof(xs[idx]));
            xs_ok[idx] = (XInputGetState((DWORD)idx, &xs[idx]) == ERROR_SUCCESS);
        }
        return xs_ok[idx] ? &xs[idx] : nullptr;
    };

    // Per-binding sampler. Defined as a lambda so it captures ks /
    // get_xinput without re-threading them through a static helper.
    // Called twice per bit (primary + alt) and OR'd, so a single
    // direction can fire from stick AND dpad simultaneously.
    auto sample_one = [&](const Binding& b) -> bool {
        switch (b.source) {
            case Binding::Source::NONE:
                return false;
            case Binding::Source::KEYBOARD: {
                int vk = Sdl3ScancodeToVk(b.code);
                return vk != 0 && (ks[vk] & 0x80) != 0;
            }
            case Binding::Source::GAMEPAD_BUTTON: {
                SDL_Gamepad* gp = has_identity ? identity_pad
                                               : GamepadAt(b.gamepad_index);
                if (gp) {
                    return SDL_GetGamepadButton(gp, (SDL_GamepadButton)b.code) != 0;
                }
                if (!xinput_fallback) return false;  // pad absent — silent
                if (const XINPUT_STATE* st = get_xinput(player_slot)) {
                    WORD xbit = SdlGamepadButtonToXInputBit(b.code);
                    return xbit != 0 && (st->Gamepad.wButtons & xbit) != 0;
                }
                return false;
            }
            case Binding::Source::GAMEPAD_AXIS: {
                SDL_Gamepad* gp = has_identity ? identity_pad
                                               : GamepadAt(b.gamepad_index);
                if (gp) {
                    Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)b.code);
                    return (b.axis_dir < 0) ? (v < -kAxisSampleThreshold)
                                            : (v >  kAxisSampleThreshold);
                }
                if (!xinput_fallback) return false;  // pad absent — silent
                if (const XINPUT_STATE* st = get_xinput(player_slot)) {
                    // Compute in int, not SHORT: negating sThumbL/RY at full
                    // deflection (-32768) overflows a SHORT back to -32768,
                    // which flips full-down into full-up (and vice versa).
                    int v = 0;
                    switch (b.code) {
                        case SDL_GAMEPAD_AXIS_LEFTX:        v = st->Gamepad.sThumbLX; break;
                        case SDL_GAMEPAD_AXIS_LEFTY:        v = -(int)st->Gamepad.sThumbLY; break;
                        case SDL_GAMEPAD_AXIS_RIGHTX:       v = st->Gamepad.sThumbRX; break;
                        case SDL_GAMEPAD_AXIS_RIGHTY:       v = -(int)st->Gamepad.sThumbRY; break;
                        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: v = st->Gamepad.bLeftTrigger * 128; break;
                        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:v = st->Gamepad.bRightTrigger * 128; break;
                        default: break;
                    }
                    return (b.axis_dir < 0) ? (v < -kAxisSampleThreshold)
                                            : (v >  kAxisSampleThreshold);
                }
                return false;
            }
        }
        return false;
    };

    uint16_t mask = 0;
    for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
        if (sample_one(pb.bits[i]) || sample_one(pb.bits_alt[i])) {
            mask |= (uint16_t)(1u << i);
        }
    }
    return mask & kFullInputMask;
#endif
}
}  // namespace FM2KInputBinder
