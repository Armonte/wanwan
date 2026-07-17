// game_discovery_sniff.cpp -- exe/engine identification (split from
// game_discovery.cpp): xxhash + known-build registry + string/size sniff +
// PE packer detect. The scan core calls these on a cache miss. KnownExe +
// the decls live in game_discovery_internal.h.
#include "game_discovery.h"
#include "game_discovery_internal.h"
#include "FM2K_Integration.h"
#include "SDL3/SDL.h"
#define XXH_INLINE_ALL
#include "vendored/xxhash/xxhash.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

namespace Utils {

    uint64_t HashFileXXH64(const std::string& path) {
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
    static constexpr KnownExe kKnownExes[] = {
        // FM2K builds
        {0x506FF9AB93D15134ULL, FM2K::Engine::FM2K, "WonderfulWorld v0.946"},
        // FM95 builds
        {0x36358AD6F9EC387BULL, FM2K::Engine::FM95, "Comic Party Wars (CPW)"},
    };

    // Look up an exe hash. Returns nullptr on miss.
    const KnownExe* FindKnownExe(uint64_t hash) {
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
    std::optional<FM2K::Engine> SniffEngineFromStrings(const std::string& path) {
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
    FM2K::Engine GuessEngineFromSize(uint64_t size_bytes) {
        if (size_bytes <= 600 * 1024)                    return FM2K::Engine::FM95;
        return FM2K::Engine::FM2K;  // default — covers the 1.1-1.7 MB FM2K cluster
    }

    // One-shot engine identification for a single exe. CLI direct-path
    // launches (--stress/--offline/--host/--connect <path>) bypass the
    // DiscoverGames scan entirely, so they need the same cascade the scan
    // runs on a cache miss: known-hash registry -> engine string sniff ->
    // file-size heuristic. Reads the exe twice (full hash + 256 KB head);
    // fine for a once-per-launch call.
    FM2K::Engine DetectEngineForExe(const std::string& exe_path) {
        if (const KnownExe* known = FindKnownExe(HashFileXXH64(exe_path))) {
            return known->engine;
        }
        if (auto sniffed = SniffEngineFromStrings(exe_path)) {
            return *sniffed;
        }
        uint64_t size = 0;
        int64_t  mtime = 0;
        if (StatFile(exe_path, size, mtime)) {
            return GuessEngineFromSize(size);
        }
        return FM2K::Engine::FM2K;  // unreadable exe -- launch will fail anyway
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
    std::string DetectPackerFromPE(const std::string& path) {
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

}  // namespace Utils
