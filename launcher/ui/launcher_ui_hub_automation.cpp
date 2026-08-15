// launcher_ui_hub_automation.cpp -- the default-off FM2K_HUB_AUTOMATION env
// surface (harness only) plus the two hub ACTION helpers it shares with the
// Hub panel's buttons.
//
// Why this TU exists. The hub-brokered flow (connect -> join room ->
// challenge -> accept -> spectate) had no CLI or env entry point at all: every
// step was an ImGui click inside RenderHubPanel. That made a hub-brokered
// spectate E2E harness impossible, so the only automated spectator coverage we
// ever had was the direct-IP `--spectate <ip:port>` path -- which bypasses the
// hub entirely and therefore never tested the grant path users actually take.
//
// Two rules shaped the implementation:
//   1. NEVER duplicate protocol code. The driver calls the same
//      HubClient methods the buttons call, and the two multi-line action
//      bodies (connect, challenge-settings) were MOVED here verbatim so both
//      callers share one copy instead of drifting.
//   2. Run OUTSIDE the paint path. TickHubAutomation() is driven from
//      LauncherUI::TickAlways(), not from RenderHubPanel: `ImGui::Begin("Hub")`
//      returns false whenever the Hub dock tab is not the selected one (or the
//      launcher is minimized), so a render-driven driver would stall the moment
//      the harness's window arrangement changed. Nothing here touches ImGui.
//
// Everything is inert unless FM2K_HUB_AUTOMATION=1: with the master switch off
// the tick returns after one getenv and the two action helpers behave exactly
// as the inline code they replaced.
#include "FM2K_Integration.h"
#include "launcher_ui_hub_internal.h"
#include "launcher_ui_hubstate.h"  // LauncherUI::HubState full def
#include "launcher_ui_internal.h"  // shared persistence helpers (namespace lui)
#include "FM2K_HubClient.h"
#include "FM2K_DiscordAuth.h"
#include "FM2K_GameIni.h"
#include "FM2K_Utf8Path.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cstdint>

using namespace lui;  // shared persistence helpers (launcher_ui_internal.h)

// ---------------------------------------------------------------------------
// Hub endpoint predicates (Phase 4e -- review A8a / A8c). Declared in
// FM2K_LauncherUI_decl.h; both hub-URL call sites use these and nothing else.
// ---------------------------------------------------------------------------
namespace fm2k { namespace hub_endpoint {
namespace {

// Parse a bare dotted-quad. Returns false for anything that is not exactly
// four decimal octets -- no DNS, no partial matches, no hex/octal forms.
bool ParseIPv4(const std::string& h, unsigned o[4]) {
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= h.size() || h[pos] < '0' || h[pos] > '9') return false;
        unsigned v = 0, digits = 0;
        while (pos < h.size() && h[pos] >= '0' && h[pos] <= '9') {
            v = v * 10u + (unsigned)(h[pos] - '0');
            if (++digits > 3) return false;
            ++pos;
        }
        if (v > 255u) return false;
        o[i] = v;
        if (i < 3) { if (pos >= h.size() || h[pos] != '.') return false; ++pos; }
    }
    return pos == h.size();
}

std::string LowerCopy(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
    return o;
}

}  // namespace

bool IsLoopbackHost(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool IsPrivateHost(const std::string& host) {
    if (IsLoopbackHost(host)) return true;
    const std::string h = LowerCopy(host);
    unsigned o[4] = {0, 0, 0, 0};
    if (ParseIPv4(h, o)) {
        if (o[0] == 127u) return true;                                 // loopback/8
        if (o[0] == 10u) return true;                                  // RFC1918 /8
        if (o[0] == 172u && o[1] >= 16u && o[1] <= 31u) return true;   // RFC1918 /12
        if (o[0] == 192u && o[1] == 168u) return true;                 // RFC1918 /16
        if (o[0] == 169u && o[1] == 254u) return true;                 // link-local
        return false;
    }
    // IPv6 literals, with or without brackets. fc00::/7 (unique-local) and
    // fe80::/10 (link-local) are the private ranges; everything else routes.
    std::string v6 = h;
    if (v6.size() >= 2 && v6.front() == '[' && v6.back() == ']')
        v6 = v6.substr(1, v6.size() - 2);
    if (v6 == "::1") return true;
    if (v6.rfind("fc", 0) == 0 || v6.rfind("fd", 0) == 0) return true;
    if (v6.rfind("fe8", 0) == 0 || v6.rfind("fe9", 0) == 0 ||
        v6.rfind("fea", 0) == 0 || v6.rfind("feb", 0) == 0) return true;
    return false;
}

bool IsRetiredHubHost(const std::string& host) {
    return LowerCopy(host) == "2dfm.sytes.net";
}

}}  // namespace fm2k::hub_endpoint

// ---------------------------------------------------------------------------
// Shared action helpers (moved bodies -- behavior identical to the inline code)
// ---------------------------------------------------------------------------

void LauncherUI::HubConnect(const std::string& outgoing_nick) {
    auto& hs = *hub_state_;
    hs.my_nick = outgoing_nick;
    // hub_host_ is normally seeded on the first RenderHubPanel pass. The
    // automation driver can reach Connect before any panel has painted (Hub is
    // not the selected dock tab, launcher minimized), and an unseeded
    // hub_host_ would silently fall back to the PRODUCTION host -- a local
    // harness quietly talking to prod is exactly the failure mode this
    // campaign is not allowed to have. Same init, idempotent, same latch.
    if (!hub_host_initialized_) {
        hub_host_initialized_ = true;
        const char* env_h = std::getenv("FM2K_HUB_HOST");
        const char* def   = (env_h && env_h[0]) ? env_h : "hub.2dfm.org";
        std::snprintf(hub_host_, sizeof(hub_host_), "%s", def);
    }
    // Auto-pick a free UDP port: bind a socket to port 0
    // (OS-assigned ephemeral), read back the chosen port via
    // getsockname, close. Same-machine multi-launcher tests
    // get distinct ports automatically; users never need to
    // think about it. Cross-machine: any free port works.
    //
    // WSAStartup is required before socket() on Windows. It's
    // idempotent -- internal refcount, fine to call repeatedly.
    // Without it socket() fails with WSANOTINITIALISED and the
    // fallback picks 7000, which then collides between two
    // launchers on the same box.
    int picked = 7000;
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: auto-pick socket() failed (err=%d) -- falling back to 7000",
            WSAGetLastError());
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            sockaddr_in bound{};
            int len = sizeof(bound);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
                picked = ntohs(bound.sin_port);
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: auto-pick bind() failed (err=%d)", WSAGetLastError());
        }
        closesocket(s);
    }
    network_config_.local_port = picked;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: auto-picked UDP port %d for this session", picked);
    // Hub address from the Host field above. The same string
    // gets persisted into the FM2K_HUB_HOST env so the spawned
    // game's nat_traversal STUN probe / relay endpoint uses
    // the same host.
    std::string hub_host = (hub_host_[0] != '\0') ? hub_host_ : "hub.2dfm.org";
    // DEFAULT RESTORATION for the ONE retired name. 2dfm.sytes.net is dead
    // (DNS removed pre-2026-08; the project moved to hub.2dfm.org behind
    // Cloudflare/Caddy), so a host field still naming it can only fail. It is
    // restored to the default -- and ONLY it.
    //
    // Phase 4e (review A8a): this used to be an UNANCHORED find("sytes.net").
    // sytes.net is a large public free-dyndns domain and the Settings help text
    // invites running your own hub, so "myhub.sytes.net" -- a deliberate, live
    // user choice typed this session -- was silently redirected to PRODUCTION,
    // taking the process-wide FM2K_HUB_HOST export (and therefore the spawned
    // game's STUN/relay endpoints) and the user's Discord hub_token with it.
    // The branch's stated premise was also impossible: hub_host_ is never
    // persisted anywhere, so there is no "stale saved config" to clean up.
    // Anything else containing sytes.net is now honoured as typed, with a warn
    // so a user who meant the retired host still gets told.
    if (fm2k::hub_endpoint::IsRetiredHubHost(hub_host)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: hub host '%s' is the RETIRED No-IP name (DNS removed) -- "
            "restoring the default hub.2dfm.org. Clear the Hub Server host "
            "field to stop seeing this.", hub_host.c_str());
        hub_host = "hub.2dfm.org";
    } else if (hub_host.find("sytes.net") != std::string::npos) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: hub host '%s' is not the retired 2dfm.sytes.net -- connecting "
            "to it AS TYPED. If you meant the old project hub, it is gone; the "
            "current one is hub.2dfm.org.", hub_host.c_str());
    }
    ::SetEnvironmentVariableA("FM2K_HUB_HOST", hub_host.c_str());
    // TCP-STUN endpoint -- same hub host, port 7713 (UDP-STUN at
    // 7711, UDP-relay at 7712). Set process-wide here so every
    // spawned game (player AND spectator) inherits and can run
    // its outbound TCP-STUN probe at hook init. Without this,
    // the spec hook logs "FM2K_HUB_TCP_STUN_ADDR unset -- skipping"
    // and falls back to local listener port for cross-NAT punch
    // -- which fails on non-port-preserving NATs.
    ::SetEnvironmentVariableA("FM2K_HUB_TCP_STUN_ADDR",
                              (hub_host + ":7713").c_str());
    // FM2K_HUB_UDP_ADDR -- set at connect time (hub_host known here).
    // FM2K_HUB_USER_ID is set on Connected (hello_ack) where my_id
    // first lands; both are required by the hook's STUN probe
    // (nat_traversal.cpp::SendStunProbe). Used to be set only
    // inside the match_start handler -- meaning a spec instance
    // launched before joining any match wouldn't STUN, so hub's
    // user.udp_addr stayed at whatever earlier game STUN landed
    // (or empty), and spectator_incoming forwarded the wrong UDP
    // port to the host. The punch went nowhere.
    ::SetEnvironmentVariableA("FM2K_HUB_UDP_ADDR",
                              (hub_host + ":7711").c_str());
    // Pull the cached Discord hub_token. Hub will reject the
    // hello with `auth_required` if missing/expired and the
    // launcher will surface the error in status_line.
    const auto cached = fm2k::discord_auth::LoadCached();
    // Endpoint selection. Public hosts go WSS:443 /ws through Caddy -- that is
    // now the ONLY public shape (the sytes.net direct-WS-on-7711 branch that
    // used to live here is gone: that host is retired, so keeping it as
    // "legacy compat" only meant dialing a dead name; stale saved hosts are
    // rewritten to the default above instead).
    //
    // The direct plain-ws://<host>:7711/ path survives for exactly one reason:
    // a LOCAL hub.py (dev box, the hub-brokered spectate harness) serves plain
    // ws on 7711 with no TLS front. `loopback_hub` covers the same three
    // spellings the UPnP skip already special-cases (launcher_ui_hub_events.cpp,
    // K::Connected); FM2K_HUB_INSECURE=1 extends it to a local hub reachable by
    // a private IP instead.
    //
    // PHASE 4e (review A8c). FM2K_HUB_INSECURE is BOUND TO PRIVATE HOSTS. As
    // first written it was not conditioned on the host at all, so setting it
    // downgraded ANY host -- hub.2dfm.org included -- to plain ws://host:7711/,
    // and the hello carrying the cached Discord hub_token went out in the clear.
    // A plaintext downgrade of a routable host is refused, loudly, and the
    // normal WSS path is taken instead. EnvFlag-style strict parse: only "1"
    // arms it, so "false"/"off"/garbage can no longer turn it on either.
    const char* insecure_env = std::getenv("FM2K_HUB_INSECURE");
    const bool  insecure_req = (insecure_env && std::strcmp(insecure_env, "1") == 0);
    const bool  loopback_hub = fm2k::hub_endpoint::IsLoopbackHost(hub_host);
    const bool  private_hub  = fm2k::hub_endpoint::IsPrivateHost(hub_host);
    const bool  hub_insecure = insecure_req && private_hub;
    if (insecure_req && !private_hub) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: REFUSING FM2K_HUB_INSECURE for '%s' -- it is not a loopback "
            "or private (RFC1918 / link-local) address. A plaintext hub "
            "connection carries your Discord hub_token in the clear, so the "
            "insecure route is available for LOCAL hubs only. Connecting over "
            "WSS as normal.", hub_host.c_str());
    }
    const bool use_direct_ws = loopback_hub || hub_insecure;
    const uint16_t      ws_port = use_direct_ws ? 7711  : 443;
    const char*         ws_path = use_direct_ws ? "/"   : "/ws";
    const bool          ws_tls  = !use_direct_ws;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: connecting to %s%s:%u (%sWS) auth=%s",
                hub_host.c_str(), ws_path, (unsigned)ws_port,
                ws_tls ? "WSS via " : "",
                cached.valid ? "present" : "missing");
    hs.client.SetStealth(hs.stealth);  // ensure the hello reflects the current toggle
    hs.client.Connect(hub_host, ws_port, ws_path, hs.my_nick,
                      cached.hub_token, ws_tls);
    hs.status_line = "connecting to " + hub_host + " ...";
}

void LauncherUI::BuildChallengeSettings(fm2k::MatchSettings& ms) {
    if (selected_game_index_ >= 0 &&
        selected_game_index_ < (int)games_.size())
    {
        const auto& g = games_[selected_game_index_];
        fm2k::game_ini::GamePlayConfig cfg;
        fm2k::game_ini::LoadResolved(g.exe_path, cfg);
        fm2k::game_ini::ForceOnlineClamps(cfg);
        ms.player0_cpu      = cfg.player0_cpu;
        ms.player1_cpu      = cfg.player1_cpu;
        ms.game_speed       = cfg.game_speed;
        ms.hit_judge        = cfg.hit_judge;
        ms.game_information = cfg.game_information;
        ms.stage_nb         = cfg.stage_nb;
        ms.joystick         = cfg.joystick;
        ms.time             = cfg.time;
        ms.exit_flag        = cfg.exit_flag;
        ms.vs_mode          = cfg.vs_mode;
        ms.vs_single_play   = cfg.vs_single_play;
        ms.vs_team_play     = cfg.vs_team_play;
    }
    // Random-stage extension (#56). When enabled,
    // generate a fresh xorshift seed per challenge
    // and ship it to the peer. Both peers re-seed
    // their hook PRNG from this same value, then
    // run identical sequences on rematches with
    // zero per-rematch wire traffic. Seed != 0
    // is the wire signal "random is on" -- keep
    // a tiny rejection loop so we never accidentally
    // hand a 0 seed.
    EnsureRandomStageLoaded();  // per-game
    if (random_stage_enable_ &&
        random_stage_max_ >= random_stage_min_)
    {
        uint32_t seed = 0;
        while (seed == 0) {
            // 32-bit mix of two rand() bursts; not
            // cryptographic but plenty random for
            // a uniform stage roll.
            seed = (uint32_t)((std::rand() & 0xFFFF) |
                              ((std::rand() & 0xFFFF) << 16));
        }
        ms.random_seed      = seed;
        ms.random_stage_min = random_stage_min_;
        ms.random_stage_max = random_stage_max_;
        // Override any explicit stage_nb so the
        // client doesn't apply both. Random takes
        // precedence when on.
        ms.stage_nb = -1;
    }
}

// ---------------------------------------------------------------------------
// FM2K_HUB_AUTOMATION -- default-off harness driver
// ---------------------------------------------------------------------------

namespace {

// Every knob. All inert unless FM2K_HUB_AUTOMATION=1.
//
//   FM2K_HUB_AUTOMATION=1        master switch (nothing below is read without it)
//   FM2K_HUB_NICK=<name>         lobby nick for this instance (required for
//                                connect: three same-box launchers need three
//                                distinct nicks to address each other)
//   FM2K_HUB_AUTO_CONNECT=1      connect on startup (skips the Connect button
//                                and its sign-in gate)
//   FM2K_HUB_AUTO_ROOM=<game_id> join that room once connected
//   FM2K_HUB_AUTO_CHALLENGE=<nick>  challenge that nick once it is idle in room
//   FM2K_HUB_AUTO_ACCEPT=1       accept any incoming challenge
//   FM2K_HUB_AUTO_SPECTATE=<nick|*>  spectate_request that nick (or the first
//                                in_match user) once it is in a match
struct AutoCfg {
    bool        enabled        = false;
    bool        auto_connect   = false;
    bool        auto_accept    = false;
    std::string nick;
    std::string room;
    std::string challenge_nick;
    std::string spectate_nick;
};

std::string EnvStr(const char* key) {
    const char* v = std::getenv(key);
    return (v && v[0]) ? std::string(v) : std::string();
}

// Strict: ONLY the literal "1" is on. Everything else -- unset, empty, "0",
// "false", "off", "no", "disabled", "00", " ", garbage -- is off.
//
// This USED to be `!v.empty() && v != "0"`, whose comment already claimed to be
// strict while the code failed UNSAFE in the one direction that matters: the
// same helper gates FM2K_HUB_AUTO_ACCEPT, so `FM2K_HUB_AUTOMATION=false` (a
// tester following a stale note) turned the driver fully ON and auto-accepted
// challenges from strangers on the production hub. Fail-safe now, and the
// warning below ("is not 1") finally describes the predicate it guards.
bool EnvFlag(const char* key) {
    return EnvStr(key) == "1";
}

// Parsed once. The contract line is emitted here so a harness reading the
// launcher log can prove WHICH automation this process was actually running
// (a silently-ignored knob is how a stage passes without testing anything).
const AutoCfg& Cfg() {
    static AutoCfg cfg = [] {
        AutoCfg c;
        c.enabled = EnvFlag("FM2K_HUB_AUTOMATION");
        static const char* kKnobs[] = {
            "FM2K_HUB_NICK", "FM2K_HUB_AUTO_CONNECT", "FM2K_HUB_AUTO_ROOM",
            "FM2K_HUB_AUTO_CHALLENGE", "FM2K_HUB_AUTO_ACCEPT",
            "FM2K_HUB_AUTO_SPECTATE",
        };
        if (!c.enabled) {
            // Strict parse: a knob set without the master switch is a harness
            // misconfiguration, and silently doing nothing about it is how a
            // green run means nothing. Say so exactly once.
            for (const char* k : kKnobs) {
                if (!EnvStr(k).empty()) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[hub-auto] %s is set but FM2K_HUB_AUTOMATION is not 1 "
                        "-- ALL hub automation is OFF this run", k);
                    break;
                }
            }
            return c;
        }
        c.auto_connect   = EnvFlag("FM2K_HUB_AUTO_CONNECT");
        c.auto_accept    = EnvFlag("FM2K_HUB_AUTO_ACCEPT");
        c.nick           = EnvStr("FM2K_HUB_NICK");
        c.room           = EnvStr("FM2K_HUB_AUTO_ROOM");
        c.challenge_nick = EnvStr("FM2K_HUB_AUTO_CHALLENGE");
        c.spectate_nick  = EnvStr("FM2K_HUB_AUTO_SPECTATE");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[hub-auto] ENABLED nick='%s' connect=%d room='%s' challenge='%s' "
            "accept=%d spectate='%s'",
            c.nick.c_str(), c.auto_connect ? 1 : 0, c.room.c_str(),
            c.challenge_nick.c_str(), c.auto_accept ? 1 : 0,
            c.spectate_nick.c_str());
        return c;
    }();
    return cfg;
}

// Per-process driver state. One LauncherUI per process, so file-static is the
// right lifetime and keeps the automation entirely out of the shipping headers.
struct AutoState {
    int      connect_attempts     = 0;
    uint32_t connect_at_ms        = 0;
    int      room_attempts        = 0;
    uint32_t room_at_ms           = 0;
    int      challenge_attempts   = 0;
    uint32_t challenge_at_ms      = 0;
    bool     matched              = false;
    bool     spectate_sent        = false;
    uint32_t in_match_seen_ms     = 0;
};
AutoState g_auto;

// Retry/backoff constants. Deliberately generous: the harness budget is
// minutes, and a driver that gives up early turns a slow hub into a confusing
// RED instead of an honest one.
constexpr int      kMaxConnectAttempts   = 12;
constexpr uint32_t kConnectRetryMs       = 5000;
constexpr int      kMaxRoomAttempts      = 20;
constexpr uint32_t kRoomRetryMs          = 3000;
constexpr int      kMaxChallengeAttempts = 12;
constexpr uint32_t kChallengeRetryMs     = 6000;
// The hub grants a spectate with whatever session_kind it last heard from the
// host. The host's kind is published from the game hook via shared mem, so it
// lands a beat after the hub flips the user to in_match. Requesting inside that
// window gets a grant that says "menu" while the host is really in battle,
// which sends the spectator down the title->CSS walk instead of boot-to-battle
// -- a legitimate path, but not the one the harness means to test. Wait for the
// in_match observation to be this old before asking.
constexpr uint32_t kSpectateSettleMs     = 2500;

}  // namespace

void LauncherUI::TickHubAutomation() {
    const AutoCfg& cfg = Cfg();
    if (!cfg.enabled || !hub_state_) return;
    auto& hs = *hub_state_;
    const uint32_t now = static_cast<uint32_t>(SDL_GetTicks());

    // ---- 1. connect ------------------------------------------------------
    if (!hs.client.IsConnected()) {
        if (!cfg.auto_connect) return;
        if (g_auto.connect_attempts >= kMaxConnectAttempts) return;
        if (g_auto.connect_attempts > 0 &&
            (now - g_auto.connect_at_ms) < kConnectRetryMs) return;
        g_auto.connect_attempts++;
        g_auto.connect_at_ms = now;
        const std::string nick = cfg.nick.empty() ? std::string("AUTO")
                                                  : cfg.nick;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[hub-auto] connect attempt %d/%d as '%s'",
            g_auto.connect_attempts, kMaxConnectAttempts, nick.c_str());
        HubConnect(nick);
        return;
    }
    // Connected: re-arm the connect budget so a mid-run hub restart is
    // recoverable rather than terminal.
    g_auto.connect_attempts = 0;
    if (hs.my_id.empty()) return;   // hello_ack not back yet

    // ---- 2. join room ----------------------------------------------------
    if (!cfg.room.empty() && hs.current_room_id != cfg.room) {
        if (g_auto.room_attempts >= kMaxRoomAttempts) return;
        if (g_auto.room_attempts > 0 &&
            (now - g_auto.room_at_ms) < kRoomRetryMs) return;
        g_auto.room_attempts++;
        g_auto.room_at_ms = now;
        if (g_auto.room_attempts <= 3) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[hub-auto] join_room '%s' attempt %d",
                cfg.room.c_str(), g_auto.room_attempts);
        }
        hs.client.JoinRoom(cfg.room, cfg.room);
        return;
    }

    // ---- 3. accept an incoming challenge ---------------------------------
    // Drives the SAME AcceptChallenge the modal's Accept button calls, and
    // clears the same three fields the click handler clears (the modal may
    // never have painted, so nothing else will).
    if (cfg.auto_accept && !hs.pending_challenge_from_id.empty()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[hub-auto] accept_challenge from '%s'",
            hs.pending_challenge_from_nick.c_str());
        hs.client.AcceptChallenge(hs.pending_challenge_from_id);
        hs.pending_challenge_from_id.clear();
        hs.pending_challenge_from_nick.clear();
        hs.show_challenge_modal = false;
    }

    // ---- 4. challenge a named peer ---------------------------------------
    // ONE brokered match per automation session. current_match_token clears at
    // every match end (and on the peer-left path), so without this latch the
    // driver re-challenges the moment the first match finishes -- the peer
    // auto-accepts, a fresh match_start fires, and the launcher spawns ANOTHER
    // game. Observed exactly that: 11 game spawns in one run, each killed by
    // the previous match's auto-terminate, and the harness gated the 11th.
    if (!hs.current_match_token.empty()) g_auto.matched = true;
    if (!cfg.challenge_nick.empty() && !g_auto.matched &&
        hs.current_match_token.empty()) {
        // An outbound challenge that never resolves (target vanished mid-
        // handshake) would otherwise pin outgoing_challenge_to_id forever and
        // wedge the driver silently. Age it out onto the normal retry path.
        if (!hs.outgoing_challenge_to_id.empty() &&
            (now - g_auto.challenge_at_ms) > (kChallengeRetryMs * 2)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[hub-auto] outbound challenge to '%s' unanswered -- re-arming",
                hs.outgoing_challenge_to_nick.c_str());
            hs.outgoing_challenge_to_id.clear();
            hs.outgoing_challenge_to_nick.clear();
            hs.show_outgoing_challenge_modal = false;
        }
        const bool can_try =
            hs.outgoing_challenge_to_id.empty() &&
            g_auto.challenge_attempts < kMaxChallengeAttempts &&
            (g_auto.challenge_attempts == 0 ||
             (now - g_auto.challenge_at_ms) >= kChallengeRetryMs);
        if (can_try) {
            for (const auto& [uid, u] : hs.users) {
                if (uid == hs.my_id)            continue;
                if (u.nick != cfg.challenge_nick) continue;
                if (u.status != "idle")         continue;
                g_auto.challenge_attempts++;
                g_auto.challenge_at_ms = now;
                fm2k::MatchSettings ms;
                BuildChallengeSettings(ms);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[hub-auto] challenge '%s' (%s) attempt %d",
                    u.nick.c_str(), uid.c_str(), g_auto.challenge_attempts);
                hs.client.Challenge(uid, ms);
                hs.outgoing_challenge_to_id   = uid;
                hs.outgoing_challenge_to_nick = u.nick;
                hs.show_outgoing_challenge_modal = true;
                hs.status_line = "challenged " + u.nick + " -- waiting for response";
                break;
            }
        }
    }

    // ---- 5. spectate a named peer ----------------------------------------
    // Fires RequestSpectate, nothing else. The spectator PROCESS is spawned by
    // the K::SpectateGranted handler -> on_spectate_match -> LaunchRemoteSpectator,
    // i.e. only if the hub actually granted. That is what makes "the spectator
    // joined VIA THE HUB" provable by construction: no --spectate argument
    // exists anywhere on this path.
    if (!cfg.spectate_nick.empty() && !g_auto.spectate_sent) {
        const fm2k::HubUser* target = nullptr;
        for (const auto& [uid, u] : hs.users) {
            if (uid == hs.my_id)          continue;
            if (u.status != "in_match")   continue;
            if (u.opponent_id.empty())    continue;
            if (cfg.spectate_nick != "*" && u.nick != cfg.spectate_nick) continue;
            target = &u;
            break;
        }
        if (!target) {
            g_auto.in_match_seen_ms = 0;
        } else if (g_auto.in_match_seen_ms == 0) {
            g_auto.in_match_seen_ms = now;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[hub-auto] '%s' is in_match -- settling %ums before "
                "spectate_request", target->nick.c_str(), kSpectateSettleMs);
        } else if ((now - g_auto.in_match_seen_ms) >= kSpectateSettleMs) {
            g_auto.spectate_sent = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[hub-auto] spectate_request -> '%s' (%s)",
                target->nick.c_str(), target->id.c_str());
            hs.client.RequestSpectate(target->id);
            hs.status_line = "spectate request sent: " + target->nick;
        }
    }
}
