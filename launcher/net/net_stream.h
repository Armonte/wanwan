#pragma once

// A TCP connection, optionally wrapped in TLS (bundled mbedTLS), with
// deadlines and cooperative cancellation. The one transport under both the
// HTTPS client (https_client.h) and the WebSocket client (ws_client.h).
//
// Replaces WinHTTP: WinHTTP's WebSocket API needs Windows 8, and Windows 7's
// schannel cannot reach modern TLS servers without a hotfix plus a registry
// edit. Our own stack behaves the same on every Windows version.
//
// Thread model: one reader thread and one writer thread may use a stream
// concurrently. Read() and WriteAll() serialise their TLS calls on an
// internal mutex, so a WebSocket sender can write while the receive loop
// waits for data. Open() and Close() must not race other calls.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fm2k::net {

enum class NetError {
    None = 0,
    BadUrl,          // unparseable / unsupported URL
    Dns,             // name did not resolve
    Connect,         // TCP refused / unreachable
    Timeout,         // connect, handshake or I/O deadline passed
    Proxy,           // HTTP proxy refused the CONNECT tunnel
    Tls,             // TLS handshake / record failure
    TlsVerify,       // certificate chain or hostname did not verify
    Protocol,        // malformed HTTP / WebSocket from the server
    Closed,          // peer closed the connection unexpectedly
    TooLarge,        // response exceeded the caller's limit
    Aborted,         // caller cancelled
    Io,              // local socket / file error
};

const char* NetErrorName(NetError e);

// The WinHTTP error number (12002 timeout, 12007 DNS, 12029 cannot connect,
// 12030 connection error, 12152 invalid response, 12175 secure failure) that
// corresponds to `e`, so existing log lines, hint tables and support notes
// keep meaning the same thing after the switch away from WinHTTP.
unsigned long LegacyWinHttpCode(NetError e);

struct NetStatus {
    NetError      code   = NetError::None;
    const char*   stage  = "";   // static string naming the step that failed
    int           sys    = 0;    // WSA error or mbedTLS code, 0 if none
    uint32_t      verify = 0;    // mbedTLS X.509 verify flags (TlsVerify)
    std::string   detail;        // human-readable cause for the log

    bool ok() const { return code == NetError::None; }
    void Set(NetError c, const char* where, int s, std::string d);
    std::string Describe() const;   // "<stage>: <name> (<detail>)"
};

struct ProxyEndpoint {
    std::string host;
    uint16_t    port = 0;
    bool valid() const { return !host.empty() && port != 0; }
};

struct ConnectOptions {
    int  timeout_ms = 15000;                  // TCP connect + TLS handshake, each
    std::function<bool()> should_abort;       // polled ~every 100 ms while blocked
    ProxyEndpoint proxy;                      // tunnel through HTTP CONNECT if valid
    const char*   alpn = "http/1.1";          // nullptr to omit ALPN
    const char*   log_tag = "net";
};

class NetStream {
public:
    NetStream();
    ~NetStream();
    NetStream(const NetStream&) = delete;
    NetStream& operator=(const NetStream&) = delete;

    // Resolves, connects (IPv4/IPv6, each address in turn), optionally
    // tunnels through a proxy, and performs the TLS handshake with chain +
    // hostname verification when `tls` is set.
    bool Open(const std::string& host, uint16_t port, bool tls,
              const ConnectOptions& opt, NetStatus& st);

    // Reads up to `len` bytes, waiting at most `timeout_ms`.
    // > 0: bytes read. 0: orderly close by the peer. -1: error or timeout
    // (st.code == Timeout when nothing arrived in time; the stream stays
    // usable after a timeout).
    long Read(void* buf, size_t len, int timeout_ms, NetStatus& st);

    // Writes all bytes or fails. Atomic with respect to other WriteAll calls.
    bool WriteAll(const void* buf, size_t len, int timeout_ms, NetStatus& st);

    // Best-effort TLS close_notify, then closes the socket. Idempotent.
    void Close();

    bool is_open() const;
    bool is_tls() const;
    std::string tls_info() const;   // "TLSv1.3 TLS1-3-AES-128-GCM-SHA256" or ""

    struct Impl;   // opaque; defined in net_stream_internal.h

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace fm2k::net
