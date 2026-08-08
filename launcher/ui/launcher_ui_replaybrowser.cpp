// launcher_ui_replaybrowser.cpp -- the C11 replay browser: scan, classify,
// group, render. Split out of launcher_ui_gameselect.cpp.
//
// WHAT CHANGED AND WHY (2026-08, the "everything is legacy" report)
// ----------------------------------------------------------------
// The old classifier was one line: `session_id == 0` printed a synthetic
// "Legacy (no session id)" header. That is not a format epoch and never was a
// compatibility warning -- session_id was minted host-only and never delivered
// to the guest, so from the moment BOTH players started writing replays
// (8c1cc65, stable v0.2.82) every file the JOINING player recorded landed in
// that bucket, on today's build, forever. Meanwhile genuinely old v1-header
// files were not labelled legacy at all: the scan hard-filtered them out and
// they simply vanished from the list.
//
// The hook side now delivers session_id to the guest (HOST_CONFIG) and stamps
// a producer_version into the file header, so this file can classify honestly:
//   * "can this build play it"      -> file_version (v1 rows are listed
//                                      DISABLED, never silently dropped);
//   * "was it recorded by a build   -> producer_version presence/threshold,
//      that carries the match's        which is the only thing that actually
//      sim settings"                   predicts a replay desync;
//   * "were these files recorded    -> game_content_tag vs the tag of the
//      against this install"           install on disk right now -> refuse.
// session_id is now used for exactly what it is: a grouping key.
#include "FM2K_Integration.h"
#include "launcher_ui_internal.h"  // shared persistence helpers (namespace lui)
#include "FM2K_Locale.h"
#include "FM2K_Utf8Path.h"
#include "FM2KHook/src/netplay/content_tag.h"
#include <shellapi.h>              // ShellExecuteW -- "open file location"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "vendored/imgui/imgui.h"

using namespace lui;

namespace {

// FM2KSessionFileHeader offsets (spec_session_file.cpp). Read by offset rather
// than by casting the hook's struct so a hook-side schema bump can never
// silently reinterpret launcher-side memory. tests/
// test_spectator_protocol_sessionfile.cpp pins every one of these.
constexpr uint32_t MAGIC_FMSS   = 0x53534D46;  // 'FMSS' little-endian
constexpr uint16_t VERSION_V2   = 2;
constexpr size_t   HEADER_SIZE  = 256;
constexpr size_t   OFF_PRODUCER_VER   = 180;
constexpr size_t   OFF_PRODUCER_VER_S = 184;
constexpr size_t   OFF_CONTENT_TAG    = 200;
constexpr size_t   OFF_GAME_SPEED     = 204;
constexpr size_t   OFF_SOCD_PLUS1     = 208;

// The build that started carrying the match's round gameconfig in MATCH_START
// (884ef58, shipped in 0.2.83). A replay recorded before it re-runs CSS from
// the VIEWER's game.ini round timer, so it plays bit-identically until the
// time-over branch fires at a different frame -- the "desyncs after a few
// minutes" report. Every file that carries a producer_version at all is
// >= this by construction, so today the threshold only ever fires via the
// producer_version == 0 arm; it exists so the next sim-settings epoch has a
// place to say so instead of adding a second mechanism.
constexpr uint32_t VER_ROUND_CFG = (0u << 16) | (2u << 8) | 83u;

std::string Utf8Of(const std::filesystem::path& p) {
    return fm2k::utf8path::WideToUtf8(p.wstring());
}

std::string FormatUnix(uint64_t t) {
    if (t == 0) return "?";
    time_t tt = static_cast<time_t>(t);
    std::tm lt = {};
    localtime_s(&lt, &tt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
    return buf;
}

std::string FormatUnixDay(uint64_t t) {
    if (t == 0) return "?";
    time_t tt = static_cast<time_t>(t);
    std::tm lt = {};
    localtime_s(&lt, &tt);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &lt);
    return buf;
}

// Last-write time as unix seconds, via Win32 rather than the
// file_time_type -> system_clock chrono dance: this is the only date an
// old-format row has, and GetFileAttributesExW is unambiguous on every
// libstdc++/MinGW combination. 0 when unavailable (renders as "?").
uint64_t FileMTimeUnix(const std::filesystem::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &fad)) {
        return 0;
    }
    ULARGE_INTEGER u;
    u.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
    u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    // FILETIME is 100ns ticks since 1601-01-01; 11644473600 s to the epoch.
    if (u.QuadPart < 116444736000000000ULL) return 0;
    return (uint64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

// <game_dir>/replays/<file>.fm2krep and <game_dir>/sessions/<file>.fm2kset --
// both are exactly two levels under the game folder (netplay_battle_end.cpp
// creates them relative to the game process CWD).
std::filesystem::path GameDirOf(const std::filesystem::path& replay) {
    return replay.parent_path().parent_path();
}

}  // namespace

bool LauncherUI::ReplayContentMismatch(const ReplayMeta& m) {
    return m.game_content_tag != 0 && m.local_content_tag != 0 &&
           m.game_content_tag != m.local_content_tag;
}

void LauncherUI::ScanReplays() {
    replays_cache_.clear();
    replays_cache_dirty_ = false;

    // One content-tag computation per distinct game folder, not per file: a
    // session folder holds dozens of .fm2krep for the same install and the tag
    // is a directory enumeration.
    std::unordered_map<std::wstring, uint32_t> tag_by_dir;

    std::error_code ec;
    auto try_load_file = [&](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return;
        uint8_t buf[HEADER_SIZE] = {};
        f.read(reinterpret_cast<char*>(buf), HEADER_SIZE);
        const std::streamsize got = f.gcount();
        // A v1 file is only 32 header bytes + body and can legitimately be
        // shorter than 256 in total, so the old "must read 256" gate dropped
        // some of them before the version check even ran.
        if (got < 8) return;

        uint32_t magic;   std::memcpy(&magic,   buf + 0, 4);
        uint16_t version; std::memcpy(&version, buf + 4, 2);
        uint16_t flags;   std::memcpy(&flags,   buf + 6, 2);
        if (magic != MAGIC_FMSS) return;

        ReplayMeta m{};
        m.path         = Utf8Of(p);
        m.file_version = version;
        m.playable     = (version == VERSION_V2);

        if (!m.playable || got < (std::streamsize)HEADER_SIZE) {
            // Old/unknown format: the hook's loader hard-refuses anything with
            // version != 2 (spec_session_file.cpp), so there is nothing
            // truthful to say about its contents. List it anyway, disabled,
            // dated by the filesystem -- an unreadable row a user can still
            // find and copy beats a file that vanished.
            m.playable = false;
            m.finished_at_unix = FileMTimeUnix(p);
            m.started_at_unix  = m.finished_at_unix;
            replays_cache_.push_back(std::move(m));
            return;
        }

        m.is_battle_slice = (flags & 0x0001) != 0;
        std::memcpy(&m.started_at_unix,  buf + 8,  8);
        std::memcpy(&m.finished_at_unix, buf + 16, 8);
        std::memcpy(&m.event_count,      buf + 24, 4);
        std::memcpy(&m.input_count,      buf + 28, 4);
        std::memcpy(m.game_id,           buf + 32, 32);
        std::memcpy(m.p1_nick,           buf + 64, 32);
        std::memcpy(m.p2_nick,           buf + 96, 32);
        // The three name fields are fixed-width and the writer only guarantees
        // a NUL because it strncpy's to size-1; a truncated or hand-edited file
        // can present 32 non-NUL bytes, and these get read as C strings (row
        // text, group keys). Terminate defensively.
        m.game_id[sizeof(m.game_id) - 1] = '\0';
        m.p1_nick[sizeof(m.p1_nick) - 1] = '\0';
        m.p2_nick[sizeof(m.p2_nick) - 1] = '\0';
        m.p1_char_id    = buf[128];
        m.p2_char_id    = buf[129];
        // colors at 130/131 -- not displayed in the tree
        m.rounds_won_p1 = buf[132];
        m.rounds_won_p2 = buf[133];
        m.match_count   = buf[134];
        m.match_index   = buf[135];
        std::memcpy(&m.session_id,       buf + 136, 8);
        m.round_count   = buf[144];
        std::memcpy(&m.producer_version, buf + OFF_PRODUCER_VER,   4);
        std::memcpy(m.producer_version_s, buf + OFF_PRODUCER_VER_S, 16);
        m.producer_version_s[sizeof(m.producer_version_s) - 1] = '\0';
        std::memcpy(&m.game_content_tag, buf + OFF_CONTENT_TAG,    4);
        std::memcpy(&m.game_speed_pct,   buf + OFF_GAME_SPEED,     4);
        m.socd_mode_plus1 = buf[OFF_SOCD_PLUS1];

        // Compare against what is installed NOW, so a re-patched game is
        // caught before it garbles instead of after. Both the file set and the
        // exe-naming rule come from content_tag.cpp -- the same code the hook
        // ran when it stamped the header -- so the two sides cannot disagree
        // about WHICH files count and produce a false [different game files].
        // No game exe resolvable => ResolveGameExeName is empty => ComputeForDir
        // returns 0 => unknown => the gate stays open.
        if (m.game_content_tag != 0) {
            const std::filesystem::path gdir = GameDirOf(p);
            const std::wstring key = gdir.wstring();
            auto it = tag_by_dir.find(key);
            if (it == tag_by_dir.end()) {
                const std::wstring exe_name =
                    fm2k::content_tag::ResolveGameExeName(key.c_str());
                const uint32_t tag = fm2k::content_tag::ComputeForDir(
                    key.c_str(), exe_name.c_str());
                it = tag_by_dir.emplace(key, tag).first;
            }
            m.local_content_tag = it->second;
        }
        replays_cache_.push_back(std::move(m));
    };

    for (const auto& root : games_root_paths_) {
        if (root.empty()) continue;
        std::filesystem::path root_fs = std::filesystem::u8path(root);
        if (!std::filesystem::is_directory(root_fs, ec)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "ReplayBrowser: skipping root '%s' -- not a directory (ec=%s)",
                root.c_str(), ec ? ec.message().c_str() : "ok");
            continue;
        }
        // Each game lives directly under root; replays/ is one level deeper.
        // Use recursive_directory_iterator with a depth cap so we don't walk
        // user-installed game subdirs unnecessarily.
        size_t hits_before = replays_cache_.size();
        size_t walked      = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root_fs,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec);
             it != std::filesystem::recursive_directory_iterator{};
             it.increment(ec))
        {
            if (ec) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ReplayBrowser: walk error under '%s': %s",
                    root.c_str(), ec.message().c_str());
                break;
            }
            if (it.depth() > 5) { it.disable_recursion_pending(); continue; }
            const auto& entry = *it;
            ++walked;
            if (!entry.is_regular_file(ec)) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (ext == ".fm2krep" || ext == ".fm2kset") {
                try_load_file(entry.path());
            }
        }
        const size_t found = replays_cache_.size() - hits_before;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "ReplayBrowser: root '%s' walked %zu entries, accepted %zu replay file(s)",
            root.c_str(), walked, found);
    }

    // Sort: newest finished first.
    std::sort(replays_cache_.begin(), replays_cache_.end(),
        [](const ReplayMeta& a, const ReplayMeta& b) {
            return a.finished_at_unix > b.finished_at_unix;
        });
    size_t n_old = 0, n_pre_prov = 0, n_mismatch = 0;
    for (const auto& r : replays_cache_) {
        if (!r.playable) ++n_old;
        else if (r.producer_version == 0) ++n_pre_prov;
        if (ReplayContentMismatch(r)) ++n_mismatch;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "ReplayBrowser: scanned %zu replay file(s) across %zu games root(s) "
        "-- %zu old-format, %zu pre-provenance, %zu content-mismatched",
        replays_cache_.size(), games_root_paths_.size(),
        n_old, n_pre_prov, n_mismatch);
}

void LauncherUI::RenderReplayBrowser() {
    if (replays_cache_dirty_) ScanReplays();

    if (ImGui::Button(T("btn_refresh"))) {
        replays_cache_dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(T("replay_order_hint"), replays_cache_.size());

    // Show the configured games-root paths so the user can verify what's
    // being scanned. Common gotcha: launcher in C:\games but games on D:\,
    // and the games-root config still points at the legacy C:\ path --
    // recursive walk silently scans nothing relevant. Surfacing the list
    // means the user can spot it immediately instead of debugging blind.
    if (games_root_paths_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s",
                           T("replay_no_roots"));
    } else {
        ImGui::TextDisabled("%s", T("replay_scanned_roots"));
        for (const auto& root : games_root_paths_) {
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", root.c_str());
        }
    }
    ImGui::Separator();

    if (replays_cache_.empty()) {
        ImGui::TextWrapped("%s", T("replay_none_found"));
        return;
    }

    // GROUPING. A session_id groups the .fm2krep slices of one connection with
    // their .fm2kset -- that is all it has ever meant. Files without one (every
    // guest recording from before the HOST_CONFIG delivery fix, plus old-format
    // rows) are NOT one undifferentiated bucket any more: they group by what
    // they do carry -- game, both nicks and the calendar day -- so a collection
    // of 200 joined matches reads as a list of opponents-and-days instead of a
    // single 200-row node with the newest recording at the bottom.
    struct SessionGroup {
        bool                has_session_id;
        uint64_t            session_id;
        std::vector<size_t> indices;  // into replays_cache_
        std::string         p1_nick, p2_nick, game_id;
        uint64_t            latest_finished;
    };
    std::vector<SessionGroup> groups;
    {
        std::unordered_map<uint64_t, size_t>    group_by_sid;
        std::unordered_map<std::string, size_t> group_by_key;
        for (size_t i = 0; i < replays_cache_.size(); ++i) {
            const auto& r = replays_cache_[i];
            const bool has_sid = r.playable && r.session_id != 0;

            size_t gi;
            if (has_sid) {
                auto [it, inserted] =
                    group_by_sid.try_emplace(r.session_id, groups.size());
                if (inserted) groups.push_back({});
                gi = it->second;
            } else {
                // Synthetic key. The game folder name backs up an empty
                // game_id so old-format rows (which carry no parsed fields at
                // all) still land next to their siblings instead of merging
                // every unreadable file in the collection into one node.
                std::string folder = Utf8Of(
                    GameDirOf(std::filesystem::u8path(r.path)).filename());
                std::string key = (r.game_id[0] ? std::string(r.game_id) : folder);
                key += '\x1f'; key += r.p1_nick;
                key += '\x1f'; key += r.p2_nick;
                key += '\x1f'; key += FormatUnixDay(r.finished_at_unix);
                auto [it, inserted] = group_by_key.try_emplace(key, groups.size());
                if (inserted) groups.push_back({});
                gi = it->second;
            }

            auto& g = groups[gi];
            if (g.indices.empty()) {
                g.has_session_id  = has_sid;
                g.session_id      = has_sid ? r.session_id : 0;
                g.p1_nick         = r.p1_nick;
                g.p2_nick         = r.p2_nick;
                g.game_id         = r.game_id[0]
                    ? std::string(r.game_id)
                    : Utf8Of(GameDirOf(std::filesystem::u8path(r.path)).filename());
                g.latest_finished = r.finished_at_unix;
            }
            g.indices.push_back(i);
            g.latest_finished = std::max(g.latest_finished, r.finished_at_unix);
        }
        std::sort(groups.begin(), groups.end(),
            [](const SessionGroup& a, const SessionGroup& b) {
                return a.latest_finished > b.latest_finished;
            });
        for (auto& g : groups) {
            std::sort(g.indices.begin(), g.indices.end(),
                [&](size_t i, size_t j) {
                    const auto& a = replays_cache_[i];
                    const auto& b = replays_cache_[j];
                    if (g.has_session_id) {
                        // A real session IS a set: the whole-set file first,
                        // then its matches in PLAY order (#61 -- Melan's study
                        // flow depends on 1..N, and match_index is finally a
                        // real counter now that the writer stamps it).
                        if (a.is_battle_slice != b.is_battle_slice)
                            return !a.is_battle_slice;
                        if (a.match_index != b.match_index)
                            return a.match_index < b.match_index;
                        return a.finished_at_unix < b.finished_at_unix;
                    }
                    // A synthetic group is not a set -- there is no play order
                    // to preserve, so the newest recording goes on top, which
                    // is what "I just played that, where is it" needs.
                    return a.finished_at_unix > b.finished_at_unix;
                });
        }
    }

    for (const auto& g : groups) {
        char hdr[320];
        const char* p1 = g.p1_nick[0] ? g.p1_nick.c_str() : "?";
        const char* p2 = g.p2_nick[0] ? g.p2_nick.c_str() : "?";
        const char* gid = g.game_id[0] ? g.game_id.c_str() : "?";
        if (g.has_session_id) {
            std::snprintf(hdr, sizeof(hdr),
                "%s vs %s -- %s -- %s -- %zu %s###sid_%016llx",
                p1, p2, FormatUnix(g.latest_finished).c_str(), gid,
                g.indices.size(), T("replay_label_matches"),
                (unsigned long long)g.session_id);
        } else if (!g.p1_nick.empty() || !g.p2_nick.empty() || !g.game_id.empty()) {
            std::snprintf(hdr, sizeof(hdr),
                "%s vs %s -- %s -- %s -- %zu %s###leg_%p",
                p1, p2, FormatUnix(g.latest_finished).c_str(), gid,
                g.indices.size(), T("replay_label_files"), (void*)&g);
        } else {
            std::snprintf(hdr, sizeof(hdr), "%s -- %zu %s###leg_%p",
                T("replay_group_ungrouped"), g.indices.size(),
                T("replay_label_files"), (void*)&g);
        }

        if (!ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen)) continue;

        // Set-playback support (#63). Only a REAL session can offer it: the
        // set has to belong to the same connection as the match being seeked
        // into, and grouping by session_id is what guarantees that. The old
        // "first non-slice anywhere in the group" pick meant every row in the
        // legacy bucket offered "watch set from here" pointing at the OLDEST
        // unrelated .fm2kset in the collection, seeked to match 1.
        const ReplayMeta* set_file = nullptr;
        if (g.has_session_id) {
            for (size_t idx : g.indices) {
                const auto& c = replays_cache_[idx];
                if (!c.is_battle_slice && c.playable) { set_file = &c; break; }
            }
        }

        for (size_t idx : g.indices) {
            const auto& r = replays_cache_[idx];
            const bool mismatch = ReplayContentMismatch(r);
            char row[512];
            if (!r.playable) {
                std::snprintf(row, sizeof(row), "%s -- v%u -- %s",
                    T("replay_row_old_format"), (unsigned)r.file_version,
                    FormatUnix(r.finished_at_unix).c_str());
            } else if (r.is_battle_slice) {
                std::snprintf(row, sizeof(row),
                    "%s %u -- char %u vs %u -- %u-%u -- %u INPUTs -- %s",
                    T("replay_label_match"), (unsigned)r.match_index,
                    (unsigned)r.p1_char_id, (unsigned)r.p2_char_id,
                    (unsigned)r.rounds_won_p1, (unsigned)r.rounds_won_p2,
                    (unsigned)r.input_count,
                    FormatUnix(r.finished_at_unix).c_str());
            } else {
                std::snprintf(row, sizeof(row), "%s -- %u %s -- %u INPUTs -- %s",
                    T("replay_label_whole_set"), (unsigned)r.match_count,
                    r.match_count == 1 ? T("replay_label_match")
                                       : T("replay_label_matches"),
                    (unsigned)r.input_count,
                    FormatUnix(r.finished_at_unix).c_str());
            }

            ImGui::PushID(static_cast<int>(idx));
            ImGui::Bullet();
            if (r.playable) ImGui::TextUnformatted(row);
            else            ImGui::TextDisabled("%s", row);

            // Findability (#61 / "I can't find my games to share them"): the
            // row now names its own file, and right-click reaches it. The path
            // was already in ReplayMeta and was only ever used to launch.
            const std::string fname =
                Utf8Of(std::filesystem::u8path(r.path).filename());
            // Sample hover BEFORE anything pushes a window: BeginTooltip /
            // BeginPopup replace ImGui's "last item" state, so a tooltip block
            // placed first would make the context menu bind to the tooltip's
            // contents instead of this row.
            const bool row_hovered = ImGui::IsItemHovered();
            if (ImGui::BeginPopupContextItem("##replay_row_ctx")) {
                if (ImGui::MenuItem(T("replay_ctx_open_location"))) {
                    std::wstring arg = L"/select,\"";
                    arg += fm2k::utf8path::Utf8ToWide(r.path);
                    arg += L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer", arg.c_str(),
                                  nullptr, SW_SHOWNORMAL);
                }
                if (ImGui::MenuItem(T("replay_ctx_copy_path"))) {
                    ImGui::SetClipboardText(r.path.c_str());
                }
                ImGui::EndPopup();
            }
            if (row_hovered) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(r.path.c_str());
                if (r.playable) {
                    ImGui::TextDisabled("%s: %s", T("replay_tip_recorded_by"),
                        r.producer_version_s[0] ? r.producer_version_s
                                                : T("replay_build_unknown"));
                    if (r.game_speed_pct) {
                        ImGui::TextDisabled("%s: %u%%",
                            T("replay_tip_game_speed"), (unsigned)r.game_speed_pct);
                    }
                    if (r.socd_mode_plus1) {
                        ImGui::TextDisabled("%s: %u", T("replay_tip_socd"),
                            (unsigned)r.socd_mode_plus1 - 1u);
                    }
                }
                ImGui::EndTooltip();
            }

            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", fname.c_str());

            // The badge that actually predicts a desync, replacing the label
            // that never did. A file with no producer_version predates the
            // provenance stamp, which also means it predates the header
            // carrying the match's round timer / game speed / SOCD -- playback
            // re-runs those from the VIEWER's game.ini and diverges when they
            // differ. Nothing to do with which side recorded it.
            if (r.playable && (r.producer_version == 0 ||
                               r.producer_version < VER_ROUND_CFG)) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s",
                                   T("replay_badge_may_desync"));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(T("replay_badge_may_desync_tip"),
                        r.producer_version_s[0] ? r.producer_version_s
                                                : T("replay_build_unknown"));
                }
            }
            if (mismatch) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s",
                                   T("replay_badge_content_mismatch"));
                if (ImGui::IsItemHovered()) {
                    // "%s" wrapper: this string takes no arguments, so passing
                    // it AS the format would make a stray % in a translation a
                    // crash rather than a typo.
                    ImGui::SetTooltip("%s", T("replay_badge_content_mismatch_tip"));
                }
            }

            // Playback gates. Refusing here (and again in on_replay_play) is
            // the honest alternative to launching a run that garbles from the
            // first frame with no message anywhere.
            const bool blocked = !r.playable || mismatch;
            ImGui::SameLine();
            if (blocked) ImGui::BeginDisabled();
            if (ImGui::SmallButton(T("replay_btn_watch"))) {
                // Set-or-clear: a prior "From here" click must not leak
                // its seek into an unrelated watch (same discipline as
                // the relay-cred envs).
                ::SetEnvironmentVariableA("FM2K_REPLAY_SEEK_MATCH", nullptr);
                if (on_replay_play) on_replay_play(r.path);
            }
            if (blocked) ImGui::EndDisabled();

            if (!blocked && r.is_battle_slice && set_file && r.match_index > 0 &&
                !ReplayContentMismatch(*set_file)) {
                ImGui::SameLine();
                if (ImGui::SmallButton(T("replay_btn_watch_set_here"))) {
                    char n[8];
                    std::snprintf(n, sizeof(n), "%u", (unsigned)r.match_index);
                    ::SetEnvironmentVariableA("FM2K_REPLAY_SEEK_MATCH", n);
                    if (on_replay_play) on_replay_play(set_file->path);
                }
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}
