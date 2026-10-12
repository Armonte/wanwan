// RFC 6455 WebSocket client. See ws_client.h.
#include "ws_client.h"
#include "tls/tls_internal.h"

#include <SDL3/SDL_log.h>

#include <chrono>
#include <cstring>
#include <string>

namespace fm2k::net {

namespace {

void FillRandom(uint8_t* out, size_t len) {
    if (tls::RandomBytes(out, len)) return;
    // PSA unavailable (should not happen): masking keys need not be secret
    // for security over TLS, only unpredictable to script-driven proxies.
    static uint32_t x = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
    for (size_t i = 0; i < len; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        out[i] = (uint8_t)x;
    }
}

}  // namespace

bool WsClient::Connect(const WsConnectOptions& opt, NetStatus& st) {
    Close();
    parser_     = ws::FrameParser(opt.max_message);
    assembler_  = ws::MessageAssembler(opt.max_message);
    pending_.clear();
    timeout_ms_ = opt.timeout_ms;
    close_sent_.store(false);
    close_code_ = ws::kCloseNoStatus;

    ConnectOptions co;
    co.timeout_ms   = opt.timeout_ms;
    co.should_abort = opt.should_abort;
    co.log_tag      = opt.log_tag;
    if (!stream_.Open(opt.host, opt.port, opt.tls, co, st)) return false;

    uint8_t nonce[16];
    FillRandom(nonce, sizeof(nonce));
    const std::string key = ws::Base64Encode(nonce, sizeof(nonce));

    Url u;
    u.scheme = opt.tls ? "wss" : "ws";
    u.host   = opt.host;
    u.port   = opt.port;
    std::string req = "GET " + (opt.path.empty() ? std::string("/") : opt.path) +
                      " HTTP/1.1\r\nHost: " + HostHeader(u) +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + key +
                      "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: " + opt.user_agent + "\r\n";
    for (const auto& h : opt.headers) req += h.name + ": " + h.value + "\r\n";
    req += "\r\n";
    if (!stream_.WriteAll(req.data(), req.size(), opt.timeout_ms, st)) {
        st.stage = "ws-upgrade";
        Close();
        return false;
    }

    std::string buf;
    char tmp[4096];
    ResponseHead rh;
    long consumed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt.timeout_ms);
    while ((consumed = ParseResponseHead(buf.data(), buf.size(), rh)) == 0) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { st.Set(NetError::Timeout, "ws-upgrade", 0, ""); Close(); return false; }
        const long n = stream_.Read(tmp, sizeof(tmp), (int)left, st);
        if (n < 0) { st.stage = "ws-upgrade"; Close(); return false; }
        if (n == 0) {
            st.Set(NetError::Closed, "ws-upgrade", 0, "server closed during upgrade");
            Close();
            return false;
        }
        buf.append(tmp, (size_t)n);
    }
    if (consumed < 0) {
        st.Set(NetError::Protocol, "ws-upgrade", 0, "malformed upgrade response");
        Close();
        return false;
    }
    if (rh.status != 101) {
        st.Set(NetError::Protocol, "ws-upgrade", rh.status,
               "server answered HTTP " + std::to_string(rh.status) + " instead of 101");
        Close();
        return false;
    }
    const std::string* up   = FindHeader(rh.headers, "Upgrade");
    const std::string* conn = FindHeader(rh.headers, "Connection");
    const std::string* acc  = FindHeader(rh.headers, "Sec-WebSocket-Accept");
    if (!up || !HeaderHasToken(*up, "websocket") || !conn || !HeaderHasToken(*conn, "upgrade")) {
        st.Set(NetError::Protocol, "ws-upgrade", 0, "missing Upgrade/Connection headers");
        Close();
        return false;
    }
    if (!acc || *acc != ws::ComputeAcceptKey(key)) {
        st.Set(NetError::Protocol, "ws-upgrade", 0, "Sec-WebSocket-Accept mismatch");
        Close();
        return false;
    }
    pending_.assign(buf.begin() + consumed, buf.end());   // frames sent right behind the 101
    return true;
}

bool WsClient::SendFrame(uint8_t opcode, const void* data, size_t len, NetStatus& st) {
    uint8_t mask[4];
    FillRandom(mask, sizeof(mask));
    std::vector<uint8_t> frame;
    frame.reserve(len + 14);
    ws::EncodeFrame(frame, opcode, true, data, len, /*mask=*/true, mask);
    // Generous per-frame deadline: a 32 KB spec-relay frame on a slow uplink
    // must not trip it, but a dead peer still surfaces.
    return stream_.WriteAll(frame.data(), frame.size(), timeout_ms_ * 2, st);
}

bool WsClient::SendText(const void* data, size_t len, NetStatus& st) {
    return SendFrame(ws::kText, data, len, st);
}

bool WsClient::SendBinary(const void* data, size_t len, NetStatus& st) {
    return SendFrame(ws::kBinary, data, len, st);
}

bool WsClient::SendPing(NetStatus& st) {
    return SendFrame(ws::kPing, nullptr, 0, st);
}

bool WsClient::SendClose(uint16_t code, const std::string& reason, NetStatus& st) {
    if (close_sent_.exchange(true)) return true;
    const std::vector<uint8_t> payload = ws::EncodeClosePayload(code, reason);
    return SendFrame(ws::kClose, payload.data(), payload.size(), st);
}

WsClient::Recv WsClient::Receive(WsMessage& out, int timeout_ms, NetStatus& st) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    uint8_t tmp[16 * 1024];
    for (;;) {
        // Drain whatever is already buffered before touching the socket.
        while (!pending_.empty()) {
            size_t used = 0;
            ws::Frame f;
            const auto r = parser_.Parse(pending_.data(), pending_.size(), used, f);
            pending_.erase(pending_.begin(), pending_.begin() + (long)used);
            if (r == ws::FrameParser::Result::Error) {
                NetStatus ignore;
                SendClose(parser_.too_big() ? ws::kCloseTooBig : ws::kCloseProtocolError,
                          "", ignore);
                st.Set(parser_.too_big() ? NetError::TooLarge : NetError::Protocol,
                       "ws-frame", 0, parser_.error());
                return Recv::Error;
            }
            if (r == ws::FrameParser::Result::NeedMore) break;

            ws::Event ev;
            if (!assembler_.Push(std::move(f), ev)) continue;
            switch (ev.kind) {
            case ws::Event::Kind::Text:
            case ws::Event::Kind::Binary:
                out.binary = (ev.kind == ws::Event::Kind::Binary);
                out.data   = std::move(ev.data);
                return Recv::Message;
            case ws::Event::Kind::Ping: {
                NetStatus ps;
                if (!SendFrame(ws::kPong, ev.data.data(), ev.data.size(), ps)) {
                    st = ps;
                    return Recv::Error;
                }
                break;
            }
            case ws::Event::Kind::Pong:
                break;
            case ws::Event::Kind::Close: {
                close_code_ = ev.close_code;
                NetStatus ignore;
                // Echo the peer's code (RFC 6455 5.5.1); 1005 must not be sent.
                SendClose(ev.close_code == ws::kCloseNoStatus ? (uint16_t)ws::kCloseNormal
                                                              : ev.close_code, "", ignore);
                st.Set(NetError::Closed, "ws-close", ev.close_code,
                       "server closed the WebSocket (code " + std::to_string(ev.close_code) +
                       (ev.close_reason.empty() ? "" : ", " + ev.close_reason) + ")");
                return Recv::Closed;
            }
            case ws::Event::Kind::Error: {
                NetStatus ignore;
                SendClose(ws::kCloseProtocolError, "", ignore);
                st.Set(NetError::Protocol, "ws-frame", 0, ev.error);
                return Recv::Error;
            }
            }
        }

        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { st.Set(NetError::Timeout, "ws-read", 0, ""); return Recv::Timeout; }
        NetStatus rs;
        const long n = stream_.Read(tmp, sizeof(tmp), (int)left, rs);
        if (n < 0) {
            st = rs;
            return rs.code == NetError::Timeout ? Recv::Timeout : Recv::Error;
        }
        if (n == 0) {
            st.Set(NetError::Closed, "ws-read", 0, "connection closed without a close frame");
            return Recv::Closed;
        }
        pending_.insert(pending_.end(), tmp, tmp + n);
    }
}

void WsClient::Close() {
    stream_.Close();
}

}  // namespace fm2k::net
