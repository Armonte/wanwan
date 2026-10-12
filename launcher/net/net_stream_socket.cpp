// TCP side of NetStream: Winsock startup, name resolution, connect with a
// deadline, the HTTP CONNECT proxy tunnel, and NetStatus helpers.
#include "net_stream_internal.h"
#include "http_parse.h"

#include <SDL3/SDL_log.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <unistd.h>
#endif

namespace fm2k::net {

const char* NetErrorName(NetError e) {
    switch (e) {
    case NetError::None:      return "ok";
    case NetError::BadUrl:    return "bad URL";
    case NetError::Dns:       return "name not resolved";
    case NetError::Connect:   return "cannot connect";
    case NetError::Timeout:   return "timed out";
    case NetError::Proxy:     return "proxy refused tunnel";
    case NetError::Tls:       return "TLS handshake failed";
    case NetError::TlsVerify: return "certificate not trusted";
    case NetError::Protocol:  return "invalid server response";
    case NetError::Closed:    return "connection closed";
    case NetError::TooLarge:  return "response too large";
    case NetError::Aborted:   return "cancelled";
    case NetError::Io:        return "I/O error";
    }
    return "?";
}

unsigned long LegacyWinHttpCode(NetError e) {
    switch (e) {
    case NetError::None:      return 0;
    case NetError::Timeout:   return 12002;  // ERROR_WINHTTP_TIMEOUT
    case NetError::Dns:       return 12007;  // ERROR_WINHTTP_NAME_NOT_RESOLVED
    case NetError::Connect:
    case NetError::Proxy:     return 12029;  // ERROR_WINHTTP_CANNOT_CONNECT
    case NetError::Closed:
    case NetError::Io:        return 12030;  // ERROR_WINHTTP_CONNECTION_ERROR
    case NetError::Protocol:
    case NetError::TooLarge:  return 12152;  // ERROR_WINHTTP_INVALID_SERVER_RESPONSE
    case NetError::Tls:
    case NetError::TlsVerify: return 12175;  // ERROR_WINHTTP_SECURE_FAILURE
    case NetError::BadUrl:    return 12005;  // ERROR_WINHTTP_INVALID_URL
    case NetError::Aborted:   return 12017;  // ERROR_WINHTTP_OPERATION_CANCELLED
    }
    return 0;
}

void NetStatus::Set(NetError c, const char* where, int s, std::string d) {
    code   = c;
    stage  = where;
    sys    = s;
    detail = std::move(d);
}

std::string NetStatus::Describe() const {
    std::string s = std::string(stage && *stage ? stage : "net") + ": " + NetErrorName(code);
    if (!detail.empty()) s += " (" + detail + ")";
    return s;
}

int SockLastError() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool SockWouldBlock(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS;
#endif
}

void SockClose(sock_t s) {
    if (s == kBadSock) return;
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

int MsLeft(Clock::time_point deadline, int cap) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now()).count();
    if (left <= 0) return 0;
    return left < cap ? (int)left : cap;
}

int WaitSock(sock_t s, bool want_write, int timeout_ms) {
    fd_set rd, wr, ex;
    FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
    if (want_write) FD_SET(s, &wr); else FD_SET(s, &rd);
    FD_SET(s, &ex);
    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int r = select((int)s + 1, &rd, &wr, &ex, &tv);
    if (r < 0) return -1;
    if (r == 0) return 0;
    return 1;   // readable / writable / exceptional: the next call reports the cause
}

namespace {

std::once_flag g_wsa_once;

void EnsureSockets() {
#if defined(_WIN32)
    // Process-lifetime Winsock reference; never WSACleanup'd so no other
    // module's sockets can be torn down underneath it.
    std::call_once(g_wsa_once, []() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif
}

bool SetNonBlocking(sock_t s) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    const int fl = fcntl(s, F_GETFL, 0);
    return fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

}  // namespace

sock_t ConnectTcp(const std::string& host, uint16_t port, Clock::time_point deadline,
                  const std::function<bool()>& abort, const char* tag, NetStatus& st) {
    EnsureSockets();
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string port_s = std::to_string(port);
    const int gai = getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        st.Set(NetError::Dns, "resolve", gai, "host \"" + host + "\"");
        return kBadSock;
    }

    int addr_count = 0;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) ++addr_count;

    sock_t result = kBadSock;
    int last_err = 0;
    bool timed_out = false;
    int tried = 0;
    for (addrinfo* ai = res; ai && result == kBadSock; ai = ai->ai_next, ++tried) {
        // Every address but the last gets at most 3 s, so a black-holed first
        // address -- typically a broken IPv6 route -- cannot eat the whole
        // budget before IPv4 is tried. The last one gets whatever is left.
        const int remaining = MsLeft(deadline, 1 << 30);
        if (remaining == 0) { timed_out = true; break; }
        const int left_addrs = addr_count - tried;
        const int share = left_addrs > 1 ? (remaining < 3000 ? remaining : 3000) : remaining;
        const auto addr_deadline = Clock::now() + std::chrono::milliseconds(share);

        sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kBadSock) { last_err = SockLastError(); continue; }
        if (!SetNonBlocking(s)) { last_err = SockLastError(); SockClose(s); continue; }
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) { result = s; break; }
        const int err = SockLastError();
        if (!SockWouldBlock(err)) { last_err = err; SockClose(s); continue; }

        bool done = false;
        while (!done) {
            if (abort && abort()) {
                SockClose(s);
                freeaddrinfo(res);
                st.Set(NetError::Aborted, "connect", 0, "");
                return kBadSock;
            }
            const int slice = MsLeft(addr_deadline, 100);
            if (slice == 0) { timed_out = true; last_err = 0; break; }
            const int w = WaitSock(s, true, slice);
            if (w == 0) continue;
            int so_err = 0;
            socklen_t sl = sizeof(so_err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &sl);
            if (w > 0 && so_err == 0) { result = s; }
            else { last_err = so_err ? so_err : SockLastError(); }
            done = true;
        }
        if (result != s) SockClose(s);
        if (result == kBadSock && ai->ai_next) timed_out = false;   // try the next address
    }
    freeaddrinfo(res);

    if (result == kBadSock) {
        if (timed_out) st.Set(NetError::Timeout, "connect", 0, host + ":" + port_s);
        else st.Set(NetError::Connect, "connect", last_err,
                    host + ":" + port_s + ", socket error " + std::to_string(last_err));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", tag, st.Describe().c_str());
    }
    return result;
}

bool SendAllRaw(sock_t s, const void* buf, size_t len, Clock::time_point deadline,
                const std::function<bool()>& abort, NetStatus& st) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        const int chunk = len > (1u << 30) ? (1 << 30) : (int)len;
        const int n = send(s, p, chunk, 0);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        const int err = SockLastError();
        if (!SockWouldBlock(err)) {
            st.Set(NetError::Closed, "send", err, "socket error " + std::to_string(err));
            return false;
        }
        if (abort && abort()) { st.Set(NetError::Aborted, "send", 0, ""); return false; }
        const int slice = MsLeft(deadline, 100);
        if (slice == 0) { st.Set(NetError::Timeout, "send", 0, ""); return false; }
        WaitSock(s, true, slice);
    }
    return true;
}

bool ProxyConnect(sock_t s, const std::string& host, uint16_t port,
                  Clock::time_point deadline, const std::function<bool()>& abort,
                  NetStatus& st) {
    const std::string authority = (host.find(':') != std::string::npos ? "[" + host + "]" : host) +
                                  ":" + std::to_string(port);
    const std::string req = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority +
                            "\r\nProxy-Connection: keep-alive\r\n\r\n";
    if (!SendAllRaw(s, req.data(), req.size(), deadline, abort, st)) {
        st.stage = "proxy";
        return false;
    }
    // Read byte-wise up to the end of the head so no tunnelled TLS bytes
    // are swallowed (the reply is tiny, so this costs nothing).
    std::string head;
    for (;;) {
        char c;
        const int n = recv(s, &c, 1, 0);
        if (n == 1) {
            head += c;
            if (head.size() > kMaxResponseHead) break;
            ResponseHead h;
            const long r = ParseResponseHead(head.data(), head.size(), h);
            if (r < 0) break;
            if (r > 0) {
                if (h.status >= 200 && h.status < 300) return true;
                st.Set(NetError::Proxy, "proxy", h.status,
                       "proxy answered HTTP " + std::to_string(h.status));
                return false;
            }
            continue;
        }
        if (n == 0) { st.Set(NetError::Proxy, "proxy", 0, "proxy closed the connection"); return false; }
        const int err = SockLastError();
        if (!SockWouldBlock(err)) {
            st.Set(NetError::Proxy, "proxy", err, "socket error " + std::to_string(err));
            return false;
        }
        if (abort && abort()) { st.Set(NetError::Aborted, "proxy", 0, ""); return false; }
        const int slice = MsLeft(deadline, 100);
        if (slice == 0) { st.Set(NetError::Timeout, "proxy", 0, ""); return false; }
        WaitSock(s, false, slice);
    }
    st.Set(NetError::Proxy, "proxy", 0, "malformed proxy reply");
    return false;
}

}  // namespace fm2k::net
