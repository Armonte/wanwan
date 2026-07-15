// xref.h -- sound-usage cross-reference over a parsed KgtFile.
//
// Model (validated against tools/kgt/blocks.py + a Bewear.player
// prototype, see docs/dev/2dfm_studio_design.md): each Script is a
// TIMELINE window into items[]: [script_index, next script_index).
// Item 0 of a script is the "settings" block regardless of opcode.
// Walking the window in order:
//   opcode 12 "I"  (Image draw): payload u16@0 = wait ticks,
//                  sprite index = p[2] | ((p[3] & 0x1F) << 8).
//                  Accumulate tick += wait; current sprite = index.
//   opcode  3 "S"  (Sound): u16@1 = sound table index. Record a use at
//                  the current (tick, sprite). p[0] is always 0 on disk
//                  (editor never writes it; loop/type flags live in the
//                  sound TABLE entry's sound_type byte -- see
//                  docs/dev/2dfm_studio_re_notes.md). One tick = one
//                  engine frame = 10ms, so seconds = tick / 100.
//   opcodes 9 "SF" / 10 "SG" / 11 "SC" (fork/goto/call) can jump the
//                  timeline: linear tick math past one is an ESTIMATE.
//                  Mark the script has_jumps (UI shows '~tick N').
//
// The walk must be defensive: script_index may be out of range or
// windows may overlap in malformed files -- clamp, never crash.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "kgt_file.h"

namespace kgt {

struct SoundUse {
    int script = 0;       // index into f.scripts
    int item = 0;         // index into f.items (the S block)
    int tick = 0;         // accumulated I-block wait at that point
    int sprite = -1;      // sprite frame on screen (-1 = none seen yet)
    uint8_t command = 0;  // S-block p[0]; always 0 on disk (kept for
                          // forensics -- a nonzero value means a hand-
                          // hacked file)
    bool tick_estimated = false;  // a jump op preceded this use
};

struct SoundXref {
    // sound table index -> uses, in file order.
    std::map<int, std::vector<SoundUse>> uses;
    // scripts containing SF/SG/SC (their ticks are estimates).
    std::vector<int> scripts_with_jumps;

    // Sounds with data but zero uses (safe to replace/repurpose).
    std::vector<int> UnusedSounds(const KgtFile& f) const;
};

SoundXref BuildSoundXref(const KgtFile& f);

}  // namespace kgt
