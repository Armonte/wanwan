#ifndef GAME_PATCHES_H
#define GAME_PATCHES_H

void PatchVsRoundCase200T4FalsePositive();

// Neuter the FM2K WndProc's F4 / Alt+Enter handlers so they no longer
// toggle `g_graphics_mode` (the runtime mirror of GameScreenMode) and
// re-init DirectDraw underneath cnc-ddraw. Called only when cnc-ddraw
// redirect is active -- without that, F4 keeps its stock toggle. Both
// patches flip a 2-byte `jnz` -> `jmp` so the toggle body is never
// entered; the rest of the WndProc dispatch is unaffected.
//
//   FM2K  WonderfulWorld_ver_0946.exe:
//     0x4060f3  jnz +0x1F (F4 body)        75 1F  ->  EB 1F
//     0x406288  jnz +0x20 (Alt+Enter body) 75 20  ->  EB 20
//
// cnc-ddraw's WH_KEYBOARD hook is what should normally catch these
// keys before the WndProc sees them; this is belt-and-suspenders for
// the (rare) window where the keyboard hook isn't yet installed
// (cnc-ddraw installs it after `g_ddraw.gui_thread_id` is known,
// i.e. after the first DirectDraw call).
void NeuterFullscreenTogglesForCncDdraw();

#endif // GAME_PATCHES_H
