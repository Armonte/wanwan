// launcher_ui_hub_match.cpp -- LauncherUI match-result reporting + outcome/upload
// polling + stats push, split out of launcher_ui_hub.cpp (pure member-fn move).
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


// Match-results CSV helpers (moved from FM2K_LauncherUI.cpp; AppendResultsCsvRow owns them).
// Local match log -- CCCaster's results.csv with FM2K-specific columns.
// One row per match end, written from MY perspective (the user running
// THIS launcher), so each peer keeps its own view. Format:
//
//   when_iso, game_id, role, my_nick, my_char, peer_nick, peer_char, result
//
// "role" is "P1" or "P2" depending on whether we hosted the match. CCCaster
// always writes one record per match, but only on `_localPlayer == 1`; we
// instead write per-perspective so the file is meaningful even if the user
// switches between hosting and joining.
//
// "result" is the same outcome string we send to the hub: self_won /
// peer_won / draw / disconnect. CCCaster used per-side round counts; we
// only have final winner so we surface that directly.
//
// Lives in the same %APPDATA%\FM2K_Rollback dir as audio.ini / settings.ini.
// First write emits a UTF-8 BOM so Excel renders Japanese / accented names
// correctly when the user double-clicks the file.
static std::string ResultsCsvPath() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\results.csv";
}

// RFC-4180 escape: wrap in quotes if the field contains comma / quote /
// newline, doubling internal quotes. Empty string → empty (unquoted).
static std::string CsvEscape(const std::string& in) {
    bool needs_quote = false;
    for (char c : in) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) return in;
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (char c : in) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

void LauncherUI::AppendResultsCsvRow(const char* outcome_str,
                                     uint32_t p1_char_id, uint32_t p2_char_id,
                                     const std::string& p1_char_name,
                                     const std::string& p2_char_name) {
    if (!hub_state_) return;
    auto& hs = *hub_state_;
    const std::string path = ResultsCsvPath();
    if (path.empty()) return;

    // Detect first-write so we can emit BOM + header. GetFileAttributesA
    // is the cheapest existence check on Win32.
    const bool fresh =
        (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES);

    FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "results.csv: open failed: %s", path.c_str());
        return;
    }
    if (fresh) {
        // UTF-8 BOM (EF BB BF). Excel needs this to interpret the file
        // as UTF-8 instead of guessing the system codepage.
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        std::fwrite(bom, 1, sizeof(bom), f);
        std::fprintf(f,
            "when,game_id,role,my_nick,my_char,peer_nick,peer_char,result\r\n");
    }

    // Map our role to a P1/P2 label: host == P1 by hub convention.
    const bool i_am_p1 = (hs.current_match_role == "host");
    const std::string my_char_name   = i_am_p1 ? p1_char_name : p2_char_name;
    const std::string peer_char_name = i_am_p1 ? p2_char_name : p1_char_name;
    const uint32_t    my_char_id     = i_am_p1 ? p1_char_id   : p2_char_id;
    const uint32_t    peer_char_id   = i_am_p1 ? p2_char_id   : p1_char_id;

    // Prefer the resolved .player filename; fall back to "id N" when the
    // hook didn't manage to resolve (older shared-mem version, char_id
    // out of roster range, etc).
    auto fmt_char = [](const std::string& name, uint32_t id) -> std::string {
        if (!name.empty()) return name;
        if (id == 0xFFFFFFFFu) return "";
        return "id " + std::to_string(id);
    };

    // ISO-8601 timestamp in local time. Sortable + readable; Excel parses
    // this format as a date out of the box.
    char when_iso[32] = "";
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    if (localtime_s(&lt, &now) == 0) {
        std::strftime(when_iso, sizeof(when_iso), "%Y-%m-%dT%H:%M:%S", &lt);
    }

    std::fprintf(f, "%s,%s,%s,%s,%s,%s,%s,%s\r\n",
        CsvEscape(when_iso).c_str(),
        CsvEscape(hs.current_match_game_id).c_str(),
        i_am_p1 ? "P1" : "P2",
        CsvEscape(hs.my_nick).c_str(),
        CsvEscape(fmt_char(my_char_name, my_char_id)).c_str(),
        CsvEscape(hs.current_match_peer_nick).c_str(),
        CsvEscape(fmt_char(peer_char_name, peer_char_id)).c_str(),
        CsvEscape(outcome_str ? outcome_str : "").c_str());

    std::fclose(f);
}

void LauncherUI::UpdateWindowTitleWithRecord() {
    if (!window_ || !hub_state_) return;
    auto& hs = *hub_state_;
    char title[256];
    // Session suffix -- Patrick's bug. Empty until we've finished at
    // least one match this launcher session so the title isn't
    // cluttered with "0-0-0" for everyone who just opened the app.
    char session_buf[48] = {};
    if (hs.session_wins + hs.session_losses + hs.session_draws > 0) {
        std::snprintf(session_buf, sizeof(session_buf),
                      " \xe2\x80\xa2 session %d-%d-%d",
                      hs.session_wins, hs.session_losses, hs.session_draws);
    }
    if (hs.my_wins < 0) {
        // No overall record yet -- bare title or session-only.
        if (session_buf[0]) {
            std::snprintf(title, sizeof(title),
                          "FM2K Rollback Launcher%s", session_buf);
        } else {
            std::snprintf(title, sizeof(title), "FM2K Rollback Launcher");
        }
    } else if (!hs.my_nick.empty()) {
        std::snprintf(title, sizeof(title),
                      "FM2K Rollback Launcher \xe2\x80\x94 %s (%d-%d-%d)%s",
                      hs.my_nick.c_str(), hs.my_wins, hs.my_losses, hs.my_draws,
                      session_buf);
    } else {
        std::snprintf(title, sizeof(title),
                      "FM2K Rollback Launcher \xe2\x80\x94 %d-%d-%d%s",
                      hs.my_wins, hs.my_losses, hs.my_draws, session_buf);
    }
    SDL_SetWindowTitle(window_, title);
}

void LauncherUI::PushStatsToHook() {
    if (!hub_state_ || !on_get_client_status) return;
    auto& hs = *hub_state_;

    uint32_t pid1 = 0, pid2 = 0;
    if (!on_get_client_status(pid1, pid2)) return;

    // Resolve vs-peer record from the cached breakdown (filled by the
    // hub's `record` event with no opponent_id filter -- the same data
    // the lobby's vs column reads).
    int32_t vs_w = -1, vs_l = -1, vs_d = -1;
    if (!hs.current_match_peer_id.empty()) {
        auto it = hs.my_vs.find(hs.current_match_peer_id);
        if (it != hs.my_vs.end()) {
            vs_w = it->second.wins;
            vs_l = it->second.losses;
            vs_d = it->second.draws;
        } else {
            // No prior matches against this peer -- explicit zeros so the
            // titlebar shows "0-0-0" instead of dashes.
            vs_w = vs_l = vs_d = 0;
        }
    }

    auto write = [&](uint32_t pid) {
        if (pid == 0) return;
        const std::string name = "FM2K_SharedMem_" + std::to_string(pid);
        HANDLE h = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE,
                                    FALSE, name.c_str());
        if (!h) return;
        FM2KSharedMemData* data = static_cast<FM2KSharedMemData*>(
            MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                          sizeof(FM2KSharedMemData)));
        if (data && data->magic == FM2K_SHARED_MEM_MAGIC &&
            data->version == FM2K_SHARED_MEM_VERSION)
        {
            data->ui_wins      = hs.my_wins;
            data->ui_losses    = hs.my_losses;
            data->ui_draws     = hs.my_draws;
            data->ui_vs_wins   = vs_w;
            data->ui_vs_losses = vs_l;
            data->ui_vs_draws  = vs_d;
            auto stash = [](char* dst, size_t cap, const std::string& src) {
                if (cap == 0) return;
                size_t n = std::min<size_t>(src.size(), cap - 1);
                if (n) memcpy(dst, src.data(), n);
                dst[n] = '\0';
            };
            stash(data->ui_peer_nick, sizeof(data->ui_peer_nick),
                  hs.current_match_peer_nick);
            stash(data->ui_my_nick,   sizeof(data->ui_my_nick),
                  hs.my_nick);
        }
        if (data) UnmapViewOfFile(data);
        CloseHandle(h);
    };
    write(pid1);
    write(pid2);
}

void LauncherUI::PushHudSystemMessage(const char* text_utf8, uint32_t ttl_ms) {
    if (!on_get_client_status || !text_utf8) return;
    uint32_t pid1 = 0, pid2 = 0;
    if (!on_get_client_status(pid1, pid2)) return;

    // Mirrors PushStatsToHook's open-write-close pattern. Bumps the
    // HUD seq AFTER the buffer + expiry land, so a fc_hud read that
    // races us only ever sees a coherent {message, expiry, seq}
    // snapshot.
    auto write = [&](uint32_t pid) {
        if (pid == 0) return;
        const std::string name = "FM2K_SharedMem_" + std::to_string(pid);
        HANDLE h = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE,
                                    FALSE, name.c_str());
        if (!h) return;
        FM2KSharedMemData* data = static_cast<FM2KSharedMemData*>(
            MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                          sizeof(FM2KSharedMemData)));
        if (data && data->magic == FM2K_SHARED_MEM_MAGIC &&
            data->version == FM2K_SHARED_MEM_VERSION)
        {
            const size_t cap = sizeof(data->hud_system_message);
            const size_t n = std::min<size_t>(std::strlen(text_utf8), cap - 1);
            if (n) std::memcpy(data->hud_system_message, text_utf8, n);
            data->hud_system_message[n] = '\0';
            data->hud_system_message_expiry_tick = GetTickCount() + ttl_ms;
            data->hud_system_message_seq += 1;
        }
        if (data) UnmapViewOfFile(data);
        CloseHandle(h);
    };
    write(pid1);
    write(pid2);
}

void LauncherUI::PollUploadQueue() {
    // Read the static dev-checkbox state. Declared inside Render's local
    // g_auto_upload_logs is a file-scope mirror of the dev checkbox
    // state, kept in sync by Render() (lambda init + Checkbox handler
    // in the developer section).
    if (!g_auto_upload_logs) return;

    // Bail when the build had no secret baked in -- nowhere to upload.
    if (!fm2k::kLogUploadSecret || fm2k::kLogUploadSecret[0] == '\0') return;

    if (games_.empty()) return;

    // Pause uploads while a match is live. Even off the UI thread, draining a
    // backlog of fat rngtrace.csv bundles saturates the user's UPSTREAM
    // bandwidth and competes with the game's live netplay UDP -> ping spikes
    // (Melancholy's 800ms persisted after the thread fix). Telemetry isn't
    // urgent: only drain at the menu, never while Connecting or InGame.
    if (launcher_state_ == LauncherState::Connecting ||
        launcher_state_ == LauncherState::InGame) return;

    // Trickle: at most one upload every ~1.5s so even a large backlog drains
    // gently instead of flooding the connection in a single burst.
    static uint64_t s_next_ms = 0;
    const uint64_t now_ms = (uint64_t)GetTickCount64();
    if (now_ms < s_next_ms) return;

    // Round-robin through ALL installed games, one game per tick, instead
    // of only the UI-selected one. The old code scanned
    // games_[selected_game_index_] only, which meant a crash/desync bundle
    // in (say) pkmncc/upload_queue never uploaded unless the user happened
    // to reopen the launcher AND re-select pkmncc -- and selected_game_index_
    // defaults to -1, so a fresh launcher start drained nothing at all.
    // That stranded the bulk of field reports (match c785d0ca's two peers
    // among them). Rotating one game per frame keeps the per-tick cost
    // identical (still one Process() call) while guaranteeing every game's
    // queue drains regardless of selection.
    // CRITICAL: Process() does a SYNCHRONOUS WinHTTP POST (up to a 15s
    // timeout per upload). This runs every frame from Render(), so doing it
    // on this (the UI/render) thread freezes the launcher for the duration
    // of each upload -- catastrophic once a user has a backlog of stranded
    // bundles (multi-second-to-minute unresponsive launcher, reported on
    // 0.2.65 when the upload secret finally worked and the drain actually
    // fired). Run the blocking drain on a detached worker thread instead, so
    // the render loop never stalls. s_busy keeps exactly one upload in flight
    // at a time (no thread pile-up); over successive frames it round-robins
    // every game's queue and drains backlogs one manifest per completed run.
    static std::atomic<bool> s_busy{false};
    if (s_busy.exchange(true)) return;  // an upload is already running
    s_next_ms = now_ms + 1500;          // >= 1.5s before the next upload starts

    static size_t s_rr = 0;
    if (s_rr >= games_.size()) s_rr = 0;
    const FM2K::FM2KGameInfo& game = games_[s_rr++];

    // Snapshot the game dir on THIS thread (we own games_); pass by value to
    // the worker so it never touches launcher state. fs::path .string() does
    // the wide->UTF-8 round-trip; game dirs are ASCII in practice.
    std::filesystem::path exe = fm2k::utf8path::Utf8ToWide(game.exe_path);
    std::string game_dir = exe.parent_path().string();

    std::thread([game_dir]() {
        fm2k::upload_queue::ProcessorConfig cfg;
        cfg.game_dir   = game_dir;
        cfg.upload_url = fm2k::kLogUploadUrl;   // global constexpr -- thread-safe
        cfg.secret     = fm2k::kLogUploadSecret;
        cfg.enabled    = true;
        // Upload ONE manifest per run, then release the slot. The 1.5s
        // throttle + round-robin in PollUploadQueue pace the rest, so a
        // backlog trickles out over time instead of flooding the connection
        // in one burst (which is what spiked netplay ping even off-thread).
        fm2k::upload_queue::Process(cfg);
        s_busy.store(false);
    }).detach();
}

// Visibility-INDEPENDENT tick, driven from FM2KLauncher::Update().
//
// These two used to live in the render path -- PollMatchOutcome() at the top of
// LauncherUI::Render(), the hub event drain inside RenderHubPanel(). Both are
// skipped when the launcher is minimized or hidden (FM2K_RollbackClient.cpp:
// `if (minimized || hidden) { should_render = false; }`), and the panel drain
// additionally needs "Hub" to be the active dock tab. So playing a whole set
// with the launcher minimized meant: no match_result ever built or sent, no
// results.csv row, nothing queued for replay, AND PushStatsToHook() never ran
// -- which leaves the shared-mem nicks empty, so the in-game HUD falls back to
// literal "P1"/"P2". That single condition produces both halves of the user
// reports (a set recorded as ZERO matches, with a player shown as "P1"), and
// minimizing partway through explains the partial 15-5 -> 12-5 case.
//
// Neither function touches ImGui, which is what makes this safe to call from
// the non-painting tick. Ordering is preserved: Update() runs before Render()
// in the same loop iteration, so the panel still paints freshly-drained state.
void LauncherUI::TickAlways() {
    if (hub_state_) {
        auto& hs = *hub_state_;
        hs.client.Poll([this](const fm2k::HubEvent& ev) { HandleHubEvent(ev); });
    }
    PollMatchOutcome();
}

void LauncherUI::PollMatchOutcome() {
    if (!hub_state_) return;
    auto& hs = *hub_state_;

    // Push the latest stats (peer nick + W-L-D) to the spawned game's
    // shared mem on every poll. The MatchStart-time push fires BEFORE
    // the game process has created its FM2K_SharedMem mapping, so that
    // initial write silently no-ops. Re-pushing here lands the data as
    // soon as the mapping appears (typically within ~1s of game spawn),
    // so the in-game titlebar shows "vs <peer> 0-0-0" on the FIRST
    // match instead of waiting until match #2's K::MatchStart fires.
    // Cheap when no game is running (on_get_client_status returns 0).
    if (!hs.current_match_token.empty()) {
        PushStatsToHook();
    }

    // Nothing more to report if there's no live hub-driven match or
    // the hub dropped. NOTE: we DON'T early-return on match_result_sent
    // anymore -- FM2K rematches stay inside the same hub `in_match`
    // session (no fresh K::MatchStart fires), so the second match's
    // outcome would be permanently swallowed if we gated on a flag
    // that's set true after match #1 ends. Per-PID last_outcome_seq
    // tracking already handles the "have we processed THIS bump"
    // dedup. match_result_sent is now only used by the K::Peer-
    // Disconnected handler to skip a redundant send.
    if (hs.current_match_token.empty()) return;
    // Pre-v0.2.45: a WS disconnect HERE silently dropped the outcome
    // for the rest of the set (long pause → keepalive timeout → WS drop
    // is the classic trigger; multiple users reported wins not counting
    // after long pauses). We now FALL THROUGH on disconnect -- CSV mirror
    // still writes, hub send is queued onto hs.pending_match_results,
    // and the K::Connected handler drains the queue on reconnect.
    const bool ws_connected = hs.client.IsConnected();
    if (!ws_connected) {
        static bool s_warned_disconnect = false;
        if (!s_warned_disconnect) {
            s_warned_disconnect = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PollMatchOutcome: hub WS disconnected -- outcomes will be "
                "queued and replayed on reconnect");
        }
    }
    if (!on_get_client_status) return;

    uint32_t pid1 = 0, pid2 = 0;
    if (!on_get_client_status(pid1, pid2)) return;

    auto try_pid = [&](uint32_t pid) {
        // Don't gate on hs.match_result_sent. Per-PID last_outcome_seq
        // already dedupes (line below), and after match #1 commits,
        // match_result_sent stays true through the rematch -- which
        // means the post-match DISCONNECT outcome (peer closed window
        // mid-CSS) would never get processed → on_session_stop never
        // fires → survivor's game stays open with no opponent.
        if (pid == 0) return;

        const std::string name = "FM2K_SharedMem_" + std::to_string(pid);
        HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
        if (!h) return;

        FM2KSharedMemData* data = static_cast<FM2KSharedMemData*>(
            MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(FM2KSharedMemData)));
        if (!data) {
            CloseHandle(h);
            return;
        }

        if (data->magic == FM2K_SHARED_MEM_MAGIC &&
            data->version == FM2K_SHARED_MEM_VERSION)
        {
            const uint32_t seq        = data->match_outcome_seq;
            const uint8_t  outcome_u8 = data->match_outcome;
            const uint32_t p1_char_id = data->match_p1_char_id;
            const uint32_t p2_char_id = data->match_p2_char_id;
            const uint32_t stage_id   = data->match_stage_id;
            // Resolved .player filenames (UTF-8 already, hook converts
            // from CP932). Bound the strnlen at the shared-mem buffer
            // size so a malformed publish (no NUL) can't walk off the
            // mapping.
            std::string p1_char_name(
                data->match_p1_char_name,
                strnlen(data->match_p1_char_name, FM2K_MATCH_CHAR_NAME_MAX));
            std::string p2_char_name(
                data->match_p2_char_name,
                strnlen(data->match_p2_char_name, FM2K_MATCH_CHAR_NAME_MAX));
            // Prefer KGT-derived display names over the hook's shared-mem
            // values. The hook publishes filenames (from g_char_slot_data
            // / g_player_file_name_array, e.g. "c1.player" → "c1") which
            // for some games (vanpri) are placeholders, while the actual
            // display name lives inside each .player file's header. The
            // launcher's parser already enriches kgt.player_names from
            // those .player headers, so when local KGT resolves to a
            // different (non-filename) name, that's the one to ship.
            if (on_resolve_char_name) {
                if (p1_char_id != 0xFFFFFFFFu) {
                    std::string n = on_resolve_char_name(
                        hs.current_match_game_id, p1_char_id);
                    if (!n.empty()) p1_char_name = std::move(n);
                }
                if (p2_char_id != 0xFFFFFFFFu) {
                    std::string n = on_resolve_char_name(
                        hs.current_match_game_id, p2_char_id);
                    if (!n.empty()) p2_char_name = std::move(n);
                }
            }
            // Stage name resolved from local KGT (FM2K has no in-memory
            // stage filename table; the launcher already parsed the .kgt
            // header at discovery). Empty when game isn't installed
            // locally -- hub will store id-only in that case.
            std::string stage_name;
            if (stage_id != 0xFFFFFFFFu && on_resolve_stage_name) {
                stage_name = on_resolve_stage_name(hs.current_match_game_id,
                                                   stage_id);
            }

            // Fire match_progress to the hub when the hook bumps its
            // chars_seq counter (which it does exactly once per
            // Netplay_StartBattleSession). Gating on seq advance --
            // not on the chars themselves -- sidesteps the rotate-
            // window race where shared mem still holds the prev
            // battle's chars at the time we set current_match_token
            // to the rotated value. During CSS / inter-battle, the
            // counter doesn't advance, no fire happens, and the
            // lobby's empty "(in CSS)" row stays put until the new
            // battle actually starts.
            const uint32_t chars_seq = data->match_chars_seq;
            if (!hs.current_match_token.empty() &&
                chars_seq != 0 &&
                chars_seq != hs.last_chars_seq[pid])
            {
                hs.client.ReportMatchProgress(
                    hs.current_match_token,
                    p1_char_id, p2_char_id,
                    p1_char_name, p2_char_name,
                    stage_id, stage_name);
                hs.last_chars_seq[pid] = chars_seq;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hub: match_progress sent token=%.8s... "
                    "p1=%u(\"%s\") p2=%u(\"%s\") stage=%u(\"%s\") "
                    "chars_seq=%u",
                    hs.current_match_token.c_str(),
                    p1_char_id, p1_char_name.c_str(),
                    p2_char_id, p2_char_name.c_str(),
                    stage_id, stage_name.c_str(),
                    (unsigned)chars_seq);
            }
            const uint32_t last_seen  = hs.last_outcome_seq[pid];

            if (seq > last_seen && outcome_u8 != FM2K_MATCH_OUTCOME_NONE) {
                hs.last_outcome_seq[pid] = seq;

                // CSS_ABORT: peer left during char select before battle
                // started. Close the surviving local game but DON'T
                // record anything -- the match never reached battle, so
                // there's no W/L/D to commit. No CSV row, no hub
                // MatchResult, no in-flight commit.
                if (outcome_u8 == FM2K_MATCH_OUTCOME_CSS_ABORT) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: peer left during CSS -- closing local game (no record)");
                    hs.status_line = "peer left during CSS -- match not recorded";
                    if (on_session_stop) on_session_stop();
                    // Clear so subsequent broadcasts of the same token
                    // (e.g. a match_rotated arriving from the hub) don't
                    // re-trigger this.
                    hs.current_match_token.clear();
                    hs.current_match_peer_id.clear();
                    hs.current_match_peer_nick.clear();
                    UnmapViewOfFile(data);
                    CloseHandle(h);
                    return;
                }

                // Desync: GekkoNet caught state checksum divergence
                // between peers and the hook terminated the game on the
                // first occurrence (prevents the cascading-corruption
                // crash users were hitting at character_state_machine
                // 0x4125FC after thousands of frames of bad sim state).
                // No W/L/D record -- sim diverged, outcome undefined.
                if (outcome_u8 == FM2K_MATCH_OUTCOME_DESYNC) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: DESYNC -- game terminated on first divergence "
                        "(no record). Inspect FM2K_P*_desync_f*.log for "
                        "the per-region diff.");
                    hs.status_line = "desync detected -- match not recorded";
                    FireSystemNotification(
                        "Desync detected",
                        "Game state diverged between peers. Match was "
                        "stopped before sim corruption caused a crash. "
                        "Re-launch to start a fresh session.");
                    if (on_session_stop) on_session_stop();
                    hs.current_match_token.clear();
                    hs.current_match_peer_id.clear();
                    hs.current_match_peer_nick.clear();
                    UnmapViewOfFile(data);
                    CloseHandle(h);
                    return;
                }

                // Hash mismatch: peers' game files diverge. Closes the
                // local session and surfaces a clear toast pointing the
                // user at the hook log so they can identify the
                // offending file. Same no-record treatment as CSS_ABORT --
                // no battle, no W/L/D delta.
                if (outcome_u8 == FM2K_MATCH_OUTCOME_HASH_MISMATCH) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hub: GAME DATA MISMATCH -- see launcher popup for "
                        "the per-file manifest.");
                    hs.status_line = "game files don't match peer";
                    // Read this game's hook log and extract the
                    // "GameHash: manifest" block so the popup can
                    // render the actual filename | size | content-hash
                    // rows that fed the local hash. The user diffs
                    // against the peer's log (which they get over
                    // Discord) to find the offending row.
                    // Player index: 0 for host, 1 for guest. Drives the
                    // hook log filename (FM2K_P1 vs FM2K_P2).
                    const int player_index =
                        network_config_.is_host ? 0 : 1;
                    std::filesystem::path game_exe;
                    if (selected_game_index_ >= 0 &&
                        selected_game_index_ < (int)games_.size()) {
                        game_exe = games_[selected_game_index_].exe_path;
                    }
                    hs.hash_mismatch_log_excerpt =
                        ExtractGameHashManifest(game_exe, player_index);
                    hs.show_hash_mismatch_modal = true;
                    FireSystemNotification(
                        T("toast_hash_mismatch_title"),
                        T("toast_hash_mismatch_body"));
                    if (on_session_stop) on_session_stop();
                    hs.current_match_token.clear();
                    hs.current_match_peer_id.clear();
                    hs.current_match_peer_nick.clear();
                    UnmapViewOfFile(data);
                    CloseHandle(h);
                    return;
                }

                const char* outcome_str = nullptr;
                switch (outcome_u8) {
                    case FM2K_MATCH_OUTCOME_SELF_WON:   outcome_str = "self_won"; break;
                    case FM2K_MATCH_OUTCOME_PEER_WON:   outcome_str = "peer_won"; break;
                    case FM2K_MATCH_OUTCOME_DRAW:       outcome_str = "draw";     break;
                    case FM2K_MATCH_OUTCOME_DISCONNECT: outcome_str = "disconnect"; break;
                    default: break;
                }
                // Session counter (Patrick's bug -- wanted current-session
                // record in the titlebar in addition to the overall). Only
                // committed outcomes count; disconnect doesn't, matching
                // the hub's ledger semantics.
                bool session_changed = false;
                switch (outcome_u8) {
                    case FM2K_MATCH_OUTCOME_SELF_WON: ++hs.session_wins;   session_changed = true; break;
                    case FM2K_MATCH_OUTCOME_PEER_WON: ++hs.session_losses; session_changed = true; break;
                    case FM2K_MATCH_OUTCOME_DRAW:     ++hs.session_draws;  session_changed = true; break;
                    default: break;
                }
                if (session_changed) UpdateWindowTitleWithRecord();
                if (outcome_str) {
                    // Local CSV mirror first -- runs even if the hub
                    // send queues silently because we're disconnected.
                    // Same data, written to %APPDATA%\FM2K_Rollback\
                    // results.csv. CCCaster-equivalent for offline
                    // history.
                    AppendResultsCsvRow(outcome_str,
                                        p1_char_id, p2_char_id,
                                        p1_char_name, p2_char_name);

                    // C10 schema-2: pull session_id + match_index +
                    // rounds[] from shared mem v10. Fields are 0/empty
                    // on legacy hook builds and the schema-2 overload
                    // automatically falls back to schema-1 wire shape
                    // when session_id == 0 && rounds.empty().
                    const uint64_t session_id =
                        data->match_session_id;
                    const uint8_t  match_index =
                        data->match_index_in_session;
                    std::vector<fm2k::HubClient::RoundJson> rounds_json;
                    const uint8_t rc =
                        std::min<uint8_t>(data->match_rounds_count, 8);
                    rounds_json.reserve(rc);
                    for (uint8_t i = 0; i < rc; ++i) {
                        const FM2KRoundResult& r = data->match_rounds[i];
                        rounds_json.push_back(fm2k::HubClient::RoundJson{
                            r.winner_idx,
                            r.p1_hp_remaining,
                            r.p2_hp_remaining,
                            r.frames_elapsed,
                        });
                    }

                    if (ws_connected) {
                        hs.client.MatchResult(hs.current_match_token, outcome_str,
                                              p1_char_id, p2_char_id,
                                              p1_char_name, p2_char_name,
                                              stage_id, stage_name,
                                              session_id, match_index,
                                              rounds_json);
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: match_result sent token=%.8s... outcome=%s "
                            "p1=%u(\"%s\") p2=%u(\"%s\") stage=%u(\"%s\") "
                            "session=0x%016llx match_idx=%u rounds=%u "
                            "pid=%lu seq=%u",
                            hs.current_match_token.c_str(), outcome_str,
                            p1_char_id, p1_char_name.c_str(),
                            p2_char_id, p2_char_name.c_str(),
                            stage_id, stage_name.c_str(),
                            (unsigned long long)session_id,
                            (unsigned)match_index, (unsigned)rc,
                            (unsigned long)pid, (unsigned)seq);
                        // Refresh our cached W/L/D so the UI updates
                        // immediately for the next room render. Also
                        // refresh the recent-matches list so the just-
                        // committed match shows up at the top.
                        hs.client.QueryRecord();
                        hs.client.RequestRecentMatches(50);
                    } else {
                        // WS dropped -- queue everything for K::Connected
                        // to drain. CSV mirror already written above so
                        // the local record is intact even if the hub
                        // stays down forever.
                        HubState::PendingMatchResult q;
                        q.token         = hs.current_match_token;
                        q.outcome       = outcome_str;
                        q.p1_char_id    = p1_char_id;
                        q.p2_char_id    = p2_char_id;
                        q.p1_char_name  = p1_char_name;
                        q.p2_char_name  = p2_char_name;
                        q.stage_id      = stage_id;
                        q.stage_name    = stage_name;
                        q.session_id    = session_id;
                        q.match_index   = match_index;
                        q.rounds        = rounds_json;
                        hs.pending_match_results.push_back(std::move(q));
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: match_result QUEUED (WS disconnected) "
                            "token=%.8s... outcome=%s -- pending=%zu, "
                            "will replay on reconnect",
                            hs.current_match_token.c_str(), outcome_str,
                            hs.pending_match_results.size());
                    }
                    hs.match_result_sent = true;

                    // Peer-disconnect: tear down the surviving local
                    // game so the user isn't stuck staring at a frozen
                    // CSS / battle screen with no opponent. Visible
                    // toast so the user knows WHY the window is
                    // closing instead of treating it as a crash.
                    if (outcome_u8 == FM2K_MATCH_OUTCOME_DISCONNECT) {
                        const std::string& peer_nick =
                            hs.current_match_peer_nick.empty()
                                ? std::string("Opponent")
                                : hs.current_match_peer_nick;
                        char body[160];
                        std::snprintf(body, sizeof(body),
                                      T("toast_peer_disconnected_body"),
                                      peer_nick.c_str());
                        hs.status_line = std::string("peer disconnected -- closing match");
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Hub: peer dropped, stopping local session");
                        if (!hs.disconnect_toast_fired) {
                            hs.disconnect_toast_fired = true;
                            FireSystemNotification(
                                T("toast_peer_disconnected_title"),
                                body);
                            // 5-second in-game centered overlay so the
                            // user sees why their match ended without
                            // alt-tabbing to the launcher's toast.
                            PushHudSystemMessage(
                                T("toast_peer_disconnected_title"), 5000);
                        }
                        if (on_session_stop) on_session_stop();
                    }
                }
            }
        }

        UnmapViewOfFile(data);
        CloseHandle(h);
    };

    try_pid(pid1);
    try_pid(pid2);
}


