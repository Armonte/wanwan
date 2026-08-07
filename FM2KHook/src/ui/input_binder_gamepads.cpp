// input_binder_gamepads.cpp -- SDL3 gamepad discovery/lifecycle.
// RefreshGamepadList + GamepadNameAt, promoted to external linkage so the
// core lifecycle + the ui device dropdown can both call them.
#include "input_binder.h"
#include "input_binder_internal.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <vector>

namespace FM2KInputBinder {
// ---------------------------------------------------------------------------
// Gamepad management
// ---------------------------------------------------------------------------

void RefreshGamepadList() {
    // Reentrancy guard. SDL_PumpEvents below dispatches Win32 messages
    // into the wndproc subclass (modal pump), which can tick code that
    // calls RefreshGamepads() again -- the reentrant call mutates
    // g_gamepad_handles / g_non_gamepad_ids while THIS frame iterates
    // them, invalidating iterators: AV inside unordered_map internals
    // (hash<unsigned> _M_cget), observed twice on spectator instances
    // under 20% loss (2026-06-11, FM2KHook+0x38d944). Skipping the
    // nested refresh is always safe -- the outer one finishes the scan.
    static bool s_refresh_in_progress = false;
    if (s_refresh_in_progress) return;
    s_refresh_in_progress = true;
    struct RefreshGuard {
        bool* flag;
        ~RefreshGuard() { *flag = false; }
    } guard{&s_refresh_in_progress};

    g_gamepad_ids.clear();
    // Pump events first -- a freshly-plugged stick may not show up in
    // SDL_GetGamepads / SDL_GetJoysticks until SDL has processed its
    // own JOYSTICK_ADDED event.
    SDL_PumpEvents();

    // Pass 1: SDL-known gamepads (have a built-in mapping).
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count; ++i) {
            SDL_JoystickID jid = ids[i];
            g_gamepad_ids.push_back(jid);
            if (g_gamepad_handles.find(jid) == g_gamepad_handles.end()) {
                SDL_Gamepad* gp = SDL_OpenGamepad(jid);
                if (gp) {
                    g_gamepad_handles[jid] = gp;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "InputBinder: opened gamepad jid=%u name='%s'",
                        (unsigned)jid,
                        SDL_GetGamepadName(gp) ? SDL_GetGamepadName(gp) : "?");
                }
            }
        }
        SDL_free(ids);
    }

    // Pass 2: walk ALL joysticks. Some sticks (Qanba Obsidian in
    // generic-HID modes, third-party PS3 clones, etc.) don't appear
    // in SDL_GetGamepads but CAN be opened as gamepads via SDL3's
    // synthetic mapping. Mirroring revolve_input_sdl3's pattern
    // here -- without it, only XInput-recognized devices showed up
    // in our binder UI.
    int joy_count = 0;
    SDL_JoystickID* joys = SDL_GetJoysticks(&joy_count);
    std::unordered_set<SDL_JoystickID> present_joysticks;
    if (joys) {
        for (int i = 0; i < joy_count; ++i) {
            SDL_JoystickID jid = joys[i];
            present_joysticks.insert(jid);
            // Skip if pass 1 already opened it, or if we already examined it
            // and found it is not a gamepad. The reject-cache (below) is what
            // stops the per-refresh SDL_OpenJoystick+SDL_CloseJoystick on a
            // non-gamepad stick (vJoy etc.) -- the ~46ms/sec hitch = "#63 95fps".
            if (g_gamepad_handles.find(jid) != g_gamepad_handles.end()) continue;
            if (g_non_gamepad_ids.find(jid) != g_non_gamepad_ids.end()) continue;
            // SDL_IsGamepad sometimes returns false right after a
            // joystick-added event before SDL loads the device's
            // mapping. Pump + retry once before giving up.
            bool is_gp = SDL_IsGamepad(jid);
            if (!is_gp) { SDL_PumpEvents(); is_gp = SDL_IsGamepad(jid); }
            if (!is_gp) {
                // Diagnostic -- name + GUID for sticks we couldn't
                // promote to a gamepad. Helps the user paste this
                // info if they need a custom mapping added.
                SDL_Joystick* j = SDL_OpenJoystick(jid);
                if (j) {
                    char guid[64] = {};
                    SDL_GUIDToString(SDL_GetJoystickGUID(j), guid, sizeof(guid));
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "InputBinder: joystick jid=%u name='%s' guid=%s "
                        "(no SDL gamepad mapping -- won't appear in binder)",
                        (unsigned)jid,
                        SDL_GetJoystickName(j) ? SDL_GetJoystickName(j) : "?",
                        guid);
                    SDL_CloseJoystick(j);
                }
                // Remember it's not a gamepad so we never re-open/close it
                // on subsequent refreshes (the #63 per-second hitch).
                g_non_gamepad_ids.insert(jid);
                continue;
            }
            SDL_Gamepad* gp = SDL_OpenGamepad(jid);
            if (!gp) { SDL_PumpEvents(); gp = SDL_OpenGamepad(jid); }
            if (gp) {
                g_gamepad_ids.push_back(jid);
                g_gamepad_handles[jid] = gp;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "InputBinder: promoted joystick jid=%u to gamepad -- name='%s'",
                    (unsigned)jid,
                    SDL_GetGamepadName(gp) ? SDL_GetGamepadName(gp) : "?");
            }
        }
        SDL_free(joys);
    }

    // Forget rejected sticks that were unplugged, so a re-plug gets
    // examined once more (and isn't skipped forever by the cache above).
    for (auto it = g_non_gamepad_ids.begin(); it != g_non_gamepad_ids.end();) {
        if (present_joysticks.find(*it) == present_joysticks.end())
            it = g_non_gamepad_ids.erase(it);
        else
            ++it;
    }

    // Drop any handles that disappeared.
    for (auto it = g_gamepad_handles.begin(); it != g_gamepad_handles.end();) {
        bool still_present = std::find(g_gamepad_ids.begin(), g_gamepad_ids.end(),
                                       it->first) != g_gamepad_ids.end();
        if (!still_present) {
            if (it->second) SDL_CloseGamepad(it->second);
            it = g_gamepad_handles.erase(it);
        } else {
            ++it;
        }
    }

    // Device set may have changed -- re-resolve each player's identity-
    // bound pad against the fresh list.
    ResolvePlayerPads();
}

// ---------------------------------------------------------------------------
// Stable per-player device identity
//
// Bindings persist a bare SDL list index, but that index is VOLATILE:
// unplugging pad A shifts pad B from index 1 to 0, so P2's bindings
// suddenly pointed at nothing (or, before the routing fix, at P1's pad).
// Identity = "<GUID string>[#<serial>]" pins each player to a physical
// device; the list index becomes a display detail.
// ---------------------------------------------------------------------------

SDL_JoystickID g_player_pad_jid[kPlayers] = {0, 0};

std::string DeviceIdentityOfJid(SDL_JoystickID jid) {
    char guid_s[64] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(jid), guid_s, sizeof(guid_s));
    std::string id = guid_s;
    // Serial (when the driver exposes one) disambiguates two identical
    // pads across sessions. Same-model pads without serials share an
    // identity and fall back to claim order below.
    auto it = g_gamepad_handles.find(jid);
    if (it != g_gamepad_handles.end() && it->second) {
        const char* ser = SDL_GetGamepadSerial(it->second);
        if (ser && *ser) {
            id += '#';
            id += ser;
        }
    }
    return id;
}

void ResolvePlayerPads() {
    SDL_JoystickID prev[kPlayers];
    for (int p = 0; p < kPlayers; ++p) prev[p] = g_player_pad_jid[p];

    std::unordered_set<SDL_JoystickID> claimed;

    // Pass 1 -- keep still-valid resolutions. Instance ids are stable
    // while connected, so a player never hops devices mid-session just
    // because the list reordered around them.
    for (int p = 0; p < kPlayers; ++p) {
        const SDL_JoystickID jid = g_player_pad_jid[p];
        if (jid != 0 &&
            g_gamepad_handles.find(jid) != g_gamepad_handles.end() &&
            !g_players[p].device_id.empty() &&
            DeviceIdentityOfJid(jid) == g_players[p].device_id) {
            claimed.insert(jid);
        } else {
            g_player_pad_jid[p] = 0;
        }
    }

    // Pass 2 -- resolve the rest by identity, in player order (P1 gets
    // first pick when both players are configured for the same model
    // and only one unit is attached). List order breaks ties between
    // identical unclaimed units. A configured-but-absent device stays
    // UNRESOLVED: that player's gamepad bindings go silent instead of
    // borrowing another player's pad.
    for (int p = 0; p < kPlayers; ++p) {
        if (g_player_pad_jid[p] != 0) continue;
        const std::string& want = g_players[p].device_id;
        if (want.empty()) continue;  // legacy index routing -- no identity
        for (SDL_JoystickID jid : g_gamepad_ids) {
            if (claimed.count(jid)) continue;
            if (DeviceIdentityOfJid(jid) == want) {
                g_player_pad_jid[p] = jid;
                claimed.insert(jid);
                break;
            }
        }
    }

    // Log transitions only (this runs every RefreshGamepads tick).
    for (int p = 0; p < kPlayers; ++p) {
        if (prev[p] == g_player_pad_jid[p]) continue;
        if (g_player_pad_jid[p] != 0) {
            auto it = g_gamepad_handles.find(g_player_pad_jid[p]);
            const char* n = (it != g_gamepad_handles.end() && it->second)
                ? SDL_GetGamepadName(it->second) : nullptr;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "InputBinder: P%d device resolved -- jid=%u '%s'",
                p + 1, (unsigned)g_player_pad_jid[p], n ? n : "?");
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "InputBinder: P%d device '%s' disconnected -- gamepad "
                "bindings inactive until it returns",
                p + 1, g_players[p].device_name.c_str());
        }
    }
}

SDL_Gamepad* ResolvedPadForPlayer(int player_slot) {
    if (player_slot < 0 || player_slot >= kPlayers) return nullptr;
    const SDL_JoystickID jid = g_player_pad_jid[player_slot];
    if (jid == 0) return nullptr;
    auto it = g_gamepad_handles.find(jid);
    return it == g_gamepad_handles.end() ? nullptr : it->second;
}

int ResolvedPadListIndex(int player_slot) {
    if (player_slot < 0 || player_slot >= kPlayers) return -1;
    const SDL_JoystickID jid = g_player_pad_jid[player_slot];
    if (jid == 0) return -1;
    for (size_t i = 0; i < g_gamepad_ids.size(); ++i) {
        if (g_gamepad_ids[i] == jid) return (int)i;
    }
    return -1;
}

void SetPlayerDevice(int player_slot, int list_index) {
    if (player_slot < 0 || player_slot >= kPlayers) return;
    PlayerBindings& pb = g_players[player_slot];
    if (list_index < 0 || list_index >= (int)g_gamepad_ids.size()) {
        pb.device_id.clear();
        pb.device_name.clear();
        g_player_pad_jid[player_slot] = 0;
        return;
    }
    const SDL_JoystickID jid = g_gamepad_ids[(size_t)list_index];
    pb.device_id   = DeviceIdentityOfJid(jid);
    pb.device_name = GamepadNameAt(list_index);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "InputBinder: P%d device set -- '%s' (%s)",
        player_slot + 1, pb.device_name.c_str(), pb.device_id.c_str());
    ResolvePlayerPads();
}

SDL_Gamepad* GamepadAt(int idx) {
    // idx == -1 means "first connected".
    if (idx < 0) {
        if (g_gamepad_ids.empty()) return nullptr;
        auto it = g_gamepad_handles.find(g_gamepad_ids.front());
        return it == g_gamepad_handles.end() ? nullptr : it->second;
    }
    if (idx >= (int)g_gamepad_ids.size()) return nullptr;
    auto it = g_gamepad_handles.find(g_gamepad_ids[idx]);
    return it == g_gamepad_handles.end() ? nullptr : it->second;
}

const char* GamepadNameAt(int idx) {
    SDL_Gamepad* gp = GamepadAt(idx);
    if (!gp) return "(no gamepad)";
    const char* n = SDL_GetGamepadName(gp);
    return n ? n : "(unknown)";
}
}  // namespace FM2KInputBinder
