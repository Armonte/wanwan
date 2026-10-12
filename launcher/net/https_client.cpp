// HTTP/1.1 client over NetStream. See https_client.h.
#include "https_client.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace fm2k::net {

namespace {

constexpr size_t kErrorBodyCap = 64 * 1024;   // non-streamed error / redirect bodies
constexpr size_t kWriteSlice   = 64 * 1024;   // per-WriteAll piece of a request body

bool IsRedirect(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

std::string BuildHead(const Url& u, const HttpRequest& req, const std::string& method,
                      size_t body_len, bool send_length) {
    std::string h = method + " " + u.target + " HTTP/1.1\r\nHost: " + HostHeader(u) + "\r\n";
    if (!FindHeader(req.headers, "User-Agent")) h += "User-Agent: " + req.user_agent + "\r\n";
    if (!FindHeader(req.headers, "Accept"))     h += "Accept: */*\r\n";
    h += "Connection: close\r\n";
    if (send_length) h += "Content-Length: " + std::to_string(body_len) + "\r\n";
    for (const auto& hd : req.headers) {
        if (IEquals(hd.name, "Host") || IEquals(hd.name, "Connection") ||
            IEquals(hd.name, "Content-Length")) {
            continue;   // owned by the client
        }
        h += hd.name + ": " + hd.value + "\r\n";
    }
    h += "\r\n";
    return h;
}

struct Attempt {
    int                 status = 0;
    std::vector<Header> headers;
    std::string         body;
    NetStatus           net;
};

// One request/response on a fresh connection. Returns false on a transport
// failure before a response head arrived (a.net says why); true otherwise,
// with a.net set if the body then failed.
bool FetchOnce(const Url& u, const HttpRequest& req, const std::string& method,
               const std::string* body, const ProxyEndpoint& proxy, Attempt& a) {
    NetStream s;
    ConnectOptions co;
    co.timeout_ms   = req.timeout_ms;
    co.should_abort = req.should_abort;
    co.proxy        = proxy;
    co.log_tag      = req.log_tag;
    if (!s.Open(u.host, u.port, u.tls, co, a.net)) return false;

    const bool send_length = body != nullptr;
    const std::string head = BuildHead(u, req, method, body ? body->size() : 0, send_length);
    if (!s.WriteAll(head.data(), head.size(), req.timeout_ms, a.net)) return false;
    if (body) {
        for (size_t off = 0; off < body->size(); off += kWriteSlice) {
            const size_t n = std::min(kWriteSlice, body->size() - off);
            if (!s.WriteAll(body->data() + off, n, req.timeout_ms, a.net)) return false;
        }
    }

    std::string buf;
    char tmp[16 * 1024];
    ResponseHead rh;
    for (;;) {   // read the head, skipping interim 1xx responses
        const long consumed = ParseResponseHead(buf.data(), buf.size(), rh);
        if (consumed < 0) {
            a.net.Set(NetError::Protocol, "response-head", 0, "malformed HTTP response head");
            return false;
        }
        if (consumed > 0) {
            buf.erase(0, (size_t)consumed);
            if (rh.status >= 100 && rh.status < 200) continue;
            break;
        }
        const long n = s.Read(tmp, sizeof(tmp), req.timeout_ms, a.net);
        if (n < 0) { a.net.stage = "response-head"; return false; }
        if (n == 0) {
            a.net.Set(NetError::Closed, "response-head", 0, "server closed before responding");
            return false;
        }
        buf.append(tmp, (size_t)n);
    }
    a.status  = rh.status;
    a.headers = rh.headers;

    BodyDecoder dec;
    if (!dec.Init(rh, method == "HEAD")) {
        a.net.Set(NetError::Protocol, "response-body", 0, "invalid Content-Length");
        return true;
    }
    const bool stream = req.on_body && rh.status >= 200 && rh.status < 300;
    const size_t cap = stream ? 0 : (req.on_body || IsRedirect(rh.status) ? kErrorBodyCap : req.max_body);
    const uint64_t total = dec.declared_length();
    uint64_t received = 0;
    std::string piece;

    auto deliver = [&](const char* data, size_t len) -> bool {
        piece.clear();
        const long used = dec.Feed(data, len, piece);
        if (used < 0) {
            a.net.Set(NetError::Protocol, "response-body", 0, "malformed chunked body");
            return false;
        }
        if (piece.empty()) return true;
        received += piece.size();
        if (stream) {
            if (!req.on_body(piece.data(), piece.size())) {
                a.net.Set(NetError::Aborted, "response-body", 0, "body sink refused data");
                return false;
            }
            if (req.on_progress) req.on_progress(received, total);
        } else {
            if (a.body.size() + piece.size() > cap) {
                a.net.Set(NetError::TooLarge, "response-body", 0,
                          "body exceeds " + std::to_string(cap) + " bytes");
                return false;
            }
            a.body += piece;
        }
        return true;
    };

    if (!buf.empty() && !deliver(buf.data(), buf.size())) return true;
    while (!dec.done()) {
        const long n = s.Read(tmp, sizeof(tmp), req.timeout_ms, a.net);
        if (n < 0) { a.net.stage = "response-body"; return true; }
        if (n == 0) {
            if (!dec.OnEof()) {
                a.net.Set(NetError::Closed, "response-body", 0,
                          "connection closed after " + std::to_string(received) + " body bytes");
            }
            return true;
        }
        if (!deliver(tmp, (size_t)n)) return true;
    }
    return true;
}

bool ProxyWorthBypassing(const NetStatus& st) {
    return st.code == NetError::Proxy || st.code == NetError::Connect ||
           st.code == NetError::Dns || st.code == NetError::Timeout;
}

}  // namespace

bool HttpFetch(const HttpRequest& req, HttpResult& out) {
    out = HttpResult{};
    std::string method = req.method.empty() ? "GET" : req.method;
    const std::string* body =
        (!req.body.empty() || method == "POST" || method == "PUT") ? &req.body : nullptr;

    Url u;
    if (!ParseUrl(req.url, u) || u.scheme == "ws" || u.scheme == "wss") {
        out.net.Set(NetError::BadUrl, "ParseUrl", 0, "unsupported URL");
        out.failed_at  = "ParseUrl";
        out.last_error = LegacyWinHttpCode(NetError::BadUrl);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s: bad URL", req.log_tag);
        return false;
    }

    for (;;) {
        out.final_url = u.scheme + "://" + HostHeader(u) + u.target;
        Attempt a;
        bool got_head = false;
        const ProxyEndpoint proxy =
            req.use_system_proxy ? SystemProxyFor(u.host, u.tls) : ProxyEndpoint{};
        if (proxy.valid()) {
            got_head = FetchOnce(u, req, method, body, proxy, a);
            if (!got_head && ProxyWorthBypassing(a.net)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "%s: via proxy %s:%u failed (%s); retrying direct",
                            req.log_tag, proxy.host.c_str(), (unsigned)proxy.port,
                            a.net.Describe().c_str());
                a = Attempt{};
                got_head = FetchOnce(u, req, method, body, ProxyEndpoint{}, a);
            }
        } else {
            got_head = FetchOnce(u, req, method, body, ProxyEndpoint{}, a);
        }

        out.status  = a.status;
        out.headers = std::move(a.headers);
        out.body    = std::move(a.body);
        out.net     = a.net;
        if (!got_head || !a.net.ok()) {
            out.failed_at  = a.net.stage;
            out.last_error = LegacyWinHttpCode(a.net.code);
            if (a.net.code != NetError::Aborted) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s: %s %s://%s%s failed -- %s",
                            req.log_tag, method.c_str(), u.scheme.c_str(), u.host.c_str(),
                            u.target.substr(0, u.target.find('?')).c_str(),
                            a.net.Describe().c_str());
            }
            return false;
        }

        const std::string* loc = FindHeader(out.headers, "Location");
        if (!IsRedirect(out.status) || !loc || out.redirects >= req.max_redirects) return true;
        Url next;
        if (!ResolveRedirect(u, *loc, next)) return true;   // hand the 3xx to the caller
        if (u.tls && !next.tls) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%s: refusing HTTPS -> HTTP redirect to %s", req.log_tag, next.host.c_str());
            return true;
        }
        ++out.redirects;
        if (out.status == 303 || ((out.status == 301 || out.status == 302) && method == "POST")) {
            method = "GET";
            body   = nullptr;
        }
        u = next;
    }
}

}  // namespace fm2k::net
