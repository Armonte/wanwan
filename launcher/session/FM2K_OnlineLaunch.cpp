// FM2K_OnlineLaunch.cpp -- see FM2K_OnlineLaunch.h. The bodies here were
// lifted verbatim from launcher_ui_hub.cpp (punch, LAN addresses),
// launcher_ui_hub_events_match.cpp (peer plan, match environment) and
// session_control.cpp (install health, online game start); their comments
// came with them because they record field incidents, not intentions.
#include "FM2K_OnlineLaunch.h"

#include "FM2K_GameInstance.h"
#include "FM2K_GameIni.h"
#include "FM2K_HubClient.h"
#include "../game/FM2K_InstallHealth.h"
#include "../ui/launcher_ui_internal.h"   // lui::Neutralize*EnvVars

#include <SDL3/SDL.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

bool HubPreflightPunch(uint16_t local_port,
                       const std::string& peer_ip,
                       uint16_t peer_port,
                       const std::string& match_token_hex,
                       int timeout_ms)
{
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in laddr{};
    laddr.sin_family = AF_INET;
    laddr.sin_port   = htons(local_port);
    laddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&laddr), sizeof(laddr)) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Preflight: bind() to %d failed err=%d", (int)local_port,
            WSAGetLastError());
        closesocket(s);
        return false;
    }

    DWORD recv_timeout = 100;  // 100ms recvfrom poll
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_timeout), sizeof(recv_timeout));

    sockaddr_in paddr{};
    paddr.sin_family = AF_INET;
    paddr.sin_port   = htons(peer_port);
    if (inet_pton(AF_INET, peer_ip.c_str(), &paddr.sin_addr) != 1) {
        closesocket(s);
        return false;
    }

    // Decode 32-hex match token to 16 binary bytes (matches hub.py + nat_traversal).
    uint8_t token[16] = {};
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    for (size_t i = 0; i + 1 < match_token_hex.size() && i / 2 < 16; i += 2) {
        int hi = nib(match_token_hex[i]);
        int lo = nib(match_token_hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        token[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }

    // CTRL_PUNCH packet: 0xCD 0x10 [16-byte token] -- matches
    // FM2KHook/src/netplay/nat_traversal.cpp wire format.
    uint8_t pkt[2 + 16];
    pkt[0] = 0xCD;
    pkt[1] = 0x10;
    std::memcpy(pkt + 2, token, 16);

    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    auto next_send      = start;
    int  sends_done     = 0;
    bool peer_seen      = false;

    while (std::chrono::steady_clock::now() < deadline) {
        auto now = std::chrono::steady_clock::now();
        if (!peer_seen && now >= next_send && sends_done < 30) {
            sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                   reinterpret_cast<const sockaddr*>(&paddr), sizeof(paddr));
            sends_done++;
            next_send = now + std::chrono::milliseconds(10);
        }

        uint8_t buf[1024];
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n >= 18 && buf[0] == 0xCD && buf[1] == 0x10 &&
            std::memcmp(buf + 2, token, 16) == 0) {
            peer_seen = true;
            // Send a few more punches so the peer also confirms us
            // before we drop the socket. NAT mapping persists through
            // close on cone NATs, but the peer needs one good packet
            // arriving from us to flip its own preflight to "done".
            for (int i = 0; i < 3; ++i) {
                sendto(s, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                       reinterpret_cast<const sockaddr*>(&paddr), sizeof(paddr));
                Sleep(5);
            }
            break;
        }
        // recvfrom returned WSAETIMEDOUT or other err -- loop and try
        // again until deadline.
    }

    closesocket(s);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Preflight: %s in %lldms (%d sends, peer %s:%u, port=%d)",
        peer_seen ? "PEER REACHED" : "TIMED OUT",
        (long long)elapsed, sends_done, peer_ip.c_str(),
        (unsigned)peer_port, (int)local_port);
    return peer_seen;
}

// Primary LAN IPv4 of this machine (see FM2K_Integration.h). UDP-connect a
// throwaway socket toward a public addr -- connect() on a datagram socket sends
// nothing, it just makes the OS pick the source interface it would route
// through -- then getsockname() reads that source back. RFC1918-gated so we
// only ever advertise a real private LAN address as the same-LAN candidate.
std::string fm2k::LocalLanIp() {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(53);
    ::inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);  // route hint only; no traffic
    std::string out;
    if (::connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
        sockaddr_in local{};
        int len = static_cast<int>(sizeof(local));
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            uint32_t h = ntohl(local.sin_addr.s_addr);
            uint8_t a = (h >> 24) & 0xFF, b = (h >> 16) & 0xFF;
            bool priv = (a == 10) || (a == 172 && b >= 16 && b <= 31) ||
                        (a == 192 && b == 168);
            if (priv) {
                char ip[INET_ADDRSTRLEN] = {};
                ::inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
                out = ip;
            }
        }
    }
    ::closesocket(s);
    return out;
}

// IPv6 counterpart of LocalLanIp: discover the GLOBAL-unicast v6 address the OS
// would source for outbound to a global dest (the connect()+getsockname() trick
// auto-handles privacy/temporary address selection). A native global v6 IS the
// routable address -- no STUN/NAT involved -- so this is our cleanest CGNAT
// bypass candidate when both peers have v6. Filter to 2000::/3 (which already
// excludes fe80:: link-local, fc00:: ULA, ::1 loopback and :: unspecified) and
// drop the flaky transition prefixes (Teredo 2001:0::/32, 6to4 2002::/16).
// Returns a bracketless "2xxx::..." or empty. NOTE: mingw's <ws2ipdef.h> has no
// IN6_IS_ADDR_GLOBAL macro -- the by-hand prefix test below is the right call.
std::string fm2k::LocalLanIpV6() {
    SOCKET s = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";
    sockaddr_in6 dst{};
    dst.sin6_family = AF_INET6;
    dst.sin6_port   = htons(53);
    ::inet_pton(AF_INET6, "2001:4860:4860::8888", &dst.sin6_addr);  // route hint only
    std::string out;
    if (::connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
        sockaddr_in6 local{};
        int len = static_cast<int>(sizeof(local));
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            const uint8_t* b = local.sin6_addr.s6_addr;
            bool global = (b[0] & 0xE0) == 0x20;  // 2000::/3 native global unicast
            bool teredo = (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00);
            bool sixto4 = (b[0] == 0x20 && b[1] == 0x02);
            if (global && !teredo && !sixto4) {
                char ip[INET6_ADDRSTRLEN] = {};
                ::inet_ntop(AF_INET6, &local.sin6_addr, ip, sizeof(ip));
                out = ip;
            }
        }
    }
    ::closesocket(s);
    return out;
}

namespace fm2k::online_launch {

PeerPlan ProbePeer(uint16_t local_port, const fm2k::HubEvent& ev) {
    // Preflight punch -- purely informational. We send
    // a quick burst of UDP probes to the peer to wake
    // up the NAT mappings so the in-game GekkoNet
    // handshake has a head start, then ALWAYS proceed
    // to spawn the game. The previous "abort if probe
    // doesn't reply" gate killed legitimate matches
    // because home-router NATs frequently take longer
    // than 2 seconds to punch (or never punch directly
    // and need relay), and the loopback fallback only
    // works for same-box tests. The in-game NAT layer
    // (nat_traversal.cpp) handles STUN, multiple punch
    // rounds, and relay engagement properly -- let it
    // do its job instead of failing fast here.
    //
    // We still TRY the probe so cone-NAT pairs benefit
    // from a few packets in-flight before launch, and
    // we still detect the same-box loopback case so the
    // game gets FM2K_REMOTE_ADDR=127.0.0.1 for that
    // setup specifically.
    PeerPlan plan;
    plan.peer_ip   = ev.match.peer_udp_ip;
    plan.peer_port = ev.match.peer_udp_port;
    plan.public_reachable = HubPreflightPunch(
        local_port,
        plan.peer_ip,
        static_cast<uint16_t>(plan.peer_port),
        ev.match.token,
        2000);
    // Same-box dev test loopback fallback only fires when
    // there is NO hub relay configured. With a relay
    // available, falsely flipping peer_ip to 127.0.0.1
    // sends HELLO into our own loopback while the relay
    // sits idle -- both peers stall at handshake. Trust
    // the public peer_ip on real cross-NAT matches and
    // let the hook's NAT traversal use the relay.
    const bool have_relay = !ev.match.relay_ip.empty()
                         && ev.match.relay_port > 0;
    if (!plan.public_reachable && !have_relay) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: public probe timed out -- trying 127.0.0.1 "
            "in case this is a same-box test");
        if (HubPreflightPunch(
                local_port,
                "127.0.0.1",
                static_cast<uint16_t>(plan.peer_port),
                ev.match.token,
                1000)) {
            plan.peer_ip = "127.0.0.1";
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: loopback responded -- same-box match, "
                "using 127.0.0.1 as remote");
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: probe didn't get a reply. Spawning "
                "game anyway -- in-game NAT traversal "
                "(STUN + punch + relay) will retry on its own.");
        }
    } else if (!plan.public_reachable && have_relay) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: public probe failed; relay configured "
            "(%s:%u). Hook NAT path will fall through to "
            "the relay -- keeping peer=%s:%d as remote.",
            ev.match.relay_ip.c_str(),
            (unsigned)ev.match.relay_port,
            plan.peer_ip.c_str(), plan.peer_port);
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: public probe succeeded -- direct path looks good");
    }
    return plan;
}

void ExportMatchEnv(const fm2k::HubEvent& ev, const std::string& my_id,
                    int local_socd_mode) {
    // Plumb hub coordinates through the spawned game's
    // env so FM2KHook's nat_traversal can fire a STUN
    // probe and authenticated punch on Netplay_Init.
    // Inherited via CreateProcess in
    // FM2KGameInstance::Launch.
    //
    // Pre-resolve the hub host to a dotted-quad IP here
    // so the in-game hook's Netplay_Init -> SendStunProbe
    // -> ResolveHostA path short-circuits instead of
    // doing DNS inside DllMain. DllMain DNS lookups can
    // hang for 5-15 s on flaky resolvers / IPv6-fallback
    // paths, which blows past the inject timeout and
    // looks like a Defender block. Doing the lookup in
    // the launcher (a normal thread, not a loader-lock
    // context) lets us survive slow DNS without timing
    // out the inject. Falls back to the hostname only
    // if resolution fails -- at least then the hook gets
    // its own attempt.
    const char* hub_host_env = std::getenv("FM2K_HUB_HOST");
    const std::string hub_host_str =
        (hub_host_env && hub_host_env[0])
            ? hub_host_env : "hub.2dfm.org";
    std::string hub_udp;
    {
        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(hub_host_str.c_str(), nullptr,
                        &hints, &res) == 0 && res) {
            char ip_str[INET_ADDRSTRLEN] = {};
            const sockaddr_in* sin =
                reinterpret_cast<const sockaddr_in*>(res->ai_addr);
            if (inet_ntop(AF_INET, &sin->sin_addr,
                          ip_str, sizeof(ip_str))) {
                hub_udp = std::string(ip_str) + ":7711";
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hub: pre-resolved %s -> %s for FM2K_HUB_UDP_ADDR "
                    "(keeps DllMain off the DNS path)",
                    hub_host_str.c_str(), ip_str);
            }
            freeaddrinfo(res);
        }
        if (hub_udp.empty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: getaddrinfo('%s') failed; passing hostname "
                "to game (hook will retry, may stall DllMain)",
                hub_host_str.c_str());
            hub_udp = hub_host_str + ":7711";
        }
    }
    ::SetEnvironmentVariableA("FM2K_HUB_UDP_ADDR",   hub_udp.c_str());
    ::SetEnvironmentVariableA("FM2K_HUB_USER_ID",    my_id.c_str());
    ::SetEnvironmentVariableA("FM2K_HUB_MATCH_TOKEN", ev.match.token.c_str());
    // Per-player local SOCD mode. The hook reads
    // FM2K_SOCD_MODE on first GetSOCDMode() call;
    // we pick the local slot's value (host == P1,
    // guest == P2) since this game process is the
    // local user. Each peer keeps its own mode so
    // they don't desync across modes.
    {
        char socd_buf[8];
        std::snprintf(socd_buf, sizeof(socd_buf), "%d", local_socd_mode);
        ::SetEnvironmentVariableA("FM2K_SOCD_MODE", socd_buf);
    }
    if (!ev.match.relay_ip.empty() && ev.match.relay_port > 0) {
        // Pre-resolve the relay host to a dotted-quad for the
        // same reason as FM2K_HUB_UDP_ADDR above: ConfigureRelay
        // -> ResolveHostA -> getaddrinfo runs inside the hook's
        // Netplay_Init (DllMain context). A hostname here would
        // put a DNS lookup on the loader-lock path; resolving it
        // in the launcher (a normal thread) keeps DllMain off
        // DNS. If relay_ip is already a dotted-quad, getaddrinfo
        // short-circuits via the numeric path. On failure we fall
        // back to the hostname so the hook still gets a try.
        std::string relay_host = ev.match.relay_ip;
        bool is_dotted_quad = false;
        {
            in_addr probe{};
            is_dotted_quad =
                (inet_pton(AF_INET, relay_host.c_str(), &probe) == 1);
        }
        if (!is_dotted_quad) {
            addrinfo hints{};
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(relay_host.c_str(), nullptr,
                            &hints, &res) == 0 && res) {
                char ip_str[INET_ADDRSTRLEN] = {};
                const sockaddr_in* sin =
                    reinterpret_cast<const sockaddr_in*>(res->ai_addr);
                if (inet_ntop(AF_INET, &sin->sin_addr,
                              ip_str, sizeof(ip_str))) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Relay: pre-resolved %s -> %s for "
                        "FM2K_HUB_RELAY_ADDR (keeps DllMain off DNS)",
                        relay_host.c_str(), ip_str);
                    relay_host = ip_str;
                }
                freeaddrinfo(res);
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Relay: getaddrinfo('%s') failed; passing "
                    "hostname to game (hook will retry)",
                    relay_host.c_str());
            }
        }
        std::string relay_addr = relay_host + ":" +
                                 std::to_string(ev.match.relay_port);
        ::SetEnvironmentVariableA("FM2K_HUB_RELAY_ADDR",    relay_addr.c_str());
        ::SetEnvironmentVariableA("FM2K_HUB_RELAY_SESSION",
                                  ev.match.relay_session_id.c_str());
    } else {
        ::SetEnvironmentVariableA("FM2K_HUB_RELAY_ADDR",    nullptr);
        ::SetEnvironmentVariableA("FM2K_HUB_RELAY_SESSION", nullptr);
    }

    // Same-LAN candidate: the peer's private addr (hub forwards
    // it from the peer's local_ip). The hook also-punches it so
    // same-router pairs connect directly over the LAN instead of
    // hairpinning + relaying. Cleared when the peer didn't
    // report one (different networks / older client).
    //
    // SAME-MACHINE guard: if the peer's LAN IP == OUR LAN IP,
    // both instances are on this box (self-connect / local
    // 2-instance test, sharing one 192.168.x.x). The same-house
    // punch goes asymmetric there (one side locks the LAN addr,
    // the other the public addr) and the battle wedges. Skip it
    // and let the normal relay/loopback path handle the self-
    // test. No-op for real same-house: two machines have
    // distinct LAN IPs.
    std::string my_lan = fm2k::LocalLanIp();
    if (!ev.match.peer_lan_ip.empty() && ev.match.peer_lan_port > 0 &&
        ev.match.peer_lan_ip != my_lan) {
        std::string peer_lan = ev.match.peer_lan_ip + ":" +
                               std::to_string(ev.match.peer_lan_port);
        ::SetEnvironmentVariableA("FM2K_PEER_LAN_ADDR", peer_lan.c_str());
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: peer same-LAN candidate %s -> FM2K_PEER_LAN_ADDR",
            peer_lan.c_str());
    } else {
        if (!ev.match.peer_lan_ip.empty() &&
            ev.match.peer_lan_ip == my_lan) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: peer LAN IP %s == ours -- same-machine "
                "self-connect, skipping same-house punch",
                ev.match.peer_lan_ip.c_str());
        }
        ::SetEnvironmentVariableA("FM2K_PEER_LAN_ADDR", nullptr);
    }

    // BUG 2: when the hub could not VERIFY the peer's reflexive
    // UDP port (CGNAT + UDP STUN blocked, so it advertised a
    // local-port guess a port-rewriting NAT will have remapped),
    // tell the hook NOT to burst that wrong port -- rely on
    // peer-learning + v6 + relay. Always clear on the verified
    // branch so a stale flag can't leak into the next match in
    // this persistent launcher process.
    if (!ev.match.peer_udp_verified) {
        ::SetEnvironmentVariableA("FM2K_PEER_REFLEXIVE_UNVERIFIED", "1");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: peer reflexive port UNVERIFIED -> "
            "FM2K_PEER_REFLEXIVE_UNVERIFIED=1 (hook skips bursting it)");
    } else {
        ::SetEnvironmentVariableA("FM2K_PEER_REFLEXIVE_UNVERIFIED", nullptr);
    }

    // Phase 1: peer's GLOBAL IPv6 candidate. Bracket notation
    // [ip]:port so the hook parser tells the v6 colons from the
    // port colon. NO same-machine guard -- two real boxes never
    // share a global v6, and a guard would block local v6
    // testing. Direct v6 bypasses CGNAT entirely (no NAT/STUN).
    if (!ev.match.peer_v6_ip.empty() && ev.match.peer_v6_port > 0) {
        std::string v6e = "[" + ev.match.peer_v6_ip + "]:" +
                          std::to_string(ev.match.peer_v6_port);
        ::SetEnvironmentVariableA("FM2K_PEER_V6_ADDR", v6e.c_str());
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: peer v6 candidate %s -> FM2K_PEER_V6_ADDR",
            v6e.c_str());
    } else {
        ::SetEnvironmentVariableA("FM2K_PEER_V6_ADDR", nullptr);
    }

    // Plumb the RANDOM-STAGE parameters into env vars; the
    // hook reads them from the environment it inherits on
    // CreateProcess (#56). Both peers see identical values from
    // the hub, so they run the same xorshift sequence on
    // rematches.
    //
    // WHAT USED TO BE HERE, AND WHY IT IS GONE (2026-08-15).
    // Eleven more FM2K_GP_* variables were written from
    // ev.match.settings, with a comment claiming StartOnlineSession
    // applied them to game.ini before spawn (#54). Nothing in
    // launcher/, FM2KHook/, tools/ or tests/ ever read one of
    // them: that consumer was never written. StartOnlineSession
    // calls game_ini::ApplyForLaunch, which resolves from the
    // LOCAL per-game override file and has no idea MatchSettings
    // exists. So the comment asserted a hub authority the code
    // did not have, and a reader debugging a settings mismatch
    // would start from a false premise.
    //
    // Deleting them is safe because every one of the eleven is
    // owned elsewhere:
    //   * round time/count, game speed, selected stage, SOCD --
    //     carried in-band by HOST_CONFIG, and since the entry
    //     barrier (netplay_control.cpp) agreement on a digest of
    //     the LIVE engine globals is a PRECONDITION of entering
    //     battle, so a guest cannot sim a frame holding
    //     different values;
    //   * player0_cpu / player1_cpu / hit_judge /
    //     game_information -- forced to 0 by ForceOnlineClamps
    //     on every online launch regardless of any of this;
    //   * joystick -- local input device selection, correctly
    //     local;
    //   * vs_mode / vs_single_play / vs_team_play -- netplay
    //     pins team mode to 1v1, so they cannot vary online.
    // Settings parity is owned by HOST_CONFIG plus the entry
    // barrier; the HOST's local override is what both peers
    // converge on.
    const auto& s = ev.match.settings;
    auto set_env = [](const char* k, int v) {
        if (v == -1) { ::SetEnvironmentVariableA(k, nullptr); return; }
        char buf[32]; std::snprintf(buf, sizeof(buf), "%d", v);
        ::SetEnvironmentVariableA(k, buf);
    };
    if (s.random_seed != 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u",
                      (unsigned)s.random_seed);
        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_SEED", buf);
        set_env("FM2K_STAGE_RANDOM_MIN", s.random_stage_min);
        set_env("FM2K_STAGE_RANDOM_MAX", s.random_stage_max);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Random-stage: ENABLED seed=%u range=%d..%d "
            "(env vars set on launcher process; child game "
            "inherits these on CreateProcess)",
            (unsigned)s.random_seed,
            s.random_stage_min, s.random_stage_max);
    } else {
        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_SEED", nullptr);
        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_MIN",  nullptr);
        ::SetEnvironmentVariableA("FM2K_STAGE_RANDOM_MAX",  nullptr);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Random-stage: DISABLED (host's match_settings "
            "carried random_seed=0 -- host hasn't enabled the "
            "Random Stage toggle, or the wire dropped it)");
    }
}

// Program Files / UAC VirtualStore split detector. The split this detects
// does not always produce a failed write: when a VirtualStore mirror already
// exists, every write SUCCEEDS and the damage is that the game is reading a
// different copy of the file than the launcher is. A check that only ran on
// an error path would therefore miss the case that actually silently
// diverges the two processes, which is the one field reports describe as
// "settings don't apply".
//
// Never fatal on its own. StartOnlineGame separately refuses to start when
// the ini could not be applied; this only makes the REASON visible, because
// the reason is otherwise invisible by construction -- under this exact split
// the launcher's log and the game's log are in two different folders.
void LogInstallHealth(const std::filesystem::path& exe_path) {
    const auto report = fm2k::install_health::Probe(exe_path);
    const std::string msg = fm2k::install_health::Describe(report);
    if (msg.empty()) return;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", msg.c_str());
}

StartResult StartOnlineGame(FM2KGameInstance& inst,
                            const std::string& exe_path,
                            FM2K::Engine engine,
                            const NetworkConfig& cfg,
                            bool is_host) {
    // Netplay must not inherit per-game patch envs from a prior OFFLINE
    // launch (they're launcher-process-level): a one-sided team-size or
    // damage-multiplier is a guaranteed desync. Online = vanilla + hook
    // defaults until per-game patches are host-synced.
    lui::NeutralizeGamePatchEnvVars();
    lui::NeutralizeDevLaunchEnvVars();

    // FM95 rollback runs only under FM95_TRAMPOLINE=1 (the hook rides
    // TrampolineFrameTick inside CPW's WinMain loop). Netplay IS the rollback
    // driver, so a CPW online match must enable it -- same as stress. Offline
    // deliberately does NOT set it (CPW runs natively at title->CSS->battle).
    // Per-instance env; no-op on FM2K.
    if (engine == FM2K::Engine::FM95) {
        inst.SetEnvironmentVariable("FM95_TRAMPOLINE", "1");
    }

    uint8_t player_index = is_host ? 0 : 1;
    uint16_t local_port = static_cast<uint16_t>(cfg.local_port);

    // Remote address:
    //   - HUB-DRIVEN: match_start carries the peer's udp_addr in config
    //     for BOTH host and guest. Use it directly.
    //   - JOIN (legacy direct connect): user-pasted "ip:port" in
    //     config.remote_address.
    //   - HOST (legacy direct connect): leave empty so the hook
    //     listens on its socket and learns the peer's address from
    //     the first inbound HELLO. The default "127.0.0.1:7001" from
    //     NetworkConfig's ctor is a UI copy-button placeholder, not
    //     a real peer -- clear it for legacy host.
    std::string remote_addr = cfg.remote_address;
    if (is_host && remote_addr == "127.0.0.1:7001") {
        remote_addr.clear();   // legacy-host placeholder; let hook learn
    }
    if (remote_addr.find(':') == std::string::npos && !remote_addr.empty()) {
        remote_addr += ":7500";  // fallback if user pasted a bare IP
    }

    inst.SetEnvironmentVariable("FM2K_PLAYER_INDEX", std::to_string(player_index));
    inst.SetEnvironmentVariable("FM2K_LOCAL_PORT", std::to_string(local_port));
    inst.SetEnvironmentVariable("FM2K_REMOTE_ADDR", remote_addr);

    // Parity recorder: OPT-IN ONLY. It writes per-frame state snapshots
    // (RNG, game_timer, render_fc) to a .pty for host-vs-spectator diffing
    // with tools/kgt_diff_pty.
    //
    // This used to DEFAULT the path to an absolute `c:/games/2dfm/wanwan/
    // parity_p<N>.pty` whenever the env var was unset -- i.e. on every user's
    // online session, pointing at one developer's drive. On any other machine
    // the directory does not exist, the recorder's fopen fails, and nothing
    // reports it: only the SUCCESS branch logs (main_loop_trampoline.cpp), so
    // the failure is invisible by construction. A diagnostic that is on by
    // default, writes somewhere the user never chose, and cannot report its
    // own failure is worse than one that is simply off.
    //
    // Now: if the operator sets FM2K_PARITY_RECORD_PATH the child inherits it
    // untouched; otherwise no recording happens. Same for the spectator
    // instance in process_manager.cpp.

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Online session: P%d port=%d remote=%s",
        player_index + 1, local_port, remote_addr.c_str());

    // Bake the host's resolved [GamePlay] config (defaults + per-game
    // overrides + online anti-cheat clamps) into the game's own
    // game.ini before CreateProcess. The game reads this file at
    // startup; by writing it now both peers boot with the same round
    // count / time / stage / etc. We restore the original ini in
    // StopSession so leaving the launcher doesn't permanently mutate
    // the user's offline settings. is_online=true forces HitJudge +
    // GameInformation = 0 (debug overlays are cheating online).
    //
    // THE RETURN VALUE IS LOAD-BEARING ON THIS PATH AND WAS BEING DISCARDED.
    // ApplyForLaunch returns false when it could not take its one-time backup
    // of game.ini (read-only directory, a Program Files / VirtualStore split,
    // the file locked by another process) and therefore refused to write, to
    // avoid clobbering the user's settings with no way back. Ignoring that
    // meant the game launched ONLINE with game.ini untouched -- i.e. without
    // ForceOnlineClamps, so HitJudge and GameInformation debug overlays go
    // live in a rated match and the round config is whatever was on disk. The
    // only trace was a WARN in launcher.log, which under exactly the
    // VirtualStore split that causes this is not where the user is looking.
    //
    // This is the anti-cheat path, so refusing to start is the defensible
    // behaviour: same shape as the Launch failure immediately below.
    //
    // ONE RETRY before refusing. The only failure this has ever been observed
    // to take in practice is a same-install collision between two launcher
    // processes (measured 2026-08-15: the hub-brokered E2E harness runs three
    // launchers against one game directory). Both known collision points are
    // fixed at the source -- the concurrent-backup carve-out and the per-pid
    // temp file in FM2K_GameIni.cpp -- so this retry is belt-and-braces against
    // any remaining transient sharing violation, not the fix for one. A genuine
    // permissions failure fails both attempts in a few hundred microseconds.
    LogInstallHealth(exe_path);
    bool ini_ok = fm2k::game_ini::ApplyForLaunch(exe_path, /*is_online=*/true);
    if (!ini_ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "GameIni: apply-for-launch failed; retrying once in 250ms before "
            "refusing the online session");
        ::Sleep(250);
        ini_ok = fm2k::game_ini::ApplyForLaunch(exe_path, /*is_online=*/true);
    }
    if (!ini_ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Online session ABORTED: could not apply game.ini for launch, so the "
            "online anti-cheat clamps (HitJudge/GameInformation = 0) and the "
            "shared round config were NOT written. Most likely cause: the game "
            "directory is not writable (Program Files / UAC VirtualStore, or the "
            "file is open elsewhere). Move the game out of Program Files or fix "
            "the permissions on '%s' and try again.",
            exe_path.c_str());
        // RESCUE A DELETED game.ini (Wave-2 review B3b). ApplyForLaunch can
        // fail INSIDE Save(), whose overwrite fallback is `remove(game.ini)` +
        // `rename(tmp, game.ini)`: if that second rename also fails, game.ini is
        // GONE and the temp has been removed too. Returning from here without
        // putting it back would leave the user with no game.ini at all, because
        // the only RestoreFromBackup call is in StopSession and this path never
        // reaches it (the session never starts).
        //
        // CONDITIONAL ON THE FILE BEING MISSING, and that is load-bearing.
        // RestoreFromBackup is `remove(ini) + rename(bak, ini)`: it CONSUMES
        // the backup. On a shared install (two launchers, one game directory --
        // the case that produced this whole family of races) an unconditional
        // restore here would revert the OTHER launcher's freshly applied
        // game.ini while its game is booting AND eat the backup its own
        // StopSession needs. Restoring only when the file has actually
        // disappeared fixes the destructive case and cannot touch a healthy one.
        if (!exe_path.empty()) {
            std::error_code ini_ec;
            const auto ini_path = fm2k::game_ini::PathForExe(exe_path);
            if (!std::filesystem::exists(ini_path, ini_ec)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameIni: game.ini is MISSING after the failed apply -- "
                    "restoring it from the backup before refusing the session");
                fm2k::game_ini::RestoreFromBackup(exe_path);
            }
        }
        return StartResult::IniRefused;
    }

    if (!inst.Launch(exe_path, engine)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch game for online session.");
        return StartResult::LaunchFailed;
    }
    return StartResult::Ok;
}

}  // namespace fm2k::online_launch
