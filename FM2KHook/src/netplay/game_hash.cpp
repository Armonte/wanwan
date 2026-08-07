#include "game_hash.h"

#include <SDL3/SDL_log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#define XXH_INLINE_ALL
#define XXH_NO_STREAM
#include "../../../vendored/xxhash/xxhash.h"

namespace fm2k::game_hash {

namespace {

uint32_t s_cached_hash = 0;
bool     s_computed    = false;
char     s_describe[128] = {};
// Full per-file manifest cached alongside the hash so we can dump it
// to the log on a HELLO mismatch. Two peers can diff each other's
// hook logs to find the offending file: "Para has Bewear.player size
// 1234567 but I have 1234580 -- they edited the .player." Without
// this they'd see only the rolled-up 32-bit hashes and have no path
// to a fix beyond "send each other your install."
std::string s_manifest;

// Cached entries (name, size, content_hash) so the on-mismatch dump
// can iterate them directly without re-parsing s_manifest. The
// string-split path corrupted one entry under conditions we haven't
// reproduced -- bypassing it entirely is the safe move.
struct ManifestEntry {
    std::string name;
    uint64_t    size;
    uint64_t    content_hash;
};
std::vector<ManifestEntry> s_manifest_entries;

std::filesystem::path GameDir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
}

// Path to the running game executable (the one we're injected into).
// Used to filter out unrelated .exes that share the game folder --
// installer leftovers (unins000.exe, update.exe), bundled launchers
// (Pokemon Close Combat Launcher.exe), or third-party tools the user
// dropped in (antimicrox.exe, lilithport.exe). Only the game's own
// .exe affects sim correctness; the rest just bloated the hash and
// caused legitimate cross-install plays to mismatch.
std::filesystem::path GameExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf);
}

// UTF-8 path conversion via Win32 wide APIs. std::filesystem::path's
// narrow accessors go through the system ANSI codepage on MinGW and
// would mangle JP filenames into '_' / '?' -- different output on a
// Japanese-locale peer vs an English-locale peer for the same file
// on disk, which means different hashes for #57's handshake check.
// Using wide → UTF-8 makes the canonical bytes identical everywhere.
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                        w.data(), n);
    return w;
}
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

bool MatchesExt(const std::filesystem::path& p, const char* dotext) {
    // Extensions are pure ASCII for our purposes; using wide here is
    // belt-and-suspenders. tolower on ASCII stays safe.
    std::string e = WideToUtf8(p.extension().wstring());
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == dotext;
}

}  // namespace

uint32_t Compute() {
    if (s_computed) return s_cached_hash;
    s_computed = true;

    // TEMP: hash check disabled while we debug the v0.2.1/v0.2.2
    // canon corruption that produced spurious mismatches between
    // peers with byte-identical .player + .kgt + .exe content (e.g.
    // user vs Vikaar with same pkmncc.kgt content_hash but different
    // overall hash). Returning 0 hits the "peer is older / can't
    // enumerate" backwards-compat branch in netplay.cpp:229
    // (`local_hash != 0 && peer_hash != 0 && ...`), so the mismatch
    // check never fires regardless of what the peer reports. We
    // still build s_describe + s_manifest_entries so the popup +
    // boot log keep working for offline diagnostic purposes.
    //
    // Re-enable for local testing by setting FM2K_HASH_CHECK=1
    // (any non-zero value); the env var only affects the local
    // hash output, not the peer side, so two peers can collaborate
    // on testing by both flipping it on. Once the canon-build
    // corruption is fixed the env-var gate goes away.
    const char* env_enable = std::getenv("FM2K_HASH_CHECK");
    const bool hash_enabled = env_enable && env_enable[0] != '\0' &&
                              env_enable[0] != '0';
    if (!hash_enabled) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "GameHash: check DISABLED (set FM2K_HASH_CHECK=1 to "
            "re-enable). Returning 0; peer mismatch check will skip.");
        s_cached_hash = 0;
        // Still populate s_describe so the launcher status line and
        // Hub HELLO log have something readable.
        std::snprintf(s_describe, sizeof(s_describe),
                      "(hash check disabled)");
        return 0;
    }

    auto dir = GameDir();
    if (dir.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameHash: GetModuleFileNameA failed; hash=0");
        s_cached_hash = 0;
        return 0;
    }

    // Lowercase UTF-8 filename of the game's own executable. Used
    // below to filter the .exe pass to ONLY the game exe -- every
    // other .exe in the directory (installer remnants, bundled
    // launchers, antimicrox, etc.) is unrelated to sim correctness.
    std::string game_exe_lower;
    {
        auto exe = GameExePath();
        if (!exe.empty()) {
            game_exe_lower = WideToUtf8(exe.filename().wstring());
            for (auto& c : game_exe_lower)
                c = (char)std::tolower((unsigned char)c);
        }
    }

    // Per-extension policy:
    //   .player  → name + size only. Palette edits in a .player are
    //              gameplay-neutral (different colors, same frame data /
    //              hitboxes / scripts) and don't desync -- content
    //              hashing them would reject those legitimate setups.
    //              Palette swaps overwrite bytes in-place so they keep
    //              size constant; size-changing edits (added frames,
    //              new scripts) get caught by the size delta.
    //   .kgt     → name + size + xxhash of contents. The compiled
    //              gamesystem owns stage data, system globals, and
    //              cross-character logic; bytes must match exactly or
    //              gameplay diverges.
    //   .exe     → name + size + xxhash of contents. Engine version
    //              must match; different patches change subtle
    //              simulation behavior.
    struct Entry {
        std::string  name_lower;
        uint64_t     size;
        XXH64_hash_t content_hash;   // 0 when not content-hashed
    };
    std::vector<Entry> entries;

    int n_player = 0, n_kgt = 0, n_exe = 0;

    auto hash_file_contents = [](const std::filesystem::path& p) -> XXH64_hash_t {
        // Stream-friendly: read in 1 MiB chunks so a multi-MB .kgt
        // doesn't allocate a giant buffer up front. xxhash supports
        // streaming via XXH3_64bits_update / digest, but XXH_NO_STREAM
        // strips that -- so for these small files we read whole and
        // call the one-shot XXH3_64bits.
        std::error_code ec;
        std::uintmax_t sz = std::filesystem::file_size(p, ec);
        if (ec) return 0;
        // _wfopen (Win32 wide-char fopen) so JP filenames open
        // reliably regardless of the process's ANSI codepage.
        // Standard fopen with path::string().c_str() on MinGW would
        // try the mangled CP1252 form and fail on non-ASCII paths.
        FILE* f = _wfopen(p.wstring().c_str(), L"rb");
        if (!f) return 0;
        std::vector<uint8_t> buf((size_t)sz);
        size_t read = std::fread(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        if (read != buf.size()) return 0;
        return XXH3_64bits(buf.data(), buf.size());
    };

    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const auto& p = e.path();
        const bool is_player = MatchesExt(p, ".player");
        const bool is_kgt    = MatchesExt(p, ".kgt");
        const bool is_exe_ext = MatchesExt(p, ".exe");
        if (!is_player && !is_kgt && !is_exe_ext) continue;

        // Wide → UTF-8 so the canonical hash input is identical bytes
        // on every peer regardless of stdlib narrow-conversion quirks.
        std::string name = WideToUtf8(p.filename().wstring());
        std::string lower = name;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);

        // .exe filter: only the GAME's own exe (the one we're injected
        // into) affects sim correctness. Skip unrelated .exes the user
        // happens to keep alongside (installer / launcher / cheat
        // tools) so cross-install plays don't trip the mismatch check
        // on cosmetic differences. game_exe_lower is the running
        // module's filename; if we couldn't resolve it (rare), fall
        // back to including all .exes -- better safe than nothing.
        const bool is_exe = is_exe_ext &&
            (game_exe_lower.empty() || lower == game_exe_lower);
        if (is_exe_ext && !is_exe) continue;

        uint64_t sz = 0;
        std::error_code ec2;
        sz = (uint64_t)std::filesystem::file_size(p, ec2);
        if (ec2) sz = 0;

        XXH64_hash_t ch = 0;
        if (is_kgt || is_exe) {
            ch = hash_file_contents(p);
        }

        entries.push_back({std::move(lower), sz, ch});
        if (is_player) ++n_player;
        else if (is_kgt) ++n_kgt;
        else if (is_exe) ++n_exe;
    }

    // Sort lexicographically so peer order doesn't matter.
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.name_lower < b.name_lower;
              });

    // Canonical input: "<name>|<size>|<content_hash_hex_or_->\n" per entry.
    std::string canon;
    canon.reserve(entries.size() * 64);
    for (const auto& e : entries) {
        canon.append(e.name_lower);
        canon.push_back('|');
        char szbuf[32];
        std::snprintf(szbuf, sizeof(szbuf), "%llu",
                      (unsigned long long)e.size);
        canon.append(szbuf);
        canon.push_back('|');
        if (e.content_hash != 0) {
            char hbuf[24];
            std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                          (unsigned long long)e.content_hash);
            canon.append(hbuf);
        } else {
            canon.push_back('-');
        }
        canon.push_back('\n');
    }

    // xxhash3-64 → fold to 32 bits. Deterministic across architectures
    // for the same byte input. We just need enough bits to make
    // accidental collision negligible at lobby scale.
    XXH64_hash_t h = (entries.empty())
        ? 0
        : XXH3_64bits(canon.data(), canon.size());
    s_cached_hash = (uint32_t)(h ^ (h >> 32));
    // Reserve 0 as "no hash" sentinel for backwards compat. If the
    // legitimate hash collides with 0, bump it to 1.
    if (s_cached_hash == 0 && !entries.empty()) s_cached_hash = 1;

    std::snprintf(s_describe, sizeof(s_describe),
                  "%d .player + %d .kgt + %d .exe -- 0x%08X",
                  n_player, n_kgt, n_exe, s_cached_hash);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "GameHash: %s", s_describe);
    // Stash the canonical manifest for hash-mismatch diagnostics.
    // Keep it lightweight (one line per file already, no per-line
    // formatting changes needed). Truncated description flag is
    // implicit -- if the dir has 200+ files, the log line will be
    // long but each line is still parseable.
    s_manifest = canon;
    // Cache the per-entry data too so ForEachManifestEntry can dump
    // identical bytes to the boot-time SDL_LogInfo per-entry loop.
    // The on-mismatch dump path uses this to avoid re-parsing
    // s_manifest, which exposed a corruption case in v0.2.1.
    s_manifest_entries.clear();
    s_manifest_entries.reserve(entries.size());
    for (const auto& e : entries) {
        s_manifest_entries.push_back({e.name_lower, e.size,
                                      (uint64_t)e.content_hash});
    }
    // Per-file log as INFO so each peer's hook log records exactly
    // which entries went into the hash. On a mismatch, two peers
    // diff their logs and the offending row jumps out.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "GameHash: manifest (%zu entries):", entries.size());
    for (const auto& e : entries) {
        if (e.content_hash != 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "  %s|%llu|%016llx",
                        e.name_lower.c_str(),
                        (unsigned long long)e.size,
                        (unsigned long long)e.content_hash);
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "  %s|%llu|-",
                        e.name_lower.c_str(),
                        (unsigned long long)e.size);
        }
    }
    return s_cached_hash;
}

const char* DescribeLocal() { return s_describe; }
const char* ManifestLocal() {
    return s_manifest.empty() ? "(not computed yet)" : s_manifest.c_str();
}

void ForEachManifestEntry(
    void (*cb)(const char* name, uint64_t size, uint64_t content_hash, void* user),
    void* user)
{
    if (!cb) return;
    for (const auto& e : s_manifest_entries) {
        cb(e.name.c_str(), e.size, e.content_hash, user);
    }
}

}  // namespace fm2k::game_hash
