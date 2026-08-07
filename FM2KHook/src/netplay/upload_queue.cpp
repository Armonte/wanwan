// upload_queue.cpp -- manifest writer used by the hook to flag a session
// for the launcher's upload pipeline. See upload_queue.h for design.

#include "upload_queue.h"

#include "../core/globals.h"        // g_player_index
#include "../netplay/nat_traversal.h"  // fm2k::nat::GetMatchTokenHex
#include "../util/zip_writer.h"     // fm2k::util::zip::WriteZip

#include <SDL3/SDL_log.h>
#include <windows.h>
#include <wincrypt.h>  // CryptAcquireContext / CryptCreateHash (stripped by WIN32_LEAN_AND_MEAN)
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "version_local.h"  // fm2k::kAppVersion

namespace fm2k::upload_queue {

namespace {

// Resolve <game_dir>/upload_queue/ -- sibling of the .exe we're injected
// into. Created lazily on first enqueue. Returns false if the game's
// directory can't be derived (extremely unlikely outside a unit test).
bool BuildQueueDir(char* out, size_t out_size) {
    char exe_path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
    if (n == 0 || n >= sizeof(exe_path)) return false;

    // Trim to directory (chop after last '\').
    char* last_slash = nullptr;
    for (char* p = exe_path; *p; ++p) {
        if (*p == '\\' || *p == '/') last_slash = p;
    }
    if (!last_slash) return false;
    *last_slash = '\0';

    int len = std::snprintf(out, out_size, "%s\\upload_queue", exe_path);
    if (len < 0 || (size_t)len >= out_size) return false;
    // Create if missing; ignore "already exists".
    CreateDirectoryA(out, nullptr);
    return true;
}

// SHA1 of a file on disk, formatted as lowercase hex. Returns empty
// string on failure (file too big, read error, etc.). Used to stamp the
// .exe / hook DLL identity into the manifest so the receiver can pair
// crashes with a specific build artifact.
//
// We use Windows' bcrypt for SHA1 -- no extra dependency, present on all
// supported Windows versions. Read in 64 KB chunks so a multi-MB exe
// doesn't balloon memory.
//
// NOTE: only called from non-crash paths (desync, normal flow). Skipped
// in CrashHandler to avoid touching bcrypt during process death.
bool FileSha1Hex(const char* path, char out_hex[41]) {
    out_hex[0] = '\0';
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT)) {
        CloseHandle(h);
        return false;
    }
    if (!CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
        CryptReleaseContext(prov, 0);
        CloseHandle(h);
        return false;
    }

    uint8_t buf[64 * 1024];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        if (!CryptHashData(hash, buf, got, 0)) {
            CryptDestroyHash(hash);
            CryptReleaseContext(prov, 0);
            CloseHandle(h);
            return false;
        }
    }
    CloseHandle(h);

    uint8_t digest[20] = {};
    DWORD dlen = sizeof(digest);
    BOOL ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0);
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    if (!ok || dlen != 20) return false;

    for (int i = 0; i < 20; ++i) {
        std::snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
    }
    out_hex[40] = '\0';
    return true;
}

// Find the FM2KHook.dll module handle so we can SHA1 its file. Returns
// empty on failure.
bool HookDllSha1Hex(char out_hex[41]) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&HookDllSha1Hex, &m) || !m) {
        return false;
    }
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(m, path, sizeof(path))) return false;
    return FileSha1Hex(path, out_hex);
}

// ISO 8601 UTC timestamp like "2026-05-12T05:21:34Z".
void IsoUtcNow(char* out, size_t out_size) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    std::snprintf(out, out_size, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
}

// Escape a string for embedding in JSON. We hand-roll this because we
// don't want to pull a JSON library into the crash path. Handles the
// minimal set: " \ \n \r \t and \uXXXX for low control chars. Caller
// provides a sufficiently-sized output buffer.
void JsonEscape(const char* in, char* out, size_t out_size) {
    size_t oi = 0;
    if (out_size == 0) return;
    for (size_t i = 0; in[i] && oi + 8 < out_size; ++i) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
            case '"':  out[oi++] = '\\'; out[oi++] = '"';  break;
            case '\\': out[oi++] = '\\'; out[oi++] = '\\'; break;
            case '\n': out[oi++] = '\\'; out[oi++] = 'n';  break;
            case '\r': out[oi++] = '\\'; out[oi++] = 'r';  break;
            case '\t': out[oi++] = '\\'; out[oi++] = 't';  break;
            default:
                if (c < 0x20) {
                    oi += std::snprintf(out + oi, out_size - oi,
                                        "\\u%04x", c);
                } else {
                    out[oi++] = (char)c;
                }
                break;
        }
    }
    out[oi] = '\0';
}

}  // namespace

bool Enqueue(const Manifest& m) {
    char queue_dir[MAX_PATH] = {};
    if (!BuildQueueDir(queue_dir, sizeof(queue_dir))) return false;

    // unix-ms timestamp for the filename (sortable, unique enough).
    SYSTEMTIME st;
    GetSystemTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    uint64_t ms = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
    ms /= 10000;  // 100ns ticks → ms
    ms -= 11644473600000ULL;  // FILETIME epoch (1601) → unix epoch (1970)

    const char* kind = m.kind ? m.kind : "unknown";

    // Cross-peer match_id: hex of the 16-byte NAT-punch token. Same on
    // both peers post-punch, so when P0 and P1 each enqueue their own
    // desync report on the same match, the server's /by_match groups
    // them. Falls back to whatever the caller-supplied match_id was
    // (may be empty) if the token isn't latched.
    char match_token_hex[33] = {};
    const bool have_token = fm2k::nat::GetMatchTokenHex(
        match_token_hex, sizeof(match_token_hex));
    const char* match_id_str = have_token
        ? match_token_hex
        : (m.match_id ? m.match_id : "");

    // Bundle every referenced log file into a single ZIP. The launcher
    // only has to upload one file; server only has to store one
    // artifact. Pattern lifted from /mnt/c/dev/bbbr/revolve_input_sdl3
    // -- inline STORED-method writer, no compression deps, finishes
    // in tens of ms.
    char zip_path[MAX_PATH] = {};
    int zip_n = std::snprintf(zip_path, sizeof(zip_path),
                              "%s\\%llu_%s_p%d_f%d.zip",
                              queue_dir, (unsigned long long)ms,
                              kind, m.player_index, m.frame);
    bool zip_ok = false;
    if (zip_n > 0 && (size_t)zip_n < sizeof(zip_path)) {
        int wrote = fm2k::util::zip::WriteZip(zip_path, m.file_paths);
        zip_ok = (wrote > 0);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "UploadQueue: zip bundle wrote %d/%zu files → %s",
            wrote, m.file_paths.size(), zip_path);
    }

    char path[MAX_PATH] = {};
    int n = std::snprintf(path, sizeof(path),
                          "%s\\%llu_%s_p%d_%llu.json",
                          queue_dir,
                          (unsigned long long)ms, kind,
                          m.player_index,
                          (unsigned long long)m.session_id);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;

    char iso[32];
    IsoUtcNow(iso, sizeof(iso));

    // SHA1 stamp of the hook DLL -- pairs the upload with the exact
    // build artifact for symbolication. Skipped on crash path (caller
    // controls via leaving the field empty; this function always tries
    // to compute it, which is safe in normal threads; CrashHandler
    // wraps the whole enqueue in __try/__except itself).
    char hook_sha1[41] = {};
    HookDllSha1Hex(hook_sha1);

    // Escape user-supplied strings.
    char esc_kind[64];
    char esc_match[256];
    char esc_game[256];
    char esc_peer[64];
    JsonEscape(kind, esc_kind, sizeof(esc_kind));
    JsonEscape(match_id_str, esc_match, sizeof(esc_match));
    JsonEscape(m.game_id ? m.game_id : "", esc_game, sizeof(esc_game));
    JsonEscape(m.peer_ip ? m.peer_ip : "", esc_peer, sizeof(esc_peer));

    std::fprintf(f,
        "{\n"
        "  \"kind\": \"%s\",\n"
        "  \"frame\": %d,\n"
        "  \"session_id\": \"%llu\",\n"
        "  \"match_id\": \"%s\",\n"
        "  \"player_index\": %d,\n"
        "  \"client_version\": \"%s\",\n"
        "  \"game_id\": \"%s\",\n"
        "  \"hook_dll_sha1\": \"%s\",\n"
        "  \"rng_seed\": \"0x%08X\",\n"
        "  \"peer_ip\": \"%s\",\n"
        "  \"timestamp\": \"%s\",\n",
        esc_kind, m.frame,
        (unsigned long long)m.session_id,
        esc_match,
        m.player_index,
        fm2k::kAppVersion,
        esc_game,
        hook_sha1,
        (unsigned)m.rng_seed,
        esc_peer,
        iso);

    // If the zip bundle succeeded, manifest references ONLY the zip.
    // Launcher uploads one file, server stores one artifact, dev tool
    // pulls one bundle. If the zip failed (rare: filesystem full?),
    // fall back to the legacy multi-file listing so the launcher can
    // still try the individual files.
    std::fprintf(f, "  \"files\": [");
    if (zip_ok) {
        char esc_zip[MAX_PATH * 2];
        JsonEscape(zip_path, esc_zip, sizeof(esc_zip));
        std::fprintf(f, "\n    \"%s\"\n  ", esc_zip);
    } else {
        for (size_t i = 0; i < m.file_paths.size(); ++i) {
            char esc_path[MAX_PATH * 2];
            JsonEscape(m.file_paths[i].c_str(), esc_path, sizeof(esc_path));
            std::fprintf(f, "%s\n    \"%s\"",
                         i == 0 ? "" : ",", esc_path);
        }
        if (!m.file_paths.empty()) std::fprintf(f, "\n  ");
    }
    std::fprintf(f, "]\n}\n");

    std::fflush(f);
    std::fclose(f);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "UploadQueue: enqueued '%s' (%s, match_id=%s) → %s",
        kind,
        zip_ok ? "zipped" : "loose files",
        match_id_str[0] ? match_id_str : "<unset>",
        path);
    return true;
}

namespace {

// WideCharToMultiByte(CP_UTF8, ...) wrapped to dump straight into a
// std::string. Returns empty on conversion failure (which on Windows
// 7+ effectively never happens for valid UTF-16; the only failure
// mode left is an empty input).
bool WideToUtf8(const wchar_t* w, std::string& out) {
    out.clear();
    if (!w || !*w) return false;
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1,
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 1) return false;  // includes terminator
    out.resize((size_t)(need - 1));
    int got = WideCharToMultiByte(CP_UTF8, 0, w, -1,
                                  out.data(), need, nullptr, nullptr);
    return got == need;
}

}  // namespace

bool GetCurrentDirectoryUtf8(std::string& out) {
    out.clear();
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetCurrentDirectoryW(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return false;
    return WideToUtf8(buf, out);
}

bool GetModuleFileNameUtf8(std::string& out) {
    out.clear();
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    return WideToUtf8(buf, out);
}

}  // namespace fm2k::upload_queue
