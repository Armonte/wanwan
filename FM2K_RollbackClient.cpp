#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#include "SDL3/SDL.h"
#include <SDL3_image/SDL_image.h>
#include "app_icon_data.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "MinHook.h"
#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include "FM2K_GameIni.h"
#include "FM2K_Utf8Path.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/ui/input_binder.h"  // RefreshGamepads() on SDL hot-plug events
#include "FM2KHook/src/netplay/spec_relay_queue.h"  // hub-relay outbound drain (Phase 2c)
#define XXH_INLINE_ALL
#include "vendored/xxhash/xxhash.h"
#include "LocalSession.h"
#include "OnlineSession.h"
#include "FM2K_PortMapper.h"  // --upnp-test self-contained router validation

#include <chrono>
#include <string>
#include <cstring>
#include <cstdlib>  // std::getenv for FM2K_FULL_CRCS perf-run override
#include <optional>
#include <vector>
#include <iostream>
#include <thread>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <system_error>


// -----------------------------------------------------------------------------
// Async game discovery support
// -----------------------------------------------------------------------------

// Custom SDL event sent from the worker thread once discovery finishes.
static Uint32 g_event_discovery_complete = 0;

// Worker thread entry-point. Performs blocking discovery on a background thread
// and notifies the main thread with the resulting vector.
static int DiscoveryThreadFunc(void* userdata) {
    FM2KLauncher* launcher = static_cast<FM2KLauncher*>(userdata);
    if (!launcher) {
        return -1;
    }

    // The heavy lifting ? this call walks the filesystem and builds the list.
    auto games = new std::vector<FM2K::FM2KGameInfo>(launcher->DiscoverGames());

    SDL_Event ev{};
    ev.type = g_event_discovery_complete;
    ev.user.data1 = games;   // Ownership transferred to main thread
    ev.user.code = 0;
    SDL_PushEvent(&ev);

    return 0;
}

// FM2K Input Structure (11-bit input mask)
struct FM2KInput {
    union {
        struct {
            uint16_t left     : 1;   // 0x001
            uint16_t right    : 1;   // 0x002
            uint16_t up       : 1;   // 0x004
            uint16_t down     : 1;   // 0x008
            uint16_t button1  : 1;   // 0x010
            uint16_t button2  : 1;   // 0x020
            uint16_t button3  : 1;   // 0x040
            uint16_t button4  : 1;   // 0x080
            uint16_t button5  : 1;   // 0x100
            uint16_t button6  : 1;   // 0x200
            uint16_t button7  : 1;   // 0x400
            uint16_t reserved : 5;   // Unused bits
        } bits;
        uint16_t value;
    } input;
};

// Global variables
SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
bool running = false;

// FM2K Process Management (Launcher Model)
HANDLE fm2k_process = nullptr;
DWORD fm2k_process_id = 0;
PROCESS_INFORMATION fm2k_process_info = {};
bool game_launched = false;

// Timing (100 FPS for FM2K)
using micro = std::chrono::microseconds;
using fm2k_frame = std::chrono::duration<unsigned int, std::ratio<1, 100>>;  // 100 FPS
using gclock = std::chrono::steady_clock;

// Global launcher instance (since callbacks need global access)
static std::unique_ptr<FM2KLauncher> g_launcher = nullptr;

// Utility implementations
namespace Utils {
    std::vector<std::string> FindFilesWithExtension(const std::string& directory, const std::string& extension) {
        // Non-recursive scan using SDL3's filesystem helper.
        std::vector<std::string> files;
        int count = 0;
        char* pattern = static_cast<char*>(SDL_malloc(SDL_strlen(extension.c_str()) + 2));
        if (!pattern) return files;
        
        SDL_snprintf(pattern, SDL_strlen(extension.c_str()) + 2, "*%s", extension.c_str());
        char **list = SDL_GlobDirectory(directory.c_str(), pattern, /*flags=*/0, &count);
        SDL_free(pattern);

        if (list) {
            for (int i = 0; i < count; ++i) {
                if (list[i]) files.emplace_back(list[i]);
            }
            SDL_free(list);
        }
        return files;
    }
    
    bool FileExists(const std::string& path) {
        if (SDL_GetPathInfo(path.c_str(), nullptr)) {
            return true;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FileExists check failed for %s: %s", path.c_str(), SDL_GetError());
        return false;
    }
    
    std::string GetFileVersion(const std::string& exe_path SDL_UNUSED) {
        // TODO: Implement proper version detection
        return "Unknown";
    }
    
    uint32_t Fletcher32(const uint16_t* data, size_t len) {
        uint32_t c0, c1;
        len = (len + 1) & ~1;
        
        for (c0 = c1 = 0; len > 0; ) {
            size_t blocklen = len;
            if (blocklen > 360 * 2) {
                blocklen = 360 * 2;
            }
            len -= blocklen;
            do {
                c0 = c0 + *data++;
                c1 = c1 + c0;
            } while ((blocklen -= 2));
            c0 = c0 % 65535;
            c1 = c1 % 65535;
        }
        return (c1 << 16 | c0);
    }
    
    float GetFM2KFrameTime(float frames_ahead) {
        const float base_frame_time = 1.0f / 100.0f;  // 10ms per frame
        
        if (frames_ahead >= 0.75f) {
            return base_frame_time * 1.02f;  // Slow down if too far ahead
        } else {
            return base_frame_time;
        }
    }
    
    std::chrono::milliseconds GetFrameDuration() {
        return std::chrono::milliseconds(10);  // 100 FPS = 10ms per frame
    }

    // ---------------------------------------------------------------------
    // Config handling (persistent games folder)
    // ---------------------------------------------------------------------
    static std::string GetConfigDir() {
        const char *pref = SDL_GetPrefPath("FM2K", "RollbackLauncher");
        std::string dir;
        
        if (pref) {
            dir = pref;
            SDL_free(const_cast<char*>(pref));
        } else {
            const char* base = SDL_GetBasePath();
            dir = base ? base : "";
            if (base) SDL_free(const_cast<char*>(base));
        }

        size_t len = SDL_strlen(dir.c_str());
        if (len > 0 && dir[len-1] != '/' && dir[len-1] != '\\') {
            dir.push_back('/');
        }

        // Ensure directory exists
        SDL_CreateDirectory(dir.c_str());
        return dir;
    }

    static std::string GetConfigFilePath() {
        return GetConfigDir() + "launcher.cfg";
    }

    // Persisted games-root list lives in launcher.cfg as one path per line.
    // The historical single-string format is just a one-line file, which the
    // line-by-line reader handles transparently — that's our migration story.
    std::vector<std::string> LoadGamesRootPaths() {
        std::vector<std::string> result;
        std::string cfg = GetConfigFilePath();
        if (!SDL_GetPathInfo(cfg.c_str(), nullptr)) {
            return result;
        }

        std::ifstream in(cfg);
        if (!in) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open config file: %s", cfg.c_str());
            return result;
        }

        std::string line;
        while (std::getline(in, line)) {
            // Strip trailing CR (Windows-saved files) and surrounding whitespace.
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                     line.back() == ' '  || line.back() == '\t')) {
                line.pop_back();
            }
            size_t start = 0;
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
                ++start;
            }
            if (start > 0) line.erase(0, start);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            result.push_back(line);
        }
        return result;
    }

    void SaveGamesRootPaths(const std::vector<std::string>& paths) {
        std::string cfg = GetConfigFilePath();
        std::ofstream out(cfg, std::ios::trunc);
        if (!out) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write config file: %s", cfg.c_str());
            return;
        }
        for (const auto& p : paths) {
            if (p.empty()) continue;
            out << p << "\n";
        }
    }

    // -------------------------------------------------------------
    // Discovery result cache.
    //
    // Stores the FULL parsed result for each previously-discovered game so
    // a warm rescan reads zero bytes from disk per game. On a hit we skip:
    //   - Utils::HashFileXXH64        (full exe read for xxhash)
    //   - fm2k::ParseKgtSummary       (kgt header + name table parse)
    //   - Utils::SniffEngineFromStrings (256 KB exe head read)
    //   - Utils::FindKnownExe         (registry lookup)
    //   - Utils::DetectPackerFromPE   (PE section sniff)
    //
    // Cache validity: per-game, cached entry is reused iff BOTH the exe's
    // (size, mtime) and the kgt's (size, mtime) match the on-disk values.
    // If either changed, the entry is treated as a miss and rebuilt.
    //
    // File format (binary, little-endian):
    //   magic[4]      = "FM2K"
    //   version       = u32 (currently 2)
    //   entry_count   = u32
    //   per entry:
    //     str exe_path           (str = u32 length + bytes, no NUL)
    //     str dll_path
    //     u64 exe_size
    //     i64 exe_mtime
    //     u64 exe_xxh64
    //     u32 engine             (FM2K::Engine cast)
    //     str clean_label
    //     str packer_label
    //     u8  is_clean
    //     u8  kgt_present        (0/1; 0 for FM95 .player-only games)
    //     u64 kgt_size           (0 if absent)
    //     i64 kgt_mtime          (0 if absent)
    //     u8  kgt_valid          (0/1; only meaningful if kgt_present)
    //     str kgt_project_name
    //     u32 player_count + that many strings
    //     u32 stage_count  + that many strings
    //     u32 demo_count   + that many strings
    //
    // Old text-format caches fail the "FM2K" magic check on read and are
    // silently dropped — first warm rescan after the upgrade pays the full
    // cost once, then writes the new binary format. No migration needed.
    // -------------------------------------------------------------

    struct GameCacheEntry {
        std::string exe_path;
        std::string dll_path;
        // exe stat — used for invalidation
        uint64_t    size       = 0;
        int64_t     mtime      = 0;
        // exe identification — populated on cache hit, skips re-hash + sniff
        uint64_t    exe_xxh64  = 0;
        FM2K::Engine engine    = FM2K::Engine::FM2K;
        std::string clean_label;
        std::string packer_label;
        bool        is_clean   = false;
        // kgt stat — separate from exe so kgt edits invalidate independently.
        // kgt_present=false for FM95 .player-only games (no .kgt file).
        bool        kgt_present = false;
        uint64_t    kgt_size    = 0;
        int64_t     kgt_mtime   = 0;
        // Parsed kgt summary — replaces fm2k::ParseKgtSummary on hit
        fm2k::KgtSummary kgt;
    };

    static std::string GetCacheFilePath() {
        return GetConfigDir() + "games.cache";
    }

    // Stat helper. Returns false if the file doesn't exist or we can't
    // read its size/mtime. mtime uses SDL_PathInfo's modify_time (ns since
    // epoch, monotonic-ish across runs) so cache hits don't fight the
    // OS's clock skew on resumed-from-sleep machines.
    static bool StatFile(const std::string& path, uint64_t& size, int64_t& mtime) {
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(path.c_str(), &info)) return false;
        if (info.type != SDL_PATHTYPE_FILE) return false;
        size  = static_cast<uint64_t>(info.size);
        mtime = static_cast<int64_t>(info.modify_time);
        return true;
    }

    // Compute xxhash64 of an exe for clean-build identification. Pulls the
    // whole file into memory (clean FM2K/FM95 exes are < 2 MB; this trades
    // a buffered loop for simplicity). Returns 0 on any failure so callers
    // can treat 0 as "unhashable / skip".
    static uint64_t HashFileXXH64(const std::string& path) {
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
        if (!io) return 0;
        Sint64 sz = SDL_GetIOSize(io);
        if (sz <= 0 || sz > (64 * 1024 * 1024)) {  // 64 MB sanity cap
            SDL_CloseIO(io);
            return 0;
        }
        std::vector<unsigned char> buf(static_cast<size_t>(sz));
        size_t got = SDL_ReadIO(io, buf.data(), buf.size());
        SDL_CloseIO(io);
        if (got != buf.size()) return 0;
        return XXH64(buf.data(), buf.size(), /*seed=*/0);
    }

    // Known-clean exe registry — used by discovery to identify the exact
    // build of each detected game. Hashes computed via xxhash64 of the full
    // exe file. Add entries here when a new clean build is verified.
    //
    // To compute a hash for a new entry on a Linux/WSL host:
    //   gcc xxh_oneshot.c -o /tmp/xxh && /tmp/xxh path/to/clean.exe
    // (xxh_oneshot.c includes vendored/xxhash/xxhash.h with XXH_INLINE_ALL.)
    struct KnownExe {
        uint64_t      xxh64;
        FM2K::Engine  engine;
        const char*   label;
    };
    static constexpr KnownExe kKnownExes[] = {
        // FM2K builds
        {0x506FF9AB93D15134ULL, FM2K::Engine::FM2K, "WonderfulWorld v0.946"},
        // FM95 builds
        {0x36358AD6F9EC387BULL, FM2K::Engine::FM95, "Comic Party Wars (CPW)"},
    };

    // Look up an exe hash. Returns nullptr on miss.
    static const KnownExe* FindKnownExe(uint64_t hash) {
        for (const auto& k : kKnownExes) {
            if (k.xxh64 == hash) return &k;
        }
        return nullptr;
    }

    // Engine fingerprint — scans the first 256 KB of the exe for the engine
    // class strings. Reliable for clean FM2K/FM95 builds. Returns a confident
    // Engine when a marker is found, or std::nullopt to fall through to the
    // size heuristic.
    //   FM2K: "KGT2KGAME" window class, "2DKGT2K" or "kgt2k.INI" tags.
    //   FM95: "KGT95GAME" window class.
    static std::optional<FM2K::Engine> SniffEngineFromStrings(const std::string& path) {
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
        if (!io) return std::nullopt;
        std::vector<unsigned char> buf(256 * 1024);
        size_t got = SDL_ReadIO(io, buf.data(), buf.size());
        SDL_CloseIO(io);
        if (got < 1024) return std::nullopt;

        auto contains = [&](const char* needle) -> bool {
            size_t n = SDL_strlen(needle);
            if (n == 0 || n > got) return false;
            for (size_t i = 0; i + n <= got; ++i) {
                if (SDL_memcmp(buf.data() + i, needle, n) == 0) return true;
            }
            return false;
        };

        if (contains("KGT95GAME"))                       return FM2K::Engine::FM95;
        if (contains("KGT2KGAME") || contains("2DKGT2K") ||
            contains("kgt2k.INI"))                       return FM2K::Engine::FM2K;
        return std::nullopt;
    }

    // File-size heuristic for engine detection when neither the hash registry
    // nor the string sniff hits. Clean FM2K runtime binaries cluster around
    // 1.1–1.7 MB (.text + .rsrc + embedded resource sprites varying per
    // build). Clean FM95 binaries are much smaller (~400 KB) because the
    // prototype shipped with less code and no sprite cache. Anything outside
    // these bands is probably packed or unrelated.
    static FM2K::Engine GuessEngineFromSize(uint64_t size_bytes) {
        if (size_bytes <= 600 * 1024)                    return FM2K::Engine::FM95;
        return FM2K::Engine::FM2K;  // default — covers the 1.1-1.7 MB FM2K cluster
    }

    // Detect known PE packers by walking the section table. Returns a label
    // ("Enigma", "UPX", "MoleBox", "ASPack", "Themida", "PECompact") when a
    // packer signature is found, or "" for clean PEs. This is the actual
    // basis for warning the user — same-size unpacked-FM2K-engine builds
    // (SCWU, vanpri, etc.) just have different hashes, NOT different
    // structure, so they should NOT be flagged.
    //
    // Reads only the PE header + section table (typically < 4 KB), so it's
    // cheap to run on every discovered exe.
    static std::string DetectPackerFromPE(const std::string& path) {
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
        if (!io) return "";

        // Pull the first 4 KB; section table on a 32-bit FM2K exe sits at
        // ~0x180-0x300 — well inside this window.
        unsigned char hdr[4096] = {};
        size_t got = SDL_ReadIO(io, hdr, sizeof(hdr));
        SDL_CloseIO(io);
        if (got < 0x40) return "";

        // DOS signature 'MZ', then e_lfanew at 0x3C points to the PE header.
        if (hdr[0] != 'M' || hdr[1] != 'Z') return "";
        uint32_t pe_off = *reinterpret_cast<uint32_t*>(hdr + 0x3C);
        if (pe_off + 24 + 96 >= got) return "";  // need PE+COFF+OptHdr min
        if (hdr[pe_off] != 'P' || hdr[pe_off+1] != 'E') return "";

        // COFF header at pe_off+4: NumberOfSections at +2, SizeOfOptionalHeader at +16.
        uint16_t num_sections    = *reinterpret_cast<uint16_t*>(hdr + pe_off + 6);
        uint16_t opt_header_size = *reinterpret_cast<uint16_t*>(hdr + pe_off + 20);
        if (num_sections == 0 || num_sections > 96) return "";

        // Section table follows the optional header, 40 bytes per entry.
        uint32_t sec_off = pe_off + 24 + opt_header_size;
        struct PackerSig { const char* needle; const char* label; };
        // Match against the 8-byte section name (null-padded). We compare
        // case-insensitively against the start of each section name.
        static constexpr PackerSig kSigs[] = {
            {".enigma",  "Enigma Protector"},
            {".themida", "Themida"},
            {".taggant", "VMProtect"},  // VMProtect appends a taggant section
            {"UPX0",     "UPX"},
            {"UPX1",     "UPX"},
            {".aspack",  "ASPack"},
            {".adata",   "ASPack"},
            {".molebox", "MoleBox"},
            {".boom",    "MoleBox"},
            {"MoleBox",  "MoleBox"},
            {".pecompact","PECompact"},
            {"PEC2",     "PECompact"},
            {".asprotec","ASProtect"},
            {".nsp",     "NsPack"},
            {".vmp",     "VMProtect"},
            {".petite",  "Petite"},
            {".mpress",  "MPRESS"},
            {".fsg",     "FSG"},
            {".mew",     "MEW"},
        };

        for (uint16_t i = 0; i < num_sections; ++i) {
            uint32_t row = sec_off + i * 40;
            if (row + 8 >= got) break;
            const char* name = reinterpret_cast<const char*>(hdr + row);
            // Section names are 8-byte null-padded; compare prefixes case-insensitively.
            for (const auto& sig : kSigs) {
                size_t n = SDL_strlen(sig.needle);
                if (n > 8) n = 8;
                if (SDL_strncasecmp(name, sig.needle, n) == 0) {
                    return sig.label;
                }
            }
        }
        return "";
    }

    // UTF-8 ↔ wide via Win32 so std::filesystem doesn't go through the
    // system ANSI codepage (CP1252 on most non-Japanese installs),
    // which silently rewrites unrepresentable codepoints — full-width
    // forms ＣＰＷ (U+FF23/FF30/FF37) become '_' or '?' on the trip
    // through path::string(). Use wide internally and convert at the
    // boundaries so the launcher renders / caches the original bytes.
    static std::wstring Utf8ToWide_(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                    nullptr, 0);
        if (n <= 0) return {};
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                            w.data(), n);
        return w;
    }
    static std::string WideToUtf8_(const std::wstring& w) {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                            s.data(), n, nullptr, nullptr);
        return s;
    }

    static std::string CanonicalizePath(const std::string& p) {
        std::error_code ec;
        std::filesystem::path fs_p = Utf8ToWide_(p);
        std::filesystem::path canon = std::filesystem::weakly_canonical(fs_p, ec);
        if (ec) {
            std::filesystem::path abs = std::filesystem::absolute(fs_p, ec);
            if (ec) return p;
            return WideToUtf8_(abs.generic_wstring());
        }
        return WideToUtf8_(canon.generic_wstring());
    }

    // ── Binary cache I/O helpers ─────────────────────────────────────
    // Native little-endian on Windows x86 — the cache is local to the user
    // and never moves between machines, so endian conversion would be
    // overhead with no payoff.

    static void WriteU8 (std::ofstream& o, uint8_t  v) { o.write((const char*)&v, 1); }
    static void WriteU32(std::ofstream& o, uint32_t v) { o.write((const char*)&v, 4); }
    static void WriteU64(std::ofstream& o, uint64_t v) { o.write((const char*)&v, 8); }
    static void WriteI64(std::ofstream& o, int64_t  v) { o.write((const char*)&v, 8); }
    static void WriteStr(std::ofstream& o, const std::string& s) {
        WriteU32(o, (uint32_t)s.size());
        if (!s.empty()) o.write(s.data(), (std::streamsize)s.size());
    }

    static bool ReadU8 (std::ifstream& i, uint8_t&  v) { return (bool)i.read((char*)&v, 1); }
    static bool ReadU32(std::ifstream& i, uint32_t& v) { return (bool)i.read((char*)&v, 4); }
    static bool ReadU64(std::ifstream& i, uint64_t& v) { return (bool)i.read((char*)&v, 8); }
    static bool ReadI64(std::ifstream& i, int64_t&  v) { return (bool)i.read((char*)&v, 8); }
    static bool ReadStr(std::ifstream& i, std::string& s, uint32_t cap = 16 * 1024 * 1024) {
        uint32_t n = 0;
        if (!ReadU32(i, n)) return false;
        if (n > cap) return false;            // sanity cap — refuse > 16 MB strings
        s.assign(n, '\0');
        if (n == 0) return true;
        return (bool)i.read(s.data(), (std::streamsize)n);
    }

    static constexpr char     kCacheMagic[4] = {'F','M','2','K'};
    static constexpr uint32_t kCacheVersion  = 2;

    void SaveGameCache(const std::vector<FM2K::FM2KGameInfo>& games) {
        // Atomic-ish: write to .tmp then rename, so a crash mid-write
        // doesn't leave a half-cooked cache that fails the magic check
        // on the next launch (cosmetic — we'd just rebuild — but the
        // rename is cheap insurance).
        std::string final_path = GetCacheFilePath();
        std::string tmp_path   = final_path + ".tmp";
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to write game cache");
            return;
        }

        out.write(kCacheMagic, 4);
        WriteU32(out, kCacheVersion);
        WriteU32(out, (uint32_t)games.size());

        for (const auto& g : games) {
            uint64_t exe_size = 0;
            int64_t  exe_mtime = 0;
            StatFile(g.exe_path, exe_size, exe_mtime);

            WriteStr(out, g.exe_path);
            WriteStr(out, g.dll_path);
            WriteU64(out, exe_size);
            WriteI64(out, exe_mtime);
            WriteU64(out, g.xxh64);
            WriteU32(out, (uint32_t)g.engine);
            WriteStr(out, g.clean_label);
            WriteStr(out, g.packer_label);
            WriteU8 (out, g.is_clean ? 1 : 0);

            // KGT stat: only meaningful when the game has a .kgt file.
            // FM2KGameInfo::dll_path stores the .kgt path (legacy field name);
            // empty for FM95 .player-only fallback.
            const bool kgt_present = !g.dll_path.empty();
            WriteU8(out, kgt_present ? 1 : 0);
            if (kgt_present) {
                uint64_t kgt_size = 0;
                int64_t  kgt_mtime = 0;
                StatFile(g.dll_path, kgt_size, kgt_mtime);
                WriteU64(out, kgt_size);
                WriteI64(out, kgt_mtime);
            } else {
                WriteU64(out, 0);
                WriteI64(out, 0);
            }

            // Parsed kgt summary
            WriteU8 (out, g.kgt.valid ? 1 : 0);
            WriteStr(out, g.kgt.project_name);
            WriteU32(out, (uint32_t)g.kgt.player_names.size());
            for (const auto& n : g.kgt.player_names) WriteStr(out, n);
            WriteU32(out, (uint32_t)g.kgt.stage_names.size());
            for (const auto& n : g.kgt.stage_names) WriteStr(out, n);
            WriteU32(out, (uint32_t)g.kgt.demo_names.size());
            for (const auto& n : g.kgt.demo_names) WriteStr(out, n);
        }
        out.close();

        // Best-effort atomic replace.
        std::error_code ec;
        std::filesystem::rename(std::filesystem::u8path(tmp_path),
                                std::filesystem::u8path(final_path), ec);
        if (ec) {
            // Rename failed (e.g. cross-device, locked). Fall back to a
            // remove+rename so we still end up with a valid cache.
            std::filesystem::remove(std::filesystem::u8path(final_path), ec);
            std::filesystem::rename(std::filesystem::u8path(tmp_path),
                                    std::filesystem::u8path(final_path), ec);
        }
    }

    // Load the on-disk cache as a map keyed by canonical exe path. The
    // discovery walker uses this to skip ALL per-file work on unchanged
    // entries (xxh64, kgt parse, engine sniff, packer sniff).
    std::unordered_map<std::string, GameCacheEntry> LoadGameCacheMap() {
        std::unordered_map<std::string, GameCacheEntry> map;
        std::ifstream in(GetCacheFilePath(), std::ios::binary);
        if (!in) return map;

        char magic[4] = {0};
        if (!in.read(magic, 4) ||
            magic[0] != kCacheMagic[0] || magic[1] != kCacheMagic[1] ||
            magic[2] != kCacheMagic[2] || magic[3] != kCacheMagic[3]) {
            // Old text-format or unknown: treat as no cache. Silently
            // ignored — next save overwrites with the new binary format.
            return map;
        }
        uint32_t version = 0;
        if (!ReadU32(in, version) || version != kCacheVersion) {
            return map;  // future format — rebuild
        }
        uint32_t entry_count = 0;
        if (!ReadU32(in, entry_count) || entry_count > 100000) {
            return map;  // sanity cap — refuse pathological counts
        }

        for (uint32_t i = 0; i < entry_count; ++i) {
            GameCacheEntry e;
            uint8_t  is_clean_b = 0;
            uint8_t  kgt_present_b = 0;
            uint8_t  kgt_valid_b = 0;
            uint32_t engine_u32 = 0;

            if (!ReadStr(in, e.exe_path))    return map;
            if (!ReadStr(in, e.dll_path))    return map;
            if (!ReadU64(in, e.size))        return map;
            if (!ReadI64(in, e.mtime))       return map;
            if (!ReadU64(in, e.exe_xxh64))   return map;
            if (!ReadU32(in, engine_u32))    return map;
            if (!ReadStr(in, e.clean_label)) return map;
            if (!ReadStr(in, e.packer_label))return map;
            if (!ReadU8 (in, is_clean_b))    return map;
            if (!ReadU8 (in, kgt_present_b)) return map;
            if (!ReadU64(in, e.kgt_size))    return map;
            if (!ReadI64(in, e.kgt_mtime))   return map;
            if (!ReadU8 (in, kgt_valid_b))   return map;

            e.engine      = (FM2K::Engine)engine_u32;
            e.is_clean    = (is_clean_b != 0);
            e.kgt_present = (kgt_present_b != 0);
            e.kgt.valid   = (kgt_valid_b != 0);

            if (!ReadStr(in, e.kgt.project_name)) return map;

            uint32_t pcount = 0;
            if (!ReadU32(in, pcount) || pcount > 1024) return map;
            e.kgt.player_names.resize(pcount);
            for (uint32_t j = 0; j < pcount; ++j) {
                if (!ReadStr(in, e.kgt.player_names[j])) return map;
            }
            uint32_t scount = 0;
            if (!ReadU32(in, scount) || scount > 1024) return map;
            e.kgt.stage_names.resize(scount);
            for (uint32_t j = 0; j < scount; ++j) {
                if (!ReadStr(in, e.kgt.stage_names[j])) return map;
            }
            uint32_t dcount = 0;
            if (!ReadU32(in, dcount) || dcount > 1024) return map;
            e.kgt.demo_names.resize(dcount);
            for (uint32_t j = 0; j < dcount; ++j) {
                if (!ReadStr(in, e.kgt.demo_names[j])) return map;
            }

            std::string key = CanonicalizePath(e.exe_path);
            map.emplace(std::move(key), std::move(e));
        }
        return map;
    }

    // Cache-first UI seed. Loads every cached game's FULL identification
    // (xxh64, engine, clean/packer labels, parsed kgt summary) so the UI
    // shows a populated games list — including character/stage dropdowns —
    // BEFORE the async directory walk runs. Net effect: the launcher feels
    // instant on warm starts; the background walk only matters when the
    // user has actually added or removed a game since the last run.
    //
    // Cheap existence check per entry — skip games whose exe was deleted
    // since the cache was written. Stat mismatches (rebuild, kgt edit)
    // aren't filtered here; the async pass picks those up and updates
    // them transparently.
    std::vector<FM2K::FM2KGameInfo> LoadGameCache() {
        std::vector<FM2K::FM2KGameInfo> cached;
        auto map = LoadGameCacheMap();
        cached.reserve(map.size());
        for (auto& kv : map) {
            auto& e = kv.second;  // moved-from below
            if (!SDL_GetPathInfo(e.exe_path.c_str(), nullptr)) continue;
            if (e.kgt_present && !e.dll_path.empty() &&
                !SDL_GetPathInfo(e.dll_path.c_str(), nullptr)) continue;

            FM2K::FM2KGameInfo game;
            game.exe_path     = std::move(e.exe_path);
            game.dll_path     = std::move(e.dll_path);
            game.is_host      = true;
            game.process_id   = 0;
            game.engine       = e.engine;
            game.is_clean     = e.is_clean;
            game.clean_label  = std::move(e.clean_label);
            game.packer_label = std::move(e.packer_label);
            game.xxh64        = e.exe_xxh64;
            game.kgt          = std::move(e.kgt);
            cached.push_back(std::move(game));
        }
        return cached;
    }

    // Helper to normalize paths for SDL (convert backslashes to forward slashes)
    inline std::string NormalizePath(std::string path) {
        char* normalized = static_cast<char*>(SDL_malloc(SDL_strlen(path.c_str()) + 1));
        if (!normalized) return path;
        
        SDL_strlcpy(normalized, path.c_str(), SDL_strlen(path.c_str()) + 1);
        
        // Special handling for Windows drive letters (C:\ etc)
        bool has_drive_letter = SDL_strlen(normalized) >= 2 && 
                              normalized[1] == ':' &&
                              ((normalized[0] >= 'A' && normalized[0] <= 'Z') ||
                               (normalized[0] >= 'a' && normalized[0] <= 'z'));
        
        // Convert all backslashes to forward slashes, except after drive letter
        for (size_t i = (has_drive_letter ? 2 : 0); i < SDL_strlen(normalized); ++i) {
            if (normalized[i] == '\\') normalized[i] = '/';
        }
        
        // Remove any double slashes, but preserve network paths
        char* src = normalized;
        char* dst = normalized;
        bool last_was_slash = false;
        bool is_network_path = SDL_strlen(normalized) >= 2 && 
                             normalized[0] == '/' && normalized[1] == '/';
        
        if (is_network_path) {
            // Copy first two slashes for network paths
            *dst++ = *src++;
            *dst++ = *src++;
        }
        
        while (*src) {
            if (*src == '/') {
                if (!last_was_slash) {
                    *dst++ = *src;
                }
                last_was_slash = true;
            } else {
                *dst++ = *src;
                last_was_slash = false;
            }
            src++;
        }
        *dst = '\0';
        
        std::string result(normalized);
        SDL_free(normalized);
        return result;
    }

    // Recursively search for files that match the provided extension
    inline std::vector<std::string> FindFilesWithExtensionRecursive(const std::string& directory,
                                                                    const std::string& extension) {
        std::vector<std::string> files;
        std::string normalized_dir = NormalizePath(directory);
        
        int count = 0;
        char* pattern = static_cast<char*>(SDL_malloc(SDL_strlen(extension.c_str()) + 2));
        if (!pattern) return files;
        
        SDL_snprintf(pattern, SDL_strlen(extension.c_str()) + 2, "*%s", extension.c_str());
        char **list = SDL_GlobDirectory(normalized_dir.c_str(), pattern, /*flags=*/0, &count);
        SDL_free(pattern);

        if (list) {
            for (int i = 0; i < count; ++i) {
                if (list[i]) files.emplace_back(list[i]);
            }
            SDL_free(list);
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "FindFilesWithExtensionRecursive: found %d '%s' under %s", 
                        (int)files.size(), extension.c_str(), normalized_dir.c_str());
            return files;
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_GlobDirectory failed for %s: %s", 
                    normalized_dir.c_str(), SDL_GetError());
        return files;
    }
}

// SDL Callback Implementation
extern "C" {

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    // Hide the console window for release. The launcher EXE is currently
    // console-subsystem (so it gets a console handle inherited by the
    // game subprocesses we spawn) but the user-facing window is the SDL
    // ImGui app — the black console alongside is purely visual noise +
    // the synchronous WriteFile pacing on a Windows console actively
    // lags printf-heavy code paths.
    //
    // Strategy: keep the console subsystem (so child processes can
    // inherit a usable stdout/stderr if needed) but hide the WINDOW
    // immediately. Then redirect our own stdout/stderr to NUL so any
    // legacy printf still goes somewhere safe instead of trying to
    // write to a hidden console (which can wedge with SetConsoleMode
    // edge cases). FM2K_DEV_MODE=1 keeps the console visible for
    // diagnostic runs.
    {
        const char* dev = std::getenv("FM2K_DEV_MODE");
        const bool show_console = (dev && dev[0] == '1' && dev[1] == '\0');
        if (!show_console) {
            if (HWND con = GetConsoleWindow()) {
                ShowWindow(con, SW_HIDE);
            }
            // Detach our own stdio from the (now hidden) console. Child
            // processes still have their own handles via inheritance —
            // this only affects the launcher's printf/cout. Redirect to
            // NUL so any leftover prints don't block.
            FILE* dummy = nullptr;
            freopen_s(&dummy, "NUL", "w", stdout);
            freopen_s(&dummy, "NUL", "w", stderr);
        } else {
            SetConsoleTitleA("FM2K Rollback Launcher (DEV)");
        }
    }
    // Rename the console window we get for the console-subsystem EXE.
    // Default title is the full EXE path or, on some launches, the
    // MinGW-w64 toolchain string ("POSIX WinThreads") inherited from
    // the runtime. Override with something meaningful (only visible
    // when FM2K_DEV_MODE=1 keeps the console window up).
    SetConsoleTitleA("FM2K Rollback Launcher");

    // Pin the AppUserModelID for this process. Without an explicit AUMID
    // Windows derives one from the EXE path and caches the displayed
    // name from whichever VERSIONINFO it sees first — and once cached,
    // toasts (Action Center), the taskbar grouping, and the "right-click
    // → app name" surface keep showing the cached entry even after we
    // ship a fixed VERSIONINFO. Setting our own AUMID gives Windows a
    // stable identity it associates with our PE's actual FileDescription
    // ("FM2K Rollback Launcher") instead of the libwinpthread-derived
    // legacy one. Loaded dynamically because the symbol is in
    // shobjidl_core.h which not every MinGW SDK ships.
    {
        using SetAumidFn = HRESULT (WINAPI*)(PCWSTR);
        if (HMODULE sh = GetModuleHandleW(L"shell32.dll")) {
            auto SetAumid = reinterpret_cast<SetAumidFn>(
                GetProcAddress(sh, "SetCurrentProcessExplicitAppUserModelID"));
            if (SetAumid) {
                SetAumid(L"FM2K.Rollback.Launcher");
            }
        }
    }

    std::cout << "=== FM2K Rollback Launcher ===\n";
    std::cout << "Initializing with SDL callbacks...\n\n";
    
    // Parse command line arguments for backward compatibility
    NetworkConfig config;
    bool direct_mode = false;
    bool spectate_mode = false;
    bool stress_mode_cli = false;        // --stress: auto-launches stress determinism test on first game in scan
    std::string stress_game_filter;       // --stress <name>: filter discovered games by substring (case-insensitive)
    bool offline_mode_cli = false;       // --offline: auto-launches the pure FM2K_TRUE_OFFLINE native path (no rollback) for perf profiling
    std::string offline_game_filter;      // --offline <name|path>: substring filter or direct .exe path
    std::string direct_game_filter;       // --host/--connect <name-or-path>: pick a specific game instead of "first discovered"
    std::string spectate_target_addr;     // "host_ip:host_port" for --spectate
    std::string spectate_join_mode = "current";  // --spectate-mode {current,full}; default flipped back to "current" 2026-05-13 (v0.2.42 Phases C/D/E)
    // CLI --spectate has no hub context — caller picks via
    // --spectate-session-kind {menu,css,battle}. Default "battle"
    // preserves prior behavior (always /F boot-to-battle).
    std::string spectate_session_kind = "battle";
    std::string replay_file_path;         // "--replay <path>" — offline .fm2krep playback
    bool upnp_test_cli = false;           // --upnp-test: discover->map->report->unmap then exit (router validation)
    uint16_t upnp_test_port = 7000;       // --upnp-test [port]: UDP port to map for the test
    bool nat_test_cli = false;            // --nat-test <hub_host> [port]: run only the dual STUN classify probe then exit
    std::string nat_test_host;            // --nat-test <hub_host>: hub to probe (UDP-STUN at :7711, classify at :7714)
    uint16_t nat_test_port = 7000;        // --nat-test <hub_host> [port]: local UDP port to bind for the probe

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" || arg == "-h") {
            config.is_host = true;
            direct_mode = true;
            // Optional next arg: game-name substring OR absolute path to
            // game .exe (same convention as --stress). Picks the specific
            // game instead of "first discovered" — the test harness needs
            // this to target WonderfulWorld instead of whichever game
            // ends up alphabetically first in the launcher registry.
            if (i + 1 < argc && argv[i+1][0] != '-') {
                direct_game_filter = argv[++i];
            }
        } else if (arg == "--connect" || arg == "-c") {
            if (i + 1 < argc) {
                config.remote_address = argv[++i];
                config.is_host = false;
                direct_mode = true;
                // Optional second arg: game filter, same as --host.
                if (i + 1 < argc && argv[i+1][0] != '-') {
                    direct_game_filter = argv[++i];
                }
            } else {
                std::cerr << "Error: --connect requires an address\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--spectate-mode") {
            // --spectate-mode {current,full}
            //   current = CURRENT_MATCH (default; CCCaster-style snapshot join)
            //   full    = FULL_SESSION  (replay from frame 0)
            if (i + 1 < argc) {
                std::string m = argv[++i];
                if (m == "current" || m == "full") {
                    spectate_join_mode = m;
                } else {
                    std::cerr << "Error: --spectate-mode must be 'current' or 'full', got: " << m << "\n";
                    return SDL_APP_FAILURE;
                }
            } else {
                std::cerr << "Error: --spectate-mode requires {current,full}\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--spectate-session-kind") {
            // --spectate-session-kind {menu,css,battle}
            // Override the host's session_kind for CLI --spectate (no
            // hub to read it from). "battle" sets FM2K_BOOT_TO_BATTLE
            // for instant join; "menu"/"css" walks the title→CSS path.
            if (i + 1 < argc) {
                std::string k = argv[++i];
                if (k == "menu" || k == "css" || k == "battle") {
                    spectate_session_kind = k;
                } else {
                    std::cerr << "Error: --spectate-session-kind must be "
                                 "{menu,css,battle}, got: " << k << "\n";
                    return SDL_APP_FAILURE;
                }
            } else {
                std::cerr << "Error: --spectate-session-kind requires "
                             "{menu,css,battle}\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--replay") {
            // --replay <path-to-.fm2krep-or-.fm2kset>
            // Launches the game as a spectator instance with no network
            // connection; the hook reads FM2K_REPLAY_FILE on init and
            // SpectatorNode_LoadSessionFile populates pb_queue from the
            // file. Trampoline's RunSpectatorTick consumes events and
            // drives the sim forward, identical to a live spectator.
            if (i + 1 < argc) {
                replay_file_path = argv[++i];
            } else {
                std::cerr << "Error: --replay requires <path>\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--spectate" || arg == "--spec") {
            // Spectate a remote host: --spectate <ip:port>
            // Skips netplay handshake and stands the game up as a passive
            // viewer dialing the host's spectator listener. Matches the UI
            // path's LaunchRemoteSpectator + the FM2K_SPECTATOR_MODE=1 hook
            // gate.
            if (i + 1 < argc) {
                spectate_target_addr = argv[++i];
                spectate_mode = true;
            } else {
                std::cerr << "Error: --spectate requires <host_ip:host_port>\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--port" || arg == "-p") {
            if (i + 1 < argc) {
                config.local_port = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: --port requires a port number\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--remote" || arg == "-r") {
            // Override the remote peer address regardless of host/guest
            // role. --connect already sets remote_address for the guest;
            // --host leaves it at the placeholder (the hook then learns
            // the peer from the first inbound HELLO). --remote lets the
            // HOST be pointed at a specific peer/blackhole too, which the
            // relay self-test needs: pointing both peers at a TEST-NET-1
            // blackhole makes the direct punch fail on BOTH sides so the
            // hub relay engages. StartOnlineSession only clears the host
            // remote when it equals the "127.0.0.1:7001" placeholder, so
            // any other value (e.g. 192.0.2.1:6001) flows through to the
            // hook's StartPunch.
            if (i + 1 < argc) {
                config.remote_address = argv[++i];
            } else {
                std::cerr << "Error: --remote requires an address\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--delay" || arg == "-d") {
            if (i + 1 < argc) {
                config.input_delay = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: --delay requires a frame count\n";
                return SDL_APP_FAILURE;
            }
        } else if (arg == "--games") {
            if (i + 1 < argc) {
                // Treat --games as "set the games-roots list to a single
                // path" for parity with the legacy command line. Users who
                // want multiple roots can configure them in the UI.
                Utils::SaveGamesRootPaths({ argv[++i] });
            }
        } else if (arg == "--stress") {
            // Auto-launches GekkoStressSession determinism test on the
            // first discovered game (or the first match of a
            // case-insensitive substring filter passed as the next arg)
            // and exits the launcher when the test game terminates.
            // Forces a rollback every check_distance=10 frames and
            // fires GekkoDesyncDetected on any forward-vs-replay
            // mismatch — exactly the test we need to validate
            // Phase F (#23) fixes.
            stress_mode_cli = true;
            // Optional next arg: game-name substring (e.g. "wanwan"
            // or "pkmncc"). The first arg that DOESN'T start with `--`
            // is the filter; lets us pick a specific known-active game
            // instead of "first alphabetical" (HHRTFG idles too much
            // for the RNG-determinism check to fire).
            if (i + 1 < argc && argv[i+1][0] != '-') {
                stress_game_filter = argv[++i];
            }
        } else if (arg == "--offline") {
            // Auto-launches the pure offline native path (FM2K_TRUE_OFFLINE)
            // on a matched game and skips the launcher UI -- the path Yamada's
            // Robot Heroes slowdown lives on. Unlike --stress this runs NO
            // GekkoNet/rollback, so the [OFFLINE-SECT]/[FRAMETIME] perf
            // instruments (FM2K_PERF_PROFILE=1) measure the real offline
            // per-frame cost. Optional next arg = game substring or direct
            // .exe path (same convention as --stress).
            offline_mode_cli = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                offline_game_filter = argv[++i];
            }
        } else if (arg == "--upnp-test") {
            // Manual router validation for the Phase 1 UPnP port mapper.
            // Runs PortMapper::StartAsync on a test port, waits a few
            // seconds for the off-thread discovery+map to land, prints the
            // full Status, tears the mapping down, and exits -- without
            // ever spawning the launcher UI or a game. This is how the user
            // checks UPnP against their real router. Optional next arg = the
            // UDP port to map (default 7000), so the user can match the port
            // their game actually binds.
            upnp_test_cli = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                upnp_test_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            }
        } else if (arg == "--nat-test") {
            // Manual / harness validation for the Phase 2a NAT classifier.
            // Runs ONLY fm2k::LauncherStunClassify against a given hub
            // (UDP-STUN on :7711, classification reflector on :7714),
            // prints nat_type + the two reflexive ports, and exits -- no
            // launcher UI, no game, no WS. Drives the same probe the lobby
            // uses at the Connected event. On a local hub (127.0.0.1) this
            // expects "cone" (same external port from both responders).
            // Args: --nat-test <hub_host> [local_port]. local_port defaults
            // to 7000 (the game's default bind).
            nat_test_cli = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                nat_test_host = argv[++i];
            }
            if (i + 1 < argc && argv[i+1][0] != '-') {
                nat_test_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            }
        }
    }

    // --upnp-test: self-contained UPnP probe, runs before any launcher /
    // SDL window / game setup and exits the process. PortMapper uses
    // SDL_Log* internally which works against SDL's default log output
    // without SDL_Init, so we get the discovery/IGD/error lines for free.
    if (upnp_test_cli) {
        std::cout << "[upnp-test] mapping UDP port " << upnp_test_port
                  << " via UPnP (up to ~6s for SSDP + map)...\n";
        // Winsock isn't up yet in this early-exit path (SDL_Init, which
        // calls WSAStartup, hasn't run). miniupnpc creates raw sockets for
        // the SSDP discover, so initialize Winsock ourselves first --
        // otherwise upnpDiscover fails with WSANOTINITIALISED (10093) and
        // we'd report a false NoIgd. In normal launcher operation the
        // mapper runs at the hub Connected event, long after WSAStartup.
        WSADATA wsa{};
        const bool wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        if (!wsa_ok) {
            std::cout << "[upnp-test] WARNING: WSAStartup failed -- "
                         "discovery may not work\n";
        }
        fm2k::PortMapper mapper;
        mapper.StartAsync(upnp_test_port);
        // Poll the status until it leaves Discovering or the ~6s budget
        // elapses (SSDP is a 2s budget + description fetch + AddPortMapping
        // round-trips; 6s comfortably covers a responsive home router).
        fm2k::PortMapper::Status st;
        for (int waited_ms = 0; waited_ms < 6000; waited_ms += 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            st = mapper.Snapshot();
            if (st.state != fm2k::PortMapper::State::Discovering &&
                st.state != fm2k::PortMapper::State::Idle) {
                break;
            }
        }
        st = mapper.Snapshot();
        auto state_name = [](fm2k::PortMapper::State s) -> const char* {
            switch (s) {
                case fm2k::PortMapper::State::Idle:        return "Idle";
                case fm2k::PortMapper::State::Discovering: return "Discovering";
                case fm2k::PortMapper::State::Mapped:      return "Mapped";
                case fm2k::PortMapper::State::NoIgd:       return "NoIgd";
                case fm2k::PortMapper::State::Failed:      return "Failed";
                case fm2k::PortMapper::State::Cgnat:       return "Cgnat";
            }
            return "?";
        };
        std::cout << "[upnp-test] result:\n"
                  << "  state        = " << state_name(st.state)   << "\n"
                  << "  backend      = " << st.backend             << "\n"
                  << "  ext_ip       = " << st.ext_ip              << "\n"
                  << "  ext_udp_port = " << st.ext_udp_port        << "\n"
                  << "  igd          = " << st.igd_desc            << "\n";
        mapper.Stop();
        if (wsa_ok) WSACleanup();
        std::cout << "[upnp-test] done (mapping torn down).\n";
        return SDL_APP_SUCCESS;
    }

    // --nat-test: self-contained NAT classification probe. Runs the exact
    // dual STUN probe the lobby uses (fm2k::LauncherStunClassify) against the
    // given hub, prints the verdict + reflexive ports, and exits. This is the
    // harness for the Phase 2a plumbing: point it at a local hub.py
    // (--host 127.0.0.1, which binds 7711-7714) and expect "cone" with
    // port_a == port_b. LauncherStunClassify calls WSAStartup internally, so
    // no Winsock bring-up is needed here (mirrors the launcher path, where it
    // runs at the hub Connected event with Winsock already up).
    if (nat_test_cli) {
        if (nat_test_host.empty()) {
            std::cout << "[nat-test] usage: --nat-test <hub_host> [local_port]\n";
            return SDL_APP_FAILURE;
        }
        // A synthetic user id so the probe doesn't early-out on empty; the
        // classification reflector (:7714) ignores it entirely and the
        // primary (:7711) just records a throwaway mapping for an unknown
        // user (harmless -- no such User exists on a fresh local hub).
        const std::string test_uid = "nat-test-harness";
        std::cout << "[nat-test] probing hub '" << nat_test_host
                  << "' (UDP-STUN :7711, classify :7714) from local port "
                  << nat_test_port << " ...\n";
        fm2k::NatClassifyResult r = fm2k::LauncherStunClassify(
            nat_test_port, nat_test_host, 7711, test_uid);
        std::cout << "[nat-test] result:\n"
                  << "  nat_type = " << r.nat_type << "\n"
                  << "  port_a   = " << r.port_a   << "  (reflected ext port from :7711)\n"
                  << "  port_b   = " << r.port_b   << "  (reflected ext port from :7714)\n";
        std::cout << "[nat-test] done.\n";
        return SDL_APP_SUCCESS;
    }

    // Create launcher instance
    g_launcher = std::make_unique<FM2KLauncher>();

    if (!g_launcher->Initialize()) {
        std::cerr << "Failed to initialize launcher\n";
        return SDL_APP_FAILURE;
    }

    // Replay mode (offline file playback). Skip UI, no network, no peer.
    // Launches a spectator instance with FM2K_REPLAY_FILE pointing at
    // the .fm2krep / .fm2kset; the hook's Netplay_InitAsSpectator reads
    // the env var at init and SpectatorNode_LoadSessionFile drains the
    // file's events into pb_queue.
    //
    // Game-exe lookup: the replay file is always written to
    // <game_dir>/replays/<timestamp>.fm2krep (see WriteCurrentBattleFile),
    // so game_dir is the replay's grandparent. We scan that directory
    // for the .exe directly — no dependency on the async discovery
    // thread, which hasn't populated yet at this point in startup.
    if (!replay_file_path.empty()) {
        std::error_code ec;
        std::filesystem::path replay_fs = std::filesystem::u8path(replay_file_path);
        std::filesystem::path canon = std::filesystem::weakly_canonical(replay_fs, ec);
        if (ec) canon = replay_fs;
        std::filesystem::path game_dir = canon.parent_path().parent_path();

        // FM2K games always ship with <project>.kgt next to <project>.exe.
        // Find the .kgt's stem and use it to pick the matching .exe — this
        // skips bundled helper binaries like antimicrox.exe that are 64-bit
        // and would fail injection.
        std::string game_exe_path;
        if (!game_dir.empty() && std::filesystem::is_directory(game_dir, ec)) {
            std::filesystem::path kgt_stem;
            for (const auto& entry : std::filesystem::directory_iterator(game_dir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                if (ext == ".kgt") {
                    kgt_stem = entry.path().stem();
                    break;
                }
            }
            if (!kgt_stem.empty()) {
                std::filesystem::path candidate = game_dir / kgt_stem;
                candidate += ".exe";
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    game_exe_path = candidate.string();
                }
            }
        }
        if (game_exe_path.empty()) {
            std::cerr << "Replay: could not locate game .exe under "
                      << game_dir.string() << "\n";
            return SDL_APP_FAILURE;
        }

        if (!g_launcher->LaunchReplayPlayer(game_exe_path, replay_file_path)) {
            std::cerr << "Replay: launch failed\n";
            return SDL_APP_FAILURE;
        }
        std::cout << "Replay mode: playing " << replay_file_path
                  << " (game=" << game_exe_path << ")\n";
        // Fall through to the launcher's headless main loop so the
        // replay-instance lifetime is managed normally.
    }

    // --stress mode: drive Phase F determinism test from the CLI so we
    // don't need to UI-click. Launches the first discovered game with
    // FM2K_STRESS_MODE=1; the hook creates a GekkoStressSession that
    // forces a rollback every check_distance=10 frames and fires
    // GekkoDesyncDetected on any forward-vs-replay mismatch. Watch
    // <game_dir>/logs/FM2K_P1_Debug.log for DESYNC events.
    if (stress_mode_cli) {
        // Direct-path bypass: if --stress <filter> looks like a literal
        // path to a .exe (contains a separator, ends in .exe, file
        // exists), skip the launcher.cfg-rooted DiscoverGames scan
        // entirely and just launch that EXE. Lets the replay-selftest
        // harness target a specific game without waiting on a
        // multi-thousand-file recursive scan of D:\Games\fm2k\.
        std::vector<FM2K::FM2KGameInfo> games;
        const bool is_direct_path = !stress_game_filter.empty()
            && (stress_game_filter.find('/')  != std::string::npos ||
                stress_game_filter.find('\\') != std::string::npos);
        if (is_direct_path) {
            FM2K::FM2KGameInfo info{};
            info.exe_path = stress_game_filter;
            // Other fields (engine, clean_label, etc) take their defaults
            // — discovery normally populates them but for a direct-path
            // launch the launcher only needs exe_path to drive
            // StartStressSession.
            games.push_back(info);
            stress_game_filter.clear();  // suppress the substring matcher below
        } else {
            // Force a SYNCHRONOUS scan here so we have a games list to
            // pick from. The launcher's async discovery thread will still
            // run later but won't change the selection we made.
            games = g_launcher->DiscoverGames();
        }
        if (games.empty()) {
            std::cerr << "No FM2K games found for --stress (scanned launcher.cfg roots)\n";
            return SDL_APP_FAILURE;
        }
        // Boot the game directly into battle so GekkoStressSession
        // actually exercises rollback during the test run. Without
        // these vars the game sits at title screen waiting for input
        // and the determinism check never fires.
        ::SetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE", "1");
        ::SetEnvironmentVariableA("FM2K_AUTO_TITLE_SKIP", "1");
        ::SetEnvironmentVariableA("FM2K_PARITY_AUTOPLAY", "1");
        // Phase F autonomous stress: keep mashing buttons during battle
        // so RNG-consuming actions (hits, scripts, projectiles) fire
        // and we exercise the active-gameplay code path that user-
        // reported desyncs cluster on. Idle stress passed 21k+ frames
        // clean. The --stress harness implies "actually try to break
        // things," not "let the engine idle."
        ::SetEnvironmentVariableA("FM2K_PARITY_AUTOPLAY_BATTLE", "1");
        // Force per-save full per-region CRC so the desync diagnostic
        // dump can attribute first-divergent region (vs the default
        // 1/sec throttle that shows everything as 0x0). Respect an
        // explicit FM2K_FULL_CRCS=0 from the environment so a perf run
        // (FM2K_PERF_PROFILE=1) can measure the TRUE production save cost
        // without the ~1.2ms/save diagnostic hash that stress mode adds.
        if (const char* fc = std::getenv("FM2K_FULL_CRCS"); !(fc && fc[0] == '0')) {
            ::SetEnvironmentVariableA("FM2K_FULL_CRCS", "1");
        }
        // Apply optional --stress <filter> game-name substring match
        // (case-insensitive). Default = first discovered.
        size_t pick = 0;
        if (!stress_game_filter.empty()) {
            auto lowercase = [](std::string s) {
                for (auto& c : s) c = (char)std::tolower((unsigned char)c);
                return s;
            };
            std::string needle = lowercase(stress_game_filter);
            bool found = false;
            for (size_t i = 0; i < games.size(); ++i) {
                if (lowercase(games[i].exe_path).find(needle) != std::string::npos) {
                    pick = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "--stress: no game matched filter '"
                          << stress_game_filter << "'; falling back to first\n";
            }
        }
        const auto& game_to_launch = games[pick];
        g_launcher->SetSelectedGame(game_to_launch);
        g_launcher->StartStressSession();
        g_launcher->SetState(LauncherState::InGame);
        std::cout << "Stress mode: GekkoStressSession started for "
                  << game_to_launch.exe_path
                  << " — watch logs/FM2K_P1_Debug.log\n";
    }

    if (offline_mode_cli) {
        // Mirror of the --stress resolution, but launches the pure offline
        // native path for perf profiling. Boots straight to battle (skips the
        // CSS, which crashes on some games like Robot Heroes) and idles --
        // NO autoplay-mash, so the per-frame cost is stage/object dominated
        // and stable across runs. Stage is pinned via FM2K_BTB_STAGE (passed
        // through from the environment); FM2K_PERF_PROFILE / FM2K_DUMP_STAGES
        // likewise flow through to the spawned game.
        std::vector<FM2K::FM2KGameInfo> games;
        const bool is_direct_path = !offline_game_filter.empty()
            && (offline_game_filter.find('/')  != std::string::npos ||
                offline_game_filter.find('\\') != std::string::npos);
        if (is_direct_path) {
            FM2K::FM2KGameInfo info{};
            info.exe_path = offline_game_filter;
            games.push_back(info);
            offline_game_filter.clear();
        } else {
            games = g_launcher->DiscoverGames();
        }
        if (games.empty()) {
            std::cerr << "No FM2K games found for --offline\n";
            return SDL_APP_FAILURE;
        }
        // --offline defaults to boot-straight-to-battle so perf profiling
        // lands in gameplay, but RESPECT an explicit FM2K_BOOT_TO_BATTLE
        // from the environment (a CSS test batch sets =0 to walk the normal
        // title->CSS path for character-select churn testing). Same opt-out
        // pattern as FM2K_PARITY_RECORD_PATH below. GetEnvironmentVariableA
        // returns 0 only when the var is unset; any explicit value wins.
        if (::GetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE", nullptr, 0) == 0) {
            ::SetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE", "1");
        }
        ::SetEnvironmentVariableA("FM2K_AUTO_TITLE_SKIP", "1");
        size_t pick = 0;
        if (!offline_game_filter.empty()) {
            auto lowercase = [](std::string s) {
                for (auto& c : s) c = (char)std::tolower((unsigned char)c);
                return s;
            };
            std::string needle = lowercase(offline_game_filter);
            for (size_t i = 0; i < games.size(); ++i) {
                if (lowercase(games[i].exe_path).find(needle) != std::string::npos) {
                    pick = i;
                    break;
                }
            }
        }
        g_launcher->SetSelectedGame(games[pick]);
        g_launcher->StartOfflineSession();
        g_launcher->SetState(LauncherState::InGame);
        std::cout << "Offline mode: native session started for "
                  << games[pick].exe_path
                  << " — watch logs/FM2K_P1_Debug.log [OFFLINE-SECT]/[FRAMETIME]\n";
    }

    // If direct mode, skip UI and go straight to game launch + network
    if (direct_mode || spectate_mode) {
        // Direct-path bypass: if --host/--connect <filter> is a literal
        // absolute path, build a one-element discovered-games list and
        // launch that — skips the registry-scan entirely (matches --stress
        // semantics for harness use). Otherwise treat as substring filter
        // against the discovered games list.
        const bool is_direct_path = !direct_game_filter.empty()
            && (direct_game_filter.find('/')  != std::string::npos ||
                direct_game_filter.find('\\') != std::string::npos);

        FM2K::FM2KGameInfo selected{};
        if (is_direct_path) {
            selected.exe_path = direct_game_filter;
        } else {
            if (g_launcher->GetDiscoveredGames().empty()) {
                std::cerr << "No FM2K games found for direct mode\n";
                return SDL_APP_FAILURE;
            }
            if (!direct_game_filter.empty()) {
                auto lowercase = [](std::string s) {
                    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
                    return s;
                };
                std::string needle = lowercase(direct_game_filter);
                const auto& games = g_launcher->GetDiscoveredGames();
                bool matched = false;
                for (size_t i = 0; i < games.size(); ++i) {
                    if (lowercase(games[i].exe_path).find(needle) != std::string::npos) {
                        selected = games[i];
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    std::cerr << "--host/--connect: no game matched filter '"
                              << direct_game_filter << "'; falling back to first\n";
                    selected = games[0];
                }
            } else {
                selected = g_launcher->GetDiscoveredGames()[0];
            }
        }
        const auto& game_to_launch = selected;

        // Manually set the selected game for the launcher
        g_launcher->SetSelectedGame(game_to_launch);

        if (spectate_mode) {
            // Parse "host_ip:host_port" target.
            const size_t colon = spectate_target_addr.find(':');
            if (colon == std::string::npos) {
                std::cerr << "Error: --spectate target must be <ip:port>, got: "
                          << spectate_target_addr << "\n";
                return SDL_APP_FAILURE;
            }
            const std::string host_ip = spectate_target_addr.substr(0, colon);
            const int host_port       = std::stoi(spectate_target_addr.substr(colon + 1));
            const int spectator_port  = (config.local_port > 0) ? config.local_port : 7702;

            // CLI --spectate has no hub context, so session_kind isn't
            // forwarded — assume "battle" (the typical e2e use case is
            // joining an already-running match). To test the CSS-walk
            // path from CLI, pass `--spectate-session-kind menu`.
            if (!g_launcher->LaunchRemoteSpectator(game_to_launch.exe_path,
                                                    spectator_port,
                                                    host_ip, host_port,
                                                    spectate_session_kind,
                                                    spectate_join_mode)) {
                std::cerr << "Spectate: launch failed\n";
                return SDL_APP_FAILURE;
            }
            std::cout << "Direct spectate mode: dialing " << host_ip << ":" << host_port
                      << " on local port " << spectator_port
                      << " (mode=" << spectate_join_mode << ")\n";
        } else {
            // Start regular host/client network session
            NetworkConfig online_config = config;
            online_config.session_mode = SessionMode::ONLINE;
            g_launcher->StartOnlineSession(online_config, config.is_host);
            std::cout << "Direct mode: game launched + network started\n";
        }

        g_launcher->SetState(LauncherState::InGame);
    }
    
    // Store launcher in appstate for other callbacks
    *appstate = g_launcher.get();
    
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    FM2KLauncher* launcher = static_cast<FM2KLauncher*>(appstate);
    if (!launcher) {
        return SDL_APP_FAILURE;
    }

    // Calculate delta time
    static auto last_time = std::chrono::steady_clock::now();
    auto current_time = std::chrono::steady_clock::now();
    float delta_time = std::chrono::duration<float>(current_time - last_time).count();
    last_time = current_time;

    // Idle/visibility throttle. Vsync is our framerate cap when the
    // window is on a real swap chain — but when the window is MINIMIZED
    // or HIDDEN, presents complete instantly (no real swap), and we'd
    // otherwise spin at thousands of fps redrawing an offscreen surface.
    // Same story when vsync fell back to software (RDP / headless / a
    // refused driver path): without a cap we burn cores.
    //
    // Symptom in the wild: users on Xeon E3 1230 v3 + GTX 1060 and on a
    // 3060 both reported ~17–22% CPU/GPU just sitting on the launcher.
    // Capping the unfocused/uncapped path to ~60 fps fixes that
    // without affecting the active-user experience.
    SDL_Window* w = launcher->GetWindow();
    const SDL_WindowFlags flags = w ? SDL_GetWindowFlags(w) : 0;
    const bool minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
    const bool hidden    = (flags & SDL_WINDOW_HIDDEN) != 0;
    const bool unfocused = !(flags & SDL_WINDOW_INPUT_FOCUS);

    if (minimized || hidden) {
        // Nothing visible — skip render entirely and sleep ~100 ms.
        // Events still pump via SDL_AppEvent so a restore wakes us up.
        SDL_Delay(100);
        return SDL_APP_CONTINUE;
    }

    launcher->Update(delta_time);
    launcher->Render();

    // Soft 60 fps cap when vsync isn't doing the limiting for us, or
    // when the window is unfocused (most users alt-tab between matches
    // and don't need 144Hz update rates on a static panel).
    static Uint64 last_present_ns = 0;
    const Uint64 now_ns = SDL_GetTicksNS();
    const Uint64 frame_target_ns =
        unfocused ? 33'333'333ULL  // ~30 fps when unfocused
                  : 16'666'666ULL; // ~60 fps focused fallback
    if (!launcher->IsVsyncAvailable() || unfocused) {
        if (last_present_ns != 0) {
            const Uint64 elapsed = now_ns - last_present_ns;
            if (elapsed < frame_target_ns) {
                SDL_DelayNS(frame_target_ns - elapsed);
            }
        }
    }
    last_present_ns = SDL_GetTicksNS();

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    FM2KLauncher* launcher = static_cast<FM2KLauncher*>(appstate);
    if (!launcher) {
        return SDL_APP_FAILURE;
    }

    // Gamepad hot-plug: refresh the binder's pad list so the input
    // bindings window (and the SOCD picker's "gamepad N" labels)
    // update without requiring the user to close & reopen the
    // launcher (Suicidal Muffin's bug report). The hook-side binder
    // gets the same treatment via a 1 s periodic refresh in
    // hooks.cpp + input.cpp, since events don't cross the process
    // boundary into the injected DLL's SDL context.
    if (event->type == SDL_EVENT_GAMEPAD_ADDED ||
        event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        FM2KInputBinder::RefreshGamepads();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SDL_AppEvent: gamepad %s — binder refreshed",
            event->type == SDL_EVENT_GAMEPAD_ADDED ? "ADDED" : "REMOVED");
    }

    // Let launcher handle the event
    launcher->HandleEvent(event);

    // Check for quit
    if (event->type == SDL_EVENT_QUIT) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL_EVENT_QUIT: Quitting application");
        return SDL_APP_SUCCESS;
    }

    // Note: async discovery completion is handled inside FM2KLauncher::HandleEvent.

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate SDL_UNUSED, SDL_AppResult result SDL_UNUSED) {
    std::cout << "Shutting down FM2K launcher...\n";
    
    if (g_launcher) {
        // Perform shutdown
        g_launcher->Shutdown();
        g_launcher.reset();
    }
    
    std::cout << "LauncherUI shutdown\n";
}

} // extern "C"

// FM2KLauncher Implementation
FM2KLauncher::FM2KLauncher() 
    : window_(nullptr)
    , renderer_(nullptr)
    , current_state_(LauncherState::GameSelection)
    , running_(true) {
    // Register the custom event type exactly once per process.
    if (g_event_discovery_complete == 0) {
        g_event_discovery_complete = SDL_RegisterEvents(1);
        if (g_event_discovery_complete == (Uint32)-1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to register discovery completion event: %s", SDL_GetError());
        }
    }

    discovery_thread_ = nullptr;
    discovery_in_progress_ = false;
    // Initialize multi-client testing
    client1_process_id_ = 0;
    client2_process_id_ = 0;
    
    // Initialize GekkoNet session management
    
    // Load saved games directories (if any) so they can be used before
    // Initialize() completes. Old single-string configs migrate naturally
    // because the persistence format is one path per line.
    games_root_paths_ = Utils::LoadGamesRootPaths();
}

FM2KLauncher::~FM2KLauncher() {
    Shutdown();
}

bool FM2KLauncher::Initialize() {
    // Set log priorities using SDL_SetLogPriority instead of SDL_LogSetPriority
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_DEBUG);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_ERROR, SDL_LOG_PRIORITY_DEBUG);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_INFO);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_INFO);
    
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initializing FM2K Launcher...");

    if (!InitializeSDL()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize SDL3: %s", SDL_GetError());
        return false;
    }
    
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize MinHook");
        return false;
    }
    
    // Create subsystems
    ui_ = std::make_unique<LauncherUI>();
    if (!ui_->Initialize(window_, renderer_)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize UI");
        return false;
    }
    
    // Connect UI callbacks to launcher logic
    ui_->on_game_selected = [this](const FM2K::FM2KGameInfo& game) {
        SetSelectedGame(game);
    };
    ui_->on_offline_session_start = [this]() {
        StartOfflineSession();
    };
    ui_->on_online_session_start = [this](const NetworkConfig& config) {
        StartOnlineSession(config, config.is_host);
    };
    ui_->on_stress_session_start = [this]() {
        StartStressSession();
    };
    ui_->on_session_stop = [this]() {
        StopSession();
    };
    ui_->on_spectator_punch_target = [this](const std::string& spec_udp_ip,
                                            int                spec_udp_port,
                                            int                spec_tcp_port,
                                            const std::string& spec_user_id) {
        // Hub forwarded a spectator's external UDP+TCP addr (we're the
        // host of an active match). Write into our running game instance's
        // shared mem so the hook's TickHostMaintenance polls the seq
        // bump and fires:
        //   * UDP heartbeat burst toward spec_udp_addr (existing — opens
        //     NAT for the spectator's first SPEC_JOIN_REQ replies),
        //   * TCP simultaneous-open punch toward spec_tcp_addr (new in
        //     v0.2.35 — opens NAT for inbound TCP from spec:tcp_port to
        //     our listener port, the data path the INPUT_BATCH stream
        //     actually rides).
        // spec_tcp_port = 0 sentinel for older spec clients that don't
        // know their own TCP listener port — host falls back to UDP-only
        // (no TCP punch).
        DWORD target_pid = 0;
        if (game_instance_ && game_instance_->IsRunning()) {
            target_pid = game_instance_->GetProcessId();
        } else if (client1_instance_ && client1_instance_->IsRunning()) {
            // Dev-mode dual-clients fallback — local-test spectator path.
            target_pid = client1_instance_->GetProcessId();
        }
        if (target_pid == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: spectator_incoming with no running game instance "
                "to deliver punch target to (addr=%s:%d/%d) — dropping",
                spec_udp_ip.c_str(), spec_udp_port, spec_tcp_port);
            return;
        }
        // Resolve dotted IPv4 -> network-byte-order u32 for StartPunch.
        IN_ADDR addr_bin{};
        if (inet_pton(AF_INET, spec_udp_ip.c_str(), &addr_bin) != 1 ||
            spec_udp_port <= 0 || spec_udp_port > 0xFFFF) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: spectator_incoming bad addr %s:%d — dropping",
                spec_udp_ip.c_str(), spec_udp_port);
            return;
        }
        const uint16_t tcp_port_u16 =
            (spec_tcp_port > 0 && spec_tcp_port <= 0xFFFF)
                ? (uint16_t)spec_tcp_port : 0u;
        const std::string mapping_name =
            "FM2K_SharedMem_" + std::to_string((unsigned)target_pid);
        HANDLE h = OpenFileMappingA(FILE_MAP_WRITE, FALSE,
                                    mapping_name.c_str());
        if (!h) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: spectator_incoming: OpenFileMapping('%s') failed: %lu",
                mapping_name.c_str(), GetLastError());
            return;
        }
        FM2KSharedMemData* shm = static_cast<FM2KSharedMemData*>(
            MapViewOfFile(h, FILE_MAP_WRITE, 0, 0,
                          sizeof(FM2KSharedMemData)));
        if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC) {
            shm->spectator_punch_ip_be    = addr_bin.S_un.S_addr;
            shm->spectator_punch_port     = (uint16_t)spec_udp_port;
            shm->spectator_punch_tcp_port = tcp_port_u16;
            // Phase 2c: also write spec_user_id (relay-mode addressing).
            // Truncate to fit (32-byte buffer, 31 chars + NUL). Empty
            // when the hub doesn't include spec_user_id (older hub);
            // hook treats absent user_id as "no relay routing" and
            // falls back to addr-only TCP behavior.
            std::memset(shm->spectator_punch_user_id, 0,
                        sizeof(shm->spectator_punch_user_id));
            const size_t uid_max = sizeof(shm->spectator_punch_user_id) - 1;
            const size_t uid_n   = std::min<size_t>(spec_user_id.size(), uid_max);
            if (uid_n > 0) {
                std::memcpy(shm->spectator_punch_user_id,
                            spec_user_id.data(), uid_n);
            }
            // Bump seq AFTER the addr writes — hook's poll reads addr
            // only when seq advances, so a torn write is harmless.
            shm->spectator_punch_seq  += 1;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Hub: queued spectator-punch target %s udp:%d tcp:%d "
                "user_id=%s to game pid %lu (seq=%u)",
                spec_udp_ip.c_str(), spec_udp_port, (int)tcp_port_u16,
                spec_user_id.empty() ? "(none)" : spec_user_id.c_str(),
                (unsigned long)target_pid,
                (unsigned)shm->spectator_punch_seq);
        }
        if (shm) UnmapViewOfFile(shm);
        CloseHandle(h);
    };

    // Phase 3: spec hub-relay inbound. Hub forwarded SpecDataHeader-
    // prefixed wire bytes for our spec game; write into its inbound
    // shared-mem ring. Lazy-open the ring keyed by game pid (similar
    // pattern to the outbound drain in Update). The hook's TickHealth
    // drains the ring and dispatches each Slot through HandleSpecData.
    ui_->on_spec_relay_bytes = [this](const std::vector<uint8_t>& bytes) {
        DWORD target_pid = 0;
        if (game_instance_ && game_instance_->IsRunning()) {
            target_pid = game_instance_->GetProcessId();
        } else if (client1_instance_ && client1_instance_->IsRunning()) {
            target_pid = client1_instance_->GetProcessId();
        }
        if (target_pid == 0) {
            // Game not running -- nothing to deliver to. Frames arriving
            // before spec game boots are normal at the very start; drop.
            return;
        }
        // Inbound ring cached as class member (spec_relay_in_ring_) so
        // the status pill in the menu bar can read its counters. Was
        // lambda-static; promotion required for menu-bar visibility.
        auto* in_ring_ptr =
            static_cast<fm2k::spec_relay::Ring*>(spec_relay_in_ring_);
        if (target_pid != spec_relay_in_pid_) {
            if (in_ring_ptr) {
                fm2k::spec_relay::Close(in_ring_ptr);
                in_ring_ptr = nullptr;
                spec_relay_in_ring_ = nullptr;
            }
            spec_relay_in_pid_ = target_pid;
        }
        // Retry open until success. Hook's mapping creation races our
        // first WS-binary delivery; the cache would otherwise stick at
        // nullptr if the first open happens before the hook is ready.
        if (!in_ring_ptr) {
            in_ring_ptr = fm2k::spec_relay::OpenInboundFor(target_pid);
            spec_relay_in_ring_ = in_ring_ptr;
            if (in_ring_ptr) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpecRelay: opened inbound ring for spec game pid %lu",
                    (unsigned long)target_pid);
            }
        }
        if (!in_ring_ptr) {
            // Mapping still not available (hook still booting, or hook
            // not in relay mode). Drop this WS frame; next one retries.
            return;
        }
        // The bytes are exactly the payload the hook hands to
        // SpectatorNode_HandleSpecData. Enqueue with TARGET_BROADCAST
        // (kind isn't load-bearing on the inbound side; we set it for
        // consistency) and zero header metadata.
        fm2k::spec_relay::Enqueue(
            in_ring_ptr,
            fm2k::spec_relay::TARGET_BROADCAST,
            /*spec_user_id=*/nullptr,
            /*spec_data_type=*/0,
            /*frame_count=*/0,
            /*spec_data_flags=*/0,
            bytes.data(), static_cast<uint32_t>(bytes.size()));
    };

    ui_->on_spectate_match = [this](const std::string& host_ip, int host_port,
                                    const std::string& session_kind,
                                    const std::string& spec_transport) {
        // Need an installed game to point the spectator at; reuse whatever
        // the launcher currently has selected.
        if (selected_game_.exe_path.empty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Spectate: no game selected — pick one before clicking Spectate");
            return;
        }
        // Phase 4: tell the user-facing log clearly which mode they're
        // about to enter. Relay-mode hosts route spec data through the
        // hub which costs ~30-50 ms extra latency but works behind any
        // NAT class. TCP-mode hosts use direct P2P (faster but blocked
        // by symmetric NAT). The auto-derivation already set the env;
        // this log is purely informational so testers know which path
        // is active.
        if (spec_transport == "relay") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spectate: host advertises spec_transport=relay -- spec "
                "will receive data via hub WS binary frames. Watch the "
                "RELAY pill in the menu bar for ring counters; drops "
                "(red) indicate snapshot or event corruption.");
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spectate: legacy P2P TCP mode (spec_transport=tcp). "
                "If this fails behind symmetric NAT, ask host to set "
                "FM2K_SPEC_TRANSPORT=relay and retry.");
        }
        // Pick a free local UDP port for the spectator's bind. 7002 by
        // convention (above the host's 7000 and the client's 7001).
        // If a spectator is already running, LaunchRemoteSpectator returns
        // false; user can stop it from the multi-client tools first.
        constexpr int SPEC_LOCAL_PORT = 7002;
        LaunchRemoteSpectator(selected_game_.exe_path, SPEC_LOCAL_PORT,
                              host_ip, host_port, session_kind, spec_transport);
    };
    ui_->on_exit = [this]() {
        running_ = false;
    };

    // C11 — Replay browser dispatch. Resolve the game .exe from the
    // replay file's grandparent directory (replays/<file>.fm2krep is
    // always under <game_dir>/replays/) — same logic as the --replay
    // CLI flag. Then call LaunchReplayPlayer to spawn the game with
    // FM2K_REPLAY_FILE set.
    ui_->on_replay_play = [this](const std::string& replay_path) {
        std::error_code ec;
        std::filesystem::path replay_fs = std::filesystem::u8path(replay_path);
        std::filesystem::path canon =
            std::filesystem::weakly_canonical(replay_fs, ec);
        if (ec) canon = replay_fs;
        std::filesystem::path game_dir =
            canon.parent_path().parent_path();

        std::string game_exe_path;
        if (!game_dir.empty() &&
            std::filesystem::is_directory(game_dir, ec)) {
            std::filesystem::path kgt_stem;
            for (const auto& entry :
                 std::filesystem::directory_iterator(game_dir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c){ return std::tolower(c); });
                if (ext == ".kgt") {
                    kgt_stem = entry.path().stem();
                    break;
                }
            }
            if (!kgt_stem.empty()) {
                std::filesystem::path candidate = game_dir / kgt_stem;
                candidate += ".exe";
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    game_exe_path = candidate.string();
                }
            }
        }
        if (game_exe_path.empty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Replay browser: could not locate game .exe under %s",
                game_dir.string().c_str());
            return;
        }
        LaunchReplayPlayer(game_exe_path, replay_path);
    };

    ui_->on_games_folders_set = [this](const std::vector<std::string>& folders) {
        SetGamesRootPaths(folders);
    };
    
    // Connect debug state callbacks
    ui_->on_debug_save_state = [this]() -> bool {
        if (game_instance_) {
            return game_instance_->TriggerManualSaveState();
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for debug save state");
        return false;
    };
    
    ui_->on_debug_load_state = [this]() -> bool {
        if (game_instance_) {
            return game_instance_->TriggerManualLoadState();
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for debug load state");
        return false;
    };
    
    ui_->on_debug_force_rollback = [this](uint32_t frames) -> bool {
        if (game_instance_) {
            return game_instance_->TriggerForceRollback(frames);
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for debug rollback");
        return false;
    };
    
    // Connect frame stepping callbacks
    ui_->on_frame_step_pause = [this](bool pause) {
        if (game_instance_) {
            game_instance_->SetFrameStepPause(pause);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for frame stepping");
        }
    };
    
    ui_->on_frame_step_single = [this]() {
        if (game_instance_) {
            game_instance_->StepSingleFrame();
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for frame stepping");
        }
    };
    
    ui_->on_frame_step_multi = [this](uint32_t frames) {
        if (game_instance_) {
            game_instance_->StepMultipleFrames(frames);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for frame stepping");
        }
    };
    
    // Connect slot-based save/load callbacks
    ui_->on_debug_save_to_slot = [this](uint32_t slot) -> bool {
        if (game_instance_) {
            return game_instance_->TriggerSaveToSlot(slot);
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for save to slot");
        return false;
    };
    
    ui_->on_debug_load_from_slot = [this](uint32_t slot) -> bool {
        if (game_instance_) {
            return game_instance_->TriggerLoadFromSlot(slot);
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for load from slot");
        return false;
    };
    
    ui_->on_debug_auto_save_config = [this](bool enabled, uint32_t interval_frames) -> bool {
        if (game_instance_) {
            game_instance_->SetAutoSaveEnabled(enabled);
            return game_instance_->SetAutoSaveInterval(interval_frames);
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No active game instance for auto-save config");
        return false;
    };
    
    // Connect auto-save config reading callback
    ui_->on_get_auto_save_config = [this](LauncherUI::AutoSaveConfig& config) -> bool {
        if (game_instance_) {
            FM2KGameInstance::AutoSaveConfig game_config;
            if (game_instance_->GetAutoSaveConfig(game_config)) {
                config.enabled = game_config.enabled;
                config.interval_frames = game_config.interval_frames;
                return true;
            }
        }
        return false;
    };
    
    // Connect debug and testing configuration callbacks
    ui_->on_set_production_mode = [this](bool enabled) -> bool {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "UI callback: SetProductionMode(%s)", enabled ? "true" : "false");
        bool success = false;
        bool deferred = false;
        
        if (game_instance_) {
            success = game_instance_->SetProductionMode(enabled);
            if (!success) {
                deferred = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Production mode config deferred - will be applied when shared memory is ready");
            }
        }
        if (client1_instance_) {
            if (client1_instance_->SetProductionMode(enabled)) {
                success = true;
            }
        }
        if (client2_instance_) {
            if (client2_instance_->SetProductionMode(enabled)) {
                success = true;
            }
        }
        
        return success || deferred;
    };
    
    ui_->on_set_input_recording = [this](bool enabled) -> bool {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "UI callback: SetInputRecording(%s)", enabled ? "true" : "false");
        bool success = false;
        bool deferred = false;
        
        if (game_instance_) {
            success = game_instance_->SetInputRecording(enabled);
            if (!success) {
                deferred = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Input recording config deferred - will be applied when shared memory is ready");
            }
        }
        if (client1_instance_) {
            if (client1_instance_->SetInputRecording(enabled)) {
                success = true;
            }
        }
        if (client2_instance_) {
            if (client2_instance_->SetInputRecording(enabled)) {
                success = true;
            }
        }
        
        return success || deferred;
    };
    
    ui_->on_set_minimal_gamestate_testing = [this](bool enabled) -> bool {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "UI callback: SetMinimalGameStateTesting(%s)", enabled ? "true" : "false");
        bool success = false;
        bool deferred = false;
        
        // Check if any game instances exist
        if (game_instance_ || client1_instance_ || client2_instance_) {
            // Apply to existing instances
            if (game_instance_) {
                success = game_instance_->SetMinimalGameStateTesting(enabled);
                if (!success) {
                    deferred = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "MinimalGameState testing config deferred - will be applied when shared memory is ready");
                }
            }
            if (client1_instance_) {
                if (client1_instance_->SetMinimalGameStateTesting(enabled)) {
                    success = true;
                }
            }
            if (client2_instance_) {
                if (client2_instance_->SetMinimalGameStateTesting(enabled)) {
                    success = true;
                }
            }
        } else {
            // No instances exist yet - store as pending configuration
            pending_config_.has_minimal_gamestate_testing = true;
            pending_config_.minimal_gamestate_testing_value = enabled;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "No game instances exist yet - storing MinimalGameState testing config as pending: %s", enabled ? "enabled" : "disabled");
            return true;  // Successfully stored as pending
        }
        
        return success || deferred;  // Return true if either applied or deferred
    };
    
    // Save profile callback removed - now using optimized FastGameState system
    
    // Connect slot status callback
    ui_->on_get_slot_status = [this](uint32_t slot, LauncherUI::SlotStatusInfo& status) -> bool {
        if (game_instance_) {
            FM2KGameInstance::SlotStatus game_status;
            if (game_instance_->GetSlotStatus(slot, game_status)) {
                status.occupied = game_status.occupied;
                status.frame_number = game_status.frame_number;
                status.timestamp_ms = game_status.timestamp_ms;
                status.checksum = game_status.checksum;
                status.state_size_kb = game_status.state_size_kb;
                status.save_time_us = game_status.save_time_us;
                status.load_time_us = game_status.load_time_us;
                status.active_object_count = game_status.active_object_count;
                return true;
            }
        }
        return false;
    };
    
    // Enhanced actions removed from shared memory - no longer available
    ui_->on_get_enhanced_actions = [this]() -> std::vector<LauncherUI::EnhancedActionInfo> {
        std::vector<LauncherUI::EnhancedActionInfo> enhanced_actions;
        return enhanced_actions;
    };
    
    // Connect multi-client testing callbacks
    ui_->on_launch_local_client1 = [this](const std::string& game_path) -> bool {
        return LaunchLocalClient(game_path, true, 7000);  // Host on port 7000
    };
    
    ui_->on_launch_local_client2 = [this](const std::string& game_path) -> bool {
        return LaunchLocalClient(game_path, false, 7001);  // Guest on port 7001
    };

    ui_->on_launch_local_spectator = [this](const std::string& game_path) -> bool {
        // Spectator subscribes to client1 (host on 7000), itself bound on 7002.
        return LaunchLocalSpectator(game_path, /*spectator_port=*/7002, /*host_port=*/7000);
    };

    ui_->on_launch_local_spectator2 = [this](const std::string& game_path) -> bool {
        // Daisy-chain: spectator 2 subscribes to spectator 1 (port 7002),
        // bound on 7003. Validates the relay-forward path.
        return LaunchLocalSpectator2(game_path, /*spectator_port=*/7003, /*upstream_port=*/7002);
    };
    
    ui_->on_terminate_all_clients = [this]() -> bool {
        bool success = TerminateAllClients();
        return success;
    };
    
    ui_->on_get_client_status = [this](uint32_t& client1_pid, uint32_t& client2_pid) -> bool {
        bool client1_running = (client1_instance_ && client1_instance_->IsRunning());
        bool client2_running = (client2_instance_ && client2_instance_->IsRunning());

        client1_pid = client1_running ? client1_instance_->GetProcessId() : 0;
        client2_pid = client2_running ? client2_instance_->GetProcessId() : 0;

        // Online (single-game) sessions live on game_instance_, not the
        // dev-mode dual slots. Surface that PID through the same channel
        // so the W/L/D shared-mem poll has something to read.
        if (client1_pid == 0 && game_instance_ && game_instance_->IsRunning()) {
            client1_pid = game_instance_->GetProcessId();
            client1_running = true;
        }

        return client1_running || client2_running;
    };

    ui_->on_resolve_stage_name = [this](const std::string& game_id,
                                        uint32_t stage_id) -> std::string {
        if (stage_id == 0xFFFFFFFFu) return {};
        const fm2k::KgtSummary* k = FindKgtByGameId(game_id);
        if (!k) return {};
        const std::string& n = k->StageName((int)stage_id);
        return n;  // empty string if slot empty / out-of-range
    };

    ui_->on_resolve_char_name = [this](const std::string& game_id,
                                       uint32_t char_id) -> std::string {
        if (char_id == 0xFFFFFFFFu) return {};
        const fm2k::KgtSummary* k = FindKgtByGameId(game_id);
        if (!k) return {};
        const std::string& n = k->PlayerName((int)char_id);
        return n;
    };

    // Network simulation callbacks removed - not connected to LocalNetworkAdapter
    
    ui_->on_get_rollback_stats = [this](RollbackStats& stats) -> bool {
        // Read real rollback statistics from hook DLL shared memory
        return ReadRollbackStatsFromSharedMemory(stats);
    };
    
    // If no games directories stored, default to the launcher's own dir.
    if (games_root_paths_.empty()) {
        std::string base_path;
        if (const char *sdl_base = SDL_GetBasePath()) {
            base_path = sdl_base;
            SDL_free(const_cast<char *>(sdl_base));
        } else {
            const char* cwd = SDL_GetCurrentDirectory();
            base_path = cwd ? cwd : "";
            if (cwd) SDL_free(const_cast<char*>(cwd));
        }
        // Remove trailing slash if present (we want the directory itself, not a subdirectory)
        if (!base_path.empty() && (base_path.back() == '/' || base_path.back() == '\\')) {
            base_path.pop_back();
        }
        if (!base_path.empty()) {
            games_root_paths_.push_back(base_path);
        }
    }

    // Cache-first display: load the full cached game list (xxh64, engine,
    // kgt summary, …) so the UI is fully populated immediately. Async
    // discovery still runs to catch newly installed/removed games, but
    // it's invisible to the user when nothing has changed.
    bool seeded_from_cache = false;
    {
        auto cached_games = Utils::LoadGameCache();
        if (!cached_games.empty()) {
            ui_->SetGames(cached_games);
            // Mirror into our internal state so FindKgtByGameId / launch
            // paths see the cached games before async discovery completes.
            discovered_games_ = std::move(cached_games);
            seeded_from_cache = true;
        }
        ui_->SetGamesRootPaths(games_root_paths_);
    }
    // Suppress the "Scanning…" spinner when the cache already filled the
    // list — the background walk just verifies nothing changed.
    StartAsyncDiscovery(/*show_spinner=*/!seeded_from_cache);
    
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher initialized successfully");
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Found %d FM2K games", (int)discovered_games_.size());
    
    return true;
}

void FM2KLauncher::HandleEvent(SDL_Event* event) {
    if (!event) return;

    // Let ImGui handle events first
    ImGui_ImplSDL3_ProcessEvent(event);

    // Handle window events - just log them, don't interfere
    if (event->type == SDL_EVENT_WINDOW_MINIMIZED) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL_EVENT_WINDOW_MINIMIZED: Window minimized normally");
    } else if (event->type == SDL_EVENT_WINDOW_RESTORED || 
               event->type == SDL_EVENT_WINDOW_SHOWN) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL_EVENT_WINDOW_RESTORED/SHOWN: Window restored");
    } else if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        if (event->window.windowID == SDL_GetWindowID(window_)) {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize.x = static_cast<float>(event->window.data1);
            io.DisplaySize.y = static_cast<float>(event->window.data2);
        }
    }

    // Handle discovery completion
    if (event->type == g_event_discovery_complete) {
        auto games_ptr = static_cast<std::vector<FM2K::FM2KGameInfo>*>(event->user.data1);
        if (games_ptr) {
            discovered_games_ = std::move(*games_ptr);
            delete games_ptr;
        }

        discovery_in_progress_ = false;
        if (discovery_thread_) {
            SDL_WaitThread(discovery_thread_, nullptr);
            discovery_thread_ = nullptr;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Async discovery complete: %d games found", (int)discovered_games_.size());
        if (ui_) {
            ui_->SetGames(discovered_games_);
            ui_->SetScanning(false);
        }
        Utils::SaveGameCache(discovered_games_);
    }

    // Only process our events if ImGui isn't capturing input
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && !io.WantCaptureKeyboard) {
        if (event->type == SDL_EVENT_KEY_DOWN) {
            if (event->key.scancode == SDL_SCANCODE_ESCAPE) {
                running_ = false;
            }
        }
    }
}

void FM2KLauncher::Update(float delta_time SDL_UNUSED) {
    if (!running_) {
        // If the main loop is not running, trigger a clean shutdown
        // This handles cases where on_exit is called
        SDL_Event quit_event;
        quit_event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit_event);
        return;
    }

    // DLL handles GekkoNet directly - no launcher-side session needed
    
    // Process DLL events from the game instance
    if (game_instance_ && game_instance_->IsRunning()) {
        game_instance_->ProcessDLLEvents();
    }

    // TCP-STUN poll: when the spec hook completes its outbound STUN
    // probe (FM2KHook/src/netplay/spectator_tcp.cpp PerformTcpStun), it
    // bumps tcp_stun_seq in SharedMem with the discovered external
    // (ip, port). Forward to hub via WS so cross-NAT spectators can be
    // told the right TCP punch target. Track per-pid last-seen seq so
    // we only forward fresh values; resets when game_instance restarts.
    {
        DWORD target_pid = 0;
        if (game_instance_ && game_instance_->IsRunning()) {
            target_pid = game_instance_->GetProcessId();
        } else if (client1_instance_ && client1_instance_->IsRunning()) {
            target_pid = client1_instance_->GetProcessId();
        }
        static DWORD    s_last_pid = 0;
        static uint32_t s_last_seq = 0;
        static uint32_t s_last_sk_seq = 0;
        if (target_pid != 0 && target_pid != s_last_pid) {
            s_last_pid = target_pid;
            s_last_seq = 0;
            s_last_sk_seq = 0;
        }
        if (target_pid != 0) {
            const std::string mapping_name =
                "FM2K_SharedMem_" + std::to_string((unsigned)target_pid);
            HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE,
                                        mapping_name.c_str());
            if (h) {
                FM2KSharedMemData* shm = static_cast<FM2KSharedMemData*>(
                    MapViewOfFile(h, FILE_MAP_READ, 0, 0,
                                  sizeof(FM2KSharedMemData)));
                if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC &&
                    shm->tcp_stun_seq != 0 &&
                    shm->tcp_stun_seq != s_last_seq) {
                    s_last_seq = shm->tcp_stun_seq;
                    if (ui_) {
                        ui_->SendHubTcpAddr(shm->tcp_stun_ext_ip_be,
                                            shm->tcp_stun_ext_port);
                    }
                }
                // session_kind poll: forwards menu/CSS/battle phase
                // transitions to the hub so spectators joining us know
                // whether to /F-boot-to-battle or walk title→CSS.
                if (shm && shm->magic == FM2K_SHARED_MEM_MAGIC &&
                    shm->session_kind_seq != 0 &&
                    shm->session_kind_seq != s_last_sk_seq) {
                    s_last_sk_seq = shm->session_kind_seq;
                    if (ui_) {
                        ui_->SendHubSessionKind(shm->session_kind);
                    }
                }
                if (shm) UnmapViewOfFile(shm);
                CloseHandle(h);
            }
        }
    }

    // Spec hub-relay drain (Phase 2c). When the hook is running in
    // FM2K_SPEC_TRANSPORT=relay mode it creates the outbound shared-mem
    // ring "FM2K_SpecRelayOut_<pid>" and produces one Slot per spec
    // frame it wants to ship. We open the ring lazily, drain pop-able
    // slots each tick, pack each Slot into the SpecDataBinary wire
    // shape (matches hub.py:handle_spec_relay_frame), and ship via
    // HubClient::SendSpecRelayFrame (-> WS binary frame).
    //
    // Hook in TCP mode never creates the mapping; OpenOutboundFor
    // returns nullptr and the drain is a no-op. No env probing or
    // mode detection needed on the launcher side -- existence of the
    // mapping IS the signal.
    {
        DWORD target_pid = 0;
        if (game_instance_ && game_instance_->IsRunning()) {
            target_pid = game_instance_->GetProcessId();
        } else if (client1_instance_ && client1_instance_->IsRunning()) {
            target_pid = client1_instance_->GetProcessId();
        }

        // Outbound ring cached as class member (was lambda static)
        // for the same reason as inbound -- menu-bar status pill needs
        // live access.
        auto* relay_ring_ptr =
            static_cast<fm2k::spec_relay::Ring*>(spec_relay_out_ring_);

        // Game pid changed (or fresh process); drop the old mapping.
        if (target_pid != spec_relay_out_pid_) {
            if (relay_ring_ptr) {
                fm2k::spec_relay::Close(relay_ring_ptr);
                relay_ring_ptr = nullptr;
                spec_relay_out_ring_ = nullptr;
            }
            spec_relay_out_pid_ = target_pid;
        }
        // Retry open every tick when we have a pid but no ring. Hook
        // creates the mapping during SpectatorNode_Init which races our
        // first tick after game spawn; without retry the cache sticks
        // at nullptr even though the hook came up milliseconds later.
        // OpenFileMappingA returns fast on miss so tick-rate retry is
        // cheap.
        if (target_pid != 0 && !relay_ring_ptr) {
            relay_ring_ptr = fm2k::spec_relay::OpenOutboundFor(target_pid);
            spec_relay_out_ring_ = relay_ring_ptr;
            if (relay_ring_ptr) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpecRelay: opened outbound ring for game pid %lu",
                    (unsigned long)target_pid);
            }
        }

        // Drain. Bound work-per-tick so a snapshot transfer doesn't
        // monopolize the UI loop; 32 slots × 16 KB = ~512 KB / tick max,
        // and snapshot transfers are paced by GekkoNet's broadcast
        // cadence anyway so this rarely saturates.
        if (relay_ring_ptr && ui_) {
            constexpr int kMaxPerTick = 32;
            for (int i = 0; i < kMaxPerTick; ++i) {
                const fm2k::spec_relay::Slot* slot =
                    fm2k::spec_relay::PeekFront(relay_ring_ptr);
                if (!slot) break;

                // Pack Slot -> SpecDataBinary wire frame.
                //   u32 magic = 0x53504442 ("SPDB")
                //   u32 frame_count
                //   u16 type
                //   u16 flags
                //   u32 payload_len
                //   u8  target_kind
                //   u8  spec_user_id_len
                //   char spec_user_id[]
                //   u8  payload[]
                std::vector<uint8_t> frame;
                const uint32_t magic = 0x53504442u;
                const uint8_t spec_id_len =
                    slot->target_kind == fm2k::spec_relay::TARGET_DIRECT
                        ? (uint8_t)std::strlen(slot->spec_user_id)
                        : 0;
                frame.reserve(16 + 2 + spec_id_len + slot->payload_len);
                auto append_u32 = [&](uint32_t v) {
                    frame.push_back((uint8_t)(v));
                    frame.push_back((uint8_t)(v >> 8));
                    frame.push_back((uint8_t)(v >> 16));
                    frame.push_back((uint8_t)(v >> 24));
                };
                auto append_u16 = [&](uint16_t v) {
                    frame.push_back((uint8_t)(v));
                    frame.push_back((uint8_t)(v >> 8));
                };
                append_u32(magic);
                append_u32(slot->frame_count);
                append_u16((uint16_t)slot->spec_data_type);
                append_u16((uint16_t)slot->spec_data_flags);
                append_u32(slot->payload_len);
                frame.push_back((uint8_t)slot->target_kind);
                frame.push_back(spec_id_len);
                if (spec_id_len) {
                    frame.insert(frame.end(),
                                 slot->spec_user_id,
                                 slot->spec_user_id + spec_id_len);
                }
                if (slot->payload_len) {
                    frame.insert(frame.end(),
                                 slot->payload,
                                 slot->payload + slot->payload_len);
                }

                ui_->SendHubSpecRelayFrame(std::move(frame));
                fm2k::spec_relay::PopFront(relay_ring_ptr);
            }
        }

        // Phase 4: surface the latest ring counters to the menu-bar
        // status pill. Both outbound (host produces -> launcher drains)
        // and inbound (launcher fills -> hook drains) read from class
        // members (promoted from lambda statics so this read works).
        LauncherUI::SpecRelayStatus st{};
        st.out_active = (relay_ring_ptr != nullptr);
        if (st.out_active) {
            st.out_enqueued = relay_ring_ptr->total_enqueued;
            st.out_dropped  = relay_ring_ptr->total_dropped;
            st.out_dequeued = relay_ring_ptr->total_dequeued;
        }
        auto* in_ring_status =
            static_cast<const fm2k::spec_relay::Ring*>(spec_relay_in_ring_);
        st.in_active = (in_ring_status != nullptr);
        if (st.in_active) {
            st.in_enqueued = in_ring_status->total_enqueued;
            st.in_dropped  = in_ring_status->total_dropped;
            st.in_dequeued = in_ring_status->total_dequeued;
        }
        if (ui_) ui_->SetSpecRelayStatus(st);
    }

    // Check for game termination
    if (game_instance_ && !game_instance_->IsRunning()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game process has terminated.");
        // Game has ended, stop the session and return to selection
        StopSession();
    }
    ui_->NewFrame();
}

void FM2KLauncher::Render() {
    // Clear screen
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    
    // Render UI
    ui_->Render();

    // Finalize ImGui draw data
    ImGui::Render();
    
    // Render ImGui draw data using the SDL_Renderer backend
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    
    // Update and Render additional Platform Windows
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    
    SDL_RenderPresent(renderer_);
}

bool FM2KLauncher::InitializeSDL() {
    // Hints MUST be set before the gamepad subsystem starts. SDL3
    // reads HIDAPI/RawInput hints when it stands up the joystick
    // backend, NOT lazily — setting them later (e.g. inside the
    // input binder's Init()) is a no-op. Without HIDAPI_PS3 the
    // PS3 controller (and Qanba sticks in PS3 mode) fall through to
    // a generic HID joystick path that has no SDL gamepad mapping,
    // so SDL_GetGamepadButton sees nothing and the binder ignores
    // every press. Mirrors revolve_input_sdl3's setup order.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI,         "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH,  "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT,       "1");

    // Initialize SDL with all necessary subsystems
    SDL_InitFlags init_flags = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;

    if (!SDL_Init(init_flags)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    // Gamepad events flow into SDL's event queue and update polled
    // state; the binder reads the polled state via SDL_GetGamepadButton.
    // Without this enabled, polled state never refreshes on Windows.
    SDL_SetGamepadEventsEnabled(true);
    
    // Create window with SDL_Renderer graphics context
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    
    window_ = SDL_CreateWindow("FM2K Rollback Launcher", 
        (int)(1280 * main_scale), (int)(720 * main_scale), 
        window_flags);
        
    if (!window_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window_);
        SDL_Quit();
        return false;
    }
    // Vsync is the framerate cap. If it silently fails (driver fallback,
    // headless / RDP session, software renderer), the launcher would spin
    // at hundreds of fps and burn CPU/GPU — exactly the symptom users
    // reported on Xeon E3 / 3060 (~20% CPU + ~20% GPU at idle). Log the
    // result, and stash a flag so SDL_AppIterate can soft-cap to ~60fps
    // via SDL_DelayNS when vsync is unavailable.
    if (!SDL_SetRenderVSync(renderer_, 1)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
            "SDL_SetRenderVSync(1) failed: %s — falling back to software cap",
            SDL_GetError());
        vsync_available_ = false;
    } else {
        int v = 0;
        if (SDL_GetRenderVSync(renderer_, &v) && v == 1) {
            vsync_available_ = true;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                "SDL_GetRenderVSync reports vsync=%d — assuming off, software-capping",
                v);
            vsync_available_ = false;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_RENDER,
        "Renderer: '%s', vsync=%s",
        SDL_GetRendererName(renderer_) ? SDL_GetRendererName(renderer_) : "?",
        vsync_available_ ? "on" : "off (software cap)");

    // App icon. We try paths first (so a future assets/icon.bmp drop-in
    // overrides without a rebuild), then fall back to an embedded
    // base64-decoded PNG (placeholder smiley). If both fail, draw a
    // 32×32 blue square. SDL3_image is statically linked so IMG_Load_IO
    // handles the PNG without an external SDL_image.dll.
    SDL_Surface* icon = nullptr;
    const char* icon_paths[] = {
        "assets/icon.bmp",
        "icon.bmp",
        "../icon.bmp"
    };

    for (const char* path : icon_paths) {
        icon = SDL_LoadBMP(path);
        if (icon) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Loaded icon from: %s", path);
            break;
        }
    }

    if (!icon) {
        // Decode the embedded base64 PNG. Decoder is short and self-
        // contained — pulling SDL_base64 would mean wiring another
        // header path, not worth it for a one-shot at startup.
        const char* b64 = fm2k::kAppIconBase64;
        const size_t b64_len = std::strlen(b64);
        std::vector<uint8_t> png_bytes;
        png_bytes.reserve((b64_len * 3) / 4 + 4);
        auto val = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        uint32_t buf = 0;
        int      bits = 0;
        for (size_t i = 0; i < b64_len; ++i) {
            const char c = b64[i];
            if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
                if (c == '=') break;
                continue;
            }
            const int v = val(c);
            if (v < 0) continue;
            buf = (buf << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                png_bytes.push_back((uint8_t)((buf >> bits) & 0xFFu));
            }
        }
        SDL_IOStream* io = SDL_IOFromConstMem(png_bytes.data(), png_bytes.size());
        if (io) {
            icon = IMG_Load_IO(io, /*closeio=*/true);
            if (icon) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Loaded embedded smiley icon (%zu bytes PNG)",
                            png_bytes.size());
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "IMG_Load_IO failed for embedded icon: %s",
                            SDL_GetError());
            }
        }
    }

    // If still no icon, draw a 32×32 blue square as final fallback.
    if (!icon) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No icon file found, creating default icon");
        icon = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_RGBA32);
        if (icon) {
            // Create a solid blue color (R=0, G=120, B=215, A=255)
            Uint8* pixels = (Uint8*)icon->pixels;
            int pitch = icon->pitch;
            SDL_LockSurface(icon);
            for (int y = 0; y < 32; y++) {
                for (int x = 0; x < 32; x++) {
                    Uint32* pixel = (Uint32*)(pixels + y * pitch + x * 4);
                    *pixel = 0x0078D7FF; // RGBA packed value for Windows blue
                }
            }
            SDL_UnlockSurface(icon);
        }
    }

    // Set window icon if we have one
    if (icon) {
        SDL_SetWindowIcon(window_, icon);

        // Console window (conhost) inherits a generic icon when we attach
        // via AllocConsole / parent inheritance. Convert the SDL surface
        // to an HICON and SendMessage(WM_SETICON) to the console window
        // so the smiley shows up in the title bar + Alt-Tab. Skipped if
        // there's no console (launcher started without one — then
        // GetConsoleWindow returns NULL).
        if (HWND console_hwnd = GetConsoleWindow()) {
            // Normalize to RGBA32 — DIB section we hand to CreateIconIndirect
            // expects 32-bit BGRA top-down. ConvertSurface returns NULL on
            // mismatch but a fresh copy on success; we own it.
            SDL_Surface* rgba = SDL_ConvertSurface(icon, SDL_PIXELFORMAT_BGRA32);
            if (rgba) {
                SDL_LockSurface(rgba);
                BITMAPV5HEADER bi = {};
                bi.bV5Size        = sizeof(bi);
                bi.bV5Width       = rgba->w;
                bi.bV5Height      = -rgba->h;          // top-down
                bi.bV5Planes      = 1;
                bi.bV5BitCount    = 32;
                bi.bV5Compression = BI_BITFIELDS;
                bi.bV5RedMask     = 0x00FF0000;
                bi.bV5GreenMask   = 0x0000FF00;
                bi.bV5BlueMask    = 0x000000FF;
                bi.bV5AlphaMask   = 0xFF000000;
                HDC screen_dc = GetDC(nullptr);
                void* dib_pixels = nullptr;
                HBITMAP color_bmp = CreateDIBSection(
                    screen_dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                    &dib_pixels, nullptr, 0);
                ReleaseDC(nullptr, screen_dc);
                if (color_bmp && dib_pixels) {
                    std::memcpy(dib_pixels, rgba->pixels,
                                (size_t)rgba->w * rgba->h * 4u);
                    HBITMAP mask_bmp = CreateBitmap(rgba->w, rgba->h, 1, 1, nullptr);
                    ICONINFO info = {};
                    info.fIcon    = TRUE;
                    info.hbmMask  = mask_bmp;
                    info.hbmColor = color_bmp;
                    HICON hicon = CreateIconIndirect(&info);
                    if (hicon) {
                        SendMessageW(console_hwnd, WM_SETICON,
                                     ICON_SMALL, (LPARAM)hicon);
                        SendMessageW(console_hwnd, WM_SETICON,
                                     ICON_BIG,   (LPARAM)hicon);
                        // Don't DestroyIcon — the console keeps a reference
                        // for the lifetime of the window. Leaks one icon
                        // handle on launcher exit, which is fine.
                    }
                    if (mask_bmp) DeleteObject(mask_bmp);
                    DeleteObject(color_bmp);
                }
                SDL_UnlockSurface(rgba);
                SDL_DestroySurface(rgba);
            }
        }
    }

    // No tray icon - just a normal window application

    // Now we can destroy the surface
    if (icon) {
        SDL_DestroySurface(icon);
    }

    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);
    
    return true;
}

void FM2KLauncher::Shutdown() {
    // Terminate any running test clients
    TerminateAllClients();


    // Stop network and game first
    // DLL handles GekkoNet directly - no launcher-side session needed

    // Close spec hub-relay shared-mem mappings (Phase 4). Close handles
    // nullptr safely; hook side closes its end on DLL unload.
    if (spec_relay_out_ring_) {
        fm2k::spec_relay::Close(
            static_cast<fm2k::spec_relay::Ring*>(spec_relay_out_ring_));
        spec_relay_out_ring_ = nullptr;
    }
    if (spec_relay_in_ring_) {
        fm2k::spec_relay::Close(
            static_cast<fm2k::spec_relay::Ring*>(spec_relay_in_ring_));
        spec_relay_in_ring_ = nullptr;
    }

    if (game_instance_) {
        game_instance_->Terminate();
        game_instance_.reset();
    }
    
    // No tray icon to destroy
    
    // Ensure UI cleanup happens before ImGui shutdown
    if (ui_) {
        ui_->Shutdown();
        ui_.reset();
    }
    
    // SDL cleanup
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    
    // Make sure discovery thread is finished before quitting SDL
    if (discovery_thread_) {
        SDL_WaitThread(discovery_thread_, nullptr);
        discovery_thread_ = nullptr;
    }

    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    
    SDL_Quit();
    MH_Uninitialize();
}

void FM2KLauncher::StartAsyncDiscovery(bool show_spinner) {
    // Prevent overlapping scans
    if (discovery_in_progress_) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Discovery already in progress ? ignoring new request");
        return;
    }

    discovery_in_progress_ = true;
    if (ui_ && show_spinner) ui_->SetScanning(true);

    // If a previous thread handle exists (shouldn't) ensure it is cleaned up.
    if (discovery_thread_) {
        SDL_WaitThread(discovery_thread_, nullptr);
        discovery_thread_ = nullptr;
    }

    discovery_thread_ = SDL_CreateThread(DiscoveryThreadFunc, "FM2KDiscovery", this);
    if (!discovery_thread_) {
        discovery_in_progress_ = false;
        if (ui_) ui_->SetScanning(false);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateThread failed: %s", SDL_GetError());
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Started background discovery thread...");
    }
}

// Max depth from a games root to descend looking for .kgt files. Real-world
// libraries are organized publisher/game/[version/], so 6 covers everything
// while preventing pathological symlink loops or scanning the whole drive.
static constexpr int DISCOVERY_MAX_DEPTH = 6;

// Walk state passed via SDL_EnumerateDirectory's userdata. Threads through
// the on-disk cache map so we can skip re-validating unchanged files.
struct DiscoveryWalkCtx {
    std::vector<FM2K::FM2KGameInfo>* games;
    const std::unordered_map<std::string, Utils::GameCacheEntry>* cache;
    int depth_remaining;
};

static SDL_EnumerationResult DirectoryEnumerator(void* userdata, const char* origdir, const char* name);

// Look up an exe path in the cache and validate that BOTH the exe and
// the kgt (if present) have not changed since the cache was written.
// `kgt_path` may be empty for FM95 .player-only games — in that case the
// kgt-stat check is skipped.
//
// Returns true on a full hit; the caller can then populate FM2KGameInfo
// directly from `out` and skip xxh64, kgt parse, engine sniff, packer
// sniff — i.e. zero file I/O for this game.
static bool TryUseCachedEntry(
    const std::unordered_map<std::string, Utils::GameCacheEntry>* cache,
    const std::string& exe_path,
    const std::string& kgt_path,
    Utils::GameCacheEntry& out)
{
    if (!cache || cache->empty()) return false;
    std::string key = Utils::CanonicalizePath(exe_path);
    auto it = cache->find(key);
    if (it == cache->end()) return false;

    uint64_t exe_size = 0;
    int64_t  exe_mtime = 0;
    if (!Utils::StatFile(exe_path, exe_size, exe_mtime)) return false;
    if (it->second.size != exe_size || it->second.mtime != exe_mtime) return false;

    // KGT validation: presence + stat must match. If the cache says no kgt
    // and now there IS one (or vice versa), force a re-scan.
    const bool now_has_kgt = !kgt_path.empty();
    if (it->second.kgt_present != now_has_kgt) return false;
    if (now_has_kgt) {
        uint64_t kgt_size = 0;
        int64_t  kgt_mtime = 0;
        if (!Utils::StatFile(kgt_path, kgt_size, kgt_mtime)) return false;
        if (it->second.kgt_size != kgt_size || it->second.kgt_mtime != kgt_mtime) {
            return false;
        }
    }

    out = it->second;
    return true;
}

static void ScanDirForGames(const std::string& dir,
                            std::vector<FM2K::FM2KGameInfo>& games,
                            const std::unordered_map<std::string, Utils::GameCacheEntry>* cache,
                            int depth_remaining)
{
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Scanning '%s' (depth_left=%d)",
                 dir.c_str(), depth_remaining);

    // 1) Check this directory itself for *.kgt + matching *.exe (FM2K).
    int count = 0;
    bool found_kgt_pair = false;
    char** list = SDL_GlobDirectory(dir.c_str(), "*.kgt", SDL_GLOB_CASEINSENSITIVE, &count);
    if (list) {
        for (int i = 0; i < count; ++i) {
            if (!list[i]) continue;

            const char* kgt_name = SDL_strrchr(list[i], '/');
            if (!kgt_name) kgt_name = SDL_strrchr(list[i], '\\');
            kgt_name = kgt_name ? kgt_name + 1 : list[i];

            char* exe_name = nullptr;
            if (SDL_asprintf(&exe_name, "%.*s.exe",
                             (int)(SDL_strlen(kgt_name) - 4), kgt_name) < 0 || !exe_name) {
                continue;
            }
            // Use platform-preferred separator so the paths displayed in
            // the games list match the slash style of the user-typed
            // root folder. FlippySpatula reported the visible mismatch
            // as confusing — Windows users see "C:\games" for their
            // root then "C:\games/foo.exe" for the scraped child.
            std::string exe_path = dir + "\\" + exe_name;
            std::string kgt_path = dir + "\\" + kgt_name;
            SDL_free(exe_name);

            // Full cache hit: reuse the parsed summary, hash, engine, and
            // packer label without reading a single byte of either file.
            Utils::GameCacheEntry cached_entry;
            bool cache_hit = TryUseCachedEntry(cache, exe_path, kgt_path, cached_entry);
            if (!cache_hit && !SDL_GetPathInfo(exe_path.c_str(), nullptr)) {
                continue;  // exe missing
            }

            FM2K::FM2KGameInfo info{exe_path, kgt_path, 0, true};

            if (cache_hit) {
                info.xxh64        = cached_entry.exe_xxh64;
                info.engine       = cached_entry.engine;
                info.clean_label  = cached_entry.clean_label;
                info.packer_label = cached_entry.packer_label;
                info.is_clean     = cached_entry.is_clean;
                info.kgt          = cached_entry.kgt;
            } else {
                info.xxh64 = Utils::HashFileXXH64(exe_path);

                // Parse the .kgt for player/stage/demo name lists so the UI
                // can populate dropdowns pre-launch. Failure is non-fatal:
                // we still surface the game; dropdowns fall back to indices.
                if (!fm2k::ParseKgtSummary(std::filesystem::u8path(kgt_path), info.kgt)) {
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                                 "KGT parse failed for '%s' — dropdowns will be empty",
                                 kgt_path.c_str());
                }

                // Engine detection cascade (best signal first):
                //   1. exact-hash registry match -> known build, friendly label
                //   2. KGT2KGAME / KGT95GAME string sniff -> exact engine
                //   3. file-size heuristic (1.1-1.7 MB FM2K, <600 KB FM95)
                // Then PE-section sniff for actual packers.
                if (const auto* known = Utils::FindKnownExe(info.xxh64)) {
                    info.engine      = known->engine;
                    info.clean_label = known->label;
                } else if (auto sniffed = Utils::SniffEngineFromStrings(exe_path)) {
                    info.engine = *sniffed;
                } else {
                    uint64_t sz = 0; int64_t mt = 0;
                    Utils::StatFile(exe_path, sz, mt);
                    info.engine = Utils::GuessEngineFromSize(sz);
                }

                info.packer_label = Utils::DetectPackerFromPE(exe_path);
                info.is_clean     = info.packer_label.empty();
            }

            if (!info.packer_label.empty()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Found PACKED exe%s: EXE='%s' packer='%s' engine=%s",
                            cache_hit ? " [cached]" : "",
                            exe_path.c_str(), info.packer_label.c_str(),
                            FM2K::EngineName(info.engine));
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Found %s game%s: EXE='%s' xxh64=%016llx%s",
                            FM2K::EngineName(info.engine),
                            cache_hit ? " [cached]" : "",
                            exe_path.c_str(),
                            (unsigned long long)info.xxh64,
                            info.clean_label.empty() ? "" : (" — " + info.clean_label).c_str());
            }
            games.push_back(std::move(info));
            found_kgt_pair = true;
        }
        SDL_free(list);
    }

    // 1b) FM95 fallback: directories with no .kgt but with .player files
    //     alongside an .exe are likely FM95 prototype builds (e.g. CPW.exe).
    //     FM95 has no master KGT data file — character data lives in .player
    //     files loaded directly via cmdline.
    if (!found_kgt_pair) {
        int player_count = 0;
        char** player_list = SDL_GlobDirectory(dir.c_str(), "*.player", SDL_GLOB_CASEINSENSITIVE, &player_count);
        if (player_list && player_count > 0) {
            int exe_count = 0;
            char** exe_list = SDL_GlobDirectory(dir.c_str(), "*.exe", SDL_GLOB_CASEINSENSITIVE, &exe_count);
            if (exe_list) {
                for (int i = 0; i < exe_count; ++i) {
                    if (!exe_list[i]) continue;
                    const char* exe_basename = SDL_strrchr(exe_list[i], '/');
                    if (!exe_basename) exe_basename = SDL_strrchr(exe_list[i], '\\');
                    exe_basename = exe_basename ? exe_basename + 1 : exe_list[i];
                    std::string exe_path = dir + "\\" + exe_basename;

                    Utils::GameCacheEntry cached_entry;
                    bool cache_hit = TryUseCachedEntry(cache, exe_path, std::string(), cached_entry);
                    if (!cache_hit && !SDL_GetPathInfo(exe_path.c_str(), nullptr)) continue;

                    FM2K::FM2KGameInfo info{exe_path, "", 0, true};

                    if (cache_hit) {
                        info.xxh64        = cached_entry.exe_xxh64;
                        info.engine       = cached_entry.engine;
                        info.clean_label  = cached_entry.clean_label;
                        info.packer_label = cached_entry.packer_label;
                        info.is_clean     = cached_entry.is_clean;
                        // No kgt for the .player-only fallback — leave info.kgt default.
                    } else {
                        info.xxh64 = Utils::HashFileXXH64(exe_path);

                        if (const auto* known = Utils::FindKnownExe(info.xxh64)) {
                            info.engine      = known->engine;
                            info.clean_label = known->label;
                        } else if (auto sniffed = Utils::SniffEngineFromStrings(exe_path)) {
                            info.engine = *sniffed;
                        } else {
                            uint64_t sz = 0; int64_t mt = 0;
                            Utils::StatFile(exe_path, sz, mt);
                            // .player-fallback path biases toward FM95 since
                            // that's what triggered this branch, but defer to
                            // a clear FM2K size signal if the exe sits in the
                            // FM2K cluster.
                            info.engine = (sz > 600 * 1024) ? FM2K::Engine::FM2K
                                                           : FM2K::Engine::FM95;
                        }

                        info.packer_label = Utils::DetectPackerFromPE(exe_path);
                        info.is_clean     = info.packer_label.empty();
                    }

                    if (!info.packer_label.empty()) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                    "Found PACKED game (no .kgt)%s: EXE='%s' packer='%s' engine=%s",
                                    cache_hit ? " [cached]" : "",
                                    exe_path.c_str(), info.packer_label.c_str(),
                                    FM2K::EngineName(info.engine));
                    } else {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "Found %s game (no .kgt, %d .player files)%s: EXE='%s'",
                                    FM2K::EngineName(info.engine), player_count,
                                    cache_hit ? " [cached]" : "",
                                    exe_path.c_str());
                    }
                    games.push_back(std::move(info));
                }
                SDL_free(exe_list);
            }
        }
        if (player_list) SDL_free(player_list);
    }

    // 2) Recurse into subdirectories, decrementing depth budget.
    if (depth_remaining > 0) {
        DiscoveryWalkCtx ctx{&games, cache, depth_remaining - 1};
        SDL_EnumerateDirectory(dir.c_str(), DirectoryEnumerator, &ctx);
    }
}

static SDL_EnumerationResult DirectoryEnumerator(void* userdata, const char* origdir, const char* name) {
    auto* ctx = static_cast<DiscoveryWalkCtx*>(userdata);

    if (SDL_strcmp(name, ".") == 0 || SDL_strcmp(name, "..") == 0) {
        return SDL_ENUM_CONTINUE;
    }

    std::string dir_str = origdir;
    if (!dir_str.empty() && dir_str.back() != '/' && dir_str.back() != '\\') {
        dir_str += '/';
    }
    std::string full_path = dir_str + name;

    SDL_PathInfo info;
    if (!SDL_GetPathInfo(full_path.c_str(), &info)) {
        return SDL_ENUM_CONTINUE;
    }
    if (info.type != SDL_PATHTYPE_DIRECTORY) {
        return SDL_ENUM_CONTINUE;
    }

    ScanDirForGames(full_path, *ctx->games, ctx->cache, ctx->depth_remaining);
    return SDL_ENUM_CONTINUE;
}

// Walk one games root and produce a list of FM2K games found under it.
// Self-contained so std::async can run multiple roots in parallel without
// shared state beyond the read-only cache map.
static std::vector<FM2K::FM2KGameInfo> ScanOneRoot(
    const std::string& root,
    const std::unordered_map<std::string, Utils::GameCacheEntry>& cache)
{
    std::vector<FM2K::FM2KGameInfo> games;

    if (root.empty() || !SDL_GetPathInfo(root.c_str(), nullptr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Games root path is empty or does not exist: '%s'",
                    root.c_str());
        return games;
    }

    // Pull root and its descendants through the same scanner. Depth budget
    // is full DISCOVERY_MAX_DEPTH because the root itself counts as level 0.
    ScanDirForGames(root, games, &cache, DISCOVERY_MAX_DEPTH);
    return games;
}

const fm2k::KgtSummary* FM2KLauncher::FindKgtByGameId(const std::string& game_id) const {
    if (game_id.empty()) return nullptr;
    // Hub-side `game_id` is the exe stem (basename without extension).
    // Match case-insensitively because Windows filesystems are themselves
    // case-insensitive and the hub propagates whatever case the original
    // host used.
    auto ieq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower((unsigned char)a[i]) !=
                std::tolower((unsigned char)b[i])) return false;
        }
        return true;
    };
    for (const auto& g : discovered_games_) {
        if (!g.kgt.valid) continue;
        std::string stem = fm2k::utf8path::StemUtf8(
            std::filesystem::u8path(g.exe_path));
        if (ieq(stem, game_id)) return &g.kgt;
    }
    return nullptr;
}

std::vector<FM2K::FM2KGameInfo> FM2KLauncher::DiscoverGames() {
    // Snapshot the roots list — the launcher might mutate it while we walk.
    std::vector<std::string> roots = games_root_paths_;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Starting game discovery across %d root(s)", (int)roots.size());
    for (const auto& r : roots) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  root: '%s'", r.c_str());
    }

    // Load the on-disk cache once and share it (read-only) with all walkers.
    auto cache = Utils::LoadGameCacheMap();

    // Walk each root concurrently. std::async with a small per-root task is
    // the right granularity — the work is filesystem-bound, not CPU-bound,
    // so a thread pool would add complexity without buying anything.
    std::vector<std::future<std::vector<FM2K::FM2KGameInfo>>> futures;
    futures.reserve(roots.size());
    for (const auto& root : roots) {
        futures.push_back(std::async(std::launch::async, ScanOneRoot,
                                     root, std::cref(cache)));
    }

    // Collect, de-duping by canonical absolute exe path so two roots that
    // alias to the same install (symlink, junction, duplicate config entry)
    // don't show the same game twice.
    std::vector<FM2K::FM2KGameInfo> games;
    std::unordered_set<std::string> seen;
    for (auto& fut : futures) {
        std::vector<FM2K::FM2KGameInfo> partial = fut.get();
        for (auto& g : partial) {
            std::string key = Utils::CanonicalizePath(g.exe_path);
            if (!seen.insert(key).second) continue;
            games.push_back(std::move(g));
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "DiscoverGames: %d unique game(s) found across %d root(s)",
                (int)games.size(), (int)roots.size());
    return games;
}

bool FM2KLauncher::ValidateGameFiles(FM2K::FM2KGameInfo& game) {
    // Basic validation - check if executable exists and is readable
    if (!Utils::FileExists(game.exe_path)) {
        return false;
    }
    
    // TODO: Add more sophisticated validation
    game.is_host = true;
    return true;
}

std::string FM2KLauncher::DetectGameVersion(const std::string& exe_path SDL_UNUSED) {
    // TODO: Implement version detection based on file properties
    return "Unknown";
}

bool FM2KLauncher::LaunchGame(const FM2K::FM2KGameInfo& game) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Attempting to launch game: %s", game.exe_path.c_str());
    
    if (!game.is_host) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot launch invalid game - is_host flag is false");
        return false;
    }
    
    // Terminate existing game if running
    if (game_instance_ && game_instance_->IsRunning()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Terminating existing game instance before new launch");
        game_instance_->Terminate();
    }
    
    // Create new game instance
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating new FM2KGameInstance");
    game_instance_ = std::make_unique<FM2KGameInstance>();
    
    // Apply any pending configuration before launching
    ApplyPendingConfigToInstance(game_instance_.get());
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Launching game with EXE: %s, KGT: %s, engine=%s",
                 game.exe_path.c_str(), game.dll_path.c_str(),
                 FM2K::EngineName(game.engine));

    if (!game_instance_->Launch(game.exe_path, game.engine)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch game: %s", game.exe_path.c_str());
        game_instance_.reset();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game launched successfully: %s", game.exe_path.c_str());
    
    // Wait a moment and check if process is still running
    SDL_Delay(100);
    if (!game_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Game process terminated immediately after launch!");
        game_instance_.reset();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game process confirmed running after 100ms");
    
    return true;
}

void FM2KLauncher::TerminateGame() {
    if (game_instance_) {
        game_instance_->Terminate();
        game_instance_.reset();
        std::cout << "? Game terminated\n";
    }
}

void FM2KLauncher::StartOfflineSession() {
    if (selected_game_.exe_path.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot start offline session: no game selected.");
        return;
    }

    // Terminate existing game if running
    if (game_instance_ && game_instance_->IsRunning()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Terminating existing game instance before new launch");
        game_instance_->Terminate();
    }
    
    // Create new game instance
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating new FM2KGameInstance for offline session");
    game_instance_ = std::make_unique<FM2KGameInstance>();
    
    // Set environment variables for true offline mode
    game_instance_->SetEnvironmentVariable("FM2K_TRUE_OFFLINE", "1");
    game_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX", "0");  // Always P1 for offline
    game_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");  // Debug mode for now
    game_instance_->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "1");  // Enable input recording
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Set FM2K_TRUE_OFFLINE=1 for pure offline session");

    if (!game_instance_->Launch(selected_game_.exe_path, selected_game_.engine)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch game for offline session.");
        game_instance_.reset();
        return;
    }

    NetworkConfig local_config;
    local_config.session_mode = SessionMode::LOCAL;

    // Configure DLL for offline mode - shared memory enabled for debugging features
    if (game_instance_) {
        game_instance_->SetNetworkConfig(false, false);
    }

    SetState(LauncherState::InGame);
    std::cout << "? LOCAL session started (offline mode)\n";
}

// Launch a single game instance with GekkoStressSession enabled.
// No second instance, no networking. The hook DLL detects FM2K_STRESS_MODE=1
// and creates a GekkoStressSession with both players local; GekkoNet then
// artificially rewinds and re-simulates on a check_distance cadence, flagging
// any sim nondeterminism via the normal DESYNC event path.
// If the game survives a match without DESYNC firing, the save/load/tick
// pipeline is deterministic. If it fires, we have a pure local repro.
void FM2KLauncher::StartStressSession() {
    if (selected_game_.exe_path.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot start stress session: no game selected.");
        return;
    }

    if (game_instance_ && game_instance_->IsRunning()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Terminating existing game instance before stress launch");
        game_instance_->Terminate();
    }

    game_instance_ = std::make_unique<FM2KGameInstance>();

    // Env vars: stress mode ON, true-offline OFF (we still need GekkoNet running)
    game_instance_->SetEnvironmentVariable("FM2K_TRUE_OFFLINE", "0");
    game_instance_->SetEnvironmentVariable("FM2K_STRESS_MODE", "1");
    game_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX", "0");  // irrelevant in stress mode but keep consistent
    game_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");  // verbose logging so we see desync diagnostics

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Starting STRESS session: GekkoStressSession will force rollbacks "
        "every check_distance frames. Any DESYNC is a local determinism bug.");

    if (!game_instance_->Launch(selected_game_.exe_path, selected_game_.engine)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch game for stress session.");
        game_instance_.reset();
        return;
    }

    SetState(LauncherState::InGame);
    std::cout << "? STRESS session started (determinism check, single instance)\n";
}

void FM2KLauncher::StartOnlineSession(const NetworkConfig& config, bool is_host) {
    if (selected_game_.exe_path.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot start online session: no game selected.");
        return;
    }

    // Terminate existing game if running
    if (game_instance_ && game_instance_->IsRunning()) {
        game_instance_->Terminate();
    }

    // Create new instance and set env vars BEFORE launch
    game_instance_ = std::make_unique<FM2KGameInstance>();
    ApplyPendingConfigToInstance(game_instance_.get());

    uint8_t player_index = is_host ? 0 : 1;
    uint16_t local_port = static_cast<uint16_t>(config.local_port);

    // Remote address:
    //   - HUB-DRIVEN: match_start carries the peer's udp_addr in config
    //     for BOTH host and guest. Use it directly.
    //   - JOIN (legacy direct connect): user-pasted "ip:port" in
    //     config.remote_address.
    //   - HOST (legacy direct connect): leave empty so the hook
    //     listens on its socket and learns the peer's address from
    //     the first inbound HELLO. The default "127.0.0.1:7001" from
    //     NetworkConfig's ctor is a UI copy-button placeholder, not
    //     a real peer — clear it for legacy host.
    std::string remote_addr = config.remote_address;
    if (is_host && remote_addr == "127.0.0.1:7001") {
        remote_addr.clear();   // legacy-host placeholder; let hook learn
    }
    if (remote_addr.find(':') == std::string::npos && !remote_addr.empty()) {
        remote_addr += ":7500";  // fallback if user pasted a bare IP
    }

    game_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX", std::to_string(player_index));
    game_instance_->SetEnvironmentVariable("FM2K_LOCAL_PORT", std::to_string(local_port));
    game_instance_->SetEnvironmentVariable("FM2K_REMOTE_ADDR", remote_addr);

    // Auto-enable parity recorder for spectator-desync diagnosis. Each
    // process writes per-frame state snapshots (RNG, game_timer, render_fc,
    // etc.) to a .pty file. Diff host vs spectator post-run with
    // tools/kgt_diff_pty to find the first divergent frame. Skip the env
    // override if the user already set one (manual diagnostic flow).
    if (!std::getenv("FM2K_PARITY_RECORD_PATH")) {
        const std::string pty_path = "c:/games/2dfm/wanwan/parity_p"
            + std::to_string(player_index + 1) + ".pty";
        game_instance_->SetEnvironmentVariable("FM2K_PARITY_RECORD_PATH", pty_path);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Online session: parity recorder -> %s", pty_path.c_str());
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Online session: P%d port=%d remote=%s",
        player_index + 1, local_port, remote_addr.c_str());

    // Bake the host's resolved [GamePlay] config (defaults + per-game
    // overrides + online anti-cheat clamps) into the game's own
    // game.ini before CreateProcess. The game reads this file at
    // startup; by writing it now both peers boot with the same round
    // count / time / stage / etc. We restore the original ini in
    // StopSession so leaving the launcher doesn't permanently mutate
    // the user's offline settings. is_online=true forces HitJudge +
    // GameInformation = 0 (debug overlays are cheating online).
    fm2k::game_ini::ApplyForLaunch(selected_game_.exe_path,
                                    /*is_online=*/true);

    if (!game_instance_->Launch(selected_game_.exe_path, selected_game_.engine)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch game for online session.");
        game_instance_.reset();
        return;
    }

    network_config_ = config;
    SetState(LauncherState::Connecting);
    std::cout << "? ONLINE session started (" << (is_host ? "Hosting" : "Joining") << ")\n";
}

void FM2KLauncher::StopSession() {
    // DLL handles GekkoNet directly - no launcher-side session needed
    std::cout << "? Session stopped\n";
    // Tell the hub the match ended BEFORE we tear the local instance
    // down. Hub flips both peers' status back to "idle" and
    // broadcasts user_status to the rest of the room — without this
    // the lobby sticks at "in_match" and Challenge stays disabled
    // until the user reconnects.
    if (ui_) {
        ui_->NotifyHubMatchEnded();
    }
    if (game_instance_) {
        game_instance_->Terminate();
        game_instance_.reset();
    }
    // Restore the game's pristine game.ini from the .fm2krollback_bak
    // backup ApplyForLaunch made. No-op when there's no backup (we
    // never overrode anything for this game). Done after Terminate so
    // we don't race the game holding its own ini open.
    if (!selected_game_.exe_path.empty()) {
        fm2k::game_ini::RestoreFromBackup(selected_game_.exe_path);
    }
    SetState(LauncherState::GameSelection);
}

void FM2KLauncher::SetSelectedGame(const FM2K::FM2KGameInfo& game) {
    selected_game_ = game;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game selected via code: %s", game.exe_path.c_str());
}

void FM2KLauncher::SetGamesRootPaths(const std::vector<std::string>& paths) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Set games root paths: %d entry/entries", (int)paths.size());
    for (const auto& p : paths) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  - %s", p.c_str());
    }
    games_root_paths_ = paths;
    Utils::SaveGamesRootPaths(paths);
    if (ui_) ui_->SetGamesRootPaths(paths);  // Update UI with new paths

    // Kick off background discovery so the UI stays responsive
    StartAsyncDiscovery();
}

void FM2KLauncher::SetState(LauncherState state) {
    current_state_ = state;
    if (ui_) {
        ui_->SetLauncherState(state);
    }
}

// Multi-client testing implementation
bool FM2KLauncher::LaunchLocalClient(const std::string& game_path, bool is_host, int port) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launching local client: %s (Host: %s, Port: %d)", 
                game_path.c_str(), is_host ? "Yes" : "No", port);
    
    // Check if game instance is already running
    std::unique_ptr<FM2KGameInstance>* target_instance = is_host ? &client1_instance_ : &client2_instance_;
    if (*target_instance && (*target_instance)->IsRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Client %d already running", is_host ? 1 : 2);
        return false;
    }
    
    // Use the provided game path directly (user has manually set up wanwan2 if needed)
    std::string actual_game_path = game_path;
    
    // Create new game instance
    *target_instance = std::make_unique<FM2KGameInstance>();
    
    // Apply any pending configuration before launching
    ApplyPendingConfigToInstance(target_instance->get());
    
    // Configure GekkoNet session coordination for this client
    uint8_t player_index = is_host ? 0 : 1;  // Host = Player 0, Guest = Player 1
    
    // FIXED: Use correct networking configuration while keeping non-network variables identical
    // Set environment variables BEFORE launching process (OnlineSession style)
    (*target_instance)->SetEnvironmentVariable("FM2K_PLAYER_INDEX", std::to_string(player_index));  // Host=0, Guest=1
    (*target_instance)->SetEnvironmentVariable("FM2K_LOCAL_PORT", std::to_string(port));  // Keep port different (required for networking)
    (*target_instance)->SetEnvironmentVariable("FM2K_REMOTE_ADDR", "127.0.0.1:" + std::to_string(is_host ? 7001 : 7000));  // Restore correct remote addressing
    
    // Add production mode and input recording settings
    (*target_instance)->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");  // Default to debug mode for now
    (*target_instance)->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "1");  // Enable input recording by default
    
    // CRITICAL: Force identical RNG seed for both clients to prevent desync
    (*target_instance)->SetEnvironmentVariable("FM2K_FORCE_RNG_SEED", "12345678");  // Fixed seed for testing
    
    // Launch clients simultaneously - no delay needed
    // The GekkoNet synchronization will handle timing differences
    if (!is_host) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Guest client launching immediately (no delay)");
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launching FM2K game with OnlineSession-style config: %s", actual_game_path.c_str());
    
    // Launch the actual FM2K game process with hook injection
    if (!(*target_instance)->Launch(actual_game_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch FM2K game: %s", actual_game_path.c_str());
        target_instance->reset();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client launched successfully - Player %u, Port %d", player_index, port);
    
    // Wait a moment and check if process is still running
    SDL_Delay(100);
    if (!(*target_instance)->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "FM2K game process terminated immediately after launch!");
        target_instance->reset();
        return false;
    }
    

    
    // Store process ID for status tracking
    uint32_t* target_pid = is_host ? &client1_process_id_ : &client2_process_id_;
    *target_pid = (*target_instance)->GetProcessId();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Client %d (FM2K game) launched successfully (PID: %u)", 
                is_host ? 1 : 2, *target_pid);
    
    return true;
}

bool FM2KLauncher::LaunchLocalSpectator(const std::string& game_path,
                                        int spectator_port,
                                        int host_port,
                                        const std::string& mode)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Launching local spectator: %s (port=%d -> host_port=%d)",
                game_path.c_str(), spectator_port, host_port);

    if (spectator_instance_ && spectator_instance_->IsRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Spectator already running");
        return false;
    }

    spectator_instance_ = std::make_unique<FM2KGameInstance>();
    ApplyPendingConfigToInstance(spectator_instance_.get());

    // Spectator-mode env vars. The hook reads FM2K_SPECTATOR_MODE=1 to skip
    // the normal HELLO/HELLO_ACK flow and instead send SPEC_JOIN_REQ to
    // FM2K_REMOTE_ADDR after the socket is up. Player index 2 is just a
    // sentinel — spectators don't claim a player slot.
    spectator_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX",   "2");
    spectator_instance_->SetEnvironmentVariable("FM2K_LOCAL_PORT",     std::to_string(spectator_port));
    spectator_instance_->SetEnvironmentVariable("FM2K_REMOTE_ADDR",    "127.0.0.1:" + std::to_string(host_port));
    spectator_instance_->SetEnvironmentVariable("FM2K_SPECTATOR_MODE", "1");
    spectator_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    spectator_instance_->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "0");
    spectator_instance_->SetEnvironmentVariable("FM2K_FORCE_RNG_SEED",  "12345678");
    {
        const std::string normalized =
            (mode == "full" || mode == "FULL" || mode == "FULL_SESSION") ? "full" : "current";
        spectator_instance_->SetEnvironmentVariable("FM2K_SPECTATE_MODE", normalized);
    }
    if (!std::getenv("FM2K_PARITY_RECORD_PATH")) {
        spectator_instance_->SetEnvironmentVariable("FM2K_PARITY_RECORD_PATH",
            "c:/games/2dfm/wanwan/parity_p3.pty");
    }

    if (!spectator_instance_->Launch(game_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch spectator: %s", game_path.c_str());
        spectator_instance_.reset();
        return false;
    }

    SDL_Delay(100);
    if (!spectator_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Spectator process terminated immediately!");
        spectator_instance_.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spectator launched successfully (PID: %u, port=%d -> host=127.0.0.1:%d)",
                spectator_instance_->GetProcessId(), spectator_port, host_port);
    return true;
}

bool FM2KLauncher::LaunchRemoteSpectator(const std::string& game_path,
                                         int spectator_port,
                                         const std::string& host_ip,
                                         int host_port,
                                         const std::string& session_kind,
                                         const std::string& mode,
                                         const std::string& spec_transport)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Launching remote spectator: %s (port=%d -> %s:%d, mode=%s, "
                "session_kind=%s, transport=%s)",
                game_path.c_str(), spectator_port, host_ip.c_str(), host_port,
                mode.c_str(), session_kind.c_str(), spec_transport.c_str());

    if (spectator_instance_ && spectator_instance_->IsRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Spectator already running");
        return false;
    }

    spectator_instance_ = std::make_unique<FM2KGameInstance>();
    ApplyPendingConfigToInstance(spectator_instance_.get());

    const std::string remote_addr = host_ip + ":" + std::to_string(host_port);
    spectator_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX",    "2");
    spectator_instance_->SetEnvironmentVariable("FM2K_LOCAL_PORT",      std::to_string(spectator_port));
    spectator_instance_->SetEnvironmentVariable("FM2K_REMOTE_ADDR",     remote_addr);
    spectator_instance_->SetEnvironmentVariable("FM2K_SPECTATOR_MODE",  "1");
    spectator_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    spectator_instance_->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "0");
    spectator_instance_->SetEnvironmentVariable("FM2K_FORCE_RNG_SEED",  "12345678");
    // Phase 4: auto-derived spec transport from host's spectate_grant.
    // Setting "tcp" explicitly clears any inherited relay env from a
    // previous spec session; setting "relay" enables the hub-relay
    // data plane in the spec hook. User no longer needs to set this
    // env manually -- it's negotiated via hub.
    if (spec_transport == "tcp" || spec_transport == "relay") {
        spectator_instance_->SetEnvironmentVariable("FM2K_SPEC_TRANSPORT", spec_transport);
    }
    {
        const std::string normalized =
            (mode == "full" || mode == "FULL" || mode == "FULL_SESSION") ? "full" : "current";
        spectator_instance_->SetEnvironmentVariable("FM2K_SPECTATE_MODE", normalized);

        // /F boot-to-battle for spectators — conditional on host's
        // current session_kind (forwarded by the hub in spectate_grant,
        // sourced from the host hook's published game_mode transitions
        // via SharedMem). Two cases:
        //
        //  - host in "battle": set /F so the spec engine's slot-0
        //    dispatcher fires `create_game_object(14, 127, 0, 0)`
        //    straight into battle (skips CSS). SpectatorNode then
        //    overlays the host's snapshot — chars, positions, RNG,
        //    everything — and sim-forwards inputs to live. ~1s join
        //    instead of ~5s title→CSS→battle walk.
        //
        //  - host in "menu" / "css": do NOT set /F. Spec walks the
        //    normal title → CSS path; the CSS-snapshot mid-CSS-join
        //    handshake (Phase E) syncs chars/cursor state. /F here
        //    would land spec in battle with placeholder chars and
        //    no battle-state snapshot to apply, crashing the engine
        //    when the eventual mode 2000→3000 transition fails.
        //
        // Older hubs / pre-session_kind clients default to "menu"
        // (no /F). This is the safe default — worst case the spec
        // joins via title walk instead of boot-to-battle (slower
        // join, never a crash).
        const bool boot_to_battle = (session_kind == "battle");
        if (boot_to_battle) {
            spectator_instance_->SetEnvironmentVariable("FM2K_BOOT_TO_BATTLE", "1");
            // Placeholder chars / stage for the /F slot-0 dispatcher's
            // battle init. The snapshot apply overlays the real values
            // from the host's saved blob, so the placeholders only
            // affect the engine's INITIAL battle frame state (which
            // SaveState_LoadFromBytes immediately overwrites).
            spectator_instance_->SetEnvironmentVariable("FM2K_BTB_P1_CHAR", "0");
            spectator_instance_->SetEnvironmentVariable("FM2K_BTB_P2_CHAR", "0");
            spectator_instance_->SetEnvironmentVariable("FM2K_BTB_STAGE",   "0");
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spec: host in battle — set FM2K_BOOT_TO_BATTLE=1");
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spec: host in %s — walking normal title→CSS path (no /F)",
                session_kind.c_str());
        }
    }
    // Default spectator parity path -- but respect an explicit
    // FM2K_PARITY_RECORD_PATH from the environment (the spec_selftest
    // harness routes the .pty into its own workspace; the unconditional
    // override silently sent it to parity_p3.pty instead).
    if (!std::getenv("FM2K_PARITY_RECORD_PATH")) {
        spectator_instance_->SetEnvironmentVariable("FM2K_PARITY_RECORD_PATH",
            "c:/games/2dfm/wanwan/parity_p3.pty");
    }

    if (!spectator_instance_->Launch(game_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch remote spectator: %s",
                     game_path.c_str());
        spectator_instance_.reset();
        return false;
    }

    SDL_Delay(100);
    if (!spectator_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Remote spectator process terminated immediately!");
        spectator_instance_.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Remote spectator launched (PID: %u, port=%d -> host=%s)",
                spectator_instance_->GetProcessId(), spectator_port, remote_addr.c_str());
    return true;
}

bool FM2KLauncher::LaunchReplayPlayer(const std::string& game_path,
                                      const std::string& replay_path)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Launching replay player: %s (replay=%s)",
                game_path.c_str(), replay_path.c_str());

    if (spectator_instance_ && spectator_instance_->IsRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Replay player already running");
        return false;
    }

    spectator_instance_ = std::make_unique<FM2KGameInstance>();
    ApplyPendingConfigToInstance(spectator_instance_.get());

    // Replay-mode env vars. Hook reads FM2K_REPLAY_FILE in
    // Netplay_InitAsSpectator and short-circuits the network setup,
    // calling SpectatorNode_LoadSessionFile to populate pb_queue from
    // disk. Trampoline's RunSpectatorTick drains it the same way it
    // drains live-wire events. PLAYER_INDEX=2 is the spectator sentinel.
    spectator_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX",    "2");
    spectator_instance_->SetEnvironmentVariable("FM2K_SPECTATOR_MODE",  "1");
    spectator_instance_->SetEnvironmentVariable("FM2K_REPLAY_FILE",     replay_path);
    spectator_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    spectator_instance_->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "0");

    if (!spectator_instance_->Launch(game_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to launch replay player: %s",
                     game_path.c_str());
        spectator_instance_.reset();
        return false;
    }

    SDL_Delay(100);
    if (!spectator_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Replay player process terminated immediately!");
        spectator_instance_.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Replay player launched (PID: %u, replay=%s)",
                spectator_instance_->GetProcessId(), replay_path.c_str());
    return true;
}

bool FM2KLauncher::LaunchLocalSpectator2(const std::string& game_path,
                                         int spectator_port,
                                         int upstream_port)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Launching local spectator 2 (chain): %s (port=%d -> upstream_port=%d)",
                game_path.c_str(), spectator_port, upstream_port);

    if (spectator2_instance_ && spectator2_instance_->IsRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Spectator 2 already running");
        return false;
    }

    spectator2_instance_ = std::make_unique<FM2KGameInstance>();
    ApplyPendingConfigToInstance(spectator2_instance_.get());

    // Same env-var shape as the first spectator. The only difference is
    // FM2K_REMOTE_ADDR points at spectator 1's port (7002) instead of the
    // host's (7000). Spectator 1 acts as upstream; on JOIN_REQ it accepts
    // and starts shipping its session_history (which it has been
    // accumulating from its OWN relay path) to spectator 2.
    spectator2_instance_->SetEnvironmentVariable("FM2K_PLAYER_INDEX",   "3");
    spectator2_instance_->SetEnvironmentVariable("FM2K_LOCAL_PORT",     std::to_string(spectator_port));
    spectator2_instance_->SetEnvironmentVariable("FM2K_REMOTE_ADDR",    "127.0.0.1:" + std::to_string(upstream_port));
    spectator2_instance_->SetEnvironmentVariable("FM2K_SPECTATOR_MODE", "1");
    spectator2_instance_->SetEnvironmentVariable("FM2K_PRODUCTION_MODE", "0");
    spectator2_instance_->SetEnvironmentVariable("FM2K_INPUT_RECORDING", "0");
    spectator2_instance_->SetEnvironmentVariable("FM2K_FORCE_RNG_SEED",  "12345678");

    if (!spectator2_instance_->Launch(game_path)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to launch spectator 2: %s", game_path.c_str());
        spectator2_instance_.reset();
        return false;
    }

    SDL_Delay(100);
    if (!spectator2_instance_->IsRunning()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Spectator 2 process terminated immediately!");
        spectator2_instance_.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spectator 2 launched (PID: %u, port=%d -> upstream=127.0.0.1:%d)",
                spectator2_instance_->GetProcessId(), spectator_port, upstream_port);
    return true;
}

bool FM2KLauncher::TerminateAllClients() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminating all local clients");
    
    bool success = true;
    
    // Terminate client 1
    if (client1_instance_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminating Client 1 (PID: %u)", client1_instance_->GetProcessId());
        client1_instance_->Terminate();
        client1_instance_.reset();
        client1_process_id_ = 0;
    }
    
    // Terminate client 2
    if (client2_instance_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminating Client 2 (PID: %u)", client2_instance_->GetProcessId());
        client2_instance_->Terminate();
        client2_instance_.reset();
        client2_process_id_ = 0;
    }

    // Terminate spectator
    if (spectator_instance_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminating Spectator (PID: %u)", spectator_instance_->GetProcessId());
        spectator_instance_->Terminate();
        spectator_instance_.reset();
    }

    // Terminate spectator 2 (daisy-chain)
    if (spectator2_instance_) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Terminating Spectator 2 (PID: %u)", spectator2_instance_->GetProcessId());
        spectator2_instance_->Terminate();
        spectator2_instance_.reset();
    }

    return success;
}


bool FM2KLauncher::ReadRollbackStatsFromSharedMemory(RollbackStats& stats) {
    // Try to read from both client processes (prioritize the first active one)
    auto try_read_stats = [&stats](DWORD process_id) -> bool {
        if (process_id == 0) return false;

        std::string shared_memory_name = "FM2K_SharedMem_" + std::to_string(process_id);
        HANDLE shared_memory_handle = OpenFileMappingA(FILE_MAP_READ, FALSE, shared_memory_name.c_str());
        if (!shared_memory_handle) return false;

        FM2KSharedMemData* shared_data = static_cast<FM2KSharedMemData*>(
            MapViewOfFile(shared_memory_handle, FILE_MAP_READ, 0, 0, sizeof(FM2KSharedMemData))
        );

        bool ok = false;
        if (shared_data && shared_data->magic == FM2K_SHARED_MEM_MAGIC) {
            stats.rollbacks_per_second = 0;                        // not available in new struct
            stats.max_rollback_frames = 0;                         // not available
            stats.avg_rollback_frames = 0;                         // not available
            stats.frame_advantage = shared_data->frames_ahead;
            stats.input_delay_frames = 2;                          // placeholder
            stats.confirmed_frames = shared_data->frame_number;
            stats.speculative_frames = shared_data->rollback_count;
            ok = true;
        }

        if (shared_data) UnmapViewOfFile(shared_data);
        CloseHandle(shared_memory_handle);
        return ok;
    };

    // Check Client 1 first, then Client 2
    if (try_read_stats(client1_process_id_)) return true;
    if (try_read_stats(client2_process_id_)) return true;
    return false;
}

// Apply pending configuration to a game instance
void FM2KLauncher::ApplyPendingConfigToInstance(FM2KGameInstance* instance) {
    if (!instance) {
        return;
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Applying pending configuration to game instance");
    
    // Apply MinimalGameState testing config
    if (pending_config_.has_minimal_gamestate_testing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Applying pending MinimalGameState testing: %s", 
                   pending_config_.minimal_gamestate_testing_value ? "enabled" : "disabled");
        instance->SetMinimalGameStateTesting(pending_config_.minimal_gamestate_testing_value);
    }
    
    // Apply production mode config
    if (pending_config_.has_production_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Applying pending production mode: %s", 
                   pending_config_.production_mode_value ? "enabled" : "disabled");
        instance->SetProductionMode(pending_config_.production_mode_value);
    }
    
    // Apply input recording config
    if (pending_config_.has_input_recording) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Applying pending input recording: %s", 
                   pending_config_.input_recording_value ? "enabled" : "disabled");
        instance->SetInputRecording(pending_config_.input_recording_value);
    }
} 

