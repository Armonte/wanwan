// FM2K_Updater -- auto-updater glue. See FM2K_Updater.h.

#include "FM2K_Updater.h"
#include "version_local.h"

#include <SDL3/SDL_log.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>   // SHGetSpecialFolderPathA, CSIDL_APPDATA

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace fm2k::updater {

namespace {

// ---------------------------------------------------------------------------
// State (file-static, accessed from worker thread + UI thread under mutex).
// ---------------------------------------------------------------------------

struct InternalState {
    Snapshot           snap;
    std::string        zip_path;        // %TEMP%\fm2k_v<ver>.zip after download
    std::thread        worker;
    std::atomic<bool>  busy{false};     // a check or download is in flight
    std::atomic<bool>  cancel{false};
    mutable std::mutex mtx;
};

InternalState g_st;

// ---------------------------------------------------------------------------
// Update channel. Three nested tiers, each a superset of the one below:
//   stable   (0) = GH non-prerelease only.
//   dev      (1) = stable + plain prereleases (NOT bleeding).
//   bleeding (2) = everything, including -bleeding-tagged prereleases.
// Tier is encoded on the GitHub release: stable = prerelease:false; dev =
// prerelease:true with a plain tag (v0.2.58); bleeding = prerelease:true
// with a "-bleeding" suffix on the tag (v0.2.58-bleeding). The numeric
// version compare (CompareVersions) ignores the suffix, so a bleeding cut
// just needs a version >= the stable/dev it sits above (bump it ahead).
// Persisted to dev_flags.ini alongside other launcher settings so the
// in-process launcher UI and this updater share the same value via the
// same file. Key = "update_channel", int value: 0 = stable, 1 = dev,
// 2 = bleeding.
// ---------------------------------------------------------------------------

enum class Channel : int { Stable = 0, Dev = 1, Bleeding = 2 };

std::string DevFlagsIniPath() {
    char buf[MAX_PATH] = {};
    if (!SHGetSpecialFolderPathA(nullptr, buf, CSIDL_APPDATA, FALSE) || !buf[0]) {
        return {};
    }
    std::string dir = std::string(buf) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\dev_flags.ini";
}

Channel ReadUpdateChannel() {
    const std::string path = DevFlagsIniPath();
    if (path.empty()) return Channel::Stable;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return Channel::Stable;
    char line[128];
    int result = 0;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        if (s.substr(0, eq) != "update_channel") continue;
        result = std::atoi(s.substr(eq + 1).c_str());
    }
    std::fclose(f);
    switch (result) {
        case 2:  return Channel::Bleeding;
        case 1:  return Channel::Dev;
        default: return Channel::Stable;
    }
}

void SetState(State s) {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    g_st.snap.state = s;
}

void SetFail(const std::string& detail) {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    g_st.snap.state        = State::Failed;
    g_st.snap.error_detail = detail;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Updater: %s", detail.c_str());
}

// ---------------------------------------------------------------------------
// Version compare. Splits on '.' and lex-compares the first 4 ints. Matches
// the way scripts/make_version.sh writes them. Tolerant of trailing text
// ("0.1.0-rc1" → (0,1,0,0)). Returns < 0 if a < b, 0 if equal, > 0 if a > b.
// ---------------------------------------------------------------------------

int CompareVersions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<int> parts;
        size_t p = 0;
        while (p < s.size() && parts.size() < 4) {
            int v = 0; bool got = false;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
                v = v * 10 + (s[p] - '0');
                ++p; got = true;
            }
            if (got) parts.push_back(v);
            if (p < s.size() && s[p] == '.') ++p;
            else break;
        }
        while (parts.size() < 4) parts.push_back(0);
        return parts;
    };
    auto pa = split(a);
    auto pb = split(b);
    for (int i = 0; i < 4; ++i) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

// ---------------------------------------------------------------------------
// WinHTTP wrappers. We do GET (text) and GET (file-stream). Both follow
// redirects automatically -- important because GitHub's release-asset
// download URLs 302 to a CDN.
// ---------------------------------------------------------------------------

bool ParseHttpsUrl(const std::string& url,
                   bool& https_out,
                   std::wstring& host_out,
                   uint16_t& port_out,
                   std::wstring& path_out)
{
    https_out = false;
    std::string s = url;
    if (s.compare(0, 8, "https://") == 0) { https_out = true;  s = s.substr(8); }
    else if (s.compare(0, 7, "http://") == 0) {                 s = s.substr(7); }
    else return false;
    size_t slash = s.find('/');
    std::string hp   = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : s.substr(slash);
    size_t colon = hp.find(':');
    std::string host = hp;
    uint16_t port = https_out ? 443 : 80;
    if (colon != std::string::npos) {
        host = hp.substr(0, colon);
        port = (uint16_t)std::atoi(hp.c_str() + colon + 1);
    }
    auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
    host_out = widen(host);
    path_out = widen(path);
    port_out = port;
    return true;
}

struct GetResp {
    int         status = 0;
    std::string body;
};

GetResp HttpGetText(const std::string& url, int timeout_ms = 8000) {
    GetResp out;
    bool https = false;
    std::wstring host, path;
    uint16_t port = 0;
    if (!ParseHttpsUrl(url, https, host, port, path)) return out;

    HINTERNET hSes = WinHttpOpen(L"FM2K_Updater/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,  // not AUTOMATIC: that needs Win8.1+ (WinHttpOpen -> 87 on Win8.0); DEFAULT works on every OS + honors system proxy
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return out;
    // Win8.0/7: enable TLS 1.2 (WinHTTP defaults to TLS 1.0 there, which breaks
    // HTTPS to GitHub). No-op on 8.1+. See FM2K_DiscordAuth for the full why.
    { DWORD sp = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(hSes, WINHTTP_OPTION_SECURE_PROTOCOLS, &sp, sizeof(sp)); }
    WinHttpSetTimeouts(hSes, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hCon = WinHttpConnect(hSes, host.c_str(), port, 0);
    if (!hCon) { WinHttpCloseHandle(hSes); return out; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes); return out; }

    // GitHub's REST API rejects requests without a User-Agent (returns
    // 403). WinHttp's WinHttpOpen agent string only sets the underlying
    // User-Agent in some cases; explicitly add the header to be safe so
    // /repos/.../releases/latest works as well as raw.githubusercontent.
    const wchar_t* extra_hdr = L"User-Agent: FM2K_Updater/0.2\r\n"
                               L"Accept: application/vnd.github+json\r\n";
    WinHttpAddRequestHeaders(hReq, extra_hdr, (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD |
                             WINHTTP_ADDREQ_FLAG_REPLACE);

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return out;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    out.status = (int)status;

    std::vector<char> buf;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        size_t off = buf.size();
        buf.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(hReq, buf.data() + off, avail, &got)) break;
        buf.resize(off + got);
    }
    out.body.assign(buf.begin(), buf.end());

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);
    return out;
}

// Streaming download with progress callback. Returns true on success.
// dst_path is created/truncated. Callback signature: (received, total).
template <class Cb>
bool HttpDownloadFile(const std::string& url,
                      const std::string& dst_path,
                      Cb on_progress,
                      int timeout_ms = 30000)
{
    bool https = false;
    std::wstring host, path;
    uint16_t port = 0;
    if (!ParseHttpsUrl(url, https, host, port, path)) return false;

    HINTERNET hSes = WinHttpOpen(L"FM2K_Updater/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,  // not AUTOMATIC: that needs Win8.1+ (WinHttpOpen -> 87 on Win8.0); DEFAULT works on every OS + honors system proxy
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return false;
    // Win8.0/7: enable TLS 1.2 (WinHTTP defaults to TLS 1.0 there, which breaks
    // HTTPS to GitHub). No-op on 8.1+. See FM2K_DiscordAuth for the full why.
    { DWORD sp = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(hSes, WINHTTP_OPTION_SECURE_PROTOCOLS, &sp, sizeof(sp)); }
    WinHttpSetTimeouts(hSes, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hCon = WinHttpConnect(hSes, host.c_str(), port, 0);
    if (!hCon) { WinHttpCloseHandle(hSes); return false; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes); return false; }

    // Auto-redirect handling -- release/download/... 302s to objects.githubusercontent.com.
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return false;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return false;
    }

    DWORD content_len = 0;
    DWORD cl_sz = sizeof(content_len);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &content_len, &cl_sz, WINHTTP_NO_HEADER_INDEX);

    FILE* fp = std::fopen(dst_path.c_str(), "wb");
    if (!fp) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return false;
    }

    uint32_t received = 0;
    char     buf[16 * 1024];
    bool     ok_all = true;
    for (;;) {
        if (g_st.cancel.load()) { ok_all = false; break; }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) { ok_all = false; break; }
        if (avail == 0) break;
        DWORD to_read = (avail > sizeof(buf)) ? sizeof(buf) : avail;
        DWORD got = 0;
        if (!WinHttpReadData(hReq, buf, to_read, &got)) { ok_all = false; break; }
        if (got == 0) break;
        if (std::fwrite(buf, 1, got, fp) != got) { ok_all = false; break; }
        received += got;
        on_progress(received, content_len);
    }
    std::fclose(fp);

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);
    return ok_all;
}

// ---------------------------------------------------------------------------
// File-system helpers
// ---------------------------------------------------------------------------

std::string TempDir() {
    char buf[MAX_PATH] = {};
    DWORD n = GetTempPathA(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string s(buf);
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
    return s;
}

std::string AppDir() {
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0) return ".";
    std::string s(buf);
    size_t slash = s.find_last_of("/\\");
    return (slash == std::string::npos) ? "." : s.substr(0, slash);
}

// ---------------------------------------------------------------------------
// Worker entry points
// ---------------------------------------------------------------------------

void CheckWorker() {
    SetState(State::Checking);
    const Channel channel = ReadUpdateChannel();
    // Stable channel (default): /releases/latest returns the most recent
    // non-prerelease tag. Simple JSON, one tag_name field.
    // Dev channel: /releases returns the full list (most recent first)
    // including prereleases. We walk it and pick the first prerelease=true
    // entry -- that's the latest dev build. Note: if you switch from dev
    // back to stable, the launcher's local version may already be NEWER
    // than what `/releases/latest` returns; CompareVersions handles that
    // (local > remote ⇒ UpToDate, no downgrade attempt).
    // Always fetch BOTH channels so the menu-bar release toggle can
    // display "Stable(0.2.53) Dev(0.2.54)" labels. We use the active
    // channel's value for the upgrade pill's UpdateAvailable comparison.
    //
    // /releases?per_page=40 returns the most-recent N entries (any
    // mix of prerelease/non-prerelease, newest first). Walking once
    // extracts all three tiers: first non-prerelease = stable, first
    // plain prerelease = dev, first "-bleeding" prerelease = bleeding.
    // One HTTP round-trip. Window is 40 (not 20) so a run of bleeding
    // cuts can't bury the latest stable/dev off the first page.
    char url[512];
    std::snprintf(url, sizeof(url),
        "https://api.github.com/repos/%s/%s/releases?per_page=40",
        kUpdateRepoOwner, kUpdateRepoName);

    GetResp r = HttpGetText(url);
    if (r.status != 200) {
        SetFail("Couldn't reach update server (HTTP " + std::to_string(r.status) + ")");
        g_st.busy.store(false);
        return;
    }

    // Walk releases JSON. For each entry capture (tag_name, prerelease).
    // Three buckets by tier: non-prerelease -> stable; prerelease with a
    // "-bleeding" tag -> bleeding; any other prerelease -> dev. Entries
    // arrive newest-first, so the first hit per bucket is that tier's
    // latest. Stop once all three are found; bail early on parse problems
    // (better to ship partial data than nothing).
    auto strip_v = [](std::string& s) {
        if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.erase(s.begin());
    };
    std::string latest_stable;
    std::string latest_dev;
    std::string latest_bleeding;
    {
        const std::string& body = r.body;
        size_t cur = 0;
        while (cur < body.size() &&
               (latest_stable.empty() || latest_dev.empty() || latest_bleeding.empty())) {
            size_t tn = body.find("\"tag_name\"", cur);
            if (tn == std::string::npos) break;
            size_t qp = body.find('"', tn + 10);
            if (qp == std::string::npos) break;
            size_t qe = body.find('"', qp + 1);
            if (qe == std::string::npos) break;
            std::string tag = body.substr(qp + 1, qe - qp - 1);

            size_t next_tn = body.find("\"tag_name\"", qe);
            size_t scan_end = (next_tn == std::string::npos) ? body.size() : next_tn;
            size_t pr = body.find("\"prerelease\"", qe);
            bool is_prerelease = false;
            if (pr != std::string::npos && pr < scan_end) {
                size_t colon = body.find(':', pr);
                if (colon != std::string::npos && colon + 1 < scan_end) {
                    size_t v = colon + 1;
                    while (v < scan_end && (body[v] == ' ' || body[v] == '\t')) ++v;
                    is_prerelease = (body.compare(v, 4, "true") == 0);
                }
            }
            // Tier marker lives in the tag itself (checked on the raw tag,
            // before strip_v) so a "-bleeding" build is routed away from
            // the dev bucket.
            const bool is_bleeding = (tag.find("bleeding") != std::string::npos);
            if (is_prerelease && is_bleeding) {
                if (latest_bleeding.empty()) latest_bleeding = std::move(tag);
            } else if (is_prerelease) {
                if (latest_dev.empty()) latest_dev = std::move(tag);
            } else {
                if (latest_stable.empty()) latest_stable = std::move(tag);
            }
            cur = scan_end;
        }
    }
    strip_v(latest_stable);
    strip_v(latest_dev);
    strip_v(latest_bleeding);

    // Pick the value the active channel cares about for the upgrade pill.
    // Each tier is a superset of the one below, so the channel's "remote"
    // is the max version across all tiers at or below it:
    //   Stable:   latest non-prerelease only. Pre-releases are invisible --
    //             stable users shouldn't be pulled into them.
    //   Dev:      max(stable, dev). Excludes bleeding. Dev is a superset of
    //             stable -- if we promote 0.2.56 to stable without leaving
    //             a matching prerelease, dev users on the previous
    //             prerelease would otherwise be stuck forever; picking the
    //             higher keeps dev tracking the newest dev-or-stable build.
    //   Bleeding: max(stable, dev, bleeding). The absolute newest of all.
    auto pick_max = [](const std::string& a, const std::string& b) -> std::string {
        if (a.empty()) return b;
        if (b.empty()) return a;
        return (CompareVersions(a, b) >= 0) ? a : b;
    };
    std::string remote;
    if (channel == Channel::Bleeding) {
        remote = pick_max(pick_max(latest_stable, latest_dev), latest_bleeding);
    } else if (channel == Channel::Dev) {
        remote = pick_max(latest_stable, latest_dev);
    } else {
        remote = latest_stable;
        if (remote.empty()) remote = latest_dev;  // bootstrap: no stable cut yet
    }
    if (remote.empty()) {
        SetFail("Update server returned no parseable releases");
        g_st.busy.store(false);
        return;
    }

    const char* channel_name = (channel == Channel::Bleeding) ? "bleeding"
                             : (channel == Channel::Dev)      ? "dev"
                                                              : "stable";
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Updater: local=%s channel=%s stable=%s dev=%s bleeding=%s -> remote=%s",
        kAppVersion, channel_name,
        latest_stable.empty()   ? "(none)" : latest_stable.c_str(),
        latest_dev.empty()      ? "(none)" : latest_dev.c_str(),
        latest_bleeding.empty() ? "(none)" : latest_bleeding.c_str(),
        remote.c_str());

    // Trigger UpdateAvailable on ANY mismatch between local and the
    // active channel's latest, not just remote-newer. Rationale: if a
    // user is on a dev build (e.g. 0.2.55) and flips the channel to
    // stable, the launcher should offer to "switch" them down to the
    // current stable (0.2.54). Otherwise they sit on the dev binary
    // forever even though they explicitly asked for stable. Pill text
    // distinguishes upgrade ("Update X -> Y") from downgrade
    // ("Switch X -> Y (stable)") -- see RenderMenuBar.
    const int cmp = CompareVersions(kAppVersion, remote);
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        g_st.snap.remote_version  = remote;
        g_st.snap.latest_stable   = latest_stable;
        g_st.snap.latest_dev      = latest_dev;
        g_st.snap.latest_bleeding = latest_bleeding;
        g_st.snap.state = (cmp != 0) ? State::UpdateAvailable : State::UpToDate;
    }
    g_st.busy.store(false);
}

void DownloadWorker(std::string remote_version) {
    SetState(State::Downloading);

    char url[512];
    std::snprintf(url, sizeof(url),
        "https://github.com/%s/%s/releases/download/v%s/fm2k_v%s.zip",
        kUpdateRepoOwner, kUpdateRepoName,
        remote_version.c_str(), remote_version.c_str());

    const std::string zip_path = TempDir() + "fm2k_update_v" + remote_version + ".zip";

    bool ok = HttpDownloadFile(url, zip_path,
        [](uint32_t got, uint32_t total) {
            std::lock_guard<std::mutex> lk(g_st.mtx);
            g_st.snap.downloaded_bytes = got;
            g_st.snap.total_bytes      = total;
        });
    if (!ok) {
        SetFail("Download failed (zip URL = " + std::string(url) + ")");
        std::error_code ec;
        std::filesystem::remove(zip_path, ec);
        g_st.busy.store(false);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        g_st.zip_path  = zip_path;
        g_st.snap.state = State::Ready;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Updater: downloaded %s", zip_path.c_str());
    g_st.busy.store(false);
}

void JoinWorker() {
    if (g_st.worker.joinable()) g_st.worker.join();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CheckForUpdates() {
    if (g_st.busy.exchange(true)) return;
    JoinWorker();
    g_st.worker = std::thread(CheckWorker);
}

void StartDownload() {
    Snapshot s = Get();
    if (s.state != State::UpdateAvailable) return;
    if (g_st.busy.exchange(true)) return;
    JoinWorker();
    g_st.worker = std::thread(DownloadWorker, s.remote_version);
}

bool ApplyUpdateAndExit() {
    std::string zip_path;
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        if (g_st.snap.state != State::Ready) return false;
        zip_path = g_st.zip_path;
    }
    if (zip_path.empty()) return false;

    const std::string app_dir       = AppDir();
    const std::string app_updater   = app_dir + "\\FM2KUpdater.exe";
    const DWORD       parent_pid    = GetCurrentProcessId();

    // CCCaster-style: copy the updater EXE to %TEMP% and run it from
    // there. We can't run it in-place because the zip contains a NEW
    // FM2KUpdater.exe and a running EXE can't be overwritten on Windows
    // (ERROR_SHARING_VIOLATION → tar exits with status 1). The tmp
    // copy is fine to overwrite when the new launcher pulls a fresh
    // FM2KUpdater.exe out of the zip into app_dir.
    const std::string tmp_updater = TempDir() + "FM2KUpdater_run.exe";
    const BOOL copy_ok = CopyFileA(app_updater.c_str(), tmp_updater.c_str(),
                                   FALSE /* overwrite ok */);
    const std::string updater = copy_ok ? tmp_updater : app_updater;
    if (!copy_ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Updater: couldn't copy FM2KUpdater.exe to %TEMP% (err=%lu) -- "
            "running from app dir; will fail to self-replace",
            (unsigned long)GetLastError());
    }

    // Argv: <updater_path> <parent_pid> <app_dir> <zip_path>
    // Quote app_dir + zip_path because they often contain spaces.
    char cmdline[2048];
    std::snprintf(cmdline, sizeof(cmdline),
        "\"%s\" %lu \"%s\" \"%s\"",
        updater.c_str(),
        (unsigned long)parent_pid,
        app_dir.c_str(),
        zip_path.c_str());

    STARTUPINFOA        si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    BOOL ok = CreateProcessA(updater.c_str(), cmdline,
                             nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE, nullptr, nullptr,
                             &si, &pi);
    if (!ok) {
        SetFail("Couldn't spawn FM2KUpdater.exe (err=" +
                std::to_string(GetLastError()) + ")");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Updater: handed off to FM2KUpdater.exe -- exiting launcher");

    // Give the updater a beat to come up before we tear down so the
    // first thing it does (waiting on our PID) sees a real handle.
    Sleep(200);
    std::exit(0);
}

Snapshot Get() {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    return g_st.snap;
}

bool IsRemoteOlderThanLocal() {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    if (g_st.snap.remote_version.empty()) return false;
    return CompareVersions(kAppVersion, g_st.snap.remote_version) > 0;
}

void Shutdown() {
    g_st.cancel.store(true);
    JoinWorker();
}

}  // namespace fm2k::updater
