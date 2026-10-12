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
#include "FM2K_OnlineLaunch.h"
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
                    // The punch, the same-box loopback fallback and every
                    // FM2K_HUB_* / FM2K_PEER_* variable the hook reads live
                    // in launcher/session/FM2K_OnlineLaunch.cpp, shared with
                    // the Slint launcher.
                    hs.status_line = "preflight: punching peer (best-effort)...";
                    const auto plan = fm2k::online_launch::ProbePeer(
                        static_cast<uint16_t>(network_config_.local_port), ev);
                    hs.status_line = "match starting...";

                    // Make sure the launcher's selected_game_ record is
                    // up to date even if RoomJoined fired before games
                    // discovery completed.
                    if (on_game_selected) on_game_selected(games_[idx]);

                    if (!socd_state_loaded_) {
                        socd_state_loaded_ = true;
                        LoadSocdState();
                    }
                    const int local_slot = (ev.match.role == "host") ? 0 : 1;
                    fm2k::online_launch::ExportMatchEnv(ev, hs.my_id,
                                                        socd_mode_[local_slot]);

                    NetworkConfig cfg = network_config_;
                    cfg.session_mode = SessionMode::ONLINE;
                    cfg.is_host = (ev.match.role == "host");
                    cfg.remote_address =
                        plan.peer_ip + ":" + std::to_string(plan.peer_port);

                    on_online_session_start(cfg);
                } else if (idx < 0 && scanning_games_ &&
                           ev.match.peer_udp_port > 0 &&
                           on_online_session_start != nullptr) {
                    // NOT a missing game -- a race with our own scanner.
                    // games_ is published in ONE shot when background
                    // discovery finishes (launcher_frame.cpp's
                    // g_event_discovery_complete -> SetGames), so while a
                    // scan is in flight it is empty and EVERY room misses.
                    // The games.cache normally hides this by filling games_
                    // at startup; with no cache (first run after install, a
                    // cache miss, a big or slow library) a challenge that
                    // lands mid-scan was aborted as "not in your library"
                    // for a game we were actively finding -- measured at
                    // 1.07s after the exe was logged and 0.60s before it
                    // was published.
                    //
                    // Hold the event and let discovery finish. Only this
                    // one precondition is deferrable: a zero peer port or a
                    // missing callback will not improve by waiting, so both
                    // still abort immediately below.
                    pending_match_start_ = std::make_unique<fm2k::HubEvent>(ev);
                    pending_match_start_ms_ = SDL_GetTicks();
                    hs.status_line = "match_start held: still scanning for "
                                     "games...";
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: match_start HELD -- '%s' not in games_ yet but "
                        "discovery is still scanning; retrying when it "
                        "publishes (match stays live)",
                        hs.current_room_id.c_str());
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

// Discovery published games_; re-run the match_start we parked. Called from
// the discovery-complete handler AFTER SetGames + SetScanning(false), so the
// retry sees a populated list and a cleared scanning_games_ -- a genuine
// miss then takes the normal abort path instead of parking again forever.
void LauncherUI::RetryPendingMatchStart() {
    if (!pending_match_start_) return;
    // Move out FIRST: HandleMatchStartEvent may park a new event, and we
    // must not clobber that or re-enter with our own.
    std::unique_ptr<fm2k::HubEvent> ev = std::move(pending_match_start_);
    pending_match_start_.reset();

    const uint64_t waited = SDL_GetTicks() - pending_match_start_ms_;
    // Staleness bound. A scan slow enough to blow this means both peers
    // gave up long ago; spawning a game into a dead match is worse than a
    // clean refusal, and the hub needs the match_ended either way or the
    // lobby shows them "in_match" forever.
    constexpr uint64_t kMaxHoldMs = 15000;
    if (waited > kMaxHoldMs) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Hub: held match_start EXPIRED after %llums (limit %llums) -- "
            "sending match_ended", (unsigned long long)waited,
            (unsigned long long)kMaxHoldMs);
        if (hub_state_) {
            hub_state_->status_line = "match aborted: discovery too slow";
            hub_state_->client.MatchEnded();
        }
        return;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Hub: discovery published after %llums -- retrying held match_start",
        (unsigned long long)waited);
    HandleMatchStartEvent(*ev);
}
