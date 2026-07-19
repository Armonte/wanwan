#pragma once

// FM2K Hub client — WebSocket transport for hub.py.
// Phase-1 scaffold; talks the JSON protocol documented in
// docs/FM2K_Matchmaking_Design.md §15.2. No persistence, no auth.
//
// TODO(cross-platform): the implementation in FM2K_HubClient.cpp uses
// WinHTTP (Windows-only). The public API in this header is platform-
// agnostic. When we go Linux, swap the .cpp out for a libcurl /
// libwebsockets / asio-beast backend; the launcher UI and integration
// surface won't need to change.

#include <windows.h>
// NOTE: <winhttp.h> is NOT included here. It conflicts with <wininet.h>
// which is pulled in by FM2K_LauncherUI.cpp (both define INTERNET_SCHEME,
// URL_COMPONENTS, etc.). The public API only needs HINTERNET, which is
// already defined in <windows.h>. WinHTTP-specific use is contained in
// FM2K_HubClient.cpp.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fm2k {

struct HubUser {
    std::string id;
    std::string nick;
    std::string room_id;
    std::string status;        // "idle" | "challenging" | "in_match"
    std::string opponent_id;
    int rtt_ms = 0;
    // Patreon tier label sent by hub. "tester" ($5 — blue name) or
    // "thanks" ($10 Special Thanks — gold name). Empty string for legacy
    // hubs that don't include the field; the launcher treats empty/unknown
    // values as plain (no recolor).
    std::string tier;
};

struct HubRoom {
    std::string id;            // == game_id
    std::string name;
    int user_count = 0;
};

// Mirror of fm2k::game_ini::GamePlayConfig — kept in this header
// (instead of pulling FM2K_GameIni.h into the public hub-client API)
// so external consumers don't have to depend on the launcher's INI
// module just to inspect a challenge payload. Sentinel -1 = unset.
//
// random_seed != 0 (#56) signals "random stage enabled — both peers
// seed an xorshift PRNG with this value and roll a fresh stage at
// every Netplay_StartBattle. Lilith-equivalent semantics: one seed
// agreed at challenge time, deterministic re-rolls on rematches."
// random_stage_min / random_stage_max bound the roll; both peers
// must compute (next_xorshift() % (max-min+1)) + min identically.
struct MatchSettings {
    int player0_cpu      = -1;
    int player1_cpu      = -1;
    int game_speed       = -1;
    int hit_judge        = -1;
    int game_information = -1;
    int stage_nb         = -1;
    int joystick         = -1;
    int time             = -1;
    int exit_flag        = -1;
    int vs_mode          = -1;
    int vs_single_play   = -1;
    int vs_team_play     = -1;
    // Random-stage extension. Off when random_seed == 0 (legacy +
    // default). Range is INCLUSIVE on both ends.
    uint32_t random_seed       = 0;
    int      random_stage_min  = -1;
    int      random_stage_max  = -1;
};

// Events the launcher polls. Only carry the data the UI needs;
// transport / timing details stay inside HubClient.
struct HubEvent {
    enum class Kind {
        Connected,             // hello_ack
        Disconnected,          // socket dropped or local Disconnect()
        RoomList,              // initial / list_rooms response
        RoomJoined,            // self joined a room (carries user list)
        RoomLeft,
        UserJoined,
        UserLeft,
        UserStatus,            // status / opponent change
        UserRtt,               // RTT update
        ChallengeReceived,     // someone challenged us
        ChallengeFailed,       // ours failed (target unavailable)
        ChallengeCancelled,    // challenger cancelled
        ChallengeDeclined,     // target declined
        MatchStart,            // both sides go into punch + GekkoNet
        MatchRotated,          // hub minted fresh token for next FM2K match in same session
        PeerDisconnected,
        SpectateGranted,       // hub returned host addr for our spectate request
        SpectateDenied,        // hub rejected spectate (target not in_match etc)
        SpectatorIncoming,     // we're the host; hub forwarded a spectator's UDP
                               // addr so we can fire a NAT punch toward them
                               // before their first JOIN_REQ arrives
        SpecRelayBinary,       // Phase 3: hub forwarded a spec data frame to
                               // us (we're the spec). Bytes are SpecDataHeader-
                               // prefixed wire payload; launcher writes them
                               // into the inbound shared-mem ring for the
                               // game hook to drain.
        RecordReceived,        // hub answered our query_record (W/L/D)
        RecentMatchesReceived, // hub answered our recent_matches request
        CurrentMatchesReceived,// hub answered our current_matches request
        MatchInProgressStarted,// public broadcast: an in-flight match began
        MatchInProgressUpdated,// public broadcast: chars/stage filled in
        MatchInProgressEnded,  // public broadcast: in-flight match resolved
        Error,
    };

    Kind kind{Kind::Error};
    std::string error;

    // Generic payloads — only the fields relevant to a given Kind are set.
    std::vector<HubRoom> rooms;
    std::vector<HubUser> users;
    HubUser user;
    std::string user_id;       // for UserLeft / UserRtt
    std::string room_id;
    int rtt_ms = 0;

    struct {
        std::string from_id;
        std::string from_nick;
        std::string room_id;
        // Host's resolved [GamePlay] config (#54). Forwarded by the
        // hub from the challenger's `challenge` message so the target
        // can display "this match will be: 3 rounds, 60s timer, stage
        // 2…" before they accept. Anti-cheat clamps already applied
        // launcher-side. Sentinel -1 across the struct means the
        // challenger ran an older launcher that didn't include them.
        MatchSettings settings;
    } challenge;

    struct {
        std::string token;
        std::string role;      // "host" | "guest"
        // Actual launchable game id (exe stem) from the hub. Prefer this
        // over the room id when spawning: the room can be a canonical
        // family alias ("URORFG") that matches no local install
        // (URORFGRelease103). Empty on old hubs -> fall back to room id.
        std::string game_id;
        HubUser peer;
        std::string peer_udp_ip;
        int peer_udp_port = 0;
        // BUG 2: false when the hub could not VERIFY the peer's reflexive UDP
        // port (UDP STUN blocked -> it advertised a local-port guess a
        // port-rewriting CGNAT will have remapped). Default true so old hubs /
        // a missing key behave as before. When false the hook skips bursting
        // that port and leans on peer-learning + v6 + relay.
        bool peer_udp_verified = true;
        std::string peer_ws_addr;
        // Peer's same-LAN candidate (its private 192.168/10/172.16 addr),
        // forwarded by the hub from the peer's local_ip. Set FM2K_PEER_LAN_ADDR
        // so the hook also punches it -- same-house pairs go direct over the LAN.
        std::string peer_lan_ip;
        int peer_lan_port = 0;
        // Peer's GLOBAL IPv6 host candidate + its local UDP port, forwarded by
        // the hub from the peer's v6. Set FM2K_PEER_V6_ADDR so the hook also
        // punches it -- direct v6 bypasses CGNAT entirely (no NAT, no STUN).
        std::string peer_v6_ip;
        int peer_v6_port = 0;
        // Relay fallback. Hub fills these on match_start so the hook
        // can switch to relay mode if direct punch fails. Empty/zero
        // means hub didn't advertise a relay (older hub or disabled).
        std::string relay_ip;
        int         relay_port = 0;
        std::string relay_session_id;   // 32-hex-char string (= match token bytes)
        // Host's resolved [GamePlay] config (#54 followup). The hub
        // remembers what the challenger sent on `challenge` and echoes
        // it here so BOTH peers see the same authoritative settings
        // (the challenged user already saw it in their accept modal,
        // but the host needs it back too if their own override file
        // changed between sending the challenge and seeing match_start).
        // Used by ApplyForLaunch to write game.ini before spawn.
        MatchSettings settings;
    } match;

    // Spectate-grant payload. host_ip / host_port are where the spectator
    // FM2K should aim its FM2K_REMOTE_ADDR; target_nick / opponent_nick
    // are advisory for UI labelling ("Watching A vs B").
    struct {
        std::string target_id;
        std::string target_nick;
        std::string opponent_id;
        std::string opponent_nick;
        std::string host_ip;
        int         host_port = 0;
        // session_kind — "menu" / "css" / "battle". The hub relays the
        // host's current game phase (host hook publishes on every
        // game_mode transition). Spec launcher uses it to decide
        // FM2K_BOOT_TO_BATTLE: "battle" → /F (instant join via snapshot
        // apply); "css" → walk title→CSS for the mid-CSS-join handshake;
        // "menu" → walk title→CSS naturally (host's CSS-entry transition
        // will arrive over the wire). Default "menu" only fires if the
        // hub field is absent — shouldn't happen in practice.
        std::string session_kind = "menu";
        // spec_transport — Phase 4 of v0.3 spec rebuild. Echoes the
        // target host's hello spec_transport so the spec launcher can
        // set FM2K_SPEC_TRANSPORT on the spec game spawn matching the
        // host's mode. Default "tcp" matches legacy behavior for hosts
        // (and hubs) that don't advertise the field.
        std::string spec_transport = "tcp";
        // spec_relay -- task #58 RC-over-relay fallback. Hub mints a
        // host-transparent relay session with every grant (0.2.83+ hubs)
        // and sends {"addr": [host, port], "session_id": 32-hex}. The
        // spec launcher exports these as FM2K_HUB_RELAY_ADDR/SESSION on
        // the spectator spawn; the viewer hook auto-falls-back onto the
        // relay when its direct RC join produces no admits. Empty =
        // hub didn't offer one (old hub / same-NAT grant) = no fallback.
        std::string spec_relay_addr;     // "host:port", already joined
        std::string spec_relay_session;  // 32-hex session id
    } spectate;

    // SpecRelayBinary payload (Phase 3). Hub forwarded a SpecDataHeader-
    // prefixed wire frame to us; launcher writes into the inbound shared-
    // mem ring for the spec hook to dispatch. Bytes are exactly what
    // SpectatorNode_HandleSpecData expects.
    std::vector<uint8_t> spec_relay_bytes;

    // SpectatorIncoming payload — we're the host; hub forwarded the
    // spectator's external UDP addr so we can fire a NAT-punch packet
    // to it before the spectator's first JOIN_REQ arrives. Without this
    // step the spectator's UDP packet hits our NAT and gets dropped
    // (no inbound state for the new source). Symptom users hit pre-fix:
    // spectator sits on "Connecting..." through 14 reconnect cycles.
    struct {
        std::string spec_user_id;   // hub's id for the spectator
        std::string spec_nick;
        std::string spec_udp_ip;    // post-STUN external IPv4 dotted
        int         spec_udp_port = 0;
        int         spec_tcp_port = 0;  // 0 if older spec client
    } spectator_incoming;

    // RecordReceived payload. Filled when Kind::RecordReceived fires.
    // user_id is whose record this is (always the asker for now). If
    // opponent_id is set, the {wins,losses,draws} are filtered to that
    // opponent. vs_breakdown is always the top-32-by-match-count global
    // rows for the asker.
    struct VsRow {
        std::string opponent_id;
        std::string opponent_nick;
        int wins = 0, losses = 0, draws = 0;
    };
    struct {
        std::string user_id;
        std::string opponent_id;   // empty = global record
        std::string game_id;       // empty = all games
        int wins = 0, losses = 0, draws = 0;
        std::vector<VsRow> vs_breakdown;
    } record;

    // RecentMatchesReceived payload — newest match first.
    struct MatchRow {
        std::string id;
        std::string p1_id;
        std::string p1_nick;
        std::string p2_id;
        std::string p2_nick;
        std::string game_id;
        std::string winner_id;     // empty = draw
        double      finished_at = 0.0;
        // Char + stage detail. -1 / empty mean "missing" (peers
        // disagreed or didn't report — older client). UI uses
        // fm2k::FormatCharLabel / FormatStageLabel which fall back
        // gracefully to "Char #N" / "Stage #N" when names are absent.
        int32_t     p1_char_id   = -1;
        int32_t     p2_char_id   = -1;
        std::string p1_char_name;
        std::string p2_char_name;
        int32_t     stage_id     = -1;
        std::string stage_name;
    };
    std::vector<MatchRow> recent_matches;

    // In-progress match payload. Used by both the snapshot response
    // (`current_matches`) and the live broadcasts (`match_in_progress_*`).
    // -1 / empty char/stage fields mean the match-progress report
    // hasn't arrived yet (battle still in CSS, or the peer's launcher
    // is older). `started_at` is unix seconds from the hub clock.
    struct MatchInProgress {
        std::string token;
        std::string p1_id;
        std::string p1_nick;
        std::string p2_id;
        std::string p2_nick;
        std::string game_id;
        double      started_at = 0.0;
        int32_t     p1_char_id   = -1;
        int32_t     p2_char_id   = -1;
        std::string p1_char_name;
        std::string p2_char_name;
        int32_t     stage_id     = -1;
        std::string stage_name;
    };
    // CurrentMatchesReceived snapshot — full list. Live updates use
    // `current_match_update` (Started/Updated) or `current_match_token`
    // (Ended; only the token to evict).
    std::vector<MatchInProgress> current_matches;
    MatchInProgress              current_match_update;
    std::string                  current_match_token;
};

class HubClient {
public:
    HubClient();
    ~HubClient();

    HubClient(const HubClient&) = delete;
    HubClient& operator=(const HubClient&) = delete;

    // Begin async connect. Returns true on dispatch (thread launched);
    // actual connection state lands as a Connected/Disconnected event.
    // host: bare hostname or IP (no scheme). port: TCP. path: "/".
    // hub_token: opaque string the launcher caches after Discord OAuth
    // sign-in. The hub validates it on hello and rejects connections
    // with a `auth_required` error if empty/invalid. Pass "" to attempt
    // an unauthenticated connect (only works against a hub started
    // with FM2K_HUB_AUTH_DISABLE=1).
    // secure=true upgrades the WebSocket to TLS (wss://) — required
    // when fronting through Caddy / a reverse proxy that terminates
    // HTTPS. Defaults to false for the legacy direct-port path.
    bool Connect(const std::string& host, uint16_t port, const std::string& path,
                 const std::string& nick, const std::string& hub_token,
                 bool secure = false);
    // Compat overload — defaults hub_token to empty.
    bool Connect(const std::string& host, uint16_t port, const std::string& path,
                 const std::string& nick) {
        return Connect(host, port, path, nick, std::string{});
    }

    void Disconnect();

    bool IsConnected() const { return connected_.load(std::memory_order_acquire); }

    // Stealth / "ghost" mode. When on, the WS hello carries "stealth":true and
    // the hub keeps this user's match + characters out of the lobby + public
    // stats (privacy for testing unreleased builds). Set BEFORE Connect -- the
    // hello is sent once at connect, so toggling mid-session applies on the next
    // (re)connect. Env FM2K_STEALTH=1 also forces it on. Thread-safe.
    // Set stealth on/off. Stores the flag for the next hello AND, if currently
    // connected, sends a live "set_stealth" message so the hub flips us in/out
    // of the lobby immediately (no reconnect). Safe to call from the UI thread.
    void SetStealth(bool on);
    bool GetStealth() const { return stealth_.load(); }

    // Drain pending events on the UI thread. Call once per frame.
    void Poll(const std::function<void(const HubEvent&)>& on_event);

    // Outbound. Safe to call any time after Connect; queued if not yet
    // connected, sent in order once the upgrade completes.
    // local_ip (optional): this machine's LAN IPv4 (fm2k::LocalLanIp()). The
    // hub relays it as the peer's same-LAN candidate so two players behind the
    // same router connect directly over the LAN. Empty = omit (backward-compat).
    // v6 (optional): this machine's GLOBAL IPv6 host candidate
    // (fm2k::LocalLanIpV6()). The hub relays it so the peer can connect
    // directly over IPv6, bypassing CGNAT entirely. Empty = omit (backward).
    void SendUdpAddr(const std::string& ip, int port, int tcp_port = -1,
                     const std::string& local_ip = "",
                     const std::string& v6 = "");

    // UPnP-extended udp_addr (Phase 1 NAT reachability). Sent as a SECOND
    // udp_addr after the PortMapper completes a router mapping: carries the
    // IGD-reported external endpoint so the hub can advertise it to a peer
    // as an authoritative any-source inbound mapping (D5 precedence:
    // verified-UPnP > STUN-learned > client-claimed). The first
    // SendUdpAddr(ip,port,tcp_port) still fires immediately at connect time
    // with upnp absent, so behavior is unchanged when UPnP is slow/absent.
    //
    // ext_ip / ext_udp_port are the WAN side the router granted; upnp=true
    // flags the claim. Old hubs ignore the unknown fields (GetSub/GetStr
    // return empty), so this is backward-compatible. The hub additionally
    // CGNAT-guards the claim by comparing ext_ip against the WS-source IP
    // (D6) before honoring the port.
    void SendUdpAddrUpnp(const std::string& ip, int port, int tcp_port,
                         const std::string& ext_ip, int ext_udp_port,
                         bool upnp, const std::string& local_ip = "",
                         const std::string& v6 = "");

    // NAT classification report (Phase 2a). Sent as a small udp_addr update
    // carrying just the nat_type string once the launcher's dual STUN probe
    // resolves (cone/symmetric/blocked/unknown -- see
    // fm2k::LauncherStunClassify). The hub validates against that set, stores
    // user.nat_type, and exposes it per-peer in match_start (peer_dict) so the
    // hook logs see it and Phase 3 can orient the match. Separate from
    // SendUdpAddr / SendUdpAddrUpnp so the classification result can be sent
    // on its own schedule (it resolves a beat after the primary STUN). Old
    // hubs ignore the unknown nat_type field -- backward compatible.
    void SendNatType(const std::string& nat_type);

    // External TCP addr discovered by the spec hook via hub TCP-STUN
    // (see FM2KHook/src/netplay/spectator_tcp.cpp PerformTcpStun). Sent
    // separately from SendUdpAddr because TCP-STUN is async — the
    // hook's outbound STUN connect happens after Init, the launcher
    // polls SharedMem for the result, and forwards once it arrives.
    void SendTcpAddr(const std::string& ip, int port);
    void ListRooms();
    void JoinRoom(const std::string& game_id, const std::string& display_name = "");
    void LeaveRoom();
    void Challenge(const std::string& target_id);
    // Variant — includes the host's resolved [GamePlay] config so the
    // target sees what they're agreeing to in the accept modal (#54).
    // Hub forwards `settings` verbatim; older hubs / launchers safely
    // ignore the extra fields. -1 in any int means "not set".
    void Challenge(const std::string& target_id,
                   const MatchSettings& settings);
    void CancelChallenge(const std::string& target_id);
    void AcceptChallenge(const std::string& challenger_id);
    void DeclineChallenge(const std::string& challenger_id);
    void MatchEnded();

    // Publish our session phase (menu/CSS/battle) to the hub. Used to
    // populate user state so that when someone requests to spectate us,
    // the hub can return our current session_kind in `spectate_grant`,
    // letting the spec launcher decide whether to /F-boot-to-battle
    // (we're in battle) or do natural CSS-walk (we're in CSS — needed
    // for the CSS snapshot to apply with the correct surfaces/state).
    //   kind ∈ {"menu", "css", "battle"}
    void UpdateSessionKind(const std::string& kind);

    // Send a spec hub-relay binary frame (Phase 2c). `frame` is the
    // SpecDataBinary wire format documented in
    // docs/dev/spec_hub_relay_design.md and parsed by hub.py's
    // handle_spec_relay_frame -- header + routing block + payload.
    // Caller (the spec relay drain loop in FM2K_RollbackClient) builds
    // the bytes from a fm2k::spec_relay::Slot before passing here.
    //
    // Queued on the same outbox as JSON control messages so cross-
    // message ordering is preserved per MSDN's "serialize sends per
    // handle" guidance.
    void SendSpecRelayFrame(std::vector<uint8_t> frame);

    // Report end-of-match outcome to the hub for stats tracking. `match_id`
    // is the token from the `match_start` event we received (== relay
    // session_id). `outcome` is one of: "self_won" | "peer_won" | "draw"
    // | "disconnect". Hub correlates both peers' reports by match_id and
    // commits to the persistent match log only when they agree.
    //
    // The 4-arg overload also carries P1/P2 character IDs captured at
    // battle start (FM2K::P1/P2_CHARACTER_ID_ADDR). Pass 0xFFFFFFFFu to
    // omit a side. Per-character stats (matchup tables, char-specific
    // W/L/D) on the hub side need both reports to agree on the IDs;
    // mismatches log a warning and fall back to global counters only.
    void MatchResult(const std::string& match_id, const std::string& outcome);
    void MatchResult(const std::string& match_id, const std::string& outcome,
                     uint32_t p1_char_id, uint32_t p2_char_id);
    // Full overload — adds resolved .player filenames (UTF-8) so the
    // hub can store human-readable char names alongside the IDs.
    // Empty strings are omitted from the JSON payload to save bytes.
    void MatchResult(const std::string& match_id, const std::string& outcome,
                     uint32_t p1_char_id, uint32_t p2_char_id,
                     const std::string& p1_char_name,
                     const std::string& p2_char_name);
    // Full overload — adds stage_id + resolved stage_name (UTF-8). Stage
    // name is resolved on the launcher via FindKgtByGameId because FM2K
    // has no in-memory stage-filename table; pass empty string when the
    // game isn't installed locally and the hub will store id-only.
    // Pass 0xFFFFFFFFu / empty string to omit either field.
    void MatchResult(const std::string& match_id, const std::string& outcome,
                     uint32_t p1_char_id, uint32_t p2_char_id,
                     const std::string& p1_char_name,
                     const std::string& p2_char_name,
                     uint32_t stage_id,
                     const std::string& stage_name);

    // C10 — schema-2 overload. Adds session correlation + per-round results.
    // session_id is the 64-bit token generated at peer-connect time on the
    // host (shared across every match this pair plays until disconnect);
    // match_index_in_session is 1-based (1 = first match of the session).
    // rounds[] carries per-round mini-records sized to the actual count
    // (FM2K caps rounds at 8). Pass session_id == 0 to fall back to the
    // schema-1 wire shape (omits all session/round fields).
    struct RoundJson {
        uint8_t  winner_idx;       // 0=p1, 1=p2, 2=draw
        uint16_t p1_hp_remaining;
        uint16_t p2_hp_remaining;
        uint32_t frames_elapsed;
    };
    void MatchResult(const std::string& match_id, const std::string& outcome,
                     uint32_t p1_char_id, uint32_t p2_char_id,
                     const std::string& p1_char_name,
                     const std::string& p2_char_name,
                     uint32_t stage_id,
                     const std::string& stage_name,
                     uint64_t session_id,
                     uint8_t  match_index_in_session,
                     const std::vector<RoundJson>& rounds);

    // Ask hub for our W/L/D record. Both args optional — pass empty
    // string to omit. Hub responds with K::RecordReceived carrying
    // wins/losses/draws + per-opponent breakdown.
    void QueryRecord(const std::string& opponent_id = "",
                     const std::string& game_id     = "");

    // Ask hub for our most-recent N matches. Hub responds with
    // K::RecentMatchesReceived.
    void RequestRecentMatches(int limit = 50);

    // Ask hub for the snapshot of all currently-in-flight matches. Hub
    // responds with K::CurrentMatchesReceived. Live updates after the
    // snapshot land as MatchInProgressStarted/Updated/Ended events. Sent
    // typically once on connect + on room rejoin so a freshly-opened
    // launcher's lobby panel populates without waiting for the next
    // accept_challenge to fire a Started broadcast.
    void RequestCurrentMatches();

    // Report this peer's view of the live char/stage state for an
    // in-flight match. Called from the launcher right after the hook
    // publishes match_p1_char_id / etc. to shared mem at battle start.
    // The hub merges (last writer wins) and broadcasts Updated to all
    // lobby viewers. Pass 0xFFFFFFFFu / empty string for fields the
    // local launcher couldn't resolve (game not installed locally).
    void ReportMatchProgress(const std::string& match_id,
                             uint32_t p1_char_id, uint32_t p2_char_id,
                             const std::string& p1_char_name,
                             const std::string& p2_char_name,
                             uint32_t stage_id,
                             const std::string& stage_name);

    // Ask hub to introduce us to the host of an in-progress match.
    // target_id is the host user's id (the user we want to spectate).
    // Hub responds with SpectateGranted / SpectateDenied.
    void RequestSpectate(const std::string& target_id);

private:
    void EnqueueOut(std::string msg);
    void IoThread(std::string host, uint16_t port, std::string path, std::string nick);
    void OnMessage(const std::string& msg);
    void EmitEvent(HubEvent ev);

    void CleanupHandles();

    // HINTERNET is a typedef in <winhttp.h>/<wininet.h>, neither of
    // which we want to expose. Store as opaque void* and cast at the
    // use sites in FM2K_HubClient.cpp.
    void* session_ = nullptr;
    void* conn_    = nullptr;
    void* req_     = nullptr;
    void* ws_      = nullptr;

    std::thread io_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    // Discord OAuth hub_token, supplied at Connect() time and embedded
    // in the WS hello payload. Empty when running against a hub that
    // disables auth (FM2K_HUB_AUTH_DISABLE=1 server-side).
    std::string hub_token_;
    bool        use_tls_ = false;  // wss:// when true
    std::atomic<bool> stealth_{false};  // SetStealth(); read into the hello in IoThread

    // Outbox holds both UTF-8 JSON control messages AND binary frames
    // (spec hub-relay, Phase 2c). The sender thread inspects is_binary
    // to pick WINHTTP_WEB_SOCKET_{UTF8,BINARY}_MESSAGE_BUFFER_TYPE.
    // Same queue + same thread keeps the cross-message ordering well-
    // defined (per MSDN, serializing WS sends per handle is recommended
    // to avoid intermixing on the wire).
    struct OutMsg {
        std::vector<uint8_t> data;
        bool                 is_binary;
    };
    std::mutex out_mtx_;
    std::condition_variable out_cv_;
    std::deque<OutMsg> outbox_;

    std::mutex in_mtx_;
    std::deque<HubEvent> inbox_;
};

}  // namespace fm2k
