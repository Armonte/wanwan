// Spectator HOST-SIDE event recording: On{MatchStart,FrameConfirmed,MatchEnd} +
// the Append* session-event ops + the determinism fingerprint. Extracted VERBATIM
// from spectator_node.cpp. Public API (decls in spectator_node.h) + the internal
// AppendOpAndFlush helper; reaches specnode helpers via using.
#include "spectator_node.h"
#include "../parity/parity_pool.h"  // ParityPool::ReadPlayer -- scan, not fixed slots 0/1
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
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
    // event hits the wire -- keeps the per-INPUT-frame numbering monotonic
    // across the CSS→battle seam. Without this, trailing CSS frames sit
    // unbatched in session_events past last_flushed_event_idx; their
    // session-relative INPUT-frame indices are below the next live battle
    // batch's start_frame, but the spectator's next_expected_frame would
    // already be at the higher index → indefinite gap.
    FlushBatch();

    // Stash the initial-match metadata as a 96-byte payload that's the
    // canonical MATCH_START event body (layout pinned by Replay::ReplayHeader
    // in replay.h -- kept stable so the wire schema doesn't churn).
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
        // h+89: THE HOST'S ACTUAL LATCH, not the source. reserved[0] of
        // ReplayHeader::reserved[3] (replay.h), previously always zero.
        //
        // WHY A SECOND FIELD FOR "the same" NUMBER. h+85 is g_default_round
        // (0x430124), the CONFIG SOURCE. What the host's SIM consumes is
        // g_round_limit (0x470048), the LATCH the engine took from that source
        // at 0x4087DA. 3297b25 exists precisely because those two can DISAGREE
        // (a late HOST_CONFIG re-writes the source after the latch was taken),
        // and its re-derive REFUSES to write on three paths -- no true settings
        // agreement, mode_flag != 1, implausible source -- each of which leaves
        // the host running latch L while h+85 advertises source S != L. A
        // viewer deriving its latch from h+85 would then install S and diverge
        // from the host by construction. So carry L itself.
        //
        // ORDERING (this stamp must be POST-relatch, and is):
        //   Netplay_IsBattleSynced() takes the g_battle_synced=true transition
        //   and calls RederiveBattleEntryLatches (netplay_barriers.cpp:451);
        //   both of its call sites (trampoline_battle.cpp:164,
        //   hooks_update.cpp:348) then call Netplay_StartBattle, which calls
        //   SpectatorNode_OnMatchStart (this function) and only afterwards
        //   SpectatorNode_StashSnapshot. So h+89 and the snapshot's 0x470048
        //   are now genuinely two readings of ONE word at one instant.
        //
        // ENCODING: one byte, 1..9 (the engine's own UDM_SETRANGE 0x00090001
        // on the 0x430124 spin control). 0 means NOT CARRIED -- the value the
        // whole 96-byte header is memset to, so every legacy .fm2krep and every
        // pre-this-build producer reads 0 and the consumer fail-closes. An
        // out-of-range live latch also stamps 0 rather than a truncated byte.
        const uint32_t host_round_latch = *(uint32_t*)0x470048;  // g_round_limit
        h[89] = (host_round_latch >= 1u && host_round_latch <= 9u)
                    ? (uint8_t)host_round_latch
                    : (uint8_t)0;
    }
    // frame_count at h+92 stays 0 -- subscribers get INPUT_BATCH frames live.
    g_state.initial_match.valid = true;

    // C6: append MATCH_START as a SessionEvent op so the metadata flows in
    // the same ordered stream as INPUTs. Spectator's drain applies the op
    // at exactly the logical frame the host set the match up. Late joiners
    // get this op as part of SendSessionBackfillFromStart. The legacy INITIAL_MATCH
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
    ++g_state.total_input_count;

    // Live broadcast batching window -- only fan out to existing subscribers.
    // Cadence trigger: every BROADCAST_BATCH_FRAMES new INPUT events.
    const uint32_t pending_inputs =
        g_state.total_input_count - g_state.flushed_input_count;
    if (pending_inputs >= BROADCAST_BATCH_FRAMES) {
        FlushBatch();
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
// INPUT event in the stream -- drain-at-head semantics on the receiver
// then apply the op exactly when the spectator's local sim is about to
// consume the same logical frame the host did.

namespace {

void AppendOpAndFlush(const SessionEvent& ev) {
    // Ops already in session_events, i.e. the op prefix count at the index
    // this event is about to occupy. Captured BEFORE the ++ below and before
    // the push_back, so it never includes this event itself -- which is
    // exactly the meaning of SendSessionEventsTo's op_cursor prefix. Only
    // consumed by the CSS-anchor stamp further down.
    const uint32_t ops_before_this = g_state.total_op_count;
    // Single choke point for non-INPUT appends -- the running op count is
    // what EVENT_BATCH2 ships as its absolute op base, so viewers dedupe
    // ops idempotently.
    ++g_state.total_op_count;
    // Deep-join anchor (Design 2): remember where the CURRENT char-select
    // starts so a between-matches joiner can be backfilled from here instead
    // of from frame 0. Recorded BEFORE the push_back, so the index is the
    // CSS_ENTERED op itself and the frame is the one the next INPUT will
    // carry (AppendInput uses total_input_count pre-increment). O(1); done at
    // this single choke point so no future CSS_ENTERED caller can miss it.
    if (ev.type == SessionEventType::CSS_ENTERED) {
        g_state.have_css_anchor        = true;
        g_state.css_anchor_event_idx   = g_state.session_events.size();
        g_state.css_anchor_input_frame = g_state.total_input_count;
        // Third leg of the same O(1) stamp: the op prefix at that index.
        // Lets the backfill senders resume their op-cursor scan here instead
        // of recounting from 0 on every gap-fill pull (candidate A1).
        g_state.css_anchor_op_count    = ops_before_this;
    } else if (ev.type == SessionEventType::MATCH_END) {
        g_state.have_prior_match = true;   // a set is now in progress
    }
    g_state.session_events.push_back(ev);
    // Flush eagerly when subscribers exist (host with live spectators OR
    // relay node with sub-spectators). When the subscriber list is empty,
    // there's nothing to send; late joiners get the full backlog via
    // SendSessionBackfillFromStart. Note: we don't gate on `broadcasting` --
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

// C3.5 -- round events. Snapshot input-frame at ROUND_START so AppendRoundEnd
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

// C10 -- 1-based per-session match counter. Bumped at every
// AppendMatchStart. Reset to 0 in SpectatorNode_AppendSessionId so a
// new session restarts numbering at 1 for its first match.
static uint8_t s_match_index_in_session = 0;

void SpectatorNode_AppendRoundStart(uint8_t  round_idx,
                                    uint16_t p1_hp_max,
                                    uint16_t p2_hp_max,
                                    uint16_t timer_seconds) {
    s_round_start_input_frame = g_state.total_input_count;
    // New round starting -- clear stale rounds_won cache from a possibly
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

    // Cache live rounds_won AT THIS MOMENT -- accurate snapshot for
    // AppendMatchEnd to use later. The match-over path resets these
    // counters before Netplay_EndBattle's read fires.
    s_last_seen_rounds_won_p1 = (uint8_t)*(uint32_t*)0x4DFC6D;
    s_last_seen_rounds_won_p2 = (uint8_t)*(uint32_t*)0x4EDCAC;

    // C10 -- also push this round's result into SharedMem so the launcher
    // can include it in the hub match_result JSON's "rounds[]" array.
    SharedMem_PublishRoundResult(winner_idx, p1_hp_remaining,
                                 p2_hp_remaining, frames);
}

// =============================================================================
// FINGERPRINT (C9) -- diagnostic state hash for desync detection
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
// Gated on FM2K_SPEC_FINGERPRINT=1 -- off by default so the wire stays
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
    // fields, both sides update together -- divergent samples would yield
    // a hash mismatch that the spectator catches at runtime.
    //
    // Player world state is resolved by SCAN (ParityPool::ReadPlayer), not
    // by fixed pool slots 0/1 -- the fixed-index read samples whatever the
    // first-free allocator happened to leave in slot 1, which for a whole
    // match can be a non-player object. That made this hash mismatch on a
    // pool displacement that was not a sim divergence. Both peers run the
    // same build and compute this the same way, so the change is symmetric;
    // it does change the VALUE of the wire fingerprint, which is safe
    // because FINGERPRINT ops are only ever compared inside one session
    // (and this whole path is off unless FM2K_SPEC_FINGERPRINT=1).
    struct Sample {
        uint32_t rng;
        uint32_t buf_idx;
        uint32_t p1_hp, p2_hp;
        uint32_t timer;
        int32_t  p1_x, p1_y, p2_x, p2_y;
        int32_t  p1_script, p2_script;
    } s;
    const ParityPool::PlayerView p1v = ParityPool::ReadPlayer(0);
    const ParityPool::PlayerView p2v = ParityPool::ReadPlayer(1);
    s.rng       = *(uint32_t*)0x41FB1C;
    s.buf_idx   = *(uint32_t*)0x447EE0;
    s.p1_hp     = *(uint32_t*)0x4DFC85;
    s.p2_hp     = *(uint32_t*)0x4EDCC4;
    s.timer     = *(uint32_t*)0x470044;
    s.p1_x      = p1v.pos_x;
    s.p1_y      = p1v.pos_y;
    s.p2_x      = p2v.pos_x;
    s.p2_y      = p2v.pos_y;
    s.p1_script = p1v.script;
    s.p2_script = p2v.script;

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

    // C10 -- bump the per-session match index and publish to SharedMem
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
    // before the read. Take the max of (cache, passed) -- the cache is
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
    // C10 -- new session, restart match numbering. The first
    // AppendMatchStart for this session bumps to 1.
    s_match_index_in_session = 0;
    SessionEvent ev{};
    ev.type          = SessionEventType::SESSION_ID;
    ev.u.session_id  = session_id;
    AppendOpAndFlush(ev);
}

// Adopt an id MINTED ELSEWHERE (the host's, arriving on HOST_CONFIG). Two
// deliberate differences from AppendSessionId:
//   * it refuses to overwrite a non-zero id, so a receiver can never fight the
//     host and the two sides' files can never disagree on the group key;
//   * it does NOT reset s_match_index_in_session. Adoption is best-effort
//     delivery: if the handshake push is lost, it can land after match 1 has
//     already been played, and zeroing the counter there would make this peer
//     write a second "Match 1" and report a match_index the host disagrees
//     with (hub reconcile_session logs that as a mismatch).
void SpectatorNode_AdoptSessionId(uint64_t session_id) {
    if (session_id == 0) return;
    if (g_state.session_id != 0) return;
    g_state.session_id = session_id;
    SessionEvent ev{};
    ev.type          = SessionEventType::SESSION_ID;
    ev.u.session_id  = session_id;
    AppendOpAndFlush(ev);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: adopted session_id=0x%016llX from host (match index "
        "continues at %u)",
        (unsigned long long)session_id, (unsigned)s_match_index_in_session);
}

uint64_t SpectatorNode_GetSessionId() {
    return g_state.session_id;
}

uint8_t SpectatorNode_GetMatchIndexInSession() {
    return s_match_index_in_session;
}
