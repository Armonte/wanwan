// Multipart form POST over the launcher HTTPS client. Its own translation unit so the
// replay uploader and the diagnostics uploader share one implementation
// -- see FM2K_HttpPost.h.
#include "FM2K_HttpPost.h"

#include "https_client.h"

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

    // SECURITY: X-FM2K-Log-Secret is a SHARED secret compiled into every
    // client (kLogUploadSecret) -- it is OBFUSCATION, NOT authentication. Any
    // user can extract it and forge/flood/poison the endpoint. The hub log-
    // ingest MUST therefore: (1) rate-limit + size-cap per source IP, (2) treat
    // every upload as untrusted (never index/execute its contents), and
    // (3) ideally issue a per-session upload token bound to the WS-authed
    // session instead of relying on this. Uploaded logs also carry match
    // tokens + peer IPs, so the secret leaking compounds the relay/STUN
    // findings -- scrub or gate those server-side.
    net::HttpRequest req;
    req.method     = "POST";
    req.url        = url;
    req.user_agent = "FM2K_Rollback/log-uploader";
    req.timeout_ms = HTTP_TIMEOUT_MS;
    req.max_body   = 16 * 1024;   // the server returns tiny JSON
    req.log_tag    = "HttpPost";
    req.headers.push_back({"Content-Type", "multipart/form-data; boundary=" + boundary});
    req.headers.push_back({"X-FM2K-Log-Secret", secret});
    req.body = std::move(body);

    net::HttpResult res;
    const bool ok = net::HttpFetch(req, res);
    out.status = res.status;
    out.body   = std::move(res.body);
    if (!ok && res.status == 0) {
        out.last_error = res.last_error;
        out.failed_at  = res.failed_at;
        return false;
    }
    return true;
}

}  // namespace fm2k::http
