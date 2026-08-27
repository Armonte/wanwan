// FM2K_InstallHealth -- detect the Program Files / UAC VirtualStore split
// that silently divides the launcher's view of the game directory from the
// game's own view of it.
//
// WHY THIS EXISTS. Our launcher ships an application manifest; the GAME does
// not. That single asymmetry is the whole bug. A manifested process is opted
// OUT of UAC filesystem virtualization, so when the launcher writes into
// C:\Program Files\<game>\ it gets a clean ACCESS_DENIED. An unmanifested
// 32-bit process is opted IN, so when the GAME (and our hook inside it)
// writes the same path, Windows silently redirects the write to
// %LOCALAPPDATA%\VirtualStore\Program Files\<game>\ and reports success.
//
// The result is not a failure, which is what makes it expensive: the two
// processes end up reading DIFFERENT game.ini files and neither one is
// wrong from its own point of view. The user changes a setting in the
// launcher and the game does not observe it, or the game writes a setting
// and the launcher cannot see it. Logs land somewhere the user is not
// looking, which is why field reports of this arrive as "no logs" or
// "settings don't apply" rather than as a permissions complaint.
//
// Reads are overlaid too, and that is why a WRITABLE directory is not
// automatically healthy. Once a virtualized copy of a file exists, the
// unmanifested process keeps reading THAT copy even if the directory later
// becomes writable. A mirror left behind by an earlier install therefore
// keeps splitting the two views indefinitely, so the mirror is probed
// independently of writability rather than only when a write fails.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fm2k::install_health {

enum class Verdict {
    Ok,                  // writable, and no VirtualStore mirror shadowing it
    NotWritable,         // launcher cannot write here; the game's writes are
                         // being virtualized right now
    VirtualStoreSplit,   // a mirror exists and shadows real files -- the two
                         // processes are reading different bytes TODAY
};

struct Report {
    Verdict verdict = Verdict::Ok;
    std::filesystem::path game_dir;
    std::filesystem::path virtualstore_dir;  // empty when no mirror exists
    std::vector<std::string> shadowed;       // basenames present in BOTH
    unsigned long write_error = 0;           // GetLastError() from the probe

    bool healthy() const { return verdict == Verdict::Ok; }
    // True when game.ini specifically is shadowed, i.e. the launcher and the
    // game are reading different CONFIG. That is the case that silently
    // breaks online clamps and round config rather than merely hiding logs.
    bool config_split() const;
};

// Probe the directory containing exe_path. Cheap (one CreateFile plus one
// directory enumeration) and side-effect free: the probe file is opened with
// FILE_FLAG_DELETE_ON_CLOSE so a crash between create and close cannot leave
// litter in the user's game folder.
Report Probe(const std::filesystem::path& exe_path);

// One-line, user-facing-ish summary for the log. Returns an empty string when
// the report is healthy so callers can log unconditionally without adding
// noise to the overwhelmingly common good case.
std::string Describe(const Report& r);

}  // namespace fm2k::install_health
