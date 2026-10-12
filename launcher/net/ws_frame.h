#pragma once

// RFC 6455 WebSocket framing for the client side: frame encoding with
// masking, an incremental frame parser, a message assembler that handles
// fragmentation and interleaved control frames, and the handshake's
// Sec-WebSocket-Accept computation (SHA-1 + base64, self-contained so the
// host unit tests need no TLS library).
//
// Pure byte-buffer code: no sockets, no Windows, no logging. The socket
// side lives in ws_client.cpp; tests/ws_frame_test.cpp covers this file.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fm2k::ws {

enum Opcode : uint8_t {
    kContinuation = 0x0,
    kText         = 0x1,
    kBinary       = 0x2,
    kClose        = 0x8,
    kPing         = 0x9,
    kPong         = 0xA,
};

// Close status codes we send or interpret (RFC 6455 section 7.4.1).
enum CloseCode : uint16_t {
    kCloseNormal        = 1000,
    kCloseGoingAway     = 1001,
    kCloseProtocolError = 1002,
    kCloseNoStatus      = 1005,   // never sent on the wire
    kCloseTooBig        = 1009,
};

// XORs `len` bytes with the 4-byte key, starting at key phase `offset`
// (so a payload can be masked in pieces).
void ApplyMask(uint8_t* data, size_t len, const uint8_t key[4], size_t offset = 0);

// Appends one encoded frame to `out`. `mask` must be true for every frame
// a client sends (RFC 6455 5.3); `key` supplies the masking key then.
void EncodeFrame(std::vector<uint8_t>& out, uint8_t opcode, bool fin,
                 const void* payload, size_t len, bool mask, const uint8_t key[4]);

// Close frame payload: 2-byte big-endian code + UTF-8 reason (<= 123 bytes).
std::vector<uint8_t> EncodeClosePayload(uint16_t code, const std::string& reason);

struct Frame {
    uint8_t              opcode = 0;
    bool                 fin    = false;
    bool                 masked = false;
    std::vector<uint8_t> payload;   // unmasked
};

// Incremental frame parser. Feed bytes; it returns at most one frame per
// call. Enforces the client-side rules: RSV bits must be zero (we negotiate
// no extensions), opcodes must be known, control frames must be FIN and
// <= 125 bytes, 64-bit lengths must have the top bit clear, and frames from
// a server must NOT be masked (unless allow_masked, used by tests acting as
// the server).
class FrameParser {
public:
    enum class Result { NeedMore, Frame, Error };

    explicit FrameParser(size_t max_payload = 16u * 1024u * 1024u,
                         bool allow_masked = false)
        : max_payload_(max_payload), allow_masked_(allow_masked) {}

    // `consumed` receives the number of bytes used from `data`.
    Result Parse(const uint8_t* data, size_t len, size_t& consumed, Frame& out);

    const char* error() const { return error_; }
    bool too_big() const { return too_big_; }

private:
    std::vector<uint8_t> hdr_;      // header bytes gathered so far
    size_t               need_hdr_ = 2;
    bool                 in_payload_ = false;
    uint64_t             payload_len_ = 0;
    Frame                cur_;
    uint8_t              key_[4] = {0, 0, 0, 0};
    size_t               max_payload_;
    bool                 allow_masked_;
    const char*          error_ = "";
    bool                 too_big_ = false;
};

// One application-visible event produced from frames.
struct Event {
    enum class Kind { Text, Binary, Ping, Pong, Close, Error };
    Kind                 kind = Kind::Error;
    std::vector<uint8_t> data;          // message / ping / pong payload
    uint16_t             close_code = kCloseNoStatus;
    std::string          close_reason;
    const char*          error = "";
};

// Reassembles fragmented data messages; control frames may arrive between
// fragments and are surfaced immediately.
class MessageAssembler {
public:
    explicit MessageAssembler(size_t max_message = 16u * 1024u * 1024u)
        : max_message_(max_message) {}

    // Returns true when `ev` was filled (a complete message, a control
    // frame, or an Error).
    bool Push(Frame&& f, Event& ev);

private:
    bool                 in_message_ = false;
    uint8_t              msg_opcode_ = 0;
    std::vector<uint8_t> buf_;
    size_t               max_message_;
};

// SHA-1 (only for the handshake accept key; not used for security).
void Sha1(const void* data, size_t len, uint8_t out[20]);
std::string Base64Encode(const void* data, size_t len);

// Sec-WebSocket-Accept for a given Sec-WebSocket-Key (RFC 6455 4.2.2).
std::string ComputeAcceptKey(const std::string& client_key);

}  // namespace fm2k::ws
