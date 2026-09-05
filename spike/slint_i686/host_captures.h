// host_captures.h -- reviewing a game's captured screenshots.
//
// The capture pipeline (tools/capture_batch.py -> capture_transcode.py ->
// capture_pick.py) leaves every shot it took in captures/<game>/ and a
// heuristic pick in index.tsv. The heuristic is a guess; the owner wants
// to APPROVE or VETO each shot and choose the lead. That decision lives
// in captures/<game>/picks.txt, one line per shot:
//     lead <name>       the tile's image
//     ok   <name>       approved: shown in the hover slideshow
//     veto <name>       never shown, never auto-picked
// `name` is the capture's stem (front_05, css_initial, battle). Both this
// launcher and capture_pick.py read and write that file, so a decision
// made here survives a re-run of the pipeline.
#pragma once
#include <string>

namespace hostgui {

// Every capture for the game with this exe, as TSV rows:
//   name \t thumb_path \t state        (0 none, 1 ok, 2 veto, 3 lead)
// Empty when the game has no capture folder.
std::string CaptureRows(const std::string& exe_path);

// A review click: "cap.lead:<name>", "cap.ok:<name>", "cap.veto:<name>",
// "cap.clear:<name>". Rewrites picks.txt and the game's index.tsv line.
// Returns false for an unknown key or a game with no captures.
bool CaptureAction(const std::string& key, const std::string& exe_path);

// The thumb the tile should show now (Windows path), "" for none.
std::string CaptureThumbFor(const std::string& exe_path);

// The approved ("ok") shots for the tile's hover slideshow, as '|'-joined
// thumb paths in picks.txt order, at most four. "" when nothing is
// approved: the slideshow only ever shows what was reviewed.
std::string CaptureSlideshow(const std::string& exe_path);

}  // namespace hostgui
