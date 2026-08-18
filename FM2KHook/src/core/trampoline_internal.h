#pragma once
// Shared decls for the split main_loop_trampoline.cpp TUs (trampoline_render /
// pacing / battle / css / native / spectator + the main_loop_trampoline.cpp
// shell). Engine-agnostic: the trampoline drives both FM2K and FM95 with no
// ENGINE_FM95 branching. Pure linkage move -- the cross-TU helpers were
// file-static in the monolith; they're now extern (defs in their per-concern
// TU). No namespace needed: grep confirms none of these names collide with a
// real symbol elsewhere (only comment references).
#include "main_loop_trampoline.h"   // LoopPhase
#include "globals.h"                 // RunGameLoopFunc / ProcessGameInputsFunc / ...
#include "../netplay/savestate.h"    // WaveCAddrs + SaveState_* (was a mid-file include in the monolith)
#include <cstdint>

// Game-side symbols the trampoline calls directly (defs in globals.cpp / hooks.cpp).
extern RunGameLoopFunc        original_run_game_loop;
extern ProcessGameInputsFunc  original_process_game_inputs;
extern UpdateGameStateFunc    original_update_game;
extern RenderGameFunc         original_render_game;
extern bool g_frame_pending_render;
extern "C" void Hook_CheckGameModeTransition_Public();
extern "C" void Hook_RenderDiagnostics_Tick();
extern "C" void Hook_BattleDiag_TickIfActive();

// #63 diag: original_render_game() wall-time, set by RenderFrameWithSnapshot
// (defined in trampoline_render.cpp), read by the offline/frametime profilers.
extern uint64_t g_render_game_only_ns;

// Cross-TU helpers (were file-static in main_loop_trampoline.cpp).
void RenderFrameWithSnapshot();                                   // trampoline_render.cpp
void RunBattleTick();                                             // trampoline_battle.cpp
void RunCssTick();                                                // trampoline_css.cpp
void RunNativeTick();                                             // trampoline_native.cpp
void RunSpectatorTick();                                          // trampoline_spectator.cpp
void SpecWatchdogTick(bool offline_replay);                       // trampoline_spec_watchdog.cpp
void SleepToTarget(uint64_t start_qpc, uint32_t target_ms, float frames_ahead = 0.0f);  // trampoline_pacing.cpp
void MaybeLogFrametime(uint64_t tick_start);                      // trampoline_pacing.cpp
void MaybeLogOfflineSections(uint64_t pgi, uint64_t ug, uint64_t render, uint32_t render_rand);  // trampoline_pacing.cpp
