// spec_css_park.h -- SPECTATOR CHARACTER-SELECT PARK (Wave 2, Lane B's fix).
//
// Declarations only. The mechanism, the un-park proof and every safety
// argument live in spec_css_park.cpp's header comment.
//
// One-line statement of the bug this closes: on the spectator plane the
// engine's demote (ResetObjectsAndCalculateSpeed @ 0x406450) lands on the far
// side of the post-confirm ramp, so the fighters stay type 4 and RUN their
// entry scripts under gravity while the character-select screen is still up --
// the owner-reported "player objects fall down on CSS". Measured: spectator
// pool all type 4 where the host's is all type 1, object descending 535 -> 921
// px over the last ~29 frames of the window.
#pragma once

namespace specnode {

// Kill-switch (FM2K_SPEC_CSS_PARK, DEFAULT ON). Read once, logged once.
bool SpecCssPark_Enabled();

// Arm the window. Called from the two spectator-only edges that bracket a
// BETWEEN-MATCH character-select window (spec_playback.cpp): the MATCH_END
// seam entry and the MATCH_START pin arm. `why` is a short static string used
// only in the one engage log line.
void SpecCssPark_Engage(const char* why);

// Per-frame, from the spectator sim step BEFORE the engine update. Parks only
// while armed AND game_mode == 2000 AND the type-10 character-select driver is
// still alive in the pool (see the cpp for why that third term is the
// by-construction proof that a battle object can never be reached).
void SpecCssPark_Tick();

// Always-on tripwire. Called at the spectator's first battle frame; logs LOUDLY
// if any type-4 object is sitting at script_init_state == 2 (parked) when the
// battle starts, which is the catastrophic failure mode of this fix (frozen
// fighters). Also closes the window.
void SpecCssPark_OnBattleFrameZero();

// Session teardown / re-join: drop the window without a battle edge.
void SpecCssPark_Reset();

}  // namespace specnode
