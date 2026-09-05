// FM2K_MatchOutcome.cpp -- see the header.
#include "FM2K_MatchOutcome.h"

#include "FM2KHook/src/ui/shared_mem.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace fm2k::match_outcome {

bool Read(uint32_t pid, Snapshot* out) {
    if (pid == 0 || !out) return false;
    const std::string name = "FM2K_SharedMem_" + std::to_string(pid);
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
    if (!h) return false;
    auto* data = static_cast<FM2KSharedMemData*>(
        MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(FM2KSharedMemData)));
    if (!data) {
        CloseHandle(h);
        return false;
    }
    bool ok = false;
    if (data->magic == FM2K_SHARED_MEM_MAGIC && data->version == FM2K_SHARED_MEM_VERSION) {
        out->outcome_seq = data->match_outcome_seq;
        out->outcome     = data->match_outcome;
        out->p1_char_id  = data->match_p1_char_id;
        out->p2_char_id  = data->match_p2_char_id;
        out->stage_id    = data->match_stage_id;
        // Bound the strnlen at the buffer so a publish without a NUL
        // cannot walk off the mapping.
        out->p1_char_name.assign(data->match_p1_char_name,
                                 strnlen(data->match_p1_char_name, FM2K_MATCH_CHAR_NAME_MAX));
        out->p2_char_name.assign(data->match_p2_char_name,
                                 strnlen(data->match_p2_char_name, FM2K_MATCH_CHAR_NAME_MAX));
        out->chars_seq   = data->match_chars_seq;
        out->session_id  = data->match_session_id;
        out->match_index = data->match_index_in_session;
        out->rounds.clear();
        const uint8_t rc = std::min<uint8_t>(data->match_rounds_count, 8);
        for (uint8_t i = 0; i < rc; ++i) {
            const FM2KRoundResult& r = data->match_rounds[i];
            out->rounds.push_back(fm2k::HubClient::RoundJson{
                r.winner_idx, r.p1_hp_remaining, r.p2_hp_remaining, r.frames_elapsed});
        }
        ok = true;
    }
    UnmapViewOfFile(data);
    CloseHandle(h);
    return ok;
}

const char* Word(uint8_t outcome) {
    switch (outcome) {
        case FM2K_MATCH_OUTCOME_SELF_WON:   return "self_won";
        case FM2K_MATCH_OUTCOME_PEER_WON:   return "peer_won";
        case FM2K_MATCH_OUTCOME_DRAW:       return "draw";
        case FM2K_MATCH_OUTCOME_DISCONNECT: return "disconnect";
        default: return nullptr;
    }
}

bool IsAbort(uint8_t outcome) {
    return outcome == FM2K_MATCH_OUTCOME_CSS_ABORT ||
           outcome == FM2K_MATCH_OUTCOME_DESYNC ||
           outcome == FM2K_MATCH_OUTCOME_HASH_MISMATCH ||
           outcome == FM2K_MATCH_OUTCOME_NO_VS_MODE;
}

const char* Reason(uint8_t outcome) {
    switch (outcome) {
        case FM2K_MATCH_OUTCOME_CSS_ABORT:     return "peer left during character select -- match not recorded";
        case FM2K_MATCH_OUTCOME_DESYNC:        return "desync detected -- match stopped, not recorded";
        case FM2K_MATCH_OUTCOME_HASH_MISMATCH: return "game files do not match the peer's -- match not recorded";
        case FM2K_MATCH_OUTCOME_NO_VS_MODE:    return "this game has no VS mode -- netplay not supported";
        default: return "";
    }
}

}  // namespace fm2k::match_outcome
