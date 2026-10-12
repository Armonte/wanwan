// See replay_provenance.h.
#include "replay_provenance.h"

#include <cstdio>
#include <cstring>

#include "FM2KHook/src/netplay/content_tag.h"
#include "FM2KHook/src/netplay/gameplay_tag.h"

namespace fm2k::replay_provenance {

namespace {
constexpr size_t OFF_CONTENT_TAG = 200;   // FM2KSessionFileHeader.game_content_tag
}  // namespace

bool Verdict::Mismatch() const {
    if (v2_present) return v2_local != 0 && v2_file != v2_local;
    return v1_file != 0 && v1_local != 0 && v1_file != v1_local;
}

Verdict Evaluate(const uint8_t* hdr, const std::filesystem::path& game_dir,
                 ScanCache* cache) {
    Verdict v;
    std::memcpy(&v.v1_file, hdr + OFF_CONTENT_TAG, 4);
    fm2k::gameplay_tag::Participants parts;
    v.v2_present = fm2k::gameplay_tag::DecodeHeader(hdr, &v.v2_file, &parts);

    const std::wstring dir = game_dir.wstring();
    // Same exe-naming rule as v1 (and as the hook, which names its own
    // module): <kgt stem>.exe, must exist, else unknown.
    // Resolved once per folder per scan (it is a directory enumeration).
    std::wstring exe;
    if (cache && cache->exe_by_dir.count(dir)) {
        exe = cache->exe_by_dir[dir];
    } else {
        exe = fm2k::content_tag::ResolveGameExeName(dir.c_str());
        if (cache) cache->exe_by_dir[dir] = exe;
    }

    if (v.v2_present) {
        parts.Normalize();
        std::wstring key = dir;
        key += parts.whole_roster ? L"|all" : L"|ids";
        for (uint8_t c : parts.chars)  { key += L"|c"; key += std::to_wstring(c); }
        for (uint8_t s : parts.stages) { key += L"|s"; key += std::to_wstring(s); }
        if (cache && cache->v2_by_key.count(key)) {
            v.v2_local = cache->v2_by_key[key];
        } else {
            v.v2_local = exe.empty() ? 0
                : fm2k::gameplay_tag::ComputeForDir(dir.c_str(), exe.c_str(), parts);
            if (cache) cache->v2_by_key[key] = v.v2_local;
        }
        return v;   // v2 decides alone; v1 is not consulted
    }

    if (v.v1_file != 0) {
        if (cache && cache->v1_by_dir.count(dir)) {
            v.v1_local = cache->v1_by_dir[dir];
        } else {
            v.v1_local = fm2k::content_tag::ComputeForDir(dir.c_str(), exe.c_str());
            if (cache) cache->v1_by_dir[dir] = v.v1_local;
        }
    }
    return v;
}

}  // namespace fm2k::replay_provenance
