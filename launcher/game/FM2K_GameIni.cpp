#include "FM2K_GameIni.h"

#include <windows.h>
#include <SDL3/SDL_log.h>
#include <chrono>         // the orphaned-temp sweep's age floor
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>   // std::errc, for the concurrent-backup carve-out
#include <vector>

namespace fm2k::game_ini {

namespace {

// All [GamePlay] keys we recognise. Order is preserved on Save so a
// diff-friendly file lands on disk every time. Each entry pairs the
// canonical key string with a pointer-to-member into GamePlayConfig.
struct KeyMap {
    const char* key;
    int GamePlayConfig::*member;
};
const KeyMap kKeyMap[] = {
    {"Editor.TestPlay.Player0.cpu",     &GamePlayConfig::player0_cpu},
    {"Editor.TestPlay.Player1.cpu",     &GamePlayConfig::player1_cpu},
    {"Editor.TestPlay.GameSpeed",       &GamePlayConfig::game_speed},
    {"Editor.TestPlay.HitJudge",        &GamePlayConfig::hit_judge},
    {"Editor.TestPlay.GameInformation", &GamePlayConfig::game_information},
    {"Editor.TestPlay.StageNb",         &GamePlayConfig::stage_nb},
    {"Editor.TestPlay.JoyStick",        &GamePlayConfig::joystick},
    {"Editor.TestPlay.time",            &GamePlayConfig::time},
    {"Editor.TestPlay.exit",            &GamePlayConfig::exit_flag},
    {"Editor.TestPlay.VSMode",          &GamePlayConfig::vs_mode},
    {"Editor.TestPlay.VSSinglePlay",    &GamePlayConfig::vs_single_play},
    {"Editor.TestPlay.VSTeamPlay",      &GamePlayConfig::vs_team_play},
    {"GameScreenMode",                  &GamePlayConfig::game_screen_mode},
};

// Trim whitespace + CR/LF in place. Returns the new length.
size_t TrimInPlace(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t')) {
        s.pop_back();
    }
    size_t lead = 0;
    while (lead < s.size() && (s[lead] == ' ' || s[lead] == '\t')) ++lead;
    if (lead) s.erase(0, lead);
    return s.size();
}

// True if `line` is a section header. `out_name` filled with the inner
// text (between the brackets) on a hit.
bool ParseSection(const std::string& line, std::string& out_name) {
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') return false;
    out_name.assign(line.begin() + 1, line.end() - 1);
    return true;
}

// Split "key=value" with a single = delimiter (anything past the first
// = stays on the value side, including embedded =). Returns false if
// there's no =.
bool SplitKeyValue(const std::string& line, std::string& key, std::string& value) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    key.assign(line, 0, eq);
    value.assign(line, eq + 1, std::string::npos);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(0, 1);
    }
    return true;
}

}  // namespace

std::filesystem::path PathForExe(const std::filesystem::path& exe_path) {
    std::error_code ec;
    auto dir = exe_path.parent_path();
    auto a = dir / "game.ini";
    if (std::filesystem::exists(a, ec)) return a;
    auto b = dir / "2dfm.ini";
    if (std::filesystem::exists(b, ec)) return b;
    // Doesn't exist yet -- return the canonical name so callers can
    // create it. Most FM2K games use game.ini.
    return a;
}

bool Load(const std::filesystem::path& ini_path, GamePlayConfig& out) {
    std::error_code ec;
    if (!std::filesystem::exists(ini_path, ec)) return false;

    std::ifstream f(ini_path);
    if (!f) return false;

    std::string line;
    std::string section;
    while (std::getline(f, line)) {
        TrimInPlace(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        std::string sec;
        if (ParseSection(line, sec)) { section = sec; continue; }
        if (section != "GamePlay") continue;
        std::string key, value;
        if (!SplitKeyValue(line, key, value)) continue;
        for (const auto& m : kKeyMap) {
            if (key == m.key) {
                char* endp = nullptr;
                long parsed = std::strtol(value.c_str(), &endp, 10);
                if (endp && endp != value.c_str()) {
                    out.*m.member = (int)parsed;
                }
                break;
            }
        }
    }
    return true;
}

bool Save(const std::filesystem::path& ini_path, const GamePlayConfig& cfg) {
    // Pass 1: read existing file into memory, preserving every section
    // and every line outside of [GamePlay]. Inside [GamePlay] we strip
    // the keys we manage (they're rewritten from `cfg`) but keep any
    // unknown key the FM2K editor / game might have added in a future
    // version.
    std::vector<std::string> kept;
    bool had_gameplay_section = false;
    int  gameplay_insert_at = -1;  // line index where we'll splice our keys

    {
        std::ifstream in(ini_path);
        std::string line;
        std::string section;
        while (in && std::getline(in, line)) {
            std::string trimmed = line;
            TrimInPlace(trimmed);
            std::string sec;
            if (ParseSection(trimmed, sec)) {
                if (sec == "GamePlay") {
                    had_gameplay_section = true;
                    kept.push_back("[GamePlay]");
                    gameplay_insert_at = (int)kept.size();
                    section = sec;
                    continue;
                }
                section = sec;
                kept.push_back(line);
                continue;
            }
            if (section == "GamePlay") {
                // Drop lines that match our managed keys; preserve
                // anything else (comments, future keys we don't know).
                if (!trimmed.empty() && trimmed[0] != '#' && trimmed[0] != ';') {
                    std::string key, value;
                    if (SplitKeyValue(trimmed, key, value)) {
                        bool managed = false;
                        for (const auto& m : kKeyMap) {
                            if (key == m.key) { managed = true; break; }
                        }
                        if (managed) continue;
                    }
                }
                kept.push_back(line);
                continue;
            }
            kept.push_back(line);
        }
    }

    if (!had_gameplay_section) {
        // No [GamePlay] yet -- append. Prepend a blank line for spacing.
        if (!kept.empty() && !kept.back().empty()) kept.push_back("");
        kept.push_back("[GamePlay]");
        gameplay_insert_at = (int)kept.size();
    }

    // Pass 2: produce the [GamePlay] body in canonical key order.
    std::vector<std::string> body;
    body.reserve(sizeof(kKeyMap) / sizeof(kKeyMap[0]));
    for (const auto& m : kKeyMap) {
        const int v = cfg.*m.member;
        if (v == kUnset) continue;  // skip -- leave the key out of the file
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s=%d", m.key, v);
        body.emplace_back(buf);
    }

    // Splice body at gameplay_insert_at.
    if (gameplay_insert_at < 0) gameplay_insert_at = (int)kept.size();
    kept.insert(kept.begin() + gameplay_insert_at,
                body.begin(), body.end());

    // Atomic write: <path>.<pid>.tmp then rename. Avoids a half-truncated
    // game.ini if the launcher crashes mid-save.
    //
    // THE PID IS LOAD-BEARING, not decoration. The temp name used to be a fixed
    // "<path>.tmp", shared by every launcher process pointed at the same
    // install. Two of them saving at once (the hub-brokered E2E harness runs
    // host + guest + spectator against one game directory, and two people on one
    // PC do the same) both open that one file, and whichever renames FIRST
    // deletes the other's source -- so the loser's rename fails, Save returns
    // false, and ApplyForLaunch reports a failure that has nothing to do with
    // permissions. Measured 2026-08-15 (Wave 2 V4/V6): with the caller finally
    // honouring the return value, that race refused a legitimate online launch.
    // A per-process temp name removes the collision instead of papering over it
    // with a retry.
    std::filesystem::path tmp = ini_path;
    {
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), ".%lu.tmp",
                      (unsigned long)GetCurrentProcessId());
        tmp += suffix;
    }
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GameIni: failed to open %ls for write",
                        tmp.c_str());
            return false;
        }
        for (const auto& l : kept) {
            out.write(l.data(), (std::streamsize)l.size());
            out.put('\n');
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, ini_path, ec);
    if (ec) {
        // Some Windows AVs hold the destination open momentarily on
        // overwrite; fall back to remove + rename.
        std::filesystem::remove(ini_path, ec);
        std::filesystem::rename(tmp, ini_path, ec);
        if (ec) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GameIni: rename failed: %s", ec.message().c_str());
            std::error_code rm;
            std::filesystem::remove(tmp, rm);   // never leave a per-pid turd
            return false;
        }
    }
    return true;
}

std::filesystem::path OverridePathForExe(const std::filesystem::path& exe_path) {
    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !*appdata) return {};
    std::filesystem::path dir =
        std::filesystem::path(appdata) / "FM2K_Rollback" / "game_configs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // Use wstring() then UTF-8 round-trip via Win32 to avoid MinGW's
    // ANSI-codepage mangling of JP filenames in path::string(). The
    // override file goes on disk; we want consistent naming across
    // peers and across stdlib builds for the same game.
    std::wstring stem_w = exe_path.stem().wstring();
    std::string  stem;
    if (!stem_w.empty()) {
        int n = WideCharToMultiByte(CP_UTF8, 0, stem_w.data(), (int)stem_w.size(),
                                    nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            stem.assign((size_t)n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, stem_w.data(), (int)stem_w.size(),
                                stem.data(), n, nullptr, nullptr);
        }
    }
    if (stem.empty()) stem = "default";
    return dir / std::filesystem::path(
        std::wstring(stem_w.empty() ? L"default.ini"
                                    : (stem_w + L".ini")));
}

bool LoadOverride(const std::filesystem::path& exe_path, GamePlayConfig& out) {
    const auto p = OverridePathForExe(exe_path);
    if (p.empty()) return false;
    return Load(p, out);
}

bool SaveOverride(const std::filesystem::path& exe_path,
                  const GamePlayConfig& cfg) {
    const auto p = OverridePathForExe(exe_path);
    if (p.empty()) return false;
    if (!cfg.any_set()) {
        // No fields set → drop the override file entirely. "Reset to
        // defaults" is just SaveOverride(empty).
        std::error_code ec;
        std::filesystem::remove(p, ec);
        return !ec || ec == std::errc::no_such_file_or_directory;
    }
    return Save(p, cfg);
}

bool LoadResolved(const std::filesystem::path& exe_path,
                  GamePlayConfig& out,
                  GamePlayConfig* defaults_out) {
    GamePlayConfig defaults;
    Load(PathForExe(exe_path), defaults);
    if (defaults_out) *defaults_out = defaults;
    GamePlayConfig override_;
    LoadOverride(exe_path, override_);

    // Layer: start with defaults, then override-set fields win.
    out = defaults;
    for (const auto& m : kKeyMap) {
        if (override_.*m.member != kUnset) out.*m.member = override_.*m.member;
    }
    return true;
}

namespace {

std::filesystem::path BackupPathFor(const std::filesystem::path& ini_path) {
    auto p = ini_path;
    p += ".fm2krollback_bak";
    return p;
}

// Sweep ORPHANED per-pid save temps next to `ini_path` (item B3a).
//
// THE RESIDUAL THIS CLEANS UP. Save() writes "<ini>.<pid>.tmp" and renames;
// the only removal sits inside the rename-FAILURE branch, so a crash or a kill
// between the ofstream closing and the rename leaves the temp behind. The old
// fixed "<ini>.tmp" name was self-overwriting and therefore capped at one file;
// the per-pid name (which fixed a worse bug -- two launchers racing on one
// shared temp, one of them silently losing its rename) removed that cap, so
// orphans can now accumulate without bound.
//
// WHY IT IS NOT A ONE-LINER. Deleting "<ini>.*.tmp" unconditionally at startup
// can delete ANOTHER LIVE LAUNCHER's in-flight temp -- which is the exact
// multi-instance case the per-pid name exists for (the hub-brokered E2E
// harness runs three launchers against one game directory). So each candidate
// is deleted only when its owner is provably gone:
//   1. the middle field must parse as a pid (anything else is not ours),
//   2. never our own pid,
//   3. never a pid that still has a live process (OpenProcess +
//      GetExitCodeProcess == STILL_ACTIVE means a launcher owns it),
//   4. never a file modified in the last 60 seconds -- the age floor is the
//      guard against the one way step 3 can be wrong, pid REUSE: a recycled
//      pid makes a dead owner look alive (safe: we skip, and the next launch
//      retries) or, in the other direction, a live owner's brand-new temp can
//      only be missed by the liveness test if the pid was recycled between the
//      write and this check, which the floor covers.
// One summary line, never one line per file.
//
// SCOPE LIMIT, deliberate: the PRE-per-pid name (`<ini>.tmp`, no middle field)
// is NOT swept. It carries no pid, so its owner's liveness cannot be tested,
// and a launcher old enough to write it is exactly the process that might still
// own it. It was also self-overwriting, so at most one can exist per install.
void SweepOrphanedSaveTemps(const std::filesystem::path& ini_path) {
    std::error_code ec;
    const auto dir = ini_path.parent_path();
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return;
    const std::wstring prefix = ini_path.filename().wstring() + L".";
    const std::wstring suffix = L".tmp";
    const unsigned long self = GetCurrentProcessId();
    const auto now = std::filesystem::file_time_type::clock::now();

    int removed = 0, kept_live = 0, kept_young = 0;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        const std::wstring name = it->path().filename().wstring();
        if (name.size() <= prefix.size() + suffix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        // Middle field must be all digits, or this is not one of our temps.
        const std::wstring mid = name.substr(
            prefix.size(), name.size() - prefix.size() - suffix.size());
        if (mid.empty()) continue;
        bool digits = true;
        for (wchar_t c : mid) if (c < L'0' || c > L'9') { digits = false; break; }
        if (!digits) continue;
        const unsigned long pid = wcstoul(mid.c_str(), nullptr, 10);
        if (pid == 0 || pid == self) continue;

        std::error_code tec;
        const auto mtime = std::filesystem::last_write_time(it->path(), tec);
        if (!tec && (now - mtime) < std::chrono::seconds(60)) { ++kept_young; continue; }

        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
        if (h) {
            DWORD code = 0;
            const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            CloseHandle(h);
            if (alive) { ++kept_live; continue; }
        }
        std::error_code rm;
        if (std::filesystem::remove(it->path(), rm) && !rm) ++removed;
    }
    if (removed || kept_live || kept_young) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "GameIni: swept %d orphaned save temp(s) next to %s "
            "(kept %d owned by a live launcher, %d younger than 60s)",
            removed, ini_path.filename().string().c_str(), kept_live, kept_young);
    }
}

}  // namespace

bool ApplyForLaunch(const std::filesystem::path& exe_path, bool is_online) {
    auto ini = PathForExe(exe_path);
    // Hygiene, before anything else touches this directory: remove save temps
    // left behind by launchers that died between the write and the rename.
    // Skips anything a LIVE process owns (see SweepOrphanedSaveTemps).
    SweepOrphanedSaveTemps(ini);
    GamePlayConfig override_;
    LoadOverride(exe_path, override_);
    const bool has_overrides = override_.any_set();
    if (!has_overrides && !is_online) {
        // Nothing to do -- no overrides AND no anti-cheat clamps to
        // apply. Leave the user's game.ini untouched so we don't kick
        // off a backup-then-restore cycle on every launch when they
        // haven't changed anything.
        return true;
    }
    // Backup once; if .bak already exists from a prior launch that
    // didn't clean up (crashed launcher, force-killed game), keep the
    // existing one -- it's the older "true original."
    auto bak = BackupPathFor(ini);
    std::error_code ec;
    const bool bak_existed = std::filesystem::exists(bak, ec);
    if (!bak_existed && std::filesystem::exists(ini, ec)) {
        std::filesystem::copy_file(ini, bak,
            std::filesystem::copy_options::none, ec);
        if (ec == std::errc::file_exists) {
            // MULTI-INSTANCE RACE, not a failure. Three launchers can share one
            // install (the hub-brokered E2E harness runs host + guest + spectator
            // against the same game directory, and two real users on one PC can
            // do the same). Both processes ran `exists(bak) == false` before
            // either created it, so the loser's copy_file lands on an existing
            // destination. The invariant this backup exists to guarantee -- "a
            // true original is on disk" -- is SATISFIED at that point, by the
            // other process, one millisecond earlier. Treat it as success and
            // carry on to the apply.
            //
            // Measured 2026-08-15 (Wave 2 V4): HOST and GUEST both reached here
            // within the same millisecond; the loser logged "backup failed (File
            // exists); aborting apply". While ApplyForLaunch's return value was
            // discarded that abort was invisible and harmless (the winner had
            // just written the same resolved config). The moment the caller
            // started HONOURING the return value it became a refused online
            // launch, which is how it was found.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "GameIni: backup created concurrently by another launcher "
                "instance (%s) -- the original is preserved, continuing",
                bak.string().c_str());
            ec.clear();
        } else if (ec) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "GameIni: backup failed (%s); aborting apply to avoid clobber",
                ec.message().c_str());
            return false;
        }
    } else if (bak_existed) {
        // Deliberately kept (see above) -- but say so. A .bak left behind by a
        // crashed launcher or a force-killed game is the file RestoreFromBackup
        // puts back at StopSession, so any game.ini edit the user made between
        // that crash and now is silently discarded on exit. Until this line
        // that happened with zero log output. INFO, not WARN: keeping the older
        // backup IS the correct invariant, it is just not obvious.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "GameIni: reusing existing backup %s (kept as the true original). "
            "THAT file, not the current game.ini, is what gets restored on exit "
            "-- edits made to game.ini since it was taken will be lost.",
            bak.string().c_str());
    }
    GamePlayConfig resolved;
    LoadResolved(exe_path, resolved);
    if (is_online) ForceOnlineClamps(resolved);
    return Save(ini, resolved);
}

bool ForceFullscreenForLaunch(const std::filesystem::path& exe_path) {
    auto ini = PathForExe(exe_path);
    GamePlayConfig current;
    Load(ini, current);
    if (current.game_screen_mode == 1) {
        // Already fullscreen -- no write needed, no backup needed.
        return true;
    }
    // Same backup-once behaviour as ApplyForLaunch. Done before we
    // mutate so a crash mid-write doesn't lose the user's pristine ini.
    auto bak = BackupPathFor(ini);
    std::error_code ec;
    if (!std::filesystem::exists(bak, ec) &&
         std::filesystem::exists(ini, ec)) {
        std::filesystem::copy_file(ini, bak,
            std::filesystem::copy_options::none, ec);
        if (ec) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "GameIni: backup for fullscreen-pin failed (%s); aborting",
                ec.message().c_str());
            return false;
        }
    }
    GamePlayConfig resolved;
    LoadResolved(exe_path, resolved);
    resolved.game_screen_mode = 1;
    return Save(ini, resolved);
}

bool RestoreFromBackup(const std::filesystem::path& exe_path) {
    auto ini = PathForExe(exe_path);
    auto bak = BackupPathFor(ini);
    std::error_code ec;
    if (!std::filesystem::exists(bak, ec)) return true;  // no-op
    std::filesystem::remove(ini, ec);
    std::filesystem::rename(bak, ini, ec);
    if (ec) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "GameIni: restore failed: %s", ec.message().c_str());
        return false;
    }
    return true;
}

void ForceOnlineClamps(GamePlayConfig& cfg) {
    // HitJudge: hit-box debug overlay. Cheating online -- both sides
    // would expose internal hit/hurt-box geometry to the player.
    cfg.hit_judge = 0;
    // GameInformation: damage / state debug overlay. Same anti-cheat
    // reasoning -- exposes meter / stun / proration that's normally
    // implicit in the visual.
    cfg.game_information = 0;
    // CPU: never let a CPU player creep into an online slot. If
    // either of these is set in the source file the launcher overrides
    // to human (0).
    cfg.player0_cpu = 0;
    cfg.player1_cpu = 0;
}

}  // namespace fm2k::game_ini
