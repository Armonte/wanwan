// Replay — wire-payload schema for the MATCH_START SessionEvent.
//
// HISTORICAL: in v0.2.x this was a separate replay-file format with its own
// recording + playback API. Retired in v0.2.27 — the v2 .fm2krep file
// format owned by spectator_node.cpp (`FM2KSessionFileHeader` + packed
// `SessionEvent[]` body) is a strict superset and now the canonical
// per-battle replay representation.
//
// What survives: the 96-byte ReplayHeader struct + REPLAY_MAGIC. They're
// the on-the-wire layout for MATCH_START's payload (a SessionEvent that
// inlines the original-match metadata). Spectator-side decoders pull
// fields by offset out of the 96-byte payload via this struct, so the
// layout has to stay byte-stable. ReplayFrame is gone (was only used by
// the legacy file body).
#pragma once

#include <cstdint>
#include <cstddef>

namespace Replay {

constexpr uint32_t REPLAY_MAGIC   = 0x52504D46;  // 'FM2KREP' tag (FMPR little-endian)
constexpr uint16_t REPLAY_VERSION = 1;

#pragma pack(push, 1)
// 96-byte header — laid out exactly as the MATCH_START SessionEvent
// payload. Fields beyond what the wire/file format consumes are kept
// reserved/zero so the schema can grow without breaking older readers.
struct ReplayHeader {
    uint32_t magic;              // REPLAY_MAGIC
    uint16_t version;            // REPLAY_VERSION
    uint16_t flags;              // Reserved
    uint64_t unix_timestamp;     // When the match started
    uint32_t game_hash;          // FM2K game identifier (for cross-variant replays)
    uint32_t initial_rng_seed;   // RNG seed at battle start — required for determinism
    uint32_t initial_state_hash; // Fingerprint at battle start — sanity check on playback
    uint8_t  p1_char_slot;       // Character selections
    uint8_t  p1_color;
    uint8_t  p2_char_slot;
    uint8_t  p2_color;
    uint8_t  p1_name[24];        // Display names (null-padded)
    uint8_t  p2_name[24];
    uint8_t  stage_id;           // Reserved
    // #66/replay: the match's round gameconfig, so playback runs the SAME round
    // length as the match. round_time_sec = g_round_time @0x430114 -- the game's
    // round timer setting, the SAME field netplay syncs host->guest in
    // host_config (netplay_control.cpp); playback re-runs CSS init from the
    // REPLAYER's game.ini otherwise and the round timer differs (user report).
    // round_count = g_default_round @0x430124. Both zero on legacy files ->
    // restore is skipped (0xFFFFFFFF also skipped = "no override" sentinel).
    uint32_t round_time_sec;     // g_round_time    @0x430114 (0 = legacy/unknown)
    uint32_t round_count;        // g_default_round @0x430124 (0 = legacy/unknown)
    uint8_t  reserved[3];        // Pad to 96 bytes
    uint32_t frame_count;        // Reserved (was finalized by legacy writer; v2 .fm2krep
                                 //   writer doesn't populate — use FM2KSessionFileHeader's
                                 //   event_count / input_count instead).
};
static_assert(sizeof(ReplayHeader) == 96, "ReplayHeader must be 96 bytes");
#pragma pack(pop)

} // namespace Replay
