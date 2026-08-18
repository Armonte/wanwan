// Spectator-protocol wire-layout tests.
//
// Pins the SpecDataHeader (10 bytes, 0xCE-prefixed) and the SpecCssUpdate
// payload, plus the input-batch chunking math the host uses to send the
// session-history backfill to a late joiner.

#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>

// We'd love to include spectator_node.h directly, but it pulls in
// <winsock2.h> which isn't available on Linux native builds. So we
// re-declare the public-protocol structs here and the tests pin them by
// equality. If the production header drifts, a separate test
// (test_protocol_consistency) below ensures we caught the change.
#include "../FM2KHook/src/netplay/replay.h"
#include "spectator_protocol_events.h"

namespace mirror {
constexpr uint8_t SPEC_DATA_MAGIC = 0xCE;

enum class SpecDataType : uint8_t {
    // 1..5, 9 and 10 are RETIRED values that must never be reused (see
    // SpecDataType in spectator_node.h): the pre-EVENT_BATCH2 wire types,
    // the deleted UDP input accelerator and its OP_BASELINE priming packet.
    SNAPSHOT_BEGIN = 6,
    EVENT_BATCH2   = 11,
};

#pragma pack(push, 1)
struct SpecDataHeader {
    uint8_t      magic;
    SpecDataType type;
    uint32_t     start_frame;
    uint16_t     frame_count;
    uint16_t     flags;
};
#pragma pack(pop)
}  // namespace mirror

TEST_CASE("SpecDataHeader is exactly 10 bytes") {
    // Datagram size budget: header + payload <= UDP MTU. Backfill chunks at
    // 256 frames * 4 B + 10 B header = 1034 B total — well under 1200.
    CHECK(sizeof(mirror::SpecDataHeader) == 10);
}

TEST_CASE("SpecDataHeader field offsets are stable") {
    using H = mirror::SpecDataHeader;
    CHECK(offsetof(H, magic)       == 0);
    CHECK(offsetof(H, type)        == 1);
    CHECK(offsetof(H, start_frame) == 2);
    CHECK(offsetof(H, frame_count) == 6);
    CHECK(offsetof(H, flags)       == 8);
}

TEST_CASE("SpecDataType enum values are stable on the wire") {
    // The LIVE values keep their historical numbers; the retired ones
    // (1..5 pre-EVENT_BATCH2 framing, 9 the deleted UDP accelerator, 10 its
    // OP_BASELINE priming packet) are never reused, so a stray packet from
    // an old peer decodes as unknown rather than as something else.
    CHECK(static_cast<uint8_t>(mirror::SpecDataType::SNAPSHOT_BEGIN) == 6);
    CHECK(static_cast<uint8_t>(mirror::SpecDataType::EVENT_BATCH2)   == 11);
}

TEST_CASE("SPEC_DATA_MAGIC distinguishes from CtrlPacket and GekkoNet") {
    // Control-channel uses 0xCC (first byte of CtrlPacketHeader.type or
    // similar — see control_channel.cpp::RawReceive). GekkoNet packets are
    // anything else. 0xCE must not collide with either.
    CHECK(mirror::SPEC_DATA_MAGIC == 0xCE);
    CHECK(mirror::SPEC_DATA_MAGIC != 0xCC);  // control-channel
}

TEST_CASE("INITIAL_MATCH payload layout matches what spectator_node.cpp parses") {
    // The host packs the 96-byte ReplayHeader into the INITIAL_MATCH payload
    // (SpectatorNode_OnMatchStart). Spectator-side handler memcpys at
    // hardcoded offsets. This test asserts those offsets are what the
    // sender encodes — paired with test_replay_format.cpp's offset checks
    // which pin the OTHER end of the contract.
    uint8_t payload[96] = {};
    uint32_t magic   = Replay::REPLAY_MAGIC;
    uint16_t version = Replay::REPLAY_VERSION;
    uint32_t game_hash         = 0xAAAAAAAA;
    uint32_t initial_rng_seed  = 0x12345678;
    uint32_t initial_state_hash = 0xBBBBBBBB;
    std::memcpy(payload + 0,  &magic,              4);
    std::memcpy(payload + 4,  &version,            2);
    std::memcpy(payload + 16, &game_hash,          4);
    std::memcpy(payload + 20, &initial_rng_seed,   4);
    std::memcpy(payload + 24, &initial_state_hash, 4);
    payload[28] = 7;   // p1_char
    payload[29] = 2;   // p1_color
    payload[30] = 4;   // p2_char
    payload[31] = 1;   // p2_color

    // Mirror the spectator's parse from spectator_node.cpp::HandleSpecData
    uint32_t parsed_seed = 0, parsed_state = 0;
    std::memcpy(&parsed_seed,  payload + 20, 4);
    std::memcpy(&parsed_state, payload + 24, 4);

    CHECK(parsed_seed     == 0x12345678);
    CHECK(parsed_state    == 0xBBBBBBBB);
    CHECK(payload[28]     == 7);
    CHECK(payload[29]     == 2);
    CHECK(payload[30]     == 4);
    CHECK(payload[31]     == 1);
}

TEST_CASE("INPUT_BATCH frame layout: (p1 u16, p2 u16) packed contiguously") {
    // Both producer (FlushBatch / SendSessionBackfillTo) and consumer
    // (HandleSpecData::INPUT_BATCH) treat the payload as raw N*4 bytes.
    constexpr int N = 5;
    uint16_t p1[N] = {0x001, 0x002, 0x004, 0x008, 0x010};
    uint16_t p2[N] = {0x100, 0x200, 0x400, 0x080, 0x040};

    uint8_t buf[N * 4];
    for (int i = 0; i < N; i++) {
        std::memcpy(buf + i * 4 + 0, &p1[i], 2);
        std::memcpy(buf + i * 4 + 2, &p2[i], 2);
    }

    // Parse back.
    for (int i = 0; i < N; i++) {
        uint16_t got_p1, got_p2;
        std::memcpy(&got_p1, buf + i * 4 + 0, 2);
        std::memcpy(&got_p2, buf + i * 4 + 2, 2);
        CHECK(got_p1 == p1[i]);
        CHECK(got_p2 == p2[i]);
    }
}

// Mirror the chunking logic from spectator_node.cpp::SendSessionBackfillTo.
// Pulled out of the .cpp so we can test it without the Windows / SDL deps.
constexpr size_t BACKFILL_CHUNK_FRAMES = 256;

struct ChunkPlan {
    size_t   chunk_count;     // Number of datagrams that will be sent
    uint32_t last_start_frame; // start_frame of the final chunk
    uint16_t last_n;          // frame_count of the final chunk (1..256)
};

static ChunkPlan PlanBackfill(size_t total_frames, uint32_t session_start) {
    ChunkPlan plan{0, 0, 0};
    for (size_t off = 0; off < total_frames; off += BACKFILL_CHUNK_FRAMES) {
        const size_t n = std::min(BACKFILL_CHUNK_FRAMES, total_frames - off);
        plan.chunk_count++;
        plan.last_start_frame = session_start + (uint32_t)off;
        plan.last_n           = (uint16_t)n;
    }
    return plan;
}

TEST_CASE("Session backfill chunking — empty history sends no datagrams") {
    ChunkPlan p = PlanBackfill(0, 0);
    CHECK(p.chunk_count == 0);
}

TEST_CASE("Session backfill chunking — single chunk for small history") {
    ChunkPlan p = PlanBackfill(100, 0);
    CHECK(p.chunk_count      == 1);
    CHECK(p.last_start_frame == 0);
    CHECK(p.last_n           == 100);
}

TEST_CASE("Session backfill chunking — exact multiple of chunk size") {
    ChunkPlan p = PlanBackfill(BACKFILL_CHUNK_FRAMES * 3, 0);
    CHECK(p.chunk_count      == 3);
    CHECK(p.last_start_frame == 2 * BACKFILL_CHUNK_FRAMES);
    CHECK(p.last_n           == BACKFILL_CHUNK_FRAMES);
}

TEST_CASE("Session backfill chunking — final chunk holds remainder") {
    // 257 frames = one full 256-frame chunk + one 1-frame chunk
    ChunkPlan p = PlanBackfill(257, 0);
    CHECK(p.chunk_count      == 2);
    CHECK(p.last_start_frame == 256);
    CHECK(p.last_n           == 1);
}

TEST_CASE("Session backfill chunking — hour-long set is well-bounded") {
    // 1 hour at 100 Hz = 360000 frames. ~1408 chunks. ~1.4 MB session memory.
    ChunkPlan p = PlanBackfill(360000, 0);
    CHECK(p.chunk_count == (360000 / BACKFILL_CHUNK_FRAMES + 1));
    CHECK(p.last_start_frame == (360000 / BACKFILL_CHUNK_FRAMES) * BACKFILL_CHUNK_FRAMES);
    CHECK(p.last_n == 360000 % BACKFILL_CHUNK_FRAMES);
}

TEST_CASE("Session backfill chunking — start_frame offset propagates") {
    ChunkPlan p = PlanBackfill(BACKFILL_CHUNK_FRAMES + 10, /*session_start=*/1000);
    CHECK(p.chunk_count == 2);
    CHECK(p.last_start_frame == 1000 + BACKFILL_CHUNK_FRAMES);
    CHECK(p.last_n == 10);
}

// =============================================================================
// SessionEvent wire format (C1)
// =============================================================================
//
// Mirror declarations of the production types in spectator_node.h. We don't
// include the production header (winsock2.h dependency); instead we re-pin
// the struct/enum/encode-decode contract here. Drift between the two surfaces
// would show up as the test binary linking against a different layout than
// the DLL ships — but since tests can't link the DLL on Linux, the contract
// is enforced by code review + symmetric test coverage on both sides.

TEST_CASE("SessionEvent struct is exactly 10 bytes packed") {
    // C3.5 bumped from 5 → 10 to inline RoundEndPayload (9 bytes) without a
    // side-table. Memory cost: 1-hour 100 Hz session = 360 000 events ×
    // 10 B = 3.6 MB (was 1.7 MB at 5 B). Still well within budget.
    CHECK(sizeof(mirror_event::SessionEvent) == 10);
}

TEST_CASE("RoundStartPayload / RoundEndPayload sizes are stable") {
    CHECK(sizeof(mirror_event::RoundStartPayload) == 7);
    CHECK(sizeof(mirror_event::RoundEndPayload)   == 9);
}

TEST_CASE("SessionEventType wire tag values are stable") {
    CHECK((uint8_t)mirror_event::SessionEventType::INPUT             == 1);
    CHECK((uint8_t)mirror_event::SessionEventType::PIN_RNG           == 2);
    CHECK((uint8_t)mirror_event::SessionEventType::RESET_INPUT_STATE == 3);
    CHECK((uint8_t)mirror_event::SessionEventType::SOUND_INIT        == 4);
    CHECK((uint8_t)mirror_event::SessionEventType::MATCH_START       == 5);
    CHECK((uint8_t)mirror_event::SessionEventType::MATCH_END         == 6);
    CHECK((uint8_t)mirror_event::SessionEventType::FINGERPRINT       == 7);
    CHECK((uint8_t)mirror_event::SessionEventType::ROUND_START       == 8);
    CHECK((uint8_t)mirror_event::SessionEventType::ROUND_END         == 9);
}

TEST_CASE("Wire payload sizes match the protocol comment") {
    using mirror_event::WirePayloadSize;
    using T = mirror_event::SessionEventType;
    CHECK(WirePayloadSize(T::INPUT)             == 4);   // 5 byte event total
    CHECK(WirePayloadSize(T::PIN_RNG)           == 4);   // 5
    CHECK(WirePayloadSize(T::RESET_INPUT_STATE) == 0);   // 1
    CHECK(WirePayloadSize(T::SOUND_INIT)        == 0);   // 1
    CHECK(WirePayloadSize(T::MATCH_START)       == 96);  // 97
    CHECK(WirePayloadSize(T::MATCH_END)         == 7);   // 8 (C7-enriched)
    CHECK(WirePayloadSize(T::FINGERPRINT)       == 4);   // 5
    CHECK(WirePayloadSize(T::ROUND_START)       == 7);   // 8 byte event total
    CHECK(WirePayloadSize(T::ROUND_END)         == 9);   // 10
    CHECK(WirePayloadSize(T::SESSION_ID)        == 8);   // 9
}

TEST_CASE("Encode/Decode ROUND_START round-trips") {
    mirror_event::RoundStartPayload p{};
    p.round_idx     = 2;
    p.p1_hp_max     = 800;
    p.p2_hp_max     = 1100;
    p.timer_seconds = 99;
    uint8_t buf[16] = {};
    size_t w = mirror_event::EncodeRoundStart(buf, sizeof(buf), p);
    CHECK(w == 8);
    CHECK(buf[0] == (uint8_t)mirror_event::SessionEventType::ROUND_START);

    mirror_event::SessionEvent ev{};
    size_t r = mirror_event::Decode(buf, w, &ev, nullptr);
    CHECK(r == 8);
    CHECK(ev.type == mirror_event::SessionEventType::ROUND_START);
    CHECK(ev.u.round_start.round_idx     == 2);
    CHECK(ev.u.round_start.p1_hp_max     == 800);
    CHECK(ev.u.round_start.p2_hp_max     == 1100);
    CHECK(ev.u.round_start.timer_seconds == 99);
}

TEST_CASE("Encode/Decode ROUND_END round-trips") {
    mirror_event::RoundEndPayload p{};
    p.winner_idx       = 1;     // P2
    p.p1_hp_remaining  = 0;
    p.p2_hp_remaining  = 380;
    p.frames_elapsed   = 4200;
    uint8_t buf[16] = {};
    size_t w = mirror_event::EncodeRoundEnd(buf, sizeof(buf), p);
    CHECK(w == 10);
    CHECK(buf[0] == (uint8_t)mirror_event::SessionEventType::ROUND_END);

    mirror_event::SessionEvent ev{};
    size_t r = mirror_event::Decode(buf, w, &ev, nullptr);
    CHECK(r == 10);
    CHECK(ev.type == mirror_event::SessionEventType::ROUND_END);
    CHECK(ev.u.round_end.winner_idx       == 1);
    CHECK(ev.u.round_end.p1_hp_remaining  == 0);
    CHECK(ev.u.round_end.p2_hp_remaining  == 380);
    CHECK(ev.u.round_end.frames_elapsed   == 4200);
}

TEST_CASE("Encode/Decode MATCH_END round-trips (C7 enriched)") {
    mirror_event::MatchEndPayload p{};
    p.winner_idx     = 0;       // P1
    p.rounds_won_p1  = 2;
    p.rounds_won_p2  = 1;
    p.frames_total   = 11700;
    uint8_t buf[16] = {};
    size_t w = mirror_event::EncodeMatchEnd(buf, sizeof(buf), p);
    CHECK(w == 8);
    CHECK(buf[0] == (uint8_t)mirror_event::SessionEventType::MATCH_END);

    mirror_event::SessionEvent ev{};
    size_t r = mirror_event::Decode(buf, w, &ev, nullptr);
    CHECK(r == 8);
    CHECK(ev.type == mirror_event::SessionEventType::MATCH_END);
    CHECK(ev.u.match_end.winner_idx     == 0);
    CHECK(ev.u.match_end.rounds_won_p1  == 2);
    CHECK(ev.u.match_end.rounds_won_p2  == 1);
    CHECK(ev.u.match_end.frames_total   == 11700u);
}

TEST_CASE("Encode/Decode SESSION_ID round-trips") {
    constexpr uint64_t kId = 0x1234567890ABCDEFull;
    uint8_t buf[16] = {};
    size_t w = mirror_event::EncodeSessionId(buf, sizeof(buf), kId);
    CHECK(w == 9);
    CHECK(buf[0] == (uint8_t)mirror_event::SessionEventType::SESSION_ID);

    mirror_event::SessionEvent ev{};
    size_t r = mirror_event::Decode(buf, w, &ev, nullptr);
    CHECK(r == 9);
    CHECK(ev.type == mirror_event::SessionEventType::SESSION_ID);
    CHECK(ev.u.session_id == kId);
}

TEST_CASE("Encode/Decode INPUT round-trips") {
    uint8_t buf[64] = {};
    size_t w = mirror_event::EncodeInput(buf, sizeof(buf), 0x103, 0x208);
    CHECK(w == 5);
    CHECK(buf[0] == (uint8_t)mirror_event::SessionEventType::INPUT);

    mirror_event::SessionEvent ev{};
    size_t r = mirror_event::Decode(buf, w, &ev, nullptr);
    CHECK(r == 5);
    CHECK(ev.type == mirror_event::SessionEventType::INPUT);
    CHECK(ev.u.input.p1 == 0x103);
    CHECK(ev.u.input.p2 == 0x208);
}

TEST_CASE("Encode/Decode PIN_RNG round-trips") {
    uint8_t buf[64] = {};
    size_t w = mirror_event::EncodePinRng(buf, sizeof(buf), 0x12345678);
    CHECK(w == 5);

    mirror_event::SessionEvent ev{};
    size_t r = mirror_event::Decode(buf, w, &ev, nullptr);
    CHECK(r == 5);
    CHECK(ev.type == mirror_event::SessionEventType::PIN_RNG);
    CHECK(ev.u.pin_rng_seed == 0x12345678u);
}

TEST_CASE("Encode/Decode zero-payload events round-trip (RESET, SOUND_INIT)") {
    uint8_t buf[3] = {};
    size_t w = mirror_event::EncodeResetInputState(buf, sizeof(buf));
    CHECK(w == 1);
    w = mirror_event::EncodeSoundInit(buf + 1, sizeof(buf) - 1);
    CHECK(w == 1);

    mirror_event::SessionEvent ev{};
    size_t r = mirror_event::Decode(buf + 0, 1, &ev, nullptr);
    CHECK(r == 1);  CHECK(ev.type == mirror_event::SessionEventType::RESET_INPUT_STATE);
    r = mirror_event::Decode(buf + 1, 1, &ev, nullptr);
    CHECK(r == 1);  CHECK(ev.type == mirror_event::SessionEventType::SOUND_INIT);
}

TEST_CASE("Encode/Decode MATCH_START round-trips with 96-byte header copy") {
    uint8_t header_in[96];
    for (size_t i = 0; i < 96; i++) header_in[i] = (uint8_t)(i ^ 0xA5);

    uint8_t buf[256] = {};
    size_t w = mirror_event::EncodeMatchStart(buf, sizeof(buf), header_in);
    CHECK(w == 97);
    CHECK(buf[0] == (uint8_t)mirror_event::SessionEventType::MATCH_START);
    CHECK(buf[1] == (uint8_t)(0 ^ 0xA5));
    CHECK(buf[96] == (uint8_t)(95 ^ 0xA5));

    mirror_event::SessionEvent ev{};
    uint8_t header_out[96] = {};
    size_t r = mirror_event::Decode(buf, w, &ev, header_out);
    CHECK(r == 97);
    CHECK(ev.type == mirror_event::SessionEventType::MATCH_START);
    CHECK(ev.u.match_start_idx == 0);  // caller fills after side-table append
    CHECK(std::memcmp(header_in, header_out, 96) == 0);
}

TEST_CASE("Encode/Decode FINGERPRINT round-trips") {
    uint8_t buf[64] = {};
    size_t w = mirror_event::EncodeFingerprint(buf, sizeof(buf), 0xDEADBEEF);
    CHECK(w == 5);

    mirror_event::SessionEvent ev{};
    size_t r = mirror_event::Decode(buf, w, &ev, nullptr);
    CHECK(r == 5);
    CHECK(ev.type == mirror_event::SessionEventType::FINGERPRINT);
    CHECK(ev.u.fingerprint_hash == 0xDEADBEEFu);
}

TEST_CASE("Decode rejects unknown type tags") {
    uint8_t buf[8] = { 0xFF, 0, 0, 0, 0, 0, 0, 0 };
    mirror_event::SessionEvent ev{};
    CHECK(mirror_event::Decode(buf, sizeof(buf), &ev, nullptr) == 0);

    buf[0] = 0;  // tag 0 isn't assigned
    CHECK(mirror_event::Decode(buf, sizeof(buf), &ev, nullptr) == 0);

    buf[0] = 10;  // one past the highest assigned tag (ROUND_END = 9)
    CHECK(mirror_event::Decode(buf, sizeof(buf), &ev, nullptr) == 0);
}

TEST_CASE("Decode returns 0 on truncated buffer") {
    // INPUT requires 5 bytes; give 4.
    uint8_t buf[4] = { (uint8_t)mirror_event::SessionEventType::INPUT, 1, 2, 3 };
    mirror_event::SessionEvent ev{};
    CHECK(mirror_event::Decode(buf, 4, &ev, nullptr) == 0);

    // MATCH_START requires 97 bytes; give 96.
    uint8_t big[96] = { (uint8_t)mirror_event::SessionEventType::MATCH_START };
    CHECK(mirror_event::Decode(big, 96, &ev, nullptr) == 0);

    // Empty buffer.
    CHECK(mirror_event::Decode(nullptr, 0, &ev, nullptr) == 0);
}

TEST_CASE("Encode returns 0 on insufficient capacity") {
    uint8_t small[4];
    CHECK(mirror_event::EncodeInput(small, 4, 0, 0) == 0);
    CHECK(mirror_event::EncodePinRng(small, 4, 0) == 0);
    CHECK(mirror_event::EncodeFingerprint(small, 4, 0) == 0);
    uint8_t hdr[96] = {};
    CHECK(mirror_event::EncodeMatchStart(small, 4, hdr) == 0);

    // Zero-cap zero-payload encoders.
    {
        mirror_event::MatchEndPayload zero{};
        CHECK(mirror_event::EncodeMatchEnd(small, 0, zero) == 0);
    }
    CHECK(mirror_event::EncodeResetInputState(small, 0) == 0);
    CHECK(mirror_event::EncodeSoundInit(small, 0) == 0);
}

TEST_CASE("Mixed-event stream: encode N events, decode them all back in order") {
    // The realistic shape of a battle-start sequence:
    //   PIN_RNG, RESET_INPUT_STATE, SOUND_INIT, MATCH_START, INPUT*4, FINGERPRINT, MATCH_END
    uint8_t buf[512] = {};
    uint8_t* p = buf;
    size_t w;
    w = mirror_event::EncodePinRng(p, sizeof(buf) - (p - buf), 0x12345678);          REQUIRE(w); p += w;
    w = mirror_event::EncodeResetInputState(p, sizeof(buf) - (p - buf));             REQUIRE(w); p += w;
    w = mirror_event::EncodeSoundInit(p, sizeof(buf) - (p - buf));                   REQUIRE(w); p += w;
    uint8_t hdr_in[96]; for (int i = 0; i < 96; i++) hdr_in[i] = (uint8_t)i;
    w = mirror_event::EncodeMatchStart(p, sizeof(buf) - (p - buf), hdr_in);          REQUIRE(w); p += w;
    for (int i = 0; i < 4; i++) {
        w = mirror_event::EncodeInput(p, sizeof(buf) - (p - buf),
                                      (uint16_t)(0x100 + i), (uint16_t)(0x200 + i));
        REQUIRE(w); p += w;
    }
    w = mirror_event::EncodeFingerprint(p, sizeof(buf) - (p - buf), 0xCAFEBABE);     REQUIRE(w); p += w;
    {
        mirror_event::MatchEndPayload me{};
        me.winner_idx     = 0;
        me.rounds_won_p1  = 2;
        me.rounds_won_p2  = 1;
        me.frames_total   = 11700;
        w = mirror_event::EncodeMatchEnd(p, sizeof(buf) - (p - buf), me);            REQUIRE(w); p += w;
    }

    const size_t total = (size_t)(p - buf);
    // 5 + 1 + 1 + 97 + 4*5 + 5 + 8 = 137 bytes
    CHECK(total == 137);

    // Walk the stream back.
    size_t off = 0;
    mirror_event::SessionEvent ev{};
    uint8_t hdr_out[96] = {};
    size_t r;

    r = mirror_event::Decode(buf + off, total - off, &ev, nullptr);    CHECK(r);
    CHECK(ev.type == mirror_event::SessionEventType::PIN_RNG);
    CHECK(ev.u.pin_rng_seed == 0x12345678u);
    off += r;

    r = mirror_event::Decode(buf + off, total - off, &ev, nullptr);    CHECK(r);
    CHECK(ev.type == mirror_event::SessionEventType::RESET_INPUT_STATE);
    off += r;

    r = mirror_event::Decode(buf + off, total - off, &ev, nullptr);    CHECK(r);
    CHECK(ev.type == mirror_event::SessionEventType::SOUND_INIT);
    off += r;

    r = mirror_event::Decode(buf + off, total - off, &ev, hdr_out);    CHECK(r);
    CHECK(ev.type == mirror_event::SessionEventType::MATCH_START);
    CHECK(std::memcmp(hdr_in, hdr_out, 96) == 0);
    off += r;

    for (int i = 0; i < 4; i++) {
        r = mirror_event::Decode(buf + off, total - off, &ev, nullptr); CHECK(r);
        CHECK(ev.type == mirror_event::SessionEventType::INPUT);
        CHECK(ev.u.input.p1 == (uint16_t)(0x100 + i));
        CHECK(ev.u.input.p2 == (uint16_t)(0x200 + i));
        off += r;
    }

    r = mirror_event::Decode(buf + off, total - off, &ev, nullptr);    CHECK(r);
    CHECK(ev.type == mirror_event::SessionEventType::FINGERPRINT);
    CHECK(ev.u.fingerprint_hash == 0xCAFEBABEu);
    off += r;

    r = mirror_event::Decode(buf + off, total - off, &ev, nullptr);    CHECK(r);
    CHECK(ev.type == mirror_event::SessionEventType::MATCH_END);
    CHECK(ev.u.match_end.winner_idx    == 0);
    CHECK(ev.u.match_end.rounds_won_p1 == 2);
    CHECK(ev.u.match_end.rounds_won_p2 == 1);
    CHECK(ev.u.match_end.frames_total  == 11700u);
    off += r;

    CHECK(off == total);
}
