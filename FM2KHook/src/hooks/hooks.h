#ifndef HOOKS_H
#define HOOKS_H

#include "globals.h"

// Hook setup/teardown
bool InitializeHooks();
void ShutdownHooks();

// Hook implementations (called by MinHook)
int __cdecl Hook_GetPlayerInput(int player_id, int input_type);
// FM95 has SEPARATE per-player input fns -- single arg `(player_idx)`. We
// hook both so the binder + facing-flip layer applies on FM95 too. The
// player_idx the host calls with is 1 (P1) and 2 (P2), used as multiplier
// into g_p_facing_snap[25*idx].
int __cdecl Hook_GetPlayerInput_FM95_P1(int player_idx);
int __cdecl Hook_GetPlayerInput_FM95_P2(int player_idx);
int __cdecl Hook_UpdateGameState();
void __cdecl Hook_RenderGame();
BOOL __cdecl Hook_RunGameLoop();
uint32_t __cdecl Hook_GameRand();
int __cdecl Hook_ProcessGameInputs();

// Engine-aware phase detection -- FM2K reads g_game_mode magic numbers
// (2000=CSS, 3000+=Battle); FM95 walks the object pool for type==19/16
// with sub_state range. Defined in hooks.cpp; consumed by main_loop_
// trampoline.cpp's ClassifyPhase + the CSS/battle transition checks.
bool IsCSSMode(uint32_t mode);
bool IsBattleMode(uint32_t mode);

// Live FM95 phase ordinal (2=battle,1=CSS,0=other). Recorded per confirmed
// frame in the gekko advance handler for deterministic cross-peer battle-end.
int Fm95CurrentPhaseByte();

// Match-DECISION ordinal for the battle-END ring: 1=active round play,
// 2=match decided (win-counter >= cap, a monotonic deterministic sim scalar),
// 0=neither. The 1->2 edge fires once at the true match end, never at a round
// boundary. See hooks_game_mode.cpp / docs RE-2b.
int Fm95MatchPhaseByte();

// SOCD (Simultaneous Opposite Cardinal Direction) cleaner -- strips
// nonsense input combinations like L+R or U+D per the currently-active
// SOCD mode (env FM2K_SOCD_MODE, runtime override via Hook_SetSOCDMode).
// Exposed so netplay.cpp can pre-apply on the host side BEFORE storing
// into the spectator stream -- eliminates SOCD-mode-mismatch between
// host and a spectator/replay using a different mode as a source of
// sim divergence. The spec re-applies on top (idempotent for the
// resolved input).
uint16_t Hook_ApplySOCD_Public(uint16_t input);

// Compute the deterministic-pseudo-random battle-autoplay input value
// for player_id (0=P1, 1=P2). Seeded from g_input_buffer_index (saved
// in InputTracking on every rollback save) so forward sim_N and replay
// sim_N produce identical values. Returns 0 when:
//   - FM2K_PARITY_AUTOPLAY_BATTLE env var isn't set
//   - we're not in battle phase (game_mode != 3000..3999)
// Exposed so the stress-mode `gekko_add_local_input` site feeds the
// same per-player input that the engine's Hook_GetPlayerInput would
// otherwise dispatch to. Without this, gekko's input pipeline sees
// keyboard (typically 0 unless focused) → .fm2krep records 0/0 → the
// replay re-runs with 0/0 even though the original sim consumed
// autoplay values. The replay-vs-record divergence at frame 0 was
// this exact mismatch.
uint16_t Hook_ComputeAutoplayBattleInput(int player_id);

// CSS counterpart (FM2K_PARITY_AUTOPLAY + FM2K_AUTOPLAY_CSS_DWELL):
// wander/dwell/confirm synthetic input for one player, returns 0 when
// game_mode != 2000. Netplay_ProcessCSS feeds this into the CSS
// GekkoSession for the LOCAL player so both peers' sims consume the
// identical lockstep stream (split-brain fix, 2026-06-11).
uint16_t Hook_ComputeAutoplayCssInput(int player_id);

// FM95 autoplay title/CSS nav pulse (attack-button pulse off the input ring).
// Returns -1 when off / in battle, else the pulse (0 or 0x010). Used by the
// FM95 netplay CSS path instead of Hook_ComputeAutoplayCssInput (which is
// FM2K game_mode==2000-gated and returns 0 on FM95). See hooks_getinput.cpp.
int Fm95ComputeAutoplayNav();

// Reset the dwell anchor above. Called at CSS SYNCED (each new CSS
// GekkoSession) so the browse window restarts per CSS phase regardless
// of the buf_idx gap heuristic.
void Hook_AutoplayCssResetDwell();

// Flush the buffered FM2K_RNG_TRACE=1 log to disk. Called by the
// harness auto-terminate path before TerminateProcess to avoid losing
// trace bytes that are still in stdio's user-space buffer. Safe no-op
// when the trace isn't enabled.
void Hook_FlushRngTrace();

// task #53 (round_events.cpp): live round-object substate (900+ = the
// end/transition sequence that frees battle heap) + pool-wide clear of the
// per-object afterimage index so render can't chain into freed heap.
int  RoundEvents_LiveSubstate();
void Fm2k_ClearAfterimageIndices();

// Match-end script-VM seam (round_events.cpp), sibling of the above one field
// over. Neutralize: park every live type-4 (script VM) object at
// script_init_state = 2 so character_state_machine returns at its entry guard
// (0x411C3F) instead of fetching an opcode through the character slot's
// commands_ptr, which the battle->CSS teardown GlobalFree'd. Check: the
// [ENDSEAM-OOB] fencepost -- per live script object, compare cursor*16+16
// against GlobalSize(commands_ptr) and log loudly on violation (GlobalSize
// returns 0 on a freed handle, so one probe catches dangling AND shrunk).
void Fm2k_NeutralizeMatchEndScriptObjects();
void Fm2k_CheckEndSeamScriptCursors(const char* where);

#endif // HOOKS_H
