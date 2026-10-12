#pragma once
// Private to net_stream.cpp / net_stream_socket.cpp: the socket shim and
// NetStream::Impl. Kept out of net_stream.h so callers do not pull in
// Winsock or mbedTLS headers.

#include "net_stream.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#endif

#include "mbedtls/ssl.h"

namespace fm2k::net {

#if defined(_WIN32)
using sock_t = SOCKET;
constexpr sock_t kBadSock = INVALID_SOCKET;
#else
using sock_t = int;
constexpr sock_t kBadSock = -1;
#endif

int  SockLastError();
bool SockWouldBlock(int err);
void SockClose(sock_t s);

using Clock = std::chrono::steady_clock;

// Milliseconds left until `deadline` (0 if passed), capped at `cap`.
int MsLeft(Clock::time_point deadline, int cap);

// Waits until `s` is readable (want_write=false) or writable. Returns 1 when
// ready, 0 on timeout, -1 on a socket error.
int WaitSock(sock_t s, bool want_write, int timeout_ms);

// Resolves `host` and connects a non-blocking TCP socket within the deadline.
// Polls `abort` every ~100 ms. Returns kBadSock with `st` set on failure.
sock_t ConnectTcp(const std::string& host, uint16_t port, Clock::time_point deadline,
                  const std::function<bool()>& abort, const char* tag, NetStatus& st);

// Sends an HTTP CONNECT through an already-connected proxy socket and
// checks for a 2xx reply.
bool ProxyConnect(sock_t s, const std::string& host, uint16_t port,
                  Clock::time_point deadline, const std::function<bool()>& abort,
                  NetStatus& st);

// Raw send/recv with deadline, for the pre-TLS proxy exchange and plain TCP.
bool SendAllRaw(sock_t s, const void* buf, size_t len, Clock::time_point deadline,
                const std::function<bool()>& abort, NetStatus& st);

struct NetStream::Impl {
    sock_t      sock = kBadSock;
    bool        tls  = false;
    bool        ssl_ready = false;
    int         last_sys = 0;            // last socket error seen by the BIO
    std::string host;
    std::string tag = "net";
    std::function<bool()> abort;
    std::mutex  mtx;                     // serialises every mbedtls_ssl_* call
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;

    bool Aborted() const { return abort && abort(); }
};

}  // namespace fm2k::net
