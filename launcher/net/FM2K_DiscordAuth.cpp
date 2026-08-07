// FM2K_DiscordAuth -- launcher-side Discord OAuth pairing flow.
// See FM2K_DiscordAuth.h for the design.

#include "FM2K_DiscordAuth.h"

#include <SDL3/SDL_log.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace fm2k::discord_auth {

namespace {

// ---------------------------------------------------------------------------
// %APPDATA%\FM2K_Rollback\discord_auth.json -- flat string fields, hand
// parsed. We don't need a real JSON library for three string keys.
// ---------------------------------------------------------------------------

std::string AppDataDir() {
    char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string CachePath() {
    auto d = AppDataDir();
    if (d.empty()) return "discord_auth.json";
    return d + "\\discord_auth.json";
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Pull "key":"value" from a flat single-object JSON. Tolerant of
// whitespace; doesn't unescape complex sequences (we only write ASCII).
std::string JsonField(const std::string& src, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t p = src.find(needle);
    if (p == std::string::npos) return "";
    p = src.find(':', p + needle.size());
    if (p == std::string::npos) return "";
    p = src.find('"', p);
    if (p == std::string::npos) return "";
    size_t q = src.find('"', p + 1);
    if (q == std::string::npos) return "";
    std::string raw = src.substr(p + 1, q - p - 1);
    // Unescape JSON. Handles the simple two-char escapes plus \uXXXX
    // (single BMP code unit + surrogate pairs) → UTF-8 bytes. Without
    // \u handling, Discord nicks containing non-ASCII (e.g. "é" arrives
    // as "é") were displayed as the literal "u00e9".
    auto hex_nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto read_u4 = [&](size_t i) -> int {
        if (i + 3 >= raw.size()) return -1;
        int n = 0;
        for (int k = 0; k < 4; ++k) {
            int d = hex_nibble(raw[i + k]);
            if (d < 0) return -1;
            n = (n << 4) | d;
        }
        return n;
    };
    auto append_utf8 = [](std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    };

    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char n = raw[++i];
            switch (n) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    int u = read_u4(i + 1);
                    if (u < 0) { out += 'u'; break; }
                    i += 4;
                    // Surrogate pair: \uD800..\uDBFF followed by
                    // \uDC00..\uDFFF combine into one code point.
                    if (u >= 0xD800 && u <= 0xDBFF &&
                        i + 6 < raw.size() &&
                        raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                        int lo = read_u4(i + 3);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            uint32_t cp = 0x10000 +
                                ((uint32_t)(u  - 0xD800) << 10) +
                                ((uint32_t)(lo - 0xDC00));
                            append_utf8(out, cp);
                            i += 6;  // skip the low surrogate too
                            break;
                        }
                    }
                    append_utf8(out, (uint32_t)u);
                    break;
                }
                default: out += n; break;
            }
        } else {
            out += raw[i];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// WinHTTP helpers. We need GET (for /pair/begin and /pair/<code>) only.
// All requests are short text payloads, so we read into a std::string.
// ---------------------------------------------------------------------------

struct HttpResp {
    int         status   = 0;
    std::string body;
    // Win32 error from GetLastError after the failing WinHttp call.
    // Only meaningful when status == 0. Common WinHTTP codes:
    //   12002 ERROR_WINHTTP_TIMEOUT
    //   12007 ERROR_WINHTTP_NAME_NOT_RESOLVED   (DNS)
    //   12029 ERROR_WINHTTP_CANNOT_CONNECT      (TCP refused / firewalled)
    //   12030 ERROR_WINHTTP_CONNECTION_ERROR
    //   12152 ERROR_WINHTTP_INVALID_SERVER_RESPONSE
    //   12175 ERROR_WINHTTP_SECURE_FAILURE      (TLS issue)
    DWORD       last_error = 0;
    const char* failed_at  = "";   // string lit identifying the call site
};

bool ParseUrl(const std::string& url, bool& https_out, std::wstring& host_out,
              uint16_t& port_out, std::wstring& path_out) {
    // Accepts http:// or https://. Path defaults to "/".
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
    auto widen = [](const std::string& s) {
        std::wstring w(s.begin(), s.end()); return w;
    };
    host_out = widen(host);
    path_out = widen(path);
    port_out = port;
    return true;
}

// Internal: single attempt at a GET with the given proxy access mode.
// Returns out.status>0 on success, status==0 with last_error/failed_at
// populated on any WinHttp failure.
static HttpResp HttpGetOnce(const std::string& url, int timeout_ms,
                            DWORD access_type) {
    HttpResp out;
    bool         https = false;
    std::wstring host, path;
    uint16_t     port = 0;
    if (!ParseUrl(url, https, host, port, path)) {
        out.failed_at = "ParseUrl";
        return out;
    }

    HINTERNET hSes = WinHttpOpen(L"FM2K_Rollback/0.1",
        access_type,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) {
        out.last_error = GetLastError();
        out.failed_at  = "WinHttpOpen";
        return out;
    }
    // Windows 8.0 / 7: WinHTTP defaults to TLS 1.0 ONLY and will not negotiate
    // TLS 1.2, so HTTPS to a modern server (hub.2dfm.org, GitHub) dies at the
    // handshake even after WinHttpOpen succeeds -- the real wall the Win8.0
    // guest hit once the AUTOMATIC_PROXY/87 issue was past. Explicitly enable
    // TLS 1.0/1.1/1.2 on the session. No-op on 8.1+ (they already negotiate
    // 1.2); required on 8.0/7. Browsers work because they do their own TLS.
    {
        DWORD secure_protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1
                               | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1
                               | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(hSes, WINHTTP_OPTION_SECURE_PROTOCOLS,
                         &secure_protocols, sizeof(secure_protocols));
    }
    WinHttpSetTimeouts(hSes, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hCon = WinHttpConnect(hSes, host.c_str(), port, 0);
    if (!hCon) {
        out.last_error = GetLastError();
        out.failed_at  = "WinHttpConnect";
        WinHttpCloseHandle(hSes);
        return out;
    }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        out.last_error = GetLastError();
        out.failed_at  = "WinHttpOpenRequest";
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return out;
    }

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) {
        out.last_error = GetLastError();
        out.failed_at  = "WinHttpSendRequest";
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return out;
    }
    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        out.last_error = GetLastError();
        out.failed_at  = "WinHttpReceiveResponse";
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

// Public: walks an access-type ladder until one returns an HTTP response.
//   1. AUTOMATIC_PROXY -- WPAD auto-detect (corporate / managed networks).
//      BUT this access type only exists on Windows 8.1+; on Windows 8.0 and
//      earlier WinHttpOpen rejects it with ERROR_INVALID_PARAMETER (87)
//      BEFORE any network I/O. That 87 is pure noise on old OSes.
//   2. DEFAULT_PROXY -- honors the system/WinHTTP proxy config. Works on
//      EVERY Windows version, so this is what saves Win8.0 guests.
//   3. NO_PROXY -- direct, ignores proxy config (broken/orphaned VPN clients).
// On total failure we must NOT surface the auto-proxy 87: it masks the real
// reason (it's just "old OS doesn't know that access type"). Prefer an attempt
// that got PAST WinHttpOpen -- that carries the genuine network error.
HttpResp HttpGet(const std::string& url, int timeout_ms = 15000) {
    auto is_open_param_noise = [](const HttpResp& x) {
        return x.status == 0 && x.failed_at
            && std::strcmp(x.failed_at, "WinHttpOpen") == 0
            && x.last_error == ERROR_INVALID_PARAMETER;  // 87 = pre-8.1 auto-proxy
    };

    HttpResp r = HttpGetOnce(url, timeout_ms, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY);
    if (r.status > 0) return r;

    HttpResp rd = HttpGetOnce(url, timeout_ms, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY);
    if (rd.status > 0) return rd;

    HttpResp rn = HttpGetOnce(url, timeout_ms, WINHTTP_ACCESS_TYPE_NO_PROXY);
    if (rn.status > 0) return rn;

    // All three failed. Return the most informative error: skip the auto-proxy
    // 87 noise in favor of a real network-stage failure from a later attempt.
    if (is_open_param_noise(r)) {
        if (!is_open_param_noise(rn)) return rn;
        if (!is_open_param_noise(rd)) return rd;
    }
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Cache I/O
// ---------------------------------------------------------------------------

CachedAuth LoadCached() {
    CachedAuth a{};
    std::ifstream f(CachePath(), std::ios::binary);
    if (!f) return a;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    a.hub_token            = JsonField(s, "hub_token");
    a.discord_user_id      = JsonField(s, "discord_user_id");
    a.nick                 = JsonField(s, "nick");
    a.discord_global_name  = JsonField(s, "discord_global_name");
    // Default true for legacy files that pre-date the field -- matches the
    // struct's initializer. JsonField returns "" on missing key; treat
    // anything other than the literal string "false" as true.
    const std::string udn  = JsonField(s, "use_discord_name");
    a.use_discord_name     = (udn != "false");
    // Legacy migration: very old caches stored Discord's global_name in
    // `nick` and had no separate discord_global_name field. If we see a
    // non-empty nick but blank discord_global_name, copy nick over so
    // toggling "Use Discord name" still has something to fall back to.
    if (a.discord_global_name.empty() && !a.nick.empty()) {
        a.discord_global_name = a.nick;
    }
    a.valid = !a.hub_token.empty();
    return a;
}

bool SaveCached(const CachedAuth& a) {
    std::ofstream f(CachePath(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "{\n";
    f << "  \"hub_token\": \""           << JsonEscape(a.hub_token)            << "\",\n";
    f << "  \"discord_user_id\": \""     << JsonEscape(a.discord_user_id)      << "\",\n";
    f << "  \"nick\": \""                << JsonEscape(a.nick)                 << "\",\n";
    f << "  \"discord_global_name\": \"" << JsonEscape(a.discord_global_name)  << "\",\n";
    f << "  \"use_discord_name\": "      << (a.use_discord_name ? "true" : "false") << "\n";
    f << "}\n";
    return true;
}

void ClearCached() {
    std::error_code ec;
    std::filesystem::remove(CachePath(), ec);
}

// ---------------------------------------------------------------------------
// Pairing
// ---------------------------------------------------------------------------

Pairing::Status Pairing::status() const {
    return (Status)status_.load();
}

std::string Pairing::error_detail() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return error_;
}

CachedAuth Pairing::result() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return result_;
}

std::string Pairing::authorize_url() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return authorize_url_;
}

std::string Pairing::pairing_code() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pairing_code_;
}

bool Pairing::browser_open_failed() const {
    return browser_open_failed_.load();
}

void Pairing::Cancel() {
    cancel_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

Pairing::~Pairing() {
    Cancel();
}

Pairing* Begin(const std::string& hub_base_url) {
    auto* p = new (std::nothrow) Pairing();
    if (!p) return nullptr;
    p->hub_base_url_ = hub_base_url;

    p->worker_ = std::thread([p]() {
        // Step 1: ask the hub for a pairing code + authorize URL.
        HttpResp begin = HttpGet(p->hub_base_url_ + "/pair/begin");
        if (begin.status != 200) {
            // Map common WinHttp error codes to actionable hints. Code
            // list: docs.microsoft.com/en-us/windows/win32/winhttp/error-messages
            const char* hint = "";
            switch (begin.last_error) {
                case 12002: hint = " -- timeout, hub unreachable (firewall / port blocked / VPN)"; break;
                case 12007: hint = " -- DNS lookup failed for hub host"; break;
                case 12029: hint = " -- connection refused (firewall / AV web shield / hub down)"; break;
                case 12030: hint = " -- connection dropped"; break;
                case 12152: hint = " -- bad response from hub (hub crashed?)"; break;
                case 12175: hint = " -- TLS handshake failed (clock skew / cert issue)"; break;
                case 0:     hint = "";  break;  // normal HTTP-level failure
                default:    hint = " -- WinHttp error";  break;
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "Hub didn't respond (http=%d, win32=%lu, at=%s)%s\n"
                "Workarounds: pause AV web shield, run as admin, try VPN.",
                begin.status, (unsigned long)begin.last_error,
                begin.failed_at && *begin.failed_at ? begin.failed_at : "?",
                hint);
            std::lock_guard<std::mutex> lk(p->mtx_);
            p->error_ = buf;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "DiscordAuth: /pair/begin failed: %s", buf);
            p->status_.store((int)Pairing::Status::Error);
            return;
        }
        std::string pc  = JsonField(begin.body, "pairing_code");
        std::string url = JsonField(begin.body, "authorize_url");
        if (pc.empty() || url.empty()) {
            std::lock_guard<std::mutex> lk(p->mtx_);
            p->error_ = "Hub gave empty pairing_code/authorize_url";
            p->status_.store((int)Pairing::Status::Error);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(p->mtx_);
            p->pairing_code_   = pc;
            p->authorize_url_  = url;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "DiscordAuth: opening browser for pairing %s", pc.c_str());

        // Step 2: pop the OS browser to the Discord authorize URL.
        // ShellExecuteA returns HINSTANCE cast to INT_PTR; values > 32
        // mean success, <= 32 are SE_ERR_* failure codes (NOASSOC=31
        // when no http handler is registered, ACCESSDENIED=5, etc).
        // Common silent-failure cases:
        //   - launcher running as Administrator → UAC blocks spawning
        //     a non-elevated browser (success code may even be returned
        //     but the browser launches in a session the user can't see)
        //   - no default browser registered (Edge wiped post-install)
        //   - AV/EDR blocking the un-whitelisted exe's spawn
        // Whichever path fails, we ALSO show the URL in the auth modal
        // so manual paste is the always-works fallback.
        auto try_open = [&](const char* url_c) -> bool {
            HINSTANCE rc = ShellExecuteA(nullptr, "open", url_c,
                                         nullptr, nullptr, SW_SHOWNORMAL);
            return (INT_PTR)rc > 32;
        };
        bool opened = try_open(url.c_str());
        if (!opened) {
            // Fallback: spawn `cmd.exe /c start "" <url>` -- `start` uses
            // its own URL-handler resolution path which sometimes works
            // when ShellExecute("open") fails (e.g. when the http verb
            // is mis-registered but the URL falls through to whatever
            // 'start' considers default).
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "DiscordAuth: ShellExecute(\"open\") failed; "
                "trying cmd /c start fallback");
            std::string cmd = "/c start \"\" \"" + url + "\"";
            HINSTANCE rc = ShellExecuteA(nullptr, "open", "cmd.exe",
                                         cmd.c_str(), nullptr, SW_HIDE);
            opened = (INT_PTR)rc > 32;
        }
        if (!opened) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "DiscordAuth: browser auto-launch FAILED -- user must "
                "copy the URL from the auth modal and paste in browser "
                "manually. Common cause: launcher running as Admin "
                "(UAC blocks elevated→non-elevated browser spawn). "
                "URL: %s", url.c_str());
            // Atomic flag the UI thread polls to flip the modal into
            // "paste this URL" mode and auto-copy on first render.
            p->browser_open_failed_.store(true);
        }

        // Step 3: poll the hub for a result.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::minutes(5);
        while (!p->cancel_.load()) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::lock_guard<std::mutex> lk(p->mtx_);
                p->error_ = "Timed out waiting for Discord authorization";
                p->status_.store((int)Pairing::Status::Expired);
                return;
            }
            HttpResp poll = HttpGet(p->hub_base_url_ + "/pair/" + pc, 8000);
            if (poll.status == 200) {
                CachedAuth a;
                a.hub_token            = JsonField(poll.body, "hub_token");
                a.discord_user_id      = JsonField(poll.body, "discord_user_id");
                a.discord_global_name  = JsonField(poll.body, "nick");
                // Initial nick value: copy from Discord global_name on first
                // sign-in. Users who previously set a custom nick keep it
                // (LoadCached merges and the launcher's ApplyAuth path only
                // touches discord_* fields). use_discord_name defaults true
                // so the Hub panel shows their Discord identity until they
                // uncheck "Use Discord name" and type a custom nick.
                a.nick                 = a.discord_global_name;
                a.use_discord_name     = true;
                a.valid                = !a.hub_token.empty();
                if (a.valid) {
                    SaveCached(a);
                    {
                        std::lock_guard<std::mutex> lk(p->mtx_);
                        p->result_ = a;
                    }
                    p->status_.store((int)Pairing::Status::Ok);
                    return;
                }
            } else if (poll.status == 410) {
                std::lock_guard<std::mutex> lk(p->mtx_);
                p->error_ = "Pairing expired (browser closed?)";
                p->status_.store((int)Pairing::Status::Expired);
                return;
            } else if (poll.status == 400) {
                std::lock_guard<std::mutex> lk(p->mtx_);
                std::string reason = JsonField(poll.body, "reason");
                if (reason == "not_a_patron") {
                    p->error_ = "Discord says you don't have a Tester ($5+) role yet. "
                                "Pledge on Patreon and link your Discord, then try again.";
                } else if (!reason.empty()) {
                    p->error_ = "Hub rejected sign-in: " + reason;
                } else {
                    p->error_ = "Hub rejected sign-in";
                }
                p->status_.store((int)Pairing::Status::Error);
                return;
            }
            // 202 (pending) or transient failure -- wait and retry.
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });

    return p;
}

}  // namespace fm2k::discord_auth
