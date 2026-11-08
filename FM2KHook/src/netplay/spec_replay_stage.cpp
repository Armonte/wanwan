// spec_replay_stage.cpp -- the stage a replay file loads for each match.
//
// A match's MATCH_START (its chars + stage header) is recorded when its battle
// starts, AFTER that match's character select. Playing a whole .fm2kset, match
// 2+'s MATCH_START therefore drains only once its stage has already loaded,
// and the viewer kept the previous match's stage for the rest of the session
// (player report 2026-11-08; match 1 was right because the file seeks straight
// to MATCH_START 1). LoadStageFile asks here instead: inside a match (its
// MATCH_START applied, MATCH_END not yet) the applied header's stage; between
// matches, the stage of the next MATCH_START still waiting in pb_queue.
#include "spectator_node.h"
#include "spectator_node_internal.h"

#include <cstdlib>

extern "C" uint32_t SpectatorNode_ReplayStageForLoad() {
    static int s_offline = -1;   // replay files only; live viewers take HOST_CONFIG's stage
    if (s_offline < 0) {
        const char* v = std::getenv("FM2K_REPLAY_FILE");
        s_offline = (v && v[0]) ? 1 : 0;
    }
    if (s_offline != 1) return 0xFFFFFFFFu;
    if (g_state.pb_awaiting_match_end) return g_state.pb_stage_id;
    for (const SessionEvent& ev : g_state.pb_queue) {
        if (ev.type != SessionEventType::MATCH_START) continue;
        if (ev.u.match_start_idx < g_state.pb_match_headers.size())
            return g_state.pb_match_headers[ev.u.match_start_idx].data()[80];
        break;
    }
    return 0xFFFFFFFFu;
}
