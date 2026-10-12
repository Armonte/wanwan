// NetStream: TLS over a non-blocking socket with deadlines. See net_stream.h.
#include "net_stream_internal.h"
#include "tls/tls_internal.h"

#include <SDL3/SDL_log.h>

#include <climits>
#include <cstring>
#include <string>

#include "mbedtls/net_sockets.h"   // MBEDTLS_ERR_NET_* codes only (MBEDTLS_NET_C is off)
// psa_util.h has no extern "C" guard in 3.6, unlike the other public headers.
extern "C" {
#include "mbedtls/psa_util.h"      // mbedtls_psa_get_random
}
#include "mbedtls/x509.h"

namespace fm2k::net {

namespace {

const char* kAlpnHttp11[] = {"http/1.1", nullptr};

int BioSend(void* ctx, const unsigned char* buf, size_t len) {
    auto* im = static_cast<NetStream::Impl*>(ctx);
    const int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
    const int n = send(im->sock, reinterpret_cast<const char*>(buf), chunk, 0);
    if (n >= 0) return n;
    const int err = SockLastError();
    if (SockWouldBlock(err)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    im->last_sys = err;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

int BioRecv(void* ctx, unsigned char* buf, size_t len) {
    auto* im = static_cast<NetStream::Impl*>(ctx);
    const int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
    const int n = recv(im->sock, reinterpret_cast<char*>(buf), chunk, 0);
    if (n >= 0) return n;   // 0 = EOF; mbedTLS maps it to CONN_EOF
    const int err = SockLastError();
    if (SockWouldBlock(err)) return MBEDTLS_ERR_SSL_WANT_READ;
    im->last_sys = err;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

void FreeTls(NetStream::Impl& im) {
    if (!im.ssl_ready) return;
    mbedtls_ssl_free(&im.ssl);
    mbedtls_ssl_config_free(&im.conf);
    im.ssl_ready = false;
}

// One TLS handshake attempt on an already-connected socket.
bool Handshake(NetStream::Impl& im, tls::Anchors anchors, const ConnectOptions& opt,
               Clock::time_point deadline, NetStatus& st) {
    mbedtls_ssl_init(&im.ssl);
    mbedtls_ssl_config_init(&im.conf);
    im.ssl_ready = true;

    int r = mbedtls_ssl_config_defaults(&im.conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) { st.Set(NetError::Tls, "tls-config", r, tls::ErrText(r)); return false; }
    mbedtls_ssl_conf_authmode(&im.conf, MBEDTLS_SSL_VERIFY_REQUIRED);   // never relaxed
    mbedtls_ssl_conf_ca_chain(&im.conf, tls::TrustAnchors(anchors), nullptr);
    mbedtls_ssl_conf_rng(&im.conf, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
    mbedtls_ssl_conf_min_tls_version(&im.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    if (opt.alpn) mbedtls_ssl_conf_alpn_protocols(&im.conf, kAlpnHttp11);

    if ((r = mbedtls_ssl_setup(&im.ssl, &im.conf)) != 0 ||
        (r = mbedtls_ssl_set_hostname(&im.ssl, im.host.c_str())) != 0) {   // SNI + name check
        st.Set(NetError::Tls, "tls-setup", r, tls::ErrText(r));
        return false;
    }
    mbedtls_ssl_set_bio(&im.ssl, &im, BioSend, BioRecv, nullptr);

    for (;;) {
        r = mbedtls_ssl_handshake(&im.ssl);
        if (r == 0) return true;
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (im.Aborted()) { st.Set(NetError::Aborted, "tls-handshake", 0, ""); return false; }
        const int slice = MsLeft(deadline, 100);
        if (slice == 0) { st.Set(NetError::Timeout, "tls-handshake", 0, ""); return false; }
        WaitSock(im.sock, r == MBEDTLS_ERR_SSL_WANT_WRITE, slice);
    }

    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        const uint32_t flags = mbedtls_ssl_get_verify_result(&im.ssl);
        st.Set(NetError::TlsVerify, "tls-verify", r, tls::VerifyText(flags));
        st.verify = flags;
    } else if (r == MBEDTLS_ERR_NET_RECV_FAILED || r == MBEDTLS_ERR_NET_SEND_FAILED ||
               r == MBEDTLS_ERR_SSL_CONN_EOF) {
        st.Set(NetError::Closed, "tls-handshake", im.last_sys,
               "connection dropped during TLS handshake (socket error " +
               std::to_string(im.last_sys) + ")");
    } else {
        st.Set(NetError::Tls, "tls-handshake", r, tls::ErrText(r));
    }
    return false;
}

}  // namespace

NetStream::NetStream() : impl_(new Impl) {}
NetStream::~NetStream() { Close(); }

bool NetStream::is_open() const { return impl_->sock != kBadSock; }
bool NetStream::is_tls()  const { return impl_->tls; }

std::string NetStream::tls_info() const {
    if (!impl_->tls || !impl_->ssl_ready) return "";
    return std::string(mbedtls_ssl_get_version(&impl_->ssl)) + " " +
           mbedtls_ssl_get_ciphersuite(&impl_->ssl);
}

bool NetStream::Open(const std::string& host, uint16_t port, bool tls,
                     const ConnectOptions& opt, NetStatus& st) {
    Close();
    st = NetStatus{};
    Impl& im = *impl_;
    im.host  = host;
    im.tag   = opt.log_tag ? opt.log_tag : "net";
    im.abort = opt.should_abort;
    im.tls   = tls;

    if (tls && !tls::GlobalInit()) {
        st.Set(NetError::Tls, "tls-init", 0, "TLS library failed to initialise");
        return false;
    }

    // Start with the store; if it lacks the issuer, retry once with the
    // bundled Mozilla roots added (see ca_store.cpp for why Win7 needs it).
    tls::Anchors anchors = tls::PreferBundle() ? tls::Anchors::WithBundle : tls::Anchors::Primary;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(opt.timeout_ms);
        const bool via_proxy = opt.proxy.valid();
        im.sock = via_proxy
            ? ConnectTcp(opt.proxy.host, opt.proxy.port, deadline, im.abort, im.tag.c_str(), st)
            : ConnectTcp(host, port, deadline, im.abort, im.tag.c_str(), st);
        if (im.sock == kBadSock) return false;
        if (via_proxy && !ProxyConnect(im.sock, host, port, deadline, im.abort, st)) {
            Close();
            return false;
        }
        if (!tls) return true;

        const auto hs_deadline = Clock::now() + std::chrono::milliseconds(opt.timeout_ms);
        if (Handshake(im, anchors, opt, hs_deadline, st)) {
            if (attempt == 1) {
                tls::SetPreferBundle();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "%s: %s verified only with the bundled Mozilla roots; the Windows "
                            "ROOT store lacks that issuer. Using the bundle from now on.",
                            im.tag.c_str(), host.c_str());
            }
            return true;
        }
        const bool retry = attempt == 0 && anchors == tls::Anchors::Primary &&
                           st.code == NetError::TlsVerify &&
                           (st.verify & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0 &&
                           !(st.verify & ~(uint32_t)MBEDTLS_X509_BADCERT_NOT_TRUSTED);
        Close();
        if (!retry) break;
        anchors = tls::Anchors::WithBundle;
        st = NetStatus{};
        im.tls = true;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s: %s:%u %s", im.tag.c_str(), host.c_str(),
                (unsigned)port, st.Describe().c_str());
    return false;
}

long NetStream::Read(void* buf, size_t len, int timeout_ms, NetStatus& st) {
    Impl& im = *impl_;
    if (im.sock == kBadSock) { st.Set(NetError::Closed, "read", 0, "not connected"); return -1; }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int r;
        bool want_write = false;
        if (im.tls) {
            {
                std::lock_guard<std::mutex> lk(im.mtx);
                r = mbedtls_ssl_read(&im.ssl, static_cast<unsigned char*>(buf), len);
            }
            if (r > 0) return r;
            if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) {
                return 0;
            }
            if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (r == MBEDTLS_ERR_NET_RECV_FAILED || r == MBEDTLS_ERR_NET_SEND_FAILED) {
                    st.Set(NetError::Closed, "read", im.last_sys,
                           "socket error " + std::to_string(im.last_sys));
                } else {
                    st.Set(NetError::Tls, "read", r, tls::ErrText(r));
                }
                return -1;
            }
            want_write = (r == MBEDTLS_ERR_SSL_WANT_WRITE);
        } else {
            const int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
            r = recv(im.sock, static_cast<char*>(buf), chunk, 0);
            if (r >= 0) return r;
            const int err = SockLastError();
            if (!SockWouldBlock(err)) {
                st.Set(NetError::Closed, "read", err, "socket error " + std::to_string(err));
                return -1;
            }
        }
        if (im.Aborted()) { st.Set(NetError::Aborted, "read", 0, ""); return -1; }
        const int slice = MsLeft(deadline, 100);
        if (slice == 0) { st.Set(NetError::Timeout, "read", 0, ""); return -1; }
        if (WaitSock(im.sock, want_write, slice) < 0) {
            st.Set(NetError::Closed, "read", SockLastError(), "select failed");
            return -1;
        }
    }
}

bool NetStream::WriteAll(const void* buf, size_t len, int timeout_ms, NetStatus& st) {
    Impl& im = *impl_;
    if (im.sock == kBadSock) { st.Set(NetError::Closed, "write", 0, "not connected"); return false; }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::lock_guard<std::mutex> lk(im.mtx);   // held across waits: frames never interleave
    if (!im.tls) return SendAllRaw(im.sock, buf, len, deadline, im.abort, st);

    const unsigned char* p = static_cast<const unsigned char*>(buf);
    while (len > 0) {
        const int r = mbedtls_ssl_write(&im.ssl, p, len);
        if (r > 0) { p += r; len -= (size_t)r; continue; }
        if (r != MBEDTLS_ERR_SSL_WANT_WRITE && r != MBEDTLS_ERR_SSL_WANT_READ) {
            if (r == MBEDTLS_ERR_NET_SEND_FAILED || r == MBEDTLS_ERR_NET_RECV_FAILED) {
                st.Set(NetError::Closed, "write", im.last_sys,
                       "socket error " + std::to_string(im.last_sys));
            } else {
                st.Set(NetError::Tls, "write", r, tls::ErrText(r));
            }
            return false;
        }
        // mbedTLS requires the same call to be repeated until it completes,
        // which is why the lock stays held while we wait.
        if (im.Aborted()) { st.Set(NetError::Aborted, "write", 0, ""); return false; }
        const int slice = MsLeft(deadline, 100);
        if (slice == 0) { st.Set(NetError::Timeout, "write", 0, ""); return false; }
        WaitSock(im.sock, r == MBEDTLS_ERR_SSL_WANT_WRITE, slice);
    }
    return true;
}

void NetStream::Close() {
    Impl& im = *impl_;
    std::lock_guard<std::mutex> lk(im.mtx);
    if (im.ssl_ready && im.sock != kBadSock && im.tls) {
        mbedtls_ssl_close_notify(&im.ssl);   // one non-blocking attempt
    }
    FreeTls(im);
    SockClose(im.sock);
    im.sock = kBadSock;
}

}  // namespace fm2k::net
