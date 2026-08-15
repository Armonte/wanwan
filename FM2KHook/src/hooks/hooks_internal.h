#pragma once
// Cross-TU surface for the split hooks.cpp (hooks_vfs / hooks_getinput /
// hooks_input / hooks_game_mode / hooks_update / hooks_render / hooks_rng +
// the hooks.cpp install/determinism shell). Engine handling stays INLINE
// (`if constexpr (FM2K::kIsFM95)`); no engine files.
//
// Almost every file-scope static is cluster-local and travels with its TU.
// This header holds only what genuinely crosses TUs: the per-cluster install
// entry points (each does its own MH_CreateHook + MH_QueueEnableHook, called
// by InitializeHooks in the shell -- MH_ApplyQueued at the end makes order
// irrelevant) plus the few helpers called across clusters that aren't already
// in the public hooks.h.
#include <cstdint>

// Per-cluster MinHook install entry points (return false on a fatal failure
// the shell should propagate; warn-and-continue failures return true).
bool InstallInputHooks();    // hooks_getinput.cpp: get_player_input (FM2K + FM95 split)
bool InstallUpdateHook();    // hooks_update.cpp: update_game
bool InstallRenderHooks();   // hooks_render.cpp: render_game + blit/sprite (env-gated) + dispatch_script_sound
bool InstallRngHook();       // hooks_rng.cpp: game_rand
bool InstallVfsHooks();      // hooks_vfs.cpp: CreateFile*/ReadFile/SetFilePointer*/CloseHandle + FPK state + LoadStageFileAlt(FM95)

// Cross-cluster helpers not already declared in hooks.h (extern "C", so they
// already have external linkage -- just need a visible declaration in callers).
extern "C" void Hook_FlushPendingCapture();               // input cluster
extern "C" void Hook_BattleDiag_TickIfActive();           // input cluster
uint16_t Hook_ApplySOCD(uint16_t input);                  // input cluster (was static inline)
uint16_t Hook_ApplySOCDForSpecConsume(uint16_t input);    // input cluster: spectator/replay pop path
void CheckGameModeTransition();                            // game_mode cluster (called by Hook_UpdateGameState)
#include "facing_trace.h"                                  // Phase 4b [FACING] ring (dark); declared here so hooks_getinput.cpp needs no new include line (1000-line rule)

// Cross-cluster file-scope state (definitions in the noted TU; were static).
extern uint16_t g_capture_p[2];                            // input TU
extern uint32_t g_capture_recorded_idx;                    // input TU
extern int      g_battle_diag_frames_remaining;            // input TU (armed by CheckGameModeTransition)
extern uint32_t g_battle_diag_frame_idx;                   // input TU
extern bool     g_battle_entry_signaled_pub;               // game_mode TU
#define g_battle_entry_signaled g_battle_entry_signaled_pub
