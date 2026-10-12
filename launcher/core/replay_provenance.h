#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

// The launcher's single replay-provenance verdict, shared by the replay
// browser (badge + Watch button) and the on_replay_play gate, so the two can
// never disagree about whether a file is [different game files].
//
//   v2 gameplay tag present  -> compare v2 ONLY (gameplay regions of the files
//                               the match loaded; cosmetic mods never differ)
//   v1 install tag only      -> old replay: keep the v1 check (directory
//                               metadata of every .player/.kgt), which can
//                               false-positive on cosmetic files -- the user
//                               override ("Watch anyway") always works
//   0 on either side         -> unknown, allow
namespace fm2k::replay_provenance {

struct Verdict {
    uint32_t v1_file  = 0, v1_local = 0;   // content_tag
    uint32_t v2_file  = 0, v2_local = 0;   // gameplay_tag
    bool     v2_present = false;
    bool     Mismatch() const;
};

// Memo for one scan: the same game folder / participant set recurs across
// dozens of files. Per-file digests are additionally cached process-wide.
struct ScanCache {
    std::unordered_map<std::wstring, uint32_t> v1_by_dir;
    std::unordered_map<std::wstring, uint32_t> v2_by_key;
    std::unordered_map<std::wstring, std::wstring> exe_by_dir;
};

// hdr256: the first 256 bytes of a v2 (FMSS version 2) replay file.
// game_dir: the folder the replay belongs to (two levels above it).
Verdict Evaluate(const uint8_t* hdr256, const std::filesystem::path& game_dir,
                 ScanCache* cache = nullptr);

}  // namespace fm2k::replay_provenance
