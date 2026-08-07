// input_binder_ui.cpp -- ImGui render layer + capture state machine.
// THE ONLY TU a future Slint rewrite replaces: RenderBody/RenderWindow +
// the bind-capture FSM (AnyInputHeld/PollCapture) + label/conflict helpers.
// All model/state access goes through input_binder_internal.h.
#include "input_binder.h"
#include "input_binder_internal.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace FM2KInputBinder {
namespace {
// Capture state -- only one row at a time across both players.
struct CaptureCtx {
    bool active        = false;
    int  player        = -1;
    int  bit           = -1;
    bool alt           = false;  // false = primary slot, true = alt slot
    bool armed         = false;  // wait for all buttons released first
};
CaptureCtx g_capture;

std::string BindingLabel(const Binding& b) {
    switch (b.source) {
        case Binding::Source::NONE:
            return "<unbound>";
        case Binding::Source::KEYBOARD: {
            const char* n = SDL_GetScancodeName((SDL_Scancode)b.code);
            if (!n || !*n) return "Key " + std::to_string(b.code);
            return std::string("Key: ") + n;
        }
        case Binding::Source::GAMEPAD_BUTTON: {
            const char* n = SDL_GetGamepadStringForButton((SDL_GamepadButton)b.code);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Pad%d Btn:%s", b.gamepad_index,
                          (n && *n) ? n : std::to_string(b.code).c_str());
            return buf;
        }
        case Binding::Source::GAMEPAD_AXIS: {
            const char* n = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)b.code);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Pad%d Axis:%s%s", b.gamepad_index,
                          (n && *n) ? n : std::to_string(b.code).c_str(),
                          b.axis_dir < 0 ? "-" : "+");
            return buf;
        }
    }
    return "?";
}

bool BindingsConflict(const Binding& a, const Binding& b) {
    if (a.source != b.source || a.source == Binding::Source::NONE) return false;
    if (a.source == Binding::Source::KEYBOARD) return a.code == b.code;
    // Gamepad sources also need same gamepad index.
    if (a.gamepad_index != b.gamepad_index) return false;
    if (a.source == Binding::Source::GAMEPAD_BUTTON) return a.code == b.code;
    return a.code == b.code && a.axis_dir == b.axis_dir;
}
// ---------------------------------------------------------------------------
// Capture: poll for a fresh press and write it into the target slot.
// ---------------------------------------------------------------------------

bool AnyInputHeld() {
    SDL_PumpEvents();  // refresh polled state
    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);
    if (ks) {
        for (int i = 0; i < nkeys; ++i) if (ks[i]) return true;
    }
    // Tighter threshold for the "is something held?" check than for
    // the actual capture: PS3 / Qanba sticks frequently park an axis
    // at full +/- 32767 on plug-in (A3 reported as 32767 on cold
    // boot per user repro). With the capture threshold (16384/50%),
    // those stuck axes pin AnyInputHeld() to true forever and the
    // bind state machine never arms. Use 90% threshold for the held-
    // check -- only a deliberately-pushed stick passes -- and the
    // tighter 50% threshold still applies for capture itself, so
    // sloppy stick presses still bind cleanly.
    constexpr int kAxisHeldThreshold = 28000;  // ~85% of int16 range
    for (auto& kv : g_gamepad_handles) {
        SDL_Gamepad* gp = kv.second;
        if (!gp) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) return true;
        }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
            Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
            if (v > kAxisHeldThreshold || v < -kAxisHeldThreshold) return true;
        }
    }
    return false;
}

// Returns true and fills `out` if a fresh press was captured.
// Returns true when a binding was captured. `cancelled` is kept in
// the API for the caller's switch but we no longer treat any specific
// key as a built-in cancel -- ESC is bindable now (Pokemon CC and other
// 2DFM games map it to pause). The Cancel button next to the row is
// the explicit out-of-capture path.
bool PollCapture(Binding& out, bool& cancelled) {
    cancelled = false;
    SDL_PumpEvents();  // refresh polled state before reading
    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);

    if (ks) {
        for (int sc = 0; sc < nkeys; ++sc) {
            if (ks[sc]) {
                out.source = Binding::Source::KEYBOARD;
                out.code = sc;
                out.axis_dir = 0;
                out.gamepad_index = -1;
                return true;
            }
        }
    }

    // Gamepads. Diagnostic-first: dump every non-zero button/axis
    // we see from any opened gamepad on each poll. If the user clicks
    // Bind and reports "no input registered", the launcher log will
    // tell us whether SDL is seeing the press at all (gamepad isn't
    // really opened) or if our threshold logic is the gate.
    static uint32_t s_diag_emitted = 0;
    for (size_t i = 0; i < g_gamepad_ids.size(); ++i) {
        SDL_Gamepad* gp = GamepadAt((int)i);
        if (!gp) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) {
                if (s_diag_emitted < 60) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "InputBinder: poll saw gamepad %zu button %d held",
                        i, b);
                    ++s_diag_emitted;
                }
                out.source = Binding::Source::GAMEPAD_BUTTON;
                out.code = b;
                out.axis_dir = 0;
                out.gamepad_index = (int)i;
                return true;
            }
        }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
            Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
            if (v > kAxisSampleThreshold || v < -kAxisSampleThreshold) {
                if (s_diag_emitted < 60) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "InputBinder: poll saw gamepad %zu axis %d v=%d "
                        "(threshold=%d)",
                        i, a, (int)v, (int)kAxisSampleThreshold);
                    ++s_diag_emitted;
                }
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
}  // anonymous namespace

bool RenderBody(int player_slot) {
    if (!g_initialized) Init();
    if (player_slot < 0 || player_slot >= kPlayers) return false;

    bool changed = false;

    static double s_last_refresh = 0.0;
    double now = ImGui::GetTime();
    if (now - s_last_refresh > 1.5) { RefreshGamepadList(); s_last_refresh = now; }

    PlayerBindings& pb = g_players[player_slot];

    // Header / utility row
    ImGui::Text("Gamepads detected: %d", (int)g_gamepad_ids.size());
    if (!g_gamepad_ids.empty()) {
        ImGui::Indent();
        for (size_t i = 0; i < g_gamepad_ids.size(); ++i) {
            ImGui::Text("  [%zu] %s", i, GamepadNameAt((int)i));
        }
        ImGui::Unindent();
    }

    // ------------------------------------------------------------------
    // Per-player primary device picker.
    //
    // Without this, "P1 controller goes to player 2 too" is the default
    // experience -- every gamepad-bound row stores its own gamepad_index,
    // and ApplyDefaultsP1/P2 leave gamepad bindings empty so the user
    // adds them one by one through capture, all picking up index 0.
    //
    // The dropdown below selects ONE primary device for this player slot
    // and propagates that index across all of this player's gamepad-
    // sourced bindings in one shot. Keyboard rows are untouched. Future
    // SDL3 multi-keyboard support (per the SDL_KeyboardEvent.which
    // field) will add a "primary keyboard" option to the same dropdown
    // -- keyboard bindings on a player become device-scoped instead of
    // one shared queue-synced GetKeyboardState snapshot.
    // ------------------------------------------------------------------
    {
        // Identity-first preview: a player with a stored device identity
        // shows its resolved pad (or "disconnected: <name>" while the
        // physical device is away -- bindings are intentionally silent in
        // that state, never borrowed from the other player). Legacy
        // identity-less configs fall back to discovering the index from
        // the first gamepad-sourced binding, exactly as before.
        int current_idx = -1;  // -1 = keyboard only
        bool identity_disconnected = false;
        if (!pb.device_id.empty()) {
            current_idx = ResolvedPadListIndex(player_slot);
            identity_disconnected = (current_idx < 0);
        } else {
            for (size_t i = 0; i < (size_t)Bit::COUNT && current_idx < 0; ++i) {
                for (const Binding* arr : { pb.bits + 0, pb.bits_alt + 0 }) {
                    const Binding& b = arr[i];
                    if (b.source == Binding::Source::GAMEPAD_BUTTON ||
                        b.source == Binding::Source::GAMEPAD_AXIS) {
                        current_idx = b.gamepad_index;
                        break;
                    }
                }
            }
            // The dropdown PREVIEW must reflect reality, not a hopeful
            // default -- current_idx stays -1 ("Keyboard only") whenever no
            // gamepad-sourced binding exists, regardless of how many pads
            // are plugged in.
            if (current_idx >= (int)g_gamepad_ids.size()) {
                current_idx = -1;  // bound device disappeared (unplugged)
            }
        }

        // Build the combo preview string. -1 = keyboard only.
        char preview[192];
        if (identity_disconnected) {
            std::snprintf(preview, sizeof(preview), "(disconnected) %s",
                          pb.device_name.c_str());
        } else if (current_idx < 0) {
            std::snprintf(preview, sizeof(preview), "Keyboard only");
        } else {
            std::snprintf(preview, sizeof(preview), "[%d] %s",
                          current_idx, GamepadNameAt(current_idx));
        }

        ImGui::Spacing();
        ImGui::TextUnformatted(player_slot == 0
                                ? "Player 1 device:"
                                : "Player 2 device:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        const char* combo_id = (player_slot == 0) ? "##p1_dev" : "##p2_dev";
        if (ImGui::BeginCombo(combo_id, preview)) {
            // Keyboard only entry -- clears gamepad bindings on this player.
            bool sel = (current_idx < 0 && !identity_disconnected);
            if (ImGui::Selectable("Keyboard only", sel)) {
                // Switch this player to keyboard-only. Reset to the
                // keyboard defaults (P1 arrows / P2 numpad) and clear
                // alt entirely. One device per player; if the user
                // wants a second binding from the SAME keyboard for
                // a bit (rare), they can manually capture into alt.
                ApplyDefaults(player_slot);
                SetPlayerDevice(player_slot, -1);  // drop device identity
                changed = true;
            }
            // One row per available SDL gamepad.
            for (int i = 0; i < (int)g_gamepad_ids.size(); ++i) {
                char row[160];
                std::snprintf(row, sizeof(row), "[%d] %s",
                              i, GamepadNameAt(i));
                bool sel_i = (current_idx == i);
                if (ImGui::Selectable(row, sel_i)) {
                    // Switch this player to gamepad i. Primary gets
                    // the dpad + face/shoulder buttons. Alt gets ONLY
                    // the left-stick axes for directionals -- buttons
                    // in alt stay empty so each face button maps to
                    // exactly one bit (no double-fire on press). This
                    // is the CXL pattern: stick AND dpad both move
                    // the character; everything else is single-bound.
                    for (auto& b : pb.bits)     b = Binding{};
                    for (auto& b : pb.bits_alt) b = Binding{};
                    FillPrimaryAsGamepad(pb, i);
                    FillAltAsStickDirections(pb, i);
                    // Pin the physical device (GUID identity) so this
                    // player's pad survives list reordering on hotplug.
                    SetPlayerDevice(player_slot, i);
                    changed = true;
                }
                if (sel_i) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled(
            "Routes this player's gamepad bindings to the chosen device. "
            "Keyboard rows are unaffected. Multi-keyboard support is a "
            "follow-up -- when SDL_KeyboardID lands the dropdown will "
            "include per-keyboard entries.");
    }

    // Per-game profile toggle. Shows the active game name (if a game is
    // selected in the launcher) and lets the user fork/delete a per-game
    // override without leaving the binder. With no game selected the
    // checkbox is disabled and we silently edit the default profile.
    if (!g_active_game.empty()) {
        bool override_active = HasGameProfile();
        const std::string label = "Use override for \"" + g_active_game + "\"";
        if (ImGui::Checkbox(label.c_str(), &override_active)) {
            if (override_active) {
                ForkDefaultToGameProfile();
            } else {
                DeleteGameProfile();
            }
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(override_active ? "(per-game)" : "(default)");
    } else {
        ImGui::TextDisabled("Editing default profile (no game selected)");
    }

    // A playable config needs every ENGINE bit mapped: 4 directions + 6
    // buttons + PAUSE. Validate at SAVE -- the moment the user is standing
    // right here and can fix it -- instead of letting them discover it
    // mid-match. PAUSE is called out separately because it is the only one
    // that fails SILENTLY: an unmapped direction or button is obvious the
    // first time you press it, an unmapped PAUSE just does nothing forever.
    // Saving is still allowed (someone may be part-way through a remap); this
    // informs, it does not block.
    // Computed EVERY frame, not on the Save click. The launcher auto-saves on
    // any edit (launcher_ui_input.cpp: `if (RenderBody(slot)) Save();`), so a
    // click-gated warning would never appear in the UI where people actually
    // bind -- this body is shared by the launcher tab and the in-game overlay.
    char missing[160] = {};
    bool start_missing = false;
    for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
        if (((1u << i) & (unsigned)kEngineInputMask) == 0) continue;  // meta bits optional
        if (pb.bits[i].source     != Binding::Source::NONE ||
            pb.bits_alt[i].source != Binding::Source::NONE) continue;
        if (missing[0]) {
            std::strncat(missing, ", ", sizeof(missing) - std::strlen(missing) - 1);
        }
        std::strncat(missing, kBitNames[i], sizeof(missing) - std::strlen(missing) - 1);
        if ((Bit)i == Bit::START) start_missing = true;
    }

    if (ImGui::Button("Save")) {
        if (Save()) changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        for (int p = 0; p < kPlayers; ++p) ApplyDefaults(p);
        Load();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Defaults")) {
        // Reset to defaults for the device the user CURRENTLY has picked
        // -- slamming back to keyboard when they're on a gamepad is the
        // wrong move. We re-derive the picked device from the existing
        // bindings (same logic the dropdown uses for its preview text)
        // and dispatch to the matching default-fill helpers.
        int picked_idx = -1;
        for (size_t i = 0; i < (size_t)Bit::COUNT && picked_idx < 0; ++i) {
            for (const Binding* arr : { pb.bits + 0, pb.bits_alt + 0 }) {
                const Binding& b = arr[i];
                if (b.source == Binding::Source::GAMEPAD_BUTTON ||
                    b.source == Binding::Source::GAMEPAD_AXIS) {
                    picked_idx = b.gamepad_index;
                    break;
                }
            }
        }
        for (auto& b : pb.bits)     b = Binding{};
        for (auto& b : pb.bits_alt) b = Binding{};
        if (picked_idx >= 0 && picked_idx < (int)g_gamepad_ids.size()) {
            FillPrimaryAsGamepad(pb, picked_idx);
            FillAltAsStickDirections(pb, picked_idx);
        } else {
            ApplyDefaults(player_slot);
        }
        changed = true;
    }

    if (missing[0]) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f),
                           "UNMAPPED: %s", missing);
        if (start_missing) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f),
                               "PAUSE is unmapped -- the pause button will do "
                               "nothing for this player.");
        }
    }

    // Live gamepad-state debug panel. Shows per-frame what SDL is
    // reporting from each opened device. If you press a button and
    // it doesn't change here, SDL isn't seeing the input at all
    // (HIDAPI driver issue, controller in wrong mode, etc.) -- vs.
    // if it DOES change here but Bind doesn't catch it, the bug is
    // in the binder's capture state machine.
    SDL_PumpEvents();
    if (ImGui::CollapsingHeader("Live gamepad state (debug)",
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (g_gamepad_handles.empty()) {
            ImGui::TextDisabled("(no gamepads opened)");
        }
        size_t idx = 0;
        for (auto& kv : g_gamepad_handles) {
            SDL_Gamepad* gp = kv.second;
            if (!gp) continue;
            const char* name = SDL_GetGamepadName(gp);
            ImGui::Text("[%zu] jid=%u %s",
                idx, (unsigned)kv.first, name ? name : "?");
            // Buttons -- print the pressed-button names compactly.
            std::string pressed;
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
                if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) {
                    if (!pressed.empty()) pressed += " ";
                    pressed += "B" + std::to_string(b);
                }
            }
            ImGui::Text("  buttons: %s",
                pressed.empty() ? "(none)" : pressed.c_str());
            // Axes -- show all six even when zero so you can watch
            // them move in real time.
            char axline[256] = {};
            char* p = axline;
            char* end = axline + sizeof(axline);
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && p < end - 16; ++a) {
                Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
                int n = std::snprintf(p, end - p, "A%d=%6d ", a, (int)v);
                if (n > 0) p += n;
            }
            ImGui::Text("  axes: %s", axline);
            ++idx;
        }
        ImGui::TextDisabled(
            "If buttons stay (none) when you press: SDL isn't seeing "
            "input. Wrong controller mode or HIDAPI driver issue.");

        // Capture-state diagnostics. Tells us which gate the bind
        // state machine is stuck behind when a press isn't being
        // captured.
        const bool any_held = AnyInputHeld();
        ImGui::Text("Capture state: active=%d armed=%d player=%d bit=%d",
            g_capture.active ? 1 : 0,
            g_capture.armed  ? 1 : 0,
            g_capture.player,
            g_capture.bit);
        ImGui::Text("AnyInputHeld() = %d", any_held ? 1 : 0);
        if (g_capture.active && !g_capture.armed && any_held) {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
                "Waiting for full release before sampling -- release "
                "ALL keys/sticks/buttons. If a stick axis is sitting "
                "past 50%% threshold at idle (deadzone drift), arm "
                "will never trigger.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Pads")) {
        RefreshGamepadList();
    }

    ImGui::Separator();

    // Compute conflicts within this player. Each slot (primary + alt)
    // can independently conflict with another bit's primary or alt, so
    // we walk the cross-product per-slot. A bit's row goes red when
    // EITHER slot collides with anything.
    bool conflict_pri[(size_t)Bit::COUNT] = {};
    bool conflict_alt[(size_t)Bit::COUNT] = {};
    auto check_pair = [&](size_t i, bool i_alt, size_t j, bool j_alt) {
        const Binding& a = i_alt ? pb.bits_alt[i] : pb.bits[i];
        const Binding& b = j_alt ? pb.bits_alt[j] : pb.bits[j];
        if (BindingsConflict(a, b)) {
            (i_alt ? conflict_alt : conflict_pri)[i] = true;
            (j_alt ? conflict_alt : conflict_pri)[j] = true;
        }
    };
    for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
        // Same-bit primary vs. alt isn't a conflict -- that's the WHOLE
        // POINT (stick + dpad on the same direction). Skip i==j entirely
        // and only flag cross-bit collisions.
        for (size_t j = i + 1; j < (size_t)Bit::COUNT; ++j) {
            check_pair(i, false, j, false);
            check_pair(i, false, j, true);
            check_pair(i, true,  j, false);
            check_pair(i, true,  j, true);
        }
    }

    // Capture handling -- only act on rows owned by this player+bit.
    if (g_capture.active && g_capture.player == player_slot) {
        if (!g_capture.armed) {
            // Wait for full release before sampling so the click that
            // started the capture isn't read back.
            if (!AnyInputHeld()) {
                g_capture.armed = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "InputBinder: armed for player=%d bit=%d -- waiting for press",
                    g_capture.player, g_capture.bit);
            }
        } else {
            Binding cap{};
            bool cancel = false;
            if (PollCapture(cap, cancel)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "InputBinder: captured src=%d code=%d axis_dir=%d gp=%d "
                    "for player=%d bit=%d",
                    (int)cap.source, cap.code, cap.axis_dir, cap.gamepad_index,
                    g_capture.player, g_capture.bit);
                if (!cancel) {
                    if (g_capture.alt) pb.bits_alt[g_capture.bit] = cap;
                    else               pb.bits[g_capture.bit]     = cap;
                    // Gamepad capture also pins this player's device
                    // identity to the pad that was pressed -- one device
                    // per player, resolved by GUID from here on.
                    if (cap.source == Binding::Source::GAMEPAD_BUTTON ||
                        cap.source == Binding::Source::GAMEPAD_AXIS) {
                        SetPlayerDevice(player_slot, cap.gamepad_index);
                    }
                    changed = true;
                }
                g_capture.active = false;
                g_capture.armed = false;
                g_capture.player = -1;
                g_capture.bit = -1;
                g_capture.alt = false;
            }
        }
    }

    // Bindings table -- primary AND alt slot per bit. Both slots OR
    // together at sample time so a single direction can fire from
    // EITHER source. CXL-style stick+dpad: drop the dpad in primary
    // and the stick axis in alt (or vice-versa); both work in-game.
    // Typical user keeps primary = keyboard, alt = gamepad.
    if (ImGui::BeginTable("bindings", 5,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Bit",     ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Alt",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();

        // Per-slot row renderer. col_label/col_btns are the table column
        // indices for the slot's text + buttons; alt_slot picks which
        // member of pb to read/write.
        auto render_slot = [&](size_t i, bool alt_slot,
                               int col_label, int col_btns) {
            const Binding& cur = alt_slot ? pb.bits_alt[i] : pb.bits[i];
            const bool conflict = (alt_slot ? conflict_alt : conflict_pri)[i];
            const bool waiting = g_capture.active &&
                                 g_capture.player == player_slot &&
                                 g_capture.bit == (int)i &&
                                 g_capture.alt == alt_slot;

            ImGui::TableSetColumnIndex(col_label);
            if (waiting) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                                   "Press a key/button... (Cancel to abort)");
            } else if (conflict) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                                   BindingLabel(cur).c_str());
            } else {
                ImGui::TextUnformatted(BindingLabel(cur).c_str());
            }

            ImGui::TableSetColumnIndex(col_btns);
            ImGui::PushID(alt_slot ? "a" : "p");
            if (waiting) {
                if (ImGui::Button("Cancel", ImVec2(60, 0))) {
                    g_capture = CaptureCtx{};
                }
            } else {
                if (ImGui::Button("Bind", ImVec2(60, 0))) {
                    g_capture.active = true;
                    g_capture.armed = false;
                    g_capture.player = player_slot;
                    g_capture.bit = (int)i;
                    g_capture.alt = alt_slot;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(60, 0))) {
                if (alt_slot) pb.bits_alt[i] = Binding{};
                else          pb.bits[i]     = Binding{};
                changed = true;
            }
            ImGui::PopID();
        };

        for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
            ImGui::TableNextRow();
            ImGui::PushID((int)i);

            ImGui::TableSetColumnIndex(0);
            // An unbound PAUSE row is silently fatal: the engine only toggles
            // g_game_paused on a rising edge of bit 0x400 built from what the
            // binder returns, so with no PAUSE binding the pause button is
            // simply dead and nothing else looks wrong. Every other bit
            // degrades visibly (you can see the character not move). Flag it
            // here rather than repairing the profile behind the user's back.
            {
                const bool is_pause = ((Bit)i == Bit::START);
                const bool unbound  =
                    pb.bits[i].source     == Binding::Source::NONE &&
                    pb.bits_alt[i].source == Binding::Source::NONE;
                if (is_pause && unbound) {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "%s",
                                       kBitNames[i]);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "PAUSE is not bound -- the pause button will not "
                            "work for this player.");
                    }
                } else {
                    ImGui::TextUnformatted(kBitNames[i]);
                }
            }

            render_slot(i, /*alt=*/false, /*col_label=*/1, /*col_btns=*/2);
            render_slot(i, /*alt=*/true,  /*col_label=*/3, /*col_btns=*/4);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("Sampled mask: 0x%03X", (unsigned)Sample(player_slot));

    return changed;
}

bool RenderWindow(int player_slot, bool* p_open) {
    if (player_slot < 0 || player_slot >= kPlayers) return false;
    char title[64];
    std::snprintf(title, sizeof(title),
                  "FM2K Input Binder - Player %d", player_slot + 1);
    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, p_open)) { ImGui::End(); return false; }
    bool changed = RenderBody(player_slot);
    ImGui::End();
    return changed;
}
}  // namespace FM2KInputBinder
