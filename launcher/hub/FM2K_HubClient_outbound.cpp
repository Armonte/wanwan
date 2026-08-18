// FM2K Hub client -- WinHTTP WebSocket transport.
//
// One I/O thread does the WS handshake then spawns a sender thread.
// The I/O thread itself owns the receive loop. Both push events
// onto a thread-safe inbox; the launcher's UI thread drains via
// HubClient::Poll() once per frame.
//
// JSON encode/decode is deliberately minimal -- the message catalog
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

void HubClient::SetStealth(bool on) {
    stealth_.store(on);
    // Live update while connected: the hub flips User.stealth and re-broadcasts
    // the lobby so we appear/disappear immediately, no reconnect. When not
    // connected this sends nothing -- the value rides the next hello (built in
    // IoThread from stealth_). Hub side handler: "set_stealth" in hub.py.
    if (connected_.load()) {
        EnqueueOut(std::string("{\"type\":\"set_stealth\",\"stealth\":")
                   + (on ? "true" : "false") + "}");
    }
}

void HubClient::SendUdpAddr(const std::string& ip, int port,
                            const std::string& local_ip,
                            const std::string& v6) {
    std::string m = "{\"type\":\"udp_addr\",\"ip\":\"" + EscapeJsonString(ip)
                  + "\",\"port\":" + std::to_string(port);
    // local_ip: same-LAN candidate. Old hubs ignore the unknown key.
    if (!local_ip.empty()) {
        m += ",\"local_ip\":\"" + EscapeJsonString(local_ip) + "\"";
    }
    // v6: global IPv6 host candidate (the CGNAT bypass). Old hubs ignore it.
    if (!v6.empty()) {
        m += ",\"v6\":\"" + EscapeJsonString(v6) + "\"";
    }
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::SendUdpAddrUpnp(const std::string& ip, int port,
                               const std::string& ext_ip, int ext_udp_port,
                               bool upnp, const std::string& local_ip,
                               const std::string& v6) {
    // Same base shape as SendUdpAddr (ip/port) plus the optional UPnP
    // fields. Kept as a separate method so the plain call sites stay
    // untouched and the wire payload only grows the ext_* keys on the
    // post-mapping re-send. ext_ip is JSON-escaped like every other string
    // we emit; ext_udp_port is a plain int; upnp is a JSON bool literal.
    std::string m = "{\"type\":\"udp_addr\",\"ip\":\"" + EscapeJsonString(ip)
                  + "\",\"port\":" + std::to_string(port);
    if (!ext_ip.empty()) {
        m += ",\"ext_ip\":\"" + EscapeJsonString(ext_ip) + "\"";
    }
    if (ext_udp_port > 0) {
        m += ",\"ext_udp_port\":" + std::to_string(ext_udp_port);
    }
    // local_ip persists across the UPnP re-send (the hub would otherwise keep
    // the value from the first SendUdpAddr, but include it for robustness).
    if (!local_ip.empty()) {
        m += ",\"local_ip\":\"" + EscapeJsonString(local_ip) + "\"";
    }
    if (!v6.empty()) {
        m += ",\"v6\":\"" + EscapeJsonString(v6) + "\"";
    }
    m += ",\"upnp\":";
    m += (upnp ? "true" : "false");
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::SendNatType(const std::string& nat_type) {
    // Minimal udp_addr update carrying only the NAT classification (Phase
    // 2a). The hub's udp_addr handler reads "nat_type" independently of the
    // ip/port/upnp fields, so we don't need to re-send those here -- the
    // primary SendUdpAddr already registered them. Validate locally too so a
    // bogus value never goes on the wire.
    if (nat_type != "cone" && nat_type != "symmetric" &&
        nat_type != "blocked" && nat_type != "unknown") {
        return;
    }
    std::string m = "{\"type\":\"udp_addr\",\"nat_type\":\""
                  + EscapeJsonString(nat_type) + "\"}";
    EnqueueOut(std::move(m));
}


void HubClient::ListRooms() {
    EnqueueOut("{\"type\":\"list_rooms\"}");
}

void HubClient::JoinRoom(const std::string& game_id, const std::string& display_name) {
    std::string m = "{\"type\":\"join_room\",\"game_id\":\"" + EscapeJsonString(game_id) + "\"";
    if (!display_name.empty()) {
        m += ",\"name\":\"" + EscapeJsonString(display_name) + "\"";
    }
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::LeaveRoom() {
    EnqueueOut("{\"type\":\"leave_room\"}");
}

void HubClient::Challenge(const std::string& target_id) {
    EnqueueOut("{\"type\":\"challenge\",\"target_id\":\"" + EscapeJsonString(target_id) + "\"}");
}

void HubClient::Challenge(const std::string& target_id,
                          const MatchSettings& s) {
    // Build a flat match_settings object. Skip kUnset (-1) fields so
    // the wire payload stays compact and the hub forwards a smaller
    // blob to the target. Older launchers without the optional
    // fields just see the existing challenge_received shape.
    std::string m = "{\"type\":\"challenge\",\"target_id\":\"" +
                    EscapeJsonString(target_id) + "\"";
    bool any = false;
    auto add = [&](const char* key, int v) {
        if (v == -1) return;
        m += (any ? "," : ",\"match_settings\":{");
        any = true;
        m += "\"";
        m += key;
        m += "\":";
        m += std::to_string(v);
    };
    add("player0_cpu",      s.player0_cpu);
    add("player1_cpu",      s.player1_cpu);
    add("game_speed",       s.game_speed);
    add("hit_judge",        s.hit_judge);
    add("game_information", s.game_information);
    add("stage_nb",         s.stage_nb);
    add("joystick",         s.joystick);
    add("time",             s.time);
    add("exit_flag",        s.exit_flag);
    add("vs_mode",          s.vs_mode);
    add("vs_single_play",   s.vs_single_play);
    add("vs_team_play",     s.vs_team_play);
    // Random-stage extension (#56). random_seed == 0 means off; we
    // skip emitting any random_* fields when off so older hubs see
    // the same payload they did pre-#56.
    if (s.random_seed != 0) {
        // Cast to int for the JSON int writer; reinterpret the same
        // bits on the receiver (uint32_t = static_cast<uint32_t>(int)).
        m += (any ? "," : ",\"match_settings\":{");
        any = true;
        m += "\"random_seed\":" + std::to_string((int)s.random_seed);
    }
    add("random_stage_min", s.random_stage_min);
    add("random_stage_max", s.random_stage_max);
    if (any) m += "}";
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::CancelChallenge(const std::string& target_id) {
    EnqueueOut("{\"type\":\"cancel_challenge\",\"target_id\":\"" + EscapeJsonString(target_id) + "\"}");
}

void HubClient::AcceptChallenge(const std::string& challenger_id) {
    EnqueueOut("{\"type\":\"accept_challenge\",\"challenger_id\":\"" + EscapeJsonString(challenger_id) + "\"}");
}

void HubClient::DeclineChallenge(const std::string& challenger_id) {
    EnqueueOut("{\"type\":\"decline_challenge\",\"challenger_id\":\"" + EscapeJsonString(challenger_id) + "\"}");
}

void HubClient::MatchEnded() {
    EnqueueOut("{\"type\":\"match_ended\"}");
}

void HubClient::UpdateSessionKind(const std::string& kind) {
    EnqueueOut("{\"type\":\"session_kind\",\"value\":\"" +
               EscapeJsonString(kind) + "\"}");
}

void HubClient::MatchResult(const std::string& match_id,
                            const std::string& outcome) {
    EnqueueOut("{\"type\":\"match_result\",\"match_id\":\"" +
               EscapeJsonString(match_id) + "\",\"outcome\":\"" +
               EscapeJsonString(outcome) + "\"}");
}

void HubClient::MatchResult(const std::string& match_id,
                            const std::string& outcome,
                            uint32_t p1_char_id, uint32_t p2_char_id) {
    MatchResult(match_id, outcome, p1_char_id, p2_char_id,
                std::string{}, std::string{});
}

void HubClient::MatchResult(const std::string& match_id,
                            const std::string& outcome,
                            uint32_t p1_char_id, uint32_t p2_char_id,
                            const std::string& p1_char_name,
                            const std::string& p2_char_name) {
    MatchResult(match_id, outcome, p1_char_id, p2_char_id,
                p1_char_name, p2_char_name,
                0xFFFFFFFFu, std::string{});
}

void HubClient::MatchResult(const std::string& match_id,
                            const std::string& outcome,
                            uint32_t p1_char_id, uint32_t p2_char_id,
                            const std::string& p1_char_name,
                            const std::string& p2_char_name,
                            uint32_t stage_id,
                            const std::string& stage_name) {
    std::string m = "{\"type\":\"match_result\",\"match_id\":\"" +
                    EscapeJsonString(match_id) + "\",\"outcome\":\"" +
                    EscapeJsonString(outcome) + "\"";
    if (p1_char_id != 0xFFFFFFFFu) {
        m += ",\"p1_char_id\":" + std::to_string(p1_char_id);
    }
    if (p2_char_id != 0xFFFFFFFFu) {
        m += ",\"p2_char_id\":" + std::to_string(p2_char_id);
    }
    if (!p1_char_name.empty()) {
        m += ",\"p1_char_name\":\"" + EscapeJsonString(p1_char_name) + "\"";
    }
    if (!p2_char_name.empty()) {
        m += ",\"p2_char_name\":\"" + EscapeJsonString(p2_char_name) + "\"";
    }
    if (stage_id != 0xFFFFFFFFu) {
        m += ",\"stage_id\":" + std::to_string(stage_id);
    }
    if (!stage_name.empty()) {
        m += ",\"stage_name\":\"" + EscapeJsonString(stage_name) + "\"";
    }
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::MatchResult(const std::string& match_id,
                            const std::string& outcome,
                            uint32_t p1_char_id, uint32_t p2_char_id,
                            const std::string& p1_char_name,
                            const std::string& p2_char_name,
                            uint32_t stage_id,
                            const std::string& stage_name,
                            uint64_t session_id,
                            uint8_t  match_index_in_session,
                            const std::vector<RoundJson>& rounds) {
    // Fall back to schema-1 if no session metadata. Keeps the wire light
    // for legacy paths (e.g. early hook builds where session_id == 0).
    if (session_id == 0 && match_index_in_session == 0 && rounds.empty()) {
        MatchResult(match_id, outcome, p1_char_id, p2_char_id,
                    p1_char_name, p2_char_name, stage_id, stage_name);
        return;
    }

    std::string m = "{\"type\":\"match_result\",\"schema\":2,\"match_id\":\"" +
                    EscapeJsonString(match_id) + "\",\"outcome\":\"" +
                    EscapeJsonString(outcome) + "\"";
    if (p1_char_id != 0xFFFFFFFFu) {
        m += ",\"p1_char_id\":" + std::to_string(p1_char_id);
    }
    if (p2_char_id != 0xFFFFFFFFu) {
        m += ",\"p2_char_id\":" + std::to_string(p2_char_id);
    }
    if (!p1_char_name.empty()) {
        m += ",\"p1_char_name\":\"" + EscapeJsonString(p1_char_name) + "\"";
    }
    if (!p2_char_name.empty()) {
        m += ",\"p2_char_name\":\"" + EscapeJsonString(p2_char_name) + "\"";
    }
    if (stage_id != 0xFFFFFFFFu) {
        m += ",\"stage_id\":" + std::to_string(stage_id);
    }
    if (!stage_name.empty()) {
        m += ",\"stage_name\":\"" + EscapeJsonString(stage_name) + "\"";
    }
    // session_id as a 16-char hex string for compactness + readability in
    // the matches.json log (the high 32 bits are unix epoch seconds, low
    // 32 bits a random nonce -- see SpectatorNode_AppendSessionId).
    char sid_buf[32];
    std::snprintf(sid_buf, sizeof(sid_buf), "%016llx",
                  static_cast<unsigned long long>(session_id));
    m += ",\"session_id\":\"";
    m += sid_buf;
    m += "\"";
    if (match_index_in_session > 0) {
        m += ",\"match_index_in_session\":" +
             std::to_string(static_cast<unsigned>(match_index_in_session));
    }
    if (!rounds.empty()) {
        m += ",\"rounds\":[";
        for (size_t i = 0; i < rounds.size(); ++i) {
            if (i > 0) m += ",";
            const RoundJson& r = rounds[i];
            const char* who = (r.winner_idx == 0) ? "p1"
                            : (r.winner_idx == 1) ? "p2" : "draw";
            m += "{\"winner\":\"";
            m += who;
            m += "\",\"frames\":" + std::to_string(r.frames_elapsed);
            m += ",\"p1_hp_left\":" + std::to_string(r.p1_hp_remaining);
            m += ",\"p2_hp_left\":" + std::to_string(r.p2_hp_remaining);
            m += "}";
        }
        m += "]";
    }
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::QueryRecord(const std::string& opponent_id,
                            const std::string& game_id) {
    std::string m = "{\"type\":\"query_record\"";
    if (!opponent_id.empty())
        m += ",\"opponent_id\":\"" + EscapeJsonString(opponent_id) + "\"";
    if (!game_id.empty())
        m += ",\"game_id\":\"" + EscapeJsonString(game_id) + "\"";
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::RequestRecentMatches(int limit) {
    if (limit < 1) limit = 50;
    EnqueueOut("{\"type\":\"recent_matches\",\"limit\":" +
               std::to_string(limit) + "}");
}

void HubClient::RequestCurrentMatches() {
    EnqueueOut("{\"type\":\"current_matches\"}");
}

void HubClient::ReportMatchProgress(const std::string& match_id,
                                    uint32_t p1_char_id, uint32_t p2_char_id,
                                    const std::string& p1_char_name,
                                    const std::string& p2_char_name,
                                    uint32_t stage_id,
                                    const std::string& stage_name) {
    std::string m = "{\"type\":\"match_progress\",\"match_id\":\"" +
                    EscapeJsonString(match_id) + "\"";
    if (p1_char_id != 0xFFFFFFFFu) {
        m += ",\"p1_char_id\":" + std::to_string(p1_char_id);
    }
    if (p2_char_id != 0xFFFFFFFFu) {
        m += ",\"p2_char_id\":" + std::to_string(p2_char_id);
    }
    if (!p1_char_name.empty()) {
        m += ",\"p1_char_name\":\"" + EscapeJsonString(p1_char_name) + "\"";
    }
    if (!p2_char_name.empty()) {
        m += ",\"p2_char_name\":\"" + EscapeJsonString(p2_char_name) + "\"";
    }
    if (stage_id != 0xFFFFFFFFu) {
        m += ",\"stage_id\":" + std::to_string(stage_id);
    }
    if (!stage_name.empty()) {
        m += ",\"stage_name\":\"" + EscapeJsonString(stage_name) + "\"";
    }
    m += "}";
    EnqueueOut(std::move(m));
}

void HubClient::RequestSpectate(const std::string& target_id) {
    EnqueueOut("{\"type\":\"spectate_request\",\"target_id\":\"" +
               EscapeJsonString(target_id) + "\"}");
}

// ----- transport: I/O thread -----

}  // namespace fm2k
