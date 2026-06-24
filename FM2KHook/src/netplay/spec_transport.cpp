// Spectator transport: UDP/TCP/relay send helpers + EVENT_BATCH flush + the
// UDP input accelerator. Extracted VERBATIM from spectator_node.cpp into
// namespace specnode (see spectator_node_internal.h). No behavior change.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
#include "spectator_tcp.h"        // TCP transport for INPUT_BATCH stream
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

// Spec data send helpers (Phase 2c). Branch on the active spec
// transport: TCP (legacy P2P) or relay (hub-mediated WS binary).
//
// `buf` carries the full TCP-wire payload: SpecDataHeader (10 B) +
// inner payload. In TCP mode it's blasted to the socket(s) as-is. In
// relay mode it becomes the SPDB envelope's `payload` (the launcher
// drain wraps it with magic + routing fields before WS send).
//
// Failure modes:
//   - relay mode with no spec_relay_out (mapping creation failed):
//     silently drop. Logged once at Init.
//   - relay mode SendTo with no spec_user_id on the matching sub:
//     drop with warn log. Means hub didn't forward user_id (older
//     hub) or this sub came in via a pre-Phase-2c spec_incoming.
//   - ring full: counted in Ring.total_dropped, no inline log.
void OutboundBroadcast(const void* buf, size_t len) {
    if (g_state.spec_transport_relay) {
        if (!g_state.spec_relay_out) return;
        // Per-bound-sub direct send rather than hub-side broadcast.
        // Why: the TCP path's BroadcastToAll has a backfill_done fence
        // (g_subs[i].backfill_done gate at spectator_tcp.cpp:285) that
        // suppresses live events to a sub until snapshot+backfill have
        // landed. The hub doesn't know about backfill_done state, so a
        // TARGET_BROADCAST would race pre-bind specs into receiving
        // EVENT_BATCH frames AHEAD of their snapshot -- bad ordering,
        // spec applies events on uninitialized state.
        //
        // Iterating bound subs and Enqueuing TARGET_DIRECT per spec
        // exactly mirrors the TCP path's gate. Pre-bind specs (no
        // tcp_bound or no spec_user_id yet) get skipped here; they'll
        // catch up via snapshot + backfill in TickHostMaintenance.
        //
        // Cost: O(N) Enqueue calls instead of one. Hub does the same
        // O(N) sends either way, so net throughput is identical;
        // ring slot consumption goes up by N -- factored into the
        // 128-slot capacity sizing.
        for (const auto& sub : g_state.subscribers) {
            if (!sub.tcp_bound) continue;
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
        return;
    }
    SpectatorTCP::BroadcastToAll(buf, len);
}

void OutboundSendTo(const sockaddr_in& to, const void* buf, size_t len) {
    if (g_state.spec_transport_relay) {
        if (!g_state.spec_relay_out) return;
        // Look up spec_user_id from the addr. Subscriber list is short
        // (capacity-bounded; SPECTATOR_DEFAULT_CAPACITY = 4 today), so
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
        return;
    }
    SpectatorTCP::SendTo(to, buf, len);
}

void FormatAddr(const sockaddr_in& a, char* out, size_t out_sz) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    std::snprintf(out, out_sz, "%s:%u", ip, ntohs(a.sin_port));
}

// =============================================================================
// BROADCAST
// =============================================================================

// Append one event's wire encoding into a byte vector. Used by both the
// live broadcast path (FlushBatch) and the backfill path
// (SendSessionBackfillTo) — keeps wire-encoding logic in one place.
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
// populate SpecDataHeader.frame_count for log/diagnostic purposes — wire
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
// flush. TCP guarantees in-order, exactly-once delivery — no redundancy
// window. The header carries:
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

    std::vector<uint8_t> buf(sizeof(SpecDataHeader) + payload.size());
    SpecDataHeader hdr = {};
    hdr.magic       = SPEC_DATA_MAGIC;
    hdr.type        = SpecDataType::EVENT_BATCH;
    hdr.start_frame = start_frame;
    hdr.frame_count = static_cast<uint16_t>(std::min<uint32_t>(input_count, 0xFFFFu));
    // For EVENT_BATCH, flags carries the payload byte count so the TCP
    // framer can size the receive without re-decoding events. 16-bit cap
    // (65535) is well above any reasonable live FlushBatch — backfill
    // chunks are explicitly bounded at BACKFILL_CHUNK_BYTES=1024.
    hdr.flags       = static_cast<uint16_t>(std::min<size_t>(payload.size(), 0xFFFFu));
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), payload.data(), payload.size());

    OutboundBroadcast(buf.data(), buf.size());
}

// ─── Phase F: UDP input accelerator (host side) ──────────────────────────

// Kill switch: FM2K_SPEC_UDP=0 disables both the host sender and the
// viewer admission. Default ON.
bool SpecUdpEnabled() {
    static int s_state = -1;
    if (s_state < 0) {
        const char* v = std::getenv("FM2K_SPEC_UDP");
        s_state = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
    }
    return s_state != 0;
}

// Broadcast the last-K-confirmed-inputs window as a raw datagram to every
// direct subscriber that advertised SPEC_JOIN_UDP_OK. Pure accelerator:
// the same inputs also flow via the TCP EVENT_BATCH stream; the viewer's
// positional dedup makes double delivery free. See spectator_node.h
// (UDP_INPUT_BATCH) for the wire layout and admission invariant.
void SendUdpInputBatches() {
    if (!SpecUdpEnabled()) return;
    if (g_state.spec_transport_relay) return;   // no direct UDP addressing
    if (g_state.subscribers.empty()) return;
    const uint32_t total = g_state.total_input_count;
    if (total == 0) return;

    const uint32_t window = (uint32_t)std::min<uint64_t>(SPEC_UDP_WINDOW, total);
    const uint32_t start  = total - window;

    // Per-op tail overhead is 9 bytes (u32 op_index + u32 input_pos +
    // u8 len) -- the original 5-byte budget undersized this array and the
    // emitter wrote up to 32 bytes past it once the ring held 8 ops
    // (exactly at battle entry, when MATCH_START joins the ring): silent
    // host-side stack corruption, observed as the UDP feed going dark at
    // 14:57:58 2026-06-11.
    uint8_t buf[sizeof(SpecDataHeader) + 4 + SPEC_UDP_WINDOW * 4
                + 1 + State::OPS_RING * (9 + sizeof(State::OpWire::bytes))];
    SpecDataHeader hdr = {};
    hdr.magic       = SPEC_DATA_MAGIC;
    hdr.type        = SpecDataType::UDP_INPUT_BATCH;
    hdr.start_frame = start;
    hdr.frame_count = (uint16_t)window;
    std::memcpy(buf, &hdr, sizeof(hdr));
    const uint32_t op_seq = g_state.total_op_count;
    std::memcpy(buf + sizeof(hdr), &op_seq, 4);
    uint8_t* w = buf + sizeof(hdr) + 4;
    for (uint32_t f = start; f < total; ++f) {
        const size_t slot = f % SPEC_UDP_WINDOW;
        std::memcpy(w, &g_state.udp_ring_p1[slot], 2); w += 2;
        std::memcpy(w, &g_state.udp_ring_p2[slot], 2); w += 2;
    }
    // Redundant ops tail: [u8 count] then per op [u32 op_index][u8 len]
    // [len bytes]. The last few ops re-ship in every datagram; the
    // receiver accepts them strictly in order (op_index == ops_seen), so
    // boundary clusters survive a dead TCP stream.
    {
        const uint32_t total_ops = g_state.total_op_count;
        const uint32_t n = (uint32_t)std::min<uint64_t>(State::OPS_RING, total_ops);
        uint8_t* count_at = w; *w++ = 0;
        uint8_t emitted = 0;
        for (uint32_t i = total_ops - n; i < total_ops; ++i) {
            const auto& slot = g_state.udp_ops_ring[i % State::OPS_RING];
            if (slot.len == 0 || slot.op_index != i) continue;  // overwritten
            // MTU guard: never let the datagram exceed ~1400B -- a
            // fragmented datagram under loss dies as a unit.
            if ((size_t)(w - buf) + 9 + slot.len > 1400) break;
            std::memcpy(w, &slot.op_index, 4); w += 4;
            std::memcpy(w, &slot.input_pos, 4); w += 4;
            *w++ = slot.len;
            std::memcpy(w, slot.bytes, slot.len); w += slot.len;
            ++emitted;
        }
        *count_at = emitted;
    }
    const size_t len = (size_t)(w - buf);
    hdr.flags = (uint16_t)(len - sizeof(SpecDataHeader));
    std::memcpy(buf, &hdr, sizeof(hdr));  // re-stamp with final flags

    size_t fanned = 0;
    for (const auto& sub : g_state.subscribers) {
        if (!sub.udp_ok || !sub.tcp_bound) continue;
        ControlChannel_SendRawTo(buf, len, sub.addr);
        if (++fanned >= SPEC_UDP_MAX_FANOUT) break;
    }
}

// TCP-borne op-count baseline for a freshly-bound subscriber. Must ship
// BEFORE the snapshot/backfill on the same connection so the viewer's
// ops_seen starts exact for the events this connection will deliver.
void SendOpBaselineTo(const sockaddr_in& to, uint32_t baseline) {
    uint8_t buf[sizeof(SpecDataHeader) + 4];
    SpecDataHeader hdr = {};
    hdr.magic       = SPEC_DATA_MAGIC;
    hdr.type        = SpecDataType::OP_BASELINE;
    hdr.start_frame = 0;
    hdr.frame_count = 0;
    hdr.flags       = 4;
    std::memcpy(buf, &hdr, sizeof(hdr));
    std::memcpy(buf + sizeof(hdr), &baseline, 4);
    OutboundSendTo(to, buf, sizeof(buf));
}

}  // namespace specnode
