// Multipart form POST over WinHTTP. Its own translation unit so the
// replay uploader and the diagnostics uploader share one implementation
// -- see FM2K_HttpPost.h.
#include "FM2K_HttpPost.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace fm2k::http {
namespace {

// Long enough that a slow VPS finishes, short enough that a dead one
// cannot pin the calling thread.
constexpr int HTTP_TIMEOUT_MS = 15000;

// Caddy fronts the hub with a 50 MB cap; stopping short of it means an
// oversized body fails here, with a name, instead of as a bare 413 from a
// proxy the caller never sees.
constexpr size_t MAX_BODY_BYTES = 48u * 1024u * 1024u;


bool ParseUrl(const std::string& url, bool& https_out, std::wstring& host_out,
              uint16_t& port_out, std::wstring& path_out) {
    https_out = false;
    std::string s = url;
    if (s.compare(0, 8, "https://") == 0) { https_out = true; s = s.substr(8); }
    else if (s.compare(0, 7, "http://") == 0) { s = s.substr(7); }
    else return false;
    size_t slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string path     = (slash == std::string::npos) ? "/" : s.substr(slash);
    size_t colon = hostport.find(':');
    std::string host = hostport;
    uint16_t port = https_out ? 443 : 80;
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = (uint16_t)std::atoi(hostport.substr(colon + 1).c_str());
    }
    host_out.assign(host.begin(), host.end());
    path_out.assign(path.begin(), path.end());
    port_out = port;
    return true;
}

std::string MakeBoundary() {
    // 16 bytes of randomness, hex-encoded. boundary must not appear
    // anywhere in the multipart body -- random 32-hex collision odds
    // are negligible for our payload sizes.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[33] = {};
    uint64_t a = rng(), b = rng();
    std::snprintf(buf, sizeof(buf),
                  "%08x%08x%08x%08x",
                  (unsigned)(a >> 32), (unsigned)(a & 0xFFFFFFFFu),
                  (unsigned)(b >> 32), (unsigned)(b & 0xFFFFFFFFu));
    return std::string("FM2KUpload-") + buf;
}

}  // namespace

bool PostMultipart(const std::string& url,
                       const std::string& secret,
                       const std::string& meta_json,
                       const std::vector<File>& files,
                       Response& out) {
    bool https = false;
    std::wstring host, path;
    uint16_t port = 0;
    if (!ParseUrl(url, https, host, port, path)) {
        out.failed_at = "ParseUrl";
        return false;
    }

    const std::string boundary = MakeBoundary();
    std::string body;
    body.reserve(meta_json.size() + 4096);
    for (const auto& f : files) body.reserve(body.size() + f.body.size() + 256);

    auto append = [&body](const std::string& s) { body.append(s); };

    // meta part
    append("--"); append(boundary); append("\r\n");
    append("Content-Disposition: form-data; name=\"meta\"\r\n\r\n");
    append(meta_json);
    append("\r\n");

    // file parts
    for (const auto& f : files) {
        append("--"); append(boundary); append("\r\n");
        append("Content-Disposition: form-data; name=\"files\"; filename=\"");
        append(f.filename);
        append("\"\r\n");
        append("Content-Type: application/octet-stream\r\n\r\n");
        body.append(f.body);
        append("\r\n");
    }
    append("--"); append(boundary); append("--\r\n");

    if (body.size() > MAX_BODY_BYTES) {
        out.failed_at = "body too large";
        return false;
    }

    HINTERNET hSes = WinHttpOpen(L"FM2K_Rollback/log-uploader",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,  // not AUTOMATIC: that needs Win8.1+ (WinHttpOpen -> 87 on Win8.0); DEFAULT works on every OS + honors system proxy
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) { out.last_error = GetLastError(); out.failed_at = "WinHttpOpen"; return false; }
    // Win8.0/7: enable TLS 1.2 (WinHTTP defaults to TLS 1.0 there). No-op on 8.1+.
    { DWORD sp = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(hSes, WINHTTP_OPTION_SECURE_PROTOCOLS, &sp, sizeof(sp)); }
    WinHttpSetTimeouts(hSes, HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS, HTTP_TIMEOUT_MS);

    HINTERNET hCon = WinHttpConnect(hSes, host.c_str(), port, 0);
    if (!hCon) {
        out.last_error = GetLastError(); out.failed_at = "WinHttpConnect";
        WinHttpCloseHandle(hSes);
        return false;
    }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hCon, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        out.last_error = GetLastError(); out.failed_at = "WinHttpOpenRequest";
        WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes);
        return false;
    }

    // SECURITY: X-FM2K-Log-Secret is a SHARED secret compiled into every
    // client (kLogUploadSecret) -- it is OBFUSCATION, NOT authentication. Any
    // user can extract it and forge/flood/poison the endpoint. The hub log-
    // ingest MUST therefore: (1) rate-limit + size-cap per source IP, (2) treat
    // every upload as untrusted (never index/execute its contents), and
    // (3) ideally issue a per-session upload token bound to the WS-authed
    // session instead of relying on this. Uploaded logs also carry match
    // tokens + peer IPs, so the secret leaking compounds the relay/STUN
    // findings -- scrub or gate those server-side.
    std::wstring headers = L"Content-Type: multipart/form-data; boundary=";
    headers.append(boundary.begin(), boundary.end());
    headers += L"\r\nX-FM2K-Log-Secret: ";
    headers.append(secret.begin(), secret.end());
    headers += L"\r\n";

    BOOL ok = WinHttpSendRequest(hReq,
        headers.c_str(), (DWORD)headers.size(),
        (LPVOID)body.data(), (DWORD)body.size(),
        (DWORD)body.size(), 0);
    if (!ok) {
        out.last_error = GetLastError(); out.failed_at = "WinHttpSendRequest";
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes);
        return false;
    }
    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        out.last_error = GetLastError(); out.failed_at = "WinHttpReceiveResponse";
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes);
        return false;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    out.status = (int)status;

    std::vector<char> respbuf;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        size_t off = respbuf.size();
        respbuf.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(hReq, respbuf.data() + off, avail, &got)) break;
        respbuf.resize(off + got);
        if (respbuf.size() > 16 * 1024) break;  // server returns tiny JSON
    }
    out.body.assign(respbuf.begin(), respbuf.end());

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);
    return true;
}

}  // namespace fm2k::http
