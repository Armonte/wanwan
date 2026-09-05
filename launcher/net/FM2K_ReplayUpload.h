#pragma once

// Upload a finished hub match's recordings so everyone can watch them.
//
// The hub has always recorded that a match happened; this sends the
// recording itself, which is what turns a per-machine replay folder into
// a browsable list for a whole game.
//
// HUB MATCHES ONLY, by the owner's call. Offline and direct-connect games
// never upload: those have no hub identity attached, and a public list
// built partly from matches nobody can attribute is worse than a smaller
// honest one.
//
// The recordings are written by the hook AFTER the match ends, so this
// cannot run the instant a match finishes -- see the settle delay in the
// implementation.

#include <string>

namespace fm2k::replay_upload {

struct Config {
    std::string game_dir;     // absolute; contains replays/ and sessions/
    std::string game_id;      // hub room id, the key the list is browsed by
    std::string upload_url;   // e.g. "https://hub.2dfm.org/replay/upload"
    std::string p1_nick;
    std::string p2_nick;
    bool        enabled = true;
};

// Note that a hub match just ended. Cheap: records the moment and the
// config, uploads nothing yet.
void OnMatchEnded(const Config& cfg);

// Service a pending upload. Call once per launcher tick; does nothing
// until the settle delay has passed and at most one upload per call.
// Returns true when it uploaded something.
bool Process();

}  // namespace fm2k::replay_upload
