// input_route.h -- the two device decisions the input binder makes, as pure
// functions with no SDL, no Win32 and no global state.
//
// They live here because both of them are only WRONG when a SECOND pad is
// connected, which is precisely the configuration no harness in this repo can
// reach: the two-instance gate drives synthetic inputs and never opens a
// physical device at all. Pulling the rules out means tests/input_route_test
// can put two pads on the table and check what each player ends up reading.
//
// The two decisions:
//
//   1. ROUTING -- which physical pad a player's gamepad bindings read.
//      Identity-pinned players resolve by "<GUID>[#serial]" with claims, so
//      two players never land on one pad. Identity-LESS (legacy) profiles
//      still go through the raw SDL list index, which is not a device: it
//      shifts when the list changes and it happily aims two players at the
//      same pad. RoutesCollide() names that state so it can stop being
//      silent.
//
//   2. BIND CAPTURE -- which pad press becomes a binding. The shipped scan
//      walked pads in list order and took the first source past the capture
//      threshold, with no reference to what was already held when the bind
//      armed. Since the arm gate uses a LOOSER threshold than the capture
//      (a stick may not block arming yet still read as pressed), an idle pad
//      parked between the two thresholds wins every bind -- and with two
//      pads on the table, the pad it steals from is the other player's.
//      PickPadCapture requires a CHANGE from the armed snapshot and looks at
//      the player's own pad first.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fm2k {
namespace inputroute {

inline constexpr int kPlayers = 2;

// One connected pad as the binder sees it. `identity` is the string
// DeviceIdentityOfJid() builds ("<GUID>" plus "#<serial>" when the driver
// exposes one), so two units of the same model with no serial share it.
// Vector order IS the SDL list order, i.e. the index a legacy
// `gamepad_index` binding names.
struct Pad {
    uint32_t    jid = 0;
    std::string identity;
};

// The routing-relevant part of one player's profile.
struct PlayerProfile {
    std::string device_id;                // "" = legacy list-index routing
    bool        has_gamepad_binding = false;
    int         legacy_index = -1;        // gamepad_index; -1 = first connected
};

// How a player's gamepad bindings reached a physical pad.
enum class Via : uint8_t { NONE, IDENTITY, LIST_INDEX };

struct Route {
    uint32_t jid = 0;          // 0 = this player's gamepad bindings are silent
    Via      via = Via::NONE;
};

// Resolve the identity-pinned pads. Exact model of ResolvePlayerPads():
// pass 1 keeps a still-valid resolution (instance ids are stable while the
// device stays connected, so nobody hops pads because the list reordered);
// pass 2 resolves the rest by identity in player order, with claims so two
// players configured for the same model cannot both take one unit. A
// configured-but-absent device stays UNRESOLVED rather than borrowing.
// `sticky` is in/out: the previous resolution in, the new one out.
inline void ResolveIdentities(const std::vector<Pad>& pads,
                              const PlayerProfile (&players)[kPlayers],
                              uint32_t (&sticky)[kPlayers]) {
    auto identity_of = [&](uint32_t jid) -> const std::string* {
        for (const Pad& p : pads) {
            if (p.jid == jid) return &p.identity;
        }
        return nullptr;  // not connected
    };

    std::vector<uint32_t> claimed;
    auto is_claimed = [&](uint32_t jid) {
        for (uint32_t c : claimed) {
            if (c == jid) return true;
        }
        return false;
    };

    for (int p = 0; p < kPlayers; ++p) {
        const uint32_t jid = sticky[p];
        const std::string* have = (jid != 0) ? identity_of(jid) : nullptr;
        if (have && !players[p].device_id.empty() &&
            *have == players[p].device_id) {
            claimed.push_back(jid);
        } else {
            sticky[p] = 0;
        }
    }

    for (int p = 0; p < kPlayers; ++p) {
        if (sticky[p] != 0) continue;
        const std::string& want = players[p].device_id;
        if (want.empty()) continue;  // legacy index routing -- no identity
        for (const Pad& pad : pads) {
            if (is_claimed(pad.jid)) continue;
            if (pad.identity == want) {
                sticky[p] = pad.jid;
                claimed.push_back(pad.jid);
                break;
            }
        }
    }
}

// The pad a player's gamepad bindings ACTUALLY read. Models the device
// selection at the top of Sample_Win32: a player carrying an identity reads
// its resolved pad and nothing else (silent while that pad is away); an
// identity-less player goes through GamepadAt(gamepad_index), where -1 means
// "first connected" and an out-of-range index means nothing at all.
inline Route RouteForPlayer(const std::vector<Pad>& pads,
                            const PlayerProfile& player,
                            uint32_t resolved_jid) {
    Route r;
    if (!player.has_gamepad_binding) return r;
    if (!player.device_id.empty()) {
        if (resolved_jid != 0) {
            r.jid = resolved_jid;
            r.via = Via::IDENTITY;
        }
        return r;
    }
    if (pads.empty()) return r;
    const int idx = player.legacy_index;
    if (idx < 0) {
        r.jid = pads.front().jid;
        r.via = Via::LIST_INDEX;
        return r;
    }
    if (idx < (int)pads.size()) {
        r.jid = pads[(size_t)idx].jid;
        r.via = Via::LIST_INDEX;
    }
    return r;
}

// True when both players' gamepad bindings read the SAME physical pad. The
// binder's stated rule is one device per player -- when this holds, one
// player's presses drive both characters and the other player's pad is dead,
// which is the shape of every "my inputs are being eaten with two
// controllers plugged in" report.
inline bool RoutesCollide(const Route& a, const Route& b) {
    return a.jid != 0 && a.jid == b.jid;
}

// ---------------------------------------------------------------------------
// Bind capture
// ---------------------------------------------------------------------------

// One pad's live state as the capture scan sees it. Axis values are the raw
// SDL range (-32768..32767).
struct PadState {
    uint32_t             jid = 0;
    std::vector<uint8_t> buttons;
    std::vector<int>     axes;
};

struct Capture {
    enum class Kind : uint8_t { NONE, PAD_BUTTON, PAD_AXIS };
    Kind kind      = Kind::NONE;
    int  pad_index = -1;   // index into the live vector
    int  code      = 0;    // SDL_GamepadButton | SDL_GamepadAxis
    int  axis_dir  = 0;    // +1 / -1 for an axis
};

namespace detail {
inline bool AxisPast(int v, int threshold, int dir) {
    return dir < 0 ? (v < -threshold) : (v > threshold);
}
// Scan one pad and return its first CHANGED-and-pressed source, if any.
inline Capture ScanPad(const PadState& live, const PadState* armed,
                       int pad_index, int axis_threshold) {
    Capture c;
    for (size_t b = 0; b < live.buttons.size(); ++b) {
        if (!live.buttons[b]) continue;
        // A button already down when the bind armed is not a press.
        if (armed && b < armed->buttons.size() && armed->buttons[b]) continue;
        c.kind = Capture::Kind::PAD_BUTTON;
        c.pad_index = pad_index;
        c.code = (int)b;
        return c;
    }
    for (size_t a = 0; a < live.axes.size(); ++a) {
        const int v = live.axes[a];
        const int dir = (v > 0) ? 1 : -1;
        if (!AxisPast(v, axis_threshold, dir)) continue;
        // An axis parked past the threshold at arm time is deadzone drift,
        // not a press. Only a NEW deflection in this direction counts.
        if (armed && a < armed->axes.size() &&
            AxisPast(armed->axes[a], axis_threshold, dir)) {
            continue;
        }
        c.kind = Capture::Kind::PAD_AXIS;
        c.pad_index = pad_index;
        c.code = (int)a;
        c.axis_dir = dir;
        return c;
    }
    return c;
}
}  // namespace detail

// Pick a gamepad capture. `armed` is the snapshot taken when the bind armed
// (same order/length as `live`; an empty vector means "no snapshot", which
// reproduces the old unguarded behaviour and is only used before arming).
// `prefer_pad_index` is the pad this player is already pinned to, scanned
// first so the other player resting on their stick cannot take the bind;
// pass -1 for no preference. Other pads are still reachable, so a player CAN
// rebind onto a new device.
inline Capture PickPadCapture(const std::vector<PadState>& live,
                              const std::vector<PadState>& armed,
                              int prefer_pad_index,
                              int axis_threshold) {
    auto armed_for = [&](size_t i) -> const PadState* {
        if (i >= armed.size()) return nullptr;
        if (armed[i].jid != live[i].jid) return nullptr;  // list changed
        return &armed[i];
    };
    if (prefer_pad_index >= 0 && prefer_pad_index < (int)live.size()) {
        const size_t i = (size_t)prefer_pad_index;
        Capture c = detail::ScanPad(live[i], armed_for(i), (int)i, axis_threshold);
        if (c.kind != Capture::Kind::NONE) return c;
    }
    for (size_t i = 0; i < live.size(); ++i) {
        if ((int)i == prefer_pad_index) continue;
        Capture c = detail::ScanPad(live[i], armed_for(i), (int)i, axis_threshold);
        if (c.kind != Capture::Kind::NONE) return c;
    }
    return Capture{};
}

}  // namespace inputroute
}  // namespace fm2k
