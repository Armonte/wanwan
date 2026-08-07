// launcher_ui_hub_panel.cpp -- LauncherUI::RenderHubPanel, the hub/lobby panel
// rendering (STUN/UPnP poll + per-frame event drain + room/user lists + challenge
// modals). Event dispatch lives in launcher_ui_hub_events.cpp; helpers in
// launcher_ui_hub.cpp. Split from launcher_ui_hub.cpp (pure move).
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

void LauncherUI::RenderHubPanel() {
    auto& hs = *hub_state_;

    // Periodic pre-match STUN refresh (every 20 s while connected) so
    // the hub-stored external NAT port stays alive even on quiet
    // lobbies. Cheap -- one UDP packet up, one back, ≤800 ms timeout.
    // Skipped while we're in an active match (the in-game hook owns the
    // socket then) and while the user is mid-challenge (we'd briefly
    // bind/release the port the preflight punch is about to need).
    if (hs.client.IsConnected() && !hs.my_id.empty() &&
        hs.current_match_token.empty()) {
        uint32_t now_ms = static_cast<uint32_t>(SDL_GetTicks());
        if (hs.last_stun_refresh_ms == 0 ||
            (now_ms - hs.last_stun_refresh_ms) > 20000) {
            const char* hub_host_env = std::getenv("FM2K_HUB_HOST");
            std::string hub_host = (hub_host_env && hub_host_env[0])
                                 ? hub_host_env
                                 : (hub_host_[0] ? hub_host_ : "hub.2dfm.org");
            fm2k::NatClassifyResult nat = fm2k::LauncherStunClassify(
                static_cast<uint16_t>(network_config_.local_port),
                hub_host, 7711, hs.my_id);
            hs.last_stun_refresh_ms = now_ms;
            // Re-send nat_type only on a change (it's stable on a given
            // network; "unknown"/"blocked" can flap on a dropped ack, so we
            // still update so the hub reflects the latest reading).
            if (nat.nat_type != hs.last_nat_type) {
                hs.client.SendNatType(nat.nat_type);
                hs.last_nat_type = nat.nat_type;
            }
        }
    }

    // UPnP port-mapper poll (Phase 1). Snapshot the mapper status once per
    // frame and, on a state TRANSITION, act exactly once. The mapper itself
    // is off-thread; this is the cheap UI-thread side. port_mapper_ is only
    // non-null for non-loopback online sessions (set at the Connected
    // event), so this is implicitly skipped for LOCAL / local-dev hubs.
    if (port_mapper_ && hs.client.IsConnected()) {
        fm2k::PortMapper::Status st = port_mapper_->Snapshot();
        const int cur_state = static_cast<int>(st.state);
        if (cur_state != port_mapper_last_state_) {
            port_mapper_last_state_ = cur_state;
            if (st.state == fm2k::PortMapper::State::Mapped &&
                st.ext_udp_port > 0 && !st.ext_ip.empty()) {
                // RE-SEND udp_addr carrying the external endpoint. The hub
                // accepts udp_addr updates at any time (STUN re-sends the
                // same way), applies the D5 precedence (UPnP ext_port
                // outranks STUN-learned), and CGNAT-guards ext_ip against
                // the WS-source IP (D6). We still send "127.0.0.1" as the
                // base ip exactly like the connect-time send -- the hub
                // ignores it for the udp_addr IP (it uses the WS source);
                // ext_ip is the field that carries the WAN address.
                hs.client.SendUdpAddrUpnp(
                    "127.0.0.1",
                    network_config_.local_port,
                    network_config_.local_port,
                    st.ext_ip,
                    static_cast<int>(st.ext_udp_port),
                    /*upnp=*/true,
                    fm2k::LocalLanIp(),
                    fm2k::LocalLanIpV6());
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] mapping live -- re-sent udp_addr ext=%s:%u to hub",
                    st.ext_ip.c_str(),
                    static_cast<unsigned>(st.ext_udp_port));
            } else if (st.state == fm2k::PortMapper::State::Cgnat) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] IGD behind CGNAT/double-NAT -- not advertising a "
                    "UPnP port (punch + relay still cover us)");
            } else if (st.state == fm2k::PortMapper::State::NoIgd) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[upnp] no UPnP-capable router found -- direct reach via "
                    "punch only");
            }
        }
    }

    // NOTE: the hub event drain used to live HERE, inside the panel render.
    // It now runs in LauncherUI::TickAlways(), driven from FM2KLauncher::
    // Update(), because this whole function is skipped when the launcher is
    // minimized (and when "Hub" isn't the active dock tab) -- which silently
    // stopped match reporting for the entire session. See TickAlways().

    // ---- UI ----
    ImGui::SeparatorText(T("hub_section_header"));

    // Nick input -- 128-byte buffer covers 32 visible codepoints even at
    // 4 bytes per UTF-8 char (CJK / emoji). Hub caps incoming nicks to 32
    // codepoints + sanitizes control chars (see hub.py). Local buffer is
    // generous so the input field doesn't truncate mid-character.
    static char s_nick[128] = "";
    static bool s_use_discord_name = true;
    static std::string s_discord_global_name;
    // Pre-fill on first hub-panel render from the persisted auth cache so
    // (a) users who set a custom nick see it again, (b) the "Use Discord
    // name" checkbox tracks their last choice, and (c) we have the
    // authoritative Discord global_name available for when they flip the
    // checkbox back on.
    static bool s_nick_initialized = false;
    if (!s_nick_initialized) {
        const auto cached = fm2k::discord_auth::LoadCached();
        if (!cached.nick.empty()) {
            std::snprintf(s_nick, sizeof(s_nick), "%s", cached.nick.c_str());
        }
        s_use_discord_name    = cached.use_discord_name;
        s_discord_global_name = cached.discord_global_name;
        s_nick_initialized = true;
    }
    // Hub host string lives on the LauncherUI (member hub_host_) and is
    // edited from Settings → Hub Server… The Hub panel is read-only here.
    if (!hub_host_initialized_) {
        hub_host_initialized_ = true;
        const char* env_h = std::getenv("FM2K_HUB_HOST");
        const char* def   = (env_h && env_h[0]) ? env_h : "hub.2dfm.org";
        std::snprintf(hub_host_, sizeof(hub_host_), "%s", def);
    }
    // Delay override panel (#24). Peers exchange a delay candidate over the
    // control channel; a peer on a COMPUTED mode adopts max(local, remote) so
    // two computed peers land on the same delay. A MANUAL pin runs verbatim and
    // purely local -- it is advertised (so a computed opponent can max() it in)
    // but is never itself raised, so two manual peers may run different delays.
    // This combo picks how THIS peer's candidate is sized:
    //   index 0 "computed (avg ping)"  -> FM2K_DELAY_MODE=0, mean RTT
    //   index 1 "computed (peak ping)" -> FM2K_DELAY_MODE=1, worst RTT
    //   index 2..18 manual 0..16       -> FM2K_LOCAL_DELAY=N
    // FM2K_LOCAL_DELAY is cleared for the two computed modes so the hook
    // computes from RTT. A manual pick runs VERBATIM and PURELY LOCAL -- it is
    // never pulled up to the peer's value. Your manual value is still
    // advertised as this peer's candidate, but that only matters to an opponent
    // on a COMPUTED mode (they factor it into their max()); it does NOT force a
    // manual opponent, who runs their own value. Two manual players can run
    // DIFFERENT delays (asymmetric is fine -- inputs are frame-tagged).
    static int s_delay_override = 0;
    {
        // Manual delay range: 0..16. 0 is opt-in for sub-1ms LAN /
        // loopback / hot-seat play -- GekkoNet's prediction-0 mode applies
        // input same frame, but ANY jitter on the link will rollback
        // every frame. Users on actual internet should leave this on
        // computed. 16 = 160 ms, basically the upper limit of playable
        // delay-only netcode.
        const char* delay_items[] = {
            "computed (avg ping)",
            "computed (peak ping)",
            "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",
            "9", "10", "11", "12", "13", "14", "15", "16",
        };
        ImGui::PushItemWidth(-120);
        ImGui::Combo("Delay", &s_delay_override, delay_items,
                     IM_ARRAYSIZE(delay_items));
        ImGui::PopItemWidth();
        ImGui::SetItemTooltip(
            "Input delay (frames at 100 Hz). \"computed (avg ping)\" sizes "
            "delay to mean RTT -- lower delay, but spikes can cause rollbacks. "
            "\"computed (peak ping)\" sizes to the worst RTT seen -- higher "
            "delay, rides out jitter. Two computed peers exchange picks and "
            "adopt the higher, so their delay matches. Pin 0..16 to force a "
            "MANUAL value: yours runs exactly as set and is NEVER raised to the "
            "opponent's -- if they also pin manual, the two sides can differ "
            "(that's fine). 0 = same-frame input, only safe on near-zero-"
            "latency links (LAN / loopback / hotseat) -- any jitter rolls back "
            "every frame. 16 = 160 ms, upper limit of playable delay-only "
            "netcode.");
        if (s_delay_override >= 2) {
            // Manual override: index 2 -> "0", index 3 -> "1", ...
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", s_delay_override - 2);
            ::SetEnvironmentVariableA("FM2K_LOCAL_DELAY", buf);
        } else {
            ::SetEnvironmentVariableA("FM2K_LOCAL_DELAY", nullptr);
        }
        // Computed-delay formula: index 0 = avg ping, index 1 = peak.
        // Harmless when a manual value is pinned (the hook ignores mode
        // once FM2K_LOCAL_DELAY is set).
        ::SetEnvironmentVariableA("FM2K_DELAY_MODE",
                                  s_delay_override == 1 ? "1" : "0");
    }

    // Runahead and prediction window are intentionally NOT exposed here.
    // Neither is a free "pick a number" knob:
    //   - prediction window must be IDENTICAL on both peers (GekkoNet
    //     derives the desync-detection checkpoint frame from it) -- a
    //     mismatch causes false desyncs, so it stays fixed at 16.
    //   - runahead's correct value is exactly the input delay; the hook
    //     auto-tracks the negotiated local_delay.
    // FM2K_PREDICTION_WINDOW / FM2K_RUNAHEAD env vars still force-pin
    // either value for dev bisecting (set on BOTH peers for prediction).

    // Stealth / ghost mode (persisted to dev_flags.ini). When on, the hub keeps
    // your match + characters out of the lobby and public stats -- for testing
    // unreleased builds without leaking them. Rendered in BOTH connect states so
    // it can be toggled LIVE: SetStealth() rides the next hello while
    // disconnected, and sends a live "set_stealth" update while connected (hub
    // flips us in/out of the lobby immediately, no reconnect).
    {
        static bool s_stealth_loaded = false;
        if (!s_stealth_loaded) {
            s_stealth_loaded = true;
            hs.stealth = LoadDevFlagInt("stealth_mode", 0) != 0;
            hs.client.SetStealth(hs.stealth);
        }
    }
    if (ImGui::Checkbox("Stealth mode (hide my match + characters from the lobby)", &hs.stealth)) {
        SaveDevFlagInt("stealth_mode", hs.stealth ? 1 : 0);
        hs.client.SetStealth(hs.stealth);  // rides hello if disconnected; live update if connected
    }

    if (!hs.client.IsConnected()) {
        // "Use Discord name" checkbox -- when checked, the nick input is
        // grayed and shows the user's Discord global_name (read-only).
        // When unchecked, the user can edit their custom nick. Toggling
        // doesn't destroy the custom nick -- it just switches WHICH value
        // gets sent on Connect. Persists immediately.
        if (ImGui::Checkbox(T("hub_use_discord_name"), &s_use_discord_name)) {
            auto cached_save = fm2k::discord_auth::LoadCached();
            if (cached_save.valid) {
                cached_save.use_discord_name = s_use_discord_name;
                fm2k::discord_auth::SaveCached(cached_save);
            }
        }
        // Display buffer: shows what'll be sent on Connect. When the
        // checkbox is on, that's discord_global_name (read-only). When
        // off, it's the editable custom nick (s_nick). Two separate
        // buffers under the hood so flipping the checkbox doesn't
        // clobber either source-of-truth.
        char display_buf[128];
        if (s_use_discord_name) {
            std::snprintf(display_buf, sizeof(display_buf), "%s",
                          s_discord_global_name.c_str());
        } else {
            std::snprintf(display_buf, sizeof(display_buf), "%s", s_nick);
        }
        ImGui::PushItemWidth(-120);
        if (s_use_discord_name) ImGui::BeginDisabled();
        if (ImGui::InputText(T("hub_nick"), display_buf, sizeof(display_buf))
            && !s_use_discord_name) {
            std::snprintf(s_nick, sizeof(s_nick), "%s", display_buf);
        }
        if (s_use_discord_name) ImGui::EndDisabled();
        ImGui::PopItemWidth();
        // Show the configured hub host as read-only context. Edit from
        // Settings → Hub Server…
        ImGui::TextDisabled(T("hub_server"), hub_host_[0] ? hub_host_ : "hub.2dfm.org");
        ImGui::SameLine();
        if (ImGui::SmallButton("change")) {
            show_hub_server_ = true;
        }
        const auto cached_auth_check = fm2k::discord_auth::LoadCached();
        // Adopt the Discord name as soon as the auth cache has one.
        //
        // s_discord_global_name is snapshotted once, on the FIRST hub-panel
        // render, and s_nick_initialized latches. On first run the sign-in
        // window auto-opens and this panel renders BEFORE OAuth completes, so
        // that snapshot is empty -- and stays empty for the rest of the
        // process even though the pairing worker has since written the real
        // name into discord_auth.json. With "Use Discord name" checked that
        // pinned Connect to "(set a nick first)" until the user restarted the
        // launcher or unchecked the box and typed a nick by hand ("can't play
        // online without a manually set nickname, even if 'use discord name'
        // is checked"). Fill only when empty, so this never stomps a name the
        // user has already got.
        if (s_discord_global_name.empty() &&
            !cached_auth_check.discord_global_name.empty()) {
            s_discord_global_name = cached_auth_check.discord_global_name;
        }
        // nick_ok: when "Use Discord name" is on, validity depends on whether
        // we know what their Discord name actually is (populated post-OAuth).
        // When off, just whether they typed something.
        const bool nick_ok = s_use_discord_name
            ? !s_discord_global_name.empty()
            : (s_nick[0] != '\0');
        const bool signed_in  = cached_auth_check.valid;
        const bool can_connect = nick_ok && signed_in;
        const char* button_label =
            !signed_in ? "(sign in with Discord first)" :
            !nick_ok   ? "(set a nick first)" : "Connect";
        if (!can_connect) ImGui::BeginDisabled();
        if (ImGui::Button(button_label, ImVec2(-1, 0))) {
            // Pick the right nick to send: Discord global_name when the
            // checkbox is on, custom nick otherwise. Custom nick still
            // persists across the connect (so toggling the checkbox back
            // off restores the user's last custom value).
            const std::string outgoing_nick =
                s_use_discord_name ? s_discord_global_name : std::string(s_nick);
            hs.my_nick = outgoing_nick;
            // Persist nick + checkbox state to discord_auth.json.
            auto cached_save = fm2k::discord_auth::LoadCached();
            if (cached_save.valid) {
                bool dirty = false;
                if (cached_save.nick != s_nick) {
                    cached_save.nick = s_nick;
                    dirty = true;
                }
                if (cached_save.use_discord_name != s_use_discord_name) {
                    cached_save.use_discord_name = s_use_discord_name;
                    dirty = true;
                }
                if (dirty) fm2k::discord_auth::SaveCached(cached_save);
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
            const std::string hub_host = (hub_host_[0] != '\0') ? hub_host_ : "hub.2dfm.org";
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
            // v0.2.8 routes WSS through Caddy on 443 by default; legacy
            // 2dfm.sytes.net hosts (set by users on older configs) keep
            // the direct-WS-on-7711 path so cutover is transparent.
            const bool use_legacy = (hub_host.find("sytes.net") != std::string::npos);
            const uint16_t      ws_port = use_legacy ? 7711  : 443;
            const char*         ws_path = use_legacy ? "/"   : "/ws";
            const bool          ws_tls  = !use_legacy;
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
        if (!can_connect) ImGui::EndDisabled();
    } else {
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f),
                           "Connected as %s", hs.my_nick.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(T("hub_disconnect"))) {
            hs.client.Disconnect();
        }
        // One-line UPnP port status (Phase 1). Reflects the off-thread
        // mapper's current state so the user can see whether they're
        // directly reachable. Only shown when we actually started a mapper
        // (non-loopback online session).
        if (port_mapper_) {
            fm2k::PortMapper::Status st = port_mapper_->Snapshot();
            switch (st.state) {
                case fm2k::PortMapper::State::Mapped:
                    ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f),
                        "Port: open via UPnP (ext %u)",
                        static_cast<unsigned>(st.ext_udp_port));
                    break;
                case fm2k::PortMapper::State::Discovering:
                    ImGui::TextDisabled("Port: checking router (UPnP)...");
                    break;
                case fm2k::PortMapper::State::Cgnat:
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                        "Port: router behind CGNAT (UPnP can't help)");
                    break;
                case fm2k::PortMapper::State::NoIgd:
                    ImGui::TextDisabled("Port: closed (no UPnP router)");
                    break;
                case fm2k::PortMapper::State::Failed:
                    ImGui::TextDisabled("Port: closed (UPnP unavailable)");
                    break;
                case fm2k::PortMapper::State::Idle:
                default:
                    break;
            }
        }
    }
    if (!hs.status_line.empty()) {
        ImGui::TextDisabled("%s", hs.status_line.c_str());
    }
    if (!hs.client.IsConnected()) return;

    // ---- Rooms ----
    ImGui::SeparatorText(T("hub_rooms_header"));
    if (hs.rooms.empty()) {
        ImGui::TextDisabled("%s", T("hub_no_rooms"));
    }
    if (ImGui::BeginTable("##rooms", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(T("col_game"));
        ImGui::TableSetupColumn(T("col_players"),  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(T("col_installed"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("",                 ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        // Sort by player count descending so the busiest rooms surface
        // first. Stable secondary sort by room name (alpha) so empty/quiet
        // rooms have a deterministic order between renders. Sort a copy so
        // we don't mutate hs.rooms (which the hub broadcast handler also
        // touches asynchronously -- sorting in-place would race).
        std::vector<fm2k::HubRoom> sorted_rooms = hs.rooms;
        std::sort(sorted_rooms.begin(), sorted_rooms.end(),
            [](const fm2k::HubRoom& a, const fm2k::HubRoom& b) {
                if (a.user_count != b.user_count) return a.user_count > b.user_count;
                return a.name < b.name;
            });
        for (auto& r : sorted_rooms) {
            int installed_idx = FindInstalledGameForRoom(games_, r.id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", r.user_count);
            ImGui::TableSetColumnIndex(2);
            if (installed_idx >= 0) {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "%s", T("label_yes"));
            } else {
                ImGui::TextColored(ImVec4(0.85f, 0.5f, 0.4f, 1.0f), "%s", T("label_no"));
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(r.id.c_str());
            if (r.id == hs.current_room_id) {
                ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1.0f), "%s", T("label_joined"));
            } else if (ImGui::SmallButton(T("btn_join"))) {
                hs.client.JoinRoom(r.id, r.name);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Quick-create room from the selected game (until a master list ships).
    bool game_selected = selected_game_index_ >= 0 &&
                         selected_game_index_ < (int)games_.size();
    if (game_selected && hs.current_room_id.empty()) {
        ImGui::Spacing();
        const auto& g = games_[selected_game_index_];
        // Use exe path stem as the room/game id so two clients with the
        // same exe land in the same room. Master list will replace this
        // with a stable canonical id.
        std::filesystem::path exe(fm2k::utf8path::Utf8ToWide(g.exe_path));
        std::string game_id = fm2k::utf8path::StemUtf8(exe);
        std::string label = "Join room for: " + game_id;
        if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) {
            hs.client.JoinRoom(game_id, game_id);
        }
    }

    // ---- Active Matches ----
    // Walk the user list, group in_match pairs (each pair appears once,
    // owned by the user with the lexicographically smaller id so we don't
    // double-render). Click "Spectate" to ask the hub for the host's UDP
    // addr; on grant a local FM2K spectator instance launches pointing at
    // it and joins the host's GekkoSpectateSession via SpectatorNode JOIN_REQ.
    if (!hs.users.empty()) {
        std::vector<std::pair<const fm2k::HubUser*, const fm2k::HubUser*>> active_pairs;
        for (auto& [uid, u] : hs.users) {
            if (u.status != "in_match") continue;
            if (u.opponent_id.empty())  continue;
            if (uid >= u.opponent_id)   continue;  // dedupe -- only the lower-id half
            auto it = hs.users.find(u.opponent_id);
            if (it == hs.users.end())   continue;
            if (it->second.status != "in_match") continue;
            active_pairs.emplace_back(&u, &it->second);
        }

        ImGui::SeparatorText(T("hub_active_matches_header"));
        if (active_pairs.empty()) {
            ImGui::TextDisabled("%s", T("hub_no_active_matches"));
        } else if (ImGui::BeginTable("##active_matches", 3,
                       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn(T("col_match"));
            ImGui::TableSetupColumn(T("col_room"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("",            ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableHeadersRow();

            for (auto& [a, b] : active_pairs) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(T("hub_active_match"), a->nick.c_str(), b->nick.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(a->room_id.empty() ? T("label_dash") : a->room_id.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::PushID(a->id.c_str());
                // Spectate via the hub -- RequestSpectate asks the hub to
                // grant us this match's host UDP addr; on grant we get a
                // K::SpectateGranted event which dispatches into
                // on_spectate_match (FM2K_RollbackClient.cpp) and ends up
                // in LaunchRemoteSpectator with default mode="current"
                // (CURRENT_MATCH snapshot-join path). The bf=8000+
                // input-replay drift that gated this button before is
                // sidestepped now: snapshot-join skips replay entirely
                // and consumes only post-anchor INPUTs. Tooltip kept
                // ambient -- see docs/dev/spectator_smoke_test.md for the
                // observable checklist.
                if (ImGui::SmallButton(T("btn_spectate"))) {
                    if (hs.client.IsConnected()) {
                        // a->id is the user we're requesting to spectate;
                        // hub maps it to their current match and replies
                        // with the host's NAT-traversed UDP addr.
                        hs.client.RequestSpectate(a->id);
                        hs.status_line = "spectate request sent: " + a->nick;
                    }
                }
                ImGui::SetItemTooltip("Watch this match (FULL_SESSION -- "
                                      "replays from session start; snapshot-join "
                                      "still baking)");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ---- Live in-progress matches across the hub ----
    RenderInProgressMatchesBody();

    // ---- Users in current room ----
    // Build a localized "Players in <room>" header with snprintf so the
    // translation string can position the room name wherever the language
    // wants it (English: "Players in %s", JP: "%s のプレイヤー").
    char players_header[160];
    if (hs.current_room_id.empty()) {
        std::snprintf(players_header, sizeof(players_header), "%s",
                      T("hub_players_header"));
    } else {
        std::snprintf(players_header, sizeof(players_header),
                      T("hub_players_in_room"), hs.current_room_id.c_str());
    }
    ImGui::SeparatorText(players_header);
    if (hs.current_room_id.empty()) {
        ImGui::TextDisabled("%s", T("hub_no_room_selected"));
    } else if (hs.users.empty()) {
        ImGui::TextDisabled("%s", T("hub_room_empty"));
    } else {
        if (ImGui::BeginTable("##users", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn(T("col_nick"));
            ImGui::TableSetupColumn(T("col_status"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn(T("col_ping"),   ImGuiTableColumnFlags_WidthFixed, 60.0f);
            // "vs" column -- my W-L-D against this opponent, "--" if we've
            // never played them. Self-row leaves it blank.
            ImGui::TableSetupColumn(T("col_vs"),     ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("",              ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableHeadersRow();

            // Tier → color mapping. Tester ($5) gets blue (0x2C7BDB,
            // matching Patreon's hub branding); Special Thanks ($10) gets
            // gold (0xFFBF03); monte (operator) gets red (0xE53935, Material
            // red 600 -- distinct from gold without being garish); guest
            // (open-access non-patron, when the hub gate is lifted) gets grey
            // (0x9E9E9E, Material grey 500) -- clearly not a paying tier.
            // Anything else (legacy hub, missing field) renders in the default
            // text color so stale clients don't turn invisible.
            const ImVec4 kTierTester(0x2C / 255.0f, 0x7B / 255.0f, 0xDB / 255.0f, 1.0f);
            const ImVec4 kTierThanks(0xFF / 255.0f, 0xBF / 255.0f, 0x03 / 255.0f, 1.0f);
            const ImVec4 kTierMonte (0xE5 / 255.0f, 0x39 / 255.0f, 0x35 / 255.0f, 1.0f);
            const ImVec4 kTierGuest (0x9E / 255.0f, 0x9E / 255.0f, 0x9E / 255.0f, 1.0f);
            for (auto& [uid, u] : hs.users) {
                // Self is shown in the list (top row, naturally -- most
                // hubs put your row at the top so you can see your own
                // tier color + status without scrolling). The Challenge
                // button is hidden for your own row below since
                // self-challenges are nonsensical.
                const bool is_self = (uid == hs.my_id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (u.tier == "monte") {
                    ImGui::TextColored(kTierMonte, "%s", u.nick.c_str());
                } else if (u.tier == "thanks") {
                    ImGui::TextColored(kTierThanks, "%s", u.nick.c_str());
                } else if (u.tier == "tester") {
                    ImGui::TextColored(kTierTester, "%s", u.nick.c_str());
                } else if (u.tier == "guest") {
                    ImGui::TextColored(kTierGuest, "%s", u.nick.c_str());
                } else {
                    ImGui::TextUnformatted(u.nick.c_str());
                }

                ImGui::TableSetColumnIndex(1);
                ImVec4 c(0.6f, 0.6f, 0.6f, 1.0f);
                // Localize status label too. The protocol value (u.status)
                // stays untranslated -- that's an internal protocol token,
                // not user-facing text. Map it to a translation key.
                const char* status_label = u.status.c_str();
                if (u.status == "idle")             { c = ImVec4(0.3f, 0.9f, 0.4f, 1.0f); status_label = T("status_idle"); }
                else if (u.status == "in_match")    { c = ImVec4(0.95f, 0.7f, 0.2f, 1.0f); status_label = T("status_in_match"); }
                else if (u.status == "challenging") { c = ImVec4(0.6f, 0.7f, 1.0f, 1.0f); status_label = T("status_challenging"); }
                ImGui::TextColored(c, "%s", status_label);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%dms", u.rtt_ms);

                ImGui::TableSetColumnIndex(3);
                if (is_self) {
                    // Self row: show overall W/L/D + a per-session
                    // counter that resets on launcher restart (Patrick
                    // asked for current-session record visibility).
                    // The "vs me" cell wouldn't make sense for self.
                    if (hs.my_wins >= 0) {
                        ImGui::Text("%d-%d-%d", hs.my_wins, hs.my_losses, hs.my_draws);
                    } else {
                        ImGui::TextDisabled("--");
                    }
                    if (hs.session_wins + hs.session_losses + hs.session_draws > 0) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(+%d-%d-%d)",
                                            hs.session_wins,
                                            hs.session_losses,
                                            hs.session_draws);
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Current launcher-session "
                                              "wins-losses-draws (resets "
                                              "on launcher restart)");
                        }
                    }
                } else {
                    auto it = hs.my_vs.find(uid);
                    if (it != hs.my_vs.end()) {
                        ImGui::Text("%d-%d-%d",
                                    it->second.wins, it->second.losses, it->second.draws);
                    } else {
                        ImGui::TextDisabled("--");
                    }
                }

                ImGui::TableSetColumnIndex(4);
                ImGui::PushID(uid.c_str());
                // Self-row shows nothing in the action column -- challenging
                // yourself isn't a thing. Other rows get the Challenge button
                // gated on idle status.
                if (!is_self) {
                    bool can_challenge = (u.status == "idle");
                    if (!can_challenge) ImGui::BeginDisabled();
                    if (ImGui::SmallButton(T("btn_challenge"))) {
                        // Build the host's resolved [GamePlay] config
                        // for THIS challenge so the target sees the
                        // round count / time / stage / etc. in their
                        // accept modal (#54). Anti-cheat clamps land
                        // launcher-side before the wire encode so the
                        // target can't see un-clamped values.
                        fm2k::MatchSettings ms;
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
                        hs.client.Challenge(uid, ms);
                        hs.outgoing_challenge_to_id   = uid;
                        hs.outgoing_challenge_to_nick = u.nick;
                        hs.show_outgoing_challenge_modal = true;
                        hs.status_line = "challenged " + u.nick + " -- waiting for response";
                    }
                    if (!can_challenge) ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ---- Incoming-challenge modal ----
    // Stable `##incoming_challenge` popup ID so a language switch mid-popup
    // doesn't break ImGui's hashed identity (see RenderConnectionStatus).
    if (hs.show_challenge_modal) {
        ImGui::OpenPopup("##incoming_challenge");
        hs.show_challenge_modal = false;
    }
    if (ImGui::BeginPopupModal("##incoming_challenge", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(T("modal_incoming_challenge_body"), hs.pending_challenge_from_nick.c_str());
        ImGui::Spacing();

        // Match settings preview (#54). Only render if the challenger
        // actually sent any -- older clients leave the whole struct at
        // -1 and we want to keep the modal compact in that case.
        const auto& s = hs.pending_challenge_settings;
        const bool any_set =
            s.player0_cpu != -1 || s.player1_cpu != -1 ||
            s.game_speed  != -1 || s.hit_judge   != -1 ||
            s.game_information != -1 || s.stage_nb != -1 ||
            s.joystick != -1 || s.time != -1 ||
            s.exit_flag != -1 || s.vs_mode != -1 ||
            s.vs_single_play != -1 || s.vs_team_play != -1;
        if (any_set) {
            ImGui::SeparatorText(T("label_match_settings"));
            if (ImGui::BeginTable("##match_settings_preview", 2,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                auto row = [](const char* label, int v) {
                    if (v == -1) return;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", v);
                };
                row("Round count (1v1)",   s.vs_single_play);
                row("Round count (team)",  s.vs_team_play);
                row("Round timer (s)",     s.time);
                row("Game speed",          s.game_speed);
                row("Stage",               s.stage_nb);
                row("Joystick",            s.joystick);
                row("VS mode",             s.vs_mode);
                ImGui::EndTable();
            }
            ImGui::Spacing();
        }

        if (ImGui::Button(T("btn_accept"), ImVec2(120, 0))) {
            hs.client.AcceptChallenge(hs.pending_challenge_from_id);
            hs.pending_challenge_from_id.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("btn_decline"), ImVec2(120, 0))) {
            hs.client.DeclineChallenge(hs.pending_challenge_from_id);
            hs.pending_challenge_from_id.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Outgoing-challenge modal ----
    // Renders for the challenger after they click Challenge so they
    // get visible feedback that the request went out. The hub-event
    // handler clears show_outgoing_challenge_modal on any terminal
    // outcome (declined / failed / cancelled / match_start).
    if (hs.show_outgoing_challenge_modal) {
        ImGui::OpenPopup("##outgoing_challenge");
    }
    if (ImGui::BeginPopupModal("##outgoing_challenge", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text(T("modal_outgoing_challenge_body"),
                    hs.outgoing_challenge_to_nick.empty()
                        ? "opponent"
                        : hs.outgoing_challenge_to_nick.c_str());
        // Animated pip so the user sees the modal is alive (ImGui's
        // own spinner widget doesn't exist; a string of dots cycling
        // is enough for the "we're waiting" signal).
        const int dots = (int)(ImGui::GetTime() * 2.0) % 4;
        char dot_str[5] = {0};
        for (int i = 0; i < dots; ++i) dot_str[i] = '.';
        ImGui::SameLine();
        ImGui::TextDisabled("%s", dot_str);
        ImGui::Spacing();
        if (ImGui::Button(T("btn_cancel"), ImVec2(120, 0))) {
            hs.client.CancelChallenge(hs.outgoing_challenge_to_id);
            hs.show_outgoing_challenge_modal = false;
            hs.outgoing_challenge_to_id.clear();
            hs.outgoing_challenge_to_nick.clear();
            hs.status_line = "challenge cancelled";
            ImGui::CloseCurrentPopup();
        }
        // Auto-close popup if the event handler dropped the flag (e.g.
        // we got match_start). BeginPopupModal returns true only while
        // the popup is open, so just stop reopening it: the next frame
        // sees show_outgoing_challenge_modal=false and skips OpenPopup.
        if (!hs.show_outgoing_challenge_modal) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Hash-mismatch popup ----
    // Fires when the hook publishes FM2K_MATCH_OUTCOME_HASH_MISMATCH.
    // Shows the local game's "GameHash: manifest" excerpt so the user
    // can diff against the peer's log to find the offending file.
    if (hs.show_hash_mismatch_modal) {
        ImGui::OpenPopup("##hash_mismatch");
    }
    if (ImGui::BeginPopupModal("##hash_mismatch", nullptr,
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("Game data mismatch -- match cancelled.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Your .kgt / .player roster differs from your peer's. "
            "Below is what we hashed locally. Send this to your peer (or "
            "exchange hook logs) -- the row with a different size or "
            "content_hash is the file that needs to match. Read the hook "
            "log for more details.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Local manifest (filename | size | content_hash):");
        // Scrolling read-only text region. Wide on purpose so the
        // hash columns line up.
        ImGui::InputTextMultiline(
            "##hash_mm_log",
            hs.hash_mismatch_log_excerpt.data(),
            hs.hash_mismatch_log_excerpt.size() + 1,
            ImVec2(720, 280),
            ImGuiInputTextFlags_ReadOnly);
        ImGui::Spacing();
        if (ImGui::Button("Copy to clipboard", ImVec2(160, 0))) {
            ImGui::SetClipboardText(hs.hash_mismatch_log_excerpt.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            hs.show_hash_mismatch_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Recent matches (collapsing) ----
    // Lives at the bottom of the Hub panel -- it's session data, not a
    // configuration setting, so it doesn't belong in the Settings tabs.
    // Collapsed by default; users who care about history click to
    // expand. The body renderer is shared with the legacy floating
    // window so any styling fixes apply to both.
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(T("menu_recent_matches"))) {
        RenderRecentMatchesBody();
    }
}


