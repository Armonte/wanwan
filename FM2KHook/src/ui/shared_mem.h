#pragma once
#include <cstdint>

// ============================================================================
// MINIMAL SHARED MEMORY - Hook -> Launcher status reporting
// Config delivery: environment variables (FM2K_PLAYER_INDEX, FM2K_LOCAL_PORT, etc.)
// Save states: local to hook DLL (savestate.cpp)
// This struct is ~64 bytes. Previously was ~9.6 MB.
// ============================================================================

constexpr uint32_t FM2K_SHARED_MEM_MAGIC = 0x464D324B;  // "FM2K"
// v13 (2026-05-18): spec hub-relay user-id propagation -- adds
// spectator_punch_user_id[32] alongside the existing punch target so
// the host hook can address relay-mode subs by hub user-id instead of
// sockaddr. Set by launcher on every spec_incoming WS event from hub.
// Empty string = legacy spec_incoming (pre-relay hub). Phase 2c of
// the v0.3 spec rebuild; see docs/dev/spec_hub_relay_design.md.
// v12 (2026-05-12): spec TCP-STUN external addr + session_kind blocks.
// v11 (2026-05-09): spectator NAT-punch coordination -- adds
// spectator_punch_{ip_be,port,seq}. Launcher writes when hub forwards a
// spectator_incoming event; hook's TickHostMaintenance polls the seq
// for changes and calls StartPunch on bumps. Fixes "stuck on Connecting"
// for spectators outside the host's NAT.
// v10 (2026-05-08): C10 hub-schema-v2 plumbing -- adds match_session_id +
// match_index_in_session + match_rounds_count + match_rounds[8]. The
// launcher includes these in its `match_result` JSON to the hub so
// matches.json records (schema: 2) can carry per-match round
// breakdowns + session grouping.
// v9 (2026-05-06): in-game HUD state block (scores, spectator count,
// system-message slot).
constexpr uint32_t FM2K_SHARED_MEM_VERSION = 13;

// Maximum bytes for a UTF-8-encoded character name in the shared mem
// outcome payload. FM2K stores .player filenames as 256-byte CP932
// (Shift-JIS) strings in g_char_slot_data; UTF-8 expansion of typical
// JP names sits well under 96 bytes (30 codepoints × 3 B), and we
// truncate-with-null on overflow rather than refusing to publish.
constexpr size_t   FM2K_MATCH_CHAR_NAME_MAX = 96;

// Match outcome enum, sent hook → launcher when a battle session ends.
// Launcher forwards as a `match_result` message to the hub. See
// docs/dev/launcher_followups.md for the W-L-D protocol.
// Per-round summary, one entry written into match_rounds[] at every
// ROUND_END. Launcher reads on match-end alongside outcome and forwards
// to the hub in the match_result JSON's "rounds" array. Matches the
// hub-schema-v2 RoundResult shape spec'd in
// docs/roadmaps/replay_hierarchy.md.
//
// 12 bytes packed. Field order chosen so each field lands on its natural
// alignment without trailing padding (frames_elapsed first to keep
// natural u32 alignment, hp pair next at u16 alignment, winner+reserved
// last). Layout matches the wire format spec'd in
// docs/roadmaps/replay_hierarchy.md.
#pragma pack(push, 1)
struct FM2KRoundResult {
    uint32_t frames_elapsed;       // sim frames the round took
    uint16_t p1_hp_remaining;
    uint16_t p2_hp_remaining;
    uint8_t  winner_idx;           // 0=P1, 1=P2, 2=draw
    uint8_t  _reserved[3];
};
static_assert(sizeof(FM2KRoundResult) == 12, "FM2KRoundResult must be 12 bytes");
#pragma pack(pop)

enum FM2KMatchOutcome : uint8_t {
    FM2K_MATCH_OUTCOME_NONE        = 0,  // no outcome reported yet
    FM2K_MATCH_OUTCOME_SELF_WON    = 1,
    FM2K_MATCH_OUTCOME_PEER_WON    = 2,
    FM2K_MATCH_OUTCOME_DRAW        = 3,
    FM2K_MATCH_OUTCOME_DISCONNECT  = 4,
    // CSS-phase abort (peer left before battle started). Triggers
    // session-stop on the launcher but does NOT contribute to W/L/D --
    // the match never reached battle, so there's nothing to record.
    FM2K_MATCH_OUTCOME_CSS_ABORT   = 5,
    // Game-data hash mismatch on HELLO (#57). Peers' .player/.kgt/.exe
    // rosters disagree, sim would silently desync. Treated like
    // CSS_ABORT for W/L/D (no record), but the launcher shows a
    // distinct toast pointing the user at the hook log's manifest
    // dump so they can find the offending file.
    FM2K_MATCH_OUTCOME_HASH_MISMATCH = 6,
    // GekkoNet caught a state checksum divergence between peers. We
    // terminate the game on the first occurrence to (a) prevent users
    // from playing on through corrupted sim state (which previously
    // led to character_state_machine AVs at 0x4125FC after thousands
    // of frames of cascading garbage) and (b) give us a clean,
    // diagnostic-frozen state to inspect. No W/L/D recorded -- the
    // match's outcome is undefined once sim diverges.
    FM2K_MATCH_OUTCOME_DESYNC      = 7,
};

struct FM2KSharedMemData {
    uint32_t magic;           // FM2K_SHARED_MEM_MAGIC - validates mapping
    uint32_t version;         // Struct version

    uint8_t  player_index;    // 0 = P1/Host, 1 = P2/Client
    uint8_t  netplay_state;   // 0=disconnected, 1=connected, 2=battle
    uint8_t  session_ready;   // GekkoNet session synced
    uint8_t  _pad0;

    uint32_t game_mode;       // 2000=CSS, 3000+=Battle
    uint32_t frame_number;    // Current netplay frame

    uint32_t rollback_count;  // Total rollbacks this session
    uint32_t desync_count;    // Total desyncs detected
    float    frames_ahead;    // Frame advantage (gekko_frames_ahead)
    uint32_t ping_ms;         // NOT wired here -- placeholder, not populated by the hook.
                              // The live RTT the HUD shows comes from fc_hud SetStats
                              // (ControlChannel_GetRttMs), not this field. Don't read this.

    uint32_t rng_seed;        // Current RNG seed (for monitoring)
    uint32_t render_fps;      // Render FPS

    // Match outcome reporting. The hook bumps `match_outcome_seq` once
    // per resolved battle session (Netplay_EndBattle); the launcher polls
    // and notices the bump, reads `match_outcome`, and sends a
    // `match_result` to the hub. seq starts at 0; first real outcome
    // arrives as seq=1. Launcher tracks last-seen-seq locally.
    uint32_t match_outcome_seq;
    uint8_t  match_outcome;   // FM2KMatchOutcome
    uint8_t  _reserved[3];

    // Per-match character indexes captured at battle start (Netplay_
    // StartBattleSession). Persist across the match so the launcher
    // can still read them after Netplay_EndBattle wipes the CSS state.
    // Read live from FM2K::P1_CHARACTER_ID_ADDR / P2_CHARACTER_ID_ADDR
    // when those values are still valid (during CSS → battle), and
    // forwarded to the hub as part of match_result so per-character
    // W/L/D and matchup tables work. 0xFFFFFFFF = unknown / not yet
    // captured. Bumped together with `match_outcome_seq`.
    uint32_t match_p1_char_id;
    uint32_t match_p2_char_id;

    // Resolved .player filenames from FM2K's g_char_slot_data roster
    // (0x435474 in WonderfulWorld; each slot is 256 B CP932). Hook
    // converts CP932 → UTF-8 before publishing so the launcher and
    // hub can hand the bytes through unchanged. Empty (first byte = 0)
    // when the hook hasn't resolved a name yet (e.g. older shared-mem
    // version mid-launch). Bumped together with match_outcome_seq.
    char     match_p1_char_name[FM2K_MATCH_CHAR_NAME_MAX];
    char     match_p2_char_name[FM2K_MATCH_CHAR_NAME_MAX];

    // Selected stage_id captured at battle start (FM2K::ADDR_SELECTED_STAGE).
    // 0xFFFFFFFF = unknown / not captured (FM95: not yet wired).
    // Stage *name* is resolved on the launcher side via FindKgtByGameId
    // because there's no documented in-memory stage-filename table; the
    // launcher already has the parsed KGT, so the hook only ships the id.
    // Bumped together with match_outcome_seq.
    uint32_t match_stage_id;

    // Bumped exactly once per Netplay_StartBattleSession (after chars +
    // stage are published). The launcher gates `match_progress` on
    // this seq advancing, so:
    //   - Inter-battle CSS window: seq doesn't advance, no fire,
    //     lobby keeps the post-rotate empty row → "(in CSS)" visual.
    //   - New battle starts: hook bumps seq, launcher reads new chars
    //     + stage and fires match_progress under the rotated token.
    // Independent from match_outcome_seq (which is per END_battle).
    uint32_t match_chars_seq;

    // Launcher → hook stats feed (#48-extension). The hook renders the
    // game titlebar from here so the user sees their record live in
    // CSS / battle without alt-tabbing to the launcher. Set valid_*
    // sentinels are -1 (== not received yet); a fresh QueryRecord
    // round-trip overwrites with real values.
    int32_t  ui_wins;             // overall W
    int32_t  ui_losses;           // overall L
    int32_t  ui_draws;            // overall D
    int32_t  ui_vs_wins;          // vs current peer
    int32_t  ui_vs_losses;
    int32_t  ui_vs_draws;
    char     ui_peer_nick[64];    // current peer's display name (UTF-8)
    char     ui_my_nick[64];      // own display name (UTF-8)

    // ─── In-game HUD (fc_hud reads, launcher and hook write) ─────────
    //
    // Wholly bidirectional: the launcher pushes for netplay events
    // (peer disconnected, round timer alert, hub-side score updates),
    // the hook pushes for in-game events (round-end Win/Loss/Draw
    // detection, "Round 1, Fight!" trigger). Both writers bump the
    // associated `_seq` field; fc_hud tracks the seq it last consumed
    // so the bar refreshes on any change.
    //
    // The `_seq == 0` sentinel means "never written" -- fc_hud uses it
    // to suppress UI elements that haven't been populated yet (the
    // score box stays hidden offline, system-message overlay stays
    // hidden until something fires).

    uint32_t hud_score_seq;       // bumped on hud_score_p1/p2 change
    uint16_t hud_score_p1;        // round wins for the P1 side
    uint16_t hud_score_p2;        // round wins for the P2 side
    uint16_t hud_spectator_count; // hub-pushed; 0 when offline
    uint16_t hud_pad0;

    // System message: short notice rendered centered inside the game
    // rect with a fade-out near expiry. Use cases: "Round 1, Fight",
    // "Peer disconnected", "Slow CPU detected -- switching to gdi
    // renderer", etc. Writer order: fill the buffer, set the expiry
    // tick (= GetTickCount() at which the message disappears), THEN
    // bump the seq. fc_hud reads in seq → buffer → expiry order.
    uint32_t hud_system_message_seq;
    uint32_t hud_system_message_expiry_tick;  // GetTickCount() value
    char     hud_system_message[160];         // UTF-8, NUL-terminated

    // ─── C10: Hub schema v2 -- session grouping + per-round results ───
    //
    // Hook populates these as battle progresses; launcher reads on
    // match_outcome_seq bump and includes them in the match_result JSON
    // to the hub. session_id ties matches together (one session_id per
    // peer connection), match_index_in_session is 1-based per session.
    //
    // match_rounds[] is filled at every AppendRoundEnd in
    // spectator_node.cpp; match_rounds_count tracks how many entries
    // are valid (0..8). Reset to 0 at MATCH_START so back-to-back
    // matches don't carry over previous match's rounds.
    //
    // session_id == 0 / match_index_in_session == 0 mean "not yet
    // populated" -- launcher omits them from the JSON in that case.
    uint64_t match_session_id;
    uint8_t  match_index_in_session;   // 1-based; 0 if not yet known
    uint8_t  match_rounds_count;       // valid entries in match_rounds[]
    uint8_t  _pad_match_c10[6];        // align match_rounds[] to 8-byte
    FM2KRoundResult match_rounds[8];   // 96 bytes, indexed [0..count-1]

    // Spectator NAT-punch target (launcher → hook channel).
    //
    // When the hub forwards a spectator_incoming event to our launcher,
    // it carries the spectator's external UDP addr. The launcher writes
    // it here (via SharedMem_PublishSpectatorPunchTarget below) and the
    // hook's TickHostMaintenance polls spectator_punch_seq for changes,
    // calling StartPunch toward the addr to open our NAT for the
    // spectator's first JOIN_REQ. Without this, spectators outside our
    // NAT got stuck on "Connecting..." through every reconnect cycle.
    //
    // ip_be is network-byte-order (big-endian) so the hook can pass it
    // straight to StartPunch without re-conversion. seq=0 sentinel for
    // "no punch target queued yet"; first publish sets seq=1.
    uint32_t spectator_punch_ip_be;
    uint16_t spectator_punch_port;       // spectator's external UDP port
    uint16_t spectator_punch_tcp_port;   // spectator's external TCP port (v12)
                                         // -- drives the host-side raw-winsock
                                         // TCP "punch" that opens host's NAT
                                         // for inbound TCP from spec:tcp_port.
                                         // 0 sentinel = TCP punch disabled
                                         // (older hub or non-TCP-capable spec).
    uint32_t spectator_punch_seq;
    // Spec hub-relay user_id (v13). Launcher copies this from the
    // spec_incoming WS event's spec_user_id field whenever the hub
    // forwards a spectator-incoming notification. Hook reads on punch_seq
    // bumps (same poll as the addr block above), stashes in a small
    // (addr -> user_id) dict, and populates Subscriber.spec_user_id when
    // HandleJoinReq accepts the matching JOIN_REQ.
    //
    // Empty string when the hub doesn't include spec_user_id (older hub)
    // OR the launcher hasn't published yet. Relay-mode SendTo skips
    // subscribers with empty user_id (can't address them through the
    // hub); TCP-mode ignores the field entirely.
    char     spectator_punch_user_id[32];

    // Hook → launcher: external TCP addr discovered by SpectatorTCP's
    // PerformTcpStun (outbound connect to hub TCP-STUN endpoint, source-
    // bound to the listener port so the NAT mapping reveals the
    // external-mapped TCP port). Launcher polls tcp_stun_seq for changes
    // and forwards (ip_be, port) to the hub via the WS `tcp_addr`
    // message; hub stores it on user.external_tcp_addr and forwards in
    // spectator_incoming so cross-NAT spectators on non-port-preserving
    // NATs still get the right punch target. seq=0 sentinel for "TCP-STUN
    // hasn't run yet" (e.g. FM2K_HUB_TCP_STUN_ADDR unset).
    uint32_t tcp_stun_ext_ip_be;
    uint16_t tcp_stun_ext_port;
    uint16_t _pad_tcp_stun;
    uint32_t tcp_stun_seq;

    // Hook → launcher: session phase (menu/CSS/battle).
    //
    // Drives the spectator-join /F boot decision: when another player
    // requests to spectate us, the hub forwards our session_kind so
    // their launcher knows whether to set FM2K_BOOT_TO_BATTLE=1 (we're
    // in battle → spec /F-boots straight to battle and applies snapshot)
    // or NOT (we're in CSS → spec walks title→CSS→battle naturally so
    // its engine has CSS surfaces/state ready when the CSS snapshot
    // applies at mode==2000).
    //
    // Updated by Hook_CheckGameModeTransition at every game_mode change:
    //   0 = menu / title / unknown
    //   1 = CSS (game_mode 2000)
    //   2 = battle (game_mode 3000)
    //
    // Launcher polls session_kind_seq for changes and forwards via WS
    // to the hub. Hub stores per-user and includes in spectate_grant.
    uint8_t  session_kind;       // 0=menu, 1=css, 2=battle
    uint8_t  _pad_session_kind[3];
    uint32_t session_kind_seq;

    // Hook → launcher: the client's own GLOBAL IPv6 endpoint (addr + bound
    // UDP port), discovered at NAT init via the connected-UDP source-address
    // trick (the address the OS uses to reach a global v6 destination -- i.e.
    // the address a peer should punch back to, so v6 firewall pinholes align).
    // Launcher polls local_v6_seq for changes and forwards (addr, port) to the
    // hub via WS; the lobby hands it to the peer as an IPv6 punch candidate
    // (StartPunch's peer_v6_addr path). IPv6 usually has no NAT, so when both
    // peers have global v6 this is a DIRECT, relay-free, NAT-free path. All-zero
    // addr + seq=0 = "no global v6" (v4-only host). Mirrors the tcp_stun block.
    uint8_t  local_v6_addr[16];  // in6_addr, network byte order
    uint16_t local_v6_port;      // our bound UDP port, network byte order
    uint16_t _pad_local_v6;
    uint32_t local_v6_seq;
};

// Bumped to 1024 to fit the v9 HUD block. Still under 4 KB which is
// the practical ceiling we agreed on for the shared-mem mapping.
static_assert(sizeof(FM2KSharedMemData) <= 1024, "Keep shared mem small");

// Shared memory lifecycle
bool InitializeSharedMemory();
void CleanupSharedMemory();

// Update shared memory with current stats (call from frame loop)
void SharedMem_Update();

// Get pointer (nullptr if not initialized)
FM2KSharedMemData* GetSharedMemory();

// Publish the outcome of a just-finished battle. Only needs to be called
// once per battle session (from Netplay_EndBattle). Bumps the seq counter
// so the launcher's poll detects the new value as fresh.
void SharedMem_PublishMatchOutcome(FM2KMatchOutcome outcome);

// Stash the P1/P2 character IDs that this battle is being fought with.
// Called from Netplay_StartBattleSession while the CSS-side values are
// still valid; held in shared mem until Netplay_EndBattle so the
// launcher's match_result payload can carry them. p1/p2 = 0xFFFFFFFF
// to clear.
//
// p1_name_utf8 / p2_name_utf8: the resolved .player filename (without
// extension) for each side, already converted to UTF-8 by the caller.
// Pass nullptr to leave the corresponding name slot empty. The buffer
// is truncated to FM2K_MATCH_CHAR_NAME_MAX-1 bytes and null-terminated;
// no validation that the input is well-formed UTF-8 -- callers convert
// from CP932 via Win32 before calling.
void SharedMem_PublishMatchChars(uint32_t p1_char_id, uint32_t p2_char_id,
                                 const char* p1_name_utf8 = nullptr,
                                 const char* p2_name_utf8 = nullptr);

// Stash the selected stage_id for the upcoming battle. Same lifecycle as
// SharedMem_PublishMatchChars -- the launcher reads it at end-of-match
// alongside char_ids/names and forwards as part of the hub match_result.
// Pass 0xFFFFFFFF to clear / mark unknown.
void SharedMem_PublishMatchStage(uint32_t stage_id);

// C10 hub-schema-v2 helpers. Writes are no-ops if the shared mem
// mapping isn't initialized yet (host-side first-match window).
//
// PublishMatchSession: called at MATCH_START (Netplay_StartBattle's
// AppendMatchStart path). session_id is the host-generated u64 from
// SpectatorNode_GetSessionId(); match_index is the 1-based per-session
// counter. Also resets match_rounds_count to 0 for the upcoming match.
void SharedMem_PublishMatchSession(uint64_t session_id,
                                   uint8_t  match_index_in_session);

// PublishRoundResult: called at every ROUND_END
// (SpectatorNode_AppendRoundEnd) to append one entry into
// match_rounds[]. Caps at 8 rounds (excess silently dropped -- far
// more than any sane best-of-N format).
void SharedMem_PublishRoundResult(uint8_t  winner_idx,
                                  uint16_t p1_hp_remaining,
                                  uint16_t p2_hp_remaining,
                                  uint32_t frames_elapsed);

// Spectator NAT-punch coordination -- called from the launcher's
// SpectatorIncoming hub event handler. Writes the spectator's external
// UDP+TCP addr into shared mem and bumps spectator_punch_seq so the
// hook's TickHostMaintenance polling loop notices and fires both the
// UDP heartbeat burst (opens NAT for SPEC_JOIN_REQ replies) and the
// TCP simultaneous-open punch (opens NAT for inbound TCP from
// spec:tcp_port to our listener port). ip_be is network-byte-order;
// udp_port and tcp_port are both host-byte-order. tcp_port=0 sentinel
// for "spec is on an older client without TCP-punch support" -- host
// only does UDP heartbeat in that case.
void SharedMem_PublishSpectatorPunchTarget(uint32_t ip_be, uint16_t udp_port,
                                           uint16_t tcp_port);

// Hook → launcher: SpectatorTCP::PerformTcpStun result. Launcher polls
// tcp_stun_seq for bumps and forwards to the hub via `tcp_addr` WS msg.
// Idempotent -- safe to call repeatedly with the same value (won't bump
// seq if values unchanged).
void SharedMem_PublishExternalTcp(uint32_t ip_be, uint16_t port);

// Hook → launcher: publish the client's own global IPv6 endpoint (16-byte
// in6_addr network-order + bound UDP port network-order). Bumps local_v6_seq
// on change so the launcher forwards it to the hub for the peer's v6 punch
// candidate. Pass all-zero addr to signal "no global v6" (harmless).
void SharedMem_PublishLocalV6(const uint8_t addr[16], uint16_t port_be);

// Publish the current session phase so the launcher can forward to hub.
// Called from Hook_CheckGameModeTransition at every game_mode change.
// Idempotent (no seq bump if kind unchanged).
//   kind: 0=menu/unknown, 1=CSS (game_mode 2000), 2=battle (game_mode 3000)
void SharedMem_PublishSessionKind(uint8_t kind);

// Launcher-side write hook for the v6 stats feed. The launcher process
// opens the same FM2K_SharedMem_<pid> mapping read-write and stuffs
// W-L-D + peer nick fields here whenever Hub::QueryRecord returns a
// fresh record or MatchStart names a new opponent. The game-side hook
// reads from these fields when formatting the in-game titlebar.
//
// Called from launcher; safe to call from hook side too (will just
// re-stamp its own copy). Pass -1 for any wins/losses/draws value to
// mean "not yet known"; pass nullptr for nicks to leave them blank.
void SharedMem_PublishUiStats(int32_t wins, int32_t losses, int32_t draws,
                              int32_t vs_wins, int32_t vs_losses, int32_t vs_draws,
                              const char* peer_nick_utf8,
                              const char* my_nick_utf8);

// ─── HUD publishers (Slices B/C/D) ────────────────────────────────────
//
// Each one updates the relevant fields, then bumps the `*_seq` so
// fc_hud's per-frame read picks up the change. Safe to call from any
// thread; idempotent (writing the same values again is a no-op apart
// from a wasted seq bump).

// Set the round-win score shown in the HUD's center score box.
// Hidden when this has never been called (seq=0). Pass {0,0} to
// reset visible scores at match start.
void SharedMem_PublishHudScores(uint16_t p1, uint16_t p2);

// Push a system message (centered overlay) with a TTL in milliseconds.
// `text` is UTF-8; truncated to 159 bytes + NUL on overflow.
// `ttl_ms == 0` immediately clears any pending message.
void SharedMem_PublishHudSystemMessage(const char* text_utf8, uint32_t ttl_ms);

// Update the spectator count shown in the HUD bar.
void SharedMem_PublishHudSpectatorCount(uint16_t n);
