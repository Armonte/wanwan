#pragma once

// Per-game runtime patches not better-housed in a feature-specific hook
// module. Currently:
//   - Damage multiplier (FM2K_DAMAGE_MULT_PCT, default 100) -- MinHook
//     detour on health_damage_manager @ 0x40e7c0 that scales the damage
//     argument by mult/100 before forwarding to the original.
//   - Team size override (FM2K_TEAM_SIZE) -- direct write to g_team_round
//     (0x430128) at hook init.
//   - Stubs for VS CPU, CPU vs CPU, training, and OPTION-button mode
//     selector -- toggles are exposed but the hook side just logs.
//
// All driven by env vars set by the launcher's ApplyGamePatchEnvVars
// before CreateProcess. See FM2K_LauncherUI.cpp.

#include <cstdint>

// Install the damage-multiplier MinHook. Call AFTER MH_Initialize() so the
// detour can register. Idempotent -- safe to skip if FM2K_DAMAGE_MULT_PCT
// is unset (no hook installed in that case to minimize overhead).
bool PerGamePatches_InstallDamageMultiplierHook();

// Install the story-init AI hijack MidHook on character_state_machine
// at 0x411C8F. The MidHook fires AFTER `mov eax, g_game_mode_flag`
// and overrides ctx.eax to 0 when a hijack mode (vs_cpu / training /
// cpu_vs_cpu) is active and we're in battle phase. That forces the
// upcoming cmp+jz to dispatch character_state_machine's CSMK_PLAYER
// init through the story-init / 1P-arcade branch -- giving P2 the
// stage-script-driven AI fields (`something_xor_mask`, `unknown_dfbb
// [80] = -1`, hitstop, starting_special_stock) instead of the VS-mode
// default init which leaves P2 as a non-AI standing dummy.
//
// The global g_game_mode_flag stays at 1, so vs_round_function and
// other consumers continue to see VS round flow. Only this one read
// inside character_state_machine sees flag=0 (per call). Idempotent;
// only installs when at least one hijack mode (or the OPTION-cycle
// gate `option_mode_selector`) is enabled at boot. SafetyHook handles
// register/flag preservation and 5-byte instruction relocation
// (`cmp eax, ebp` + `mov [esi+4], ecx`).
bool PerGamePatches_InstallStoryInitHijack();

// Apply the three AI-driver fields (gate=1, difficulty=50, mode=1) directly
// to the relevant char_data slot(s) when a hijacked submode is active.
// Replicates the per-fighter loop at 0x408B72 inside vs_round_function's
// story-mode RSS_BATTLE_INIT branch -- the VS-mode branch (which our flag=1
// hijack runs in) skips the loop entirely, so the AI gate at 0xDF5D stays
// zero and ai_input_processor (0x411270) bails for every fighter.
//
// Apply slot selection by submode:
//   vs_cpu_mode      → P2 only       (P1 stays human)
//   training_mode    → P2 only when behavior == 1 (CPU). Other behaviors
//                      drive P2 via input-override.
//   cpu_vs_cpu_mode  → P1 + P2 (both AI)
//
// Call site: round_events.cpp's Hook_vs_round_function, immediately after
// the original returns, gated on pre==RSS_BATTLE_INIT (100) && post!=100.
// One-shot per round entry; the engine's natural per-round resets don't
// touch 0xDF5D/0xDF61/0xDF65 so the values persist round-to-round.
void PerGamePatches_OnBattleInitComplete();

// Install Option-A KOF retention: byte-level code-cave patch on
// character_state_machine's CSMK_PLAYER HP-init instruction at
// 0x411CB1 (`mov [ebx+0xDF05], eax`). When the patched instruction
// fires, our interceptor decides whether to write the engine's
// computed max_hp value (normal init) or the winner's snapshotted
// HP/meter from RoundEvents (KOF retention). Idempotent -- only
// installs when FM2K_TEAM_KOF_RETENTION is enabled.
bool PerGamePatches_InstallKofHpInitPatch();

// Apply the runtime flags / byte writes that don't need a MinHook
// detour. Reads env vars and forwards them to the appropriate sink.
// Called from dllmain.cpp's ApplyPerGameRuntimePatches.
void PerGamePatches_ApplyRuntime();

// Install a MinHook trampoline on InitializeGameFromCommandLine
// (0x409a60 -- the slot-0 boot dispatcher). At entry, our detour writes
// "<exe_basename>.kgt" into g_iniFile_nameOverride (0x43012c) so the
// engine's /F debug-boot path can resolve the kgt name.
//
// Why a hook and not a DllMain direct-write: hit_judge_set_function
// (called by InitializeMainWindow during boot) reads the kgt's
// [File].Filename key and writes its result back into the same global.
// Standalone games ship without an editor INI, so the default fires
// and the default is empty -- clobbering any earlier write. The hook
// fires AFTER that stomp and right before the dispatcher reads it.
//
// No-op unless FM2K_BOOT_TO_BATTLE=1 is set. Idempotent -- safe to call
// when the env var isn't set (skips install, costs nothing). Call
// AFTER MH_Initialize() so the detour can register.
bool PerGamePatches_InstallBootToBattleHook();

// Set BTB char/stage overrides at runtime -- preferred over the
// FM2K_BTB_* env vars when populated. Spec hook calls this from
// SpectatorNode_HandleJoinAck when the host advertises real chars +
// stage; ApplyBootToBattleStateOverrides reads from here first before
// falling through to getenv(). Necessary because SetEnvironmentVariableA
// updates the Win32 env block but not the CRT's _environ cache, so
// getenv() called minutes after a runtime SetEnv returns the stale
// process-init value (placeholder char 0 → mirror Blaziken crash).
// Pass 0xFF for any field to leave it unset (env-var fallback wins).
void PerGamePatches_SetRuntimeBtbOverrides(uint8_t p1_char,
                                           uint8_t p2_char,
                                           uint8_t stage,
                                           uint8_t p1_color = 0xFF,
                                           uint8_t p2_color = 0xFF);

// Spectator natural-boot abort: the JOIN_ACK said the host is pre-battle
// (kind=CSS/NONE), so the /F boot is wrong -- the viewer should walk
// title->CSS naturally and replay the host's stream from frame 0
// (tournament flow). Releases the /F dispatch hold; the dispatcher then
// clears g_debug_mode so the engine takes the normal title path.
void PerGamePatches_AbortBtbNaturalBoot();

// Returns the user-set team size override (2..8) or 0 if unset / out
// of range. Called from css_autoconfirm.cpp's Hook_GameStateManager
// each frame to re-write g_team_round_setting AFTER the engine's
// natural copy from g_team_round. Direct hook-init writes don't stick
// because hit_judge_set_function runs later in boot and re-loads the
// INI default into g_team_round.
int PerGamePatches_GetTeamSizeOverride();

// Per-frame input override hook. Called from Hook_GetPlayerInput's offline
// branch (right before original_get_player_input would run). Returns the
// 16-bit input value to use, or -1 if no override applies (caller falls
// through to the original input). Honors VS CPU, CPU-vs-CPU, and training
// mode toggles set by the launcher's per-game INI.
//   player_id:  0 = P1, 1 = P2.
//   game_mode:  *(uint32_t*)0x470054. 1000=title, 2000=CSS, 3000-3999=battle.
int PerGamePatches_TryOverrideInput(int player_id, uint32_t game_mode);

// CSS takeover gating for solo-driver modes (VS CPU / CPU vs CPU /
// Training). Computes what P2's input should be on a CSS frame given
// P1's current 11-bit input:
//   * Before P1 confirms (g_p1_action_state == 0): returns 0 -- P2 cursor
//     stays put, doesn't follow P1 yet.
//   * After P1 confirms, while P1's attack-confirm press is still held:
//     returns 0 -- prevents the rising-edge of the same attack press
//     from instantly confirming P2.
//   * After P1 confirms AND has released attack: returns p1_input as-is
//     so P1 can move P2's cursor and confirm the second character.
// Caller is responsible for invoking only on CSS frames (game_mode==2000)
// when a solo-driver mode is active and the call is for player_id==1.
uint16_t PerGamePatches_GatedP2CssInput(uint16_t p1_input);

// Battle-phase input override for solo-driver modes. Computes the input
// value to return for `player_id` during battle (game_mode 3000-3999)
// when an alt mode is active. Returns -1 when no override applies
// (caller falls through to the original/binder input). Same precedence
// as PerGamePatches_TryOverrideInput's battle branch -- exposed so the
// binder-active path in Hook_GetPlayerInput can apply it too (binder
// returns before TryOverrideInput is reached).
//   player_id:  0 = P1, 1 = P2
//   p1_input:   P1's current 11-bit input (used by training Imitate mode)
int PerGamePatches_BattleInputOverride(int player_id, uint16_t p1_input);

// Cycle the training-mode P2 behavior selector to the next entry. Called
// from a hotkey handler (F2 by default) and the in-match overlay. The
// behavior cycles 0 → 1 → 2 → 3 → 4 → 0:
//   0 = player (no override; use real P2 input)
//   1 = CPU   (zero input; let character scripts drive AI branches)
//   2 = imitate (mirror P1's input frame-for-frame)
//   3 = guard (force back-direction)
//   4 = jump-up (force UP bit)
void PerGamePatches_CycleTrainingP2Behavior();
int  PerGamePatches_GetTrainingP2Behavior();
const char* PerGamePatches_TrainingP2BehaviorLabel(int behavior);

// OPTION-button mode selector -- title-screen sub-mode cycle. Cycle is
// CONTEXT-AWARE based on which title menu entry the cursor is currently
// on (read from g_menu_selection @ 0x424780):
//
//   Context 0 (1P/Story entry):  Default → 1P VS CPU → Training
//                                  (alt modes hijack to 2P CSS so P1
//                                   picks both chars)
//   Context 1 (VS entry):        Default → Team Versus → CPU vs CPU
//
// Cycle position (g_vs_submode 0..2) is independent of context -- moving
// the cursor between entries changes the badge label but keeps the cycle
// index. Confirming on a given entry applies that entry's slot-N action.
void PerGamePatches_OnOptionPressed();
int  PerGamePatches_GetVsSubmode();              // 0..2
int  PerGamePatches_GetVsMenuContext();          // 0=1P, 1=VS, derived from g_menu_selection
const char* PerGamePatches_VsSubmodeLabel(int submode, int menu_context);
bool PerGamePatches_IsOptionModeSelectorActive();
bool PerGamePatches_IsTrainingModeActive();
bool PerGamePatches_IsVsCpuModeActive();
bool PerGamePatches_IsCpuVsCpuModeActive();

// Per-frame tick -- call from a place that runs once per real frame
// (imgui_overlay's NewFrame is convenient). Polls F2 for training-P2
// behavior cycle, watches game_mode for CSS→title transitions to
// clear stale mode flags. The title→CSS apply lives in
// PerGamePatches_OnGameStateManagerEntry instead (so it fires BEFORE
// CSS STATE 0 runs -- render-frame ticks fire AFTER and that breaks
// the 1P-to-2P hijack since STATE 0 has already created the wrong
// number of cursor objects).
void PerGamePatches_OnFrameTick();

// Called from css_autoconfirm.cpp's Hook_GameStateManager at the TOP,
// before the original game_state_manager body runs. Tracks game_mode
// internally; on the title→CSS edge applies the OPTION-cycle submode
// (writing g_game_mode_flag + the right mode-driver flag) so STATE 0
// of game_state_manager sees the final flag value when it dispatches
// CSS init.
void PerGamePatches_OnGameStateManagerEntry();

// Notify per-game patches of an OPTION-bit press observed on the
// title screen (game_mode == 1000). Idempotent; tracks a rising-edge
// internally so holding the button doesn't run away the cycle.
void PerGamePatches_OnTitleInputTick(uint16_t raw_input, uint32_t game_mode);
