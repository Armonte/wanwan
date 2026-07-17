// FM2K_CncDDraw — see header for rationale.

#include "FM2K_CncDDraw.h"
#include "FM2K_Utf8Path.h"  // WideToUtf8 -- keep the module path UTF-8, not ANSI

#include <SDL3/SDL_log.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// Stock miniz, vendored at vendored/miniz/. The SDL_image vendor at
// vendored/SDL_image/src/miniz.h is a separate copy with archive APIs
// hard-disabled; we don't touch it. Our copy stays in its own include
// dir so `#include <miniz.h>` here doesn't accidentally collide.
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

// Default ddraw.ini baked into the launcher (`kDefaultIni`). Generated
// from a tuned working ini; written to the install dir on fresh installs.
#include "FM2K_CncDDraw_DefaultIni.inc"

namespace fm2k::cnc_ddraw {

namespace {

// ---------------------------------------------------------------------------
// Repo coordinates. Hardcoded — we don't pull from a config file. If
// upstream moves we'll patch the launcher.
// ---------------------------------------------------------------------------
constexpr const char* kRepoOwner = "FunkyFr3sh";
constexpr const char* kRepoName  = "cnc-ddraw";
constexpr const char* kAssetName = "cnc-ddraw.zip";

// PINNED release. The hook DLL hard-depends on this exact build: the F4
// fullscreen toggle calls util_toggle_fullscreen at 2DFMD.dll+0x8770
// (signature-guarded) and the toggle diagnostics read RVAs from the
// 2DFMD IDB — a different upstream build moves those and silently kills
// the F4 hotkey. Tracking releases/latest meant any upstream release
// could do that without us shipping anything. Bump this tag ONLY after
// re-verifying the offsets in wndproc_subclass.cpp against the new build.
constexpr const char* kPinnedTag = "v7.1.0.0";

// File that lives in the install dir — its presence + 2DFMD.dll's
// presence + version.txt's content tell us "install is good."
constexpr const char* kRenamedDll = "2DFMD.dll";
constexpr const char* kZipDllName = "ddraw.dll";   // what the zip contains

// ---------------------------------------------------------------------------
// State (file-static, accessed from worker thread + UI thread under mutex).
// ---------------------------------------------------------------------------

struct InternalState {
    Snapshot           snap;
    std::thread        worker;
    std::atomic<bool>  busy{false};
    std::atomic<bool>  cancel{false};
    mutable std::mutex mtx;
};

InternalState g_st;

void SetState(State s) {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    g_st.snap.state = s;
}

void SetFail(const std::string& detail) {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    g_st.snap.state        = State::Failed;
    g_st.snap.error_detail = detail;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CncDDraw: %s", detail.c_str());
}

// ---------------------------------------------------------------------------
// WinHTTP wrappers. Same pattern as FM2K_Updater — duplicated rather than
// extracted to a shared header so the two updaters stay independently
// editable; the surface is small enough that the duplication cost is low.
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

// Streamed file download with progress. Returns true on success.
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

    HINTERNET hSes = WinHttpOpen(L"FM2K_CncDDraw/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,  // not AUTOMATIC: that needs Win8.1+ (WinHttpOpen -> 87 on Win8.0); DEFAULT works on every OS + honors system proxy
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return false;
    // Win8.0/7: enable TLS 1.2 (WinHTTP defaults to TLS 1.0 there). No-op on 8.1+.
    { DWORD sp = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(hSes, WINHTTP_OPTION_SECURE_PROTOCOLS, &sp, sizeof(sp)); }
    WinHttpSetTimeouts(hSes, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hCon = WinHttpConnect(hSes, host.c_str(), port, 0);
    if (!hCon) { WinHttpCloseHandle(hSes); return false; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes); return false; }

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

// "v6.6.0.4" -> "6.6.0.4". Pass-through for anything that doesn't have
// a leading 'v', so unusual upstream tagging schemes don't break us.
std::string StripLeadingV(const std::string& tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) return tag.substr(1);
    return tag;
}

// ---------------------------------------------------------------------------
// Filesystem helpers.
// ---------------------------------------------------------------------------

std::string AppDir() {
    // Wide module path -> UTF-8. GetModuleFileNameA would hand back ANSI bytes
    // for a non-ASCII username (e.g. C:\Users\Jose\...), and the std::string
    // then flows into std::filesystem::path(...) below, whose narrow->wide
    // conversion THROWS "illegal byte sequence" -- the crash that closed the
    // launcher for those users. UTF-8 + the global UTF-8 locale converts cleanly.
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return ".";
    std::string s = fm2k::utf8path::WideToUtf8(std::wstring(buf, n));
    size_t slash = s.find_last_of("/\\");
    return (slash == std::string::npos) ? "." : s.substr(0, slash);
}

std::string TempDir() {
    char buf[MAX_PATH] = {};
    DWORD n = GetTempPathA(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string s(buf);
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
    return s;
}

bool EnsureDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec || std::filesystem::is_directory(path, ec);
}

// ---------------------------------------------------------------------------
// Zip extraction via stock miniz. Walks the central directory, writes
// each non-directory entry to `dst_dir`, creating subdirectories as
// needed. Returns true if every file extracted; on failure leaves
// whatever was already written in place (caller wipes `dst_dir` and
// retries). Pure C — runs on Win7 / Win8 / anywhere our 32-bit MinGW
// build executes, no OS dependency on bundled tar.exe.
// ---------------------------------------------------------------------------
bool ExtractZip(const std::string& zip_path, const std::string& dst_dir) {
    if (!EnsureDir(dst_dir)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CncDDraw: couldn't create extract dir %s", dst_dir.c_str());
        return false;
    }

    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CncDDraw: mz_zip_reader_init_file failed for %s",
            zip_path.c_str());
        return false;
    }

    bool all_ok = true;
    const mz_uint num = mz_zip_reader_get_num_files(&zip);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CncDDraw: extracting %u entries from %s",
        (unsigned)num, zip_path.c_str());
    for (mz_uint i = 0; i < num; ++i) {
        mz_zip_archive_file_stat st = {};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "CncDDraw: file_stat failed for entry %u", (unsigned)i);
            all_ok = false; break;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        // Zip-slip protection + path normalization. cnc-ddraw releases
        // are clean, but anyone who replaces the asset URL with a
        // malicious zip shouldn't get arbitrary writes; reject any
        // entry whose name walks above dst_dir or includes a drive.
        std::string rel = st.m_filename;
        if (rel.empty() || rel[0] == '/' || rel[0] == '\\' ||
            (rel.size() > 1 && rel[1] == ':') ||
            rel.find("..") != std::string::npos) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "CncDDraw: skipping suspicious zip entry '%s'", rel.c_str());
            continue;
        }
        for (char& c : rel) if (c == '/') c = '\\';

        const std::string dst = dst_dir + "\\" + rel;
        const size_t last_slash = dst.find_last_of('\\');
        if (last_slash != std::string::npos) {
            EnsureDir(dst.substr(0, last_slash));
        }

        if (!mz_zip_reader_extract_to_file(&zip, i, dst.c_str(), 0)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "CncDDraw: extract failed for entry '%s' -> %s",
                rel.c_str(), dst.c_str());
            all_ok = false; break;
        }
    }
    mz_zip_reader_end(&zip);
    return all_ok;
}

// ---------------------------------------------------------------------------
// Worker entry points.
// ---------------------------------------------------------------------------

// Fixes managed keys in an EXISTING ddraw.ini in place. Updates that
// preserve a user-tuned ini (and installs that predate a default change)
// keep old key values alive forever otherwise. Runs on every worker pass,
// before any network I/O, so offline launchers still get migrated.
//
// Current migrations:
//   keytogglefullscreen2: 0x73 (F4) -> 0x00. cnc-ddraw's single-key
//   hotkey matcher has no Alt check and swallows the key at the
//   WH_KEYBOARD hook (keyboard.c) — with F4 bound there, Alt+F4 toggled
//   fullscreen instead of closing the game. Our wndproc subclass owns F4
//   now (wndproc_subclass.cpp); cnc-ddraw itself passes Alt+F4 through
//   to the normal close chain once nothing binds F4. Only the exact old
//   default 0x73 is rewritten, so a deliberate custom binding survives.
void MigrateManagedIniKeys() {
    const std::string ini = InstallDir() + "\\ddraw.ini";
    std::error_code ec;
    if (!std::filesystem::exists(ini, ec)) return;
    // GetPrivateProfileIntA parses the 0x-prefixed hex these keys use.
    const int fs2 = (int)GetPrivateProfileIntA("ddraw", "keytogglefullscreen2",
                                               0, ini.c_str());
    if (fs2 == 0x73) {
        WritePrivateProfileStringA("ddraw", "keytogglefullscreen2", "0x00",
                                   ini.c_str());
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CncDDraw: migrated keytogglefullscreen2 0x73 -> 0x00 "
            "(F4 is hook-owned now; frees Alt+F4 to close the game)");
    }
}

// Stages the install dir if the PINNED release != local. `force` skips
// the version compare and always installs. On success, ends in
// State::Ready (newly installed) or State::UpToDate (no work needed).
// On failure, ends in State::Failed.
void InstallWorkerImpl(bool force) {
    SetState(State::Checking);

    MigrateManagedIniKeys();

    // 1. Target version is the pinned tag — no releases/latest query.
    // (Also removes the GitHub-API-rate-limit failure mode on every
    // launcher start; an up-to-date install now needs no network at all.)
    const std::string tag = kPinnedTag;
    const std::string remote = StripLeadingV(tag);
    const std::string local  = ReadLocalVersion();
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        g_st.snap.local_version  = local;
        g_st.snap.remote_version = remote;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CncDDraw: local='%s' remote='%s' (tag=%s)",
        local.c_str(), remote.c_str(), tag.c_str());

    // 2. If we already have the right version (and the dll is on disk),
    // we're done. The dll-presence check guards against a partial install
    // where version.txt got written but extraction was interrupted.
    std::error_code ec;
    const bool dll_present = std::filesystem::exists(DllPath(), ec);
    if (!force && local == remote && !local.empty() && dll_present) {
        SetState(State::UpToDate);
        g_st.busy.store(false);
        return;
    }

    // 3. Download zip to %TEMP%.
    SetState(State::Downloading);
    const std::string temp_dir = TempDir();
    const std::string zip_path = temp_dir + "cnc-ddraw_v" + remote + ".zip";

    // GitHub's release-download URL pattern. Redirects to objects.githubusercontent
    // — HttpDownloadFile follows.
    char zip_url[512];
    std::snprintf(zip_url, sizeof(zip_url),
        "https://github.com/%s/%s/releases/download/%s/%s",
        kRepoOwner, kRepoName, tag.c_str(), kAssetName);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CncDDraw: downloading %s -> %s", zip_url, zip_path.c_str());

    bool dl_ok = HttpDownloadFile(zip_url, zip_path,
        [](uint32_t got, uint32_t total) {
            std::lock_guard<std::mutex> lk(g_st.mtx);
            g_st.snap.downloaded_bytes = got;
            g_st.snap.total_bytes      = total;
        });
    if (!dl_ok) {
        SetFail("Download failed: " + std::string(zip_url));
        std::filesystem::remove(zip_path, ec);
        g_st.busy.store(false);
        return;
    }

    // 4. Extract into a sibling staging dir, then atomic-swap with the
    // install dir. Staging-then-swap is what gives us "no half-installed
    // state ever observable to a subsequent launcher run." We deliberately
    // do NOT clobber the user's existing ddraw.ini if one is in the
    // install dir already (Phase D will manage defaults).
    SetState(State::Extracting);
    const std::string install_dir = InstallDir();
    if (install_dir.empty()) {
        SetFail("InstallDir() returned empty (GetModuleFileName failed?)");
        std::filesystem::remove(zip_path, ec);
        g_st.busy.store(false);
        return;
    }
    const std::string staging_dir = install_dir + ".staging";
    // Wipe any stale staging from a previous interrupted run.
    std::filesystem::remove_all(staging_dir, ec);
    if (!EnsureDir(staging_dir)) {
        SetFail("Couldn't create staging dir " + staging_dir);
        std::filesystem::remove(zip_path, ec);
        g_st.busy.store(false);
        return;
    }
    if (!ExtractZip(zip_path, staging_dir)) {
        SetFail("Zip extraction failed");
        std::filesystem::remove_all(staging_dir, ec);
        std::filesystem::remove(zip_path, ec);
        g_st.busy.store(false);
        return;
    }

    // Rename ddraw.dll -> 2DFMD.dll inside staging.
    const std::string staged_ddraw = staging_dir + "\\" + kZipDllName;
    const std::string staged_renamed = staging_dir + "\\" + kRenamedDll;
    if (!std::filesystem::exists(staged_ddraw, ec)) {
        SetFail("Zip didn't contain " + std::string(kZipDllName) +
                " — upstream release layout changed?");
        std::filesystem::remove_all(staging_dir, ec);
        std::filesystem::remove(zip_path, ec);
        g_st.busy.store(false);
        return;
    }
    std::filesystem::remove(staged_renamed, ec);  // shouldn't exist, but be safe
    std::filesystem::rename(staged_ddraw, staged_renamed, ec);
    if (ec) {
        SetFail("Rename ddraw.dll -> " + std::string(kRenamedDll) +
                " failed: " + ec.message());
        std::filesystem::remove_all(staging_dir, ec);
        std::filesystem::remove(zip_path, ec);
        g_st.busy.store(false);
        return;
    }

    // Overwrite the stock ini that came out of the zip with our baked
    // default — the launcher's canonical settings live in the binary,
    // not in upstream's defaults. (cnc-ddraw's stock turns hotkeys on,
    // doesn't pin the renderer, etc. — we want consistent defaults
    // matched to the FM2K integration.)
    {
        FILE* fp = std::fopen((staging_dir + "\\ddraw.ini").c_str(), "wb");
        if (fp) {
            std::fwrite(kDefaultIni, 1, sizeof(kDefaultIni) - 1, fp);
            std::fclose(fp);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "CncDDraw: couldn't write baked ddraw.ini into staging — keeping zip's stock");
        }
    }

    // Preserve a user-tuned ddraw.ini if one already lives in the
    // install dir — copying it into staging AFTER our default write
    // means user tuning wins on updates while fresh installs still
    // pick up our baked defaults.
    const std::string user_ini = install_dir + "\\ddraw.ini";
    if (std::filesystem::exists(user_ini, ec)) {
        std::filesystem::copy_file(user_ini, staging_dir + "\\ddraw.ini",
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "CncDDraw: couldn't preserve user ddraw.ini (%s) — using baked default",
                ec.message().c_str());
            ec.clear();
        }
    }

    // Atomic-ish swap. Windows can't `rename` over an existing dir; we
    // move the old install aside, then rename staging into place, then
    // remove the old. If the game is currently running and 2DFMD.dll
    // is locked, the rename of the old dir will fail with sharing
    // violation — surface that clearly.
    const std::string backup_dir = install_dir + ".old";
    std::filesystem::remove_all(backup_dir, ec);
    if (std::filesystem::exists(install_dir, ec)) {
        std::filesystem::rename(install_dir, backup_dir, ec);
        if (ec) {
            SetFail("Couldn't move existing install aside (game running with "
                    "2DFMD.dll loaded?): " + ec.message());
            std::filesystem::remove_all(staging_dir, ec);
            std::filesystem::remove(zip_path, ec);
            g_st.busy.store(false);
            return;
        }
    }
    std::filesystem::rename(staging_dir, install_dir, ec);
    if (ec) {
        // Try to roll back so we don't leave the launcher with no install.
        SetFail("Final rename of staging -> install failed: " + ec.message());
        if (std::filesystem::exists(backup_dir, ec)) {
            std::error_code ec2;
            std::filesystem::rename(backup_dir, install_dir, ec2);
        }
        std::filesystem::remove(zip_path, ec);
        g_st.busy.store(false);
        return;
    }
    std::filesystem::remove_all(backup_dir, ec);
    std::filesystem::remove(zip_path, ec);

    // Write version.txt last — its presence is the integrity flag.
    {
        FILE* fp = std::fopen(VersionFilePath().c_str(), "wb");
        if (!fp) {
            SetFail("Couldn't write version.txt at " + VersionFilePath());
            g_st.busy.store(false);
            return;
        }
        std::fwrite(remote.data(), 1, remote.size(), fp);
        std::fclose(fp);
    }

    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        g_st.snap.local_version = remote;
        g_st.snap.state         = State::Ready;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CncDDraw: installed v%s at %s", remote.c_str(), install_dir.c_str());
    g_st.busy.store(false);
}

// Safety net: cnc-ddraw is a COSMETIC display upgrade and runs on its own
// worker thread, so an uncaught exception here (e.g. a residual std::filesystem
// path conversion on an exotic path) must never call std::terminate and take
// the whole launcher down with it. Catch, log, fail the install, keep running.
void InstallWorker(bool force) {
    try {
        InstallWorkerImpl(force);
    } catch (const std::exception& e) {
        SetFail(std::string("cnc-ddraw install aborted (exception): ") + e.what());
        g_st.busy.store(false);
    } catch (...) {
        SetFail("cnc-ddraw install aborted (unknown exception)");
        g_st.busy.store(false);
    }
}

void JoinWorker() {
    if (g_st.worker.joinable()) g_st.worker.join();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string InstallDir() {
    return AppDir() + "\\cnc-ddraw";
}

std::string VersionFilePath() {
    return InstallDir() + "\\version.txt";
}

std::string DllPath() {
    return InstallDir() + "\\" + kRenamedDll;
}

std::string ReadLocalVersion() {
    FILE* fp = std::fopen(VersionFilePath().c_str(), "rb");
    if (!fp) return {};
    char buf[64] = {};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t')) s.pop_back();
    return s;
}

void EnsureInstalled() {
    if (g_st.busy.exchange(true)) return;
    JoinWorker();
    g_st.worker = std::thread(InstallWorker, /*force=*/false);
}

void ForceReinstall() {
    if (g_st.busy.exchange(true)) return;
    JoinWorker();
    g_st.worker = std::thread(InstallWorker, /*force=*/true);
}

Snapshot Get() {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    return g_st.snap;
}

void Shutdown() {
    g_st.cancel.store(true);
    JoinWorker();
}

// ─── ddraw.ini editing ──────────────────────────────────────────────────
//
// The launcher UI calls these from Settings → Display. Read happens once
// per tab-open into IniConfig; writes go per-widget-change. Section is
// always "ddraw" (cnc-ddraw's global section); per-game [<exe>] blocks
// the user might add are untouched by these calls.

namespace {

constexpr const char* kSection = "ddraw";

// `WritePrivateProfileString(NULL, NULL, NULL, path)` flushes the
// per-process Win32 profile cache for that file — call before reads
// when we suspect another process (cnc-ddraw config.exe, manual edits)
// touched the file. We only do this on initial Load; per-widget writes
// are already cache-coherent since they go through the same API.
void FlushProfileCache(const char* path) {
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, path);
}

// cnc-ddraw accepts "true"/"false" (config program writes) and "1"/"0"
// (older custom inis). Match both.
bool ReadBool(const char* path, const char* key, bool def) {
    char buf[16] = {};
    GetPrivateProfileStringA(kSection, key,
                             def ? "true" : "false",
                             buf, sizeof(buf), path);
    return _stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0;
}

int ReadInt(const char* path, const char* key, int def) {
    return GetPrivateProfileIntA(kSection, key, def, path);
}

std::string ReadStr(const char* path, const char* key, const std::string& def) {
    char buf[1024] = {};
    GetPrivateProfileStringA(kSection, key, def.c_str(),
                             buf, sizeof(buf), path);
    return buf;
}

}  // namespace

std::string IniPath() {
    return InstallDir() + "\\ddraw.ini";
}

bool ResetIniToDefault() {
    const std::string p = IniPath();
    FILE* fp = std::fopen(p.c_str(), "wb");
    if (!fp) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "CncDDraw::ResetIniToDefault: open %s failed", p.c_str());
        return false;
    }
    std::fwrite(kDefaultIni, 1, sizeof(kDefaultIni) - 1, fp);
    std::fclose(fp);
    // Profile cache may still have stale values; flush so the next
    // LoadIni picks up our reset content.
    FlushProfileCache(p.c_str());
    return true;
}

void LoadIni(IniConfig& o) {
    const std::string ps = IniPath();
    const char* p = ps.c_str();
    FlushProfileCache(p);

    // Display
    o.width        = ReadInt (p, "width",        o.width);
    o.height       = ReadInt (p, "height",       o.height);
    o.fullscreen   = ReadBool(p, "fullscreen",   o.fullscreen);
    o.windowed     = ReadBool(p, "windowed",     o.windowed);
    o.maintas      = ReadBool(p, "maintas",      o.maintas);
    o.aspect_ratio = ReadStr (p, "aspect_ratio", o.aspect_ratio);
    o.boxing       = ReadBool(p, "boxing",       o.boxing);
    o.maxfps       = ReadInt (p, "maxfps",       o.maxfps);
    o.vsync        = ReadBool(p, "vsync",        o.vsync);
    o.adjmouse     = ReadBool(p, "adjmouse",     o.adjmouse);
    o.shader       = ReadStr (p, "shader",       o.shader);
    o.posX         = ReadInt (p, "posX",         o.posX);
    o.posY         = ReadInt (p, "posY",         o.posY);
    o.renderer     = ReadStr (p, "renderer",     o.renderer);
    o.devmode      = ReadBool(p, "devmode",      o.devmode);
    o.border       = ReadBool(p, "border",       o.border);
    o.savesettings = ReadInt (p, "savesettings", o.savesettings);
    o.resizable    = ReadBool(p, "resizable",    o.resizable);
    o.d3d9_filter  = ReadInt (p, "d3d9_filter",  o.d3d9_filter);
    o.anti_aliased_fonts_min_size =
                     ReadInt (p, "anti_aliased_fonts_min_size",
                                  o.anti_aliased_fonts_min_size);
    o.min_font_size= ReadInt (p, "min_font_size",o.min_font_size);
    o.center_window= ReadInt (p, "center_window",o.center_window);
    o.inject_resolution = ReadStr(p, "inject_resolution", o.inject_resolution);
    o.vhack        = ReadBool(p, "vhack",        o.vhack);
    o.screenshotdir= ReadStr (p, "screenshotdir",o.screenshotdir);
    o.toggle_borderless = ReadBool(p, "toggle_borderless", o.toggle_borderless);
    o.toggle_upscaled   = ReadBool(p, "toggle_upscaled",   o.toggle_upscaled);

    // Compatibility
    o.noactivateapp   = ReadBool(p, "noactivateapp",   o.noactivateapp);
    o.maxgameticks    = ReadInt (p, "maxgameticks",    o.maxgameticks);
    o.limiter_type    = ReadInt (p, "limiter_type",    o.limiter_type);
    o.minfps          = ReadInt (p, "minfps",          o.minfps);
    o.nonexclusive    = ReadBool(p, "nonexclusive",    o.nonexclusive);
    o.singlecpu       = ReadBool(p, "singlecpu",       o.singlecpu);
    o.resolutions     = ReadInt (p, "resolutions",     o.resolutions);
    o.fixchilds       = ReadInt (p, "fixchilds",       o.fixchilds);
    o.hook_peekmessage= ReadBool(p, "hook_peekmessage",o.hook_peekmessage);

    // Undocumented / advanced
    o.fix_alt_key_stuck   = ReadBool(p, "fix_alt_key_stuck",   o.fix_alt_key_stuck);
    o.game_handles_close  = ReadBool(p, "game_handles_close",  o.game_handles_close);
    o.fix_not_responding  = ReadBool(p, "fix_not_responding",  o.fix_not_responding);
    o.no_compat_warning   = ReadBool(p, "no_compat_warning",   o.no_compat_warning);
    o.guard_lines         = ReadInt (p, "guard_lines",         o.guard_lines);
    o.max_resolutions     = ReadInt (p, "max_resolutions",     o.max_resolutions);
    o.lock_surfaces       = ReadBool(p, "lock_surfaces",       o.lock_surfaces);
    o.flipclear           = ReadBool(p, "flipclear",           o.flipclear);
    o.rgb555              = ReadBool(p, "rgb555",              o.rgb555);
    o.no_dinput_hook      = ReadBool(p, "no_dinput_hook",      o.no_dinput_hook);
    o.center_cursor_fix   = ReadBool(p, "center_cursor_fix",   o.center_cursor_fix);
    o.lock_mouse_top_left = ReadBool(p, "lock_mouse_top_left", o.lock_mouse_top_left);
    o.hook                = ReadInt (p, "hook",                o.hook);
    o.limit_gdi_handles   = ReadBool(p, "limit_gdi_handles",   o.limit_gdi_handles);
    o.remove_menu         = ReadBool(p, "remove_menu",         o.remove_menu);
    o.refresh_rate        = ReadInt (p, "refresh_rate",        o.refresh_rate);

    // Hotkeys — read as int; cnc-ddraw accepts both decimal and 0x-prefixed
    // hex from GetPrivateProfileInt's standard parsing.
    o.keytogglefullscreen  = ReadInt(p, "keytogglefullscreen",  o.keytogglefullscreen);
    o.keytogglefullscreen2 = ReadInt(p, "keytogglefullscreen2", o.keytogglefullscreen2);
    o.keytogglemaximize    = ReadInt(p, "keytogglemaximize",    o.keytogglemaximize);
    o.keytogglemaximize2   = ReadInt(p, "keytogglemaximize2",   o.keytogglemaximize2);
    o.keyunlockcursor1     = ReadInt(p, "keyunlockcursor1",     o.keyunlockcursor1);
    o.keyunlockcursor2     = ReadInt(p, "keyunlockcursor2",     o.keyunlockcursor2);
    o.keyscreenshot        = ReadInt(p, "keyscreenshot",        o.keyscreenshot);
}

void SaveBool(const char* key, bool val) {
    WritePrivateProfileStringA(kSection, key,
                               val ? "true" : "false",
                               IniPath().c_str());
}

void SaveInt(const char* key, int val) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", val);
    WritePrivateProfileStringA(kSection, key, buf, IniPath().c_str());
}

void SaveHex(const char* key, int val) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%02X", val & 0xFF);
    WritePrivateProfileStringA(kSection, key, buf, IniPath().c_str());
}

void SaveString(const char* key, const std::string& val) {
    WritePrivateProfileStringA(kSection, key, val.c_str(), IniPath().c_str());
}

}  // namespace fm2k::cnc_ddraw
