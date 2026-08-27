#include "FM2K_InstallHealth.h"
#include "FM2K_Utf8Path.h"

#include <windows.h>

#include <algorithm>
#include <system_error>

namespace fm2k::install_health {
namespace {

// Cap on how many shadowed names we collect. This is a diagnostic for a log
// line and a UI hint, not an inventory, and a mirrored game directory can hold
// hundreds of files.
constexpr size_t kMaxShadowed = 24;

// %LOCALAPPDATA%\VirtualStore\<dir without its drive>.
//
// relative_path() is what does the work: on "C:\Program Files\game" it yields
// "Program Files\game", which is precisely the layout Windows uses under
// VirtualStore. It also correctly yields nothing useful for a UNC or relative
// path, which is why the empty result is treated as "no mirror to look for"
// rather than being papered over.
std::filesystem::path VirtualStoreMirror(const std::filesystem::path& dir) {
    wchar_t lad[MAX_PATH] = {0};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const std::filesystem::path rel = dir.relative_path();
    if (rel.empty()) return {};
    return std::filesystem::path(lad) / L"VirtualStore" / rel;
}

// THE VALIDITY OF THIS PROBE DEPENDS ON OUR OWN MANIFEST, so it is worth
// stating rather than assuming. app.manifest gives the launcher an explicit
// requestedExecutionLevel, which opts this process OUT of UAC filesystem
// virtualization. That is the only reason a failed write here is meaningful:
// an UNmanifested process would have the same write silently redirected into
// VirtualStore and would report success, turning this check into a permanent
// false OK. If the manifest is ever dropped, this function starts lying.
bool DirWritable(const std::filesystem::path& dir, unsigned long* err) {
    const std::filesystem::path probe = dir / L".fm2k_write_probe";
    // DELETE_ON_CLOSE rather than a create/close/remove sequence: a crash
    // between the two would otherwise leave a stray dotfile in the user's
    // game folder, and this runs on every launch.
    const HANDLE h = CreateFileW(
        probe.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    CloseHandle(h);
    *err = 0;
    return true;
}

// Names present in BOTH the mirror and the real directory. Those are the
// files where the two processes genuinely disagree: the game reads the
// mirrored copy through the overlay, the launcher reads the real one.
std::vector<std::string> ShadowedFiles(const std::filesystem::path& real,
                                       const std::filesystem::path& mirror) {
    std::vector<std::string> out;
    std::error_code ec;
    if (mirror.empty() || !std::filesystem::is_directory(mirror, ec)) return out;

    for (std::filesystem::directory_iterator it(mirror, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::filesystem::path name = it->path().filename();
        if (!std::filesystem::exists(real / name, ec)) continue;
        // FilenameUtf8, never path::string(): on MinGW the narrow conversion
        // throws for any name outside the active code page, and a game folder
        // under a Japanese install is exactly where that bites.
        out.push_back(fm2k::utf8path::FilenameUtf8(name));
        if (out.size() >= kMaxShadowed) break;
    }

    // game.ini first when present -- it is the one entry that changes the
    // verdict from "logs are hiding" to "the two processes are running
    // different config", so it should not be buried mid-list in a log line.
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const bool ai = (_stricmp(a.c_str(), "game.ini") == 0);
        const bool bi = (_stricmp(b.c_str(), "game.ini") == 0);
        if (ai != bi) return ai;
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    return out;
}

}  // namespace

bool Report::config_split() const {
    return std::any_of(shadowed.begin(), shadowed.end(), [](const std::string& s) {
        return _stricmp(s.c_str(), "game.ini") == 0;
    });
}

Report Probe(const std::filesystem::path& exe_path) {
    Report r;
    if (exe_path.empty()) return r;
    r.game_dir = exe_path.parent_path();
    if (r.game_dir.empty()) return r;

    // Never let a probe abort a launch. Every failure mode here (a path the
    // filesystem library refuses, a mirror directory that vanishes mid-scan,
    // a conversion that throws) is strictly less important than starting the
    // game, so the whole body is contained and a broken probe degrades to
    // "no complaint" rather than to a crash on the launch path.
    try {
        const std::filesystem::path mirror = VirtualStoreMirror(r.game_dir);
        r.shadowed = ShadowedFiles(r.game_dir, mirror);
        if (!r.shadowed.empty()) r.virtualstore_dir = mirror;

        const bool writable = DirWritable(r.game_dir, &r.write_error);

        // ORDER MATTERS. A live mirror outranks a failed write, because it is
        // the stronger and more specific statement: NotWritable says the
        // game's writes are being redirected from now on, while a mirror that
        // shadows real files says the two processes are already reading
        // different bytes. The second is true even when the directory has
        // since become writable, which is the case a writability-only check
        // would call healthy and hand back a config that silently does not
        // apply.
        if (!r.shadowed.empty())      r.verdict = Verdict::VirtualStoreSplit;
        else if (!writable)           r.verdict = Verdict::NotWritable;
        else                          r.verdict = Verdict::Ok;
    } catch (const std::exception&) {
        r.verdict = Verdict::Ok;
    }
    return r;
}

std::string Describe(const Report& r) {
    if (r.healthy()) return {};

    const std::string dir = fm2k::utf8path::ToUtf8(r.game_dir);
    if (r.verdict == Verdict::NotWritable) {
        return "Install health: '" + dir + "' is NOT writable by the launcher "
               "(error " + std::to_string(r.write_error) + "). The game is "
               "unmanifested, so ITS writes to this folder are being silently "
               "redirected by UAC into %LOCALAPPDATA%\\VirtualStore -- settings "
               "and logs will appear to vanish. Move the game out of Program "
               "Files, or grant write access to this folder.";
    }

    std::string names;
    for (size_t i = 0; i < r.shadowed.size(); ++i) {
        if (i) names += ", ";
        names += r.shadowed[i];
    }
    if (r.shadowed.size() >= kMaxShadowed) names += ", ...";

    std::string s =
        "Install health: SPLIT INSTALL. A UAC VirtualStore mirror of '" + dir +
        "' exists at '" + fm2k::utf8path::ToUtf8(r.virtualstore_dir) +
        "' and shadows " + std::to_string(r.shadowed.size()) +
        " file(s): " + names +
        ". The game reads the mirrored copies; the launcher reads the real "
        "ones. They are different files.";
    if (r.config_split()) {
        s += " game.ini IS AMONG THEM, so launcher settings and the game's "
             "actual config have already diverged. Delete the mirror or move "
             "the game out of Program Files.";
    }
    return s;
}

}  // namespace fm2k::install_health
