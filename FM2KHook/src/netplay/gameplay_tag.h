#pragma once

#include <cstdint>
#include <string>

#include "gameplay_tag_core.h"

// Win32 half of the gameplay tag (replay provenance v2). The region policy,
// the sprite-header decision and the header block layout live in
// gameplay_tag_core.h. Compiled into BOTH the hook (stamps the tag at replay
// write time) and the launcher (recomputes it for a replay's game folder), so
// the file-naming rules below exist exactly once.
//
// Naming rules, same on both sides:
//   exe      the caller names it (hook: its own module; launcher:
//            content_tag::ResolveGameExeName), never "any exe in the folder"
//   .kgt     <exe stem>.kgt; if that is absent, the folder's only .kgt;
//            otherwise unknown (tag 0)
//   .player  "<roster name>.player", the roster name read from the .kgt as the
//            raw CP932 bytes the engine passes to CreateFileA
//   .stage   "<stage name>.stage", likewise
//
// Cost: files are parsed by seeking past sprite pixels and sounds, and every
// per-file digest is cached by (path, size, mtime) for the life of the
// process, so a replay-browser rescan is a handful of stat calls. Never call
// this on a per-frame path; the hook calls it at MATCH_START (prewarm) and at
// replay write time.
namespace fm2k::gameplay_tag {

// The .kgt for a folder, per the rule above. Empty = unknown.
std::wstring ResolveKgtName(const wchar_t* dir_w, const wchar_t* game_exe_name_w);

// Full v2 tag for an arbitrary game folder. 0 = unknown (no exe / .kgt).
uint32_t ComputeForDir(const wchar_t* dir_w, const wchar_t* game_exe_name_w,
                       const Participants& parts);

// Same for the current process's own folder + executable (hook side).
uint32_t ComputeLocal(const Participants& parts);

}  // namespace fm2k::gameplay_tag
