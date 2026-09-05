// host_replays.h -- find replay files, read who is in them, and group the
// slices of one connection back into the SET they were played as.
//
// A set is the unit people actually watch. The recorder writes one
// .fm2kset per connection plus one .fm2krep per match inside it, sharing a
// session_id, and a flat list of files throws away the only thing that
// makes a first-to-five legible.
//
// Its own TU rather than more of host_scan.cpp: game discovery answers
// "what is installed", this answers "what did you play".
#pragma once

#include <string>
#include <vector>

// Walk each games root for <game>/replays and <game>/sessions, and return
// one TSV line per row, sets newest first with their matches following in
// PLAY order:
//   "kind\twhen\tgame\tp1\tp2\tscore\tsize\tpath\tset_path\tmatch_index
//    \tp1_face\tp2_face\twinner"
// where the faces are image paths from the character sweep and winner is
// 1, 2, or 0 when the recorder captured no outcome.
// kind is SET or MATCH.
std::string ScanReplays(const std::vector<std::string>& roots);

// Fetch the hub's community list for one game and render it in the same
// row shape. Blocking; call it off the frame loop.
// `reached` reports whether the hub actually answered. An empty result with
// reached=true means nobody has uploaded for this game; with reached=false it
// means we never got to ask. The UI cannot print one sentence for both, and
// printing neither is how the tab sat on "loading..." forever.
std::string FetchCommunityReplays(const std::string& hub_host,
                                  const std::string& game_id,
                                  bool* reached);

// Download one community recording into a local cache and return its
// path, or empty on failure. `ref` is the "hub:<id>|<file>" form the
// community rows carry.
std::string FetchCommunityFile(const std::string& hub_host,
                               const std::string& ref);
