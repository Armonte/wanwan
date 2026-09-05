#include "host_hub.h"
#include "host_discord.h"   // DiscordHubToken -- the cached sign-in
#include "host_launch.h"    // LaunchOnline -- BOOT for a hub match
#include "FM2K_MatchOutcome.h"

#include "FM2K_HubClient.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
void slint_ui_set_peers(const char* utf8);
void slint_ui_set_chat(const char* utf8);
void slint_ui_set_chat_notice(const char* utf8);
void slint_ui_set_connected(int on);
void slint_ui_set_status(const char* utf8);
void slint_ui_set_match(int stage, const char* nick, int rtt, int seconds,
                        int sync_pct, const char* detail);
void slint_ui_set_scene(int scene);
}

namespace hostgui {
namespace {

struct ChatLine {
    long long   ts = 0;
    std::string user_id;
    std::string nick;
    std::string text;
    bool        system = false;
};

struct Bridge {
    fm2k::HubClient client;
    bool            started = false;
    std::string     my_id;
    std::string     my_nick;
    std::string     want_room;        // the room to sit in: the selected game's stem
    std::string     want_room_title;  // what to call it on the wire
    std::string     room_id;
    // The library, for match_start (HubSetLibrary).
    std::vector<std::string> lib_stems, lib_paths, lib_engines;
    // Who the open challenge is with. Accept and decline have to name the
    // challenger on the wire, and the modal itself only carries a display
    // name -- which is not a key.
    std::string     pending_from_id;
    std::string     outgoing_to_id;
    // Who the match in progress is actually against. MatchStart clears the
    // challenge ids, so without latching this the booting card and the
    // result card have no opponent to name -- which is how a fixture nick
    // ended up hardcoded into the live end-of-match path.
    // The live hub match: its token (rotated per rematch), whether its
    // result went out, and the hook's sequence counters per game pid --
    // never reset on rotation (they are per process, see the header).
    std::string     match_token;
    bool            result_sent = false;
    std::map<uint32_t, uint32_t> last_outcome_seq, last_chars_seq;
    std::string     match_nick;
    int             match_rtt = 0;
    std::map<std::string, fm2k::HubUser> users;
    // Chat PER ROOM, keyed by the hub's canonical room id. The rail
    // watches several rooms at once (subscribe_rooms), so their lines
    // arrive interleaved and each has to land in its own backlog --
    // otherwise switching rooms would show someone else's conversation.
    // The view shows the JOINED room (b.room_id); the rest accumulate so
    // a switch is instant rather than empty.
    std::map<std::string, std::deque<ChatLine>> chat_by_room;
    // What was last asked for with subscribe_rooms, so a re-ask on
    // reconnect sends the same set.
    std::vector<std::string> sub_stems;
    // Which room each rail stem folded onto, straight from the hub's
    // subscribe answer: two stems can share a room, so this cannot be
    // derived from the accepted list.
    std::map<std::string, std::string> stem_room;
    // Unread lines per room since it was last shown, and whether the rail
    // still has to be told.
    std::map<std::string, int> unread;
    bool unread_dirty = false;
    // Whether this hub can watch rooms at all (hello_ack capabilities,
    // "rooms_watch_v1"). An older hub ignores subscribe_rooms silently,
    // and a client that assumed support would show empty channels for
    // every room it believed it was watching -- so ask, do not guess.
    bool can_watch = false;
    // Ignored users, by hub id. Kept client-side and by ID rather than by
    // nick, because a nick is not a key -- someone can rename, and two
    // people in a room can share a display name.
    std::set<std::string> ignored;
    // Blocked is ignore PLUS auto-declining their challenges. Kept apart
    // from `ignored` so unmuting someone does not quietly start accepting
    // matches from them again.
    std::set<std::string> blocked;
    // game_id (lowercased) -> people in that room, straight from the hub's
    // room list. This is what the browse grid's online badge shows.
    std::map<std::string, int> room_users;
    bool rooms_dirty = false;
    // Ids in the order the last roster was published, so a click index
    // resolves to a user even though the FFI reorders rows into groups.
    std::vector<std::string> roster_ids;
    bool dirty_roster = true;
    bool dirty_chat   = true;
};

Bridge& B() {
    static Bridge b;
    return b;
}

// Same cap the launcher's own backlog uses. The hub keeps no history, so
// this deque IS the scrollback and an unbounded one would grow for as long
// as the launcher stays open.
constexpr size_t kChatMax = 500;

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool IsLocalHost(const std::string& h) {
    return h == "localhost" || h == "127.0.0.1" || h == "::1" ||
           h.rfind("192.168.", 0) == 0 || h.rfind("10.", 0) == 0;
}

std::string Clock(long long unix_seconds) {
    if (unix_seconds <= 0) return "--:--";
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm     lt{};
    if (!localtime_s(&lt, &t)) {
        char buf[8];
        std::strftime(buf, sizeof(buf), "%H:%M", &lt);
        return buf;
    }
    return "--:--";
}

void PublishRoster() {
    Bridge& b = B();
    b.roster_ids.clear();
    std::string tsv;
    for (const auto& [id, u] : b.users) {
        const bool self = (id == b.my_id);
        // Resolve opponent_id to a name, but only for someone actually in
        // a match. The id is useless to a reader, and a name we cannot
        // resolve (they left, or they are in another room) is better shown
        // as nothing than as a hex string.
        std::string opponent;
        if (u.status == "in_match" && !u.opponent_id.empty()) {
            auto it = b.users.find(u.opponent_id);
            if (it != b.users.end()) opponent = it->second.nick;
        }
        tsv += u.nick;                       tsv += '\t';
        tsv += u.status.empty() ? "idle" : u.status; tsv += '\t';
        tsv += std::to_string(u.rtt_ms);     tsv += '\t';
        tsv += u.tier;                       tsv += '\t';
        tsv += self ? "1" : "0";             tsv += '\t';
        // W-L-D against this player needs the hub's record query, which
        // the spike does not issue. Showing "--" is the same thing the
        // shipping launcher shows for an opponent you have never played,
        // so it reads as absent rather than as zero wins.
        tsv += "--";                         tsv += '\t';
        tsv += opponent;                     tsv += '\t';
        tsv += b.ignored.count(id) ? "1" : "0"; tsv += '\t';
        tsv += b.blocked.count(id) ? "1" : "0"; tsv += '\n';
        b.roster_ids.push_back(id);
    }
    slint_ui_set_peers(tsv.c_str());
}

void PublishChat() {
    Bridge& b = B();
    std::string all;
    // The room the shell is showing is the one we JOINED; the other
    // watched rooms sit in their own backlogs until the rail switches.
    static const std::deque<ChatLine> kNone;
    auto it = b.chat_by_room.find(b.room_id);
    const std::deque<ChatLine>& lines = (it == b.chat_by_room.end()) ? kNone : it->second;
    for (const ChatLine& l : lines) {
        // Filtered at render rather than at receive, so un-ignoring
        // brings their history back instead of leaving a hole.
        if (!l.user_id.empty() &&
            (b.ignored.count(l.user_id) || b.blocked.count(l.user_id))) continue;
        all += Clock(l.ts);                          all += '\t';
        all += l.nick;                               all += '\t';
        all += l.text;                               all += '\t';
        all += l.system ? "1" : "0";                 all += '\t';
        all += (!l.user_id.empty() && l.user_id == b.my_id) ? "1" : "0";
        all += '\n';
    }
    slint_ui_set_chat(all.c_str());
}

void OnEvent(const fm2k::HubEvent& ev) {
    Bridge& b = B();
    using K = fm2k::HubEvent::Kind;
    switch (ev.kind) {
        case K::RoomList:
            // The hub sends the full list, so replace rather than merge --
            // a room that emptied and vanished has to disappear from the
            // grid, and merging would leave its last count on screen.
            b.room_users.clear();
            for (const auto& r : ev.rooms) b.room_users[Lower(r.id)] = r.user_count;
            b.rooms_dirty = true;
            break;
        case K::Connected:
            for (const auto& r : ev.rooms) b.room_users[Lower(r.id)] = r.user_count;
            b.rooms_dirty = true;
            b.my_id = ev.user_id;
            b.can_watch = false;
            for (const std::string& c : ev.capabilities) {
                if (c == "rooms_watch_v1") { b.can_watch = true; break; }
            }
            slint_ui_set_connected(1);
            slint_ui_set_status("HUB CONNECTED");
            // The roster is scoped to a room, so a client that connects
            // and never joins one sees an empty list and looks broken.
            // Joining from inside the event is safe: sending takes a
            // different lock from the one Poll drains under.
            if (!b.want_room.empty())
                b.client.JoinRoom(b.want_room, b.want_room_title.empty() ? b.want_room : b.want_room_title);
            // Re-ask for the watch set: subscriptions live with the
            // connection, so a reconnect starts from none.
            if (b.can_watch && !b.sub_stems.empty()) b.client.SetRooms(b.sub_stems);
            break;
        case K::Disconnected:
            // Counts are only true while connected. Leaving them on screen
            // would advertise a populated room to someone who cannot see it.
            b.room_users.clear();
            b.rooms_dirty = true;
            b.users.clear();
            // The scrollback goes with the connection. The hub cannot
            // replay it, so keeping it would leave dead lines sitting
            // above whatever arrives after a reconnect.
            b.chat_by_room.clear();
            b.my_id.clear();
            b.room_id.clear();
            slint_ui_set_connected(0);
            slint_ui_set_status(ev.error.empty() ? "HUB DISCONNECTED"
                                                 : ev.error.c_str());
            b.dirty_roster = b.dirty_chat = true;
            break;
        case K::RoomJoined:
            if (!ev.rooms.empty()) b.room_id = ev.rooms.front().id;
            b.users.clear();
            // The OTHER rooms' backlogs stay: switching rooms in the rail
            // should show a conversation that is already there. Only the
            // roster is per joined room (presence is single-room).
            for (const auto& u : ev.users) b.users[u.id] = u;
            b.dirty_roster = b.dirty_chat = true;
            break;
        case K::RoomLeft:
            b.users.clear();
            b.room_id.clear();
            b.dirty_roster = b.dirty_chat = true;
            break;
        case K::Subscriptions:
            // One entry per requested stem, carrying the room it folded
            // onto. That mapping is what lets an incoming chat line's
            // room_id be matched back to a chip on the rail.
            for (const auto& r : ev.rooms) {
                if (!r.requested.empty()) b.stem_room[r.requested] = r.id;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: watching %zu room(s)", ev.rooms.size());
            break;
        case K::UserJoined:
            if (ev.room_id == b.room_id) {
                b.users[ev.user.id] = ev.user;
                b.dirty_roster = true;
            }
            break;
        case K::UserLeft:
            if (b.users.erase(ev.user_id)) b.dirty_roster = true;
            break;
        case K::UserStatus:
        case K::UserRtt: {
            auto it = b.users.find(ev.user.id.empty() ? ev.user_id : ev.user.id);
            if (it != b.users.end()) {
                // Merge rather than assign: an RTT update carries only the
                // ping, and overwriting the whole row with it would blank
                // the status and tier every time a ping arrived.
                if (!ev.user.status.empty()) it->second.status = ev.user.status;
                if (!ev.user.nick.empty())   it->second.nick = ev.user.nick;
                if (!ev.user.tier.empty())   it->second.tier = ev.user.tier;
                it->second.opponent_id = ev.user.opponent_id;
                if (ev.user.rtt_ms > 0)      it->second.rtt_ms = ev.user.rtt_ms;
                b.dirty_roster = true;
            }
            break;
        }
        case K::ChatReceived: {
            // NO ROOM FILTER HERE. This used to drop any line whose room
            // was not the joined one, which is every room the rail
            // WATCHES -- so the backlogs those rooms are supposed to fill
            // stayed empty and the unread marks never appeared. Routing is
            // the backlog's job now: each line lands in its own room's
            // deque and PublishChat shows the one being looked at.
            ChatLine l;
            l.ts      = static_cast<long long>(ev.chat.ts);
            l.user_id = ev.chat.user_id;
            l.nick    = ev.chat.nick;
            l.text    = ev.chat.text;
            l.system  = ev.chat.system;
            {
                const std::string rid = ev.chat.room_id.empty() ? b.room_id : ev.chat.room_id;
                auto& backlog = b.chat_by_room[rid];
                // Unread only counts for rooms you are not looking at, and
                // never counts your own line.
                if (rid != b.room_id && l.user_id != b.my_id) {
                    ++b.unread[rid];
                    b.unread_dirty = true;
                }
                backlog.push_back(std::move(l));
                // Cap the backlog the line landed in: the hub keeps no
                // history, so each of these IS that room's scrollback.
                while (backlog.size() > kChatMax) backlog.pop_front();
            }
            // A line landing proves we are under the limit again.
            slint_ui_set_chat_notice("");
            b.dirty_chat = true;
            break;
        }
        case K::ChatRateLimited: {
            const int ms = ev.chat_rate_limited.retry_after_ms;
            std::string n = "sending too fast -- that line was dropped";
            if (ms > 0) n += ", retry in " + std::to_string((ms + 999) / 1000) + "s";
            slint_ui_set_chat_notice(n.c_str());
            break;
        }
        case K::ChallengeReceived:
            // Stage 1 is INCOMING. The rtt we have for them is the room's
            // last ping, which is what the modal should show -- the match
            // has not started, so there is no session RTT yet.
            {
                // Blocked means blocked: decline on the wire and never
                // raise the modal. Showing it and auto-dismissing would
                // still interrupt whatever the user was doing.
                if (b.blocked.count(ev.challenge.from_id)) {
                    b.client.DeclineChallenge(ev.challenge.from_id);
                    break;
                }
                // The challenger is in ev.challenge, NOT in the generic
                // ev.user / ev.user_id payload -- reading those rendered
                // the modal with a blank name and no ping, which is a
                // challenge you cannot make a decision about.
                int rtt = 0;
                auto it = b.users.find(ev.challenge.from_id);
                if (it != b.users.end()) rtt = it->second.rtt_ms;
                const std::string& who = ev.challenge.from_nick.empty()
                                       ? ev.challenge.from_id
                                       : ev.challenge.from_nick;
                b.pending_from_id = ev.challenge.from_id;
                b.match_nick = who;
                b.match_rtt  = rtt;
                slint_ui_set_match(1, who.c_str(), rtt, 20, 0, "");
                slint_ui_set_scene(1);
            }
            break;
        case K::ChallengeDeclined:
            b.outgoing_to_id.clear();
            slint_ui_set_match(0, "", 0, 0, 0, "");
            slint_ui_set_status("challenge declined");
            break;
        case K::ChallengeCancelled:
            b.pending_from_id.clear();
            slint_ui_set_match(0, "", 0, 0, 0, "");
            slint_ui_set_status("challenge cancelled");
            break;
        case K::ChallengeFailed:
            b.outgoing_to_id.clear();
            slint_ui_set_match(0, "", 0, 0, 0, "");
            slint_ui_set_status(ev.error.empty() ? "challenge failed"
                                                 : ev.error.c_str());
            break;
        case K::MatchStart: {
            b.pending_from_id.clear();
            b.outgoing_to_id.clear();
            b.match_token = ev.match.token;
            b.result_sent = false;
            // There is no "accepted" event -- acceptance only ever shows up
            // as the match starting. Booting is stage 4, and it names the
            // opponent latched when the challenge was raised.
            slint_ui_set_match(4, b.match_nick.c_str(), b.match_rtt, 0, 0, "");
            slint_ui_set_scene(1);
            // The hub's launchable game id (exe stem) over the room id: a
            // canonical family room matches no local exe. Same rule as the
            // ImGui launcher.
            std::string want = ev.match.game_id.empty() ? b.room_id : ev.match.game_id;
            std::transform(want.begin(), want.end(), want.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            size_t idx = b.lib_stems.size();
            for (size_t i = 0; i < b.lib_stems.size(); ++i) {
                std::string s = b.lib_stems[i];
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (s == want) { idx = i; break; }
            }
            std::string err;
            bool ok = false;
            if (ev.match.peer_udp_port <= 0) err = "peer never sent udp_addr";
            else if (idx >= b.lib_paths.size()) err = "game not in your library";
            else ok = hostgui::LaunchOnline(b.lib_paths[idx], b.lib_engines[idx],
                                            b.lib_stems[idx], ev, b.my_id, &err);
            if (!ok) {
                // Tell the hub now, or both players sit at in_match forever.
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: match_start aborted (%s) -- sending match_ended", err.c_str());
                slint_ui_set_status(("match aborted: " + err).c_str());
                b.client.MatchEnded();
                slint_ui_set_match(0, "", 0, 0, 0, "");
            }
            break;
        }
        case K::MatchRotated:
            // A rematch inside the same game process: the hub minted a
            // fresh token, the next outcome goes out under it. The
            // hook's seq counters are NOT reset (per process).
            if (!ev.match.token.empty()) { b.match_token = ev.match.token; b.result_sent = false; }
            break;
        case K::PeerDisconnected:
            slint_ui_set_status("peer disconnected -- closing match");
            if (!b.match_token.empty() && !b.result_sent) {
                b.client.MatchResult(b.match_token, "disconnect");
                b.result_sent = true;
            }
            hostgui::LaunchTerminate();
            break;
        case K::Error:
            slint_ui_set_status(ev.error.c_str());
            break;
        default:
            break;
    }
}

}  // namespace

bool HubStart(const char* host, const char* nick, const char* room) {
    if (!host || !host[0]) return false;
    Bridge& b = B();
    if (b.started) return true;

    const std::string h = host;
    // The user's Discord sign-in (discord_auth.json, shared with the
    // shipping launcher); FM2K_HUB_TOKEN overrides it for the harness.
    const std::string cached_tok = hostgui::DiscordHubToken();
    const char* env_tok = std::getenv("FM2K_HUB_TOKEN");
    const char* tok = (env_tok && env_tok[0]) ? env_tok : cached_tok.c_str();
    // A local hub.py serves plain ws on 7711 with no TLS in front; public
    // hosts go WSS:443 /ws through Caddy. Same split the launcher makes,
    // and the plaintext route stays bound to local hosts so a hub token
    // cannot go out in the clear to a routable one.
    const bool  local = IsLocalHost(h);
    const int   port  = local ? 7711 : 443;
    const char* path  = local ? "/" : "/ws";

    b.my_nick   = (nick && nick[0]) ? nick : "spike";
    // No default room: the selected game's room is joined once the
    // selection resolves (HubJoinRoom). --room still forces one.
    b.want_room = (room && room[0]) ? room : "";
    b.started = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: spike connecting to %s%s:%d (%s) auth=%s",
                h.c_str(), path, port, local ? "ws" : "wss",
                (tok && tok[0]) ? "present" : "missing");
    b.client.Connect(h, static_cast<uint16_t>(port), path, b.my_nick,
                     tok ? tok : "", !local);
    return true;
}

void HubTick() {
    Bridge& b = B();
    if (!b.started) return;
    b.client.Poll([](const fm2k::HubEvent& ev) { OnEvent(ev); });
    // Republish only on change. The roster and the transcript are whole
    // model replacements, and doing that every frame would rebuild both
    // models at vsync for a list that changes a few times a minute.
    if (b.dirty_roster) { PublishRoster(); b.dirty_roster = false; }
    if (b.dirty_chat)   { PublishChat();   b.dirty_chat = false; }
}

void HubChallenge(int index) {
    Bridge& b = B();
    if (!b.started) return;
    if (index < 0 || index >= static_cast<int>(b.roster_ids.size())) return;
    const std::string& id = b.roster_ids[static_cast<size_t>(index)];
    if (id == b.my_id) return;
    // No MatchSettings here: building them means loading the selected
    // game's resolved [GamePlay] config and applying the anti-cheat
    // clamps, which lives in the launcher. Sending a settings-less
    // challenge is the documented older-client shape, and the target's
    // modal renders without a preview rather than with wrong values.
    b.client.Challenge(id);
    b.outgoing_to_id = id;
    // Stage 2 is OUTGOING.
    auto it = b.users.find(id);
    b.match_nick = it != b.users.end() ? it->second.nick : std::string();
    b.match_rtt  = it != b.users.end() ? it->second.rtt_ms : 0;
    slint_ui_set_match(2, b.match_nick.c_str(), b.match_rtt, 20, 0, "");
    slint_ui_set_scene(1);
}

void HubMatchAction(int action) {
    Bridge& b = B();
    if (!b.started) return;
    // Closing the modal is the UI's business and already happened. What
    // is left is the half the other side is waiting on: a challenge that
    // is only dismissed locally leaves the challenger sitting on
    // "waiting for a response" until the hub times it out.
    switch (action) {
        case 1:
            if (!b.pending_from_id.empty()) b.client.AcceptChallenge(b.pending_from_id);
            b.pending_from_id.clear();
            break;
        case 2:
            if (!b.pending_from_id.empty()) b.client.DeclineChallenge(b.pending_from_id);
            b.pending_from_id.clear();
            break;
        case 3:
            if (!b.outgoing_to_id.empty()) b.client.CancelChallenge(b.outgoing_to_id);
            b.outgoing_to_id.clear();
            break;
        default:
            break;
    }
}

void HubSetLibrary(const std::vector<std::string>& stems,
                   const std::vector<std::string>& paths,
                   const std::vector<std::string>& engines) {
    Bridge& b = B();
    b.lib_stems = stems; b.lib_paths = paths; b.lib_engines = engines;
}

void HubSetRooms(const std::vector<std::string>& stems) {
    Bridge& b = B();
    if (stems == b.sub_stems) return;   // the list has not changed
    b.sub_stems = stems;
    if (!b.started || b.my_id.empty()) return;   // re-sent on connect
    if (!b.can_watch) return;                    // hub predates room lists
    b.client.SetRooms(b.sub_stems);
}

void HubLeaveRoom(const std::string& stem) {
    Bridge& b = B();
    const auto it = b.stem_room.find(stem);
    const std::string room = (it == b.stem_room.end()) ? stem : it->second;
    b.unread.erase(room);
    b.chat_by_room.erase(room);
    b.stem_room.erase(stem);
    b.sub_stems.erase(std::remove(b.sub_stems.begin(), b.sub_stems.end(), stem),
                      b.sub_stems.end());
    if (!b.started || b.my_id.empty()) return;
    b.client.LeaveRoom(room);
}

void HubJoinRoom(const char* game_id, const char* title) {
    if (!game_id || !game_id[0]) return;
    Bridge& b = B();
    b.want_room = game_id;
    b.want_room_title = (title && title[0]) ? title : game_id;
    // Showing a room clears its unread mark, whether or not the join
    // round-trips (the rail should not keep nagging about the room you
    // are looking at).
    {
        auto it = b.stem_room.find(b.want_room);
        if (b.unread.erase(it == b.stem_room.end() ? b.want_room : it->second)) {
            b.unread_dirty = true;
        }
        b.dirty_chat = true;
    }
    if (!b.started || b.my_id.empty() || b.room_id == b.want_room) return;
    b.client.JoinRoom(b.want_room, b.want_room_title);
}

void HubPollOutcome(uint32_t pid) {
    Bridge& b = B();
    if (!b.started || pid == 0 || b.match_token.empty()) return;
    fm2k::match_outcome::Snapshot s;
    if (!fm2k::match_outcome::Read(pid, &s)) return;

    // A battle started: tell the lobby who is fighting on what.
    if (s.chars_seq != 0 && s.chars_seq != b.last_chars_seq[pid]) {
        b.last_chars_seq[pid] = s.chars_seq;
        b.client.ReportMatchProgress(b.match_token, s.p1_char_id, s.p2_char_id,
                                     s.p1_char_name, s.p2_char_name, s.stage_id, std::string());
    }

    if (s.outcome_seq <= b.last_outcome_seq[pid] || s.outcome == 0) return;
    b.last_outcome_seq[pid] = s.outcome_seq;

    if (fm2k::match_outcome::IsAbort(s.outcome)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Hub: %s", fm2k::match_outcome::Reason(s.outcome));
        slint_ui_set_status(fm2k::match_outcome::Reason(s.outcome));
        hostgui::LaunchTerminate();
        b.match_token.clear();
        return;
    }
    const char* word = fm2k::match_outcome::Word(s.outcome);
    if (!word) return;
    // NOT PORTED: the launcher's results.csv mirror and its KGT-resolved
    // character/stage names; the hook's names go out as they are.
    b.client.MatchResult(b.match_token, word, s.p1_char_id, s.p2_char_id,
                         s.p1_char_name, s.p2_char_name, s.stage_id, std::string(),
                         s.session_id, s.match_index, s.rounds);
    b.result_sent = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hub: match_result sent token=%.8s... outcome=%s pid=%u seq=%u",
                b.match_token.c_str(), word, (unsigned)pid, (unsigned)s.outcome_seq);
    slint_ui_set_status(std::string(word) == "self_won" ? "you won" :
                        std::string(word) == "peer_won" ? "you lost" :
                        std::string(word) == "draw"     ? "draw" : "peer disconnected -- closing match");
    b.client.QueryRecord();
    b.client.RequestRecentMatches(50);
    if (std::string(word) == "disconnect") hostgui::LaunchTerminate();
}

void HubMatchEnded() {
    Bridge& b = B();
    if (!b.started) return;
    b.client.MatchEnded();
    slint_ui_set_match(0, "", 0, 0, 0, "");
}

bool HubTakeUnreadDirty() {
    Bridge& b = B();
    const bool was = b.unread_dirty;
    b.unread_dirty = false;
    return was;
}

int HubUnreadFor(const std::string& stem) {
    Bridge& b = B();
    auto it = b.stem_room.find(stem);
    const std::string& room = (it == b.stem_room.end()) ? stem : it->second;
    auto u = b.unread.find(room);
    return u == b.unread.end() ? 0 : u->second;
}

const char* HubMatchNick() { return B().match_nick.c_str(); }
int         HubMatchRtt()  { return B().match_rtt; }

void HubSpectate(int index) {
    Bridge& b = B();
    if (!b.started) return;
    if (index < 0 || index >= static_cast<int>(b.roster_ids.size())) return;
    const std::string& id = b.roster_ids[static_cast<size_t>(index)];
    if (id == b.my_id) return;
    // The hub decides: it answers SpectateGranted with the host's
    // address, or SpectateDenied when the target is not actually in a
    // match. Asking optimistically is the shipped launcher's behaviour.
    b.client.RequestSpectate(id);
    slint_ui_set_status("spectate requested");
}

void HubIgnore(int index) {
    Bridge& b = B();
    if (!b.started) return;
    if (index < 0 || index >= static_cast<int>(b.roster_ids.size())) return;
    const std::string& id = b.roster_ids[static_cast<size_t>(index)];
    if (id == b.my_id) return;
    // Toggle, so the same menu entry undoes itself -- an ignore you
    // cannot lift is a support request.
    if (b.ignored.count(id)) {
        b.ignored.erase(id);
        slint_ui_set_status("unignored");
    } else {
        b.ignored.insert(id);
        slint_ui_set_status("ignored");
    }
    b.dirty_chat = true;
}

void HubBlock(int index) {
    Bridge& b = B();
    if (!b.started) return;
    if (index < 0 || index >= static_cast<int>(b.roster_ids.size())) return;
    const std::string& id = b.roster_ids[static_cast<size_t>(index)];
    if (id == b.my_id) return;
    if (b.blocked.count(id)) {
        b.blocked.erase(id);
        slint_ui_set_status("unblocked");
    } else {
        b.blocked.insert(id);
        slint_ui_set_status("blocked");
    }
    b.dirty_roster = true;
    b.dirty_chat = true;
}

void HubSendChat(const char* text) {
    Bridge& b = B();
    if (!b.started || !text || !text[0] || b.room_id.empty()) return;
    // Not appended locally: the hub broadcasts every accepted line back to
    // the whole room, sender included, so the line arrives as a normal
    // ChatReceived. Echoing here would print everything twice.
    b.client.SendChat(b.room_id, text);
}

int HubRoomUsers(const char* game_id) {
    Bridge& b = B();
    if (!b.started || !game_id || !game_id[0]) return 0;
    auto it = b.room_users.find(Lower(game_id));
    return it == b.room_users.end() ? 0 : it->second;
}

bool HubTakeRoomsDirty() {
    Bridge& b = B();
    const bool d = b.rooms_dirty;
    b.rooms_dirty = false;
    return d;
}

bool HubActive() { return B().started; }

void HubStop() {
    Bridge& b = B();
    if (!b.started) return;
    b.started = false;
    b.client.Disconnect();
    // Everything below was only true while connected. Leaving it up would
    // show a populated room and a live roster to someone who is offline.
    b.users.clear();
    b.chat_by_room.clear();
    b.room_users.clear();
    b.roster_ids.clear();
    b.my_id.clear();
    b.room_id.clear();
    slint_ui_set_connected(0);
    slint_ui_set_peers("");
    slint_ui_set_chat("");
}

bool HubRestart(const char* host, const char* nick, const char* room) {
    HubStop();
    return HubStart(host, nick, room);
}

}  // namespace hostgui
