// Spectator backfill: stream session history (chunked EVENT_BATCH) + ship the
// state snapshot to a joining subscriber. Extracted VERBATIM from spectator_node.cpp
// into namespace specnode. No behavior change.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
#include "spectator_tcp.h"        // TCP transport for INPUT_BATCH stream
#include "control_channel.h"
#include "netplay.h"
#include "replay.h"
#include "savestate.h"            // SaveState_Save / Peek for snapshot capture
#include "netplay_state.h"
#include "../audio/sound_rollback.h"  // Op apply: SOUND_INIT
#include "../hooks/css_autoconfirm.h" // Replay-mode CSS lock-and-confirm
#include "../hooks/per_game_patches.h" // PerGamePatches_SetRuntimeBtbOverrides
#include "../ui/shared_mem.h"         // C10: SharedMem_PublishMatchSession / RoundResult
#include "gekkonet.h"

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <array>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
using namespace specnode;
namespace specnode {

// Send the full session event log to a specific subscriber, chunked at
// ~BACKFILL_CHUNK_BYTES per datagram. Used on JOIN_ACK so a late joiner
// can fast-forward from session start to live. Each chunk is its own
// EVENT_BATCH datagram. Hdr.start_frame = session-relative INPUT-frame
// index of the first INPUT in the chunk; receiver's dedup gate uses this
// to detect gaps / redundant retransmits across reconnects.
//
// Bytes-based chunking (vs the old fixed-frame chunking) handles variable-
// length events: a chunk with mostly INPUT events fits ~200 events; a
// chunk with a 96-byte MATCH_START fits fewer. Sized to stay under typical
// UDP MTU (1200 B safe) once we keep TCP — over TCP this only matters for
// receive-buffer pacing and avoids an oversized syscall stalling the
// session_events traversal.
// 8 KB chunks — a 1000-INPUT backfill (5 B/INPUT) + non-INPUT ops fits
// in a single chunk. The previous 1 KB cap chunked into ~3 chunks; in
// practice spectators only ever received the FIRST chunk, leaving a
// 500+ frame gap that they couldn't bridge before live EVENT_BATCH
// broadcasts started flowing (host's pkmncc match — see 07:54 logs).
// Root cause of the multi-chunk delivery loss is still unclear (TCP
// is reliable; both sides on localhost) but a single-chunk send
// sidesteps it. 8 KB stays well under the uint16 hdr.flags cap (65 KB).
constexpr size_t BACKFILL_CHUNK_BYTES = 8192;

// Walk session_events from a given index/INPUT-frame anchor and stream
// chunked EVENT_BATCH datagrams to a single subscriber. The legacy
// "from frame 0" backfill is just (first_event_idx=0,
// start_input_frame=session_start_frame); the snapshot-join path
// (task #18 phase 3) calls with the snapshot's anchor — events emitted
// reflect "everything from the snapshot's frame onward," not the
// preceding history we already shipped via SNAPSHOT_*.
void SendSessionEventsTo(const sockaddr_in& to,
                         size_t   first_event_idx,
                         uint32_t start_input_frame) {
    // Clamp the backfill at the GLOBAL live-flush cursor, not the vector
    // tip. Live EVENT_BATCH broadcasts resume from last_flushed_event_idx
    // for every fenced subscriber; shipping [cursor..tip) here too sent
    // those events TWICE to a fresh joiner (backfill + next FlushBatch),
    // shifting its input stream at the handoff -- deep mid-battle join
    // diverged at exactly the backfill->live boundary (k=3474,
    // 2026-06-11). The clamped tail reaches the sub via the very next
    // regular FlushBatch instead.
    const size_t total_events =
        (g_state.last_flushed_event_idx < g_state.session_events.size())
            ? g_state.last_flushed_event_idx
            : g_state.session_events.size();
    if (first_event_idx >= total_events) return;

    char addr_buf[48] = {};
    FormatAddr(to, addr_buf, sizeof(addr_buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: streaming events [%zu..%zu) (%u INPUTs total in session) "
                "to %s, anchor INPUT-frame=%u",
                first_event_idx, total_events, g_state.total_input_count,
                addr_buf, start_input_frame);

    // Walk session_events, packing into chunks bounded by BACKFILL_CHUNK_BYTES.
    // For each chunk, hdr.start_frame is the INPUT-frame index of the first
    // INPUT *in the chunk*. If a chunk is non-INPUT-only (rare; trailing
    // ops at session tail), use the running input cursor.
    size_t   ev_idx          = first_event_idx;
    uint32_t cursor_inputs   = start_input_frame;

    while (ev_idx < total_events) {
        std::vector<uint8_t> payload;
        payload.reserve(BACKFILL_CHUNK_BYTES);

        const size_t chunk_first_idx   = ev_idx;
        uint32_t     chunk_first_input = cursor_inputs;
        bool         saw_input         = false;
        uint32_t     chunk_input_count = 0;

        while (ev_idx < total_events) {
            const SessionEvent& ev = g_state.session_events[ev_idx];
            // Pre-encode to know the size; if it'd overflow the chunk
            // budget, defer to the next chunk (unless the chunk is empty,
            // in which case ship even an oversize event alone).
            uint8_t one[SESSION_EVENT_MAX_WIRE_SIZE];
            size_t  one_w = 0;
            switch (ev.type) {
                case SessionEventType::INPUT:
                    one_w = SessionEvent_EncodeInput(one, sizeof(one),
                                                     ev.u.input.p1, ev.u.input.p2);
                    break;
                case SessionEventType::PIN_RNG:
                    one_w = SessionEvent_EncodePinRng(one, sizeof(one),
                                                      ev.u.pin_rng_seed);
                    break;
                case SessionEventType::RESET_INPUT_STATE:
                    one_w = SessionEvent_EncodeResetInputState(one, sizeof(one));
                    break;
                case SessionEventType::SOUND_INIT:
                    one_w = SessionEvent_EncodeSoundInit(one, sizeof(one));
                    break;
                case SessionEventType::MATCH_START:
                    if (ev.u.match_start_idx < g_state.match_headers.size()) {
                        one_w = SessionEvent_EncodeMatchStart(one, sizeof(one),
                            g_state.match_headers[ev.u.match_start_idx].data());
                    }
                    break;
                case SessionEventType::MATCH_END:
                    one_w = SessionEvent_EncodeMatchEnd(one, sizeof(one),
                                                        ev.u.match_end);
                    break;
                case SessionEventType::FINGERPRINT:
                    one_w = SessionEvent_EncodeFingerprint(one, sizeof(one),
                                                           ev.u.fingerprint_hash);
                    break;
                case SessionEventType::ROUND_START:
                    one_w = SessionEvent_EncodeRoundStart(one, sizeof(one),
                                                          ev.u.round_start);
                    break;
                case SessionEventType::ROUND_END:
                    one_w = SessionEvent_EncodeRoundEnd(one, sizeof(one),
                                                        ev.u.round_end);
                    break;
                case SessionEventType::SESSION_ID:
                    one_w = SessionEvent_EncodeSessionId(one, sizeof(one),
                                                         ev.u.session_id);
                    break;
                case SessionEventType::CSS_ENTERED:
                    one_w = SessionEvent_EncodeCssEntered(one, sizeof(one), ev);
                    break;
            }
            if (one_w == 0) { ++ev_idx; continue; }   // unknown / unencodable

            if (!payload.empty() && payload.size() + one_w > BACKFILL_CHUNK_BYTES) {
                break;  // emit current chunk; this event goes in the next
            }
            payload.insert(payload.end(), one, one + one_w);

            if (ev.type == SessionEventType::INPUT) {
                if (!saw_input) {
                    saw_input         = true;
                    chunk_first_input = cursor_inputs;
                }
                ++chunk_input_count;
                ++cursor_inputs;
            }
            ++ev_idx;
        }

        if (payload.empty()) break;

        std::vector<uint8_t> buf(sizeof(SpecDataHeader) + payload.size());
        SpecDataHeader hdr = {};
        hdr.magic       = SPEC_DATA_MAGIC;
        hdr.type        = SpecDataType::EVENT_BATCH;
        hdr.start_frame = saw_input ? chunk_first_input
                                    : cursor_inputs;     // tail-only chunk
        hdr.frame_count = static_cast<uint16_t>(std::min<uint32_t>(chunk_input_count, 0xFFFFu));
        // EVENT_BATCH: flags carries payload byte count for the receiver's
        // TCP framer (see PayloadLenForType in spectator_tcp.cpp). The
        // BACKFILL_CHUNK_BYTES=1024 cap keeps this well under the 16-bit
        // limit. (Was: bit 0 flagged deferred-ops chunk — moot now since
        // the framer needs the byte count regardless.)
        hdr.flags       = static_cast<uint16_t>(std::min<size_t>(payload.size(), 0xFFFFu));
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr), payload.data(), payload.size());

        OutboundSendTo(to, buf.data(), buf.size());

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode:   chunk sent: start_frame=%u count=%u "
            "payload=%zu bytes (events [%zu..%zu))",
            (unsigned)hdr.start_frame, (unsigned)hdr.frame_count,
            payload.size(), chunk_first_idx, ev_idx);
    }
}

// Legacy entry point: ships ALL session_events from frame 0. Used by
// FULL_SESSION-mode subscribers and as the back-compat fallback when
// CURRENT_MATCH was requested but no snapshot has been captured yet
// (e.g. JOIN_REQ landed mid-CSS before any battle started).
void SendSessionBackfillTo(const sockaddr_in& to) {
    SendSessionEventsTo(to, 0, g_state.session_start_frame);
}

// Phase 3 entry point: ships only events at-or-after the given INPUT-frame
// anchor. Paired with SendSnapshotTo for CURRENT_MATCH-mode subscribers —
// the snapshot covers state up to its anchor frame, then this fills in
// the events from there to live edge.
//
// Linear scan over session_events to find the first event whose
// session-relative INPUT-frame index >= anchor_frame. Non-INPUT events
// (PIN_RNG / RESET / SOUND_INIT / MATCH_START / MATCH_END / FINGERPRINT)
// don't have an explicit frame number; they belong to "the INPUT that
// follows them" so we keep them grouped. We therefore find the FIRST
// INPUT >= anchor and back up to include any preceding non-INPUT ops in
// the same group.
// Hoisted index computation: the first session_events index a backfill
// anchored at `anchor_input_frame` would ship (the first INPUT at-or-after
// the anchor, backed up over its preceding non-INPUT op group). Returns
// session_events.size() when nothing is at-or-after the anchor. Shared by
// SendSessionBackfillFromFrame and the OP_BASELINE computation in the bind
// path so both always agree on where the connection's delivery starts.
size_t BackfillFirstIdxForFrame(uint32_t anchor_input_frame) {
    const size_t total = g_state.session_events.size();
    uint32_t cursor = g_state.session_start_frame;
    size_t   first_idx = total;   // sentinel: nothing matched
    for (size_t i = 0; i < total; ++i) {
        const SessionEvent& ev = g_state.session_events[i];
        if (ev.type == SessionEventType::INPUT) {
            if (cursor >= anchor_input_frame) {
                first_idx = i;
                break;
            }
            ++cursor;
        }
    }
    if (first_idx >= total) return total;

    // Back up over any non-INPUT ops immediately preceding first_idx
    // so they belong to the SAME chunk as their following INPUT (the
    // spectator's drain semantic applies them just before that INPUT
    // pops). Without this, ops would land in the previous chunk that
    // we're skipping → spectator misses PIN_RNG/etc that should fire
    // at the snapshot's anchor frame.
    while (first_idx > 0 &&
           g_state.session_events[first_idx - 1].type != SessionEventType::INPUT) {
        --first_idx;
    }
    return first_idx;
}

void SendSessionBackfillFromFrame(const sockaddr_in& to,
                                  uint32_t anchor_input_frame) {
    const size_t total = g_state.session_events.size();
    if (total == 0) return;

    const size_t first_idx = BackfillFirstIdxForFrame(anchor_input_frame);
    if (first_idx >= total) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: no events at-or-after INPUT-frame=%u "
            "(total INPUTs=%u, snapshot is at the live edge)",
            anchor_input_frame, g_state.total_input_count);
        return;
    }

    SendSessionEventsTo(to, first_idx, anchor_input_frame);
}

// Phase 3: ship the cached SaveState blob to a single subscriber as a
// SNAPSHOT_BEGIN / SNAPSHOT_CHUNK*N / SNAPSHOT_END sequence. Spectator
// reassembles in HandleSpecData (phase 4) and calls SaveState_Load on
// END. After this, host must call SendSessionBackfillFromFrame with the
// snapshot's input_frame anchor so the spectator's pb_queue picks up at
// the same point the loaded state expects.
//
// Idempotent and side-effect-free on g_state. Caller is responsible for
// ordering this BEFORE BroadcastToAll re-engages for the new sub
// (TickHostMaintenance handles that via the backfill_done fence).
void SendSnapshotTo(const sockaddr_in& to) {
    const auto& cache = g_state.current_snapshot;
    if (!cache.valid || cache.blob.empty()) return;

    char addr_buf[48] = {};
    FormatAddr(to, addr_buf, sizeof(addr_buf));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: shipping snapshot to %s "
                "(match=%u, %zu bytes, anchor INPUT-frame=%u)",
                addr_buf, cache.match_index, cache.blob.size(), cache.input_frame);

    // ---- SNAPSHOT_BEGIN ----------------------------------------------
    {
        SnapshotMetadata meta = {};
        meta.version            = SPECTATOR_SNAPSHOT_VERSION;
        meta.total_bytes        = (uint32_t)cache.blob.size();
        meta.match_index        = cache.match_index;
        meta.flags              = 0;
        meta.compressed_bytes   = (uint32_t)cache.blob.size();
        // Phase E: tell the spec which mode this snapshot was captured
        // at. v0.2.41 spectators see this as `reserved1` and ignore it
        // (their apply gate is hard-coded to `game_mode >= 3000` which
        // matches battle-only captures). v0.2.42+ spectators check the
        // field and apply when their local engine reaches the matching
        // mode.
        meta.captured_game_mode = cache.captured_game_mode;

        std::vector<uint8_t> buf(sizeof(SpecDataHeader) + sizeof(meta));
        SpecDataHeader hdr = {};
        hdr.magic       = SPEC_DATA_MAGIC;
        hdr.type        = SpecDataType::SNAPSHOT_BEGIN;
        hdr.start_frame = cache.input_frame;       // anchor for spectator's next_expected_frame
        hdr.frame_count = 0;
        hdr.flags       = (uint16_t)sizeof(meta);  // 16 — payload byte count
        // Zero-RLE the blob; ship compressed when it actually wins.
        static std::vector<uint8_t> s_wire;   // reused scratch
        ZeroRleCompress(cache.blob, s_wire);
        const bool use_rle = s_wire.size() < cache.blob.size();
        const std::vector<uint8_t>& wire = use_rle ? s_wire : cache.blob;
        if (use_rle) {
            meta.flags            |= SNAPSHOT_FLAG_ZERO_RLE;
            meta.compressed_bytes  = (uint32_t)s_wire.size();
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: snapshot wire size %zu bytes (%s, raw %zu)",
            wire.size(), use_rle ? "zero-RLE" : "uncompressed",
            cache.blob.size());
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr), &meta, sizeof(meta));
        OutboundSendTo(to, buf.data(), buf.size());

    // ---- SNAPSHOT_CHUNK xN -------------------------------------------
    size_t emitted = 0;
    while (emitted < wire.size()) {
        const size_t remaining = wire.size() - emitted;
        const size_t chunk_n   = std::min(remaining, SPECTATOR_SNAPSHOT_CHUNK_BYTES);

        std::vector<uint8_t> cbuf(sizeof(SpecDataHeader) + chunk_n);
        SpecDataHeader chdr = {};
        chdr.magic       = SPEC_DATA_MAGIC;
        chdr.type        = SpecDataType::SNAPSHOT_CHUNK;
        chdr.start_frame = (uint32_t)emitted;       // byte offset (running)
        chdr.frame_count = 0;
        chdr.flags       = (uint16_t)chunk_n;
        std::memcpy(cbuf.data(), &chdr, sizeof(chdr));
        std::memcpy(cbuf.data() + sizeof(chdr),
                    wire.data() + emitted, chunk_n);
        OutboundSendTo(to, cbuf.data(), cbuf.size());

        emitted += chunk_n;
    }
    }

    // ---- SNAPSHOT_END -------------------------------------------------
    {
        std::vector<uint8_t> buf(sizeof(SpecDataHeader) + sizeof(uint32_t));
        SpecDataHeader hdr = {};
        hdr.magic       = SPEC_DATA_MAGIC;
        hdr.type        = SpecDataType::SNAPSHOT_END;
        hdr.start_frame = 0;
        hdr.frame_count = 0;
        hdr.flags       = (uint16_t)sizeof(uint32_t);  // 4
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(hdr), &cache.checksum, sizeof(uint32_t));
        OutboundSendTo(to, buf.data(), buf.size());
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: snapshot shipped (%zu bytes in %zu chunks, fletcher32=0x%08X)",
                cache.blob.size(),
                (cache.blob.size() + SPECTATOR_SNAPSHOT_CHUNK_BYTES - 1) /
                    SPECTATOR_SNAPSHOT_CHUNK_BYTES,
                cache.checksum);
}

}  // namespace specnode
