// SpectatorNode — daisy-chain replay streaming (see docs/FM2K_Spectator_Design.md).
//
// Every node (host, gameplay peer, accepted spectator) can run a SpectatorNode
// that accepts downstream subscribers, holds the input-history window needed to
// serve them, and forwards INITIAL_MATCH + INPUT_BATCH + MATCH_END datagrams.
// Over its direct-subscriber capacity it redirects new joiners to an existing
// subscriber, à la CCCaster's `getRandomRedirectAddress`.
//
// This is a SEPARATE transport from GekkoNet. It does not participate in
// rollback or input prediction. Viewers reconstruct the match by driving a
// local replay engine from the streamed inputs, not by running a GekkoNet
// SpectateSession.
#pragma once

#include <cstdint>
#include <cstddef>
#include <winsock2.h>

// Spectator-stream datagram magic — distinct from 0xCC control and GekkoNet
// traffic on the multiplexed UDP socket.
constexpr uint8_t SPEC_DATA_MAGIC = 0xCE;

// Variable-length spectator datagram types (live on the 0xCE path).
//
// Design note: inputs are inputs. We don't have a separate CSS-state-mirror
// packet. The host streams confirmed (p1, p2) input pairs from the moment
// CSS starts (lockstep-confirmed) through battle (GekkoNet-confirmed) and
// back to CSS for the next match. The spectator's local FM2K, also booted
// to CSS, consumes the same inputs in lockstep, walks the same CSS path,
// locks the same chars, transitions to battle when game_mode flips, etc.
// INITIAL_MATCH carries metadata + a seed sanity-rewrite at each new match;
// MATCH_END is informational only.
enum class SpecDataType : uint8_t {
    INITIAL_MATCH  = 1, // Legacy (retired in C12). Receivers ignore.
    INPUT_BATCH    = 2, // Legacy (retired in C12). Receivers ignore.
    MATCH_END      = 3, // Legacy (retired in C12). Receivers ignore.
    INPUT_REQUEST  = 4, // Dead under TCP (in-order + exactly-once delivery
                        // makes the gap-recovery handshake unnecessary).
    EVENT_BATCH    = 5, // Primary wire type. Payload = packed SessionEvent[]
                        // stream. SpecDataHeader.start_frame = session-
                        // relative INPUT-frame index of first INPUT in
                        // batch; frame_count = INPUT events in batch;
                        // flags = total payload byte count (variable-length
                        // since SessionEvents are 1-, 5-, or 97-byte each).

    // ----- Snapshot join (task #18) — Phase 1 ABI -----
    //
    // The CCCaster-style "jump to current match" flow uses a SaveState_Save
    // blob shipped to the spectator on JOIN. Three packet types frame the
    // blob, sent in order:
    //
    //   SNAPSHOT_BEGIN:
    //     start_frame = INPUT-frame index the snapshot was captured at
    //                   (spectator anchors next_expected_frame here)
    //     frame_count = unused (0)
    //     flags       = sizeof(SnapshotMetadata) = 16 — payload byte count
    //     payload     = SnapshotMetadata{version, total_bytes, match_index}
    //
    //   SNAPSHOT_CHUNK (sent N times to cover total_bytes):
    //     start_frame = byte offset into the assembled blob (running total)
    //     frame_count = unused (0)
    //     flags       = bytes in this chunk's payload (≤ SPECTATOR_SNAPSHOT_CHUNK_BYTES)
    //     payload     = raw blob bytes
    //
    //   SNAPSHOT_END:
    //     start_frame = unused (0)
    //     frame_count = unused (0)
    //     flags       = 4 (payload byte count)
    //     payload     = uint32_t fletcher32 over assembled blob (sanity check)
    //
    // Followed by EVENT_BATCH packets streaming events from the snapshot's
    // INPUT-frame onward — the spectator's pb_queue starts there instead
    // of session frame 0.
    //
    // Phase 1 (this commit): wire types reserved, framer recognises them
    // (returns correct payload length so the connection doesn't drop on
    // "unknown SpecDataType=N"), but host doesn't emit and spectator
    // doesn't act on them. Phase 2-4 wire up cache, send, and apply.
    SNAPSHOT_BEGIN = 6,
    SNAPSHOT_CHUNK = 7,
    SNAPSHOT_END   = 8,

    // ----- UDP input accelerator (Phase F) -----
    //
    // UDP_INPUT_BATCH is a raw datagram on the shared control UDP socket
    // (NEVER framed over TCP -- PayloadLenForType deliberately has no case
    // for it, so a host bug that puts it on the stream fails loudly).
    // TCP stays the single authority for ops, history, backfill and
    // snapshots; this datagram is a pure positional INPUT accelerator so
    // a TCP retransmit stall (multi-second at real loss rates) can't
    // starve the playback queue mid-battle.
    //
    //   UDP_INPUT_BATCH:
    //     start_frame = session-relative INPUT-frame index of first frame
    //                   in the window
    //     frame_count = frames in window (<= SPEC_UDP_WINDOW)
    //     flags       = payload byte count (4 + frame_count*4)
    //     payload     = u32 op_seq (host's total non-INPUT event count at
    //                   send time, little-endian)
    //                 + frame_count x { u16 p1, u16 p2 }
    //
    // Admission invariant (spectator side): a window is admitted only when
    // ops_seen >= op_seq, i.e. every non-INPUT event the host appended
    // before these inputs has already been TCP-decoded locally. Positional
    // guarantee: an op always precedes the inputs appended after it, and a
    // datagram only carries already-appended inputs -- so no input can
    // ever be admitted past an unseen sim-critical op, on the playback
    // queue, the hop-1 relay log, or recordings.
    //
    //   OP_BASELINE (TCP-borne, sent at bind BEFORE snapshot/backfill):
    //     start_frame = unused (0)
    //     frame_count = unused (0)
    //     flags       = 4 (payload byte count)
    //     payload     = u32 count of non-INPUT events in
    //                   session_events[0 .. backfill start) -- the ops a
    //                   mid-session joiner will never receive. Initializes
    //                   ops_seen and (re-)arms the UDP admission epoch for
    //                   the new connection; sent only to subscribers that
    //                   advertised SPEC_JOIN_UDP_OK (old framers would
    //                   drop the connection on an unknown type).
    UDP_INPUT_BATCH = 9,
    OP_BASELINE     = 10,
};

// UDP accelerator tuning. Window 64 + send-every-2-confirmed-frames means
// a frame is re-shipped in ~32 consecutive datagrams: at 20% loss the
// probability that ALL of them drop is ~0.2^32. ~270 B per datagram at
// 50/s = ~13.5 KB/s per subscriber.
// 256 confirmed frames = ~2.5s of input redundancy per datagram
// (~1KB inputs + ops tail, still under MTU). TCP outages shorter than
// this window are invisible; longer ones heal via the light resume
// re-join (no snapshot, gap-only backfill).
constexpr size_t   SPEC_UDP_WINDOW        = 256;
constexpr uint32_t SPEC_UDP_SEND_INTERVAL = 2;   // confirmed frames per send
constexpr size_t   SPEC_UDP_MAX_FANOUT    = 8;   // cap UDP sends per tick

// SPEC_JOIN_REQ reserved[0] capability bits (old builds send zeros).
constexpr uint8_t SPEC_JOIN_UDP_OK = 0x01;
// reserved[0] bit 1: reserved[1..4] carry the viewer's resume position
// (next_expected_frame, LE u32). The host backfills exactly the gap and
// ships NO snapshot -- the light re-join that makes TCP death healing
// one round trip instead of a 1MB snapshot ceremony.
constexpr uint8_t SPEC_JOIN_RESUME = 0x02;  // viewer accepts UDP_INPUT_BATCH + OP_BASELINE

// Spectator's preferred backfill mode, declared in SPEC_JOIN_REQ payload.
// Default at the wire level (zero-init) is FULL_SESSION so an older host
// or one that hasn't taken a snapshot yet falls through to the existing
// replay-from-frame-0 path.
enum class SpecJoinMode : uint8_t {
    FULL_SESSION  = 0, // Replay session_events from frame 0 (existing
                       // behaviour). Streamers / archivists who want to
                       // watch the whole set from the very start.
    CURRENT_MATCH = 1, // CCCaster-style snapshot join. Host ships its
                       // current SaveState blob + tail events, spectator
                       // does SaveState_Load and skips all previous
                       // matches. Default for live "browsing matches in
                       // progress" — most spectators just want to watch
                       // what's happening right now.
};

// Snapshot-wire constants.
constexpr uint16_t SPECTATOR_SNAPSHOT_VERSION     = 2;
constexpr size_t   SPECTATOR_SNAPSHOT_CHUNK_BYTES = 16384;  // ~16KB / chunk

#pragma pack(push, 1)
// Snapshot meta flags (v2+).
constexpr uint16_t SNAPSHOT_FLAG_ZERO_RLE = 0x0001;  // chunks carry zero-RLE-compressed bytes

struct SnapshotMetadata {
    uint16_t version;             // SPECTATOR_SNAPSHOT_VERSION (v2 adds flags + compressed_bytes)
    uint16_t flags;               // SNAPSHOT_FLAG_* (v1 senders: always 0)
    uint32_t total_bytes;         // UNCOMPRESSED SaveState blob size
    uint32_t match_index;         // 0-based index of the match this snapshot covers
    // captured_game_mode: the g_game_mode value when the host captured
    // this snapshot. Phase E (mid-CSS spectator join, v0.2.42+) writes
    // 2000 for CSS-phase snapshots and 3000 for battle-phase snapshots.
    //
    // Backward-compat: pre-Phase-E hosts left this field as `reserved1`
    // (always 0). The spec-side apply gate treats 0 as "battle-only
    // capture", matching the v0.2.41 behavior (apply when spec reaches
    // game_mode >= 3000). Hence the wire size + version stay the same.
    uint32_t captured_game_mode;
    // v2: wire byte count when SNAPSHOT_FLAG_ZERO_RLE is set (the chunks
    // carry this many compressed bytes; decompressed size = total_bytes).
    // The ~1MB savestate blob is mostly zero runs -- RLE cuts it ~10x,
    // which cuts the lossy-network join window ~10x (1MB at 20%-loss TCP
    // throughput was ~30-45s; every watchdog race lived in that window).
    uint32_t compressed_bytes;
};
static_assert(sizeof(SnapshotMetadata) == 20, "SnapshotMetadata must be 20 bytes (v2: +compressed_bytes)");
#pragma pack(pop)

#pragma pack(push, 1)
struct SpecDataHeader {
    uint8_t      magic;         // SPEC_DATA_MAGIC
    SpecDataType type;
    uint32_t     start_frame;   // First frame in payload (for INPUT_BATCH)
    uint16_t     frame_count;   // Number of ReplayFrames in payload (INPUT_BATCH only)
    uint16_t     flags;         // Reserved
};
static_assert(sizeof(SpecDataHeader) == 10, "SpecDataHeader must be 10 bytes");
#pragma pack(pop)

// =============================================================================
// SESSION EVENT STREAM (C1+: typed event log)
// =============================================================================
//
// One ordered event stream replaces the old "INPUT_BATCH only" wire format.
// Every state mutation the host applies — RNG pin, input-state reset, sound
// dedup re-init, match boundaries, per-frame inputs — is appended as a typed
// event in the same vector. The spectator drains the stream in order: non-
// INPUT events at the head are applied immediately; INPUT events are popped
// one per sim tick.
//
// "Frame number" is implicit: it's the index of the next INPUT event in the
// stream. Non-INPUT events execute *before* the input that follows them.
// This collapses ordering — there's no separate "ops timeline" to schedule
// against the input timeline, which is exactly the race that caused the
// game_mode-driven CheckGameModeTransition spectator branch to desync.
//
// Wire encoding per event (1-byte type tag + variant payload):
//
//   Type | Tag | Wire payload          | Bytes (incl. tag)
//   ---------------------------------------------------------
//   INPUT             | 1 | u16 p1, u16 p2     | 5
//   PIN_RNG           | 2 | u32 seed           | 5
//   RESET_INPUT_STATE | 3 | (none)             | 1
//   SOUND_INIT        | 4 | (none)             | 1
//   MATCH_START       | 5 | u8[96] ReplayHeader| 97
//   MATCH_END         | 6 | (none)             | 1
//   FINGERPRINT       | 7 | u32 fletcher32     | 5
//
// Endianness: little-endian throughout (matches FM2K's native layout).
enum class SessionEventType : uint8_t {
    INPUT             = 1,
    PIN_RNG           = 2,
    RESET_INPUT_STATE = 3,
    SOUND_INIT        = 4,
    MATCH_START       = 5,
    MATCH_END         = 6,   // C7 — payload extended to MatchEndPayload (7 B)
    FINGERPRINT       = 7,
    ROUND_START       = 8,   // C3.5 — emitted at vs_round_function 100→101 edge
    ROUND_END         = 9,   // C3.5 — emitted at vs_round_function *→900 edge
    SESSION_ID        = 10,  // C7 — once per session (host-generated u64)
    CSS_ENTERED       = 11,  // Phase F seam mirror — host's game_mode hit
                             // 2000 (CSS opened). Lets the viewer split the
                             // seam stream into [results inputs | CSS
                             // inputs] so the CSS dance mirrors from its
                             // frame 0 with both sides' CSS state freshly
                             // initialized (cursor reset). No payload.
};

constexpr size_t SESSION_EVENT_MATCH_HDR_SIZE = 96;
constexpr size_t SESSION_EVENT_MAX_WIRE_SIZE  = 1 + SESSION_EVENT_MATCH_HDR_SIZE;  // 97 (MATCH_START)

#pragma pack(push, 1)
struct RoundStartPayload {
    uint8_t  round_idx;        // 1-based within match
    uint16_t p1_hp_max;
    uint16_t p2_hp_max;
    uint16_t timer_seconds;
};
static_assert(sizeof(RoundStartPayload) == 7, "RoundStartPayload must be 7 bytes packed");

struct RoundEndPayload {
    uint8_t  winner_idx;        // 0=P1, 1=P2, 2=draw
    uint16_t p1_hp_remaining;
    uint16_t p2_hp_remaining;
    uint32_t frames_elapsed;
};
static_assert(sizeof(RoundEndPayload) == 9, "RoundEndPayload must be 9 bytes packed");

// C7 MATCH_END enrichment. Captured at Netplay_EndBattle from the same
// HP / round-win counters the launcher reads for SharedMem outcome publish.
// Makes .fm2krep self-describing — the writer can populate winner / per-side
// rounds without having to scan the body bytes for the latest ROUND_END.
struct MatchEndPayload {
    uint8_t  winner_idx;        // 0=P1, 1=P2, 2=draw
    uint8_t  rounds_won_p1;
    uint8_t  rounds_won_p2;
    uint32_t frames_total;      // session-input-frame delta from MATCH_START
};
static_assert(sizeof(MatchEndPayload) == 7, "MatchEndPayload must be 7 bytes packed");
#pragma pack(pop)

// In-memory event. MATCH_START's 96-byte header still stored out-of-band
// (match_headers side table). Round payloads inline to avoid extra side-
// tables; bumps SessionEvent from 5 → 10 bytes (~2x session_events memory).
#pragma pack(push, 1)
struct SessionEvent {
    SessionEventType type;          // 1 byte
    union {                         // 9 bytes — sized to fit RoundEndPayload
        struct { uint16_t p1; uint16_t p2; } input;
        uint32_t                             pin_rng_seed;
        uint32_t                             fingerprint_hash;
        uint16_t                             match_start_idx;
        RoundStartPayload                    round_start;
        RoundEndPayload                      round_end;
        MatchEndPayload                      match_end;     // C7 (7 B)
        uint64_t                             session_id;    // C7 (8 B)
        // Host's CSS start state, carried on CSS_ENTERED so a snapshot-join
        // spectator can align its rematch-CSS replay walk to the host's (it
        // never walked CSS1, so its own cursor/selected are stale). 6 B.
        struct { int8_t p1_cur_x, p1_cur_y, p2_cur_x, p2_cur_y,
                        p1_sel, p2_sel; }    css_entered;
        uint8_t                              raw[9];
    } u;
};
#pragma pack(pop)
static_assert(sizeof(SessionEvent) == 10, "SessionEvent must be 10 bytes packed");

// ---- Wire-format encoders ---------------------------------------------------
// Each Encode* writes one event into `out`. Returns bytes written, or 0 if
// `cap` is insufficient. No partial writes — the buffer is left untouched on
// overflow.

size_t SessionEvent_EncodeInput            (uint8_t* out, size_t cap, uint16_t p1, uint16_t p2);
size_t SessionEvent_EncodePinRng           (uint8_t* out, size_t cap, uint32_t seed);
size_t SessionEvent_EncodeResetInputState  (uint8_t* out, size_t cap);
size_t SessionEvent_EncodeSoundInit        (uint8_t* out, size_t cap);
size_t SessionEvent_EncodeCssEntered       (uint8_t* out, size_t cap, const SessionEvent& ev);
size_t SessionEvent_EncodeMatchStart       (uint8_t* out, size_t cap,
                                            const uint8_t header[SESSION_EVENT_MATCH_HDR_SIZE]);
size_t SessionEvent_EncodeMatchEnd         (uint8_t* out, size_t cap,
                                            const MatchEndPayload& p);
size_t SessionEvent_EncodeFingerprint      (uint8_t* out, size_t cap, uint32_t hash);
size_t SessionEvent_EncodeRoundStart       (uint8_t* out, size_t cap,
                                            const RoundStartPayload& p);
size_t SessionEvent_EncodeRoundEnd         (uint8_t* out, size_t cap,
                                            const RoundEndPayload& p);
size_t SessionEvent_EncodeSessionId        (uint8_t* out, size_t cap, uint64_t session_id);

// ---- Wire-format decoder ----------------------------------------------------
// Decode the next event from a byte buffer. Returns bytes consumed (1, 5, or
// 97), or 0 if the buffer is truncated or the type byte isn't a valid
// SessionEventType.
//
// `out_event->u.match_start_idx` is left at 0 on MATCH_START — caller
// (HandleSpecData) populates it after appending the parsed header to the
// match_headers side table. The 96-byte header is copied into
// `out_match_header` if non-null; pass nullptr to skip the copy.
size_t SessionEvent_Decode(const uint8_t* in, size_t in_len,
                           SessionEvent* out_event,
                           uint8_t out_match_header[SESSION_EVENT_MATCH_HDR_SIZE]);

// =============================================================================
// LIFECYCLE
// =============================================================================

// Start accepting spectator subscribers on this node. Called once at hook init.
// Pulls the shared UDP socket from control_channel (no separate socket).
void SpectatorNode_Init();

// Shutdown — disconnects all subscribers, clears state.
void SpectatorNode_Shutdown();

// =============================================================================
// HOST-SIDE (match is running on this node)
// =============================================================================

// Called at battle start. Captures the initial-match metadata that gets
// handed to every new subscriber + persisted in the .fm2krep file.
void SpectatorNode_OnMatchStart(
    uint32_t game_hash,
    uint32_t initial_rng_seed,
    uint32_t initial_state_hash,
    uint8_t p1_char, uint8_t p1_color,
    uint8_t p2_char, uint8_t p2_color,
    uint8_t stage_id);

// Called once per confirmed (non-rollback) frame. The node buffers the input
// pair and, every broadcast_interval frames, batches and forwards to all
// subscribers.
void SpectatorNode_OnFrameConfirmed(uint16_t p1_input, uint16_t p2_input);

// Called at battle end — broadcasts MATCH_END to subscribers, carrying the
// C7 enriched payload (winner / per-side rounds / total frames). Netplay_
// EndBattle gathers the payload values from FM2K HP / FM95 round-win
// counters before this fires.
void SpectatorNode_OnMatchEnd(const MatchEndPayload& p);

// Append a state-init op to the session event log + flush immediately so
// any currently-subscribed spectator receives it before the INPUT events
// that follow. The spectator drains non-INPUT events at the head of its
// playback queue and dispatches on type, applying the same memory writes
// the host just did. Late joiners see ops as part of their session backfill.
//
// Call sites (host only — no-op on spectator nodes):
//   PIN_RNG           — every site that writes 0x12345678 to ADDR_RANDOM_SEED
//                       (handshake, CSS rendezvous, battle start)
//   RESET_INPUT_STATE — paired with battle-start SaveState_Save's first-call
//                       buf_idx + edge-state + history-rings reset
//   SOUND_INIT        — every SoundRollback::Init() call site
//   FINGERPRINT       — diagnostic only; gated on FM2K_SPEC_FINGERPRINT
void SpectatorNode_AppendPinRng(uint32_t seed);
void SpectatorNode_AppendResetInputState();
void SpectatorNode_AppendSoundInit();
// Phase F seam mirror: host's game_mode reached 2000 (CSS opened).
void SpectatorNode_AppendCssEntered();
void SpectatorNode_AppendFingerprint(uint32_t hash);

// C3.5 — round boundary events. Hook-side (vs_round_function detour) calls
// AppendRoundStart at the 100→101 substate edge and AppendRoundEnd at the
// *→900 edge. round_idx is a 1-based intra-match counter the hook owns;
// frames_elapsed is computed inside AppendRoundEnd from total_input_count
// delta against the most-recent AppendRoundStart's snapshot.
void SpectatorNode_AppendRoundStart(uint8_t  round_idx,
                                    uint16_t p1_hp_max,
                                    uint16_t p2_hp_max,
                                    uint16_t timer_seconds);
void SpectatorNode_AppendRoundEnd  (uint8_t  winner_idx,
                                    uint16_t p1_hp_remaining,
                                    uint16_t p2_hp_remaining);

// Append a MATCH_START op carrying the 96-byte ReplayHeader-compatible
// payload (magic + version + game_hash + initial_rng_seed +
// initial_state_hash + p1/p2 char/color/name + stage_id). The receiver
// caches the header in its pb_match_headers side table and flips
// playing_back=true at apply time. C6 supersedes the legacy
// INITIAL_MATCH packet path; the wire packet type stays in the enum
// for compatibility but is no longer emitted by C6+ hosts.
void SpectatorNode_AppendMatchStart(const uint8_t header[96]);

// Append a MATCH_END op carrying the C7 enriched payload (winner_idx,
// per-side rounds_won, frames_total). Apply-time spectator effect:
// playing_back=false (queue keeps draining naturally so the final frames
// render). Payload values are captured at Netplay_EndBattle.
void SpectatorNode_AppendMatchEnd(const MatchEndPayload& p);

// C7 — emit the host's session_id as a SESSION_ID op. Called once per
// game-vs-game session (typically at the first Netplay_StartBattle on a
// peer connection). Subsequent battles in the same session reuse the
// already-appended id; the writer reads it back from g_state when
// emitting .fm2kset / .fm2krep file headers.
void SpectatorNode_AppendSessionId(uint64_t session_id);

// Read the host's currently-active session_id (0 if none yet established).
// Used by the file writer to populate FM2KSessionFileHeader.session_id.
uint64_t SpectatorNode_GetSessionId();

// Snapshot capture (task #18 phase 2). Called from Netplay_StartBattle
// AFTER the existing pin-and-init sequence (PIN_RNG / RESET_INPUT_STATE /
// SOUND_INIT / OnMatchStart). Triggers a SaveState_Save(0), copies the
// resulting slot bytes into the host's snapshot cache, computes a
// fletcher32 checksum, and tags it with the current INPUT-frame index +
// match counter.
//
// The cache is the host-side input to phase 3's CURRENT_MATCH backfill:
// when a spectator joins with mode=CURRENT_MATCH, the host transmits the
// cached blob via SNAPSHOT_BEGIN/CHUNK/END, then streams events from the
// cache's anchor frame onward. Spectator does SaveState_Load and skips
// every previous match.
//
// Phase 2 (this commit): captures the snapshot per match, exposes status
// via SpectatorNode_HasSnapshot() / SpectatorNode_GetSnapshotInfo(), but
// no transmission yet — phase 3 wires the host send path.
void SpectatorNode_StashSnapshot();

// Status: is a valid snapshot cached on this node right now? False
// before the first Netplay_StartBattle of the session.
bool SpectatorNode_HasSnapshot();

// Read-only snapshot metadata, valid when HasSnapshot() returns true.
// Used by phase 3's send path. Returns sentinel zeros if no snapshot.
struct SpectatorSnapshotInfo {
    uint32_t input_frame;   // INPUT-frame index where the snapshot was taken
    uint32_t match_index;   // 0-based match counter within the session
    uint32_t total_bytes;   // byte size of the cached blob
    uint32_t checksum;      // fletcher32 over the blob
};
SpectatorSnapshotInfo SpectatorNode_GetSnapshotInfo();

// Spectator-side: lazily apply a snapshot whose SNAPSHOT_END validation
// completed but whose SaveState_LoadFromBytes was deferred because the
// local engine hadn't progressed past pre-WinMain bootup (game_mode == 0).
// Polled every spec tick from RunSpectatorTick; no-op when nothing is
// pending or when game_mode is still 0. Once mode flips to 1000+ (title
// or beyond) the held blob is loaded, pb_queue is anchored at the
// snapshot's INPUT-frame, and live broadcasts can resume.
void SpectatorNode_ApplyPendingSnapshot();

// Apply any queued PIN_RNG seed at battle-entry. Host emits PIN_RNG at
// MATCH_START (= battle entry); replay defers application until its own
// engine flips to game_mode 3000 so rng parity at battle frame 0 matches
// host's recording. No-op if no PIN_RNG is queued.
void SpectatorNode_ApplyPendingPinRng();

// =============================================================================
// SESSION REPLAY FILE WRITERS (C7)
// =============================================================================
//
// Both write the same on-disk format — packed SessionEvent bytes (1-byte
// tag + variant payload, MATCH_START's 96-byte header inline) preceded by
// a 32-byte file header. Distinguished by the `is_battle_slice` flag in
// the header. Loaders (Replay_LoadSessionFile, C8) feed events into the
// playback driver pb_queue same as live wire ingest.
//
// Path format:
//   replays/<timestamp>.fm2krep   — per-battle slice
//   sessions/<timestamp>.fm2kset  — full session
// (Relative to the game's working directory.)
//
// Returns true on successful write. False if there's no usable data
// (empty session, MATCH_START not seen yet, etc.) or the file open failed.

// Write everything in session_events to a .fm2kset.
bool SpectatorNode_WriteSessionFile(const char* path);

// Write the slice [last_match_start_idx ... session_events.size()) — i.e.
// the per-battle segment closing on the most-recent MATCH_END. Call
// AFTER OnMatchEnd has appended its MATCH_END so the slice includes it.
bool SpectatorNode_WriteCurrentBattleFile(const char* path);

// =============================================================================
// SESSION FILE LOADER (C8)
// =============================================================================
//
// Read a .fm2kset / .fm2krep file written by SpectatorNode_WriteSessionFile
// or WriteCurrentBattleFile and push every event into pb_queue. Same
// playback driver as the live wire path (HandleSpecData::EVENT_BATCH) —
// the trampoline's RunSpectatorTick drains them identically.
//
// Resets receiver-side state on entry: pb_queue cleared, pb_match_headers
// cleared, dedup baseline cleared, playing_back flipped on. Caller is
// responsible for ensuring g_spectator_mode is true before calling — the
// trampoline only routes through RunSpectatorTick when it is.
//
// Returns true on successful parse + queue. False on file open failure,
// magic/version mismatch, or truncated body.

// C8 — seek-target struct for round-level "jump into a replay at round N"
// playback. Reads the v2 header's round_offsets[] to locate the body byte
// position of the requested ROUND_START event, then does a two-pass body
// walk: Pass 1 emits ONLY state-init events (PIN_RNG, RESET_INPUT_STATE,
// SOUND_INIT, MATCH_START, SESSION_ID) up to that offset — rebuilds engine
// state without sim work. Pass 2 streams normally from the offset onward.
// The existing C5.5 catchup drain in RunSpectatorTick fast-forwards the
// pre-anchor INPUT events (skipped in Pass 1) at unbounded sim rate.
//
// kind == NONE → play from start (legacy behavior, identical to single-arg
// LoadSessionFile call). kind == ROUND_START + idx is 1-based; idx==1 means
// "start at round 1" (effectively MATCH_START since no INPUTs precede it).
enum class SeekEventKind : uint8_t {
    NONE        = 0,
    ROUND_START = 1,
    // MATCH_START reserved for .fm2kset multi-match seek (future).
};

struct SeekTarget {
    SeekEventKind kind = SeekEventKind::NONE;
    uint16_t      idx  = 0;   // 1-based for ROUND_START
};

bool SpectatorNode_LoadSessionFile(const char* path,
                                   const SeekTarget& seek = {});

// =============================================================================
// FINGERPRINT DIAGNOSTIC (C9)
// =============================================================================

// True if FM2K_SPEC_FINGERPRINT=1 was set at process start. Gates host
// emit of FINGERPRINT ops + spectator-side mismatch logging.
bool SpectatorFingerprint_Enabled();

// Compute Fletcher-32 over the current FM2K state sample (RNG, buf_idx,
// HPs, timer, positions, scripts). Same shape on host and spectator;
// drain-at-head ordering on the spectator means both sides hash at the
// same logical frame. Mismatch indicates a desync.
uint32_t SpectatorFingerprint_Compute();

// =============================================================================
// JOIN / REDIRECT (runs on any accepting node)
// =============================================================================

// Capacity: the default direct-subscriber cap. Star topology — host serves
// everyone directly up to this many. Beyond capacity, new joiners get
// redirected to an existing subscriber (1-hop relay), à la CCCaster's
// random-redirect. We rarely expect to hit this in practice: at ~500 B/s
// per spectator, even 100 direct subscribers is only 50 KB/s upload.
// Daisy-chain is the failure case, not the design.
constexpr size_t SPECTATOR_DEFAULT_CAPACITY = 32;

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
constexpr uint64_t SPECTATOR_SUBSCRIBER_EXPIRY_MS  = 30000;  // upstream-side
                                                             // sweep: drop
                                                             // subscribers
                                                             // silent >30 s

// Set the direct-subscriber cap. 0 disables accepting (node still relays if
// it already has subscribers).
void SpectatorNode_SetCapacity(size_t max_direct);

// Reset the per-session GekkoSpectator addr tracking set. Call once
// after each fresh gekko_create + gekko_start so the new session starts
// with an empty "already added" map. Without this, spec rejoins after a
// session boundary would be no-op'd as "already added" against the
// destroyed previous session's records. See spectator_node.cpp comment
// next to g_gekko_spectator_addrs for the leak background.
void SpectatorNode_ClearGekkoSpectatorTracking();

// Handle an inbound SPEC_JOIN_REQ from a peer. Either:
//   - accept (below capacity) → enqueue INITIAL_MATCH + SPEC_JOIN_ACK
//   - redirect (at capacity, have subscribers) → send SPEC_JOIN_REDIRECT
//   - reject (at capacity, no subscribers) → send SPEC_JOIN_REDIRECT w/ null
// Called from the control-channel message handler. `mode` is the
// SpecJoinMode value the joining peer declared in its SPEC_JOIN_REQ
// payload (zero / FULL_SESSION when sent by older builds that don't
// know about the field — the back-compat path).
// `caps` is the JOIN_REQ's reserved[0] capability byte (SPEC_JOIN_* bits;
// zeros from older builds).
void SpectatorNode_HandleJoinReq(const sockaddr_in& from,
                                 SpecJoinMode mode = SpecJoinMode::FULL_SESSION,
                                 uint8_t caps = 0,
                                 uint32_t resume_frame = 0);

// Handle SPEC_LEAVE — remove subscriber from list.
void SpectatorNode_HandleLeave(const sockaddr_in& from);

// Host side: broadcast SPEC_SESSION_END to all subscribers on clean exit so
// they stop instead of storm-reconnecting to a dead host. Call in Netplay_Shutdown.
void SpectatorNode_BroadcastSessionEnd();

// Viewer side: handle SPEC_SESSION_END from upstream — park the reconnect path.
void SpectatorNode_HandleSessionEnd(const sockaddr_in& from);

// Handle SPEC_HEARTBEAT — refresh last-seen timestamp for this subscriber.
void SpectatorNode_HandleHeartbeat(const sockaddr_in& from);

// =============================================================================
// STATUS / INTROSPECTION
// =============================================================================

// How many direct subscribers this node is serving.
size_t SpectatorNode_GetSubscriberCount();

// Is this node currently pushing a live match to subscribers?
bool SpectatorNode_IsBroadcasting();

// Snapshot of subscriber addresses, used by Netplay_Start{CSS,Battle}Session
// to call gekko_add_actor(GekkoSpectator, &addr) for each. Returned by value
// (copy) to avoid exposing the internal Subscriber struct.
#include <vector>
std::vector<sockaddr_in> SpectatorNode_GetSubscriberAddrs();

// =============================================================================
// VIEWER-SIDE (this node is a spectator subscribing upstream)
// =============================================================================

// Send a SPEC_JOIN_REQ upstream. Called when the user clicks "Spectate" on
// a match in the lobby (or enters a direct IP). upstream is the host/relay's
// address; socket is the multiplexed UDP socket we already have.
//
// `mode` declares the spectator's preferred backfill: FULL_SESSION replays
// from session frame 0 (existing default); CURRENT_MATCH asks the host to
// send its most recent SaveState snapshot so the spectator skips earlier
// matches. Older hosts ignore the field and always respond with full-
// session data — back-compat is automatic.
bool SpectatorNode_RequestJoin(const sockaddr_in& upstream,
                               SpecJoinMode mode = SpecJoinMode::FULL_SESSION);

// Set the always-on failback root address. TickHealth will reconnect to
// root if our current upstream goes silent. Called once at spectator init
// from Netplay_InitAsSpectator.
void SpectatorNode_SetRootAddr(const sockaddr_in& root);

// Handle inbound SPEC_JOIN_ACK — upstream accepted us. host_session_kind
// from the ACK payload (1=CSS, 2=BATTLE, 0=unknown/between-matches) is
// used to create a matching GekkoSpectateSession. host_p1_char /
// host_p2_char / host_stage carry the host's current battle chars when
// host_session_kind == BATTLE (0xFF = unknown / not in battle); spec
// seeds FM2K_BTB_* env vars from these so /F-boot loads the right
// character files instead of the placeholder char 0.
void SpectatorNode_HandleJoinAck(const sockaddr_in& from,
                                 uint8_t host_session_kind,
                                 uint16_t host_tcp_port,
                                 uint8_t host_p1_char = 0xFF,
                                 uint8_t host_p2_char = 0xFF,
                                 uint8_t host_stage = 0xFF,
                                 uint8_t host_p1_color = 0xFF,
                                 uint8_t host_p2_color = 0xFF);

// Handle inbound SPEC_JOIN_REDIRECT — retry against redirect target.
void SpectatorNode_HandleJoinRedirect(const sockaddr_in& from,
                                      uint32_t redirect_ip,
                                      uint16_t redirect_port);

// Handle an inbound 0xCE datagram. Parses the SpecDataHeader and routes
// into the playback queue: SessionEvent_Decode walks the EVENT_BATCH
// payload, MATCH_START / round events / inputs flow into pb_queue; the
// SNAPSHOT_BEGIN/CHUNK/END group runs SaveState_LoadFromBytes on
// completion. Called by the UDP poll path in control_channel.cpp when
// it sees SPEC_DATA_MAGIC.
void SpectatorNode_HandleSpecData(const uint8_t* buf, size_t len,
                                  const sockaddr_in& from);

// Narrow handler for raw 0xCE datagrams arriving on the shared control
// UDP socket (Phase F accelerator). Accepts ONLY UDP_INPUT_BATCH, and only
// from the current upstream while the admission epoch is armed -- never
// runs the full HandleSpecData parser (TCP ordering assumptions don't
// hold for raw datagrams). Called from control_channel.cpp's RawReceive
// demux before the GekkoNet queue.
void SpectatorNode_HandleUdpInputDatagram(const uint8_t* buf, size_t len,
                                          const sockaddr_in& from);

// Upstream TCP stream died (read error / corrupt stream / unknown type).
// Drops the subscription + UDP admission epoch so TickHealth's reconnect
// path re-JOINs immediately, instead of waiting on a silence failover that
// the UDP accelerator suppresses forever (queue never idles while
// datagrams flow, but ops are TCP-only -- the q:7 boundary zombie).
void SpectatorNode_OnUpstreamTcpDead();

// Playback-pacing queries for the trampoline's jitter floor (Phase F):
// the q<floor freeze must not strand a queued boundary op (MATCH_END
// behind tail inputs) or an active SEAM/PINNING walk.
bool SpectatorNode_InBoundary();
bool SpectatorNode_QueueHasPendingOp();

// Milliseconds since the last INPUT was admitted to the playback queue
// (0 until the first admission). The jitter floor uses this to detect
// genuine starvation and play out its held frames instead of freezing.
uint32_t SpectatorNode_MsSinceLastAdmit();
void SpectatorNode_StampInputAdmit();
// Layer-2 render pacing: the spectator's outer-tick sleep target in ms, =
// clamp(measured host production period, 10, 20). Lets the spectator render at
// the host's true rate (e.g. ~14ms/70fps on a heavy stage) with no duplicates.
uint32_t SpectatorNode_RenderPeriodMs();
// UDP-datagram INPUT admissions only (subset of the above). Feeds the
// adaptive bank's TCP-only floor pre-arm.
void SpectatorNode_StampUdpInputAdmit();

// Broadcast delay-bank target in frames (Phase G adaptive): the larger
// of the FM2K_SPEC_DELAY floor (default 300) and the rolling-30-60s max
// inter-admission gap x1.5 (grow-only per session, capped 2000, gaps
// over 5s ignored as outages). FM2K_SPEC_ADAPTIVE=0 pins to the floor.
// Single source of truth for every pacing regime (bank build hold,
// glide, gentle drain, battle emergency, catchup exit).
size_t SpectatorNode_TargetDelayFrames();

// Natural-boot viewer still walking title/menu (pre-CSS) -- the jitter
// floor must not hold the tick on queue depth during this phase.
bool SpectatorNode_InNaturalBootWalk();

// Re-request the upstream JOIN if none is in flight (1Hz throttle).
// Used by the /F dispatch hold, which can run before the DLL-init path
// has sent the original JOIN_REQ.
void SpectatorNode_KickJoin();

// Are we currently subscribed upstream (receiving a live match stream)?
bool SpectatorNode_IsSubscribedUpstream();

// Upstream TCP died but the subscription is riding on UDP with a
// background re-JOIN in flight (window title shows "Resyncing...").
bool SpectatorNode_IsTcpRejoinPending();

// Periodic health tick. Called from the trampoline once per iteration
// (cheap — internally rate-limited). Drives:
//   * heartbeat send to upstream every HEARTBEAT_INTERVAL_MS
//   * silence detection → failover to root after SILENCE_FAILOVER_MS
//   * upstream-side subscriber sweep (expire silent subscribers)
void SpectatorNode_TickHealth();

// Host-side maintenance only — bind newly-accepted TCP clients to subscriber
// slots, ship deferred INITIAL_MATCH/backfill on first bind, and expire silent
// subscribers. Driven from ControlChannel_Poll so it runs on the host path
// (which doesn't go through SpectatorNode_TickHealth) every poll iteration.
// Idempotent and short-circuiting when there are no subscribers.
void SpectatorNode_TickHostMaintenance();

// Clean disconnect from upstream.
void SpectatorNode_LeaveUpstream();

// =============================================================================
// VIEWER-SIDE PLAYBACK DRIVER (consumed by main_loop_trampoline)
// =============================================================================
//
// When the spectator is subscribed and the host has sent INITIAL_MATCH,
// `IsPlayingBack()` flips true and inputs flow:
//   trampoline RunSpectatorTick() →
//     pulls (p1, p2) via PopFrameInputs() →
//     stores into shared globals →
//     drives original_process_game_inputs / original_update_game →
//     RenderFrameWithSnapshot
// MATCH_END flips IsPlayingBack() back to false; queue drains naturally.

// Are we actively playing back a host's match (inside SPECTATOR_PLAYBACK phase)?
// True from INITIAL_MATCH receipt until MATCH_END.
bool SpectatorNode_IsPlayingBack();

// Pull the next confirmed frame's (p1, p2) inputs from the local queue.
// Returns false if the queue is empty (we're paused, waiting for the next
// upstream INPUT_BATCH); the trampoline holds the current rendered frame
// rather than advancing the sim. Returns true and fills *p1_input/*p2_input
// when a frame is available.
bool SpectatorNode_PopFrameInputs(uint16_t* p1_input, uint16_t* p2_input);

// Cached input pair from the most recent successful PopFrameInputs. The hook
// at Hook_GetPlayerInput uses these to satisfy multiple GetPlayerInput calls
// per sim tick from a single queue pop. Mirrors the netplay-active path
// where Netplay_GetInput returns a stable per-frame value.
uint16_t SpectatorNode_GetCurrentP1Input();
uint16_t SpectatorNode_GetCurrentP2Input();

// Force the cached inputs to zero. Used by RunSpectatorTick during
// post-match idle (queue empty, playing_back=false) so the local sim
// auto-advances through results / intermission with neutral inputs
// rather than re-using last frame's streamed input.
void SpectatorNode_ResetCurrentInputs();

// Number of buffered (p1, p2) frames waiting to be consumed. Used by the
// trampoline to decide if we're paused (0) or have headroom.
size_t SpectatorNode_PendingFrameCount();
