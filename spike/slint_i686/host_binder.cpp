// host_binder.cpp -- see host_binder.h. The capture state machine and the
// binding labels are lifted from the ImGui layer (input_binder_ui.cpp) so
// the two launchers bind identically; everything else calls the core.
#include "host_binder.h"

#include "input_binder.h"
#include "input_binder_internal.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace hostgui {
namespace {

using namespace FM2KInputBinder;

// ---- capture, one row at a time across both players --------------------

struct Capture {
    bool active = false;
    int  player = -1;
    int  bit    = -1;
    bool alt    = false;
    bool armed  = false;   // every key/button released since the click
} g_cap;

std::string g_game;   // the stem the profile follows

// Friendlier than the ImGui labels: a button box reads "Z", "Pad: A",
// "Stick: Left", not "Key: Z" / "Pad0 Btn:a".
std::string Label(const Binding& b) {
    switch (b.source) {
        case Binding::Source::NONE: return "";
        case Binding::Source::KEYBOARD: {
            const char* n = SDL_GetScancodeName((SDL_Scancode)b.code);
            return (n && *n) ? std::string(n) : "Key " + std::to_string(b.code);
        }
        case Binding::Source::GAMEPAD_BUTTON: {
            const char* n = SDL_GetGamepadStringForButton((SDL_GamepadButton)b.code);
            return std::string("Pad: ") + ((n && *n) ? n : std::to_string(b.code).c_str());
        }
        case Binding::Source::GAMEPAD_AXIS: {
            const char* n = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)b.code);
            std::string s = std::string("Stick: ") + ((n && *n) ? n : std::to_string(b.code).c_str());
            s += b.axis_dir < 0 ? " -" : " +";
            return s;
        }
    }
    return "?";
}

bool AnyInputHeld() {
    SDL_PumpEvents();
    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);
    if (ks) for (int i = 0; i < nkeys; ++i) if (ks[i]) return true;
    // 85% of the axis range: a stick parked at full deflection on plug-in
    // (PS3 / Qanba) must not pin this to true forever (input_binder_ui.cpp).
    constexpr int kAxisHeldThreshold = 28000;
    for (auto& kv : g_gamepad_handles) {
        SDL_Gamepad* gp = kv.second;
        if (!gp) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
            if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) return true;
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
            const Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
            if (v > kAxisHeldThreshold || v < -kAxisHeldThreshold) return true;
        }
    }
    return false;
}

// A fresh press into `out`. No built-in cancel key: ESC is bindable (some
// 2DFM games map it to pause); the UI's cancel is the explicit way out.
bool PollCapture(Binding& out) {
    SDL_PumpEvents();
    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);
    if (ks) {
        for (int sc = 0; sc < nkeys; ++sc) {
            if (ks[sc]) {
                out = Binding{};
                out.source = Binding::Source::KEYBOARD;
                out.code = sc;
                return true;
            }
        }
    }
    for (size_t i = 0; i < g_gamepad_ids.size(); ++i) {
        SDL_Gamepad* gp = GamepadAt((int)i);
        if (!gp) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) {
                out = Binding{};
                out.source = Binding::Source::GAMEPAD_BUTTON;
                out.code = b;
                out.gamepad_index = (int)i;
                return true;
            }
        }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
            const Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
            if (v > kAxisSampleThreshold || v < -kAxisSampleThreshold) {
                out = Binding{};
                out.source = Binding::Source::GAMEPAD_AXIS;
                out.code = a;
                out.axis_dir = (v > 0) ? 1 : -1;
                out.gamepad_index = (int)i;
                return true;
            }
        }
    }
    return false;
}

const char* kSocdLabels[7] = {
    "Launcher default",
    "R wins L+R, U wins U+D",
    "Hitbox: L+R = neutral, U wins U+D",
    "R wins L+R, U+D = neutral",
    "L+R = neutral, U+D = neutral",
    "Same as the first (CCCaster alias)",
    "Same as Hitbox (CCCaster alias)",
};

// The eleven bits a playable config needs, in the order a player thinks
// of them. OPTION / FN1 / FN2 are hook features and are not offered here.
const struct { Bit bit; const char* name; bool required; } kRows[] = {
    {Bit::UP, "Up", true}, {Bit::DOWN, "Down", true}, {Bit::LEFT, "Left", true}, {Bit::RIGHT, "Right", true},
    {Bit::A, "A", true}, {Bit::B, "B", true}, {Bit::C, "C", true},
    {Bit::D, "D", true}, {Bit::E, "E", true}, {Bit::F, "F", true},
    {Bit::START, "Pause / Start", true},
};

std::string Clean(std::string s) {
    for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string Unmapped(const PlayerBindings& pb) {
    std::string out;
    for (const auto& r : kRows) {
        const size_t i = (size_t)r.bit;
        if (pb.bits[i].source == Binding::Source::NONE && pb.bits_alt[i].source == Binding::Source::NONE) {
            if (!out.empty()) out += ", ";
            out += r.name;
        }
    }
    return out;
}

int Field(const std::string& s, size_t idx) {
    size_t start = 0;
    for (size_t i = 0; i < idx; ++i) {
        start = s.find(':', start);
        if (start == std::string::npos) return -99;
        ++start;
    }
    return std::atoi(s.c_str() + start);
}

}  // namespace

void BinderInit(const std::string& game_stem) {
    g_game = game_stem;
    if (!g_game.empty()) SetGameProfile(g_game.c_str());
    Init();
}

void BinderSetGame(const std::string& game_stem) {
    if (game_stem == g_game) return;
    g_game = game_stem;
    SetGameProfile(g_game.empty() ? nullptr : g_game.c_str());
    Load();
    g_cap = Capture{};
}

std::string BinderPlayers() {
    std::string out;
    for (int p = 0; p < kPlayers; ++p) {
        const PlayerBindings& pb = g_players[p];
        const int idx = ResolvedPadListIndex(p);
        std::string dev;
        if (idx >= 0) dev = GamepadNameAt(idx);
        else if (!pb.device_id.empty()) dev = "Disconnected: " + pb.device_name;
        else dev = "Keyboard";
        const int socd = (pb.socd_mode >= 0 && pb.socd_mode <= 5) ? pb.socd_mode + 1 : 0;
        out += std::to_string(p) + '\t' + Clean(dev) + '\t' + std::to_string(idx) + '\t' +
               std::to_string(g_gamepad_ids.size()) + '\t' + std::to_string(socd) + '\t' +
               kSocdLabels[socd] + '\t' + (HasGameProfile() ? "1" : "0") + '\t' +
               Clean(g_game) + '\t' + Clean(Unmapped(pb)) + '\n';
    }
    return out;
}

std::string BinderRows() {
    std::string out;
    for (int p = 0; p < kPlayers; ++p) {
        const PlayerBindings& pb = g_players[p];
        for (const auto& r : kRows) {
            const size_t i = (size_t)r.bit;
            int capturing = 0;
            if (g_cap.active && g_cap.player == p && g_cap.bit == (int)r.bit) capturing = g_cap.alt ? 2 : 1;
            out += std::to_string(p) + '\t' + std::to_string((int)r.bit) + '\t' + r.name + '\t' +
                   Clean(Label(pb.bits[i])) + '\t' + Clean(Label(pb.bits_alt[i])) + '\t' +
                   std::to_string(capturing) + '\t' + (r.required ? "1" : "0") + '\n';
        }
    }
    return out;
}

bool BinderAction(const std::string& a) {
    const auto starts = [&](const char* p) { return a.rfind(p, 0) == 0; };
    if (a == "cancel") { g_cap = Capture{}; return true; }
    if (starts("bind:")) {
        const int p = Field(a, 1), bit = Field(a, 2), slot = Field(a, 3);
        if (p < 0 || p >= kPlayers || bit < 0 || bit >= (int)Bit::COUNT) return false;
        g_cap = Capture{};
        g_cap.active = true; g_cap.player = p; g_cap.bit = bit; g_cap.alt = (slot == 1);
        g_cap.armed = !AnyInputHeld();
        return true;
    }
    if (starts("clear:")) {
        const int p = Field(a, 1), bit = Field(a, 2), slot = Field(a, 3);
        if (p < 0 || p >= kPlayers || bit < 0 || bit >= (int)Bit::COUNT) return false;
        (slot == 1 ? g_players[p].bits_alt : g_players[p].bits)[(size_t)bit] = Binding{};
        Save();
        return true;
    }
    if (starts("device:")) {
        const int p = Field(a, 1), idx = Field(a, 2);
        if (p < 0 || p >= kPlayers) return false;
        SetPlayerDevice(p, idx);
        Save();
        return true;
    }
    if (starts("padfill:")) {
        const int p = Field(a, 1);
        if (p < 0 || p >= kPlayers) return false;
        const int idx = ResolvedPadListIndex(p);
        if (idx < 0) return false;
        FillPrimaryAsGamepad(g_players[p], idx);
        Save();
        return true;
    }
    if (starts("stickfill:")) {
        const int p = Field(a, 1);
        if (p < 0 || p >= kPlayers) return false;
        const int idx = ResolvedPadListIndex(p);
        if (idx < 0) return false;
        FillAltAsStickDirections(g_players[p], idx);
        Save();
        return true;
    }
    if (starts("defaults:")) {
        const int p = Field(a, 1);
        if (p < 0 || p >= kPlayers) return false;
        ApplyDefaults(p);
        Save();
        return true;
    }
    if (starts("socd:")) {
        const int p = Field(a, 1), m = Field(a, 2);
        if (p < 0 || p >= kPlayers || m < 0 || m > 6) return false;
        g_players[p].socd_mode = m - 1;
        Save();
        return true;
    }
    if (starts("profile:")) {
        const int on = Field(a, 2);
        if (on == 1) { if (!HasGameProfile()) ForkDefaultToGameProfile(); }
        else if (HasGameProfile()) DeleteGameProfile();
        Load();
        return true;
    }
    return false;
}

bool BinderTick() {
    bool changed = false;
    static Uint64 s_last_refresh = 0;
    const Uint64 now = SDL_GetTicks();
    if (now - s_last_refresh > 1500) {
        s_last_refresh = now;
        const size_t before = g_gamepad_ids.size();
        RefreshGamepads();
        if (g_gamepad_ids.size() != before) changed = true;
    }
    if (g_cap.active) {
        if (!g_cap.armed) {
            if (!AnyInputHeld()) g_cap.armed = true;
        } else {
            Binding b;
            if (PollCapture(b)) {
                PlayerBindings& pb = g_players[g_cap.player];
                (g_cap.alt ? pb.bits_alt : pb.bits)[(size_t)g_cap.bit] = b;
                // A pad press names the player's device too (the shipping
                // binder does the same in its capture path).
                if (b.source != Binding::Source::KEYBOARD && pb.device_id.empty())
                    SetPlayerDevice(g_cap.player, b.gamepad_index);
                Save();
                g_cap = Capture{};
                changed = true;
            }
        }
    }
    return changed;
}

void BinderShutdown() { Shutdown(); }

}  // namespace hostgui
