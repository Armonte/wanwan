// xref.cpp -- sound-usage cross-reference walk (see xref.h for the model).
//
// Port of the validated Python prototype over tools/kgt/blocks.py
// (Bewear.player results in docs/dev/2dfm_studio_design.md; the fixture
// gate is studio/tests/xref/). Payload decodes, little-endian:
//   I  (12): wait = u16 @ p[0]; sprite = p[2] | ((p[3] & 0x1F) << 8)
//   S  (3):  command = p[0];    sound  = u16 @ p[1]
//   SF/SG/SC (9/10/11): timeline jump -- every tick recorded after the
//   first one in a script is a linear ESTIMATE (tick_estimated).
//
// Defensive by design: script windows are clamped to items.size(),
// descending/overlapping script_index yields an empty (or shared)
// window instead of UB, and item 0 of each non-empty window is skipped
// as the settings block regardless of its opcode byte.

#include "xref.h"

namespace kgt {

namespace {

inline int U16(const std::array<uint8_t, 15>& p, size_t i) {
    return p[i] | (p[i + 1] << 8);
}

// Clamp a (possibly negative / oversized) index into [lo, hi].
inline size_t ClampIndex(int64_t v, size_t lo, size_t hi) {
    if (v < static_cast<int64_t>(lo)) return lo;
    if (v > static_cast<int64_t>(hi)) return hi;
    return static_cast<size_t>(v);
}

}  // namespace

SoundXref BuildSoundXref(const KgtFile& f) {
    SoundXref x;
    const size_t n_items = f.items.size();
    const int n_scripts = static_cast<int>(f.scripts.size());

    for (int si = 0; si < n_scripts; ++si) {
        // Timeline window: [script_index, next script's script_index).
        // The last script runs to the end of items[]. Malformed files
        // can have out-of-range or descending indices -- clamp so the
        // window is always a valid (possibly empty) range.
        const int64_t raw_begin = f.scripts[si].script_index;
        const int64_t raw_end = (si + 1 < n_scripts)
            ? static_cast<int64_t>(f.scripts[si + 1].script_index)
            : static_cast<int64_t>(n_items);
        const size_t begin = ClampIndex(raw_begin, 0, n_items);
        const size_t end = ClampIndex(raw_end, begin, n_items);
        if (begin >= end) continue;

        int tick = 0;
        int sprite = -1;
        bool jumped = false;

        // begin+1: item 0 of every script is the settings block,
        // regardless of what its opcode byte says.
        for (size_t it = begin + 1; it < end; ++it) {
            const ScriptItem& item = f.items[it];
            const std::array<uint8_t, 15>& p = item.payload;
            switch (item.script_type) {
            case 12: {  // I: image draw -- advances the timeline
                tick += U16(p, 0);
                sprite = p[2] | ((p[3] & 0x1F) << 8);
                break;
            }
            case 3: {  // S: sound -- record a use at the current position
                SoundUse u;
                u.script = si;
                u.item = static_cast<int>(it);
                u.tick = tick;
                u.sprite = sprite;
                u.command = p[0];
                u.tick_estimated = jumped;
                x.uses[U16(p, 1)].push_back(u);
                break;
            }
            case 9:    // SF: fork
            case 10:   // SG: goto
            case 11: { // SC: call -- linear tick math past here is an estimate
                if (!jumped) {
                    jumped = true;
                    x.scripts_with_jumps.push_back(si);
                }
                break;
            }
            default:
                break;
            }
        }
    }
    return x;
}

std::vector<int> SoundXref::UnusedSounds(const KgtFile& f) const {
    std::vector<int> unused;
    for (size_t i = 0; i < f.sounds.size(); ++i) {
        if (f.sounds[i].data.empty()) continue;  // empty slot, not "unused"
        auto it = uses.find(static_cast<int>(i));
        if (it == uses.end() || it->second.empty())
            unused.push_back(static_cast<int>(i));
    }
    return unused;
}

}  // namespace kgt
