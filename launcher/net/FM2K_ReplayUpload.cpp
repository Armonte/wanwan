#include "FM2K_ReplayUpload.h"

#include "FM2K_HttpPost.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace fm2k::replay_upload {
namespace {

// The hook finalises the session file after the match ends, so uploading
// immediately would read a partial file or miss it entirely. Waiting is
// simpler and more robust than trying to detect the write, and nobody is
// watching for their replay to appear within two seconds of a set.
constexpr int64_t kSettleMs = 4000;

// Never send more than one set's worth. The hub refuses oversized uploads
// anyway; this keeps a pathological game folder from trying.
constexpr size_t kMaxFiles = 24;
constexpr size_t kMaxTotal = 24u * 1024u * 1024u;

struct Pending {
    bool     armed = false;
    int64_t  at_ms = 0;
    Config   cfg;
};

// One upload at a time. The flag is the only thing the worker and the UI
// thread share.
std::atomic<bool> g_inflight{false};

Pending& State() {
    static Pending p;
    return p;
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// The newest .fm2kset, plus the .fm2krep slices written alongside it.
// Chosen by write time rather than by name: the launcher does not know
// what the hook called the file, and the newest one after a match is the
// match that just ended.
bool CollectNewest(const fs::path& game_dir,
                   std::vector<fm2k::http::File>& out,
                   std::string& stem_out) {
    std::error_code ec;
    fs::path best;
    fs::file_time_type best_t{};
    const fs::path sessions = game_dir / "sessions";
    if (!fs::is_directory(sessions, ec)) return false;
    for (const auto& e : fs::directory_iterator(sessions, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".fm2kset") continue;
        const auto t = fs::last_write_time(e.path(), ec);
        if (ec) continue;
        if (best.empty() || t > best_t) { best = e.path(); best_t = t; }
    }
    if (best.empty()) return false;

    size_t total = 0;
    const std::string set_bytes = ReadFile(best);
    if (set_bytes.empty()) return false;
    total += set_bytes.size();
    out.push_back({best.filename().string(), set_bytes});

    // The slices share the session's timestamp prefix -- "<stamp>_pN".
    stem_out = best.filename().string();
    const size_t us = stem_out.find('_', stem_out.find('_') + 1);
    const std::string prefix = (us == std::string::npos) ? stem_out : stem_out.substr(0, us);

    const fs::path replays = game_dir / "replays";
    if (fs::is_directory(replays, ec)) {
        for (const auto& e : fs::directory_iterator(replays, ec)) {
            if (out.size() >= kMaxFiles) break;
            if (!e.is_regular_file(ec)) continue;
            if (e.path().extension() != ".fm2krep") continue;
            if (e.path().filename().string().rfind(prefix, 0) != 0) continue;
            const std::string b = ReadFile(e.path());
            if (b.empty() || total + b.size() > kMaxTotal) continue;
            total += b.size();
            out.push_back({e.path().filename().string(), b});
        }
    }
    return !out.empty();
}

void Upload(const Config& cfg);

}  // namespace

void OnMatchEnded(const Config& cfg) {
    if (!cfg.enabled || cfg.game_dir.empty() || cfg.game_id.empty() ||
        cfg.upload_url.empty()) {
        return;
    }
    Pending& p = State();
    p.armed = true;
    p.at_ms = NowMs() + kSettleMs;
    p.cfg   = cfg;
}

bool Process() {
    Pending& p = State();
    if (!p.armed || NowMs() < p.at_ms) return false;
    if (g_inflight.load()) return false;
    p.armed = false;

    // OFF THE UI THREAD. PostMultipart is a synchronous WinHTTP call with
    // a 15 second timeout, and this is driven from the launcher's render
    // tick -- doing it inline freezes the window for the whole upload.
    // The diagnostics uploader learned this the expensive way and its
    // comment says so; this does not get to repeat it. The config is
    // copied by value so the worker never touches launcher state.
    const Config cfg = p.cfg;
    g_inflight.store(true);
    std::thread([cfg]() {
        Upload(cfg);
        g_inflight.store(false);
    }).detach();
    return true;
}

namespace {

void Upload(const Config& cfg_in) {
    const Config& p_cfg = cfg_in;
    std::vector<fm2k::http::File> files;
    std::string stem;
    if (!CollectNewest(fs::path(p_cfg.game_dir), files, stem)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "ReplayUpload: nothing to send for '%s'", p_cfg.game_id.c_str());
        return;
    }

    char meta[768];
    std::snprintf(meta, sizeof meta,
        "{\"game_id\":\"%s\",\"p1\":\"%s\",\"p2\":\"%s\",\"files\":%d}",
        JsonEscape(p_cfg.game_id).c_str(),
        JsonEscape(p_cfg.p1_nick).c_str(),
        JsonEscape(p_cfg.p2_nick).c_str(),
        (int)files.size());

    fm2k::http::Response resp;
    // No secret: replay upload is deliberately unauthenticated so anyone
    // can contribute to a game's list.
    const bool ok = fm2k::http::PostMultipart(p_cfg.upload_url, "", meta, files, resp);
    if (ok && resp.status == 200) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "ReplayUpload: sent %d file(s) for '%s'",
                    (int)files.size(), p_cfg.game_id.c_str());
        return;
    }
    // Not retried. A replay that fails to upload is a missing row in a
    // browsable list, not lost data -- the files stay on disk and the
    // local replay browser still shows them. Retrying would mean a queue
    // and a dedup key, which is not worth it for that.
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "ReplayUpload: failed for '%s' (status=%d at=%s)",
                p_cfg.game_id.c_str(), resp.status, resp.failed_at);
}

}  // namespace

}  // namespace fm2k::replay_upload
