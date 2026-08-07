// game_discovery_cache.cpp -- games.cache binary serialization +
// path canonicalization. Split from game_discovery.cpp (pure move).
// Shares GameCacheEntry + GetCacheFilePath/StatFile via the internal header.
#include "game_discovery.h"

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
#include "game_discovery_internal.h"

namespace Utils {

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

    std::string CanonicalizePath(const std::string& p) {
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
    // Native little-endian on Windows x86 -- the cache is local to the user
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
        if (n > cap) return false;            // sanity cap -- refuse > 16 MB strings
        s.assign(n, '\0');
        if (n == 0) return true;
        return (bool)i.read(s.data(), (std::streamsize)n);
    }

    static constexpr char     kCacheMagic[4] = {'F','M','2','K'};
    static constexpr uint32_t kCacheVersion  = 2;

    void SaveGameCache(const std::vector<FM2K::FM2KGameInfo>& games) {
        // Atomic-ish: write to .tmp then rename, so a crash mid-write
        // doesn't leave a half-cooked cache that fails the magic check
        // on the next launch (cosmetic -- we'd just rebuild -- but the
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
            // ignored -- next save overwrites with the new binary format.
            return map;
        }
        uint32_t version = 0;
        if (!ReadU32(in, version) || version != kCacheVersion) {
            return map;  // future format -- rebuild
        }
        uint32_t entry_count = 0;
        if (!ReadU32(in, entry_count) || entry_count > 100000) {
            return map;  // sanity cap -- refuse pathological counts
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
    // shows a populated games list -- including character/stage dropdowns --
    // BEFORE the async directory walk runs. Net effect: the launcher feels
    // instant on warm starts; the background walk only matters when the
    // user has actually added or removed a game since the last run.
    //
    // Cheap existence check per entry -- skip games whose exe was deleted
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

}  // namespace Utils
