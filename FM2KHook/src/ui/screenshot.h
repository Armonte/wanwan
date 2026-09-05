#pragma once

#include <string>
#include <cstdint>

// Hook-side framebuffer → PNG writer for the auto-capture banner
// pipeline. Triggered by main_loop_trampoline at game_mode boundaries
// when the FM2K_AUTO_CAPTURE env var is set. See
// docs/dev/banner_pipeline.md for the full state machine.
//
// Reads the game's own framebuffer (not the window: a GDI BitBlt path
// existed and was superseded) and encodes with stb_image_write. PNG is the
// only supported output format; callers append ".png" to filename.

namespace FM2KCapture {

// Initialize once at hook bring-up. Picks up FM2K_AUTO_CAPTURE +
// FM2K_CAPTURE_DIR from the environment; every capture call below becomes
// a no-op if FM2K_AUTO_CAPTURE is unset. Idempotent -- safe to call
// repeatedly.
void Init();

// Tear down the GDI+ token. Called from the hook DLL detach path so
// we don't leak the GDI+ subsystem state.
void Shutdown();

// True when FM2K_AUTO_CAPTURE was set at Init time. Lets the
// trampoline cheaply skip the entire state machine in non-capture
// runs.
bool IsActive();

// Snapshot the current framebuffer into a hold buffer. Cheap (one memcpy of
// 600 KB) and called once per frame while a wanted state is on screen.
void RememberFrame();

// Encode and write whatever RememberFrame last held. Returns false if
// nothing usable was held. Used at state EXIT so the saved image is the last
// fully-drawn frame of that state rather than a guess at when it settled.
bool WriteRememberedFrame(const std::string& filename);

// The per-character sweep (hooks_game_mode.cpp, FM2K_CAPTURE_CHARS=1):
// while `g_sweep_active` the CSS autoplay neither wanders nor confirms
// and its dwell clock is frozen; each `g_sweep_nudge` is consumed by the
// P1 autoplay input as ONE press of RIGHT, which steps the cursor to the
// next roster cell without knowing the grid's shape.
extern bool g_sweep_active;
extern bool g_sweep_nudge;        // one press of RIGHT: next cell in the row
extern bool g_sweep_nudge_down;   // one press of DOWN: next row

// Forget the current best-frame score without writing. Used between samples
// of a sequence so each window competes on its own, instead of one early
// bright card winning every remaining slot.
void ResetHeldScore();

// HOW LONG THE PICTURE HAS BEEN STILL, in consecutive frames, as counted by
// RememberFrame. These games fade, wipe and slide between screens, and a
// shot taken during one of those is a smeared half-frame nobody would pick.
// A frame counter alone cannot avoid that: it does not know whether the
// transition has finished, only how long it has been waiting. This compares
// a coarse thumbnail against the previous frame's, so a full-screen fade
// reads as motion while an animating portrait or a blinking cursor does not.
// Zero means the picture is moving right now.
int FramesSettled();

// Start counting stillness again. Call when the screen is about to change
// on purpose -- a state switch, or a cursor press in the roster sweep.
void ResetSettle();

}  // namespace FM2KCapture
