// launcher_ui_hub_events.cpp -- LauncherUI::HandleHubEvent, the hub WS event
// dispatch switch, extracted from RenderHubPanel (was the hs.client.Poll([&]{...})
// lambda body). Behavior-preserving: same switch, now a member method invoked
// per-event by the Poll call in RenderHubPanel.
// launcher_ui_hub.cpp -- LauncherUI hub/lobby panel + match polling + hub helpers. Split from FM2K_LauncherUI.cpp. NOTE: RenderHubPanel is large; flagged for follow-up factoring.
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

void LauncherUI::HandleHubEvent(const fm2k::HubEvent& ev) {
    auto& hs = *hub_state_;
        using K = fm2k::HubEvent::Kind;
        switch (ev.kind) {
            case K::Connected:
                hs.my_id = ev.user_id;
                hs.rooms = ev.rooms;
                hs.status_line = "connected";
                // Drain any match results queued while the WS was down.
                // PollMatchOutcome falls through to push here when
                // hs.client.IsConnected() is false (long pause →
                // keepalive timeout → drop is the classic case; pre-
                // v0.2.45 those outcomes were silently lost for the
                // rest of the set). Local CSV was already written at
                // outcome time, so this is purely hub catch-up.
                if (!hs.pending_match_results.empty()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: replaying %zu queued match_result(s) on reconnect",
                        hs.pending_match_results.size());
                    for (const auto& q : hs.pending_match_results) {
                        hs.client.MatchResult(
                            q.token, q.outcome,
                            q.p1_char_id, q.p2_char_id,
                            q.p1_char_name, q.p2_char_name,
                            q.stage_id, q.stage_name,
                            q.session_id, q.match_index,
                            q.rounds);
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "  replayed token=%.8s... outcome=%s",
                            q.token.c_str(), q.outcome.c_str());
                    }
                    hs.pending_match_results.clear();
                    // Refresh the record + recent-matches after the
                    // catch-up so the UI shows the corrected totals
                    // right away.
                    hs.client.QueryRecord();
                    hs.client.RequestRecentMatches(50);
                }
                // Process-wide FM2K_HUB_USER_ID — companion to FM2K_HUB_UDP_ADDR
                // set at hub-connect time. Together they unlock the hook's
                // SendStunProbe call, so any spawned game (player or spec)
                // gets STUN'd and hub's user.udp_addr reflects this hook's
                // actual external UDP mapping.
                ::SetEnvironmentVariableA("FM2K_HUB_USER_ID", hs.my_id.c_str());
                // Tell the hub our planned UDP listen so it can relay
                // it to a peer in match_start. Both launchers register
                // their already-configured network_config_.local_port.
                // For LAN/internet, replace "127.0.0.1" with the hub-
                // observed reflexive IP (Phase 2 — STUN responder).
                // Spec hook binds its TCP listener to the same port as UDP
                // (convention enforced in spectator_tcp.cpp's Start). Send
                // both so the hub can forward spec_tcp_port in the
                // spectator_incoming event for the host's TCP punch.
                hs.client.SendUdpAddr("127.0.0.1", network_config_.local_port,
                                      network_config_.local_port,
                                      fm2k::LocalLanIp(),
                                      fm2k::LocalLanIpV6());

                // UPnP automatic port mapping (Phase 1 NAT reachability).
                // Kick off an asynchronous router mapping of our game UDP
                // port so peers can reach us directly. This runs ALONGSIDE
                // the SendUdpAddr above -- we never block on it; if UPnP is
                // slow or absent the SendUdpAddr already gave the hub our
                // STUN-learned endpoint, so behavior is exactly today's. On
                // success the per-frame poll in RenderHubPanel re-sends
                // udp_addr with the external endpoint (see below).
                //
                // Skip entirely for a loopback hub: that's the local-dev /
                // multi-instance test path (the netcfg hint literally says
                // "use 127.0.0.1 when running your own hub.py"), where two
                // launchers on one machine must NOT both grab the same
                // router mapping. Real online sessions always run against a
                // non-loopback hub.
                {
                    const char* hub_host_env = std::getenv("FM2K_HUB_HOST");
                    std::string hh = (hub_host_env && hub_host_env[0])
                                   ? hub_host_env
                                   : (hub_host_[0] ? hub_host_ : "hub.2dfm.org");
                    const bool loopback_hub =
                        (hh == "127.0.0.1" || hh == "localhost" || hh == "::1");
                    if (!loopback_hub) {
                        if (!port_mapper_) {
                            port_mapper_ = std::make_unique<fm2k::PortMapper>();
                        }
                        // Reset the re-send latch so a reconnect re-arms the
                        // one-shot Mapped transition send.
                        port_mapper_last_state_ = -1;
                        port_mapper_->StartAsync(
                            static_cast<uint16_t>(network_config_.local_port));
                    } else {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[upnp] loopback hub (%s) -- skipping UPnP mapping",
                            hh.c_str());
                    }
                }
                // Pre-match STUN. The in-game hook does its own STUN at
                // launch but match_start fires immediately on accept and
                // the hook's STUN doesn't arrive at the hub for several
                // seconds (after game spawn + hook init). Without a
                // pre-match STUN, hub.peer_dict() falls back to the
                // launcher-reported local port, which only matches the
                // external NAT mapping on port-preserving cone NATs —
                // every other client gets the wrong port and punches a
                // closed door. Doing this here means the hub has the
                // correct (ip, ext_port) before any challenge fires.
                {
                    const char* hub_host_env = std::getenv("FM2K_HUB_HOST");
                    std::string hub_host = (hub_host_env && hub_host_env[0])
                                         ? hub_host_env
                                         : (hub_host_[0] ? hub_host_ : "hub.2dfm.org");
                    // The probe doubles as NAT classification (Phase 2a): it
                    // still STUNs :7711 (registering user.udp_addr exactly as
                    // before) and additionally probes :7714 to derive
                    // cone/symmetric. Report the result to the hub so it lands
                    // in peer_dict for the next match_start.
                    fm2k::NatClassifyResult nat = fm2k::LauncherStunClassify(
                        static_cast<uint16_t>(network_config_.local_port),
                        hub_host, 7711, hs.my_id);
                    hs.last_stun_refresh_ms = static_cast<uint32_t>(SDL_GetTicks());
                    hs.client.SendNatType(nat.nat_type);
                    hs.last_nat_type = nat.nat_type;
                }
                // Pull our own W/L/D + per-opponent breakdown so the
                // lobby column and titlebar both have data on first
                // render. Refreshed after every match end via the same
                // QueryRecord call in PollMatchOutcome.
                hs.client.QueryRecord();
                // Pre-load the recent-matches panel so it isn't blank
                // the first time the user opens it. 50 rows is the
                // hub's default cap; anything more is a "history page"
                // which we don't ship yet.
                hs.client.RequestRecentMatches(50);
                // Snapshot current in-flight matches for the lobby
                // panel. Live updates arrive via MatchInProgress*
                // broadcasts after this point.
                hs.client.RequestCurrentMatches();
                // Auto-rejoin: if we were in a room when the hub
                // dropped (hub restart, network blip), put us back
                // in. last_room_id is set by the Disconnected handler
                // before it clears current_room_id. The hub re-creates
                // the room on demand if it's not seeded, so the name
                // we pass is purely cosmetic — fall back to id when we
                // don't have a separately-cached display name.
                if (!hs.last_room_id.empty()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: auto-rejoining room '%s' after reconnect",
                        hs.last_room_id.c_str());
                    const std::string& name = hs.last_room_name.empty()
                        ? hs.last_room_id : hs.last_room_name;
                    hs.client.JoinRoom(hs.last_room_id, name);
                    hs.last_room_id.clear();
                    hs.last_room_name.clear();
                }
                break;
            case K::Disconnected:
                // Stash the room we were in so the next K::Connected
                // event can auto-rejoin (hub restart shouldn't kick
                // users back to the game-picker).
                if (!hs.current_room_id.empty()) {
                    hs.last_room_id = hs.current_room_id;
                    // Look up the display name from the cached rooms
                    // list so the rejoin call carries something
                    // sensible if the hub doesn't seed this room.
                    for (const auto& r : hs.rooms) {
                        if (r.id == hs.current_room_id) {
                            hs.last_room_name = r.name;
                            break;
                        }
                    }
                }
                hs.users.clear();
                hs.current_room_id.clear();
                hs.my_id.clear();
                hs.status_line = ev.error.empty() ? "disconnected" : ("disconnected: " + ev.error);
                break;
            case K::RoomList:
                hs.rooms = ev.rooms;
                break;
            case K::RoomJoined: {
                if (!ev.rooms.empty()) hs.current_room_id = ev.rooms.front().id;
                hs.users.clear();
                for (auto& u : ev.users) hs.users[u.id] = u;
                // Auto-select the installed game matching this room and
                // ALSO fire on_game_selected so the launcher's
                // FM2KLauncher::selected_game_ record is populated —
                // not just our local UI mirror selected_game_index_.
                // Without this, StartOnlineSession bails on
                // selected_game_.exe_path.empty() even though the UI
                // showed a selection.
                int idx = FindInstalledGameForRoom(games_, hs.current_room_id);
                if (idx >= 0) {
                    selected_game_index_ = idx;
                    if (on_game_selected) on_game_selected(games_[idx]);
                    hs.status_line = "auto-selected installed game: "
                        + fm2k::utf8path::StemUtf8(
                            std::filesystem::path(
                                fm2k::utf8path::Utf8ToWide(games_[idx].exe_path)));
                } else {
                    hs.status_line = "joined room '" + hs.current_room_id +
                        "' — game not in your library, install it before challenging";
                }
                break;
            }
            case K::RoomLeft:
                hs.current_room_id.clear();
                // Explicit leave — clear the auto-rejoin snapshot so a
                // subsequent hub disconnect doesn't drag us back into
                // a room we just left.
                hs.last_room_id.clear();
                hs.last_room_name.clear();
                hs.users.clear();
                break;
            case K::UserJoined:
                if (ev.room_id == hs.current_room_id) hs.users[ev.user.id] = ev.user;
                break;
            case K::UserLeft:
                if (ev.room_id == hs.current_room_id) hs.users.erase(ev.user_id);
                break;
            case K::UserStatus: {
                // Fast peer-abort detection: when our match peer's hub
                // status TRANSITIONS from in_match to idle while we
                // haven't reported our own match_result yet (i.e. peer
                // closed window / Alt-F4'd before the match concluded
                // normally), tear down the local session immediately.
                // Without this we'd wait for the in-game GekkoNet
                // timeout (~5s) before the hook publishes DISCONNECT.
                //
                // Two guards stop the spam:
                //   - Compare against the PREVIOUSLY-cached status —
                //     fire only on the in_match → !in_match edge, not
                //     on every periodic re-broadcast of "idle".
                //   - hs.match_result_sent — at normal match end the
                //     peer also goes idle, but we already sent our own
                //     outcome, so don't double-handle that as an abort.
                std::string prev_status;
                if (auto it = hs.users.find(ev.user.id); it != hs.users.end()) {
                    prev_status = it->second.status;
                }
                hs.users[ev.user.id] = ev.user;

                const bool is_match_peer =
                    !hs.current_match_peer_id.empty() &&
                    ev.user.id == hs.current_match_peer_id;
                const bool transitioned_out =
                    prev_status == "in_match" &&
                    ev.user.status != "in_match";
                const bool peer_aborted =
                    is_match_peer && transitioned_out &&
                    !hs.current_match_token.empty() &&
                    !hs.match_result_sent;

                if (peer_aborted) {
                    const std::string& peer_nick =
                        hs.current_match_peer_nick.empty()
                            ? std::string("Opponent")
                            : hs.current_match_peer_nick;
                    char body[160];
                    std::snprintf(body, sizeof(body),
                                  T("toast_peer_disconnected_body"),
                                  peer_nick.c_str());
                    hs.status_line = "peer left match — closing local game";
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Hub: %s (peer status=%s)",
                                hs.status_line.c_str(),
                                ev.user.status.c_str());
                    if (!hs.disconnect_toast_fired) {
                        hs.disconnect_toast_fired = true;
                        FireSystemNotification(
                            T("toast_peer_disconnected_title"), body);
                        PushHudSystemMessage(
                            T("toast_peer_disconnected_title"), 5000);
                    }
                    // No MatchResult here. This branch fires when the
                    // peer's hub status flipped off in_match WITHOUT us
                    // having sent our own match_result yet — meaning
                    // battle hadn't ended (we'd have already published
                    // and recorded otherwise). CSS-phase aborts must not
                    // count toward W/L/D. The hub's in-flight match
                    // sweeps cleanly via the "ambiguous, drop" branch
                    // when both peers go silent without reports.
                    // match_result_sent stays false; the hook's
                    // CSS_ABORT path (if it fires before this) already
                    // cleared current_match_token, and we clear again
                    // here so re-broadcasts don't re-trigger.
                    if (on_session_stop) on_session_stop();
                    // Clear so a re-broadcast of the same idle status
                    // can't re-trigger this branch.
                    hs.current_match_token.clear();
                    hs.current_match_peer_id.clear();
                    hs.current_match_peer_nick.clear();
                }
                break;
            }
            case K::UserRtt:
                if (auto it = hs.users.find(ev.user_id); it != hs.users.end()) {
                    it->second.rtt_ms = ev.rtt_ms;
                }
                break;
            case K::ChallengeReceived:
                hs.pending_challenge_from_id   = ev.challenge.from_id;
                hs.pending_challenge_from_nick = ev.challenge.from_nick;
                hs.pending_challenge_settings  = ev.challenge.settings;
                hs.show_challenge_modal = true;
                FireChallengeNotification(ev.challenge.from_nick);
                break;
            case K::ChallengeFailed:
                hs.status_line = "challenge failed: " + ev.error;
                hs.show_outgoing_challenge_modal = false;
                hs.outgoing_challenge_to_id.clear();
                hs.outgoing_challenge_to_nick.clear();
                break;
            case K::ChallengeCancelled:
                // Server tells US our outbound challenge was cancelled
                // (e.g., target went offline) OR an inbound challenge
                // was cancelled by the sender. Same handling for both:
                // close any open modal that referenced it.
                hs.show_challenge_modal = false;
                hs.pending_challenge_from_id.clear();
                hs.show_outgoing_challenge_modal = false;
                hs.outgoing_challenge_to_id.clear();
                hs.outgoing_challenge_to_nick.clear();
                hs.status_line = "challenge cancelled";
                break;
            case K::ChallengeDeclined:
                hs.status_line = "challenge declined by " +
                    (hs.outgoing_challenge_to_nick.empty()
                        ? std::string("opponent")
                        : hs.outgoing_challenge_to_nick);
                hs.show_outgoing_challenge_modal = false;
                hs.outgoing_challenge_to_id.clear();
                hs.outgoing_challenge_to_nick.clear();
                break;
            case K::MatchStart:
                HandleMatchStartEvent(ev);
                break;
            case K::MatchRotated: {
                // Hub minted a fresh in-flight token after the previous
                // match committed. FM2K rematches stay inside the same
                // hub_session (peers loop CSS → battle → CSS without
                // respawning the game), so the next outcome publish
                // would otherwise be sent under the OLD (already-
                // committed) token and silently dropped by the hub.
                // Update token + reset match_result_sent. No game spawn —
                // we're just relabeling the in-flight match.
                //
                // Critically: do NOT clear last_outcome_seq. The hook's
                // match_outcome_seq monotonically increments across all
                // matches in the same process. Clearing the launcher's
                // last_seen would re-trigger PollMatchOutcome on the
                // already-processed seq, sending the previous match's
                // outcome under the rotated token → infinite commit/
                // rotate loop. The hook bumps seq on the NEXT match's
                // outcome publish, which correctly compares > the
                // preserved last_seen.
                hs.current_match_token = ev.match.token;
                hs.match_result_sent   = false;
                hs.disconnect_toast_fired = false;
                // Don't touch last_chars_seq — the same hook (same
                // PID) keeps incrementing the seq across rotates.
                // The next Netplay_StartBattleSession will bump it,
                // PollMatchOutcome will see seq advance, and fire
                // match_progress under the new token. Clearing
                // here would re-fire the prev battle's chars under
                // the new token.
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: match rotated, new token=%.8s...",
                            hs.current_match_token.c_str());
                break;
            }
            case K::PeerDisconnected: {
                // Hub-side: peer's WebSocket dropped. The hook on the
                // surviving instance will *usually* notice via GekkoNet
                // peer-timeout and publish a DISCONNECT outcome, but we
                // can't always count on that (e.g. peer was idle in CSS
                // and the GekkoNet session is between matches). Stop
                // the local session here too so the survivor doesn't
                // hang on the menu screen waiting for someone who's
                // gone. Idempotent — second StopSession is a no-op.
                const std::string& peer_nick =
                    hs.current_match_peer_nick.empty()
                        ? std::string("Opponent")
                        : hs.current_match_peer_nick;
                char body[160];
                std::snprintf(body, sizeof(body),
                              T("toast_peer_disconnected_body"),
                              peer_nick.c_str());
                hs.status_line = "peer disconnected — closing match";
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: %s", hs.status_line.c_str());
                if (!hs.disconnect_toast_fired) {
                    hs.disconnect_toast_fired = true;
                    FireSystemNotification(T("toast_peer_disconnected_title"), body);
                    PushHudSystemMessage(
                        T("toast_peer_disconnected_title"), 5000);
                }
                if (on_session_stop) on_session_stop();
                // Best-effort match_result so the hub closes its
                // in-flight record. If we never had a current_match_token
                // (e.g. peer dropped before MatchStart fired), this is
                // a no-op on the hub side — match_id won't correlate.
                if (!hs.current_match_token.empty() && !hs.match_result_sent) {
                    hs.client.MatchResult(hs.current_match_token, "disconnect");
                    hs.match_result_sent = true;
                }
                break;
            }
            case K::SpectateGranted: {
                hs.status_line = "spectate: " + ev.spectate.target_nick +
                                 " vs " + ev.spectate.opponent_nick +
                                 " @ " + ev.spectate.host_ip + ":" +
                                 std::to_string(ev.spectate.host_port);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hub: %s", hs.status_line.c_str());
                // Spec relay fallback creds (task #58): same env names the
                // match path uses (ConfigureRelay reads them unchanged),
                // same set-OR-CLEAR discipline so a spectate after a
                // relayed match (or vice versa) can't inherit the other
                // session's relay. Hub advertises an IP in prod; if it
                // ever sends a hostname the hook's own resolve is the
                // fallback (same caveat as the match path's pre-resolve).
                if (!ev.spectate.spec_relay_addr.empty() &&
                    !ev.spectate.spec_relay_session.empty()) {
                    ::SetEnvironmentVariableA("FM2K_HUB_RELAY_ADDR",
                                              ev.spectate.spec_relay_addr.c_str());
                    ::SetEnvironmentVariableA("FM2K_HUB_RELAY_SESSION",
                                              ev.spectate.spec_relay_session.c_str());
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: spec relay fallback available at %s",
                        ev.spectate.spec_relay_addr.c_str());
                } else {
                    ::SetEnvironmentVariableA("FM2K_HUB_RELAY_ADDR",    nullptr);
                    ::SetEnvironmentVariableA("FM2K_HUB_RELAY_SESSION", nullptr);
                }
                if (on_spectate_match) {
                    on_spectate_match(ev.spectate.host_ip, ev.spectate.host_port,
                                      ev.spectate.session_kind,
                                      ev.spectate.spec_transport);
                }
                break;
            }
            case K::SpectateDenied:
                hs.status_line = "spectate denied: " + ev.error;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Hub: %s", hs.status_line.c_str());
                break;
            case K::SpectatorIncoming: {
                // We're the host of an in-progress match; hub forwarded a
                // spectator's external UDP+TCP addr. Forward to game-instance
                // shared mem → hook's TickHostMaintenance fires both:
                //   * UDP heartbeat burst (existing — opens NAT for the
                //     spectator's first SPEC_JOIN_REQ replies)
                //   * TCP simultaneous-open punch (new in v0.2.35 — opens
                //     NAT for inbound TCP from spec:tcp_port to our
                //     listener port, the path the INPUT_BATCH stream uses)
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hub: spectator_incoming nick=%s addr=%s udp:%d tcp:%d — punching",
                    ev.spectator_incoming.spec_nick.c_str(),
                    ev.spectator_incoming.spec_udp_ip.c_str(),
                    ev.spectator_incoming.spec_udp_port,
                    ev.spectator_incoming.spec_tcp_port);
                if (on_spectator_punch_target) {
                    on_spectator_punch_target(
                        ev.spectator_incoming.spec_udp_ip,
                        ev.spectator_incoming.spec_udp_port,
                        ev.spectator_incoming.spec_tcp_port,
                        ev.spectator_incoming.spec_user_id);
                }
                break;
            }
            case K::SpecRelayBinary: {
                // Phase 3: hub forwarded spec data bytes to us. Hand to
                // the launcher controller which writes them into the
                // inbound shared-mem ring for the running spec game.
                // Dispatch via the same callback pattern as
                // on_spectator_punch_target so launcher controls the
                // mapping lifecycle.
                if (on_spec_relay_bytes && !ev.spec_relay_bytes.empty()) {
                    on_spec_relay_bytes(ev.spec_relay_bytes);
                }
                break;
            }
            case K::RecordReceived: {
                // Only the unfiltered global-record reply (no opponent_id /
                // game_id filter) carries the per-opponent breakdown that
                // populates the lobby column. Filtered queries from other
                // call sites (per-game tab, per-opponent tooltip) overwrite
                // their own narrower views and shouldn't clobber the
                // overall numbers.
                if (ev.record.user_id == hs.my_id &&
                    ev.record.opponent_id.empty() &&
                    ev.record.game_id.empty())
                {
                    hs.my_wins   = ev.record.wins;
                    hs.my_losses = ev.record.losses;
                    hs.my_draws  = ev.record.draws;
                    hs.my_vs.clear();
                    for (auto& row : ev.record.vs_breakdown) {
                        if (row.opponent_id.empty()) continue;
                        hs.my_vs[row.opponent_id] = {row.wins, row.losses, row.draws};
                    }
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: record W-L-D = %d-%d-%d (vs %u opponents)",
                        hs.my_wins, hs.my_losses, hs.my_draws,
                        (unsigned)hs.my_vs.size());
                    UpdateWindowTitleWithRecord();
                    // Push to the in-game shared mem so the game window
                    // titlebar (and later the overlay) can render the
                    // updated W/L/D without an alt-tab to the launcher.
                    PushStatsToHook();
                }
                break;
            }
            case K::RecentMatchesReceived:
                hs.recent_matches        = ev.recent_matches;
                hs.recent_matches_loaded = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: cached %u recent matches",
                            (unsigned)hs.recent_matches.size());
                break;
            case K::CurrentMatchesReceived:
                hs.current_matches        = ev.current_matches;
                hs.current_matches_loaded = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: cached %u in-progress matches",
                            (unsigned)hs.current_matches.size());
                break;
            case K::MatchInProgressStarted:
            case K::MatchInProgressUpdated: {
                // Replace existing token entry or append. Keeps the list
                // in last-write-wins state without rebuilding from a
                // fresh snapshot for every update.
                const auto& upd = ev.current_match_update;
                bool replaced = false;
                for (auto& m : hs.current_matches) {
                    if (m.token == upd.token) {
                        m = upd;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) hs.current_matches.push_back(upd);
                break;
            }
            case K::MatchInProgressEnded: {
                const std::string& tok = ev.current_match_token;
                hs.current_matches.erase(
                    std::remove_if(hs.current_matches.begin(),
                                   hs.current_matches.end(),
                                   [&](const auto& m) { return m.token == tok; }),
                    hs.current_matches.end());
                break;
            }
            case K::Error:
                hs.status_line = "error: " + ev.error;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: %s", hs.status_line.c_str());
                // Auth-required error: pop the Discord sign-in window so
                // the user knows what to do. Matches "auth_required"
                // reason from hub/hub.py.
                if (ev.error.find("auth_required") != std::string::npos) {
                    show_discord_auth_ = true;
                }
                break;
        }
}

