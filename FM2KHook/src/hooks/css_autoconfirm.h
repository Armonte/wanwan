// CSS auto-lock-and-confirm hook.
//
// For offline replay playback (FM2K_REPLAY_FILE set): the .fm2krep file
// contains battle INPUTs only (slice from MATCH_START → MATCH_END), no
// CSS-traversal inputs. Without this hook, those battle inputs leak into
// the local game's CSS state machine as cursor moves + confirm bits → game
// auto-walks CSS but lands on the wrong characters → desynced replay.
//
// This hook intercepts game_state_manager @ 0x406FC0 (FM2K) and, while
// game_mode == 2000 (CSS), pre-injects state writes that pin cursor
// positions to the chars recorded in MATCH_START's header and force a
// confirm bit each frame until both action_states lock. The natural state-1
// logic does the actual work -- file loads, portrait creation, player color
// assignment -- so the local game ends up with the right chars selected
// without any duplicated setup logic on our side.
//
// See docs/analysis/css_state_machine.md for the IDA pass that maps the
// state machine + critical addresses.

#pragma once

#include <cstdint>

// Install MinHook detour on game_state_manager. Called from the main hook-
// init path. Returns false on hook-install failure (logged); other code
// continues without auto-confirm in that case.
bool CssAutoConfirm_Install();

// Activate auto-confirm for the next CSS phase. Called from the spectator
// node's MATCH_START apply path when an offline-replay session is loading
// per-battle data. p1_char / p2_char index into the FM2K char grid;
// p1_color / p2_color are the recorded color slots (0..5) that map to the
// attack-button bit our auto-confirm injects so AssignPlayerColor lands on
// the same color the original player picked; stage_id is the recorded
// stage slot. Live-spectator paths do NOT call this -- they drive CSS via
// the full host input stream and don't need the override.
void CssAutoConfirm_OnReplayMatchStart(uint8_t p1_char, uint8_t p1_color,
                                       uint8_t p2_char, uint8_t p2_color,
                                       uint8_t stage_id);

// Reset on MATCH_END / battle-mode entry -- re-engaged by the next
// CssAutoConfirm_OnReplayMatchStart for back-to-back replay matches.
void CssAutoConfirm_Disengage();

// Enable/disable team-mode duplicate-slot lock. When enabled and team
// mode (g_game_mode_flag == 2) is active, the hook masks each player's
// confirm bits if their cursor would re-pick a character already locked
// into one of their earlier team slots. Independent of the replay
// auto-confirm path. Read from the FM2K_TEAM_CSS_DUPE_LOCK env var at
// hook init; toggled per-game via the launcher's host config panel.
void CssAutoConfirm_SetTeamDupeLock(bool enabled);

// Spectator match-boundary seam hold (Phase F). While enabled and the
// auto-confirm pin is NOT armed, the hook keeps the local CSS from
// advancing: both action_states forced to 0 and confirm bits masked each
// frame. FM2K's VS-rematch CSS auto-advances even on neutral inputs (the
// previous match's locks persist), which let the spectator race into the
// next battle BEFORE the host's MATCH_START arrived -- early battle entry
// consumed the once-per-battle init edge before the deferred PIN_RNG /
// RESET_INPUT_STATE were even queued (battle-2 desync), and the screen ran
// ahead of the source clients. Set on SEAM entry; the pin arming
// (OnReplayMatchStart) takes precedence via the !armed gate; cleared at
// boundary teardown.
// p1_color/p2_color carry the just-finished match's color slots so the
// held portraits keep the palettes the viewer just watched (the engine
// only assigns colors at confirm, which the hold suppresses).
void CssAutoConfirm_SetSeamHold(bool enabled,
                                uint8_t p1_color = 0, uint8_t p2_color = 0);
