// Game discovery: filesystem scan, exe/engine sniffing, games-cache I/O, and
// the async discovery worker. Extracted VERBATIM from FM2K_RollbackClient.cpp
// (pure move, no behavior change). See game_discovery.h for the surface the
// launcher calls back into.
#include "game_discovery.h"
#include "game_discovery_internal.h"

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


// Custom SDL event sent from the worker thread once discovery finishes.
Uint32 g_event_discovery_complete = 0;  // extern in game_discovery.h

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
    // line-by-line reader handles transparently -- that's our migration story.
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
    // silently dropped -- first warm rescan after the upgrade pays the full
    // cost once, then writes the new binary format. No migration needed.
    // -------------------------------------------------------------


    std::string GetCacheFilePath() {
        return GetConfigDir() + "games.cache";
    }

    // Stat helper. Returns false if the file doesn't exist or we can't
    // read its size/mtime. mtime uses SDL_PathInfo's modify_time (ns since
    // epoch, monotonic-ish across runs) so cache hits don't fight the
    // OS's clock skew on resumed-from-sleep machines.
    bool StatFile(const std::string& path, uint64_t& size, int64_t& mtime) {
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(path.c_str(), &info)) return false;
        if (info.type != SDL_PATHTYPE_FILE) return false;
        size  = static_cast<uint64_t>(info.size);
        mtime = static_cast<int64_t>(info.modify_time);
        return true;
    }


    // UTF-8 ↔ wide via Win32 so std::filesystem doesn't go through the
    // system ANSI codepage (CP1252 on most non-Japanese installs),
    // which silently rewrites unrepresentable codepoints -- full-width
    // forms ＣＰＷ (U+FF23/FF30/FF37) become '_' or '?' on the trip
    // through path::string(). Use wide internally and convert at the
    // boundaries so the launcher renders / caches the original bytes.
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
// `kgt_path` may be empty for FM95 .player-only games -- in that case the
// kgt-stat check is skipped.
//
// Returns true on a full hit; the caller can then populate FM2KGameInfo
// directly from `out` and skip xxh64, kgt parse, engine sniff, packer
// sniff -- i.e. zero file I/O for this game.
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
            // as confusing -- Windows users see "C:\games" for their
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
                                 "KGT parse failed for '%s' -- dropdowns will be empty",
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
                            info.clean_label.empty() ? "" : (" -- " + info.clean_label).c_str());
            }
            games.push_back(std::move(info));
            found_kgt_pair = true;
        }
        SDL_free(list);
    }

    // 1b) FM95 fallback: directories with no .kgt but with .player files
    //     alongside an .exe are likely FM95 prototype builds (e.g. CPW.exe).
    //     FM95 has no master KGT data file -- character data lives in .player
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
                        // No kgt for the .player-only fallback -- leave info.kgt default.
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
    // Snapshot the roots list -- the launcher might mutate it while we walk.
    std::vector<std::string> roots = games_root_paths_;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Starting game discovery across %d root(s)", (int)roots.size());
    for (const auto& r : roots) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  root: '%s'", r.c_str());
    }

    // Load the on-disk cache once and share it (read-only) with all walkers.
    auto cache = Utils::LoadGameCacheMap();

    // Walk each root concurrently. std::async with a small per-root task is
    // the right granularity -- the work is filesystem-bound, not CPU-bound,
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
