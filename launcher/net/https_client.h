#pragma once

// Small HTTP/1.1 client over NetStream (bundled TLS + Winsock): GET/POST,
// Content-Length and chunked bodies, redirects up to a limit, timeouts,
// cancellation, streaming downloads, and the system's static HTTP proxy.
//
// Every launcher HTTP(S) call goes through HttpFetch(). It replaced WinHTTP
// and WinINet so the launcher runs on a stock Windows 7 SP1 (no TLS 1.2
// hotfix, no WebSocket API) with the same code path as Windows 10/11.
//
// Each request uses a fresh connection with "Connection: close"; our call
// volume is a handful of requests per session, so keep-alive is not worth
// its complexity.

#include "http_parse.h"
#include "net_stream.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fm2k::net {

struct HttpRequest {
    std::string         method = "GET";
    std::string         url;
    std::vector<Header> headers;      // extra request headers
    std::string         body;         // sent with Content-Length when non-empty or POST/PUT
    std::string         user_agent = "FM2K_Rollback";
    int                 timeout_ms = 15000;   // connect, handshake, and per-read inactivity
    int                 max_redirects = 5;    // 0 = return 3xx responses as-is
    size_t              max_body = 16u * 1024u * 1024u;  // buffered-body cap
    bool                use_system_proxy = true;
    const char*         log_tag = "HTTP";

    // Optional streaming sink for a 2xx response body (redirect and error
    // bodies are still buffered, capped at 64 KB, for diagnostics). Return
    // false to abort the transfer.
    std::function<bool(const char* data, size_t len)> on_body;
    // Optional progress callback for streamed bodies: (received, total or 0).
    std::function<void(uint64_t received, uint64_t total)> on_progress;
    // Polled while blocked; return true to cancel.
    std::function<bool()> should_abort;
};

struct HttpResult {
    int                 status = 0;    // final HTTP status, 0 if none was received
    std::vector<Header> headers;
    std::string         body;          // buffered body (empty when streamed)
    std::string         final_url;
    int                 redirects = 0;
    NetStatus           net;           // set when status == 0 or the body failed
    // WinHTTP-equivalent error number for the failure (see LegacyWinHttpCode).
    unsigned long       last_error = 0;
    const char*         failed_at  = "";

    // A complete response arrived (any status) and its body was read whole.
    bool ok() const { return status > 0 && net.ok(); }
};

// Performs the request, following redirects. Returns HttpResult::ok().
// Logs failures through SDL_Log (the launcher's disk + UI log sink).
bool HttpFetch(const HttpRequest& req, HttpResult& out);

// The user's static WinINet proxy ("Internet Options" > LAN settings) if one
// applies to `host`; empty otherwise. PAC/WPAD scripts are not evaluated.
ProxyEndpoint SystemProxyFor(const std::string& host, bool tls);

}  // namespace fm2k::net
