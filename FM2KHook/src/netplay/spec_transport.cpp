// Spectator transport: RC-over-UDP / hub-relay send helpers + EVENT_BATCH
// flush. Two transports, and only two: the reliable channel over UDP
// (reliable-ordered + Reed-Solomon FEC, the default for every direct join) and
// the hub relay (shared-memory rings the launcher drains onto the hub
// WebSocket, for peers direct UDP cannot reach). The legacy TCP data plane and
// the non-CRC-protected 0xCE UDP input accelerator are DELETED -- see the
// transport section of docs/dev/architecture_spectate.md.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
#include "reliable_channel_net.h" // the spectator data plane: reliable-ordered + FEC over UDP
#include "control_channel.h"
#include "netplay.h"
#include "replay.h"
#include "savestate.h"            // SaveState_Save / Peek for snapshot capture
#include "netplay_state.h"
#include "../audio/sound_rollback.h"  // Op apply: SOUND_INIT
#include "../hooks/css_autoconfirm.h" // Replay-mode CSS lock-and-confirm
#include "../hooks/per_game_patches.h" // PerGamePatches_SetRuntimeBtbOverrides
#include "../ui/shared_mem.h"         // C10: SharedMem_PublishMatchSession / RoundResult
#include "gekkonet.h"

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <array>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
using namespace specnode;
namespace specnode {

// Classic Fletcher-32 over a byte buffer. Used both by the C9 desync
// fingerprint and the task-#18 snapshot-blob checksum. Self-contained so
// the test-side mirror can verify by replicating the algorithm without
// pulling in any production headers.
uint32_t Fletcher32(const uint8_t* data, size_t len) {
    uint32_t sum1 = 0xFFFF, sum2 = 0xFFFF;
    size_t i = 0;
    while (i + 1 < len) {
        uint16_t w = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
        sum1 = (sum1 + w)    % 0xFFFFu;
        sum2 = (sum2 + sum1) % 0xFFFFu;
        i += 2;
    }
    if (i < len) {
        uint16_t w = data[i];
        sum1 = (sum1 + w)    % 0xFFFFu;
        sum2 = (sum2 + sum1) % 0xFFFFu;
    }
    return (sum2 << 16) | sum1;
}

// =============================================================================
// UDP HELPERS
// =============================================================================

bool AddrEqual(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_family == b.sin_family
        && a.sin_port   == b.sin_port
        && a.sin_addr.s_addr == b.sin_addr.s_addr;
}

// FormatAddr is defined below; forward-decl so the Phase 2c send
// helpers can use it for their warning logs without reordering the
// whole helper block.
void FormatAddr(const sockaddr_in& a, char* out, size_t out_sz);

// Spec data send helpers. Exactly two transports:
//
//   * RELIABLE CHANNEL over UDP -- the default and the only direct one.
//     Reliable-ordered + Reed-Solomon FEC on the shared UDP socket, three
//     logical channels (live events / bulk snapshot / unordered blob). It
//     replaced a TCP data plane that head-of-line-blocked under loss and
//     black-screened outright when a NAT swallowed the inbound dial; both
//     gaps closed 2026-07-18 with loss soaks (TCP-free join 3/3 pass, was
//     3/3 fail; snapshot completion under 6% loss 6/6, was 4/4 fail).
//
//   * HUB RELAY -- selected by FM2K_SPEC_TRANSPORT=relay. Exists precisely
//     because direct UDP does not reach that peer, so it and RC are mutually
//     exclusive: RC would hijack the send path.
//
// `buf` carries the full wire payload: SpecDataHeader (10 B) + inner payload.
// RC sends it as-is; relay mode makes it the SPDB envelope's `payload` (the
// launcher drain wraps it with magic + routing fields before WS send).
//
// Failure modes:
//   - relay mode with no spec_relay_out (mapping creation failed):
//     silently drop. Logged once at Init.
//   - relay mode SendTo with no spec_user_id on the matching sub:
//     drop with warn log. Means hub didn't forward user_id (older
//     hub) or this sub came in via a pre-Phase-2c spec_incoming.
//   - ring full: counted in Ring.total_dropped, no inline log.

static bool RcSpectatorBroadcast(const void* buf, size_t len) {
    if (g_state.spec_transport_relay) return false;  // relay = no direct UDP path
    // Same backfill fence as the relay path: only bound subs get live events.
    for (const auto& sub : g_state.subscribers) {
        if (!sub.bound) continue;
        sockaddr_storage ss{};
        std::memcpy(&ss, &sub.addr, sizeof(sub.addr));
        ReliableChannel_SendTo(ss, RC_CHAN_SPEC, RC_CLASS_RELIABLE_ORDERED,
                               reinterpret_cast<const uint8_t*>(buf), static_cast<int>(len));
    }
    return true;
}

void OutboundBroadcast(const void* buf, size_t len) {
    if (RcSpectatorBroadcast(buf, len)) return;
    if (!g_state.spec_relay_out) return;
    // Per-bound-sub direct send rather than hub-side broadcast: the hub does
    // not know about the bind fence, so a TARGET_BROADCAST would race pre-bind
    // specs into receiving EVENT_BATCH frames AHEAD of their snapshot -- bad
    // ordering, spec applies events on uninitialized state. Pre-bind specs (no
    // bind or no spec_user_id yet) get skipped here; they catch up via
    // snapshot + backfill in TickHostMaintenance.
    //
    // Cost: O(N) Enqueue calls instead of one. Hub does the same O(N) sends
    // either way, so net throughput is identical; ring slot consumption goes
    // up by N -- factored into the 128-slot capacity sizing.
    for (const auto& sub : g_state.subscribers) {
        if (!sub.bound) continue;
        if (sub.spec_user_id.empty()) continue;
        fm2k::spec_relay::Enqueue(
            g_state.spec_relay_out,
            fm2k::spec_relay::TARGET_DIRECT,
            sub.spec_user_id.c_str(),
            /*spec_data_type=*/0,
            /*frame_count=*/0,
            /*spec_data_flags=*/0,
            buf, static_cast<uint32_t>(len));
    }
}

void OutboundSendTo(const sockaddr_in& to, const void* buf, size_t len) {
    // Snapshot + backfill (this path) is BULK: a ~1MB snapshot ships as ~66
    // chunks + 352 backfill events in one burst. RC's send pacing (token
    // bucket + cwnd) bounds the burst so it cannot self-flood, and CRC +
    // RC-level acks keep it correct under loss.
    if (!g_state.spec_transport_relay) {
        sockaddr_storage ss{};
        std::memcpy(&ss, &to, sizeof(to));
        // SNAPSHOT_BEGIN/CHUNK/END are content-addressed (reassembled by byte
        // offset), so they ride an UNORDERED channel -- a lost chunk delays only
        // itself, not the other 65 (no head-of-line block on the 1MB transfer).
        // Backfill EVENT_BATCH2 stays on the ORDERED bulk channel.
        uint8_t rc_chan = RC_CHAN_SPEC_SNAPSHOT;
        int     rc_cls  = RC_CLASS_RELIABLE_ORDERED;
        if (len >= static_cast<int>(sizeof(SpecDataHeader))) {
            SpecDataType t = reinterpret_cast<const SpecDataHeader*>(buf)->type;
            if (t == SpecDataType::SNAPSHOT_BEGIN || t == SpecDataType::SNAPSHOT_CHUNK ||
                t == SpecDataType::SNAPSHOT_END) {
                rc_chan = RC_CHAN_SPEC_BLOB;
                rc_cls  = RC_CLASS_RELIABLE_UNORDERED;
            }
        }
        ReliableChannel_SendTo(ss, rc_chan, rc_cls,
                               reinterpret_cast<const uint8_t*>(buf), static_cast<int>(len));
        return;
    }
    if (!g_state.spec_relay_out) return;
    // Look up spec_user_id from the addr. Subscriber list is short
    // (capacity-bounded by SPECTATOR_DEFAULT_CAPACITY = 32), so a
    // linear scan is fine.
    const char* spec_uid = nullptr;
    for (const auto& sub : g_state.subscribers) {
        if (sub.addr.sin_family == to.sin_family &&
            sub.addr.sin_port   == to.sin_port &&
            sub.addr.sin_addr.s_addr == to.sin_addr.s_addr) {
            if (!sub.spec_user_id.empty()) spec_uid = sub.spec_user_id.c_str();
            break;
        }
    }
    if (!spec_uid) {
        // No user_id available; can't route through relay. Log
        // rarely so a misconfigured pair doesn't spam.
        static uint64_t s_warn_count = 0;
        if ((s_warn_count++ % 64) == 0) {
            char addr_buf[48] = {};
            FormatAddr(to, addr_buf, sizeof(addr_buf));
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: relay SendTo %s -- no spec_user_id "
                "on Subscriber, dropping (warns:%llu)",
                addr_buf, (unsigned long long)s_warn_count);
        }
        return;
    }
    fm2k::spec_relay::Enqueue(
        g_state.spec_relay_out,
        fm2k::spec_relay::TARGET_DIRECT,
        spec_uid,
        /*spec_data_type=*/0,
        /*frame_count=*/0,
        /*spec_data_flags=*/0,
        buf, static_cast<uint32_t>(len));
}

void FormatAddr(const sockaddr_in& a, char* out, size_t out_sz) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    // PII (FlippySpatula 2026-07-19): the sink-level scrub keeps the first
    // two octets of public IPs ("108.197.*.*") -- still identifying, and
    // spectator lines don't need ANY of it. Fully mask public peers at
    // the source in production; loopback/RFC1918/link-local stay intact
    // (the harness + same-LAN diagnostics depend on them). Port is kept:
    // it's the correlation key for subs and carries no identity.
    static const bool s_prod = []{
        const char* v = std::getenv("FM2K_PRODUCTION_MODE");
        return !(v && v[0] == '0' && v[1] == '\0');
    }();
    const uint32_t be = a.sin_addr.s_addr;          // network order
    const uint8_t o1 = (uint8_t)(be), o2 = (uint8_t)(be >> 8);
    const bool priv = o1 == 127 || o1 == 10 ||
                      (o1 == 192 && o2 == 168) ||
                      (o1 == 172 && o2 >= 16 && o2 <= 31) ||
                      (o1 == 169 && o2 == 254);
    if (s_prod && !priv) {
        std::snprintf(out, out_sz, "<pub>:%u", ntohs(a.sin_port));
        return;
    }
    std::snprintf(out, out_sz, "%s:%u", ip, ntohs(a.sin_port));
}

// =============================================================================
// BROADCAST
// =============================================================================

// Append one event's wire encoding into a byte vector. Used by both the
// live broadcast path (FlushBatch) and the backfill path
// (SendSessionBackfillFromStart) -- keeps wire-encoding logic in one place.
void AppendEventToWire(std::vector<uint8_t>& out, const SessionEvent& ev,
                       const std::vector<MatchHeader>& headers) {
    uint8_t buf[SESSION_EVENT_MAX_WIRE_SIZE];
    size_t w = 0;
    switch (ev.type) {
        case SessionEventType::INPUT:
            w = SessionEvent_EncodeInput(buf, sizeof(buf),
                                         ev.u.input.p1, ev.u.input.p2);
            break;
        case SessionEventType::PIN_RNG:
            w = SessionEvent_EncodePinRng(buf, sizeof(buf), ev.u.pin_rng_seed);
            break;
        case SessionEventType::RESET_INPUT_STATE:
            w = SessionEvent_EncodeResetInputState(buf, sizeof(buf));
            break;
        case SessionEventType::SOUND_INIT:
            w = SessionEvent_EncodeSoundInit(buf, sizeof(buf));
            break;
        case SessionEventType::MATCH_START:
            if (ev.u.match_start_idx < headers.size()) {
                w = SessionEvent_EncodeMatchStart(buf, sizeof(buf),
                                                  headers[ev.u.match_start_idx].data());
            }
            break;
        case SessionEventType::MATCH_END:
            w = SessionEvent_EncodeMatchEnd(buf, sizeof(buf), ev.u.match_end);
            break;
        case SessionEventType::FINGERPRINT:
            w = SessionEvent_EncodeFingerprint(buf, sizeof(buf), ev.u.fingerprint_hash);
            break;
        case SessionEventType::ROUND_START:
            w = SessionEvent_EncodeRoundStart(buf, sizeof(buf), ev.u.round_start);
            break;
        case SessionEventType::ROUND_END:
            w = SessionEvent_EncodeRoundEnd(buf, sizeof(buf), ev.u.round_end);
            break;
        case SessionEventType::SESSION_ID:
            w = SessionEvent_EncodeSessionId(buf, sizeof(buf), ev.u.session_id);
            break;
        case SessionEventType::CSS_ENTERED:
            w = SessionEvent_EncodeCssEntered(buf, sizeof(buf), ev);
            break;
    }
    if (w > 0) out.insert(out.end(), buf, buf + w);
}

// Count INPUT events in [first, last) of a SessionEvent slice. Used to
// populate SpecDataHeader.frame_count for log/diagnostic purposes -- wire
// dedup uses start_frame.
uint32_t CountInputs(const std::vector<SessionEvent>& events,
                     size_t first, size_t last) {
    uint32_t n = 0;
    for (size_t i = first; i < last; i++) {
        if (events[i].type == SessionEventType::INPUT) ++n;
    }
    return n;
}

// Emit an EVENT_BATCH datagram covering all events appended since the last
// flush. The reliable channel guarantees in-order, exactly-once delivery on
// RC_CHAN_SPEC -- no redundancy window. The header carries:
//   start_frame = session-relative INPUT-frame index of the first INPUT in
//                 this batch (= flushed_input_count at entry). Used by the
//                 receiver's next_expected_frame dedup gate.
//   frame_count = number of INPUT events in this batch (informational).
//   payload     = packed SessionEvent[] (1-byte type tag + variant payload
//                 per event; see SessionEvent_Encode* in this file).
void FlushBatch() {
    const size_t first = g_state.last_flushed_event_idx;
    const size_t last  = g_state.session_events.size();
    if (first == last) return;

    const uint32_t input_count = CountInputs(g_state.session_events, first, last);
    const uint32_t start_frame = g_state.flushed_input_count;

    // Advance flush watermark unconditionally so the cadence trigger
    // ("every BROADCAST_BATCH_FRAMES INPUT events") keeps walking even
    // when there are no subscribers to receive the batch.
    g_state.last_flushed_event_idx = last;
    g_state.flushed_input_count   += input_count;

    if (g_state.subscribers.empty()) return;

    std::vector<uint8_t> payload;
    payload.reserve((last - first) * SESSION_EVENT_MAX_WIRE_SIZE);
    for (size_t i = first; i < last; i++) {
        AppendEventToWire(payload, g_state.session_events[i], g_state.match_headers);
    }
    if (payload.empty()) return;

    // EVENT_BATCH2: frame_count carries the absolute op base = index of
    // this batch's first non-INPUT event over ALL non-INPUT session events.
    // Every op append goes through AppendOpAndFlush (bumps total_op_count)
    // AND lands in session_events, so the ops in [first,last) are exactly
    // the most recent op_count appends.
    uint32_t op_count = 0;
    for (size_t i = first; i < last; i++) {
        if (g_state.session_events[i].type != SessionEventType::INPUT) ++op_count;
    }
    const uint32_t op_base = g_state.total_op_count - op_count;
    if (op_base > 0xFFFFu) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: op_base %u exceeds u16 -- EVENT_BATCH2 identity "
            "saturated (session with >65535 boundary ops?)", op_base);
    }

    std::vector<uint8_t> buf(sizeof(SpecDataHeader) + payload.size());
    SpecDataHeader hdr = {};
    hdr.magic       = SPEC_DATA_MAGIC;
    hdr.type        = SpecDataType::EVENT_BATCH2;
    hdr.start_frame = start_frame;
    hdr.frame_count = static_cast<uint16_t>(std::min<uint32_t>(op_base, 0xFFFFu));
    // For EVENT_BATCH, flags carries the payload byte count so a receiver can
    // size the payload without re-decoding events. 16-bit cap
    // (65535) is well above any reasonable live FlushBatch -- backfill
    // chunks are explicitly bounded at BACKFILL_CHUNK_BYTES=1024.
    hdr.flags       = static_cast<uint16_t>(std::min<size_t>(payload.size(), 0xFFFFu));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), payload.data(), payload.size());

    OutboundBroadcast(buf.data(), buf.size());
}


// task #55: pre-subscribe stash -- see State::pre_sub_stash. Bounded so a
// stale host blasting at a non-subscribing node can't grow memory: 2MB
// covers a full snapshot + backfill burst with margin.
void SpecStashPreSubscribe(uint8_t chan, const uint8_t* data, int len) {
    constexpr size_t kMaxMsgs  = 1024;
    constexpr size_t kMaxBytes = 2u * 1024u * 1024u;
    if (len <= 0) return;
    if (g_state.pre_sub_stash.size() >= kMaxMsgs ||
        g_state.pre_sub_stash_bytes + static_cast<size_t>(len) > kMaxBytes) {
        static uint64_t s_dropped = 0;
        if ((s_dropped++ % 64) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: pre-subscribe stash full -- dropping spec msg "
                "(%d B, drops=%llu)", len, (unsigned long long)s_dropped);
        }
        return;
    }
    g_state.pre_sub_stash.emplace_back(
        chan, std::vector<uint8_t>(data, data + len));
    g_state.pre_sub_stash_bytes += static_cast<size_t>(len);
}

void SpecReplayPreSubStash() {
    if (g_state.pre_sub_stash.empty()) return;
    auto stash = std::move(g_state.pre_sub_stash);
    g_state.pre_sub_stash.clear();
    g_state.pre_sub_stash_bytes = 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: replaying %zu pre-subscribe spec msg(s) that beat "
        "the JOIN_ACK", stash.size());
    for (auto& m : stash) {
        SpectatorNode_HandleSpecData(m.second.data(), m.second.size(),
                                     g_state.upstream_addr, m.first);
    }
}

}  // namespace specnode

// --- Global-scope (not specnode): ReliableChannel consumer glue -------------
// RC delivery -> the spec-data handler. Ordered channel
// guarantees ops-before-inputs and contiguous frames, which HandleSpecData
// already assumes -- so the consumer contract is preserved by construction.
static void RcSpectatorDeliver(void* /*ctx*/, const sockaddr_storage& from,
                               uint8_t channel, const uint8_t* data, int len) {
    // Live (ch1, ordered), backfill/op (ch2, ordered), and snapshot blob (ch3,
    // unordered offset-reassembled) all carry SpecData framing; the consumer
    // demuxes by SpecDataType and reorders EVENT_BATCH by frame across channels.
    if ((channel != RC_CHAN_SPEC && channel != RC_CHAN_SPEC_SNAPSHOT &&
         channel != RC_CHAN_SPEC_BLOB) || len <= 0) return;
    // task #55 race: the host streams the backfill the instant it accepts
    // our JOIN_REQ, so these deliveries can arrive BEFORE our JOIN_ACK
    // processing flips subscribed_upstream. The handlers would silently
    // drop them -- and RC has already acked them as delivered, so the
    // one-shot backfill would be gone forever (viewer never admits a
    // frame). Stash and replay on subscribe instead.
    if (!g_state.subscribed_upstream) {
        specnode::SpecStashPreSubscribe(channel, data, len);
        return;
    }
    sockaddr_in from4{};
    std::memcpy(&from4, &from, sizeof(from4));
    SpectatorNode_HandleSpecData(data, static_cast<size_t>(len), from4, channel);
}

void SpectatorNode_RegisterRcDeliver() {
    ReliableChannel_SetDeliver(&RcSpectatorDeliver, nullptr);
}
