#pragma once

#include <cstdint>

// C3.5 -- vs_round_function detour for ROUND_START / ROUND_END emit.
//
// FM2K-only emit; on FM95 builds these are no-ops (FM95 has different
// round-state plumbing -- separate hand-off).
//
// Lifecycle:
//   - RoundEvents_Install: called from Hooks::Hook_Initialize after the
//     other MinHook detours are wired up. Adds the trampoline for
//     vs_round_function @ 0x004086A0 (FM2K) so each call edge-detects the
//     substate field and emits the appropriate session event.
//   - RoundEvents_OnMatchStart: called from Netplay_StartBattle so the
//     1-based round-counter resets at every match boundary.

bool RoundEvents_Install();
void RoundEvents_OnMatchStart();

// Enable/disable KOF-style HP/meter retention in team mode. When ON and
// g_game_mode_flag == 2 (team mode), the round-end edge snapshots the
// winner's slot HP and super_meter. The Option-A code-cave patch on
// character_state_machine's HP-init instruction (installed by
// PerGamePatches_InstallKofHpInitPatch) intercepts the next round's
// HP write and substitutes the snapshotted values for the winner's
// slot -- the loser's incoming character initializes normally.
// Read from FM2K_TEAM_KOF_RETENTION env var at hook init; toggled
// per-game via the launcher's host config panel.
void RoundEvents_SetKofRetention(bool enabled);

// Snapshot accessors used by the Option-A interceptor in
// per_game_patches.cpp. The snapshot is set in Hook_vs_round_function
// at the round-end substate edge (any → 900) and consumed by the
// HpInitInterceptor when character_state_machine's CSMK_PLAYER init
// hits the patched instruction at 0x411CB1.
bool     RoundEvents_KofRetentionEnabled();
bool     RoundEvents_KofSnapshotPending();
int      RoundEvents_KofSnapshotWinnerIdx();   // 0=P1, 1=P2 (only valid if pending)
uint32_t RoundEvents_KofSnapshotWinnerHp();
uint32_t RoundEvents_KofSnapshotWinnerMeter();
void     RoundEvents_KofSnapshotMarkApplied(); // called by interceptor after write
