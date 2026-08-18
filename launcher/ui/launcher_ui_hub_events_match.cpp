// launcher_ui_hub_events_match.cpp -- LauncherUI::HandleMatchStartEvent.
// Extracted from HandleHubEvent's 358-line K::MatchStart case (the heaviest
// single hub-event handler: drops modals, resolves peer/host roles, seeds
// the battle session, kicks the game launch). Re-derives hs/K like the
// dispatcher; body is verbatim.
#include "FM2K_Integration.h"
#include "launcher_ui_hub_internal.h"
#include "launcher_ui_hubstate.h"  // LauncherUI::HubState full def
#include "launcher_ui_internal.h"  // shared persistence helpers (namespace lui)
#include "FM2K_HubClient.h"
#include "FM2K_PortMapper.h"  // UPnP port mapper member of LauncherUI (Phase 1)
#include "FM2K_DiscordAuth.h"
#include "FM2K_Locale.h"
#include "FM2K_Updater.h"
#include "version_local.h"
#include "auto_upload_secret.h"
#include "FM2K_UploadQueue.h"
#include "FM2KHook/src/ui/input_binder.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/util/pii_scrub.h"
#include "FM2K_GameIni.h"
#include "FM2K_DDrawRedirect.h"
#include "FM2K_CncDDraw.h"
#include "FM2K_Utf8Path.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <shellapi.h>  // Shell_NotifyIcon for challenge toast
#include <shobjidl.h>  // IFileOpenDialog (modern native folder picker)
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include "vendored/imgui/imgui.h"
#include "imgui_internal.h"
#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>

using namespace lui;  // shared persistence helpers (launcher_ui_internal.h)

void LauncherUI::HandleMatchStartEvent(const fm2k::HubEvent& ev) {
    auto& hs = *hub_state_;
    using K = fm2k::HubEvent::Kind;  // body references K::RecordReceived
                // Match is on -- drop both modals (incoming and outgoing)
                // and clear any pending challenge state on both sides.
                // Without clearing the incoming modal here, the
                // accepter sees their challenge dialog persist after
                // accept (it's normally dismissed by the click handler,
                // but kb-shortcut accepts or hub-side timeouts can
                // leave it visible).
                hs.show_outgoing_challenge_modal = false;
                hs.outgoing_challenge_to_id.clear();
                hs.outgoing_challenge_to_nick.clear();
                hs.show_challenge_modal = false;
                hs.pending_challenge_from_id.clear();
                hs.pending_challenge_from_nick.clear();
                // Cancel any pending taskbar flash from a prior
                // FireChallengeNotification / FireSystemNotification
                // call. FLASHW_TIMERNOFG only auto-stops when the
                // window comes to the foreground; if the user accepted
                // via the modal without focusing the launcher first
                // (e.g. clicked through Discord's notification toast),
                // the flash keeps blinking forever. FLASHW_STOP forces
                // it off explicitly. No-op if nothing was flashing.
                if (window_) {
                    HWND hwnd = (HWND)SDL_GetPointerProperty(
                        SDL_GetWindowProperties(window_),
                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
                    if (hwnd) {
                        FLASHWINFO fi = { sizeof(fi), hwnd,
                                          FLASHW_STOP, 0, 0 };
                        FlashWindowEx(&fi);
                    }
                }
                // Remember the match token so the shared-mem outcome
                // poll can correlate the hook's report with the hub's
                // match record. Cleared after we send match_result.
                hs.current_match_token    = ev.match.token;
                hs.current_match_role     = ev.match.role;
                hs.current_match_peer_id  = ev.match.peer.id;
                hs.current_match_peer_nick= ev.match.peer.nick;
                // Prefer the hub's actual launchable game id (exe stem)
                // over the room id: a canonical family room ("URORFG")
                // matches no locally-discovered game and the spawn fails
                // (live 2026-07-17). Old hubs send no game_id -> room id.
                hs.current_match_game_id  = ev.match.game_id.empty()
                                              ? hs.current_room_id
                                              : ev.match.game_id;
                hs.current_match_settings = ev.match.settings;
                hs.match_result_sent      = false;
                hs.disconnect_toast_fired = false;
                hs.last_outcome_seq.clear();
                // Reset chars_seq tracking -- fresh game spawn means a
                // fresh shared-mem mapping with seq=0; first
                // Netplay_StartBattleSession will bump to 1 and fire
                // match_progress against this token.
                hs.last_chars_seq.clear();
                // Push the freshly-set peer nick + cached vs record into
                // the spawned game's shared mem. The hook reads from
                // there to fill the in-game titlebar; without this, the
                // first time the game window renders post-MatchStart it
                // would show "vs <empty>" until the next K::RecordReceived.
                PushStatsToHook();
                hs.status_line = "match_start: " + ev.match.role +
                    " peer=" + ev.match.peer.nick +
                    " udp=" + ev.match.peer_udp_ip + ":" +
                    std::to_string(ev.match.peer_udp_port);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: %s", hs.status_line.c_str());

                // Three preconditions must hold to actually launch:
                //   (1) peer reported a non-zero UDP port
                //   (2) we have the room's game installed
                //   (3) the launcher exposes on_online_session_start
                // Failing any of these, tell the hub the "match" is over
                // immediately so both peers go back to idle -- otherwise
                // the lobby reads "in_match" forever and they can't
                // re-challenge or pick a new game.
                int idx = FindInstalledGameForRoom(games_, hs.current_room_id);
                bool ok = (ev.match.peer_udp_port > 0)
                       && (idx >= 0)
                       && (on_online_session_start != nullptr);

                if (ok) {
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
                    hs.status_line = "preflight: punching peer (best-effort)...";
                    std::string peer_ip   = ev.match.peer_udp_ip;
                    int         peer_port = ev.match.peer_udp_port;
                    const bool public_reachable = HubPreflightPunch(
                        static_cast<uint16_t>(network_config_.local_port),
                        peer_ip,
                        static_cast<uint16_t>(peer_port),
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
                    if (!public_reachable && !have_relay) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: public probe timed out -- trying 127.0.0.1 "
                            "in case this is a same-box test");
                        if (HubPreflightPunch(
                                static_cast<uint16_t>(network_config_.local_port),
                                "127.0.0.1",
                                static_cast<uint16_t>(peer_port),
                                ev.match.token,
                                1000)) {
                            peer_ip = "127.0.0.1";
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Hub: loopback responded -- same-box match, "
                                "using 127.0.0.1 as remote");
                        } else {
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Hub: probe didn't get a reply. Spawning "
                                "game anyway -- in-game NAT traversal "
                                "(STUN + punch + relay) will retry on its own.");
                        }
                    } else if (!public_reachable && have_relay) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: public probe failed; relay configured "
                            "(%s:%u). Hook NAT path will fall through to "
                            "the relay -- keeping peer=%s:%d as remote.",
                            ev.match.relay_ip.c_str(),
                            (unsigned)ev.match.relay_port,
                            peer_ip.c_str(), peer_port);
                    } else {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: public probe succeeded -- direct path looks good");
                    }
                    hs.status_line = "match starting...";

                    // Make sure the launcher's selected_game_ record is
                    // up to date even if RoomJoined fired before games
                    // discovery completed.
                    if (on_game_selected) on_game_selected(games_[idx]);

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
                    ::SetEnvironmentVariableA("FM2K_HUB_USER_ID",    hs.my_id.c_str());
                    ::SetEnvironmentVariableA("FM2K_HUB_MATCH_TOKEN", ev.match.token.c_str());
                    // Per-player local SOCD mode. The hook reads
                    // FM2K_SOCD_MODE on first GetSOCDMode() call;
                    // we pick the local slot's value (host == P1,
                    // guest == P2) since this game process is the
                    // local user. Each peer keeps its own mode so
                    // they don't desync across modes.
                    if (!socd_state_loaded_) {
                        socd_state_loaded_ = true;
                        LoadSocdState();
                    }
                    {
                        const int local_slot = (ev.match.role == "host") ? 0 : 1;
                        char socd_buf[8];
                        std::snprintf(socd_buf, sizeof(socd_buf), "%d",
                                      socd_mode_[local_slot]);
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

                    NetworkConfig cfg = network_config_;
                    cfg.session_mode = SessionMode::ONLINE;
                    cfg.is_host = (ev.match.role == "host");
                    cfg.remote_address =
                        peer_ip + ":" + std::to_string(peer_port);

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

                    on_online_session_start(cfg);
                } else {
                    const char* reason =
                        (ev.match.peer_udp_port == 0) ? "peer never sent udp_addr" :
                        (idx < 0)                     ? "game not in your library" :
                                                        "launcher missing online callback";
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: match_start aborted (%s) -- sending match_ended", reason);
                    hs.status_line = std::string("match aborted: ") + reason;
                    hs.client.MatchEnded();
                }
}
