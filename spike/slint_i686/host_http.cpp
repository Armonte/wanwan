#include "host_http.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace hostgui {
namespace {

constexpr int kTimeoutMs = 12000;

bool Split(const std::string& url, bool* https, std::wstring* host,
           uint16_t* port, std::wstring* path) {
    std::string s = url;
    if (s.rfind("https://", 0) == 0) { *https = true;  s = s.substr(8); }
    else if (s.rfind("http://", 0) == 0) { *https = false; s = s.substr(7); }
    else return false;
    const size_t slash = s.find('/');
    std::string hostport = slash == std::string::npos ? s : s.substr(0, slash);
    const std::string p  = slash == std::string::npos ? "/" : s.substr(slash);
    *port = *https ? 443 : 80;
    const size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        *port = (uint16_t)std::atoi(hostport.c_str() + colon + 1);
        hostport = hostport.substr(0, colon);
    }
    host->assign(hostport.begin(), hostport.end());
    path->assign(p.begin(), p.end());
    return true;
}

}  // namespace

bool HttpGet(const std::string& url, std::string* out) {
    bool https = false;
    std::wstring host, path;
    uint16_t port = 0;
    if (!Split(url, &https, &host, &port, &path)) return false;

    // DEFAULT_PROXY rather than AUTOMATIC: the automatic variant needs
    // Win8.1+ and fails outright on older systems, which some of these
    // games are still played on.
    HINTERNET ses = WinHttpOpen(L"2dfm-spike/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    bool ok = false;
    if (HINTERNET con = WinHttpConnect(ses, host.c_str(), port, 0)) {
        HINTERNET req = WinHttpOpenRequest(
            con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (req) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD code = 0, len = sizeof(code);
                WinHttpQueryHeaders(req,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
                if (code == 200) {
                    std::string body;
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                        std::vector<char> buf(avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(req, buf.data(), avail, &got) || got == 0) break;
                        body.append(buf.data(), got);
                    }
                    *out = std::move(body);
                    ok = true;
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}

}  // namespace hostgui
