// Spectator wire codecs: SessionEvent encode/decode + the zero-RLE snapshot
// compressor. Pure serialization, no g_state. Extracted VERBATIM from
// spectator_node.cpp (pure move, no behavior change).
#include "spec_wire.h"
#include "spectator_node.h"   // SessionEventType, payload structs, SessionEvent_* decls
#include <cstring>
#include <cstdint>
#include <vector>

// ---- zero-RLE codec for snapshot transfer ---------------------------------
// Record stream: [u16 literal_len][u16 zero_len][literal bytes] repeated.
// Decompress target is pre-zeroed, so zero runs are just skips. The
// savestate blob is dominated by zeroed pool slots -- typical ratio ~10x.
void ZeroRleCompress(const std::vector<uint8_t>& in,
                            std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(in.size() / 4);
    size_t i = 0;
    const size_t n = in.size();
    while (i < n) {
        // literal run: until we hit a zero run worth encoding (>= 8) or cap
        size_t lit_start = i;
        size_t zeros_at  = n;
        while (i < n && i - lit_start < 0xFFFF) {
            if (in[i] == 0) {
                size_t z = i;
                while (z < n && in[z] == 0 && z - i < 0xFFFF) ++z;
                if (z - i >= 8) { zeros_at = i; break; }
                i = z;  // short zero run -- keep as literal
                continue;
            }
            ++i;
        }
        const size_t lit_len = (zeros_at == n ? i : zeros_at) - lit_start;
        size_t zero_len = 0;
        if (zeros_at != n) {
            size_t z = zeros_at;
            while (z < n && in[z] == 0 && zero_len < 0xFFFF) { ++z; ++zero_len; }
            i = z;
        }
        const uint16_t l16 = (uint16_t)lit_len;
        const uint16_t z16 = (uint16_t)zero_len;
        out.push_back((uint8_t)(l16 & 0xFF)); out.push_back((uint8_t)(l16 >> 8));
        out.push_back((uint8_t)(z16 & 0xFF)); out.push_back((uint8_t)(z16 >> 8));
        out.insert(out.end(), in.begin() + lit_start, in.begin() + lit_start + lit_len);
    }
}

bool ZeroRleDecompress(const uint8_t* in, size_t in_len,
                              uint8_t* out, size_t out_len) {
    std::memset(out, 0, out_len);
    size_t ri = 0, wo = 0;
    while (ri + 4 <= in_len) {
        const uint16_t lit  = (uint16_t)(in[ri] | (in[ri + 1] << 8));
        const uint16_t zero = (uint16_t)(in[ri + 2] | (in[ri + 3] << 8));
        ri += 4;
        if (ri + lit > in_len || wo + lit + zero > out_len) return false;
        std::memcpy(out + wo, in + ri, lit);
        ri += lit;
        wo += lit + zero;
    }
    return ri == in_len && wo == out_len;
}


namespace {

// Wire payload size (excluding the 1-byte type tag) per SessionEventType.
// Used by both encoders (sanity) and the decoder (bounds check before memcpy).
size_t WirePayloadSize(SessionEventType t) {
    switch (t) {
        case SessionEventType::INPUT:             return 4;   // u16 + u16
        case SessionEventType::PIN_RNG:           return 4;   // u32
        case SessionEventType::RESET_INPUT_STATE: return 0;
        case SessionEventType::SOUND_INIT:        return 0;
        case SessionEventType::MATCH_START:       return SESSION_EVENT_MATCH_HDR_SIZE;
        case SessionEventType::MATCH_END:         return sizeof(MatchEndPayload);    // 7  (was 0 in v1)
        case SessionEventType::FINGERPRINT:       return 4;   // u32
        case SessionEventType::ROUND_START:       return sizeof(RoundStartPayload);  // 7
        case SessionEventType::ROUND_END:         return sizeof(RoundEndPayload);    // 9
        case SessionEventType::SESSION_ID:        return 8;   // u64
        case SessionEventType::CSS_ENTERED:       return 6;   // cursor xy x2 + sel x2
    }
    return SIZE_MAX;  // unknown tag — caller treats as malformed
}

bool IsValidEventTag(uint8_t tag) {
    return tag >= static_cast<uint8_t>(SessionEventType::INPUT)
        && tag <= static_cast<uint8_t>(SessionEventType::CSS_ENTERED);
}

} // namespace

size_t SessionEvent_EncodeInput(uint8_t* out, size_t cap, uint16_t p1, uint16_t p2) {
    constexpr size_t need = 1 + 4;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::INPUT);
    std::memcpy(out + 1, &p1, 2);
    std::memcpy(out + 3, &p2, 2);
    return need;
}

size_t SessionEvent_EncodePinRng(uint8_t* out, size_t cap, uint32_t seed) {
    constexpr size_t need = 1 + 4;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::PIN_RNG);
    std::memcpy(out + 1, &seed, 4);
    return need;
}

size_t SessionEvent_EncodeResetInputState(uint8_t* out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::RESET_INPUT_STATE);
    return 1;
}

size_t SessionEvent_EncodeSoundInit(uint8_t* out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::SOUND_INIT);
    return 1;
}

size_t SessionEvent_EncodeCssEntered(uint8_t* out, size_t cap, const SessionEvent& ev) {
    constexpr size_t need = 1 + 6;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::CSS_ENTERED);
    out[1] = static_cast<uint8_t>(ev.u.css_entered.p1_cur_x);
    out[2] = static_cast<uint8_t>(ev.u.css_entered.p1_cur_y);
    out[3] = static_cast<uint8_t>(ev.u.css_entered.p2_cur_x);
    out[4] = static_cast<uint8_t>(ev.u.css_entered.p2_cur_y);
    out[5] = static_cast<uint8_t>(ev.u.css_entered.p1_sel);
    out[6] = static_cast<uint8_t>(ev.u.css_entered.p2_sel);
    return need;
}

size_t SessionEvent_EncodeMatchStart(uint8_t* out, size_t cap,
                                     const uint8_t header[SESSION_EVENT_MATCH_HDR_SIZE]) {
    constexpr size_t need = 1 + SESSION_EVENT_MATCH_HDR_SIZE;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::MATCH_START);
    std::memcpy(out + 1, header, SESSION_EVENT_MATCH_HDR_SIZE);
    return need;
}

size_t SessionEvent_EncodeMatchEnd(uint8_t* out, size_t cap, const MatchEndPayload& p) {
    constexpr size_t need = 1 + sizeof(MatchEndPayload);
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::MATCH_END);
    std::memcpy(out + 1, &p, sizeof(p));
    return need;
}

size_t SessionEvent_EncodeSessionId(uint8_t* out, size_t cap, uint64_t session_id) {
    constexpr size_t need = 1 + 8;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::SESSION_ID);
    std::memcpy(out + 1, &session_id, 8);
    return need;
}

size_t SessionEvent_EncodeRoundStart(uint8_t* out, size_t cap, const RoundStartPayload& p) {
    constexpr size_t need = 1 + sizeof(RoundStartPayload);
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::ROUND_START);
    std::memcpy(out + 1, &p, sizeof(p));
    return need;
}

size_t SessionEvent_EncodeRoundEnd(uint8_t* out, size_t cap, const RoundEndPayload& p) {
    constexpr size_t need = 1 + sizeof(RoundEndPayload);
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::ROUND_END);
    std::memcpy(out + 1, &p, sizeof(p));
    return need;
}

size_t SessionEvent_EncodeFingerprint(uint8_t* out, size_t cap, uint32_t hash) {
    constexpr size_t need = 1 + 4;
    if (cap < need) return 0;
    out[0] = static_cast<uint8_t>(SessionEventType::FINGERPRINT);
    std::memcpy(out + 1, &hash, 4);
    return need;
}

size_t SessionEvent_Decode(const uint8_t* in, size_t in_len,
                           SessionEvent* out_event,
                           uint8_t out_match_header[SESSION_EVENT_MATCH_HDR_SIZE]) {
    if (in_len < 1) return 0;
    const uint8_t tag = in[0];
    if (!IsValidEventTag(tag)) return 0;

    const SessionEventType type = static_cast<SessionEventType>(tag);
    const size_t payload = WirePayloadSize(type);
    if (in_len < 1 + payload) return 0;

    if (out_event) {
        out_event->type = type;
        // Zero the union so unused-variant readers see deterministic 0s.
        std::memset(&out_event->u, 0, sizeof(out_event->u));

        switch (type) {
            case SessionEventType::INPUT:
                std::memcpy(&out_event->u.input.p1, in + 1, 2);
                std::memcpy(&out_event->u.input.p2, in + 3, 2);
                break;
            case SessionEventType::PIN_RNG:
                std::memcpy(&out_event->u.pin_rng_seed, in + 1, 4);
                break;
            case SessionEventType::FINGERPRINT:
                std::memcpy(&out_event->u.fingerprint_hash, in + 1, 4);
                break;
            case SessionEventType::MATCH_START:
                // u.match_start_idx left at 0 — caller assigns when appending
                // to its match_headers side table.
                break;
            case SessionEventType::ROUND_START:
                std::memcpy(&out_event->u.round_start, in + 1, sizeof(RoundStartPayload));
                break;
            case SessionEventType::ROUND_END:
                std::memcpy(&out_event->u.round_end, in + 1, sizeof(RoundEndPayload));
                break;
            case SessionEventType::MATCH_END:
                std::memcpy(&out_event->u.match_end, in + 1, sizeof(MatchEndPayload));
                break;
            case SessionEventType::SESSION_ID:
                std::memcpy(&out_event->u.session_id, in + 1, 8);
                break;
            case SessionEventType::RESET_INPUT_STATE:
            case SessionEventType::SOUND_INIT:
                break;
            case SessionEventType::CSS_ENTERED:
                out_event->u.css_entered.p1_cur_x = (int8_t)in[1];
                out_event->u.css_entered.p1_cur_y = (int8_t)in[2];
                out_event->u.css_entered.p2_cur_x = (int8_t)in[3];
                out_event->u.css_entered.p2_cur_y = (int8_t)in[4];
                out_event->u.css_entered.p1_sel   = (int8_t)in[5];
                out_event->u.css_entered.p2_sel   = (int8_t)in[6];
                break;
        }
    }
    if (type == SessionEventType::MATCH_START && out_match_header) {
        std::memcpy(out_match_header, in + 1, SESSION_EVENT_MATCH_HDR_SIZE);
    }

    return 1 + payload;
}
