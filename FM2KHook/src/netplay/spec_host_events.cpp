// Spectator HOST-SIDE event recording: On{MatchStart,FrameConfirmed,MatchEnd} +
// the Append* session-event ops + the determinism fingerprint. Extracted VERBATIM
// from spectator_node.cpp. Public API (decls in spectator_node.h) + the internal
// AppendOpAndFlush helper; reaches specnode helpers via using.
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

// -----------------------------------------------------------------------------
// HOST-SIDE
// -----------------------------------------------------------------------------

void SpectatorNode_OnMatchStart(
    uint32_t game_hash,
    uint32_t initial_rng_seed,
    uint32_t initial_state_hash,
    uint8_t p1_char, uint8_t p1_color,
    uint8_t p2_char, uint8_t p2_color,
    uint8_t stage_id)
{
    g_state.broadcasting = true;
    // Flush any unbatched CSS events before the match-start MATCH_START
    // event hits the wire — keeps the per-INPUT-frame numbering monotonic
    // across the CSS→battle seam. Without this, trailing CSS frames sit
    // unbatched in session_events past last_flushed_event_idx; their
    // session-relative INPUT-frame indices are below the next live battle
    // batch's start_frame, but the spectator's next_expected_frame would
    // already be at the higher index → indefinite gap.
    FlushBatch();

    // Stash the initial-match metadata as a 96-byte payload that's the
    // canonical MATCH_START event body (layout pinned by Replay::ReplayHeader
    // in replay.h — kept stable so the wire schema doesn't churn).
    uint8_t* h = g_state.initial_match.header_bytes;
    std::memset(h, 0, 96);
    uint32_t magic   = 0x52504D46;  // Replay::REPLAY_MAGIC
    uint16_t version = 1;
    std::memcpy(h + 0,  &magic,              4);
    std::memcpy(h + 4,  &version,            2);
    std::memcpy(h + 16, &game_hash,          4);  // game_hash (after timestamp)
    std::memcpy(h + 20, &initial_rng_seed,   4);
    std::memcpy(h + 24, &initial_state_hash, 4);
    h[28] = p1_char;
    h[29] = p1_color;
    h[30] = p2_char;
    h[31] = p2_color;
    // p1_name / p2_name at h+32 / h+56 left zeroed; filled once UI plumbs them.
    h[80] = stage_id;
    // #66/replay: carry the match's round-timer gameconfig so playback runs the
    // SAME round length (see Replay::ReplayHeader). h+81 round_time_cfg, h+85
    // round_count. FM2K globals; skipped on FM95 (different config layout).
    if constexpr (!FM2K::kIsFM95) {
        uint32_t round_time_sec = *(uint32_t*)0x430114;  // g_round_time (netplay-synced)
        uint32_t round_count    = *(uint32_t*)0x430124;  // g_default_round
        std::memcpy(h + 81, &round_time_sec, 4);
        std::memcpy(h + 85, &round_count,    4);
    }
    // frame_count at h+92 stays 0 — subscribers get INPUT_BATCH frames live.
    g_state.initial_match.valid = true;

    // C6: append MATCH_START as a SessionEvent op so the metadata flows in
    // the same ordered stream as INPUTs. Spectator's drain applies the op
    // at exactly the logical frame the host set the match up. Late joiners
    // get this op as part of SendSessionBackfillTo. The legacy INITIAL_MATCH
    // packet path (still sent below) is kept for back-compat; once all
    // peers run C6+ builds we can retire it.
    SpectatorNode_AppendMatchStart(g_state.initial_match.header_bytes);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Match start broadcast (seed=0x%08X, subs=%zu)",
                initial_rng_seed, g_state.subscribers.size());
}

void SpectatorNode_OnFrameConfirmed(uint16_t p1_input, uint16_t p2_input) {
    // Append an INPUT event to the session log so a late joiner can backfill
    // every confirmed frame from session start, including CSS frames that
    // happened before any spectator subscribed. 5 B/event in memory.
    SessionEvent ev{};
    ev.type = SessionEventType::INPUT;
    ev.u.input.p1 = p1_input;
    ev.u.input.p2 = p2_input;
    g_state.session_events.push_back(ev);
    // Phase F: mirror into the UDP accelerator ring, keyed by this input's
    // session-relative frame index (= total_input_count pre-increment).
    g_state.udp_ring_p1[g_state.total_input_count % SPEC_UDP_WINDOW] = p1_input;
    g_state.udp_ring_p2[g_state.total_input_count % SPEC_UDP_WINDOW] = p2_input;
    ++g_state.total_input_count;

    // Live broadcast batching window — only fan out to existing subscribers.
    // Cadence trigger: every BROADCAST_BATCH_FRAMES new INPUT events.
    const uint32_t pending_inputs =
        g_state.total_input_count - g_state.flushed_input_count;
    if (pending_inputs >= BROADCAST_BATCH_FRAMES) {
        FlushBatch();
    }

    // Phase F: redundant UDP window every SPEC_UDP_SEND_INTERVAL confirmed
    // frames. Internally no-ops when disabled / relay-mode / no udp_ok subs.
    if ((g_state.total_input_count % SPEC_UDP_SEND_INTERVAL) == 0) {
        SendUdpInputBatches();
    }
}

void SpectatorNode_OnMatchEnd(const MatchEndPayload& p) {
    if (!g_state.broadcasting) return;
    // Flush whatever's left in the pending event window so viewers see the
    // final frames before MATCH_END.
    FlushBatch();
    // MATCH_END flows in-band as a SessionEvent op; the apply-at-head drain
    // on the receiver flips playing_back=false at the same logical frame
    // the host appended. (Legacy MATCH_END packet was retired in C12.)
    SpectatorNode_AppendMatchEnd(p);
    g_state.broadcasting = false;
    g_state.initial_match.valid = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: Match end broadcast (winner=%u rounds=%u-%u frames=%u)",
        p.winner_idx, p.rounds_won_p1, p.rounds_won_p2, p.frames_total);
}

// -----------------------------------------------------------------------------
// HOST-SIDE OP APPENDERS (C3)
// -----------------------------------------------------------------------------
//
// Append-and-flush helpers. Called by host pin sites in netplay.cpp /
// savestate.cpp immediately after the local memory write. The append+flush
// pair guarantees the op reaches subscribed spectators before the next
// INPUT event in the stream — drain-at-head semantics on the receiver
// then apply the op exactly when the spectator's local sim is about to
// consume the same logical frame the host did.

namespace {

void AppendOpAndFlush(const SessionEvent& ev) {
    // Phase F: single choke point for non-INPUT appends -- the running op
    // count ships as op_seq in UDP_INPUT_BATCH so viewers can order
    // inputs after ops (see admission invariant in spectator_node.h).
    // Pre-encode into the redundant ops ring for the datagram tail.
    {
        std::vector<uint8_t> w;
        AppendEventToWire(w, ev, g_state.match_headers);
        if (!w.empty() && w.size() <= sizeof(State::OpWire::bytes)) {
            auto& slot = g_state.udp_ops_ring[g_state.total_op_count % State::OPS_RING];
            slot.op_index  = g_state.total_op_count;
            slot.input_pos = g_state.total_input_count;
            slot.len       = (uint8_t)w.size();
            std::memcpy(slot.bytes, w.data(), w.size());
        }
    }
    ++g_state.total_op_count;
    g_state.session_events.push_back(ev);
    // Flush eagerly when subscribers exist (host with live spectators OR
    // relay node with sub-spectators). When the subscriber list is empty,
    // there's nothing to send; late joiners get the full backlog via
    // SendSessionBackfillTo. Note: we don't gate on `broadcasting` —
    // that flag is host-side match state and doesn't apply to the relay
    // path where a spectator's HandleSpecData re-Appends incoming ops
    // to its own session_events for sub-spectator forwarding.
    if (!g_state.subscribers.empty()) {
        FlushBatch();
    }
}

} // namespace

void SpectatorNode_AppendPinRng(uint32_t seed) {
    SessionEvent ev{};
    ev.type            = SessionEventType::PIN_RNG;
    ev.u.pin_rng_seed  = seed;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendResetInputState() {
    SessionEvent ev{};
    ev.type = SessionEventType::RESET_INPUT_STATE;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendCssEntered() {
    SessionEvent ev{};
    ev.type = SessionEventType::CSS_ENTERED;
    // Capture the host's CSS start state (cursor x,y + selected, per player) so a
    // snapshot-join spectator can align its rematch-CSS replay walk to the host's
    // -- it never walked CSS1, so its own cursor/selected are stale and the pure
    // replay otherwise lands on the wrong character. WW/FM2K addresses.
    if constexpr (!FM2K::kIsFM95) {
        ev.u.css_entered.p1_cur_x = (int8_t)*(int32_t*)0x424E50;
        ev.u.css_entered.p1_cur_y = (int8_t)*(int32_t*)0x424E54;
        ev.u.css_entered.p2_cur_x = (int8_t)*(int32_t*)0x424E58;
        ev.u.css_entered.p2_cur_y = (int8_t)*(int32_t*)0x424E5C;
        ev.u.css_entered.p1_sel   = (int8_t)*(int32_t*)0x470020;
        ev.u.css_entered.p2_sel   = (int8_t)*(int32_t*)0x470024;
    }
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendSoundInit() {
    SessionEvent ev{};
    ev.type = SessionEventType::SOUND_INIT;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendFingerprint(uint32_t hash) {
    SessionEvent ev{};
    ev.type                  = SessionEventType::FINGERPRINT;
    ev.u.fingerprint_hash    = hash;
    AppendOpAndFlush(ev);
}

// C3.5 — round events. Snapshot input-frame at ROUND_START so AppendRoundEnd
// can compute frames_elapsed without the hook needing access to the private
// total_input_count counter.
static uint32_t s_round_start_input_frame = 0;

// Most-recent rounds_won values seen at AppendRoundEnd time. Cached
// because Netplay_EndBattle's read of FM2K::ADDR_P1/P2_ROUNDS_WON fires
// AFTER vs_round_function's match-over branch creates the type=10
// match-end object, whose update sometimes resets the live counters
// before the read. ROUND_END's read is reliably accurate (verified
// empirically), so AppendMatchEnd overrides the (potentially stale)
// values Netplay_EndBattle passed in with these.
static uint8_t s_last_seen_rounds_won_p1 = 0;
static uint8_t s_last_seen_rounds_won_p2 = 0;

// C10 — 1-based per-session match counter. Bumped at every
// AppendMatchStart. Reset to 0 in SpectatorNode_AppendSessionId so a
// new session restarts numbering at 1 for its first match.
static uint8_t s_match_index_in_session = 0;

void SpectatorNode_AppendRoundStart(uint8_t  round_idx,
                                    uint16_t p1_hp_max,
                                    uint16_t p2_hp_max,
                                    uint16_t timer_seconds) {
    s_round_start_input_frame = g_state.total_input_count;
    // New round starting — clear stale rounds_won cache from a possibly
    // earlier match. AppendRoundEnd repopulates it as rounds tick by.
    if (round_idx == 1) {
        s_last_seen_rounds_won_p1 = 0;
        s_last_seen_rounds_won_p2 = 0;
    }
    SessionEvent ev{};
    ev.type = SessionEventType::ROUND_START;
    ev.u.round_start.round_idx     = round_idx;
    ev.u.round_start.p1_hp_max     = p1_hp_max;
    ev.u.round_start.p2_hp_max     = p2_hp_max;
    ev.u.round_start.timer_seconds = timer_seconds;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendRoundEnd(uint8_t  winner_idx,
                                  uint16_t p1_hp_remaining,
                                  uint16_t p2_hp_remaining) {
    const uint32_t frames =
        (g_state.total_input_count >= s_round_start_input_frame)
            ? (g_state.total_input_count - s_round_start_input_frame)
            : 0;
    SessionEvent ev{};
    ev.type = SessionEventType::ROUND_END;
    ev.u.round_end.winner_idx       = winner_idx;
    ev.u.round_end.p1_hp_remaining  = p1_hp_remaining;
    ev.u.round_end.p2_hp_remaining  = p2_hp_remaining;
    ev.u.round_end.frames_elapsed   = frames;
    AppendOpAndFlush(ev);

    // Cache live rounds_won AT THIS MOMENT — accurate snapshot for
    // AppendMatchEnd to use later. The match-over path resets these
    // counters before Netplay_EndBattle's read fires.
    s_last_seen_rounds_won_p1 = (uint8_t)*(uint32_t*)0x4DFC6D;
    s_last_seen_rounds_won_p2 = (uint8_t)*(uint32_t*)0x4EDCAC;

    // C10 — also push this round's result into SharedMem so the launcher
    // can include it in the hub match_result JSON's "rounds[]" array.
    SharedMem_PublishRoundResult(winner_idx, p1_hp_remaining,
                                 p2_hp_remaining, frames);
}

// =============================================================================
// FINGERPRINT (C9) — diagnostic state hash for desync detection
// =============================================================================
//
// Both host and spectator sample the same set of FM2K state fields, hash
// them with classic Fletcher-32, and the host appends the result as a
// FINGERPRINT op every 30 sim frames. Spectator's ApplySessionEvent
// computes its own hash on its current state at the same logical frame
// (drain-at-head ordering ensures it's the same frame the host hashed)
// and logs WARN on mismatch, including both values. Replaces the manual
// [HOST-FP] / [SPEC-FP] log-grep diagnostic once enabled.
//
// Gated on FM2K_SPEC_FINGERPRINT=1 — off by default so the wire stays
// quiet for normal play.

bool SpectatorFingerprint_Enabled() {
    static int s_state = -1;  // 0=off, 1=on
    if (s_state < 0) {
        const char* v = std::getenv("FM2K_SPEC_FINGERPRINT");
        s_state = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
    }
    return s_state != 0;
}

uint32_t SpectatorFingerprint_Compute() {
    // Same fields the [HOST-FP]/[SPEC-FP] logs already pin. If we ever add
    // fields, both sides update together — divergent samples would yield
    // a hash mismatch that the spectator catches at runtime.
    constexpr uintptr_t POOL = 0x4701E0;
    constexpr size_t    SLOT = 382;
    struct Sample {
        uint32_t rng;
        uint32_t buf_idx;
        uint32_t p1_hp, p2_hp;
        uint32_t timer;
        int32_t  p1_x, p1_y, p2_x, p2_y;
        int32_t  p1_script, p2_script;
    } s;
    s.rng       = *(uint32_t*)0x41FB1C;
    s.buf_idx   = *(uint32_t*)0x447EE0;
    s.p1_hp     = *(uint32_t*)0x4DFC85;
    s.p2_hp     = *(uint32_t*)0x4EDCC4;
    s.timer     = *(uint32_t*)0x470044;
    s.p1_x      = *(int32_t*)(POOL + 0 * SLOT + 0x08);
    s.p1_y      = *(int32_t*)(POOL + 0 * SLOT + 0x0C);
    s.p2_x      = *(int32_t*)(POOL + 1 * SLOT + 0x08);
    s.p2_y      = *(int32_t*)(POOL + 1 * SLOT + 0x0C);
    s.p1_script = *(int32_t*)(POOL + 0 * SLOT + 0x30);
    s.p2_script = *(int32_t*)(POOL + 1 * SLOT + 0x30);

    return Fletcher32(reinterpret_cast<const uint8_t*>(&s), sizeof(s));
}

// Snapshot at MATCH_START for the C7 frames_total computation in
// AppendMatchEnd. Reset on every MATCH_START so back-to-back matches
// each get an accurate per-match input-frame delta.
static uint32_t s_match_start_input_frame = 0;

void SpectatorNode_AppendMatchStart(const uint8_t header[96]) {
    // Stash the 96-byte header in the side table and reference it by index
    // from the SessionEvent (keeps the in-memory event size at 5 B).
    MatchHeader hdr_copy;
    std::memcpy(hdr_copy.data(), header, hdr_copy.size());
    g_state.match_headers.push_back(hdr_copy);

    s_match_start_input_frame = g_state.total_input_count;

    SessionEvent ev{};
    ev.type = SessionEventType::MATCH_START;
    ev.u.match_start_idx =
        static_cast<uint16_t>(g_state.match_headers.size() - 1);
    const size_t match_start_idx = g_state.session_events.size();
    g_state.last_match_start_idx = static_cast<int64_t>(match_start_idx);

    // Backward-scan through PIN_RNG / RESET_INPUT_STATE / SOUND_INIT /
    // SESSION_ID events that precede this MATCH_START, so the per-battle
    // .fm2krep slice can include the full state-init prefix and play
    // back without depending on prior state. Stops at the first
    // non-state-init event (typically the last CSS-phase INPUT, but
    // could also be the prior match's MATCH_END / final ROUND_END).
    auto is_pre_match_init = [](SessionEventType t) {
        return t == SessionEventType::PIN_RNG
            || t == SessionEventType::RESET_INPUT_STATE
            || t == SessionEventType::SOUND_INIT
            || t == SessionEventType::SESSION_ID;
    };
    size_t pre_init_idx = match_start_idx;
    while (pre_init_idx > 0 &&
           is_pre_match_init(g_state.session_events[pre_init_idx - 1].type)) {
        --pre_init_idx;
    }
    g_state.last_pre_match_init_idx = static_cast<int64_t>(pre_init_idx);

    // C10 — bump the per-session match index and publish to SharedMem
    // so the launcher can include {session_id, match_index_in_session}
    // in its match_result JSON to the hub.
    if (s_match_index_in_session < 255) ++s_match_index_in_session;
    SharedMem_PublishMatchSession(g_state.session_id, s_match_index_in_session);

    AppendOpAndFlush(ev);
}

void SpectatorNode_GetCachedRoundsWon(uint8_t* p1, uint8_t* p2) {
    // Canonical per-match round tally, cached at each ROUND_END emit.
    // round_events attributes timeout rounds correctly (HP compare at the
    // round-over edge), and the cache survives the match-over object's
    // reset of the live engine counters -- which is why Netplay_EndBattle
    // must NOT trust the raw counter read for the deciding round.
    if (p1) *p1 = s_last_seen_rounds_won_p1;
    if (p2) *p2 = s_last_seen_rounds_won_p2;
}

void SpectatorNode_AppendMatchEnd(const MatchEndPayload& p) {
    SessionEvent ev{};
    ev.type        = SessionEventType::MATCH_END;
    ev.u.match_end = p;
    // Override caller's rounds_won with the cached values from the most
    // recent ROUND_END. Netplay_EndBattle reads from the live FM2K
    // counters but those get reset by the match-over object's update
    // before the read. Take the max of (cache, passed) — the cache is
    // reliable, but if for any reason the cache is stale (no
    // AppendRoundEnd fired yet) we fall back to whatever Netplay
    // passed.
    if (s_last_seen_rounds_won_p1 > p.rounds_won_p1) {
        ev.u.match_end.rounds_won_p1 = s_last_seen_rounds_won_p1;
    }
    if (s_last_seen_rounds_won_p2 > p.rounds_won_p2) {
        ev.u.match_end.rounds_won_p2 = s_last_seen_rounds_won_p2;
    }
    // Winner backstop (task #63 lineage): the rounds override above fixed
    // the TALLY but historically left winner_idx as the caller's guess --
    // a timeout-decided match shipped "winner=DRAW rounds=1-0" into every
    // replay header and spectator stream. If the corrected rounds are
    // decisive, they name the winner.
    if (ev.u.match_end.rounds_won_p1 != ev.u.match_end.rounds_won_p2) {
        ev.u.match_end.winner_idx =
            (ev.u.match_end.rounds_won_p1 > ev.u.match_end.rounds_won_p2) ? 0 : 1;
    }
    // Caller passes frames_total=0; we compute the actual value here so
    // hook code (Netplay_EndBattle) doesn't need access to the private
    // total_input_count counter.
    ev.u.match_end.frames_total =
        (g_state.total_input_count >= s_match_start_input_frame)
            ? (g_state.total_input_count - s_match_start_input_frame)
            : 0;
    AppendOpAndFlush(ev);
}

void SpectatorNode_AppendSessionId(uint64_t session_id) {
    g_state.session_id = session_id;
    // C10 — new session, restart match numbering. The first
    // AppendMatchStart for this session bumps to 1.
    s_match_index_in_session = 0;
    SessionEvent ev{};
    ev.type          = SessionEventType::SESSION_ID;
    ev.u.session_id  = session_id;
    AppendOpAndFlush(ev);
}

uint64_t SpectatorNode_GetSessionId() {
    return g_state.session_id;
}
