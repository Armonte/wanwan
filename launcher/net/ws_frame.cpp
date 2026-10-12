// RFC 6455 framing. See ws_frame.h.
#include "ws_frame.h"

#include <cstring>

namespace fm2k::ws {

void ApplyMask(uint8_t* data, size_t len, const uint8_t key[4], size_t offset) {
    for (size_t i = 0; i < len; ++i) data[i] ^= key[(offset + i) & 3];
}

void EncodeFrame(std::vector<uint8_t>& out, uint8_t opcode, bool fin,
                 const void* payload, size_t len, bool mask, const uint8_t key[4]) {
    out.push_back((uint8_t)((fin ? 0x80 : 0x00) | (opcode & 0x0F)));
    const uint8_t mbit = mask ? 0x80 : 0x00;
    if (len <= 125) {
        out.push_back((uint8_t)(mbit | len));
    } else if (len <= 0xFFFF) {
        out.push_back((uint8_t)(mbit | 126));
        out.push_back((uint8_t)(len >> 8));
        out.push_back((uint8_t)(len & 0xFF));
    } else {
        out.push_back((uint8_t)(mbit | 127));
        const uint64_t l = (uint64_t)len;
        for (int s = 56; s >= 0; s -= 8) out.push_back((uint8_t)((l >> s) & 0xFF));
    }
    if (mask) out.insert(out.end(), key, key + 4);
    const size_t start = out.size();
    if (len) {
        const uint8_t* p = static_cast<const uint8_t*>(payload);
        out.insert(out.end(), p, p + len);
        if (mask) ApplyMask(out.data() + start, len, key, 0);
    }
}

std::vector<uint8_t> EncodeClosePayload(uint16_t code, const std::string& reason) {
    std::vector<uint8_t> p;
    p.push_back((uint8_t)(code >> 8));
    p.push_back((uint8_t)(code & 0xFF));
    const size_t n = reason.size() > 123 ? 123 : reason.size();
    p.insert(p.end(), reason.begin(), reason.begin() + (long)n);
    return p;
}

FrameParser::Result FrameParser::Parse(const uint8_t* data, size_t len,
                                       size_t& consumed, Frame& out) {
    consumed = 0;
    if (*error_) return Result::Error;
    auto fail = [this](const char* why) { error_ = why; return Result::Error; };

    while (!in_payload_) {
        while (hdr_.size() < need_hdr_ && consumed < len) hdr_.push_back(data[consumed++]);
        if (hdr_.size() < need_hdr_) return Result::NeedMore;

        if (need_hdr_ == 2) {
            const uint8_t b0 = hdr_[0], b1 = hdr_[1];
            if (b0 & 0x70) return fail("reserved bits set");
            const uint8_t op = b0 & 0x0F;
            const bool ctrl = (op & 0x08) != 0;
            if (op != kContinuation && op != kText && op != kBinary &&
                op != kClose && op != kPing && op != kPong) {
                return fail("unknown opcode");
            }
            const bool masked = (b1 & 0x80) != 0;
            if (masked && !allow_masked_) return fail("masked frame from server");
            const uint8_t l7 = b1 & 0x7F;
            if (ctrl && (!(b0 & 0x80) || l7 > 125)) return fail("bad control frame");
            need_hdr_ = 2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + (masked ? 4 : 0);
            if (need_hdr_ > 2) continue;   // gather the extended header
        }

        // Full header present.
        const uint8_t b0 = hdr_[0], b1 = hdr_[1];
        const uint8_t l7 = b1 & 0x7F;
        size_t p = 2;
        uint64_t plen = l7;
        if (l7 == 126) {
            plen = ((uint64_t)hdr_[2] << 8) | hdr_[3];
            p = 4;
        } else if (l7 == 127) {
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | hdr_[2 + i];
            if (plen >> 63) return fail("64-bit length top bit set");
            p = 10;
        }
        cur_ = Frame{};
        cur_.opcode = b0 & 0x0F;
        cur_.fin    = (b0 & 0x80) != 0;
        cur_.masked = (b1 & 0x80) != 0;
        if (cur_.masked) std::memcpy(key_, hdr_.data() + p, 4);
        if (plen > max_payload_) { too_big_ = true; return fail("frame too large"); }
        payload_len_ = plen;
        cur_.payload.reserve((size_t)plen);
        in_payload_ = true;
    }

    const size_t have = cur_.payload.size();
    const uint64_t want = payload_len_ - have;
    const size_t avail = len - consumed;
    const size_t take = (size_t)((uint64_t)avail < want ? (uint64_t)avail : want);
    if (take) {
        cur_.payload.insert(cur_.payload.end(), data + consumed, data + consumed + take);
        if (cur_.masked) ApplyMask(cur_.payload.data() + have, take, key_, have);
        consumed += take;
    }
    if (cur_.payload.size() < payload_len_) return Result::NeedMore;

    out = std::move(cur_);
    cur_ = Frame{};
    hdr_.clear();
    need_hdr_ = 2;
    in_payload_ = false;
    payload_len_ = 0;
    return Result::Frame;
}

bool MessageAssembler::Push(Frame&& f, Event& ev) {
    ev = Event{};
    auto error = [&ev](const char* why) {
        ev.kind  = Event::Kind::Error;
        ev.error = why;
        return true;
    };
    switch (f.opcode) {
    case kPing:
        ev.kind = Event::Kind::Ping;
        ev.data = std::move(f.payload);
        return true;
    case kPong:
        ev.kind = Event::Kind::Pong;
        ev.data = std::move(f.payload);
        return true;
    case kClose:
        ev.kind = Event::Kind::Close;
        if (f.payload.size() == 1) return error("1-byte close payload");
        if (f.payload.size() >= 2) {
            ev.close_code = (uint16_t)((f.payload[0] << 8) | f.payload[1]);
            ev.close_reason.assign(f.payload.begin() + 2, f.payload.end());
        }
        return true;
    case kText:
    case kBinary:
        if (in_message_) return error("new data frame inside fragmented message");
        if (f.fin) {
            ev.kind = (f.opcode == kText) ? Event::Kind::Text : Event::Kind::Binary;
            ev.data = std::move(f.payload);
            return true;
        }
        in_message_ = true;
        msg_opcode_ = f.opcode;
        buf_ = std::move(f.payload);
        if (buf_.size() > max_message_) return error("message too large");
        return false;
    case kContinuation:
        if (!in_message_) return error("continuation without a message");
        if (buf_.size() + f.payload.size() > max_message_) return error("message too large");
        buf_.insert(buf_.end(), f.payload.begin(), f.payload.end());
        if (!f.fin) return false;
        ev.kind = (msg_opcode_ == kText) ? Event::Kind::Text : Event::Kind::Binary;
        ev.data = std::move(buf_);
        buf_.clear();
        in_message_ = false;
        return true;
    default:
        return error("unknown opcode");
    }
}

// ---- SHA-1 (FIPS 180-4), handshake use only ----

namespace {
uint32_t Rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

void Sha1Block(uint32_t h[5], const uint8_t* blk) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) | (uint32_t)blk[i * 4 + 3];
    }
    for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        const uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
}  // namespace

void Sha1(const void* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t n = len;
    while (n >= 64) { Sha1Block(h, p); p += 64; n -= 64; }
    uint8_t tail[128] = {};
    std::memcpy(tail, p, n);
    tail[n] = 0x80;
    const size_t tlen = (n < 56) ? 64 : 128;
    const uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) tail[tlen - 1 - i] = (uint8_t)(bits >> (8 * i));
    Sha1Block(h, tail);
    if (tlen == 128) Sha1Block(h, tail + 64);
    for (int i = 0; i < 5; ++i) {
        out[i * 4]     = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

std::string Base64Encode(const void* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];  out += tbl[v & 63];
    }
    if (i < len) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < len) v |= (uint32_t)p[i + 1] << 8;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string ComputeAcceptKey(const std::string& client_key) {
    const std::string s = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    Sha1(s.data(), s.size(), digest);
    return Base64Encode(digest, sizeof(digest));
}

}  // namespace fm2k::ws
