// Slim .kgt parser -- extracts player/stage/demo name lists from a 2DFM
// .kgt file without loading any of the heavy payloads (scripts, pictures,
// palettes, sounds). Lets the launcher populate dropdowns pre-launch
// instead of having to boot the game once and ReadProcessMemory the
// in-memory buffers.
//
// Adapted from /mnt/c/dev/wanwan/2dfm/2dfmFileReader.cpp by 厉猛 (limen).
// Original parser allocates + decodes everything via axmol; this strips
// to just the seek-past + name reads.
//
// Encoding: 2DFM Chinese-system games store SJIS/GBK strings; we use
// CP932 here because our locale spoof + the JP-system game corpus we
// support both target Shift-JIS. For Chinese games the same bytes will
// decode to garbled text via CP932 -- fix later by attempting CP932 first
// and falling back to GBK (CP936) on validation failure.

#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fm2k {

struct KgtSummary {
    std::string                project_name;     // from KgtFileHeader.name
    std::vector<std::string>   player_names;     // size 50, may include empties
    std::vector<std::string>   stage_names;      // size 50  (the dropdown source)
    std::vector<std::string>   demo_names;       // size 100
    bool                       valid = false;    // true on successful parse

    // Lookup by slot index (the same id the game stores in memory). Returns
    // the parsed name if the slot is in-range and non-empty; otherwise an
    // empty string so callers can substitute a fallback (e.g. "???" or
    // "Char #N"). Negative ids and out-of-range ids both fall through.
    static const std::string& EmptyName() {
        static const std::string s_empty;
        return s_empty;
    }
    const std::string& PlayerName(int id) const {
        if (id < 0 || (size_t)id >= player_names.size()) return EmptyName();
        return player_names[(size_t)id];
    }
    const std::string& StageName(int id) const {
        if (id < 0 || (size_t)id >= stage_names.size()) return EmptyName();
        return stage_names[(size_t)id];
    }
    const std::string& DemoName(int id) const {
        if (id < 0 || (size_t)id >= demo_names.size()) return EmptyName();
        return demo_names[(size_t)id];
    }

    // Index lists of non-empty slots -- useful for random-stage range display
    // ("rolls one of 6 stages") and for dropdown population that needs to map
    // a visible row back to the underlying slot id.
    std::vector<int> NonEmptyPlayerIds() const {
        std::vector<int> out;
        for (size_t i = 0; i < player_names.size(); ++i) {
            if (!player_names[i].empty()) out.push_back((int)i);
        }
        return out;
    }
    std::vector<int> NonEmptyStageIds() const {
        std::vector<int> out;
        for (size_t i = 0; i < stage_names.size(); ++i) {
            if (!stage_names[i].empty()) out.push_back((int)i);
        }
        return out;
    }
};

// Parse a .kgt file and fill `out` with name lists. Returns true on
// success. Empty slots are preserved as empty strings so the array index
// of each name is preserved as-is -- the caller skips empties at display
// time, matching the in-game dropdown's CB_INSERTSTRING-with-empty-skip
// behaviour.
bool ParseKgtSummary(const std::filesystem::path& kgt_path, KgtSummary& out);

// Display helpers -- return "<name> (#id)" when the slot has a name, else
// "Char #id" / "Stage #id" so callers can drop the result straight into
// UI without branching. `kgt` may be nullptr (game not installed locally,
// summary failed to parse) -- falls back cleanly to the id-only form.
std::string FormatCharLabel (const KgtSummary* kgt, int id);
std::string FormatStageLabel(const KgtSummary* kgt, int id);

} // namespace fm2k
