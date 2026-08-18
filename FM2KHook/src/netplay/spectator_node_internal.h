#pragma once
// Spectator node shared state model. Lifted VERBATIM out of spectator_node.cpp
// (no behavior change) so the spectator/replay logic can be split across sibling
// TUs (spec_*.cpp) that all operate on the one shared g_state. Internal to the
// spectator_node*.cpp set -- not a public API.
#include "spectator_node.h"     // SessionEvent, SpecJoinMode, SnapshotMetadata, SPEC_UDP_WINDOW, SPECTATOR_DEFAULT_CAPACITY, SESSION_EVENT_MATCH_HDR_SIZE
#include "spec_relay_queue.h"   // fm2k::spec_relay::Ring
#include "spec_snapshot_coverage.h"  // fm2k::specsnap::CoverageSet (snapshot inbox)
#include "control_channel.h"    // CtrlPacket (BuildJoinAckPacket)
#include <winsock2.h>           // sockaddr_in
#include <array>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <map>

// ---------------------------------------------------------------------------
// Wire encoding + timing constants that ONLY the spec_*.cpp set uses. They sat
// in spectator_node.h until it reached the 1000-line cap; every consumer of
// each name below already includes THIS header, so moving them changes no
// visibility (verified by grep before the move, and the public header still
// carries everything the trampoline / hooks / tests reference). Same rationale
// as the deep-join tunables at the bottom of this file.
//
// Placed ABOVE the Subscriber struct on purpose: Subscriber::pinned_ack_kind is
// initialised from SPEC_ACK_KIND_NONE.
// ---------------------------------------------------------------------------
// SPEC_JOIN_ACK host_session_kind encoding. Low 2 bits are the session kind
// (NONE / CSS / BATTLE, mirroring NetplaySessionKind). Bit 2 flags the packet
// as a LIVE REFRESH rather than a per-subscriber GRANT.
//
// GRANT (bit clear) -- "this is the shape of the stream I have pinned for YOU;
// boot accordingly". Emitted only where the host pins or re-states a specific
// subscriber's join_mode, and always from that subscriber's pinned_ack_kind.
//
// LIVE REFRESH (bit set) -- "here is where the host is right now". Emitted by
// the battle-entry re-broadcast (one packet to every subscriber at once, so it
// cannot be per-subscriber) and by the re-ACKs that intentionally change
// nothing. It is authoritative about the host's CURRENT phase and nothing else.
//
// The split is load-bearing in both directions:
//  * A refresh must never complete a FIRST-TIME handshake. The battle-entry
//    broadcast reads live state and knows nothing about what any individual
//    subscriber was pinned to, so if it is the first ACK a viewer manages to
//    process (accept ACK lost, or the two datagrams reordered -- both plain
//    unreliable UDP), a FULL_SESSION-granted viewer would read BATTLE, /F-boot
//    into battle, and then replay the from-frame-0 CSS stream it was actually
//    assigned as battle input from uninitialised state. Deterministic desync,
//    observed at 2/16 under 20% loss.
//  * A grant must never drive a PHASE SWAP on an already-mirroring viewer. A
//    CSS-era grant re-stated while the host is now in battle would otherwise
//    swap a battle mirror back out to CSS.
//
// Safe without a wire version bump: the version gate in
// SpectatorNode_HandleJoinReq rejects any joiner whose minor.patch differs from
// the host's, so both ends of a spectate session are always the same build.
// netplay_control.cpp forwards this field verbatim, so no dispatch change is
// needed either.
constexpr uint8_t SPEC_ACK_KIND_NONE    = 0;
constexpr uint8_t SPEC_ACK_KIND_CSS     = 1;
constexpr uint8_t SPEC_ACK_KIND_BATTLE  = 2;
constexpr uint8_t SPEC_ACK_KIND_MASK    = 0x03;
constexpr uint8_t SPEC_ACK_LIVE_REFRESH = 0x04;
// Bit 3 -- BOUNDED DEEP JOIN (Wave 4). Set only on a GRANT, only for a
// subscriber the host decided (at pin time, from the same single read the kind
// comes from) to serve with a bounded backfill anchored at the current
// char-select. It tells the viewer that it is skipping prior matches'
// SIMULATION, so it must HOLD at battle entry for the host's battle savestate
// instead of entering from scratch. The live-refresh builder never sets it, so
// no broadcast can arm a hold on a viewer that is bit-exact by simulation.
constexpr uint8_t SPEC_ACK_DEEP_JOIN    = 0x08;

// Failover timing.
constexpr uint64_t SPECTATOR_HEARTBEAT_INTERVAL_MS = 1000;   // 1 s outbound
// 15s: with UDP datagrams + heartbeat echoes as the liveness clock,
// silence this long means the host is GONE or frozen solid -- and a
// re-join against a frozen host only churns (the 5s budget flipped the
// overlay to "connecting" during the players' own rollback stalls).
constexpr uint64_t SPECTATOR_SILENCE_FAILOVER_MS   = 15000;  // 15 s with no
                                                             // inbound from
                                                             // current
                                                             // upstream → fall
                                                             // back to root
constexpr uint64_t SPECTATOR_RECONNECT_BACKOFF_MS  = 2000;   // throttle JOIN_REQ
// Cap for the exponential reconnect backoff (base doubled per consecutive
// failure). A genuinely-gone host is retried at most this often, so the
// spectator stops storming it while it waits out the host-gone watchdog.
constexpr uint64_t SPECTATOR_RECONNECT_MAX_BACKOFF_MS = 4000;
// RC full-transport handshake watchdog (task #70). Once JOIN_ACK lands,
// subscribed_upstream latches true and the reconnect loop shuts off; if the
// host's one-shot OP_BASELINE+snapshot+backfill burst is then lost beyond RC's
// 7s message retirement, nothing re-JOINs (silence-failover + op-gap need a TCP
// recv stamp that's 0 in RC mode; gap-fill needs have_frame_baseline) and the
// viewer sits subscribed-but-never-admitted until the 30s process-exit. Re-JOIN
// after this long with no admit to force the host to reset bind state and
// re-ship. > the 7s RC retirement (don't reset an in-flight transfer) and > the
// host's 3s reset-suppression (so the re-JOIN actually re-ships).
constexpr uint64_t SPECTATOR_NOADMIT_REJOIN_MS = 8000;
// FIRST no-admit recovery attempt. 8s is most of the 30s connect deadline and
// a viewer stares at nothing for the whole interval, so the first retry fires
// fast; only REPEATS fall back to the 8s cadence above (by then we know one
// fast retry did not fix it, and the RC-retransmit-window rationale applies).
// 3500ms is picked to clear the host's 3s destructive-reset floor
// (spec_join.cpp HandleJoinReq) with margin, so the re-JOIN always provokes a
// real re-ship instead of a bare re-ACK -- same reason SPECTATOR_KICK_JOIN_
// INTERVAL_MS below uses it. Each fast retry stays cheap because the host-side
// snapshot re-ship floor turns it into a backfill-only burst.
constexpr uint64_t SPECTATOR_NOADMIT_FIRST_REJOIN_MS = 3500;
// /F-dispatch-hold join kick cadence (SpectatorNode_KickJoin). MUST stay above
// the host's 3s destructive-reset floor: at the old 1Hz the viewer re-JOINed
// three times per host re-ship window, so a JOIN_ACK that finally landed could
// arrive on a SUPPRESSED re-ship (re-ACK only, no data) -- half of the
// stash-destruction race that produced the total=0 freeze. The other half is
// the address-gated pre_sub_stash clear in SpectatorNode_RequestJoin; either
// fix alone leaves the race open.
constexpr uint64_t SPECTATOR_KICK_JOIN_INTERVAL_MS = 3500;
// Host-side snapshot re-ship floor. A destructive reset re-runs the bind path,
// which used to re-ship the FULL snapshot (tens of KB when the zero-RLE wins,
// ~1MB when it does not) on every single re-JOIN -- congestion on the link
// whose loss caused the
// re-JOIN, and the viewer discards a re-join snapshot anyway once it has
// started. The backfill (the ONLY thing besides an applied snapshot that can
// set have_frame_baseline) still ships on every rebind. Above the 3500ms
// re-JOIN cadences so the fast retry is cheap, below the 8s repeat cadence so
// a genuinely-lost snapshot is always re-shipped on the second attempt.
constexpr uint64_t SPECTATOR_SNAPSHOT_RESHIP_MS = 6000;
// Surgical gap-fill (mid-match snapshot join under loss). When the
// admission cursor stalls behind buffered future batches for this long,
// pull the missing range over the reliable conn. Kept small so the gap
// heals in well under a second -- far below the 15s silence-failover that
// would otherwise tear down and loop on a full re-snapshot. Throttle
// bounds the pull rate while the host re-ships the (idempotent) range.
constexpr uint64_t SPECTATOR_GAP_FILL_STALL_MS     = 250;    // stall before pull
constexpr uint64_t SPECTATOR_GAP_FILL_THROTTLE_MS  = 500;    // min between pulls
// HOST-side floor + window for answering those pulls (candidate A1). The two
// constants above are the VIEWER's cadence, and until now they were the only
// thing bounding the host's work: every pull re-encoded [cursor .. live edge)
// with no floor of its own, on the worker thread under g_poll_mutex, and the
// trigger condition IS a stuck cursor -- so the re-shipped range grew by 100
// events/s for as long as the stall lasted, on a session_events vector that is
// never trimmed. Two bounds, both necessary:
//   * a floor, so a 500 ms viewer cadence cannot set the host's work rate; and
//   * a window, so the cost per pull is O(window) instead of O(session).
// The window still ADVANCES every pull -- the viewer admits what it gets, its
// cursor moves, and its next pull asks from there -- so a legitimately-behind
// viewer closes the gap at ~2000 events/s against ~100 events/s of live
// production. It is bounded work per pull, not a bounded total.
// Distinct from SPECTATOR_SNAPSHOT_RESHIP_MS above: that floors SNAPSHOT
// re-ships from the bind path; this floors EVENT re-ships from the gap-fill
// path, which the bind path never reaches.
constexpr uint64_t SPECTATOR_GAP_FILL_HOST_FLOOR_MS = 1000;
constexpr size_t   SPECTATOR_GAP_FILL_MAX_EVENTS    = 2048;
// ---- mid-stream starve escalation ------------------------------------------
// The gap-fill pull is SURGICAL and cannot heal a TRANSPORT wedge: the host
// answers it over the same reliable endpoint, explicitly without resetting
// that endpoint's state ("live conn -- reliable re-ship, no reset"). When the
// RC ordered stream head-of-line-blocks, every pull is correctly anchored,
// correctly ranged, correctly answered -- and delivers nothing. So bound it:
// after this many consecutive pulls with ZERO cursor movement, escalate to the
// resume-suppressed re-JOIN that forces the host's destructive-reset branch AND
// resets the RC endpoint on both sides.
constexpr uint32_t SPECTATOR_STARVE_PULLS_BEFORE_ESCALATE = 3;
// Cap on [CSS-STARVE] episode-end lines per session. Measurement keeps running
// past it; see State::css_window_starve_logged.
constexpr uint32_t SPECTATOR_CSS_STARVE_LOG_MAX = 32;
// ...but never before the RC layer has had its own go. RC_ORDERED_STALL_SEC is
// 3.5s (reliable_channel_internal.h), and its repair is strictly cheaper than a
// re-JOIN, so the ladder is ORDERED by making the escalation wait out that
// horizon. Measured on MsSinceLastAdmit -- the same clock the host-gone
// watchdog uses -- so the rungs are directly comparable:
//   3.5s RC ordered-stall repair | 4.0s escalation #1 | 8.0s escalation #2
//   | 12.0s host-gone give-up (FM2K_SPEC_HOST_GONE_MS default)
// ...and, when the transport reports active repair, escalation #1 slides to at
// most SPECTATOR_STARVE_ESCALATE_HARD_MS (7.5s) with #2 behind it at 11.5s.
// Both layouts fit the 12000ms budget; see the derivation on HARD_MS below.
constexpr uint64_t SPECTATOR_STARVE_ESCALATE_MIN_STALL_MS = 4000;
// Floor between escalations. Must clear BOTH the host's 3s destructive-reset
// suppression (spec_join.cpp) and the 3.5s re-JOIN cadences (KICK_JOIN /
// NOADMIT_FIRST), or an escalation lands on a bare re-ACK and re-ships nothing.
constexpr uint64_t SPECTATOR_STARVE_ESCALATE_FLOOR_MS     = 4000;
// ...and never wait PAST the point the transport itself gives up. While RC
// reports active ordered repair (the sender is provably still holding and
// retransmitting our hole) escalation is deferred, because escalation destroys
// the endpoint and with it the very retention that repair depends on. This is
// the cap on that deferral.
//
// THE NUMBER IS DERIVED, NOT PICKED. It was 9000 -- chosen to mirror the
// transport's own RC_ORDERED_STALL_HARD_SEC -- and adversarial review
// (2026-08-16, D3) showed that spent escalation #2 entirely at the shipping
// budget. The arithmetic, all on SpectatorNode_MsSinceLastAdmit():
//
//   escalation #1 fires at min(deferral cap, ...) once pulls are exhausted
//   escalation #2 fires no earlier than #1 + SPECTATOR_STARVE_ESCALATE_FLOOR_MS
//   the viewer exits at FM2K_SPEC_HOST_GONE_MS, default 12000
//     (trampoline_spectator.cpp -- 5000 is a HARNESS-only override)
//
//   old: 9000 + 4000 = 13000 > 12000  -> escalation #2 UNREACHABLE for any
//        viewer that took the deferral path. Pre-fix, #1 at 4000 left 8s and
//        both rungs fit; the deferral silently traded the second rung away.
//   now: cap <= 12000 - 4000 = 8000 is the hard requirement. 7500 is taken
//        rather than 8000 so #2 lands at 11500 with 500ms of margin before the
//        give-up rather than landing on the same tick as it (the ladder is
//        sampled from RunSpectatorTick at ~100Hz, so a tie is a coin flip --
//        the same mistake RC_ORDERED_STALL_SEC's own history records).
//
// CONSEQUENCE, recorded honestly: the app now releases its deferral 1500ms
// BEFORE the transport's RC_ORDERED_STALL_HARD_SEC (9.0s) backstop, so the two
// numbers are no longer equal. Between 7.5s and 9.0s a still-deferring RC
// endpoint can be destroyed by escalation #1, discarding retained content --
// exactly what S3 exists to avoid, now confined to a 1.5s window instead of
// starting at 4.0s. That trade is deliberate: at 7.5s of zero cursor movement
// the viewer has 4.5s of budget left, and one escalation that can still be
// followed by a second beats 1.5s more of waiting followed by none. RC's own
// backstop cannot simply be lowered to match: it must stay clear of the
// SENDER's RC_RETIRE_SEC (7.0s) by enough that a retire-then-advertise round
// trip repairs on a FACT instead of on the backstop.
constexpr uint64_t SPECTATOR_STARVE_ESCALATE_HARD_MS      = 7500;
// SPECTATOR_HOST_GONE_DEFAULT_MS (12000) lives in spectator_node.h so the
// viewer's watchdog and these derived rungs read one definition. The budget
// arithmetic above is therefore checked by the compiler, not by a comment.
static_assert(SPECTATOR_STARVE_ESCALATE_HARD_MS +
                  SPECTATOR_STARVE_ESCALATE_FLOOR_MS <
              SPECTATOR_HOST_GONE_DEFAULT_MS,
              "starve escalation #2 must land strictly inside the host-gone "
              "budget: deferral cap + escalation floor < budget");
static_assert(SPECTATOR_STARVE_ESCALATE_HARD_MS >
                  SPECTATOR_STARVE_ESCALATE_MIN_STALL_MS,
              "the deferral cap must sit above escalation #1's own floor, or "
              "the deferral can never happen");
constexpr uint64_t SPECTATOR_SUBSCRIBER_EXPIRY_MS  = 30000;  // upstream-side
                                                             // sweep: drop
                                                             // subscribers
                                                             // silent >30 s

// Broadcast cadence: emit a batch every N newly-confirmed frames.
constexpr size_t BROADCAST_BATCH_FRAMES = 8;

// Redundancy window: each batch carries the last N confirmed frames (not
// just the BROADCAST_BATCH_FRAMES new ones). This means each frame appears
// in floor(REDUNDANCY_WINDOW / BROADCAST_BATCH_FRAMES) consecutive batches,
// so a single dropped UDP packet doesn't lose any inputs -- the next batch
// re-delivers them. With WINDOW=32 and BATCH=8, each frame ships in 4
// batches: tolerates up to 3 consecutive packet losses before any data is
// genuinely lost.
//
// Wire cost: ~128 B / batch payload (32 frames × 4 B) + 10 B header + UDP
// overhead ≈ 180 B / packet. At 12.5 batches/sec that's ~2.3 KB/s per
// subscriber -- still nothing.
constexpr size_t REDUNDANCY_WINDOW = 32;

// Subscriber entry from the host's perspective.
struct Subscriber {
    sockaddr_in  addr;
    uint64_t     last_seen_ms;
    uint32_t     ack_frame;    // RESERVED for per-subscriber flow control. NOT wired:
                               // only ever set to 0, never read/gated. Do NOT make a
                               // backfill/pacing decision off this until a SPEC_ACK
                               // message actually populates it (would let the host see
                               // how far behind each subscriber is -- a real future win).
    bool         tcp_bound;    // True once SpectatorTCP::RegisterAcceptedClient(addr)
                               // has paired this sub with an accepted TCP socket.
                               // Until then, INITIAL_MATCH + backfill are deferred --
                               // any send before binding silently drops on the floor.
    SpecJoinMode join_mode;    // Backfill preference declared in SPEC_JOIN_REQ.
                               // Phase 1: stored only; phase 3 branches on it
                               // (CURRENT_MATCH ships snapshot+tail instead of
                               // SendSessionBackfillTo from frame 0).
    // spec_user_id -- hub's identifier for this spectator. Populated in
    // relay mode (Phase 2c) by parsing spec_incoming's spec_user_id
    // field via shared-mem. In TCP mode this stays empty; addressing
    // uses `addr` instead. Phase 2b just declares the field so the
    // Subscriber shape is forward-compatible.
    std::string  spec_user_id;
    // Phase F: viewer advertised SPEC_JOIN_UDP_OK in its JOIN_REQ
    // reserved bits. Gates BOTH UDP_INPUT_BATCH datagrams and the
    // TCP-borne OP_BASELINE (an old build's framer drops the connection
    // on an unknown SpecDataType).
    bool         udp_ok = false;
    // Light re-join (SPEC_JOIN_RESUME): the viewer's next_expected_frame
    // from its JOIN_REQ. Non-zero = the viewer is mid-stream; the bind
    // path skips the snapshot and backfills exactly the gap from here.
    uint32_t     resume_frame = 0;
    // Destructive-reset rate limit (task #55): a starved viewer retries
    // JOIN_REQ every 500ms; in RC mode each retry used to reset bind
    // state and re-blast the FULL backfill (observed: 3974 events every
    // 500ms while the host was wedged). Full resets are floored to one
    // per 3s -- a genuinely restarted viewer still recovers within 3s,
    // a retry storm degrades to background noise.
    uint64_t     last_reset_ms = 0;
    // Snapshot re-ship floor. Every destructive reset re-runs the bind path,
    // which used to re-ship the FULL snapshot on every re-JOIN -- tens of KB
    // when the zero-RLE wins, up to ~1MB when it does not, onto the exact link
    // whose loss provoked the re-JOIN, and the viewer discards a re-join
    // snapshot anyway once it has started playing back. Floored to one per
    // SPECTATOR_SNAPSHOT_RESHIP_MS; the backfill still ships on every rebind
    // because it is the only thing besides an applied snapshot that can set
    // the viewer's have_frame_baseline. Keyed by match so a NEW match's
    // snapshot -- genuinely different state the viewer has never seen -- is
    // never withheld. 0 / 0xFFFFFFFF = nothing shipped yet.
    //
    // snapshot_reship_skips bounds the damage when the throttle guesses
    // wrong. The host cannot tell "the viewer lost my backfill" from "the
    // viewer lost my snapshot" -- a viewer with no baseline sends
    // resume_frame=0 either way -- and withholding a snapshot the viewer
    // really is missing lets its bounded download-window hold expire into the
    // from-scratch battle path (a silent desync, strictly worse than the
    // freeze this is fixing). So: never suppress twice in a row. Reset to 0
    // on every actual ship.
    uint64_t     last_snapshot_ship_ms   = 0;
    uint32_t     last_snapshot_match_idx = 0xFFFFFFFFu;
    uint8_t      snapshot_reship_skips   = 0;
    // The host_session_kind advertised to THIS subscriber, pinned in the same
    // block as join_mode from a single Netplay_GetSessionKind() read. Every
    // ACK to this sub repeats it, so a host that crosses CSS -> battle between
    // the accept and a later re-ACK can never tell a FULL_SESSION-granted
    // viewer "BATTLE" -- which made it /F-boot and then replay the
    // from-frame-0 CSS stream as battle input from uninitialised state
    // (deterministic ~205-frame desync, 2/16 at 20% loss). Changes ONLY when
    // the host deliberately re-pins join_mode. SPEC_ACK_KIND_* values.
    uint8_t      pinned_ack_kind         = SPEC_ACK_KIND_NONE;

    // ─── Wave 4: bounded deep-join snapshot push ────────────────────────
    // deep_join_eligible -- this sub was granted the bounded deep-join path
    // (CURRENT_MATCH, a NON-battle grant, DeepJoinEnabled() -- on unless the
    // kill-switch is thrown -- and a CSS anchor). Decided ONCE at pin time in
    // the same block as join_mode and pinned_ack_kind, so the grant the viewer
    // boots on and the payload the bind ships can never disagree -- the bind
    // KEYS on this field instead of re-deriving it later against moved state.
    //
    // Sticky until the viewer reports SPEC_SNAPREQ_DONE. That is what makes the
    // per-battle re-arm work: a bounded joiner whose FIRST battle ended before
    // its snapshot landed is still eligible, so the next StashSnapshot arms it
    // again and the next battle entry serves it without any new handshake.
    bool         deep_join_eligible      = false;
    // Armed at the bounded bind (only when the host is ALREADY in the battle
    // this viewer is about to walk into) and re-armed by StashSnapshot at every
    // battle entry while eligible. Drained one sub per maintenance tick, the
    // same pacing the bind loop uses.
    bool         deep_join_pending       = false;
    // current_snapshot.input_frame of the last blob pushed to this sub on the
    // deep-join path. Dedups repeat pushes within one battle and makes a stale
    // arm (cache unchanged since the last push) a no-op instead of a re-ship.
    uint32_t     deep_join_push_anchor   = 0xFFFFFFFFu;
    // SPEC_SNAPSHOT_REQ response floor -- one reply per sub per
    // SPECTATOR_DEEPJOIN_REPLY_FLOOR_MS, so a starved viewer's 1.5 s request
    // cadence can never turn into a host-side re-ship storm.
    uint64_t     deep_join_last_reply_ms = 0;

    // Gap-fill EVENT re-ship floor (candidate A1). Last time this sub was
    // actually answered on the surgical gap-fill path, and how many pulls
    // have been suppressed since. The count is REPORTED on the next real
    // ship rather than logged per suppression -- hook-side logging is a
    // synchronous fputs+fflush on the calling thread, so a per-pull line on
    // a path that fires twice a second is itself a frame-thread cost.
    uint64_t     last_gapfill_ship_ms   = 0;
    uint32_t     gapfill_suppressed     = 0;
};

// Cached initial-match metadata so new joiners get a consistent handoff.
struct InitialMatch {
    bool     valid;
    uint8_t  header_bytes[96];  // Serialized ReplayHeader
};

// Side table of MATCH_START headers (96 bytes each) referenced by index from
// SessionEvent::u.match_start_idx. Out-of-band so SessionEvent stays at 5 B
// in memory regardless of match boundary frequency.
using MatchHeader = std::array<uint8_t, SESSION_EVENT_MATCH_HDR_SIZE>;

struct State {
    bool                      broadcasting      = false;
    size_t                    capacity          = SPECTATOR_DEFAULT_CAPACITY;
    std::vector<Subscriber>   subscribers;
    InitialMatch              initial_match     = {};
    // spec_transport_relay -- Phase 2b. True when FM2K_SPEC_TRANSPORT=relay
    // was set at SpectatorNode_Init. In relay mode the TCP listener +
    // PerformTcpStun are skipped entirely; spec data flows out via the
    // shared-mem ring below instead.
    bool                      spec_transport_relay = false;
    // Outbound spec-relay ring (Phase 2c). Non-null only when relay mode
    // is active. Hook produces (Enqueue) when shipping spec data; the
    // launcher process (which opened the same kernel mapping by our
    // pid) drains and forwards each Slot as a WS binary frame to the
    // hub. Layout + ordering documented in spec_relay_queue.h.
    fm2k::spec_relay::Ring*   spec_relay_out       = nullptr;
    // Inbound spec-relay ring (Phase 3). Launcher writes WS binary
    // frames forwarded from hub here; hook drains in TickHealth and
    // dispatches each Slot's payload through the existing
    // SpectatorNode_HandleSpecData path (same handler the TCP-arrived
    // bytes feed). Only meaningful when this process is acting as a
    // spec; harmless empty ring otherwise.
    fm2k::spec_relay::Ring*   spec_relay_in        = nullptr;
    // Pending (ip:port) -> spec_user_id from spec_incoming events that
    // arrived before the matching SPEC_JOIN_REQ. Populated by the punch-
    // target poll in TickHostMaintenance; consumed (and erased) by
    // HandleJoinReq's new-subscriber branch which assigns the user_id
    // onto the Subscriber. Entries that never get a matching JOIN_REQ
    // age out passively when the next punch for the same addr
    // overwrites them.
    std::unordered_map<std::string, std::string> pending_spec_user_ids;

    // ─── HOST SIDE: session event log ──────────────────────────────────
    // Every confirmed event the host produces (INPUT pairs in C2; PIN_RNG
    // / RESET_INPUT_STATE / SOUND_INIT / MATCH_START / MATCH_END /
    // FINGERPRINT in C3+) is appended monotonically here. Late joiners get
    // the whole vector replayed via SendSessionBackfillTo. Memory: 5 B/event
    // → ~1.7 MB for an hour of 100 Hz INPUT events; non-INPUT events are
    // sparse (a few per match).
    std::vector<SessionEvent> session_events;
    std::vector<MatchHeader>  match_headers;     // side table for MATCH_START
    uint32_t                  total_input_count   = 0;  // INPUT events in session_events
    uint32_t                  session_start_frame = 0;  // INPUT-frame index of session_events[0]
    // Deep-join anchor (Design 2). Index into session_events of the most
    // recent CSS_ENTERED op, and the INPUT-frame the next INPUT after it
    // carries. Lets a between-matches joiner be backfilled from the CURRENT
    // char-select instead of from frame 0: a viewer joining an ongoing set
    // used to replay every prior CSS phase and every prior battle, which is
    // ~a minute of fast-forward dominated by cold .player loads.
    //
    // CSS_ENTERED specifically, not "the most recent boundary of either kind":
    // it is the only anchor that guarantees the mirror starts at CSS frame 0,
    // which the seam machinery assumes. Anchoring on a bare MATCH_END would
    // start a fresh viewer on a results screen it has no MATCH_START for.
    // Maintained O(1) in AppendOpAndFlush; no log trimming in this wave, so
    // these indices stay valid for the life of the session.
    bool                      have_css_anchor         = false;
    size_t                    css_anchor_event_idx    = 0;
    uint32_t                  css_anchor_input_frame  = 0;
    // Op prefix at css_anchor_event_idx: the number of non-INPUT events
    // STRICTLY BELOW that index. Stamped from the same O(1) choke point as
    // the two above (AppendOpAndFlush, captured BEFORE its ++total_op_count
    // and before the push_back, so it counts exactly the ops already in the
    // vector). This is the third leg that lets the backfill senders resume
    // BOTH of their linear scans -- the event scan and the op-cursor prefix
    // scan -- from the anchor instead of from index 0 (candidate A1).
    uint32_t                  css_anchor_op_count     = 0;
    // Gate for the above: only bound the backfill once a match has actually
    // ENDED. Before that the session is still its first char-select plus its
    // first battle -- there is no prior match to skip, and anchoring on the
    // first CSS_ENTERED would drop whatever ops precede it (PIN_RNG and
    // friends). A fresh session-start join therefore keeps taking the
    // from-frame-0 path byte for byte.
    bool                      have_prior_match        = false;
                                                         // (always 0 unless we ever drop history)

    // Pending broadcast cursor: events past this index are unflushed.
    // FlushBatch encodes [last_flushed_event_idx .. session_events.size()),
    // emits as one EVENT_BATCH datagram, advances the cursor.
    size_t                    last_flushed_event_idx = 0;
    uint32_t                  flushed_input_count    = 0;  // INPUT events flushed so far

    // ─── HOST SIDE: UDP input accelerator (Phase F) ─────────────────────
    // Ring of the most recent confirmed (p1,p2) pairs, indexed by
    // session-relative INPUT-frame % SPEC_UDP_WINDOW. Maintained by
    // OnFrameConfirmed; read by SendUdpInputBatches every
    // SPEC_UDP_SEND_INTERVAL confirmed frames. total_op_count is the
    // running count of non-INPUT events appended (AppendOpAndFlush is
    // the single op choke point) -- shipped as op_seq in every datagram
    // so the spectator's admission gate can order inputs after ops.
    uint16_t                  udp_ring_p1[SPEC_UDP_WINDOW] = {};
    uint16_t                  udp_ring_p2[SPEC_UDP_WINDOW] = {};
    uint32_t                  total_op_count         = 0;
    // Redundant ops tail: the last few non-INPUT events, pre-encoded, so
    // UDP datagrams can deliver boundary ops when the TCP stream is dead
    // (this box's loopback kills established TCP connections at will --
    // both ends see "forcibly closed"; inputs already survive via the
    // accelerator, ops were the remaining hostage).
    struct OpWire {
        uint32_t op_index;
        uint32_t input_pos;   // total_input_count at append = inputs before this op
        uint8_t  len;
        uint8_t  bytes[100];
    };
    static constexpr size_t   OPS_RING = 8;
    OpWire                    udp_ops_ring[OPS_RING] = {};

    // Index of the most recent MATCH_START event in session_events. Used by
    // SpectatorNode_WriteCurrentBattleFile to slice the per-battle segment.
    // -1 sentinel = no MATCH_START emitted in this session yet.
    int64_t                   last_match_start_idx   = -1;

    // Index of the FIRST state-init event in the contiguous init block
    // immediately preceding the most recent MATCH_START. Set in
    // SpectatorNode_AppendMatchStart by backward-scanning from
    // last_match_start_idx through PIN_RNG / RESET_INPUT_STATE /
    // SOUND_INIT / SESSION_ID events until a non-init event is hit.
    // WriteCurrentBattleFile slices from THIS index instead of
    // last_match_start_idx so the .fm2krep file is fully self-replayable
    // (RNG seed re-pin, input ring reset, sound layer init all included).
    // Defaults to last_match_start_idx if no init block precedes it.
    int64_t                   last_pre_match_init_idx = -1;

    // Snapshot cache (task #18 phase 2). Refreshed by StashSnapshot at
    // every Netplay_StartBattle. When a spectator joins with mode=
    // CURRENT_MATCH, the host's TickHostMaintenance bind path will (in
    // phase 3) send this blob via SNAPSHOT_BEGIN/CHUNK/END instead of
    // calling SendSessionBackfillTo from frame 0. Spectator's
    // SaveState_Load on receipt skips every prior match.
    //
    // Empty / invalid before the first match starts. New session_events
    // accumulating during pre-battle CSS don't disturb the previous
    // snapshot -- it just goes stale until the next StashSnapshot
    // overwrites it.
    struct SnapshotCache {
        std::vector<uint8_t> blob;
        // Pre-compressed WIRE form of `blob`, built ONCE at capture
        // (StashSnapshot) instead of once per recipient inside
        // SendSnapshotTo. The old per-recipient ZeroRleCompress ran over
        // ~1MB on the host main loop while holding g_poll_mutex, so the
        // host hitch scaled with viewer count and with every re-JOIN
        // re-ship. wire_flags carries SNAPSHOT_FLAG_ZERO_RLE iff the
        // compressed form actually won; when it did not, wire_blob is a
        // copy of the raw blob so the ship path stays branch-free.
        std::vector<uint8_t> wire_blob;
        uint16_t             wire_flags         = 0;
        uint32_t             input_frame        = 0;
        uint32_t             match_index        = 0;
        uint32_t             checksum           = 0;
        // captured_game_mode: g_game_mode at the moment we captured this
        // snapshot (2000 = CSS, 3000 = battle). Phase E uses this to gate
        // the spec-side apply on a matching mode so a CSS snapshot doesn't
        // get applied during the spec's battle phase (wrong state regions
        // would dominate). 0 means "legacy battle-only capture" -- apply
        // when spec reaches game_mode >= 3000.
        uint32_t             captured_game_mode = 0;
        bool                 valid              = false;
    } current_snapshot;

    // ─── VIEWER SIDE: playback queue ───────────────────────────────────
    // Populated by HandleSpecData (decodes incoming EVENT_BATCH /
    // INPUT_BATCH); consumed by RunSpectatorTick. Mirrors the host's
    // session_events shape: typed events drained in order, non-INPUT
    // events at the head are applied (RNG pin etc.) before the next
    // INPUT pops. C2 keeps non-INPUT drain as a no-op gate; C3+ wires
    // the apply paths.
    std::vector<SessionEvent> pb_queue;
    std::vector<MatchHeader>  pb_match_headers;

    // Snapshot reassembly buffer (task #18 phase 4). Filled across
    // SNAPSHOT_BEGIN → SNAPSHOT_CHUNK*N → SNAPSHOT_END from the upstream's
    // CURRENT_MATCH bind path. Applied once via SaveState_LoadFromBytes
    // when END validates; cleared on apply or on protocol error.
    //
    // anchor_frame = SNAPSHOT_BEGIN's start_frame, replayed into
    // next_expected_frame at apply time so the EVENT_BATCH stream that
    // follows resumes exactly where the host stashed the snapshot.
    struct SnapshotInbox {
        std::vector<uint8_t> blob;
        SnapshotMetadata     meta            = {};
        uint32_t             anchor_frame    = 0;
        // UNIQUE covered bytes, derived from `covered` below. NOT a running
        // sum of chunk lengths: the old counter added `n` unconditionally on
        // every chunk, so a re-shipped chunk (new RC msg_seq, same offset --
        // RC only dedups within one seq space) DOUBLE-COUNTED and the exact
        // `== wire_bytes` finalize test could then never pass. The transfer
        // wedged forever, logging "END seen but inbox incomplete" at 1Hz with
        // N sometimes GREATER than M.
        size_t               bytes_received  = 0;
        // Coverage set: merged, non-overlapping [start,end) byte ranges that
        // have actually landed in `blob`. Idempotent by construction -- a
        // duplicate or partially-overlapping chunk contributes only the bytes
        // it newly covers. Completion is "one interval spanning [0,wire)",
        // which cannot be faked by overshoot. Chunk-size agnostic, so a
        // re-ship that used a different chunking still composes.
        fm2k::specsnap::CoverageSet covered;
        // Staleness tracking (there was NO timeout at all before: an
        // abandoned transfer sat `active` forever, blocking the BEGIN
        // restart path, holding ~1MB, and spamming the 1Hz gap warning with
        // no way to tell a dead transfer from a slow one).
        uint64_t             started_ms      = 0;
        uint64_t             last_progress_ms = 0;
        bool                 active          = false;
        // Set by SNAPSHOT_END once the blob has been validated (size +
        // fletcher32) but the SaveState_LoadFromBytes call is held back
        // because the local game hasn't booted past mode 0 yet.
        // SpectatorNode_ApplyPendingSnapshot polls each spec tick, sees
        // game_mode > 0, and runs the actual apply. Without this gate the
        // raw memcpy lands during the engine's pre-WinMain init and the
        // next render tick reads from corrupted state → crash. Observed
        // on Vanguard Princess (1.08 MB snapshots arrived 12ms after
        // JOIN_ACK, before mode flipped 0→1000).
        bool                 pending_apply   = false;
        // Unordered blob channel (RC pure-UDP): SNAPSHOT_END may arrive before all
        // chunks, and chunks may arrive before BEGIN. Validate when bytes_received
        // == wire size AND end seen (not on END arrival). Chunks before BEGIN are
        // held here and replayed once BEGIN allocates the blob.
        bool                 end_seen        = false;
        uint32_t             end_checksum    = 0;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pending_chunks;  // (offset, data)
        size_t               pending_bytes   = 0;   // sum of held pre-BEGIN chunk sizes (memory bound)
        // Pre-BEGIN END stash (task #55 root #3): on the unordered blob
        // channel END can beat BEGIN, and BEGIN's init used to WIPE
        // end_seen -- stranding the snapshot silently (no finalize, no gap
        // warn). BEGIN replays this like the chunk stash.
        bool                 pending_end_valid    = false;
        uint32_t             pending_end_checksum = 0;
    } pb_snapshot_inbox;

    // Viewer-side state (this node subscribed upstream).
    bool                      subscribed_upstream = false;
    // Latched true the first time subscribed_upstream is set (JOIN_ACK landed).
    // Stays true even after the no-admit watchdog re-JOINs (which clears
    // subscribed_upstream). Lets the reconnect loop tell "never connected yet"
    // (fast retry to establish) from "was subscribed but the handshake burst
    // was lost" (slow retry so each re-ship gets a full RC retransmit window
    // before the next re-JOIN restarts it). Reset at teardown. (task #70)
    bool                      ever_subscribed = false;
    // Pre-subscribe RC stash (task #55): the host binds + streams the
    // backfill the instant it accepts a JOIN_REQ, so RC-delivered spec
    // data can beat our own JOIN_ACK processing by up to ~1s. The
    // EVENT_BATCH/OP_BASELINE handlers drop data while
    // !subscribed_upstream -- and RC has already acked it as delivered,
    // so a one-shot backfill was lost forever (viewer never admitted a
    // frame; 30s connect watchdog killed the process). Raw deliveries
    // land here instead and replay the moment the JOIN_ACK is processed.
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> pre_sub_stash;  // (chan, spec msg)
    size_t                    pre_sub_stash_bytes = 0;
    // Set when the upstream sends SPEC_SESSION_END (host exited cleanly). Stops
    // the reconnect machinery so we don't storm a dead host. Cleared on a fresh
    // manual RequestJoin / Netplay_InitAsSpectator.
    bool                      session_ended       = false;
    // Join-grace: false from JOIN until the first EVENT_BATCH applies.
    // The silence failover uses a 30s budget until then -- at real loss
    // rates the dial + 1MB snapshot + backfill exceed the steady-state
    // 5s budget and failover used to guillotine the join repeatedly.
    bool live_established = false;
    bool                      join_req_pending    = false;  // We sent SPEC_JOIN_REQ; expect ACK.
    // Set when a first-time JOIN_ACK granted BATTLE, i.e. we /F-booted into
    // battle on the promise of a snapshot. TickHealth clears it the moment any
    // snapshot byte shows up (or one applies) and otherwise raises the
    // grant-vs-stream contradiction: booted for a snapshot join, the stream
    // arrived, no snapshot ever did. That combination means the ACK we obeyed
    // did not describe the stream the host actually assigned us, and simulating
    // on is how a mismatched pairing turns into a silent desync.
    bool                      spec_boot_expects_snapshot = false;
    sockaddr_in               upstream_addr       = {};
    // Sticky copy of the mode we declared on the FIRST RequestJoin from
    // Netplay_StartSpectator. The reconnect path (silence failover) calls
    // RequestJoin(root) without specifying a mode, which would otherwise
    // fall through to the SpectatorNode_RequestJoin default (FULL_SESSION)
    // and clobber the original CURRENT_MATCH preference -- host then ships
    // no snapshot, spec sits with placeholder chars on a /F-booted battle.
    SpecJoinMode              last_requested_mode = SpecJoinMode::FULL_SESSION;
    // Failover support. root_addr is the originally-configured upstream
    // (the actual match host). If our current upstream goes silent we
    // fall back to root, which always-on by design. Liveness is tracked
    // via SpectatorTCP::LastUpstreamRecvMs() (TCP-side) -- this struct
    // owns only handshake / heartbeat send-cadence state.
    sockaddr_in               root_addr           = {};
    uint64_t                  last_heartbeat_send_ms = 0;
    uint64_t                  last_reconnect_attempt_ms = 0;
    // Consecutive failed reconnect attempts (JOIN_REQ fired, no admit since).
    // Drives exponential backoff so a genuinely-gone host isn't stormed with
    // a fixed-rate JOIN_REQ every 500ms for the whole host-gone window. Reset
    // to 0 the moment new content is admitted again (a real blip recovers on
    // the first quick retry, before the interval grows).
    uint32_t                  reconnect_fail_count = 0;
    // De-dup gate. Backfill from a reconnected upstream replays history
    // from frame 0; frames at or below this counter were already consumed
    // locally and would re-render the past. Updated by PopFrameInputs.
    uint32_t                  highest_consumed_frame = 0;
    // Frame-counter offset so highest_consumed_frame is comparable to
    // the start_frame field on incoming EVENT_BATCH datagrams. Set on the
    // first batch received after JOIN_ACK; subsequent batches increment.
    bool                      have_frame_baseline = false;
    uint32_t                  next_expected_frame = 0;  // session-relative INPUT-frame index
    // Cross-channel reorder buffer (ReliableChannel: snapshot/backfill on
    // RC_CHAN_SPEC_SNAPSHOT, live on RC_CHAN_SPEC -- independently ordered). Under
    // loss the two channels' ARRIVAL order != host-append order, so we buffer
    // out-of-order EVENT_BATCH payloads keyed by start_frame and drain them in
    // frame order -- restoring host-append order for both inputs AND ops. Empty
    // (no cost) on a single ordered stream (TCP). Bounded to avoid unbounded hold.
    struct ReorderBatch {
        int32_t op_base = -1;   // EVENT_BATCH2 absolute op base; -1 = legacy
        std::vector<uint8_t> bytes;
        // Did this batch arrive on an RC/UDP channel (rc_channel != 0)? Carried
        // across the reorder buffer so a batch drained LATER still stamps the
        // UDP-borne admit clock that the TCP-only delay-bank pre-arm reads --
        // otherwise a viewer whose live stream is entirely reorder-buffered
        // would look TCP-only and double its bank floor (spec_playback_state).
        bool udp_borne = false;
    };
    std::map<uint32_t, ReorderBatch> pb_reorder;
    // Pull-based gap recovery: periodic + on-demand INPUT_REQUEST
    // (spectator → upstream) for the missing frame range. Closes any
    // gap that the push-based redundancy window can't recover from.
    uint64_t                  last_input_request_send_ms = 0;

    // Surgical gap-fill (mid-match snapshot join under loss). On a
    // CURRENT_MATCH join the snapshot+backfill ride TCP while the live
    // stream rides RC; under loss the RC endpoint comes up some frames
    // AFTER the bind, so [backfill-end .. RC-live-start) is delivered by
    // neither channel. next_expected_frame then stalls behind buffered
    // pb_reorder batches that can never drain. These track the stall so
    // TickHealth can pull exactly that range over the reliable conn
    // (SPEC_JOIN_RESUME) before the silence-failover tears everything
    // down and loops on a full re-snapshot. gap_fill_stall_frame ==
    // 0xFFFFFFFF means "no gap currently observed".
    uint32_t                  gap_fill_stall_frame    = 0xFFFFFFFFu;
    uint64_t                  gap_fill_stall_since_ms = 0;
    uint64_t                  last_gap_fill_send_ms   = 0;

    // Viewer-side playback driver state.
    // Populated by HandleSpecData (INITIAL_MATCH / EVENT_BATCH / MATCH_END);
    // consumed by the trampoline's RunSpectatorTick + Hook_GetPlayerInput.
    bool                      playing_back        = false;
    uint32_t                  pb_initial_seed     = 0;
    uint32_t                  pb_initial_state    = 0;  // fingerprint sanity
    uint8_t                   pb_p1_char          = 0;
    uint8_t                   pb_p1_color         = 0;
    uint8_t                   pb_p2_char          = 0;
    uint8_t                   pb_p2_color         = 0;
    uint8_t                   pb_stage_id         = 0;
    uint16_t                  pb_current_p1       = 0;
    uint16_t                  pb_current_p2       = 0;
    // Session-scoped sim settings decoded from a LOADED FILE's 256-byte
    // header (SpectatorNode_LoadSessionFile). Deferred rather than applied at
    // load time: the loader runs from DLL_PROCESS_ATTACH, i.e. BEFORE WinMain,
    // so anything it writes into an engine global can be overwritten by the
    // engine's own game.ini load during startup. They are applied at the
    // MATCH_START drain instead -- the same late timing the round-timer
    // restore already uses, and the timing at which HOST_CONFIG's write to the
    // very same address demonstrably sticks.
    //
    // Only a file load ever sets these, so they stay 0 on a LIVE spectator
    // (which never loads a file and gets both settings over HOST_CONFIG). The
    // apply site is therefore replay-only BY CONSTRUCTION, not by env check.
    // 0 = the producer did not record it -> no write.
    uint32_t                  pb_file_game_speed_pct = 0;
    uint8_t                   pb_file_socd_plus1     = 0;
    // PIN_RNG seed deferred from event drain to battle-entry. The host
    // emitted PIN_RNG at MATCH_START (battle entry). Applying it on the
    // spec side at the first event drain (= title screen) would let the
    // pre-battle sim mutate the seed further, breaking rng parity with
    // host's battle frame 0. Stash it here; SpectatorSimOneFrame writes
    // it to engine RNG when game_mode flips to 3000.
    uint32_t                  pending_pin_rng_seed  = 0;
    bool                      pending_pin_rng_valid = false;
    // RESET_INPUT_STATE / SOUND_INIT deferred the same way, but only when
    // they arrive during a match-boundary seam (pb_boundary != NONE). In
    // lockstep replay (FULL_SESSION from frame 0) the immediate apply is
    // correct -- the spec consumes the same frames the host did. During a
    // seam the spec runs EXTRA local frames (results screens + CSS pin
    // walk) between the op's queue position and its own battle entry;
    // applying RESET early would let those frames write back into the
    // freshly-zeroed input history ring, leaving garbage at slots the
    // host has as zeros (the task-#34 ring-shift failure class).
    bool                      pending_reset_input   = false;
    bool                      pending_sound_init    = false;

    // ─── VIEWER SIDE: UDP input accelerator (Phase F) ───────────────────
    // ops_seen mirrors the host's total_op_count: incremented for EVERY
    // non-INPUT event decoded from the current TCP connection (exactness
    // is what matters -- the gate compares it against op_seq in incoming
    // datagrams). udp_epoch_armed gates admission per-connection: cleared
    // on every (re)join so a stale stream can't poison the count, re-armed
    // when the host's OP_BASELINE for the new connection arrives.
    uint32_t                  ops_seen              = 0;
    // Per-TCP-connection op cursor: the stream delivers ops in global
    // order starting at the OP_BASELINE, so conn_ops_baseline +
    // conn_ops_decoded = the global index of the next op this connection
    // will deliver. Ops the UDP tail already accepted (index < ops_seen)
    // are skipped as duplicates.
    uint32_t                  conn_ops_baseline     = 0;
    uint32_t                  conn_ops_decoded      = 0;
    // TCP died but the subscription lives on: UDP (inputs + ops tail)
    // keeps feeding pb_queue while TickHealth re-JOINs in the background
    // to restore the TCP side. Cleared when the new connection's
    // OP_BASELINE lands.
    bool                      tcp_rejoin_pending    = false;
    // Last validated UDP datagram from the upstream -- the PRIMARY
    // liveness signal. With inputs and ops riding UDP, a quiet TCP
    // means nothing while datagrams flow.
    uint64_t                  last_udp_recv_ms      = 0;
    // Viewer chose natural boot (pre-battle JOIN_ACK aborted /F): it
    // replays the session from frame 0 and can NEVER accept a snapshot
    // (battle-captured state into a title/CSS engine = phase-wait
    // deadlock: pending_apply blocks pops, pops are the only road to
    // the apply gate).
    bool                      natural_boot          = false;
    // Broadcast delay bank built once at CSS open: the mirror start is
    // held until the queue holds the full bank, so playback never rides
    // the live edge (riding the edge = every arrival gap stalls the
    // picture). Players are browsing during the hold -- invisible.
    bool                      pb_bank_built         = false;
    // True between MATCH_START apply and MATCH_END apply. When the local
    // game reaches CSS while this is still set, the local sim has outrun
    // the stream's results tail -- those queued inputs belong to results
    // screens we already finished and MUST NOT feed the fresh CSS (8
    // leaked frames displaced the cursors before the seam engaged; the
    // whole mirrored dance then ran offset = wrong chars AND colors at
    // the rematch, 2026-06-11 15:09).
    bool                      pb_awaiting_match_end = false;
    // True once the LOCAL game has been in battle mode (>=3000) since the
    // last MATCH_START apply. Distinguishes the rematch boundary (local
    // played the battle, returned to CSS early -> queued inputs are the
    // results tail, discard) from match ENTRY (local still at CSS while
    // the stream just started the battle -> queued inputs ARE the battle,
    // keep). Without it the results-tail guard ate the entire offline
    // replay at boot: .fm2krep starts with MATCH_START, the pin walk
    // reaches mode 2000, and all 3616 battle INPUTs were discarded as a
    // "results tail" (2026-06-11 16:24, replay parity 0 rows).
    bool                      pb_local_battle_seen  = false;
    bool                      udp_epoch_armed       = false;
    // Post-CSS-open confirm-mask countdown (lean seam): pops remaining
    // during which CssAutoConfirm's hold eats confirm bits, so the edge
    // echo of the last pre-CSS input can't lock the carried cursors.
    uint32_t                  pb_post_css_mask_pops = 0;
    // Seam mirror split marker: set when the seam stream's CSS_ENTERED
    // op applies. Before it, seam INPUTs are the host's results-screen
    // presses (discarded -- the viewer advances its own results on
    // synthetic edges); after it, seam INPUTs are the host's CSS dance
    // and mirror 1:1 from CSS frame 0 on both sides.
    bool                      pb_css_marker_seen = false;
    // True once a snapshot has applied this playback session. The
    // rewind-discard guard in ApplyPendingSnapshot is strictly for
    // RE-JOIN re-ships; the FIRST snapshot must always apply (the BTB
    // boot's local battle state is garbage until it does).
    bool                      pb_snapshot_applied_once = false;
    // True once the viewer has popped its first INPUT this session --
    // the real "we are mid-stream" signal. Snapshot applies must be
    // refused from then on: a naturally-joined viewer has
    // pb_snapshot_applied_once == false, so a re-join's re-shipped
    // snapshot passed the old guard as a "first apply" and rewound the
    // live sim to the match anchor (battle restarted, 2026-06-11).
    bool                      pb_started               = false;
    // Battle-entry alignment (live CSS-walk FULL_SESSION spectator). Set when
    // MATCH_START drains while the local engine is still in CSS (mode<3000) --
    // i.e. the host already entered battle but our (mask-delayed) CSS lock
    // hasn't fired. While set, PopFrameInputs HOLDS battle inputs at the queue
    // head (feeds neutral) and the armed CssAutoConfirm pin force-locks the
    // host's chars; cleared the frame the engine crosses into battle. This
    // anchors our battle-start to the host's MATCH_START session-frame instead
    // of our local lock timing, killing the CSS-overrun input offset.
    bool                      pb_battle_align_pending  = false;
    // Highest op_seq announced by any received UDP datagram. Drives the
    // silent-TCP-death detector in TickHealth: a persistent gap vs
    // ops_seen while TCP is quiet means the op stream is wedged.
    uint32_t                  udp_highest_op_seq    = 0;
    // [SPEC-UDP] diagnostics (rate-limited 1Hz log).
    uint32_t                  udp_admitted_total    = 0;
    uint32_t                  udp_paused_on_gate    = 0;
    uint32_t                  udp_dropped_on_gap    = 0;

    // C7 -- host's session_id for this peer connection. Generated lazily
    // (first AppendSessionId call) and stays stable until the SpectatorNode
    // is shut down or the next AppendSessionId overwrites it. The wire
    // event sources g_state.session_events; the file writer sources this
    // field; spectator-side application copies the wire payload here so
    // a spectator's own .fm2kset/.fm2krep recordings carry the same id
    // as the upstream's.
    uint64_t                  session_id           = 0;

    // Match-boundary playback state (A4 wrong-characters fix). Raw replay
    // of the host's seam frames (results screens + CSS traversal) cannot
    // align on the spectator: the spec's own 3000->2000 transition timing
    // is driven by ITS local result-screen processing, and a CURRENT_MATCH
    // joiner's CSS init state never matched the host's to begin with. So
    // a spectator that replayed CSS inputs raw locked whatever its
    // diverged cursor walk landed on -- match 2 started with the wrong
    // characters (observed 2026-06-11, A4 multi-match run). The robust
    // pattern is the one offline replay already uses: characters come
    // from MATCH_START + CssAutoConfirm, never from CSS input replay.
    //   NONE    -- normal battle playback; INPUT pops feed the sim 1:1.
    //   SEAM    -- between MATCH_END apply and MATCH_START apply: INPUT
    //              events at the queue head are consumed-and-DISCARDED
    //              (host seam frames are presentation-only), ops apply
    //              as they reach head, and the local sim runs on
    //              synthetic inputs (confirm-edge to leave the results
    //              screen, neutral at CSS).
    //   PINNING -- MATCH_START applied (CssAutoConfirm armed with the
    //              new picks): battle INPUTs stay AT the head while the
    //              local game walks CSS under the pin; pops resume the
    //              moment game_mode crosses 3000 so spec battle frame 0
    //              consumes host battle frame 0's input.
    enum class PbBoundary : uint8_t { NONE, SEAM, PINNING };
    PbBoundary                pb_boundary          = PbBoundary::NONE;
    // PINNING release requires a mode EDGE, not a level: the local game
    // must first LEAVE battle (results screen is still mode 3000 -- with
    // the UDP accelerator MATCH_START arrives ~2s after MATCH_END, long
    // before the local game walks off the old match's results screen) and
    // only then re-enter >= 3000. Releasing on the stale 3000 fed the new
    // match's inputs into the old results screen and let CssAutoConfirm
    // disengage before CSS even opened (wrong characters at UDP speed).
    bool                      pb_boundary_left_battle = false;

    // ─── Wave 4: bounded deep-join viewer state ─────────────────────────
    // spec_deep_join -- latched from a GRANT carrying SPEC_ACK_DEEP_JOIN. Same
    // "only a GRANT changes boot posture" rule that owns natural_boot and the
    // BTB seeding: the live-refresh builder never sets the bit, so no
    // broadcast can arm a hold. Deliberately NOT cleared by a later grant that
    // lacks the bit -- the escalation re-JOIN below lands while the host is in
    // battle and correctly comes back as a BATTLE grant, and that grant's
    // snapshot is exactly what releases the hold. Cleared only when a deep-join
    // snapshot APPLIES (this viewer is then bit-exact and is an ordinary
    // continuing viewer) or at teardown.
    bool                      spec_deep_join            = false;
    // Armed by PopFrameInputs the first tick the local engine is in battle with
    // this battle's snapshot still missing. While set, PopFrameInputs returns
    // false: the sim does not advance, nothing is popped, pb_started cannot
    // move, and the consumed cursor stays exactly on the snapshot's anchor.
    bool                      pb_deep_join_await        = false;
    uint32_t                  pb_deep_join_anchor       = 0;  // consumed pos at hold start
    uint64_t                  pb_deep_join_hold_since_ms = 0;
    uint64_t                  pb_deep_join_last_req_ms  = 0;
    size_t                    pb_deep_join_last_bytes   = 0;  // inbox progress watermark
    uint32_t                  pb_deep_join_reqs         = 0;
    uint32_t                  pb_deep_join_escalations  = 0;
    uint64_t                  pb_deep_join_scan_ms      = 0;  // 1 Hz MATCH_END scan
    // Set for exactly ONE RequestJoin so the escalation re-JOIN omits
    // SPEC_JOIN_RESUME. A resume-flagged JOIN_REQ makes the host take the
    // light-resume bind (gap backfill, explicitly NO snapshot), which is the
    // precise opposite of what an escalation needs -- it must reach the host's
    // DESTRUCTIVE-RESET branch, which is also where the host resets its RC
    // endpoint for us.
    //
    // SHARED (was pb_deep_join_force_full_rejoin, deep-join only). The
    // mid-stream STARVE escalation needs exactly the same one-shot: a viewer
    // whose gap-fill pulls are re-shipping over a wedged RC endpoint has to
    // stop asking for a re-ship and force the reset instead.
    bool                      spec_force_full_rejoin = false;

    // ─── Mid-stream starve escalation (the RC head-of-line wedge) ────────
    // Consecutive gap-fill pulls that produced ZERO cursor advancement. The
    // pull re-ships over the SAME reliable endpoint and explicitly does not
    // reset it ("live conn -- reliable re-ship, no reset"), so it cannot heal
    // a transport-level wedge no matter how many times it fires -- the
    // captures burned 4 and 8 of them against a host that was answering every
    // single one. Past SPECTATOR_STARVE_PULLS_BEFORE_ESCALATE the ladder stops
    // pulling and forces the reset. Reset to 0 the moment the cursor moves.
    uint32_t                  gap_fill_pulls_no_progress = 0;
    uint32_t                  gap_fill_pull_total        = 0;  // session total, for the exit message
    uint32_t                  starve_escalations         = 0;  // ditto
    uint64_t                  last_starve_escalate_ms    = 0;  // escalation floor
    // Times the escalation gate was open on its own clock but held back
    // because RC reported active ordered repair. Session total; a nonzero
    // value on an otherwise-clean run is the fix doing its job, not a fault.
    uint32_t                  starve_escalations_deferred = 0;
    // 1Hz throttle for the deferral line. Lives HERE, not in a function-local
    // static: this diff's own S1 half fixed exactly that antipattern for the RC
    // retire line (Endpoint::last_retire_log), where a process-wide static
    // silently muted one endpoint's warnings from another's clock. Harmless
    // today (one spectator node per process) and it stays harmless by not being
    // reintroduced.
    uint64_t                  last_starve_defer_log_ms   = 0;
    // Episode cap for the [CSS-STARVE] line. The predicate is sampled at ~100Hz
    // from RunSpectatorTick and one line is emitted per episode END, so a
    // predicate that flaps at batch cadence would emit at batch cadence -- into
    // a hook whose logging is SYNCHRONOUS. Measured 0-3 episodes per session
    // across four validation runs, so this cap never binds in practice; it is
    // there so a flapping predicate degrades to a bounded log instead of a
    // frame-rate bug. Counting continues after the cap; only printing stops.
    uint32_t                  css_window_starve_logged   = 0;
    // CSS-window starve measurement (pre-committed trigger for the
    // between-matches gap-fill rung). The whole starve ladder is gated to
    // battle mode, so a viewer starving at gm==2000 has NO rung at all and the
    // host-gone timer runs unopposed -- the shape the surviving R3b log shows.
    // We do not widen the gate on this evidence: a pull's JOIN_ACK carries the
    // host's CURRENT kind and a between-matches kind aborts the viewer's BTB.
    // We MEASURE instead, and the trigger is pre-committed: if any gated run
    // records a CSS-window starve episode longer than 3000ms, the
    // between-matches rung gets implemented.
    uint64_t                  css_window_starve_since_ms = 0;  // 0 = not starving
    uint32_t                  css_window_starve_episodes = 0;
    uint32_t                  css_window_starve_max_ms   = 0;
};

// The one definition lives in spectator_node.cpp; sibling TUs see it via this.
extern State g_state;

// Built in spec_join.cpp; also called by StashSnapshot (spectator_node.cpp) for
// the battle-entry JOIN_ACK re-broadcast. Unique name -> kept at global scope.
// LIVE REFRESH (host's current kind, SPEC_ACK_LIVE_REFRESH bit set): the
// battle-entry re-broadcast, plus the re-ACK paths that deliberately change
// nothing. Says where the host IS; can never complete a first-time handshake.
// GRANT (BuildJoinAckPacketFor, bit clear): what THIS subscriber was pinned
// to. Every send that pins, re-pins, or re-states a subscriber's join_mode
// must use it, so the viewer's boot decision follows its own grant and not
// whatever phase the host happened to reach in between.
CtrlPacket BuildJoinAckPacket();
CtrlPacket BuildJoinAckPacketFor(const Subscriber& sub);

// Internal cross-TU API for the spectator node. The implementation is split
// across spec_*.cpp; these all share g_state above. Wrapped in `specnode` so
// generically-named helpers (Fletcher32, AddrEqual, FormatAddr) don't collide
// with same-named functions elsewhere (e.g. savestate.cpp's Fletcher32). Each
// spectator TU does `using namespace specnode;` so call sites stay unqualified.
namespace specnode {

// ---- transport (spec_transport.cpp) ----
uint32_t Fletcher32(const uint8_t* data, size_t len);
bool     AddrEqual(const sockaddr_in& a, const sockaddr_in& b);
void     FormatAddr(const sockaddr_in& a, char* out, size_t out_sz);
void     OutboundBroadcast(const void* buf, size_t len);
void     OutboundSendTo(const sockaddr_in& to, const void* buf, size_t len);
void     AppendEventToWire(std::vector<uint8_t>& out, const SessionEvent& ev,
                           const std::vector<MatchHeader>& headers);
uint32_t CountInputs(const std::vector<SessionEvent>& events, size_t first, size_t last);
void     FlushBatch();
bool     SpecUdpEnabled();
// Pure-UDP spectator stream gates (task #55). RC (reliable-ordered + FEC
// over UDP) is DEFAULT ON -- the TCP data plane's hole-punch dependence
// black-screened wild spectators whose UDP was fine. FM2K_SPEC_RC=0
// restores the TCP-primary baseline; FM2K_SPEC_RC_SNAPSHOT=0 keeps the
// snapshot leg on TCP while events ride RC.
bool     SpecRcEnabled();
bool     SpecRcSnapshotEnabled();
void     SpecStashPreSubscribe(uint8_t chan, const uint8_t* data, int len);
void     SpecReplayPreSubStash();
void     SendUdpInputBatches();
void     SendOpBaselineTo(const sockaddr_in& to, uint32_t baseline);

// ---- playback apply (spec_playback.cpp) ----
// ApplyResetInputState is also called by SpectatorNode_ApplyPendingPinRng
// (snapshot-cache, stays in spectator_node.cpp), so it must be cross-TU visible.
void     ApplyResetInputState();
void     ApplySessionEvent(const SessionEvent& ev);

// ---- backfill (spec_backfill.cpp) ----
size_t   BackfillFirstIdxForFrame(uint32_t anchor_input_frame);
// max_events == 0 means "to the live edge" (every pre-existing caller). A
// non-zero cap ships at most that many session events and stops -- used by the
// gap-fill re-ship, whose caller pulls again for the next window.
void     SendSessionEventsTo(const sockaddr_in& to, size_t first_event_idx,
                             uint32_t start_input_frame, size_t max_events = 0);
void     SendSessionBackfillTo(const sockaddr_in& to);
void     SendSessionBackfillFromFrame(const sockaddr_in& to, uint32_t anchor_input_frame,
                                      size_t max_events = 0);
// Design 2: bounded backfill from the most recent CSS_ENTERED (falls back to
// SendSessionBackfillTo when the session has no char-select boundary yet).
// HaveBoundedAnchor() reports whether that fallback would trigger, so callers
// can log the path they will actually take rather than the one they intended.
bool     HaveBoundedAnchor();
void     SendSessionBackfillFromRecentAnchor(const sockaddr_in& to);
void     SendSnapshotTo(const sockaddr_in& to);

// ---- bounded deep join (spec_deep_join.cpp) ----
// FM2K_SPEC_DEEP_JOIN gate, read once and logged once. DEFAULT ON: the var is
// a KILL-SWITCH (=0 -> legacy path); an unrecognised value fails SAFE to OFF.
bool     DeepJoinEnabled();
// The viewer's CONSUMED INPUT position: next_expected_frame minus the INPUT
// events still sitting in pb_queue. ONE definition, shared by the Wave 2.1
// pre-anchor trim (which calls it "head_frame") and by the Wave 4 snapshot
// applicability rule, so the two can never drift apart -- after a deep-join
// apply the trim is a no-op by construction because head_frame == anchor.
//
// INVARIANT: every writer that pushes an INPUT onto pb_queue advances
// next_expected_frame by exactly one in the same step, and non-INPUT ops never
// touch it (spec_recv.cpp's EVENT_BATCH path and its UDP accelerator path are
// the only two). Do not add a third INPUT writer without keeping the cursor in
// step, or every caller of this loses its frame reference.
uint32_t ConsumedInputPos();

// HOST. DeepJoinOnBind is called from the bind path the moment the bounded
// backfill is actually shipped; DeepJoinArmSubscribers from StashSnapshot AFTER
// the cache is refreshed (so the arm always names the NEW battle);
// DeepJoinPushTick from TickHostMaintenance (one sub per tick).
void     DeepJoinOnBind(Subscriber& sub);
void     DeepJoinArmSubscribers();
void     DeepJoinPushTick(uint64_t now);
// SPEC_SNAPSHOT_REQ body. The public SpectatorNode_HandleSnapshotReq (declared
// in spectator_node.h for netplay_control.cpp's dispatch) is a thin forwarder.
void     SpectatorNode_HandleSnapshotReq_Impl(const sockaddr_in& from,
                                              uint32_t anchor_frame,
                                              uint32_t match_index,
                                              uint8_t  flags);

// VIEWER. DeepJoinShouldHold is the PopFrameInputs battle-entry hook;
// DeepJoinHoldTick is the supervision (progress-gated re-request, then loud
// escalation) called from TickHealth; DeepJoinOnSnapshotApplied closes the deep
// join out.
bool     DeepJoinShouldHold();
void     DeepJoinHoldTick(uint64_t now);
void     DeepJoinOnSnapshotApplied(uint32_t anchor);

// True while a bounded deep joiner is still SIMULATING toward its battle-entry
// anchor (mirroring the anchored char-select) rather than held at it. Used by
// PopFrameInputs to exempt this path from the "a validated snapshot is parked,
// freeze the sim" early-return: for every other join shape that early return is
// correct, but here the snapshot legitimately arrives BEFORE the local engine
// has walked the remaining char-select, and freezing then is the 2026-06-11
// circular deadlock in a new dress (apply waits for mode 3000, mode 3000 needs
// the CSS walk, the walk needs the sim). Consuming the queued CSS INPUTs is
// exactly how the cursor reaches the anchor, and it CANNOT overshoot: the
// battle-align hold parks the battle INPUTs at the head until mode >= 3000, and
// the deep-join hold takes over from there.
bool     DeepJoinWalkingToAnchor();

// ---- shared recovery mechanism (spec_join_viewer.cpp) ----------------------
// The resume-suppressed re-JOIN, generalised out of the deep-join hold ladder.
// Suppresses SPEC_JOIN_RESUME for exactly one JOIN_REQ so the host takes its
// DESTRUCTIVE-RESET branch (re-pin + fresh bind + re-ship) instead of the
// light-resume gap bind, AND resets the RC endpoint toward the upstream so a
// wedged reliable stream is actually cleared rather than re-shipped into.
// Honours session_ended / no-root and stamps last_reconnect_attempt_ms so it
// does not race TickHealth's own reconnect cadence. `reason` is a short tag
// for the log line. Returns true if a re-JOIN was actually issued.
bool     SpecForceFullReJoin(SpecJoinMode mode, const char* reason);

// The gap-fill ladder's decision point, called from TickHealth once the stall
// timer + throttle have both elapsed: issue the surgical pull, or -- once the
// pull has provably failed to move the cursor -- escalate. Owns the log line
// for whichever it does, so the log never promises a re-ship it did not send.
// `gap_ahead` is TickHealth's own variant-1/variant-2 discriminator, passed in
// rather than re-derived so the two can never drift. Lives here rather than
// inline in TickHealth so spec_health.cpp stays under the 1000-line cap.
void     SpecGapFillPullOrEscalate(uint64_t now, bool gap_ahead);

// CSS-window starve measurement. `starving` is "we are mid-stream, playing
// back, and the pending-frame queue is empty"; `in_battle` gates the real
// ladder. Accumulates episodes only for the NOT-in-battle case, which is the
// window every rung is gated out of. Measurement only -- it never pulls, never
// escalates, and never touches the ladder. Lives here for the same 1000-line
// reason as SpecGapFillPullOrEscalate above.
void     SpecCssWindowStarveTick(uint64_t now, bool starving, bool in_battle);

// The snapshot applicability rule, as a tri-state so a snapshot that is merely
// EARLY is not confused with one that is wrong.
//   APPLY  -- anchor == our consumed INPUT position, local phase has reached
//             the capture phase, and the engine's chars match the MATCH_START
//             header (the 0x40CD47 AV guard). Nothing is rewound: our sim sits
//             on the snapshot's logical frame having consumed nothing past it.
//   WAIT   -- correct snapshot, too early. Keep the inbox and retry next tick.
//   REFUSE -- backward (stale), or a battle we already missed, or mismatched
//             chars. Discard.
enum class DeepJoinVerdict : uint8_t { REFUSE, WAIT, APPLY };
DeepJoinVerdict DeepJoinSnapshotVerdict(const State::SnapshotInbox& inbox);

}  // namespace specnode

// Bounded deep-join tunables. Grouped here rather than in spectator_node.h --
// nothing outside the spec_*.cpp set needs them and that header is near its cap.
//
// REQ_INTERVAL: minimum spacing between SPEC_SNAPSHOT_REQ datagrams, and the
//   interval is RESET by any observed inbox progress, so a healthy (merely
//   slow) transfer is never prodded.
// BUDGET: total hold time without an applied snapshot before the ladder
//   escalates from "re-request" to "loud failure + full re-JOIN". ~10 requests.
// REPLY_FLOOR: host-side one-response-per-sub floor for SPEC_SNAPSHOT_REQ.
constexpr uint64_t SPECTATOR_DEEPJOIN_REQ_INTERVAL_MS  = 1500;
constexpr uint64_t SPECTATOR_DEEPJOIN_BUDGET_MS        = 15000;
constexpr uint64_t SPECTATOR_DEEPJOIN_REPLY_FLOOR_MS   = 500;

// ---- snapshot inbox staleness sweep (spec_recv.cpp) ----
// Global scope (spec_recv.cpp has no namespace block). Called once per
// spectator tick from SpectatorNode_ApplyPendingSnapshot; returns true if it
// reclaimed a dead transfer. Chunk arrival cannot drive this -- a dead
// transfer has no arrivals, which is exactly why it used to wedge forever.
bool SpectatorNode_SweepStaleSnapshotInbox();

// ---- admission pacing (spec_playback_state.cpp) ----
// Global scope, matching the SpectatorNode_* accessors it sits with.
// Called by SpectatorNode_PopFrameInputs, which stayed in spec_playback.cpp,
// so it cannot be file-static.
size_t SpecDelayBankFrames();
