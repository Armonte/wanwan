#pragma once

// RFC 6455 WebSocket client over NetStream (ws:// and wss://). Replaces
// WinHttpWebSocket*, which does not exist before Windows 8.
//
// Thread model (matches the hub transport): one thread calls Receive() in
// a loop while another calls Send*(); every frame is written with a single
// NetStream::WriteAll so frames never interleave. Receive() answers pings
// and close frames itself.

#include "http_parse.h"
#include "net_stream.h"
#include "ws_frame.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fm2k::net {

struct WsConnectOptions {
    std::string         host;
    uint16_t            port = 443;
    std::string         path = "/";
    bool                tls  = true;
    int                 timeout_ms = 15000;
    std::string         user_agent = "FM2K-Launcher";
    std::vector<Header> headers;          // extra handshake headers
    size_t              max_message = 16u * 1024u * 1024u;
    std::function<bool()> should_abort;   // polled while connecting / waiting
    const char*         log_tag = "WS";
};

struct WsMessage {
    bool                 binary = false;
    std::vector<uint8_t> data;
};

class WsClient {
public:
    enum class Recv { Message, Timeout, Closed, Error };

    WsClient() = default;
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // TCP + TLS + HTTP upgrade with Sec-WebSocket-Accept verification.
    bool Connect(const WsConnectOptions& opt, NetStatus& st);

    bool SendText(const void* data, size_t len, NetStatus& st);
    bool SendBinary(const void* data, size_t len, NetStatus& st);
    bool SendPing(NetStatus& st);
    // Sends a close frame once (later calls are no-ops).
    bool SendClose(uint16_t code, const std::string& reason, NetStatus& st);

    // Waits up to `timeout_ms` for the next complete data message. Pings are
    // answered with pongs, pongs are swallowed, a close frame is echoed and
    // reported as Closed (close_code() holds the peer's status).
    Recv Receive(WsMessage& out, int timeout_ms, NetStatus& st);

    void Close();   // drops the connection (no close handshake)

    uint16_t    close_code() const { return close_code_; }
    std::string tls_info() const { return stream_.tls_info(); }

private:
    bool SendFrame(uint8_t opcode, const void* data, size_t len, NetStatus& st);

    NetStream            stream_;
    ws::FrameParser      parser_;
    ws::MessageAssembler assembler_;
    std::vector<uint8_t> pending_;     // received bytes not yet parsed
    int                  timeout_ms_ = 15000;
    std::atomic<bool>    close_sent_{false};
    uint16_t             close_code_ = ws::kCloseNoStatus;
};

}  // namespace fm2k::net
