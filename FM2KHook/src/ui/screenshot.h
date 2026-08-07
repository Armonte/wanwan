#pragma once

#include <string>
#include <cstdint>

// Hook-side framebuffer → PNG writer for the auto-capture banner
// pipeline. Triggered by main_loop_trampoline at game_mode boundaries
// when the FM2K_AUTO_CAPTURE env var is set. See
// docs/dev/banner_pipeline.md for the full state machine.
//
// Writes the game's current window contents to <out_dir>/<filename>
// using GDI BitBlt + GDI+ for PNG encoding. No new dependencies --
// gdiplus.dll ships with every Windows since XP. PNG is the only
// supported output format; callers append ".png" to filename.

namespace FM2KCapture {

// Initialize once at hook bring-up. Picks up FM2K_AUTO_CAPTURE +
// FM2K_CAPTURE_DIR from the environment; subsequent SaveScreenshot
// calls become no-ops if FM2K_AUTO_CAPTURE is unset. Idempotent --
// safe to call repeatedly.
void Init();

// Tear down the GDI+ token. Called from the hook DLL detach path so
// we don't leak the GDI+ subsystem state.
void Shutdown();

// True when FM2K_AUTO_CAPTURE was set at Init time. Lets the
// trampoline cheaply skip the entire state machine in non-capture
// runs.
bool IsActive();

// Save the game window to "<capture_dir>/<filename>" if Init() was
// called with capture enabled. `filename` is just the basename (no
// directory components); the writer prepends FM2K_CAPTURE_DIR.
// Returns true on success, false otherwise (and logs the reason).
bool SaveScreenshot(const std::string& filename);

}  // namespace FM2KCapture
