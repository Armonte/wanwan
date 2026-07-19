// FM2K Hub client — WinHTTP WebSocket transport.
//
// One I/O thread does the WS handshake then spawns a sender thread.
// The I/O thread itself owns the receive loop. Both push events
// onto a thread-safe inbox; the launcher's UI thread drains via
// HubClient::Poll() once per frame.
//
// JSON encode/decode is deliberately minimal — the message catalog
// in docs/FM2K_Matchmaking_Design.md §15.2 is small enough that
// hand-rolled extractors are simpler than vendoring a JSON lib.
// If that catalog grows, swap in nlohmann/json.

// WinHTTP WebSocket APIs (WinHttpWebSocketCompleteUpgrade etc.) are
// gated on _WIN32_WINNT >= 0x0602 (Windows 8). Project-wide setting
// is 0x0601 (Win7) for compatibility; bump only this TU.
#ifdef _WIN32_WINNT
#  undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0602
#ifdef WINVER
#  undef WINVER
#endif
#define WINVER 0x0602

#include "FM2K_HubClient.h"
#include "version_local.h"  // fm2k::kAppVersion
#include <winhttp.h>

#include <SDL3/SDL_log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

#include "FM2K_HubClient_internal.h"

namespace fm2k {

void HubClient::OnMessage(const std::string& msg) {
    std::string type = GetStr(msg, "type");
    HubEvent ev;

    if (type == "ping") {
        EnqueueOut("{\"type\":\"pong\"}");
        return;
    }

    if (type == "hello_ack") {
        connected_.store(true);
        ev.kind = HubEvent::Kind::Connected;
        ev.user_id = GetStr(msg, "user_id");
        std::string rooms_arr = GetSub(msg, "rooms");
        if (!rooms_arr.empty()) {
            for (auto& obj : SplitObjectArray(rooms_arr)) ev.rooms.push_back(ParseRoom(obj));
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "room_list") {
        ev.kind = HubEvent::Kind::RoomList;
        std::string rooms_arr = GetSub(msg, "rooms");
        for (auto& obj : SplitObjectArray(rooms_arr)) ev.rooms.push_back(ParseRoom(obj));
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "room_joined") {
        ev.kind = HubEvent::Kind::RoomJoined;
        std::string room_obj = GetSub(msg, "room");
        if (!room_obj.empty()) {
            HubRoom r = ParseRoom(room_obj);
            ev.room_id = r.id;
            ev.rooms.push_back(std::move(r));
        }
        std::string users_arr = GetSub(msg, "users");
        for (auto& uobj : SplitObjectArray(users_arr)) ev.users.push_back(ParseUser(uobj));
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "room_left") {
        ev.kind = HubEvent::Kind::RoomLeft;
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "user_joined") {
        ev.kind = HubEvent::Kind::UserJoined;
        ev.room_id = GetStr(msg, "room_id");
        ev.user = ParseUser(GetSub(msg, "user"));
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "user_left") {
        ev.kind = HubEvent::Kind::UserLeft;
        ev.room_id = GetStr(msg, "room_id");
        ev.user_id = GetStr(msg, "user_id");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "user_status") {
        ev.kind = HubEvent::Kind::UserStatus;
        ev.user = ParseUser(GetSub(msg, "user"));
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "user_rtt") {
        ev.kind = HubEvent::Kind::UserRtt;
        ev.user_id = GetStr(msg, "user_id");
        ev.rtt_ms  = GetInt(msg, "rtt_ms", 0);
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "challenge_received") {
        ev.kind = HubEvent::Kind::ChallengeReceived;
        ev.challenge.from_id   = GetStr(msg, "from_id");
        ev.challenge.from_nick = GetStr(msg, "from_nick");
        ev.challenge.room_id   = GetStr(msg, "room_id");
        // Optional match_settings sub-object (#54). Omitted by older
        // launchers; sentinel -1 across the struct flags "unknown" so
        // the accept modal falls back to the existing "this match
        // will use the host's defaults" wording.
        std::string ms = GetSub(msg, "match_settings");
        if (!ms.empty()) {
            auto& s = ev.challenge.settings;
            s.player0_cpu      = GetInt(ms, "player0_cpu",      -1);
            s.player1_cpu      = GetInt(ms, "player1_cpu",      -1);
            s.game_speed       = GetInt(ms, "game_speed",       -1);
            s.hit_judge        = GetInt(ms, "hit_judge",        -1);
            s.game_information = GetInt(ms, "game_information", -1);
            s.stage_nb         = GetInt(ms, "stage_nb",         -1);
            s.joystick         = GetInt(ms, "joystick",         -1);
            s.time             = GetInt(ms, "time",             -1);
            s.exit_flag        = GetInt(ms, "exit_flag",        -1);
            s.vs_mode          = GetInt(ms, "vs_mode",          -1);
            s.vs_single_play   = GetInt(ms, "vs_single_play",   -1);
            s.vs_team_play     = GetInt(ms, "vs_team_play",     -1);
            const int seed_signed = GetInt(ms, "random_seed", 0);
            s.random_seed         = static_cast<uint32_t>(seed_signed);
            s.random_stage_min    = GetInt(ms, "random_stage_min", -1);
            s.random_stage_max    = GetInt(ms, "random_stage_max", -1);
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "challenge_failed") {
        ev.kind = HubEvent::Kind::ChallengeFailed;
        ev.error = GetStr(msg, "reason");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "challenge_cancelled") {
        ev.kind = HubEvent::Kind::ChallengeCancelled;
        ev.user_id = GetStr(msg, "by_id");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "challenge_declined") {
        ev.kind = HubEvent::Kind::ChallengeDeclined;
        ev.user_id = GetStr(msg, "by_id");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "match_start") {
        ev.kind = HubEvent::Kind::MatchStart;
        ev.match.token = GetStr(msg, "token");
        ev.match.role  = GetStr(msg, "role");
        ev.match.game_id = GetStr(msg, "game_id");  // empty on old hubs
        std::string peer_obj = GetSub(msg, "peer");
        ev.match.peer = ParseUser(peer_obj);
        std::string udp = GetSub(peer_obj, "udp_addr");
        if (!udp.empty()) {
            // udp_addr is either a [ip, port] tuple or null. We sent a
            // tuple via udp_addr; the hub forwards the (ip, port) pair.
            // hub.py serializes Python tuple → JSON array, so parse as
            // ["1.2.3.4", 12345].
            if (udp.front() == '[' || udp.front() == '"') {
                // Pull first quoted string and first integer in the substring.
                size_t a = udp.find('"');
                size_t b = (a == std::string::npos) ? a : udp.find('"', a + 1);
                if (a != std::string::npos && b != std::string::npos) {
                    ev.match.peer_udp_ip = udp.substr(a + 1, b - a - 1);
                }
                size_t c = udp.find(',');
                if (c != std::string::npos) ev.match.peer_udp_port = std::atoi(udp.c_str() + c + 1);
            }
        }
        // BUG 2: did the hub VERIFY the peer's reflexive port? Default true so
        // old hubs (no key) behave as before. False -> the hook won't burst it.
        ev.match.peer_udp_verified = GetBool(peer_obj, "udp_verified", true);
        // Peer's same-LAN candidate: hub forwards "local": [ip, port] when the
        // peer self-reported a LAN IP (same array shape as udp_addr). Used to
        // also-punch the peer's private addr for same-router pairs.
        std::string loc = GetSub(peer_obj, "local");
        if (!loc.empty() && (loc.front() == '[' || loc.front() == '"')) {
            size_t a = loc.find('"');
            size_t b = (a == std::string::npos) ? a : loc.find('"', a + 1);
            if (a != std::string::npos && b != std::string::npos) {
                ev.match.peer_lan_ip = loc.substr(a + 1, b - a - 1);
            }
            size_t c = loc.find(',');
            if (c != std::string::npos) {
                ev.match.peer_lan_port = std::atoi(loc.c_str() + c + 1);
            }
        }

        // Peer's GLOBAL IPv6 candidate: hub forwards "v6": [addr, port] (same
        // array shape as local). The address is a QUOTED v6 literal, so its
        // inner colons stay inside the quotes and the comma after the closing
        // quote cleanly splits addr from port. Direct v6 bypasses CGNAT.
        std::string v6s = GetSub(peer_obj, "v6");
        if (!v6s.empty() && (v6s.front() == '[' || v6s.front() == '"')) {
            size_t a = v6s.find('"');
            size_t b = (a == std::string::npos) ? a : v6s.find('"', a + 1);
            if (a != std::string::npos && b != std::string::npos) {
                ev.match.peer_v6_ip = v6s.substr(a + 1, b - a - 1);
            }
            size_t c = v6s.find(',');
            if (c != std::string::npos) {
                ev.match.peer_v6_port = std::atoi(v6s.c_str() + c + 1);
            }
        }

        std::string ws_addr = GetSub(peer_obj, "ws_addr");
        ev.match.peer_ws_addr = ws_addr;

        // Optional relay fallback advertised by the hub.
        std::string relay_obj = GetSub(msg, "relay");
        if (!relay_obj.empty()) {
            std::string addr_arr = GetSub(relay_obj, "addr");
            if (!addr_arr.empty()) {
                size_t a = addr_arr.find('"');
                size_t b = (a == std::string::npos) ? a : addr_arr.find('"', a + 1);
                if (a != std::string::npos && b != std::string::npos) {
                    ev.match.relay_ip = addr_arr.substr(a + 1, b - a - 1);
                }
                size_t c = addr_arr.find(',');
                if (c != std::string::npos) {
                    ev.match.relay_port = std::atoi(addr_arr.c_str() + c + 1);
                }
            }
            ev.match.relay_session_id = GetStr(relay_obj, "session_id");
        }

        // Optional match_settings echoed by hub on match_start so both
        // peers (host + guest) apply the same authoritative config at
        // game-spawn time. Empty/absent on legacy hubs.
        std::string ms = GetSub(msg, "match_settings");
        if (!ms.empty()) {
            auto& s = ev.match.settings;
            s.player0_cpu      = GetInt(ms, "player0_cpu",      -1);
            s.player1_cpu      = GetInt(ms, "player1_cpu",      -1);
            s.game_speed       = GetInt(ms, "game_speed",       -1);
            s.hit_judge        = GetInt(ms, "hit_judge",        -1);
            s.game_information = GetInt(ms, "game_information", -1);
            s.stage_nb         = GetInt(ms, "stage_nb",         -1);
            s.joystick         = GetInt(ms, "joystick",         -1);
            s.time             = GetInt(ms, "time",             -1);
            s.exit_flag        = GetInt(ms, "exit_flag",        -1);
            s.vs_mode          = GetInt(ms, "vs_mode",          -1);
            s.vs_single_play   = GetInt(ms, "vs_single_play",   -1);
            s.vs_team_play     = GetInt(ms, "vs_team_play",     -1);
            // Random-stage fields (#56). random_seed parsed as 64-bit
            // signed because GetInt returns int; a 32-bit unsigned seed
            // (high bit set) would otherwise wrap negative. Re-cast to
            // uint32 on assign so xorshift seeding sees the same value
            // on both peers.
            const int seed_signed = GetInt(ms, "random_seed", 0);
            s.random_seed         = static_cast<uint32_t>(seed_signed);
            s.random_stage_min    = GetInt(ms, "random_stage_min", -1);
            s.random_stage_max    = GetInt(ms, "random_stage_max", -1);
        }

        EmitEvent(std::move(ev));
        return;
    }

    if (type == "spectate_grant") {
        ev.kind = HubEvent::Kind::SpectateGranted;
        ev.spectate.target_id     = GetStr(msg, "target_id");
        ev.spectate.target_nick   = GetStr(msg, "target_nick");
        ev.spectate.opponent_id   = GetStr(msg, "opponent_id");
        ev.spectate.opponent_nick = GetStr(msg, "opponent_nick");
        ev.spectate.host_ip       = GetStr(msg, "host_ip");
        ev.spectate.host_port     = GetInt(msg, "host_port", 0);
        {
            std::string sk = GetStr(msg, "session_kind");
            if (sk == "menu" || sk == "css" || sk == "battle") {
                ev.spectate.session_kind = std::move(sk);
            }
            // else: hub didn't supply — keep default "menu" from struct init
        }
        {
            // Phase 4: pick up host's spec_transport so we can match
            // the mode on the spec game spawn. "tcp" or "relay" only;
            // anything else is treated as default tcp.
            std::string st = GetStr(msg, "spec_transport");
            if (st == "tcp" || st == "relay") {
                ev.spectate.spec_transport = std::move(st);
            }
        }
        {
            // Optional spec relay fallback (task #58): same block shape as
            // match_start's "relay" -- {"addr": [host, port], "session_id"}.
            std::string sr = GetSub(msg, "spec_relay");
            if (!sr.empty()) {
                std::string addr_arr = GetSub(sr, "addr");
                std::string ip; int port = 0;
                if (!addr_arr.empty()) {
                    size_t a = addr_arr.find('"');
                    size_t b = (a == std::string::npos) ? a : addr_arr.find('"', a + 1);
                    if (a != std::string::npos && b != std::string::npos) {
                        ip = addr_arr.substr(a + 1, b - a - 1);
                    }
                    size_t c = addr_arr.find(',');
                    if (c != std::string::npos) {
                        port = std::atoi(addr_arr.c_str() + c + 1);
                    }
                }
                if (!ip.empty() && port > 0) {
                    ev.spectate.spec_relay_addr = ip + ":" + std::to_string(port);
                    ev.spectate.spec_relay_session = GetStr(sr, "session_id");
                }
            }
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "spectate_denied") {
        ev.kind = HubEvent::Kind::SpectateDenied;
        ev.spectate.target_id = GetStr(msg, "target_id");
        ev.error              = GetStr(msg, "reason");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "spectator_incoming") {
        ev.kind = HubEvent::Kind::SpectatorIncoming;
        ev.spectator_incoming.spec_user_id  = GetStr(msg, "spec_user_id");
        ev.spectator_incoming.spec_nick     = GetStr(msg, "spec_nick");
        ev.spectator_incoming.spec_udp_ip   = GetStr(msg, "spec_udp_ip");
        ev.spectator_incoming.spec_udp_port = GetInt(msg, "spec_udp_port", 0);
        ev.spectator_incoming.spec_tcp_port = GetInt(msg, "spec_tcp_port", 0);
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "peer_disconnected") {
        ev.kind = HubEvent::Kind::PeerDisconnected;
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "match_rotated") {
        // Hub minted a fresh in-flight match-id for the next FM2K
        // round in the same hub_session (no re-spawn — peers are
        // already in CSS). We just forward the new token; the
        // launcher updates current_match_token + resets per-match
        // flags so the next outcome publish commits cleanly.
        ev.kind = HubEvent::Kind::MatchRotated;
        ev.match.token = GetStr(msg, "new_token");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "record") {
        ev.kind = HubEvent::Kind::RecordReceived;
        ev.record.user_id     = GetStr(msg, "user_id");
        ev.record.opponent_id = GetStr(msg, "opponent_id");
        ev.record.game_id     = GetStr(msg, "game_id");
        ev.record.wins        = GetInt(msg, "wins", 0);
        ev.record.losses      = GetInt(msg, "losses", 0);
        ev.record.draws       = GetInt(msg, "draws", 0);
        std::string vs_arr    = GetSub(msg, "vs_breakdown");
        if (!vs_arr.empty()) {
            for (auto& obj : SplitObjectArray(vs_arr)) {
                HubEvent::VsRow row;
                row.opponent_id   = GetStr(obj, "opponent_id");
                row.opponent_nick = GetStr(obj, "opponent_nick");
                row.wins   = GetInt(obj, "wins", 0);
                row.losses = GetInt(obj, "losses", 0);
                row.draws  = GetInt(obj, "draws", 0);
                ev.record.vs_breakdown.push_back(std::move(row));
            }
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "recent_matches") {
        ev.kind = HubEvent::Kind::RecentMatchesReceived;
        std::string arr = GetSub(msg, "matches");
        if (!arr.empty()) {
            for (auto& obj : SplitObjectArray(arr)) {
                HubEvent::MatchRow row;
                row.id        = GetStr(obj, "id");
                row.p1_id     = GetStr(obj, "p1_id");
                row.p1_nick   = GetStr(obj, "p1_nick");
                row.p2_id     = GetStr(obj, "p2_id");
                row.p2_nick   = GetStr(obj, "p2_nick");
                row.game_id   = GetStr(obj, "game_id");
                row.winner_id = GetStr(obj, "winner_id");
                // finished_at is a float in JSON; GetInt lossy but
                // sufficient for sorting / display purposes.
                row.finished_at = (double)GetInt(obj, "finished_at", 0);
                // Char + stage fields — server sends null when peers
                // disagreed/omitted; GetInt returns the default (-1)
                // for null so the row stays clean for UI fallbacks.
                row.p1_char_id   = GetInt(obj, "p1_char_id", -1);
                row.p2_char_id   = GetInt(obj, "p2_char_id", -1);
                row.p1_char_name = GetStr(obj, "p1_char_name");
                row.p2_char_name = GetStr(obj, "p2_char_name");
                row.stage_id     = GetInt(obj, "stage_id", -1);
                row.stage_name   = GetStr(obj, "stage_name");
                ev.recent_matches.push_back(std::move(row));
            }
        }
        EmitEvent(std::move(ev));
        return;
    }

    // In-progress match payloads — used by the lobby panel. Snapshot
    // form (current_matches response, list of objects) and live forms
    // (match_in_progress_started/updated, single object) parse the
    // same shape via this helper.
    auto parse_in_progress = [](const std::string& obj) -> HubEvent::MatchInProgress {
        HubEvent::MatchInProgress r;
        r.token        = GetStr(obj, "token");
        r.p1_id        = GetStr(obj, "p1_id");
        r.p1_nick      = GetStr(obj, "p1_nick");
        r.p2_id        = GetStr(obj, "p2_id");
        r.p2_nick      = GetStr(obj, "p2_nick");
        r.game_id      = GetStr(obj, "game_id");
        r.started_at   = (double)GetInt(obj, "started_at", 0);
        r.p1_char_id   = GetInt(obj, "p1_char_id", -1);
        r.p2_char_id   = GetInt(obj, "p2_char_id", -1);
        r.p1_char_name = GetStr(obj, "p1_char_name");
        r.p2_char_name = GetStr(obj, "p2_char_name");
        r.stage_id     = GetInt(obj, "stage_id", -1);
        r.stage_name   = GetStr(obj, "stage_name");
        return r;
    };

    if (type == "current_matches") {
        ev.kind = HubEvent::Kind::CurrentMatchesReceived;
        std::string arr = GetSub(msg, "matches");
        if (!arr.empty()) {
            for (auto& obj : SplitObjectArray(arr)) {
                ev.current_matches.push_back(parse_in_progress(obj));
            }
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "match_in_progress_started" ||
        type == "match_in_progress_updated") {
        ev.kind = (type == "match_in_progress_started")
                  ? HubEvent::Kind::MatchInProgressStarted
                  : HubEvent::Kind::MatchInProgressUpdated;
        std::string sub = GetSub(msg, "match");
        if (!sub.empty()) {
            ev.current_match_update = parse_in_progress(sub);
        }
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "match_in_progress_ended") {
        ev.kind = HubEvent::Kind::MatchInProgressEnded;
        ev.current_match_token = GetStr(msg, "token");
        EmitEvent(std::move(ev));
        return;
    }

    if (type == "error") {
        // Server-issued error — most commonly auth_required when the
        // launcher connects without a valid Discord hub_token. Surface
        // both reason and detail so the UI can show something useful.
        const std::string reason = GetStr(msg, "reason");
        const std::string detail = GetStr(msg, "detail");
        std::string combined = reason;
        if (!detail.empty()) {
            if (!combined.empty()) combined += ": ";
            combined += detail;
        }
        if (combined.empty()) combined = "hub error";
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "HubClient: hub error — %s", combined.c_str());
        ev.kind  = HubEvent::Kind::Error;
        ev.error = std::move(combined);
        EmitEvent(std::move(ev));
        return;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "HubClient: unhandled message type='%s'", type.c_str());
}

}  // namespace fm2k
