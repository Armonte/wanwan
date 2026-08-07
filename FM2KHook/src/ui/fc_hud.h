#pragma once

// fc_hud -- Fightcade-style in-game HUD for FM2K.
//
// Draws a persistent top bar inside cnc-ddraw's game-pixels rect
// (computed in imgui_overlay.cpp via cnc-ddraw's own layout math).
// Uses ImGui's foreground draw list directly -- no ImGui windows, no
// chrome -- so the HUD looks/feels native to the game frame rather
// than like a debug menu. Mirrors the architecture of fightcade-fbneo's
// `vid_overlay.cpp`: state setters push data in, a single per-frame
// `Render` call lays everything out in normalized coords.
//
// Phase 1 surface: top bar with `P1: <name>` / `P2: <name>` plus
// FPS and (when connected) ping. State setters for scores, chat,
// system messages, etc. land in later phases.
//
// All entry points are thread-safe; state setters can be called from
// any thread, Render must be called from the d3d9 render thread.

#include <cstdint>

namespace fc_hud {

// Lazy bookkeeping; no allocations. Safe to call repeatedly. Currently
// only initializes the internal state struct on first call.
void Init();

// Tear down any resources. No-op for v1 since everything is on the
// stack of internal state.
void Shutdown();

// Per-frame render. Must be called between ImGui::NewFrame() and
// ImGui::Render(). `rect_*` is the cnc-ddraw game-pixels rect on the
// current backbuffer (X, Y top-left in pixels; W, H size). All HUD
// drawing is clipped to that rect so we never paint over black bars.
void Render(int rect_x, int rect_y, int rect_w, int rect_h);

// ─── State setters ────────────────────────────────────────────────
//
// Mirrors the shape of Fightcade's `VidOverlaySet*` API. Each one
// just stashes its argument into the internal HUD state under a
// mutex; the next Render call picks up the new values. Setting a
// field repeatedly with the same value is fine and cheap.

// Update the player-name labels shown on the top bar. Pass UTF-8.
// Either side may be null/empty -- Render falls back to "P1" / "P2"
// for blank slots.
void SetPlayerNames(const char* p1, const char* p2);

// Push the latest stats. `fps` is current rendered fps, `ping_ms`
// is round-trip ping (0 when offline), `delay` is the GekkoNet
// input-delay value (frames). Render decides what's worth showing
// based on netplay state.
void SetStats(int fps, uint32_t ping_ms, int delay);

// Mark the netplay session as connected/disconnected. Render gates
// the ping + opponent-name visibility on this. Set true once the
// peer is known and active, false on disconnect.
void SetConnected(bool connected);

// True while the user has the chat-input box open (Slice F). Read
// by the game-input hooks to zero out local input for the duration
// (so typing 'a' doesn't make the local fighter throw a punch) and
// by Hook_WndProc to know it should forward keyboard messages to
// ImGui even when the F9 debug overlay isn't visible.
bool IsChatInputActive();

// ─── Runtime style controls ──────────────────────────────────────
//
// Live-tuned from the F9 debug overlay's HUD tab. All HUD metrics
// derive from `frame_scale = rect_h / 260` × these multipliers;
// ship-default values keep current visual sizing. The visibility
// flags hide whole sections so users can pare down to "just FPS"
// or "just chat" without code changes.
struct StyleControls {
    // Defaults tuned for typical 640×480 / 800×600 windowed FM2K +
    // upscaled cnc-ddraw output -- at scale 1.0 the HUD bar dominates
    // the screen on small game rects. 0.3 keeps it discreet at native
    // size and the user can dial up via the F9 slider when running
    // on big monitors.
    float scale          = 0.3f;   // 0.3..2.0; multiplier on bar/text/chat
    float bar_opacity    = 0.50f;  // 0..1; alpha multiplier for top-bar fill
    bool  show_top_bar   = true;
    bool  show_chat      = true;
    bool  show_system_message = true;
};

// Mutable accessor; the F9 HUD tab edits the returned reference
// directly, fc_hud::Render reads on each frame. Values persist for
// the process lifetime; not yet written to disk.
StyleControls& Style();

}  // namespace fc_hud
