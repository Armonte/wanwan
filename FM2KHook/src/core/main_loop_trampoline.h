// Main-loop trampoline — replaces the game's native main_game_loop @0x405AD0.
//
// During battle under GekkoNet the trampoline IS the game loop. During CSS
// and menus it emulates the native loop's structure so gameplay code that
// relies on main_game_loop's prologue writes keeps working. This eliminates
// the cadence mismatch between forward-sim and rollback-replay that was
// causing object_pool divergence — main_game_loop's per-iteration state
// writes never happen on forward any more either, so there's nothing to
// mismatch against.
//
// Hook chain: Hook_RunGameLoop @ FM2K::ADDR_RUN_GAME_LOOP (0x405AD0) detours
// to TrampolineMainLoop(). No calls to original_run_game_loop — we own the
// outer loop.
#pragma once
#include <windows.h>
#include <cstdint>

enum class LoopPhase {
    NATIVE,             // Menu / intro / results — full original flow reproduced
    CSS,                // Character select — control-channel-lockstep, no GekkoNet
    TRAMPOLINE_BATTLE,  // GekkoNet drives sim + save + load + advance
    SPECTATOR_PLAYBACK  // Inputs sourced from spectator stream queue, no GekkoNet
};

// Entry point replacing main_game_loop wholesale (FM2K). Returns the same
// BOOL the original did: non-zero on normal exit via WM_QUIT, zero on
// abnormal. Pumps messages, calls TrampolineFrameTick, paces with QPC.
BOOL TrampolineMainLoop();

#if defined(ENGINE_FM95)
// FM95: install the inline patch at CPW's WinMain loop head (0x40AD67) so
// TrampolineMainLoop owns the loop the way FM2K owns main_game_loop. Call from
// Setup_Hooks before WinMain runs. Sets g_fm95_loop_owned on success.
bool Fm95InstallOwningLoop();
#endif

// Per-frame engine work — extracted body callable from either
// TrampolineMainLoop (FM2K, host loop replaced) or Hook_UpdateGameState
// (FM95, where the host's WinMain owns the message pump and timing).
// Returns the LoopPhase actually executed so callers can decide whether
// the host's natural render call should still fire (NATIVE) or be skipped
// (BATTLE / CSS — the tick already drove update + render via AdvanceEvent
// / RenderFrameWithSnapshot).
LoopPhase TrampolineFrameTick();

// FM95 host-driven variant. Classifies the phase and dispatches the tick for
// BATTLE / CSS / SPECTATOR only; for NATIVE it does NOTHING and returns
// NATIVE, letting the caller's own fall-through drive the single host update.
// TrampolineFrameTick() would RunNativeTick() (a full update) for NATIVE, and
// the FM95 caller then falls through to original_update_game() = double-tick
// on title/menus. Use this from Hook_UpdateGameState's FM95 branch.
LoopPhase TrampolineFrameTickHostDriven();

// [EB]-OPCODE DIAGNOSTIC: log shake-effect timer + camera position at the
// given render-boundary phase (PRE-SAVE / PRE-RENDER / POST-RENDER /
// POST-RESTORE). Gated on FM2K_EB_DIAG=1 env var; output goes to a per-PID
// `FM2K_eb_diag_pid<PID>.log` next to the game EXE (NOT the main launcher
// log). Used by both the trampoline render path AND Hook_RenderGame so
// FM2K_BYPASS_TRAMPOLINE=1 still produces diag output for A/B testing
// against the trampoline path. Cheap when env not set (single static-bool
// branch). Frame counter advances on each PRE-SAVE call.
void EbDiag_Dump(const char* tag);
