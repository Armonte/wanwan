// Game-install content tag -- see content_tag.h for why this exists next to
// game_hash.cpp instead of reusing it. Compiled into BOTH the hook (stamps the
// tag into replay headers at write time) and the launcher (recomputes it for a
// replay's game folder and refuses playback on a mismatch), so the two sides
// cannot drift apart on the canonicalization.
#include "content_tag.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#define XXH_INLINE_ALL
#define XXH_NO_STREAM
#include "../../../vendored/xxhash/xxhash.h"

namespace fm2k::content_tag {

namespace {

// Wide -> UTF-8 via Win32. std::filesystem::path's narrow accessors go through
// the process ANSI codepage on MinGW, which turns JP filenames into '_' / '?'
// differently on a JP-locale machine than an EN-locale one -- the canonical
// bytes have to be locale-independent or two identical installs disagree.
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

void LowerAscii(std::string& s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
}

std::string ExtLower(const std::filesystem::path& p) {
    std::string e = WideToUtf8(p.extension().wstring());
    LowerAscii(e);
    return e;
}

struct Entry {
    std::string name_lower;
    uint64_t    size;
};

}  // namespace

std::wstring ResolveGameExeName(const wchar_t* dir_w) {
    if (!dir_w || !dir_w[0]) return {};
    std::filesystem::path dir(dir_w);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return {};
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        std::string ext = ExtLower(e.path());
        if (ext != ".kgt") continue;
        std::filesystem::path exe = dir / e.path().stem();
        exe += L".exe";
        // Must EXIST. A .kgt whose sibling .exe is missing or renamed means we
        // cannot name the game exe, and guessing would compute the tag over a
        // different file set than the hook did -- which is the false-block this
        // whole shared-rule arrangement exists to prevent. Empty -> tag 0.
        std::error_code ec2;
        if (!std::filesystem::is_regular_file(exe, ec2)) return {};
        return exe.filename().wstring();
    }
    return {};
}

uint32_t ComputeForDir(const wchar_t* dir_w, const wchar_t* game_exe_name_w) {
    if (!dir_w || !dir_w[0]) return 0;

    // REQUIRED, no fallback. Including "every .exe" when the caller could not
    // name one would make the launcher hash a different file set than the hook
    // did on a .kgt-less install, and the replay row would be falsely blocked
    // as [different game files]. 0 = unknown correctly disables the gate.
    if (!game_exe_name_w || !game_exe_name_w[0]) return 0;
    std::string exe_lower = WideToUtf8(std::wstring(game_exe_name_w));
    LowerAscii(exe_lower);

    std::filesystem::path dir(dir_w);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;

    std::vector<Entry> entries;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const auto& p = e.path();
        const std::string ext = ExtLower(p);
        const bool is_player = (ext == ".player");
        const bool is_kgt    = (ext == ".kgt");
        const bool is_exe    = (ext == ".exe");
        if (!is_player && !is_kgt && !is_exe) continue;

        std::string name = WideToUtf8(p.filename().wstring());
        LowerAscii(name);
        // Exactly one .exe participates: the one the caller named. See the
        // shared-by-construction rule in content_tag.h.
        if (is_exe && name != exe_lower) continue;

        std::error_code ec2;
        const uint64_t sz = (uint64_t)std::filesystem::file_size(p, ec2);
        entries.push_back({std::move(name), ec2 ? 0u : sz});
    }
    if (entries.empty()) return 0;

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.name_lower < b.name_lower;
              });

    std::string canon;
    canon.reserve(entries.size() * 48);
    for (const auto& e : entries) {
        canon.append(e.name_lower);
        canon.push_back('|');
        char szbuf[32];
        std::snprintf(szbuf, sizeof(szbuf), "%llu", (unsigned long long)e.size);
        canon.append(szbuf);
        canon.push_back('\n');
    }

    const XXH64_hash_t h = XXH3_64bits(canon.data(), canon.size());
    uint32_t tag = (uint32_t)(h ^ (h >> 32));
    // 0 is the "unknown / not recorded" sentinel every consumer branches on,
    // so a legitimate fold onto 0 gets bumped rather than being read as absent.
    if (tag == 0) tag = 1;
    return tag;
}

uint32_t ComputeLocal() {
    static uint32_t s_cached = 0;
    static bool     s_done   = false;
    if (s_done) return s_cached;
    s_done = true;

    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    const std::filesystem::path exe(buf);
    s_cached = ComputeForDir(exe.parent_path().wstring().c_str(),
                             exe.filename().wstring().c_str());
    return s_cached;
}

}  // namespace fm2k::content_tag
