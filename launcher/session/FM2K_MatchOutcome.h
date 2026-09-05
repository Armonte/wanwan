// FM2K_MatchOutcome.h -- what the hook says about the match, read from
// the game's shared memory. Launcher-free, so the Slint launcher reports
// results the same way the ImGui launcher does (PollMatchOutcome in
// launcher_ui_hub_match.cpp still maps the block inline; it should move
// onto this reader -- one mapping, one set of field rules).
//
// The hook (FM2KHook/src/ui/shared_mem.h) bumps `match_outcome_seq` once
// per resolved battle and `match_chars_seq` once per battle START. The
// launcher polls, notices a bump, and sends match_progress / match_result
// to the hub. Sequence tracking is the caller's: seqs are monotonic per
// game PROCESS, so the last seen value must be kept per pid and must NOT
// be reset on a hub match rotation.
#pragma once
#include "FM2K_HubClient.h"   // fm2k::HubClient::RoundJson

#include <cstdint>
#include <string>
#include <vector>

namespace fm2k::match_outcome {

struct Snapshot {
    uint32_t outcome_seq = 0;
    uint8_t  outcome = 0;               // FM2KMatchOutcome (shared_mem.h)
    uint32_t p1_char_id = 0xFFFFFFFFu;
    uint32_t p2_char_id = 0xFFFFFFFFu;
    uint32_t stage_id = 0xFFFFFFFFu;
    std::string p1_char_name;           // UTF-8, the hook's resolved .player names
    std::string p2_char_name;
    uint32_t chars_seq = 0;
    uint64_t session_id = 0;            // schema-2: 0 when the hook predates it
    uint8_t  match_index = 0;
    std::vector<fm2k::HubClient::RoundJson> rounds;
};

// Map FM2K_SharedMem_<pid> read-only and copy the match fields out.
// False when the game has no mapping yet, or its magic/version disagree.
bool Read(uint32_t pid, Snapshot* out);

// The hub's word for a recordable outcome: "self_won", "peer_won",
// "draw", "disconnect"; nullptr for NONE and for every no-record kind.
const char* Word(uint8_t outcome);

// True for the kinds that end the session WITHOUT a record: CSS abort,
// desync, hash mismatch, no VS mode. `Reason` says which, for the user.
bool IsAbort(uint8_t outcome);
const char* Reason(uint8_t outcome);

}  // namespace fm2k::match_outcome
