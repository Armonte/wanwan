#pragma once
// LauncherUI::HubState definition, moved out of FM2K_LauncherUI.cpp so the
// split launcher_ui_*.cpp TUs (hub/netcfg/notify + core) that dereference
// hub_state_ all see the full type. Kept in this INTERNAL header (not the
// public FM2K_Integration.h) so FM2K_HubClient.h stays off the public surface.
#include "FM2K_Integration.h"
#include "FM2K_HubClient.h"
#include <string>
#include <vector>
#include <unordered_map>

// Local-only state owned by LauncherUI for the Hub panel. Defined here
// rather than in the header to keep FM2K_HubClient.h out of the public
// integration surface. unique_ptr<HubState> destructor needs the full
// type, which it has thanks to this definition + the LauncherUI dtor
// living in this file (line 82 onwards).
struct LauncherUI::HubState {
    fm2k::HubClient client;
    // Stealth/ghost mode (persisted to dev_flags.ini "stealth_mode"). Mirrors
    // client.SetStealth(); the Hub-panel checkbox drives both.
    bool stealth = false;
    std::string my_id;
    std::string my_nick;
    std::string current_room_id;
    // Snapshot of the room we were in at the moment the WS dropped.
    // Used by the Connected handler to auto-rejoin after a hub
    // restart / network blip; cleared once the rejoin call is sent.
    std::string last_room_id;
    std::string last_room_name;

    std::vector<fm2k::HubRoom> rooms;                     // discovered rooms
    std::unordered_map<std::string, fm2k::HubUser> users; // users in current room
    std::string pending_challenge_from_id;
    std::string pending_challenge_from_nick;
    // Match settings the challenger sent us (#54). Rendered in the
    // accept modal so the user sees what they're agreeing to. Sentinel
    // -1 across the struct = challenger didn't send any (older client).
    fm2k::MatchSettings pending_challenge_settings;
    // Hub-authoritative settings for the active hub-driven match. Set
    // on K::MatchStart from ev.match.settings; consumed by the launch
    // path (FM2KLauncher::StartOnlineSession via on_online_session_start
    // can't see this directly -- we expose it via the existing
    // network_config_ piggyback below). Random-stage env vars are
    // derived from `random_seed`/`random_stage_*`.
    fm2k::MatchSettings current_match_settings;
    std::string status_line;
    bool show_challenge_modal = false;

    // Outbound challenge state -- populated when WE click Challenge on
    // somebody, cleared when the hub tells us the outcome (declined,
    // cancelled, failed, or match_start). Drives the "Waiting for X..."
    // modal so the challenger gets feedback instead of a silent UI.
    std::string outgoing_challenge_to_id;
    std::string outgoing_challenge_to_nick;
    bool        show_outgoing_challenge_modal = false;

    // Hash-mismatch popup state. Set when the hook publishes a
    // FM2K_MATCH_OUTCOME_HASH_MISMATCH; cleared when the user
    // dismisses the modal. The log_excerpt is the most recent
    // "GameHash: manifest" block read from the spawned game's hook
    // log so the popup can show *what* hashed (per-file name|size|
    // content_hash) and the user knows which file to compare against
    // their peer.
    bool        show_hash_mismatch_modal = false;
    std::string hash_mismatch_log_excerpt;

    // Active hub-driven match. Set on K::MatchStart from ev.match.token,
    // cleared once we publish a match_result to the hub. Used by the
    // shared-mem poll path so a single bump of `match_outcome_seq` in
    // the hook turns into exactly one outbound match_result. Empty when
    // we're not in a hub match (offline / dev / spectator).
    std::string current_match_token;
    // Per-match peer + role snapshot -- stashed on K::MatchStart so the
    // local results.csv writer (#42) can render the row from MY
    // perspective even after the hub modal / users-list state has moved
    // on. role is "host" (we're P1) or "guest" (we're P2); peer_nick
    // is the opponent's display name as the hub gave it to us.
    std::string current_match_role;
    std::string current_match_peer_id;
    std::string current_match_peer_nick;
    std::string current_match_game_id;
    // Per-process last-seen outcome seq, keyed by PID. The hook starts
    // seq at 0; first real outcome arrives as 1. We send match_result
    // when the value we read is greater than what's stored here, then
    // bump our copy to that value so subsequent identical reads (the
    // shared mem stays at the last value forever) don't re-send.
    std::unordered_map<uint32_t, uint32_t> last_outcome_seq;

    // My own overall W/L/D, populated by hub `record` events and used
    // for the launcher titlebar. (-1, -1, -1) means we haven't received
    // a record yet -- render no titlebar suffix in that case.
    int my_wins   = -1;
    int my_losses = -1;
    int my_draws  = -1;
    // Per-launcher-session counters (Patrick's bug -- wanted "current
    // session record" alongside the overall in the titlebar). These
    // increment on every committed match_result (self_won / peer_won /
    // draw) and reset only on launcher process restart. Never reset
    // across hub disconnect / reconnect, since the user expects "this
    // play session" to span their entire afternoon. Disconnect outcomes
    // don't count (consistent with hub-side ledger).
    int session_wins   = 0;
    int session_losses = 0;
    int session_draws  = 0;
    // Per-opponent record from MY perspective ("how I've done vs them").
    // Keyed by opponent hub user_id; populated from the `vs_breakdown`
    // attached to the record event when we issue an unfiltered
    // QueryRecord. Empty until first record arrives or for opponents
    // we've never played.
    struct VsCell { int wins = 0, losses = 0, draws = 0; };
    std::unordered_map<std::string, VsCell> my_vs;
    // Recent matches (#49). Hub answers RequestRecentMatches with at
    // most N rows ordered newest-first. Mirrored here so the "Recent
    // Matches" window can render from cached state without re-hitting
    // the hub on every render.
    std::vector<fm2k::HubEvent::MatchRow> recent_matches;
    bool recent_matches_loaded = false;
    // Currently-in-flight matches (lobby panel). Snapshot from
    // RequestCurrentMatches on connect; live updates from
    // MatchInProgressStarted/Updated/Ended broadcasts. Sorted by
    // started_at ascending (newest at the bottom) on each render.
    std::vector<fm2k::HubEvent::MatchInProgress> current_matches;
    bool current_matches_loaded = false;
    // Per-PID last-seen match_chars_seq from the hook. The hook bumps
    // this counter exactly once per Netplay_StartBattleSession (after
    // chars + stage are published). Launcher fires match_progress only
    // when this counter advances, so during the inter-battle CSS
    // window -- where shared mem still holds the prev battle's data --
    // no spurious match_progress is sent and the lobby's "(in CSS)"
    // row stays clean until the new battle actually starts.
    std::unordered_map<uint32_t, uint32_t> last_chars_seq;
    // True once the hook has published an outcome that resolved the
    // current match (any of self_won/peer_won/draw/disconnect). Reset
    // on the next K::MatchStart so back-to-back matches don't share a
    // stale flag.
    bool        match_result_sent = false;

    // Match results we couldn't send because the hub WS was disconnected
    // at outcome time (long pause → keepalive timeout → WS drop is the
    // classic trigger -- multiple users reported wins not counting after
    // long pauses). On K::Connected we drain this queue and ship every
    // entry. Local CSV is written EITHER WAY (PollMatchOutcome writes
    // it before checking WS state), so the per-user record stays
    // accurate even if the hub is permanently down.
    struct PendingMatchResult {
        std::string token;
        std::string outcome;
        uint32_t    p1_char_id   = 0xFFFFFFFFu;
        uint32_t    p2_char_id   = 0xFFFFFFFFu;
        std::string p1_char_name;
        std::string p2_char_name;
        uint32_t    stage_id     = 0xFFFFFFFFu;
        std::string stage_name;
        uint64_t    session_id   = 0;
        uint8_t     match_index  = 0;
        std::vector<fm2k::HubClient::RoundJson> rounds;
    };
    std::vector<PendingMatchResult> pending_match_results;
    // De-dupe for peer-disconnected toasts. Three independent paths
    // can fire one for a single dropout: (a) the hook publishes a
    // DISCONNECT outcome via shared mem, (b) the hub sends a UserUpdate
    // moving the peer out of "in_match", (c) the hub sends a peer-
    // disconnected event when the peer's WS closes. Whichever lands
    // first sets this flag; the other two skip. Reset on MatchStart.
    bool        disconnect_toast_fired = false;

    // Wall-clock SDL_GetTicks() of the last successful pre-match STUN.
    // Refreshed every ~20 s by the lobby tick so the hub-stored external
    // port stays alive -- most home NATs idle-time UDP mappings out
    // around 30 s, so 20 s gives one re-bind worth of headroom.
    uint32_t    last_stun_refresh_ms = 0;
    // Last NAT classification reported to the hub (Phase 2a). The STUN probe
    // now doubles as a classification probe; we only re-send the nat_type WS
    // update when the result changes (it's stable on a given network), so a
    // quiet lobby doesn't spam udp_addr updates every refresh.
    std::string last_nat_type;
};
