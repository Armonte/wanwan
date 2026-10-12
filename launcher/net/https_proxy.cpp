// System proxy lookup for the HTTPS client. WinHTTP used to honour the
// machine proxy for us (WINHTTP_ACCESS_TYPE_DEFAULT_PROXY); with our own
// socket stack we read the user's static WinINet proxy ourselves:
//   HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings
//     ProxyEnable   DWORD  1 = use ProxyServer
//     ProxyServer   SZ     "host:port" or "http=h:p;https=h:p;..."
//     ProxyOverride SZ     "<local>;*.corp.example;10.*"
// The request is tunnelled with HTTP CONNECT, and HttpFetch retries direct
// if the proxy itself is unreachable. PAC/WPAD scripts are not evaluated.
#include "https_client.h"

#include <cctype>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fm2k::net {

namespace {

#if defined(_WIN32)
std::string ReadRegString(HKEY key, const wchar_t* name) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_SZ || size == 0 || size > 64 * 1024) {
        return "";
    }
    std::wstring w(size / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&w[0]), &size)
        != ERROR_SUCCESS) {
        return "";
    }
    w.resize(wcsnlen(w.c_str(), w.size()));
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) p = s.size();
        std::string part = s.substr(start, p - start);
        while (!part.empty() && std::isspace((unsigned char)part.front())) part.erase(part.begin());
        while (!part.empty() && std::isspace((unsigned char)part.back())) part.pop_back();
        if (!part.empty()) out.push_back(part);
        start = p + 1;
    }
    return out;
}

// '*' wildcard match, case-insensitive (both already lower-cased).
bool Glob(const std::string& pat, const std::string& s) {
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == s[i] || pat[p] == '?')) { ++p; ++i; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = i; }
        else if (star != std::string::npos) { p = star + 1; i = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

bool ParseHostPort(const std::string& hp, ProxyEndpoint& out) {
    std::string s = hp;
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    while (!s.empty() && s.back() == '/') s.pop_back();
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    const int port = std::atoi(s.substr(colon + 1).c_str());
    if (port <= 0 || port > 65535) return false;
    out.host = s.substr(0, colon);
    out.port = (uint16_t)port;
    return true;
}

}  // namespace

ProxyEndpoint SystemProxyFor(const std::string& host, bool tls) {
    ProxyEndpoint ep;
#if defined(_WIN32)
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return ep;
    }
    DWORD enable = 0, size = sizeof(enable), type = 0;
    const bool on = RegQueryValueExW(key, L"ProxyEnable", nullptr, &type,
                                     reinterpret_cast<BYTE*>(&enable), &size) == ERROR_SUCCESS &&
                    type == REG_DWORD && enable != 0;
    const std::string server   = on ? ReadRegString(key, L"ProxyServer") : "";
    const std::string override_list = on ? Lower(ReadRegString(key, L"ProxyOverride")) : "";
    RegCloseKey(key);
    if (server.empty()) return ep;

    const std::string h = Lower(host);
    for (const std::string& pat : Split(override_list, ';')) {
        if (pat == "<local>") {
            if (h.find('.') == std::string::npos || h == "127.0.0.1" || h == "::1") return ep;
        } else if (Glob(pat, h)) {
            return ep;
        }
    }
    if (h == "localhost" || h == "127.0.0.1" || h == "::1") return ep;

    if (server.find('=') == std::string::npos) {
        ParseHostPort(server, ep);
        return ep;
    }
    // Per-protocol list: prefer the matching scheme, then plain http.
    ProxyEndpoint http_ep;
    for (const std::string& entry : Split(server, ';')) {
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        const std::string proto = Lower(entry.substr(0, eq));
        ProxyEndpoint cand;
        if (!ParseHostPort(entry.substr(eq + 1), cand)) continue;
        if (proto == (tls ? "https" : "http")) return cand;
        if (proto == "http") http_ep = cand;
    }
    return http_ep;
#else
    (void)host;
    (void)tls;
    return ep;
#endif
}

}  // namespace fm2k::net
