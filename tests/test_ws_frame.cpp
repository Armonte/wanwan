// RFC 6455 framing: launcher/net/ws_frame.cpp (hub WebSocket transport).
#include "doctest.h"
#include "ws_frame.h"

#include <cstring>
#include <string>
#include <vector>

using namespace fm2k::ws;

namespace {

const uint8_t kKey[4] = {0x37, 0xfa, 0x21, 0x3d};

// Parses every frame in `bytes`, feeding `step` bytes at a time.
std::vector<Frame> ParseAll(FrameParser& p, const std::vector<uint8_t>& bytes,
                            size_t step, bool* err = nullptr) {
    std::vector<Frame> frames;
    size_t pos = 0;
    while (pos < bytes.size()) {
        const size_t n = std::min(step, bytes.size() - pos);
        size_t off = 0;
        while (off < n) {
            size_t used = 0;
            Frame f;
            auto r = p.Parse(bytes.data() + pos + off, n - off, used, f);
            off += used;
            if (r == FrameParser::Result::Error) { if (err) *err = true; return frames; }
            if (r == FrameParser::Result::Frame) frames.push_back(std::move(f));
            else if (used == 0) break;
        }
        pos += n;
    }
    return frames;
}

std::vector<uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace

TEST_CASE("ws: RFC 6455 section 5.7 examples") {
    // Single-frame unmasked text "Hello".
    std::vector<uint8_t> out;
    EncodeFrame(out, kText, true, "Hello", 5, false, nullptr);
    CHECK(out == std::vector<uint8_t>{0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});

    // Single-frame masked text "Hello" with the RFC's key.
    out.clear();
    EncodeFrame(out, kText, true, "Hello", 5, true, kKey);
    CHECK(out == std::vector<uint8_t>{0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                      0x7f, 0x9f, 0x4d, 0x51, 0x58});

    // Unmasked ping "Hello".
    out.clear();
    EncodeFrame(out, kPing, true, "Hello", 5, false, nullptr);
    CHECK(out == std::vector<uint8_t>{0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});
}

TEST_CASE("ws: mask/unmask round trip at any phase") {
    std::vector<uint8_t> data = Bytes("The quick brown fox jumps over the lazy dog");
    std::vector<uint8_t> orig = data;
    ApplyMask(data.data(), data.size(), kKey, 0);
    CHECK(data != orig);
    // Unmask in two pieces with the phase carried across the split.
    ApplyMask(data.data(), 7, kKey, 0);
    ApplyMask(data.data() + 7, data.size() - 7, kKey, 7);
    CHECK(data == orig);
}

TEST_CASE("ws: extended lengths (126 and 127) parse back") {
    for (size_t len : {size_t(125), size_t(126), size_t(65535), size_t(65536), size_t(200000)}) {
        std::vector<uint8_t> payload(len);
        for (size_t i = 0; i < len; ++i) payload[i] = (uint8_t)(i * 31);
        std::vector<uint8_t> wire;
        EncodeFrame(wire, kBinary, true, payload.data(), len, true, kKey);
        if (len <= 125)      CHECK((wire[1] & 0x7F) == len);
        else if (len < 65536) CHECK((wire[1] & 0x7F) == 126);
        else                 CHECK((wire[1] & 0x7F) == 127);
        FrameParser p(1u << 20, /*allow_masked=*/true);
        auto frames = ParseAll(p, wire, 1000);
        REQUIRE(frames.size() == 1);
        CHECK(frames[0].opcode == kBinary);
        CHECK(frames[0].masked);
        CHECK(frames[0].payload == payload);
    }
}

TEST_CASE("ws: byte-at-a-time parsing of several frames") {
    std::vector<uint8_t> wire;
    EncodeFrame(wire, kText, true, "abc", 3, false, nullptr);
    EncodeFrame(wire, kPing, true, "", 0, false, nullptr);
    std::vector<uint8_t> big(300, 'x');
    EncodeFrame(wire, kBinary, true, big.data(), big.size(), false, nullptr);
    FrameParser p;
    auto frames = ParseAll(p, wire, 1);
    REQUIRE(frames.size() == 3);
    CHECK(frames[0].payload == Bytes("abc"));
    CHECK(frames[1].opcode == kPing);
    CHECK(frames[1].payload.empty());
    CHECK(frames[2].payload == big);
}

TEST_CASE("ws: client rejects masked server frames and protocol violations") {
    auto rejects = [](std::vector<uint8_t> wire) {
        FrameParser p;
        bool err = false;
        ParseAll(p, wire, wire.size(), &err);
        return err;
    };
    std::vector<uint8_t> masked;
    EncodeFrame(masked, kText, true, "hi", 2, true, kKey);
    CHECK(rejects(masked));
    CHECK(rejects({0xC1, 0x00}));                 // RSV1 set
    CHECK(rejects({0x83, 0x00}));                 // opcode 3 reserved
    CHECK(rejects({0x09, 0x00}));                 // fragmented ping
    CHECK(rejects({0x89, 0x7E, 0x00, 0x80}));     // ping with 126-byte length
    CHECK(rejects({0x82, 0x7F, 0x80, 0, 0, 0, 0, 0, 0, 1}));  // 64-bit top bit
    FrameParser small(16);
    size_t used = 0;
    Frame f;
    std::vector<uint8_t> big;
    EncodeFrame(big, kBinary, true, std::string(17, 'a').data(), 17, false, nullptr);
    CHECK(small.Parse(big.data(), big.size(), used, f) == FrameParser::Result::Error);
    CHECK(small.too_big());
}

TEST_CASE("ws: fragmentation with an interleaved control frame") {
    std::vector<uint8_t> wire;
    EncodeFrame(wire, kText, false, "Hel", 3, false, nullptr);
    EncodeFrame(wire, kPing, true, "p", 1, false, nullptr);
    EncodeFrame(wire, kContinuation, false, "lo ", 3, false, nullptr);
    EncodeFrame(wire, kContinuation, true, "world", 5, false, nullptr);
    FrameParser p;
    auto frames = ParseAll(p, wire, 2);
    REQUIRE(frames.size() == 4);
    MessageAssembler a;
    std::vector<Event> events;
    for (auto& f : frames) {
        Event ev;
        if (a.Push(std::move(f), ev)) events.push_back(std::move(ev));
    }
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == Event::Kind::Ping);
    CHECK(events[0].data == Bytes("p"));
    CHECK(events[1].kind == Event::Kind::Text);
    CHECK(events[1].data == Bytes("Hello world"));
}

TEST_CASE("ws: fragmented binary and assembler protocol errors") {
    MessageAssembler a;
    Event ev;
    Frame f1; f1.opcode = kBinary; f1.fin = false; f1.payload = {1, 2};
    Frame f2; f2.opcode = kContinuation; f2.fin = true; f2.payload = {3};
    CHECK_FALSE(a.Push(std::move(f1), ev));
    REQUIRE(a.Push(std::move(f2), ev));
    CHECK(ev.kind == Event::Kind::Binary);
    CHECK(ev.data == std::vector<uint8_t>{1, 2, 3});

    MessageAssembler b;
    Frame orphan; orphan.opcode = kContinuation; orphan.fin = true;
    REQUIRE(b.Push(std::move(orphan), ev));
    CHECK(ev.kind == Event::Kind::Error);

    MessageAssembler c;
    Frame s; s.opcode = kText; s.fin = false; s.payload = {'a'};
    Frame s2; s2.opcode = kText; s2.fin = true; s2.payload = {'b'};
    CHECK_FALSE(c.Push(std::move(s), ev));
    REQUIRE(c.Push(std::move(s2), ev));
    CHECK(ev.kind == Event::Kind::Error);

    MessageAssembler d(4);
    Frame big; big.opcode = kText; big.fin = false; big.payload = {'a', 'b', 'c'};
    Frame big2; big2.opcode = kContinuation; big2.fin = true; big2.payload = {'d', 'e'};
    CHECK_FALSE(d.Push(std::move(big), ev));
    REQUIRE(d.Push(std::move(big2), ev));
    CHECK(ev.kind == Event::Kind::Error);
}

TEST_CASE("ws: close frames") {
    auto payload = EncodeClosePayload(kCloseNormal, "bye");
    CHECK(payload == std::vector<uint8_t>{0x03, 0xE8, 'b', 'y', 'e'});
    CHECK(EncodeClosePayload(1000, std::string(200, 'r')).size() == 125);

    std::vector<uint8_t> wire;
    EncodeFrame(wire, kClose, true, payload.data(), payload.size(), false, nullptr);
    FrameParser p;
    auto frames = ParseAll(p, wire, wire.size());
    REQUIRE(frames.size() == 1);
    MessageAssembler a;
    Event ev;
    REQUIRE(a.Push(std::move(frames[0]), ev));
    CHECK(ev.kind == Event::Kind::Close);
    CHECK(ev.close_code == 1000);
    CHECK(ev.close_reason == "bye");

    Frame empty; empty.opcode = kClose; empty.fin = true;
    REQUIRE(a.Push(std::move(empty), ev));
    CHECK(ev.close_code == kCloseNoStatus);

    Frame bad; bad.opcode = kClose; bad.fin = true; bad.payload = {0x03};
    REQUIRE(a.Push(std::move(bad), ev));
    CHECK(ev.kind == Event::Kind::Error);
}

TEST_CASE("ws: handshake accept key, SHA-1 and base64 vectors") {
    // RFC 6455 section 1.3 example.
    CHECK(ComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    uint8_t d[20];
    Sha1("abc", 3, d);
    CHECK(Base64Encode(d, 20) == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
    std::string m(1000, 'a');
    Sha1(m.data(), m.size(), d);   // crosses several blocks + 2-block tail
    CHECK(Base64Encode(d, 20) == "KR6abGaZSUm1e6XmUDYemPw2sbo=");
    CHECK(Base64Encode("", 0).empty());
    CHECK(Base64Encode("f", 1) == "Zg==");
    CHECK(Base64Encode("fo", 2) == "Zm8=");
    CHECK(Base64Encode("foo", 3) == "Zm9v");
}
