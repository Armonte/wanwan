// Spectator session-file format (.fm2kset / .fm2krep): write the session event
// log + load/seek it back for offline replay. Extracted VERBATIM from
// spectator_node.cpp. Public Write/Load API (decls in spectator_node.h) + the
// internal encode/resolve helpers; reaches specnode helpers via using.
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
#include <ctime>
using namespace specnode;

// =============================================================================
// SESSION FILE FORMAT (C7) -- .fm2kset / .fm2krep
// =============================================================================
//
// On-disk layout: [SessionFileHeader (32 B)] [packed SessionEvent[] bytes]
//
// Same wire encoding as EVENT_BATCH payload (SessionEvent_Encode*), so
// loaders can reuse SessionEvent_Decode without an intermediate format.
// Events are self-describing (1-byte tag + variant payload), so no
// separate side table is needed -- MATCH_START's 96-byte ReplayHeader is
// inline in the encoded byte stream.

namespace {

constexpr uint32_t SESSION_FILE_MAGIC   = 0x53534D46;  // 'FMSS' little-endian
constexpr uint16_t SESSION_FILE_VERSION = 2;           // 256 B header (C7)

#pragma pack(push, 1)
// C7 -- 256 B enriched header.
//
// Carries everything the launcher's replay-browser tree needs without
// rescanning the body bytes: nicks, character/color, winner + per-side
// rounds_won, session grouping (session_id + match_index), and a
// round_offsets[] table of byte positions so round-level seek is a
// single fread+fseek instead of a body walk.
//
// Body layout unchanged: same packed SessionEvent[] bytes after the header.
struct FM2KSessionFileHeader {
    // wire envelope (preserves first 8 bytes from v1 layout)
    uint32_t magic;              // 'FMSS' (0x53534D46)
    uint16_t version;            // = 2
    uint16_t flags;              // bit 0: 1=battle slice (.fm2krep), 0=full session (.fm2kset)
                                 // bit 1: 1=round_offsets[] populated

    // descriptive
    uint64_t started_at_unix;
    uint64_t finished_at_unix;
    uint32_t event_count;
    uint32_t input_count;
    char     game_id[32];        // e.g. "pkmncc"
    char     p1_nick[32];
    char     p2_nick[32];

    // character / outcome
    uint8_t  p1_char_id;
    uint8_t  p2_char_id;
    uint8_t  p1_color;
    uint8_t  p2_color;
    uint8_t  rounds_won_p1;      // from latest MATCH_END payload
    uint8_t  rounds_won_p2;
    uint8_t  match_count;        // .fm2kset: total in session; .fm2krep: 1
    uint8_t  match_index;        // .fm2krep: 1-based; .fm2kset: 0
    uint64_t session_id;         // shared across .fm2krep slices of one .fm2kset

    // seek anchors -- body-relative byte offsets pointing at ROUND_START
    // tag bytes. Unused slots = 0. Capped at 8 (best-of-15 doesn't exist).
    uint8_t  round_count;        // 0..8
    uint8_t  reserved0[3];
    uint32_t round_offsets[8];

    // future-proofing -- all zeros for v2 readers
    uint8_t  reserved[76];
};
static_assert(sizeof(FM2KSessionFileHeader) == 256,
              "FM2KSessionFileHeader must be 256 bytes");
#pragma pack(pop)

// Encode events[first..last) into a vector<uint8_t> of packed wire bytes.
// MATCH_START events look up their 96-byte header from the host's
// match_headers side table. While encoding, record the body-relative byte
// offset of each ROUND_START tag we emit so the C7 header can populate
// round_offsets[].
void EncodeEventSliceToBytes(const std::vector<SessionEvent>& events,
                             const std::vector<MatchHeader>& headers,
                             size_t first, size_t last,
                             std::vector<uint8_t>& out_bytes,
                             uint32_t& out_input_count,
                             std::vector<uint32_t>& out_round_offsets) {
    out_input_count = 0;
    out_round_offsets.clear();
    out_bytes.reserve((last - first) * SESSION_EVENT_MAX_WIRE_SIZE);
    for (size_t i = first; i < last; i++) {
        const SessionEvent& ev = events[i];
        const size_t off_pre = out_bytes.size();
        AppendEventToWire(out_bytes, ev, headers);
        if (ev.type == SessionEventType::INPUT) ++out_input_count;
        if (ev.type == SessionEventType::ROUND_START) {
            out_round_offsets.push_back(static_cast<uint32_t>(off_pre));
        }
    }
}

// Locate the most-recent MATCH_START event preceding the given index in the
// slice and return its 96-byte header bytes (empty if none in slice). Used
// at write time to populate the C7 header's char IDs.
bool ResolveMatchHeader(const std::vector<SessionEvent>& events,
                        const std::vector<MatchHeader>& headers,
                        size_t first, size_t last,
                        MatchHeader& out_hdr) {
    for (size_t i = last; i-- > first; ) {
        const SessionEvent& ev = events[i];
        if (ev.type == SessionEventType::MATCH_START &&
            ev.u.match_start_idx < headers.size()) {
            out_hdr = headers[ev.u.match_start_idx];
            return true;
        }
    }
    return false;
}

// Find the most-recent MATCH_END payload in the slice (used to populate the
// header's winner / per-side rounds_won fields for .fm2krep). For .fm2kset
// (full session) this is the LAST match's MATCH_END which is the right
// summary for the session.
bool ResolveLatestMatchEnd(const std::vector<SessionEvent>& events,
                           size_t first, size_t last,
                           MatchEndPayload& out) {
    for (size_t i = last; i-- > first; ) {
        if (events[i].type == SessionEventType::MATCH_END) {
            out = events[i].u.match_end;
            return true;
        }
    }
    return false;
}

uint8_t CountMatchesInSlice(const std::vector<SessionEvent>& events,
                            size_t first, size_t last) {
    size_t n = 0;
    for (size_t i = first; i < last; i++) {
        if (events[i].type == SessionEventType::MATCH_START) ++n;
    }
    return n > 255 ? (uint8_t)255 : (uint8_t)n;
}

bool WriteSessionFileImpl(const char* path,
                          const std::vector<SessionEvent>& events,
                          const std::vector<MatchHeader>& headers,
                          size_t first, size_t last,
                          bool is_battle_slice) {
    if (first >= last) return false;
    if (last > events.size()) return false;

    std::vector<uint8_t> body;
    uint32_t input_count = 0;
    std::vector<uint32_t> round_offsets;
    EncodeEventSliceToBytes(events, headers, first, last,
                            body, input_count, round_offsets);
    if (body.empty()) return false;

    FM2KSessionFileHeader hdr = {};
    hdr.magic            = SESSION_FILE_MAGIC;
    hdr.version          = SESSION_FILE_VERSION;
    hdr.flags            = is_battle_slice ? 1u : 0u;
    if (!round_offsets.empty()) hdr.flags |= (1u << 1);

    const uint64_t now_unix = static_cast<uint64_t>(std::time(nullptr));
    hdr.started_at_unix  = now_unix;   // best-effort: use write time as
    hdr.finished_at_unix = now_unix;   //   both anchors when .fm2kset is
                                       //   written at session end.
                                       //   (Full session walks already
                                       //   emit at shutdown -- close enough
                                       //   for chronological sort.)
    hdr.event_count      = static_cast<uint32_t>(last - first);
    hdr.input_count      = input_count;

    // Char/color from the most-recent MATCH_START header in this slice.
    MatchHeader mh;
    if (ResolveMatchHeader(events, headers, first, last, mh)) {
        hdr.p1_char_id = mh[28];
        hdr.p1_color   = mh[29];
        hdr.p2_char_id = mh[30];
        hdr.p2_color   = mh[31];
    }

    // Latest MATCH_END payload populates winner + per-side rounds.
    MatchEndPayload me{};
    if (ResolveLatestMatchEnd(events, first, last, me)) {
        hdr.rounds_won_p1 = me.rounds_won_p1;
        hdr.rounds_won_p2 = me.rounds_won_p2;
    }

    hdr.match_count  = is_battle_slice ? 1
                                       : CountMatchesInSlice(events, first, last);
    hdr.match_index  = is_battle_slice ? 1 : 0;
    hdr.session_id   = g_state.session_id;

    // Populate p1_nick / p2_nick / game_id from SharedMem + exe path.
    // Previously these were left at the memset-zeroed default which made
    // the launcher's replay browser show every match as "?" vs "?" and
    // the stats/hub couldn't distinguish matches by participant. The
    // launcher writes ui_my_nick + ui_peer_nick to SharedMem at HELLO
    // exchange time; we read them here and assign to p1/p2 based on
    // which side we are (host = player_index 0 = p1).
    // Only host (0) and joiner (1) participate in the match -- spec
    // (player_index = 2 sentinel) has its own nick but neither matches
    // P1 nor P2. For spec-written .fm2kset / .fm2krep we leave nicks
    // zero rather than write spec's-own-nick as one of the participants.
    // TODO: relay P1/P2 nicks via a SESSION_NICKS or MATCH_START extension
    // so spec-written files can also carry the correct nicks.
    if (FM2KSharedMemData* sm = GetSharedMemory();
        sm && (g_player_index == 0 || g_player_index == 1)) {
        const char* my_nick   = sm->ui_my_nick;
        const char* peer_nick = sm->ui_peer_nick;
        if (g_player_index == 0) {
            std::strncpy(hdr.p1_nick, my_nick,   sizeof(hdr.p1_nick) - 1);
            std::strncpy(hdr.p2_nick, peer_nick, sizeof(hdr.p2_nick) - 1);
        } else {  // joiner (1)
            std::strncpy(hdr.p1_nick, peer_nick, sizeof(hdr.p1_nick) - 1);
            std::strncpy(hdr.p2_nick, my_nick,   sizeof(hdr.p2_nick) - 1);
        }
    }

    // game_id from exe basename (matches the convention used by
    // upload_queue manifests at netplay.cpp:198-209). Strip dirs + .exe.
    {
        char exe_path[MAX_PATH] = {};
        DWORD n = GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
        if (n > 0 && n < sizeof(exe_path)) {
            const char* basename = exe_path;
            for (const char* p = exe_path; *p; ++p) {
                if (*p == '\\' || *p == '/') basename = p + 1;
            }
            std::strncpy(hdr.game_id, basename, sizeof(hdr.game_id) - 1);
            // Strip ".exe" suffix -- zero from dot to end (not just dot itself,
            // otherwise trailing "exe" bytes after the inline NUL show up in
            // downstream consumers that don't stop at first NUL).
            if (char* dot = std::strrchr(hdr.game_id, '.')) {
                std::memset(dot, 0,
                            sizeof(hdr.game_id) - (size_t)(dot - hdr.game_id));
            }
        }
    }

    const size_t n_rounds = std::min<size_t>(round_offsets.size(),
                                             sizeof(hdr.round_offsets) / sizeof(hdr.round_offsets[0]));
    hdr.round_count = static_cast<uint8_t>(n_rounds);
    for (size_t i = 0; i < n_rounds; i++) {
        hdr.round_offsets[i] = round_offsets[i];
    }

    FILE* fp = std::fopen(path, "wb");
    if (!fp) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: failed to open %s for write", path);
        return false;
    }
    std::fwrite(&hdr, 1, sizeof(hdr), fp);
    std::fwrite(body.data(), 1, body.size(), fp);
    std::fclose(fp);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: wrote %s v2 (%u events, %u INPUTs, %u rounds, "
        "session=0x%016llX, %zu bytes, %s)",
        path, hdr.event_count, hdr.input_count, hdr.round_count,
        (unsigned long long)hdr.session_id,
        sizeof(hdr) + body.size(),
        is_battle_slice ? "battle slice" : "full session");
    return true;
}

} // namespace

bool SpectatorNode_WriteSessionFile(const char* path) {
    return WriteSessionFileImpl(path,
                                g_state.session_events,
                                g_state.match_headers,
                                0, g_state.session_events.size(),
                                /*is_battle_slice=*/false);
}

bool SpectatorNode_WriteCurrentBattleFile(const char* path) {
    if (g_state.last_match_start_idx < 0) return false;
    // Slice from the start of the state-init prefix (PIN_RNG / RESET /
    // SOUND_INIT / SESSION_ID block immediately preceding MATCH_START)
    // not from MATCH_START itself. Without the prefix the file isn't
    // self-replayable: a spectator that loads it would inherit stale
    // RNG seed / input edge state / sound layer state from whatever
    // ran in their FM2K before the load. With the prefix, the events
    // drain through ApplySessionEvent and rebuild the engine to the
    // exact state the host had at battle entry.
    const size_t first = (g_state.last_pre_match_init_idx >= 0)
        ? static_cast<size_t>(g_state.last_pre_match_init_idx)
        : static_cast<size_t>(g_state.last_match_start_idx);
    const size_t last  = g_state.session_events.size();
    return WriteSessionFileImpl(path,
                                g_state.session_events,
                                g_state.match_headers,
                                first, last,
                                /*is_battle_slice=*/true);
}

// State-init event types -- emitted unconditionally during a Pass-1 walk
// up to the seek anchor. These rebuild engine state (RNG seed, input ring
// reset, sound layer init, match header) so playback resumes correctly at
// the anchor without replaying every prior INPUT. ROUND_END is included
// so the post-round banner clean-up state is consistent at the moment the
// next ROUND_START fires; ROUND_START itself is what the seek lands on,
// so it's NOT emitted in Pass 1 (Pass 2 starts at the ROUND_START tag and
// pushes it as the first event).
static bool IsStateInitForSeek(SessionEventType t) {
    return t == SessionEventType::PIN_RNG
        || t == SessionEventType::RESET_INPUT_STATE
        || t == SessionEventType::SOUND_INIT
        || t == SessionEventType::MATCH_START
        || t == SessionEventType::SESSION_ID;
}

bool SpectatorNode_LoadSessionFile(const char* path, const SeekTarget& seek) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: failed to open %s for read", path);
        return false;
    }

    FM2KSessionFileHeader hdr = {};
    if (std::fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s truncated header", path);
        return false;
    }
    if (hdr.magic != SESSION_FILE_MAGIC) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s bad magic 0x%08X", path, hdr.magic);
        return false;
    }
    if (hdr.version != SESSION_FILE_VERSION) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s unsupported version %u (expected %u)",
            path, hdr.version, SESSION_FILE_VERSION);
        return false;
    }

    // Resolve the seek anchor against header round_offsets[] before
    // touching pb_queue. If the seek is unsatisfiable (round idx out of
    // range, header missing round table, etc.) bail without disturbing
    // playback state -- caller can retry without seek.
    size_t anchor_offset = 0;  // 0 = no seek, walk from body start
    if (seek.kind == SeekEventKind::MATCH_START && seek.idx == 0) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s seek match=0 invalid (1-based)", path);
        return false;
    }
    if (seek.kind == SeekEventKind::ROUND_START) {
        if (hdr.round_count == 0 ||
            (hdr.flags & (1u << 1)) == 0) {
            std::fclose(fp);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s has no round_offsets -- seek unavailable",
                path);
            return false;
        }
        if (seek.idx == 0 || seek.idx > hdr.round_count) {
            std::fclose(fp);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s seek round=%u out of range (have %u)",
                path, seek.idx, hdr.round_count);
            return false;
        }
        anchor_offset = hdr.round_offsets[seek.idx - 1];
    }

    const uint32_t reported_event_count = hdr.event_count;
    const uint64_t loaded_session_id    = hdr.session_id;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: %s loading v2 (events=%u, %u rounds, session=0x%016llX)",
        path, hdr.event_count, hdr.round_count,
        (unsigned long long)hdr.session_id);

    std::fseek(fp, 0, SEEK_END);
    long total = std::ftell(fp);
    if (total < (long)sizeof(hdr)) {
        std::fclose(fp);
        return false;
    }
    const size_t body_len = static_cast<size_t>(total) - sizeof(hdr);
    std::fseek(fp, static_cast<long>(sizeof(hdr)), SEEK_SET);
    std::vector<uint8_t> body(body_len);
    if (std::fread(body.data(), 1, body_len, fp) != body_len) {
        std::fclose(fp);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s body read failed", path);
        return false;
    }
    std::fclose(fp);

    if (anchor_offset > body_len) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: %s anchor offset %zu past body end %zu",
            path, anchor_offset, body_len);
        return false;
    }

    // MATCH_START seek: no header table exists for match offsets (256B v2
    // header predates set-seek), so resolve by pre-scanning the body for
    // the idx-th MATCH_START. Single decode walk over an in-memory buffer
    // -- microseconds even for a long set.
    if (seek.kind == SeekEventKind::MATCH_START) {
        size_t scan = 0; uint16_t seen = 0; bool found = false;
        while (scan < body_len) {
            SessionEvent ev{};
            uint8_t hb[SESSION_EVENT_MATCH_HDR_SIZE] = {};
            size_t r = SessionEvent_Decode(body.data() + scan,
                                           body_len - scan, &ev, hb);
            if (r == 0) break;
            if (ev.type == SessionEventType::MATCH_START && ++seen == seek.idx) {
                anchor_offset = scan;
                found = true;
                break;
            }
            scan += r;
        }
        if (!found) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s seek match=%u not found (%u MATCH_START "
                "event(s) in body)", path, seek.idx, seen);
            return false;
        }
    }

    // Fresh playback: clear receiver state, then walk events.
    g_state.pb_queue.clear();
    g_state.pb_match_headers.clear();
    g_state.pb_current_p1 = 0;
    g_state.pb_current_p2 = 0;
    g_state.have_frame_baseline = false;
    g_state.next_expected_frame = 0;
    g_state.playing_back = true;
    g_state.pb_boundary         = State::PbBoundary::NONE;
    g_state.pending_reset_input = false;
    g_state.pending_sound_init  = false;
    CssAutoConfirm_SetSeamHold(false);
    if (loaded_session_id != 0) g_state.session_id = loaded_session_id;

    auto push_event = [&](SessionEvent& ev, const uint8_t* hdr_buf) {
        if (ev.type == SessionEventType::MATCH_START) {
            MatchHeader hdr_copy;
            std::memcpy(hdr_copy.data(), hdr_buf, hdr_copy.size());
            g_state.pb_match_headers.push_back(hdr_copy);
            ev.u.match_start_idx =
                static_cast<uint16_t>(g_state.pb_match_headers.size() - 1);
        }
        g_state.pb_queue.push_back(ev);
    };

    size_t off = 0;
    uint32_t pushed_inputs = 0;
    uint32_t pre_anchor_skipped = 0;

    // Pass 1 -- body[0..anchor_offset). Emit only state-init events
    // (skipped if anchor_offset == 0, i.e. no-seek).
    while (off < anchor_offset) {
        SessionEvent ev{};
        uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE] = {};
        size_t r = SessionEvent_Decode(body.data() + off, body_len - off,
                                       &ev, hdr_buf);
        if (r == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s pass1 decode failed at off=%zu",
                path, off);
            return false;
        }
        if (off + r > anchor_offset) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s anchor offset %zu mid-event "
                "(event ends at %zu)",
                path, anchor_offset, off + r);
            return false;
        }
        // Match-seek keeps ONLY session context from the pre-anchor body:
        // earlier matches' MATCH_START/PIN/SOUND_INIT would make the drain
        // boot every prior match's characters before reaching the target.
        // The anchor MATCH_START (pass 2, inclusive) carries the target
        // match's full init in-band.
        const bool keep = (seek.kind == SeekEventKind::MATCH_START)
            ? (ev.type == SessionEventType::SESSION_ID)
            : IsStateInitForSeek(ev.type);
        if (keep) {
            push_event(ev, hdr_buf);
        } else {
            ++pre_anchor_skipped;
        }
        off += r;
    }

    // Pass 2 -- body[anchor_offset..body_len). Emit everything.
    while (off < body_len) {
        SessionEvent ev{};
        uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE] = {};
        size_t r = SessionEvent_Decode(body.data() + off, body_len - off,
                                       &ev, hdr_buf);
        if (r == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: %s pass2 decode failed at off=%zu",
                path, off);
            return false;
        }
        push_event(ev, hdr_buf);
        if (ev.type == SessionEventType::INPUT) ++pushed_inputs;
        off += r;
    }

    if (seek.kind == SeekEventKind::ROUND_START ||
        seek.kind == SeekEventKind::MATCH_START) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: loaded %s -- seek=%s %u, "
            "%u state-init kept + %u skipped pre-anchor, "
            "%u INPUTs queued post-anchor (%u total events)",
            path,
            seek.kind == SeekEventKind::MATCH_START ? "MATCH_START"
                                                    : "ROUND_START",
            seek.idx,
            (uint32_t)(g_state.pb_queue.size() - pushed_inputs),
            pre_anchor_skipped, pushed_inputs, reported_event_count);
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: loaded %s -- %u events (%u INPUTs) into pb_queue",
            path, reported_event_count, pushed_inputs);
    }
    return true;
}
