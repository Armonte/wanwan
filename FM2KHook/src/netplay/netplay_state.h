// Netplay State Machine Types
// FM2K Rollback Netcode Implementation
#pragma once

#include <cstdint>

// =============================================================================
// STATE MACHINE
// =============================================================================

enum class NetplayState : uint8_t {
    DISCONNECTED,       // No connection
    CONNECTING,         // UDP handshake in progress
    SYNCED,             // Initial sync complete (version, player assignment)

    CSS_LOBBY,          // In CSS, syncing cursors, neither locked
    CSS_LOCAL_READY,    // Local player locked, waiting for remote
    CSS_REMOTE_READY,   // Remote player locked, waiting for local
    CSS_BOTH_READY,     // Both locked, preparing GekkoNet session

    BATTLE_INIT,        // Creating GekkoNet session
    BATTLE_SYNCING,     // GekkoNet handshake in progress
    BATTLE_RUNNING,     // Full rollback active
    BATTLE_PAUSED,      // Game paused during battle
    BATTLE_END,         // Match ended, cleanup
};

// Convert state to string for logging
inline const char* NetplayStateToString(NetplayState state) {
    switch (state) {
        case NetplayState::DISCONNECTED:    return "DISCONNECTED";
        case NetplayState::CONNECTING:      return "CONNECTING";
        case NetplayState::SYNCED:          return "SYNCED";
        case NetplayState::CSS_LOBBY:       return "CSS_LOBBY";
        case NetplayState::CSS_LOCAL_READY: return "CSS_LOCAL_READY";
        case NetplayState::CSS_REMOTE_READY:return "CSS_REMOTE_READY";
        case NetplayState::CSS_BOTH_READY:  return "CSS_BOTH_READY";
        case NetplayState::BATTLE_INIT:     return "BATTLE_INIT";
        case NetplayState::BATTLE_SYNCING:  return "BATTLE_SYNCING";
        case NetplayState::BATTLE_RUNNING:  return "BATTLE_RUNNING";
        case NetplayState::BATTLE_PAUSED:   return "BATTLE_PAUSED";
        case NetplayState::BATTLE_END:      return "BATTLE_END";
        default:                            return "UNKNOWN";
    }
}

// =============================================================================
// CONTROL CHANNEL MESSAGES
// =============================================================================

// Magic byte to identify control packets (vs GekkoNet packets)
constexpr uint8_t CTRL_MAGIC = 0xCC;

// Control channel message types
enum class CtrlMsg : uint8_t {
    // Connection management
    PING = 0,           // Heartbeat request
    PONG,               // Heartbeat response
    HELLO,              // Initial connection (includes player index)
    HELLO_ACK,          // Connection accepted
    DISCONNECT,         // Clean disconnect

    // Values 5..10 were the CCCaster-style CSS control plane (raw CSS input,
    // cursor, char-select, lock, unlock, start). CSS is a GekkoNet lockstep
    // session now (prediction_window = 0), so those messages are DELETED --
    // no sender, no handler, no payload struct. The NUMBERING below is frozen
    // at its historical values because the 0xCC control channel has no version
    // handshake: an old peer that still emits one of those types must land on
    // the dispatch default and be ignored, which it only does if every LIVE
    // type keeps the number it has always had.

    // Battle coordination
    BATTLE_READY = 11,  // Ready to start GekkoNet session (CSS sync)
    BATTLE_ACK,         // Acknowledged battle ready
    BATTLE_ENTERING,    // Game mode changed to battle, waiting for sync
    BATTLE_START,       // Begin battle (both confirmed)
    BATTLE_END,         // Match over

    // Chat (peer-to-peer text over the control channel). Short messages only.
    CHAT,

    // Spectator tree coordination (see docs/dev/architecture_spectate.md).
    // Bulk stream data rides the reliable channel (0xCB) -- too variable-size
    // for the fixed CtrlPacket. These CtrlMsg values cover control-plane
    // coordination only.
    SPEC_JOIN_REQ,      // Viewer asks upstream node to be a subscriber
    SPEC_JOIN_ACK,      // Upstream accepts; viewer will start receiving 0xCE stream
    SPEC_JOIN_REDIRECT, // Upstream at capacity, redirect to existing subscriber
    SPEC_HEARTBEAT,     // 1s keepalive both directions
    SPEC_LEAVE,         // Clean disconnect from subscriber tree

    // Host config snapshot -- host pushes its match-config (selected stage,
    // round count, time limit, game speed, SOCD mode) to client so both
    // peers run with identical settings without the user having to mirror
    // them by hand. Sent at HELLO_ACK and again whenever the host UI
    // changes a value or a new match starts. Client mem-writes the
    // mapped fields and adopts the SOCD mode locally.
    HOST_CONFIG,

    // DELAY_PROPOSAL -- each peer broadcasts its own input-delay
    // candidate over the control channel through CSS so both sides
    // converge on max(mine, theirs) at battle start. Without it peers
    // computed delay independently off their own RTT samples and ended
    // up asymmetric on jittery links (#24).
    DELAY_PROPOSAL,

    // SPEC_SESSION_END -- host is exiting cleanly (player quit / left the match).
    // Broadcast to all subscribers so they STOP, instead of treating the dropped
    // stream as a transient glitch and storm-reconnecting to a now-dead host.
    // Appended at the end so existing wire values don't renumber; older peers
    // hit the dispatch default and ignore it harmlessly.
    SPEC_SESSION_END,

    // SPEC_SNAPSHOT_REQ -- viewer -> host, bounded deep-join only (Wave 4).
    // A deep joiner skipped every prior match's SIMULATION, so it cannot enter
    // a battle from scratch; it HOLDS at battle entry until the host's battle
    // savestate applies. This message is the lightweight supervision of that
    // hold: WANT re-requests the snapshot for a named anchor, DONE reports that
    // one applied so the host stops pushing. Appended at the end for the same
    // no-renumber reason; the spectate version gate keeps both ends on the same
    // build so no back-compat handling is needed.
    SPEC_SNAPSHOT_REQ,
};

// Convert message type to string for logging
inline const char* CtrlMsgToString(CtrlMsg msg) {
    switch (msg) {
        case CtrlMsg::PING:         return "PING";
        case CtrlMsg::PONG:         return "PONG";
        case CtrlMsg::HELLO:        return "HELLO";
        case CtrlMsg::HELLO_ACK:    return "HELLO_ACK";
        case CtrlMsg::DISCONNECT:   return "DISCONNECT";
        case CtrlMsg::BATTLE_READY: return "BATTLE_READY";
        case CtrlMsg::BATTLE_ACK:   return "BATTLE_ACK";
        case CtrlMsg::BATTLE_ENTERING: return "BATTLE_ENTERING";
        case CtrlMsg::BATTLE_START: return "BATTLE_START";
        case CtrlMsg::BATTLE_END:   return "BATTLE_END";
        case CtrlMsg::CHAT:              return "CHAT";
        case CtrlMsg::SPEC_JOIN_REQ:     return "SPEC_JOIN_REQ";
        case CtrlMsg::SPEC_JOIN_ACK:     return "SPEC_JOIN_ACK";
        case CtrlMsg::SPEC_JOIN_REDIRECT:return "SPEC_JOIN_REDIRECT";
        case CtrlMsg::SPEC_HEARTBEAT:    return "SPEC_HEARTBEAT";
        case CtrlMsg::SPEC_LEAVE:        return "SPEC_LEAVE";
        case CtrlMsg::HOST_CONFIG:       return "HOST_CONFIG";
        case CtrlMsg::DELAY_PROPOSAL:    return "DELAY_PROPOSAL";
        case CtrlMsg::SPEC_SESSION_END:  return "SPEC_SESSION_END";
        case CtrlMsg::SPEC_SNAPSHOT_REQ: return "SPEC_SNAPSHOT_REQ";
        default:                         return "UNKNOWN";
    }
}

// =============================================================================
// PACKET STRUCTURES
// =============================================================================

#pragma pack(push, 1)

// Control channel packet header
struct CtrlPacketHeader {
    uint8_t magic;          // Always CTRL_MAGIC (0xCC)
    uint16_t seq;           // Sequence number
    uint16_t ack;           // Acknowledged sequence
    CtrlMsg type;           // Message type
    uint8_t player_id;      // Sender's player ID (0 or 1)
};

// Full control packet with data union (max 32 bytes total)
struct CtrlPacket {
    CtrlPacketHeader header;

    union {
        // HELLO data
        struct {
            uint8_t version;    // Protocol version
            uint8_t player_id;  // Requested player ID
            uint32_t game_hash; // Game version hash (for compatibility check)
        } hello;

        // BATTLE_START / BATTLE_ENTERING / BATTLE_END frame sync data.
        // epoch/flags are used by the swap barriers only; BATTLE_START
        // and older builds leave them 0.
        //   epoch: barrier instance counter (1..255, wraps skipping 0).
        //          Both peers arm the entry/end barriers in the same
        //          order (CSS session up / battle session up are both
        //          bilateral rendezvous), so the counters stay in step.
        //          0 = legacy peer, treated as wildcard.
        //   flags bit0: sender has COMPLETED this barrier (has both
        //          signals). A completed sender answers retries from a
        //          lagging peer; nobody echoes back to a completed
        //          sender (storm termination).
        //   cfg_digest: BATTLE_ENTERING only -- the sender's MATCH-SETTINGS
        //          digest (Netplay_MatchSettingsDigest: round time, round
        //          count, game speed, selected stage, SOCD mode, read from
        //          the LIVE engine globals). The entry barrier will not
        //          complete until both peers announce the SAME value, which
        //          is what makes HOST_CONFIG delivery reliable-by-
        //          construction: a lost config packet holds the barrier
        //          instead of silently letting the two sims stamp different
        //          round timers at RSS_BATTLE_INIT. 0 = "not announced"
        //          (spectator swap broadcasts, BATTLE_START/BATTLE_END, or a
        //          peer that predates this field) and disables the gate; the
        //          digest itself is never 0 by construction.
        struct {
            uint32_t frame;     // Frame number / proposed swap frame
            uint8_t  epoch;
            uint8_t  flags;
            uint8_t  pad[2];
            uint32_t cfg_digest;
        } sync;

        // PING / PONG timing. send_ms overlaps sync.frame (same offset-0 uint32)
        // so RTT stays wire-compatible with peers that never learned host-clock
        // sync. host_us_{lo,hi} carry the sender's host-clock send stamp (µs) as
        // two 32-bit halves (no uint64 in the union → alignment/size unchanged).
        // host_us == 0 => sender has no host clock → receiver uses RTT/2.
        // frame = the sender's current battle sim frame at send time: the receiver
        // projects the remote's frame forward by the elapsed host-clock time for a
        // JITTER-FREE frame-advantage (pacing off production time, not arrival time).
        struct {
            uint32_t send_ms;
            uint32_t host_us_lo;
            uint32_t host_us_hi;
            uint32_t frame;
        } ping;

        // CHAT data -- short messages (gg, wp, ez, etc.). Longer chat goes
        // over the lobby TCP channel. Null-terminated within the 24 bytes.
        struct {
            char text[24];
        } chat;

        // SPEC_JOIN_REDIRECT -- upstream is full, try this peer instead.
        struct {
            uint32_t redirect_ip;    // IPv4 in network byte order
            uint16_t redirect_port;  // host byte order
        } spec_redirect;

        // SPEC_JOIN_ACK -- host tells joining spectator which session kind
        // to mirror (CSS=1, BATTLE=2, NONE=0=between-matches). Plus the
        // host's TCP listener port -- spectator MUST dial it to receive
        // the INPUT_BATCH / INITIAL_MATCH / MATCH_END stream. UDP carries
        // only handshake + heartbeat; TCP carries the bulk stream.
        //
        // host_p1_char / host_p2_char / host_stage carry the host's
        // current battle char + stage indices when host_session_kind ==
        // BATTLE (2). Spec hook seeds FM2K_BTB_* env vars from these so
        // when the slot-0 /F dispatcher fires create_game_object(14,...)
        // the engine loads the RIGHT character files (not mirror char 0)
        // and the snapshot apply lands on a valid initial battle state.
        // 0xFF means "unknown / not in battle" → leave BTB env unset.
        struct {
            uint8_t  host_session_kind;
            uint8_t  host_p1_char;   // FM2K char grid index (0..49), 0xFF=unset
            uint8_t  host_p2_char;
            uint8_t  host_stage;
            // Confirm-button color slots (0..7), 0xFF=unknown. The /F boot
            // path never presses a confirm button, so without these the
            // engine's hardcoded /F colors (P1=0, P2=1) win and spectator
            // battles render the wrong palettes.
            uint8_t  host_p1_color;
            uint8_t  host_p2_color;
        } spec_join_ack;

        // SPEC_JOIN_REQ. byte 0 used to carry a join-mode preference
        // (from-frame-0 vs current-match); there is one join flavour now
        // -- "the match happening right now" -- so the host answers from
        // its own session state and never reads it. Kept as a reserved
        // zero byte rather than reused, so a stale value from a peer that
        // slipped the version gate cannot mean anything.
        struct {
            uint8_t _reserved0;
            uint8_t reserved[7];
        } spec_join_req;

        // SPEC_SNAPSHOT_REQ -- bounded deep-join hold supervision (Wave 4).
        //   anchor_frame: the viewer's CONSUMED INPUT position, i.e. the
        //                 anchor a snapshot must carry to be applicable at
        //                 all (see the applicability rule in
        //                 SpectatorNode_ApplyPendingSnapshot). The host
        //                 re-ships only when its cached snapshot is keyed to
        //                 exactly this frame; otherwise it arms the push so
        //                 the NEXT battle entry serves the viewer, and never
        //                 ships a stale blob.
        //   match_index:  advisory, for logs only.
        //   flags:        SPEC_SNAPREQ_WANT / SPEC_SNAPREQ_DONE.
        struct {
            uint32_t anchor_frame;
            uint32_t match_index;
            uint8_t  flags;
        } spec_snapshot_req;

        // HOST_CONFIG -- host's authoritative match settings, mirrored to
        // client + spectators so everyone runs with identical rules.
        // Address-mapped fields are written via direct memcpy to the
        // documented FM2K addresses inside the receiver.
        //
        // SENTINEL: 0xFFFFFFFF means "unset, don't apply" for ALL the
        // uint32 fields (0xFF for socd_mode). Note that 0 is a VALID
        // value for round_time_sec (= infinite timer) and round_count
        // (engine reads as 0/special), so we can't use 0 to mean unset.
        //
        // session_id (APPENDED, so every field above keeps its offset) is the
        // host's replay-session grouping id. It rides HOST_CONFIG because that
        // is the one packet already pushed to BOTH the peer AND every kind of
        // spectator (handshake broadcast, per-match re-broadcast, the entry
        // barrier's re-push, and the one-shot Netplay_SendHostConfigToSpec at
        // subscriber bind) -- so one field reaches the guest and every
        // late/bounded/deep spectator without a new message type. 0 = "not
        // announced"; a receiver ADOPTS it only while its own id is still 0,
        // it never mints one (see SpectatorNode_AdoptSessionId). Adding it
        // grew CtrlPacket by 8 bytes: RawReceive zero-fills the tail of the
        // receive buffer past recv_len, so a SHORT packet from a pre-0.2.84
        // peer reads session_id as 0 (= not announced) instead of stale bytes.
        struct {
            uint32_t selected_stage;    // → FM2K::ADDR_SELECTED_STAGE (0x43010c on FM2K).
            uint32_t round_count;       // → lParam @ 0x430124 (g_default_round, 1v1)
            uint32_t round_time_sec;    // → lParam @ 0x430114 (loaded from TestPlay.time, default 60)
            uint32_t game_speed_pct;    // → uValue @ 0x430104 (loaded from TestPlay.GameSpeed, default 10)
            uint8_t  socd_mode;         // 0..5 per Hook_GetSOCDMode. 0xFF = unset
            uint8_t  reserved[3];
            uint64_t session_id;        // replay-session grouping id. 0 = unset
        } host_config;

        // DELAY_PROPOSAL -- this peer's input-delay candidate. See the
        // CtrlMsg::DELAY_PROPOSAL comment. mode is informational (which
        // formula produced the number); the receiver only needs delay.
        struct {
            uint8_t delay;   // proposed input delay frames, 0..16
            uint8_t mode;    // 0 = avg ping, 1 = peak ping
        } delay_proposal;

        // Raw bytes for unknown/future use
        uint8_t raw[24];
    } data;
};

#pragma pack(pop)

// Ensure packet fits in single UDP datagram (plenty of room).
// The union is sized by host_config (28 B) since the session_id append;
// every other member's field offsets are unchanged, so a peer running an
// older build reads the prefix it knows and ignores the tail.
static_assert(sizeof(CtrlPacket) <= 64, "CtrlPacket too large");

// =============================================================================
// CSS STATE TRACKING
// =============================================================================

struct CSSState {
    // Cursor positions for both players
    uint8_t cursor_x[2];
    uint8_t cursor_y[2];

    // Selected character slot (0xFF = none)
    uint8_t selected_char[2];

    // Color/palette selection
    uint8_t selected_color[2];

    // Lock status (true = character confirmed)
    bool locked[2];

    // Initialize to default state
    void Reset() {
        cursor_x[0] = cursor_x[1] = 0;
        cursor_y[0] = cursor_y[1] = 0;
        selected_char[0] = selected_char[1] = 0xFF;
        selected_color[0] = selected_color[1] = 0;
        locked[0] = locked[1] = false;
    }
};

// =============================================================================
// PROTOCOL CONSTANTS
// =============================================================================

constexpr uint8_t NETPLAY_PROTOCOL_VERSION = 1;

// Timeouts (in milliseconds)
constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;       // 5 seconds to connect
// Heartbeat cadence -- tuned 2026-05-05 to balance "fast detection on
// real DC" vs "ride out lag spikes / Win32 modal title-drag pauses".
// 250ms ping × 6 missed = 1500ms tolerance. The hook also installs a
// WM_TIMER pump in imgui_overlay's WndProc that keeps ControlChannel_
// Poll() ticking inside DefWindowProc's modal loop (title drag, menu
// open), so dragging a window no longer triggers a disconnect on the
// peer side. Real-DC detection is still ~1.5s end-to-end.
constexpr uint32_t PING_INTERVAL_MS = 250;          // Ping every 250ms (4Hz)
constexpr uint32_t PING_TIMEOUT_MS  = 1500;         // ~6 missed pings = disconnect
constexpr uint32_t BATTLE_READY_TIMEOUT_MS = 5000;  // 5 seconds to start battle

// Packet send intervals (in frames at 100 FPS)
constexpr int CSS_CURSOR_SEND_INTERVAL = 3;         // Send cursor every 3 frames (30ms)
constexpr int PING_SEND_INTERVAL = 100;             // Send ping every 100 frames (1s)
