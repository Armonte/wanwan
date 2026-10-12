#pragma once

#include <cstdint>

#include "gameplay_tag_core.h"

// Hook-only glue for the replay gameplay tag (v2). See gameplay_tag_core.h.
namespace fm2k::gameplay_tag {

// Add the team-battle rosters to `parts` when the current game mode is VS Team
// (g_game_mode_flag == 2). A 1v1 match is fully described by MATCH_START's
// p1/p2 char ids; a team match loads each round's member from these tables.
// No-op on FM95 (v2 is FM2K-only).
void AddLiveTeamRoster(Participants* parts);

// Fill the per-file digest cache for the match about to start, so the replay
// writer at battle end only stats files. Called once at MATCH_START, never per
// frame. The engine has just loaded these very files, so the reads come from
// the OS page cache; sprite pixels and sounds are seeked past, not read.
void PrewarmMatch(uint8_t p1_char, uint8_t p2_char, uint8_t stage_id);

}  // namespace fm2k::gameplay_tag
