// input_binder_gamepads.cpp -- SDL3 gamepad discovery/lifecycle.
// RefreshGamepadList + GamepadNameAt, promoted to external linkage so the
// core lifecycle + the ui device dropdown can both call them.
#include "input_binder.h"
#include "input_binder_internal.h"
#include "input_route.h"          // pure identity/claim resolver + route model
#include <SDL3/SDL.h>
#include <algorithm>
#include <string>
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

// Snapshot the connected pads in SDL list order, as the pure resolver wants
// them. Only pads we actually hold a handle for are listed: an unopened id
// can neither be sampled nor claimed, and both passes below already required
// a handle.
static std::vector<fm2k::inputroute::Pad> SnapshotPads() {
    std::vector<fm2k::inputroute::Pad> pads;
    pads.reserve(g_gamepad_ids.size());
    for (SDL_JoystickID jid : g_gamepad_ids) {
        if (g_gamepad_handles.find(jid) == g_gamepad_handles.end()) continue;
        pads.push_back({(uint32_t)jid, DeviceIdentityOfJid(jid)});
    }
    return pads;
}

// The routing-relevant part of a player's profile: does it bind a gamepad at
// all, and -- for identity-less legacy profiles -- which SDL list index do
// its gamepad rows name. Every profile the binder UI writes stamps ONE index
// across all of a player's gamepad rows (FillPrimaryAsGamepad /
// FillAltAsStickDirections), so the first such row is representative; a
// hand-edited ini that mixes indices is reported by its first row.
static fm2k::inputroute::PlayerProfile ProfileOf(int p) {
    fm2k::inputroute::PlayerProfile out;
    out.device_id = g_players[p].device_id;
    for (size_t i = 0; i < (size_t)Bit::COUNT && !out.has_gamepad_binding; ++i) {
        for (const Binding* arr : { g_players[p].bits + 0,
                                    g_players[p].bits_alt + 0 }) {
            const Binding& b = arr[i];
            if (b.source == Binding::Source::GAMEPAD_BUTTON ||
                b.source == Binding::Source::GAMEPAD_AXIS) {
                out.has_gamepad_binding = true;
                out.legacy_index = b.gamepad_index;
                break;
            }
        }
    }
    return out;
}

// Report which PHYSICAL pad each player's gamepad bindings ended up on, and
// shout when both landed on the same one.
//
// The binder's rule is one device per player, and identity routing enforces
// it -- but a profile with no device_id (written before identity existed, or
// whose rows were captured without ever opening the device dropdown) still
// routes by raw SDL list index, which is a POSITION, not a device. Two such
// players can name the same index, and then one person's pad drives both
// characters while the other person's pad does nothing. That state is
// invisible from inside the game, so it has to be visible in the log:
// "my inputs are being eaten with two controllers plugged in" is otherwise
// unanswerable. Change-triggered, so this costs one comparison per refresh.
static void LogRouteTable(const std::vector<fm2k::inputroute::Pad>& pads) {
    using namespace fm2k::inputroute;
    const PlayerProfile players[kPlayers] = { ProfileOf(0), ProfileOf(1) };
    Route routes[kPlayers];
    for (int p = 0; p < kPlayers; ++p) {
        routes[p] = RouteForPlayer(pads, players[p],
                                   (uint32_t)g_player_pad_jid[p]);
    }

    static uint32_t s_last_jid[kPlayers] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    static size_t   s_last_pad_count = (size_t)-1;
    if (s_last_jid[0] == routes[0].jid && s_last_jid[1] == routes[1].jid &&
        s_last_pad_count == pads.size()) {
        return;
    }
    s_last_jid[0] = routes[0].jid;
    s_last_jid[1] = routes[1].jid;
    s_last_pad_count = pads.size();

    auto name_of = [](uint32_t jid) -> const char* {
        auto it = g_gamepad_handles.find((SDL_JoystickID)jid);
        if (it == g_gamepad_handles.end() || !it->second) return "?";
        const char* n = SDL_GetGamepadName(it->second);
        return n ? n : "?";
    };
    for (int p = 0; p < kPlayers; ++p) {
        if (!players[p].has_gamepad_binding) continue;
        const char* via = routes[p].via == Via::IDENTITY   ? "device id"
                        : routes[p].via == Via::LIST_INDEX ? "list index"
                                                           : "nothing";
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "InputBinder: P%d gamepad route -- %s (jid=%u '%s') via %s "
            "[%zu pad(s) connected]",
            p + 1, routes[p].jid ? "resolved" : "UNRESOLVED",
            (unsigned)routes[p].jid, name_of(routes[p].jid), via,
            pads.size());
    }
    if (RoutesCollide(routes[0], routes[1])) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "InputBinder: BOTH PLAYERS ARE READING THE SAME CONTROLLER "
            "(jid=%u '%s'). One pad drives both characters and the other "
            "pad is dead. Open Input Settings and pick each player's device "
            "from the dropdown to pin them to their own controller.",
            (unsigned)routes[0].jid, name_of(routes[0].jid));
    }
}

void ResolvePlayerPads() {
    SDL_JoystickID prev[kPlayers];
    for (int p = 0; p < kPlayers; ++p) prev[p] = g_player_pad_jid[p];

    // Identity resolution (sticky-then-claim) lives in input_route.h so
    // tests/input_route_test.cpp can exercise it with two pads on the table
    // -- a configuration the two-instance gate structurally cannot reach.
    const std::vector<fm2k::inputroute::Pad> pads = SnapshotPads();
    const fm2k::inputroute::PlayerProfile profiles[kPlayers] = {
        ProfileOf(0), ProfileOf(1)
    };
    uint32_t sticky[kPlayers];
    for (int p = 0; p < kPlayers; ++p) sticky[p] = (uint32_t)g_player_pad_jid[p];
    fm2k::inputroute::ResolveIdentities(pads, profiles, sticky);
    for (int p = 0; p < kPlayers; ++p) {
        g_player_pad_jid[p] = (SDL_JoystickID)sticky[p];
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

    LogRouteTable(pads);
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
