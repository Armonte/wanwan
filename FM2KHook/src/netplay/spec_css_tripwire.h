// spec_css_tripwire.h -- battle-frame-0 PARKED-VM DETECTOR (spectator plane).
//
// What this is, and what it deliberately is NOT.
//
// It is the surviving half of the Wave-2 spectator character-select park
// (spec_css_park.{h,cpp}, DELETED 2026-08-17). The park suppressed the SYMPTOM
// of the between-match falling character-select object by writing
// script_init_state (+0x152) = 2 over the character-bound type-4 VMs for the
// ~101-frame post-confirm ramp, so character_state_machine returned at its
// 0x411C3F entry guard. The CAUSE was then found and fixed at its site: the
// CssAutoConfirm pin wrote selected[i] = -1 without the engine's paired
// Css_UnloadPlayerPreview (0x406520), orphaning the outgoing preview object
// over a char slot whose script blob player_data_file_loader then replaced.
// With the cause fixed the park has nothing left to suppress, so it is gone --
// not disabled, not env-gated: removed, together with FM2K_SPEC_CSS_PARK and
// FM2K_SPEC_CSS_PARK_WIDE.
//
// This detector stays because a detector is not a fallback. If any mechanism --
// ours, the engine's, or one nobody has found yet -- ever leaves a script VM
// parked when a battle starts, those fighters are frozen for the round, and
// that must announce itself in the log rather than be silently re-suppressed.
// It writes nothing and has no kill switch.
#pragma once

namespace specnode {

// ALWAYS ON. Called at the spectator's first battle frame; walks the object
// pool and logs LOUDLY if any type-4 object is sitting at
// script_init_state == 2 when the battle starts.
void SpecCssTripwire_OnBattleFrameZero();

}  // namespace specnode
