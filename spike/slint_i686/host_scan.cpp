#include "host_scan.h"
#include "host_captures.h"   // CaptureSlideshow: field 9 of a row

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <windows.h>

namespace fs = std::filesystem;

// ── DISCOVERY ────────────────────────────────────────────────────────────
//
// Reads the REAL launcher config rather than a spike-local one, so the
// skeleton lists the same games the shipping launcher does on this machine
// without anyone re-entering paths:
//   %APPDATA%\FM2K\RollbackLauncher\launcher.cfg -- one games root per line.
// That file is treated as READ-ONLY here. A spike must not be able to
// corrupt the real launcher's configuration; new roots picked in
// onboarding go to the spike's own file.

static fs::path ConfigDir() {
    // GetEnvironmentVariableW rather than _wdupenv_s: the latter is an MSVC
    // secure-CRT extension and does not link under mingw.
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(buf) / L"FM2K" / L"RollbackLauncher";
}

// CP932 -> UTF-8. Game titles are Shift-JIS and there is no way around it:
// the name lives in the .kgt as raw CP932 bytes.
static std::string Cp932ToUtf8(const char* bytes, size_t n) {
    if (!bytes || n == 0) return {};
    const int wn = MultiByteToWideChar(932, 0, bytes, (int)n, nullptr, 0);
    if (wn <= 0) return {};
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(932, 0, bytes, (int)n, &w[0], wn);
    const int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    if (un <= 0) return {};
    std::string u((size_t)un, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &u[0], un, nullptr, nullptr);
    return u;
}

// The friendly title. KgtFileHeader puts a 256-byte CP932 project name at
// offset 16, which is what the shipping launcher shows via ParseKgtSummary.
// Without this every game displays as its exe stem -- "WonderfulWorld_ver_0946"
// instead of the name its author gave it.
static std::string KgtProjectName(const fs::path& kgt) {
    std::ifstream f(kgt, std::ios::binary);
    if (!f) return {};
    char hdr[16 + 256] = {};
    f.read(hdr, sizeof hdr);
    if ((size_t)f.gcount() < sizeof hdr) return {};
    const char* name = hdr + 16;
    size_t n = 0;
    while (n < 256 && name[n] != '\0') ++n;
    std::string s = Cp932ToUtf8(name, n);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

// Short rail code, e.g. "Hypothetical HRT Fighting Game" -> "HHFG".
// The design hand-authors these ("KOF98", "SF3-3S") and discovery has no
// such field, so it is derived: initials when the title has several words,
// otherwise the leading characters. A 36px chip cannot hold a title, and
// eliding one gives four chips all reading "Hy..." -- which is worse than a
// crude abbreviation because it cannot be told apart at all.
static std::string RailCode(const std::string& title) {
    // Count ASCII word starts. Non-ASCII (CJK) titles have no spaces to
    // split on, so they fall through to the leading-characters path, which
    // is right: two kanji already identify a game.
    std::string initials;
    bool at_start = true;
    bool ascii_only = true;
    for (unsigned char c : title) {
        if (c >= 0x80) { ascii_only = false; break; }
    }
    if (ascii_only) {
        for (unsigned char c : title) {
            const bool sep = (c == ' ' || c == '_' || c == '-' || c == '.');
            if (sep) { at_start = true; continue; }
            if (at_start && std::isalnum(c)) {
                initials += (char)std::toupper(c);
                at_start = false;
                if (initials.size() >= 5) break;
            }
        }
        if (initials.size() >= 2) return initials;
    }
    // Leading characters, cut on a UTF-8 boundary so a multi-byte glyph is
    // never sliced in half (which renders as a replacement box).
    // Two glyphs for CJK, four for Latin. CJK is full-width, so four of
    // them overflow a 36px chip and elide back to "..." -- which is exactly
    // the unreadable state this function exists to avoid.
    const size_t budget = ascii_only ? 4u : 2u;
    std::string out;
    size_t i = 0, glyphs = 0;
    while (i < title.size() && glyphs < budget) {
        const unsigned char c = (unsigned char)title[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > title.size()) break;
        out.append(title, i, len);
        i += len;
        ++glyphs;
    }
    return out.empty() ? std::string("--") : out;
}

// Companion .kgt for an exe: same stem first, then any .kgt in the folder.
// Same-stem first is not cosmetic -- a folder can hold several games (our
// URORFG folder holds four), and picking "any" would label them all alike.
static fs::path CompanionKgt(const fs::path& exe) {
    std::error_code ec;
    fs::path same = exe;
    same.replace_extension(".kgt");
    if (fs::exists(same, ec)) return same;
    for (fs::directory_iterator it(exe.parent_path(), ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string e = it->path().extension().string();
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (e == ".kgt") return it->path();
    }
    return {};
}

// ENGINE SNIFF -- the same test the shipping launcher uses
// (launcher/discovery/game_discovery_sniff.cpp:64): scan the first 256 KB
// of an exe for the engine's window-class / tag strings. Reproduced rather
// than invented so the spike lists exactly the games the real launcher
// would, including the ones it would reject.
static const char* SniffEngine(const fs::path& exe) {
    std::ifstream f(exe, std::ios::binary);
    if (!f) return nullptr;
    std::vector<char> buf(256 * 1024);
    f.read(buf.data(), (std::streamsize)buf.size());
    const size_t got = (size_t)f.gcount();
    if (got < 1024) return nullptr;
    auto has = [&](const char* needle) {
        const size_t n = std::char_traits<char>::length(needle);
        if (n == 0 || n > got) return false;
        return std::search(buf.begin(), buf.begin() + (ptrdiff_t)got,
                           needle, needle + n) != buf.begin() + (ptrdiff_t)got;
    };
    if (has("KGT95GAME")) return "FM95";
    if (has("KGT2KGAME") || has("2DKGT2K") || has("kgt2k.INI")) return "FM2K";
    return nullptr;
}

// THUMBNAIL INDEX, written by tools/capture_transcode.py.
//
// A plain "exe path -> thumbnail path" table, because the mapping from a
// game to its capture directory lives in the registry and the Python batch
// resolves it there. Duplicating that resolution in C++ would mean two
// implementations of the same matching rules drifting apart -- and those
// rules already needed three passes (exe stems, folded names, version
// stripping) to cover the library.
static std::map<std::string, std::string> g_thumbs;

void LoadThumbIndex() {
    const fs::path idx = fs::path("/mnt/c/games/captures/index.tsv");
    std::ifstream f(idx);
    if (!f) {
        // Try the Windows-side path too; the spike runs from either.
        std::ifstream g("C:\\games\\captures\\index.tsv");
        if (!g) return;
        std::string line;
        while (std::getline(g, line)) {
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            g_thumbs[line.substr(0, tab)] = line.substr(tab + 1);
        }
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        g_thumbs[line.substr(0, tab)] = line.substr(tab + 1);
    }
}

// Scan progress, shared with the UI thread.
std::mutex g_scan_mutex;
std::string g_scan_result;
std::atomic<bool> g_scan_done{false};
std::atomic<bool> g_scanning{false};
std::atomic<int> g_scan_seen{0};
std::atomic<int> g_scan_hits{0};
std::atomic<int> g_scan_cached{0};

// CACHE. The shipping launcher keeps a games.cache for exactly this reason:
// a cold scan reads 256 KB per candidate exe, and repeating that on every
// start is the difference between an instant launcher and a five-second
// stare. Keyed on path + size + mtime, so a rebuilt or replaced game is
// re-sniffed while everything else is free.
//
// Written to the SPIKE's own file, never the launcher's games.cache -- the
// formats are unrelated and clobbering the real one would break the
// shipping launcher's startup.
struct CacheEntry { std::string engine, name; uintmax_t size; long long mtime; };
static std::map<std::string, CacheEntry> g_cache;

static fs::path SpikeCachePath() { return fs::path("2dfm_spike_games.cache"); }

void LoadCache() {
    std::ifstream f(SpikeCachePath());
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // path \t size \t mtime \t engine \t name
        size_t a = line.find('\t'); if (a == std::string::npos) continue;
        size_t b = line.find('\t', a + 1); if (b == std::string::npos) continue;
        size_t c = line.find('\t', b + 1); if (c == std::string::npos) continue;
        size_t d = line.find('\t', c + 1); if (d == std::string::npos) continue;
        CacheEntry e;
        e.size   = (uintmax_t)std::strtoull(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
        e.mtime  = std::strtoll(line.substr(b + 1, c - b - 1).c_str(), nullptr, 10);
        e.engine = line.substr(c + 1, d - c - 1);
        e.name   = line.substr(d + 1);
        g_cache[line.substr(0, a)] = std::move(e);
    }
}

void SaveCache() {
    std::ofstream f(SpikeCachePath(), std::ios::trunc);
    for (const auto& [path, e] : g_cache)
        f << path << '\t' << e.size << '\t' << e.mtime << '\t'
          << e.engine << '\t' << e.name << '\n';
}

// Walk the configured roots and build the game list. Bounded depth: a games
// root is a folder of games, not a whole drive, and an unbounded recurse
// from C:\ would hang the UI on the first mis-click in the folder picker.
// One published library row (name, rail code, folder, engine, size, online,
// thumb, stem, exe path). Shared by the live scan and the cache-first
// publish, so the two cannot describe a game differently.
static void EmitRow(std::string& out, const std::string& name, const fs::path& canon,
                    const std::string& engine, uintmax_t sz, const std::string& key,
                    const fs::path& p) {
    g_scan_hits.fetch_add(1);
    out += name;
    out += '\t';
    out += RailCode(name);
    out += '\t';
    // Immediate parent folder only. The full path is too long for a
    // list row and the leaf is what actually distinguishes copies.
    {
        // SUBTITLE IS ONLY SHOWN WHEN IT ADDS SOMETHING. The .kgt
        // project title and the folder name are the same string for
        // most games ("Henichi" / "Henichi"), and printing both
        // makes every tile look like it is stuttering. The folder is
        // kept only when it actually differs -- which is exactly the
        // case where it disambiguates two copies of one game.
        std::string folder = canon.parent_path().filename().u8string();
        auto fold = [](std::string v) {
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            v.erase(std::remove_if(v.begin(), v.end(),
                    [](unsigned char c) { return c == ' ' || c == '_' || c == '-'; }),
                    v.end());
            return v;
        };
        out += (fold(folder) == fold(name)) ? std::string() : folder;
    }
    out += '\t';
    out += engine;
    out += '\t';
    // The exe's own size, not a recursive folder walk. The blueprint
    // shows a download size, which is a backend fact; walking every
    // game folder on startup to invent a local approximation would
    // cost more than the number is worth.
    {
        char sb[32];
        if (sz >= 1024u * 1024u)
            std::snprintf(sb, sizeof sb, "%.1f MB", (double)sz / (1024.0 * 1024.0));
        else
            std::snprintf(sb, sizeof sb, "%llu KB", (unsigned long long)(sz / 1024u));
        out += sb;
    }
    // Field 5 is the online count. It leaves here as 0 and the
    // host rewrites it from the hub's room list -- discovery has no
    // idea who is online and should not pretend to.
    out += "\t0\t";
    {
        auto it = g_thumbs.find(key);
        if (it != g_thumbs.end()) {
            // The index names the 320x240 ".thumb.webp"; the full
            // 640x480 capture sits beside it. Tiles are 320 wide
            // or larger, so the full frame is the right source:
            // it only ever scales DOWN. Fall back to the thumb.
            std::string full = it->second;
            const size_t t = full.find(".thumb.");
            if (t != std::string::npos) full.erase(t, 6);
            std::error_code ec2;
            out += fs::exists(fs::path(full), ec2) ? full : it->second;
        }
    }
    // Field 7: the EXE STEM, which is what the hub uses as a room
    // id. Appended rather than inserted so the field positions the
    // UI already parses do not shift, and carried explicitly
    // because matching a room by folder or title guesses wrong on
    // exactly the games that have several copies installed.
    out += '\t';
    out += p.stem().u8string();
    // Field 8: the exe itself. BOOT launches it; the stem alone
    // cannot name a game installed twice.
    out += '\t';
    out += p.u8string();
    // Field 9: the reviewed slideshow -- approved captures beyond the
    // lead, '|'-joined thumb paths (host_captures.h). Empty until the
    // owner approves shots on the GAME tab.
    out += '\t';
    out += hostgui::CaptureSlideshow(p.u8string());
    out += "\n";
}

static std::string ScanGames(const std::vector<std::string>& roots) {
    std::string out;
    std::error_code ec;
    // DEDUPE by canonical path. The same game reachable through two roots,
    // or through a root and a symlink, is one game -- the shipping launcher
    // dedupes and a list that shows it twice invites launching "the other
    // one" to fix a problem.
    std::set<std::string> seen_paths;

    for (const std::string& root : roots) {
        const fs::path base = fs::u8path(root);
        if (!fs::is_directory(base, ec)) { ec.clear(); continue; }
        for (fs::recursive_directory_iterator it(base,
                 fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            // Same depth budget as the shipping launcher (DISCOVERY_MAX_DEPTH,
            // game_discovery.cpp): publisher/game/version and then some.
            if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) { ec.clear(); continue; }
            const fs::path p = it->path();
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (ext != ".exe") continue;

            // Cheap size gate BEFORE opening the file. The launcher's own
            // heuristic puts clean FM2K runtimes near 1.1-1.7 MB and FM95
            // near 400 KB, so anything outside a generous band is not a
            // candidate and does not deserve a 256 KB read.
            const auto sz = fs::file_size(p, ec);
            if (ec) { ec.clear(); continue; }
            if (sz < 64u * 1024u || sz > 8u * 1024u * 1024u) continue;

            fs::path canon = fs::weakly_canonical(p, ec);
            if (ec) { ec.clear(); canon = p; }
            const std::string key = canon.u8string();
            if (!seen_paths.insert(key).second) continue;

            const auto wt = fs::last_write_time(p, ec);
            const long long mtime = ec ? 0 : (long long)wt.time_since_epoch().count();
            if (ec) ec.clear();

            g_scan_seen.fetch_add(1);

            std::string engine, name;
            auto hit = g_cache.find(key);
            if (hit != g_cache.end() && hit->second.size == sz && hit->second.mtime == mtime) {
                if (hit->second.engine.empty()) continue;  // cached NEGATIVE
                engine = hit->second.engine;
                name   = hit->second.name;
                g_scan_cached.fetch_add(1);
            } else {
                const char* e2 = SniffEngine(p);
                // Cache the miss too. Most exes under a games root are not
                // games, and re-reading 256 KB of each of them on every
                // start is most of the cold-scan cost.
                CacheEntry ce{ e2 ? e2 : "", "", sz, mtime };
                if (!e2) { g_cache[key] = ce; continue; }
                engine = e2;
                const fs::path kgt = CompanionKgt(p);
                if (!kgt.empty()) name = KgtProjectName(kgt);
                ce.name = name;
                g_cache[key] = ce;
            }
            if (engine.empty()) continue;
            if (name.empty()) name = p.stem().u8string();  // fallback: exe stem

            EmitRow(out, name, canon, engine, sz, key, p);
        }
        ec.clear();
    }
    SaveCache();
    return out;
}

std::string RowsFromCache() {
    std::string out;
    for (const auto& kv : g_cache) {
        const CacheEntry& e = kv.second;
        if (e.engine.empty()) continue;   // cached negative
        const fs::path p = fs::u8path(kv.first);
        const std::string name = e.name.empty() ? p.stem().u8string() : e.name;
        EmitRow(out, name, p, e.engine, e.size, kv.first, p);
    }
    return out;
}

void StartScan(std::vector<std::string> roots) {
    if (g_scanning.exchange(true)) return;  // one at a time
    g_scan_done = false;
    g_scan_seen = 0;
    g_scan_hits = 0;
    g_scan_cached = 0;
    std::thread([roots = std::move(roots)] {
        std::string r = ScanGames(roots);
        {
            std::lock_guard<std::mutex> lk(g_scan_mutex);
            g_scan_result = std::move(r);
        }
        g_scan_done = true;
        g_scanning = false;
    }).detach();
}

// Roots: THE launcher.cfg, the same file the shipping launcher reads and
// writes (%APPDATA%\FM2K\RollbackLauncher\launcher.cfg, one folder per
// line). "Set up" means "has a games folder" -- the rule the shipping
// launcher's first-run prompt uses -- so there is no second flag to drift.
// Both launchers agree on what is installed and whether setup is done.
//
// Everything else the shell remembers for itself (hub host, recently
// played, nick) lives beside it in ui_state.ini, never beside the exe.
static fs::path SpikeConfigPath() { return ConfigDir() / L"ui_state.ini"; }

std::vector<std::string> LoadRoots(bool* onboarded) {
    std::vector<std::string> roots;
    std::ifstream rf(ConfigDir() / L"launcher.cfg");
    std::string line;
    while (std::getline(rf, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) roots.push_back(line);
    }
    *onboarded = !roots.empty();
    return roots;
}

void SaveRoots(const std::vector<std::string>& roots, bool /*onboarded: implied by roots*/) {
    std::error_code ec;
    fs::create_directories(ConfigDir(), ec);
    std::ofstream f(ConfigDir() / L"launcher.cfg", std::ios::trunc);
    for (const std::string& r : roots) f << r << "\n";
}

// The folder-dialog callback may fire on another thread (SDL documents
// this), so the result is parked under a lock and applied by the main loop.

// Preferences live beside the games roots in the spike's own ini. They are
// read back through the same key/value pass rather than a second parser:
// one file, one format, one place a typo can hide.
//
// The DEFAULT is deliberately not zero. Fullscreen and vsync ship on, so an
// absent file has to mean "on", and a naive 0 would silently turn both off
// the first time anyone launched.
// SOCD mode 1 (Hitbox) sits in bits 4-6: it is the tournament default and
// the shipping launcher's compiled-in default, so a missing file behaves
// identically in both.
static constexpr int kPrefsDefault = 1 /*fullscreen*/ | 2 /*vsync*/ | (1 << 4);

void LoadPrefs(int* bits, std::string* nick) {
    if (bits) *bits = kPrefsDefault;
    if (nick) nick->clear();
    std::ifstream f(SpikeConfigPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (k == "prefs" && bits) {
            // std::stoi throws on garbage, and a corrupt ini should not
            // take the launcher down -- fall back to the defaults.
            try { *bits = std::stoi(v); } catch (...) { *bits = kPrefsDefault; }
        } else if (k == "nick" && nick) {
            *nick = v;
        }
    }
}

// The MRU is stored as one `mru=` line per game, in order, rather than a
// delimited list: a game title can contain almost anything (these are CP932
// project names) and picking a separator it cannot contain is a losing game.
std::string LoadHubHost() {
    std::ifstream f(SpikeConfigPath());
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("hub=", 0) == 0) return line.substr(4);
    }
    return {};
}

void SaveHubHost(const std::string& host) {
    std::vector<std::string> keep;
    {
        std::ifstream f(SpikeConfigPath());
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("hub=", 0) == 0) continue;
            if (!line.empty()) keep.push_back(line);
        }
    }
    std::ofstream o(SpikeConfigPath(), std::ios::trunc);
    if (!o) return;
    for (const std::string& l : keep) o << l << "\n";
    o << "hub=" << host << "\n";
}

std::vector<std::string> LoadMru() {
    std::vector<std::string> mru;
    std::ifstream f(SpikeConfigPath());
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("mru=", 0) == 0) mru.push_back(line.substr(4));
    }
    return mru;
}

void SaveMru(const std::vector<std::string>& mru) {
    std::vector<std::string> keep;
    {
        std::ifstream f(SpikeConfigPath());
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("mru=", 0) == 0) continue;
            if (!line.empty()) keep.push_back(line);
        }
    }
    std::ofstream o(SpikeConfigPath(), std::ios::trunc);
    if (!o) return;
    for (const std::string& l : keep) o << l << "\n";
    // Bounded: the rail shows a handful, and an unbounded list would grow
    // for as long as someone keeps trying games.
    size_t n = 0;
    for (const std::string& m : mru) {
        if (n++ >= 16) break;
        o << "mru=" << m << "\n";
    }
}

void SavePrefs(int bits, const std::string& nick) {
    // Rewrite the whole file: the roots lines have to survive, so read
    // them back first rather than appending duplicate keys forever.
    std::vector<std::string> keep;
    {
        std::ifstream f(SpikeConfigPath());
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("prefs=", 0) == 0 || line.rfind("nick=", 0) == 0) continue;
            if (!line.empty()) keep.push_back(line);
        }
    }
    std::ofstream o(SpikeConfigPath(), std::ios::trunc);
    if (!o) return;
    for (const std::string& l : keep) o << l << "\n";
    o << "prefs=" << bits << "\n";
    o << "nick=" << nick << "\n";
}
