#pragma once

// HTTP/1.1 client-side parsing: URLs, response heads, and body framing
// (Content-Length, chunked, read-until-close).
//
// Pure functions over byte buffers -- no sockets, no Windows, no logging --
// so tests/http_parse_test.cpp exercises them with the host compiler. The
// socket side lives in https_client.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fm2k::net {

struct Url {
    std::string scheme;   // "http" | "https" | "ws" | "wss" (lower-cased)
    std::string host;     // no brackets for IPv6 literals
    uint16_t    port = 0; // explicit or scheme default
    std::string target;   // path + query, always starts with '/'
    bool        tls  = false;
    bool        explicit_port = false;
};

// Parses http://, https://, ws:// and wss:// URLs. Userinfo is rejected
// (we never send credentials in URLs) and so are empty hosts and bad ports.
bool ParseUrl(const std::string& url, Url& out);

// "host" or "host:port" as it belongs in a Host header (port omitted when
// it is the scheme default; IPv6 literals re-bracketed).
std::string HostHeader(const Url& u);

// Resolves a Location header against the URL that produced it. Handles
// absolute URLs, scheme-relative ("//host/x"), absolute paths ("/x") and
// relative paths ("x", "../x" is kept literal -- servers we talk to never
// emit dot segments). Returns false when the result is not http(s).
bool ResolveRedirect(const Url& base, const std::string& location, Url& out);

struct Header {
    std::string name;
    std::string value;
};

bool IEquals(const std::string& a, const char* b);

// Finds the first header named `name` (case-insensitive); nullptr if absent.
const std::string* FindHeader(const std::vector<Header>& headers, const char* name);

// True if a comma-separated header value contains `token` (case-insensitive),
// e.g. Connection: keep-alive, Upgrade.
bool HeaderHasToken(const std::string& value, const char* token);

struct ResponseHead {
    int                 status        = 0;
    int                 minor_version = 1;
    std::string         reason;
    std::vector<Header> headers;
};

// Upper bound on a response head; anything longer is treated as malformed
// so a hostile or broken server cannot make us buffer without limit.
constexpr size_t kMaxResponseHead = 64 * 1024;

// Parses a response status line plus headers from the front of `data`.
// Returns the number of bytes consumed (head + terminating blank line) when
// a complete head is present, 0 when more bytes are needed, -1 when the
// bytes are not a valid HTTP/1.x response head (or exceed kMaxResponseHead).
// Accepts bare-LF line endings as well as CRLF.
long ParseResponseHead(const char* data, size_t len, ResponseHead& out);

// Incremental body decoder. Initialise from the head, then feed raw bytes
// as they arrive; decoded body bytes are appended to `out`.
class BodyDecoder {
public:
    enum class Mode { None, ContentLength, Chunked, UntilClose };

    // `head_request` / 1xx / 204 / 304 responses carry no body.
    // Returns false if the framing headers are invalid (bad Content-Length).
    bool Init(const ResponseHead& head, bool head_request);

    // Consumes up to `len` bytes. Returns bytes consumed, or -1 on a framing
    // error. Bytes after the end of the body are left unconsumed.
    long Feed(const char* data, size_t len, std::string& out);

    // Called when the peer closes the connection. Returns true if that is a
    // valid end of body (UntilClose, or already done).
    bool OnEof();

    bool     done()  const { return done_; }
    bool     error() const { return error_; }
    Mode     mode()  const { return mode_; }
    // Declared Content-Length, or 0 when unknown (chunked / until-close).
    uint64_t declared_length() const { return mode_ == Mode::ContentLength ? total_ : 0; }

private:
    enum class Chunk { Size, Ext, SizeLf, Data, DataCr, DataLf, Trailer, TrailerLf };
    Mode     mode_      = Mode::None;
    Chunk    cstate_    = Chunk::Size;
    uint64_t total_     = 0;   // Content-Length
    uint64_t remaining_ = 0;   // bytes left in body (CL) or current chunk
    uint64_t chunk_size_ = 0;
    int      size_digits_ = 0;
    bool     trailer_line_empty_ = true;
    bool     done_  = false;
    bool     error_ = false;
};

}  // namespace fm2k::net
