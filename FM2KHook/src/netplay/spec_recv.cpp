// Spectator-side incoming data dispatch: HandleSpecData (EVENT_BATCH / SNAPSHOT_* /
// OP_BASELINE decode -> playback queue) and HandleUdpInputDatagram (admission-gated
// UDP input fast path). Extracted VERBATIM from spectator_node.cpp. Public API
// (decls in spectator_node.h); reaches specnode helpers via using.
#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
#include "spectator_tcp.h"        // TCP transport for INPUT_BATCH stream
#include "reliable_channel_net.h" // RC_CHAN_SPEC / RC_CHAN_SPEC_SNAPSHOT (cross-channel reorder)
#include "spec_impair.h"          // test-only spectator-downlink loss (FM2K_SPEC_DROP)
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

// Snapshot inbox staleness horizon. There used to be NO timeout at all: an
// abandoned transfer stayed `active` forever, held ~1MB, blocked nothing from
// restarting it, and logged "waiting on stragglers" at 1Hz indistinguishably
// from a slow-but-healthy one. RC retransmits an unacked message every
// clamp(2*RTT, 80ms, 750ms) and retires it at 7s, so 20s with zero forward
// progress means the host has genuinely stopped shipping.
constexpr uint64_t kSnapshotInboxStaleMs = 20000;

// Coverage-set blowup guard. A well-behaved sender emits
// ceil(wire / SPECTATOR_SNAPSHOT_CHUNK_BYTES) disjoint ranges (~1130 for an
// uncompressed 1.08MB blob) which merge down as holes fill. Anyone can inject
// SNAPSHOT_CHUNK (HandleSpecData ignores `from`), so bound the interval count
// rather than letting 1-byte chunks at alternating offsets pin map memory.
constexpr size_t kMaxCoverageIntervals = 4096;

// Write one chunk into the blob. IDEMPOTENT: re-delivering a chunk (host
// re-ship, pre-BEGIN stash replay landing on already-applied bytes) rewrites
// the same bytes and contributes nothing to the coverage set.
static void ApplySnapshotChunk(State::SnapshotInbox& inbox, size_t off,
                               const uint8_t* data, size_t n) {
    if (n == 0 || off + n > inbox.blob.size()) return;  // stray/bad -> ignore
    if (inbox.covered.size() >= kMaxCoverageIntervals) {
        static uint32_t s_last_cap_ms = 0;
        const uint32_t now_ms = GetTickCount();
        if (now_ms - s_last_cap_ms >= 1000) {
            s_last_cap_ms = now_ms;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: snapshot coverage set at cap (%zu ranges) -- "
                "dropping chunk off=%zu n=%zu; the transfer will time out and reset",
                inbox.covered.size(), off, n);
        }
        return;
    }
    std::memcpy(inbox.blob.data() + off, data, n);
    const size_t fresh = fm2k::specsnap::CoverRange(
        inbox.covered, static_cast<uint32_t>(off), static_cast<uint32_t>(n));
    inbox.bytes_received += fresh;
    if (fresh > 0) inbox.last_progress_ms = GetTickCount64();
}

// Drop a transfer that has made no forward progress inside the staleness
// horizon. Called every spectator tick from SpectatorNode_ApplyPendingSnapshot
// (chunk arrival can't drive this -- a dead transfer has no arrivals).
bool SpectatorNode_SweepStaleSnapshotInbox() {
    auto& inbox = g_state.pb_snapshot_inbox;
    if (inbox.pending_apply) return false;   // complete, waiting on the phase gate
    // Two reclaimable shapes: a transfer stuck mid-download, and a pre-BEGIN
    // chunk stash whose BEGIN never arrived (up to 2MB pinned with nothing to
    // apply it to).
    if (!inbox.active && inbox.pending_chunks.empty()) return false;
    const uint64_t now_ms = GetTickCount64();
    const uint64_t last   = inbox.last_progress_ms ? inbox.last_progress_ms
                                                   : inbox.started_ms;
    if (last == 0 || now_ms - last < kSnapshotInboxStaleMs) return false;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: snapshot inbox STALE -- no progress for %llums "
        "(active=%d, %zu/%u wire bytes, %zu coverage ranges, %zu stashed "
        "pre-BEGIN chunks, end_seen=%d) -- resetting so a fresh SNAPSHOT_BEGIN "
        "can start clean",
        (unsigned long long)(now_ms - last), (int)inbox.active,
        inbox.bytes_received,
        (inbox.meta.flags & SNAPSHOT_FLAG_ZERO_RLE) ? inbox.meta.compressed_bytes
                                                    : inbox.meta.total_bytes,
        inbox.covered.size(), inbox.pending_chunks.size(), (int)inbox.end_seen);
    inbox = State::SnapshotInbox{};
    return true;
}

// Finalize once the full blob is present AND SNAPSHOT_END's checksum is known --
// order-independent (unordered blob channel: END may precede the last chunks).
// Decompress, verify fletcher32, mark pending_apply.
static void TryFinalizeSnapshot(State::SnapshotInbox& inbox) {
    if (!inbox.active || !inbox.end_seen || inbox.pending_apply) return;
    const uint32_t expected_wire =
        (inbox.meta.flags & SNAPSHOT_FLAG_ZERO_RLE)
            ? inbox.meta.compressed_bytes : inbox.meta.total_bytes;
    if (!fm2k::specsnap::FullyCovered(inbox.covered, expected_wire)) {
        // Not all chunks yet (unordered channel: END may precede stragglers)
        // -- OR the transfer is permanently stuck. Log the gap at 1Hz so a
        // never-finalizing snapshot is visible instead of silent (task #55:
        // the viewer previously idled on placeholder chars with no clue).
        // The hole count is the actionable number: a shrinking count is a
        // healthy transfer, a static one is a dead chunk awaiting re-ship.
        static uint32_t s_last_gap_ms = 0;
        const uint32_t now_ms = GetTickCount();
        if (now_ms - s_last_gap_ms >= 1000) {
            s_last_gap_ms = now_ms;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: snapshot END seen but inbox incomplete -- "
                "%zu/%u wire bytes covered in %zu range(s) (waiting on stragglers)",
                inbox.bytes_received, expected_wire, inbox.covered.size());
        }
        return;
    }
    if (inbox.meta.flags & SNAPSHOT_FLAG_ZERO_RLE) {
        std::vector<uint8_t> raw(inbox.meta.total_bytes);
        if (!ZeroRleDecompress(inbox.blob.data(), inbox.blob.size(),
                               raw.data(), raw.size())) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: snapshot zero-RLE decompress failed (%zu -> %u) -- discarding",
                inbox.blob.size(), inbox.meta.total_bytes);
            inbox = State::SnapshotInbox{};
            return;
        }
        inbox.blob.swap(raw);
    }
    const uint32_t local = Fletcher32(inbox.blob.data(), inbox.blob.size());
    if (local != inbox.end_checksum) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: snapshot checksum mismatch (local=0x%08X expected=0x%08X) -- discarding",
            local, inbox.end_checksum);
        inbox = State::SnapshotInbox{};
        return;
    }
    inbox.pending_apply = true;
    inbox.active        = false;
    inbox.covered.clear();   // fully covered; free the interval set
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: snapshot finalized (match=%u, %zu bytes, fletcher32=0x%08X) "
        "anchor INPUT-frame=%u (deferred apply)",
        inbox.meta.match_index, inbox.blob.size(), local, inbox.anchor_frame);
}

void SpectatorNode_HandleSpecData(const uint8_t* buf, size_t len,
                                  const sockaddr_in& from, uint8_t rc_channel)
{
    (void)from;
    if (len < sizeof(SpecDataHeader)) return;
    SpecDataHeader hdr;
    std::memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != SPEC_DATA_MAGIC) return;

    const uint8_t* payload = buf + sizeof(hdr);
    const size_t   payload_len = len - sizeof(hdr);

    switch (hdr.type) {
        case SpecDataType::INITIAL_MATCH:
        case SpecDataType::INPUT_BATCH:
        case SpecDataType::INPUT_REQUEST:
            // Legacy wire types -- retired in C12. Production hosts ship
            // MATCH_START / MATCH_END / FINGERPRINT / per-INPUT events
            // inside EVENT_BATCH datagrams. The enum values stay in
            // spectator_node.h for ABI but receivers no longer act on them
            // (silently drop instead of warn -- old peers shouldn't exist
            // in practice and a stale packet during a hub-driven session
            // swap shouldn't spam the log).
            break;
        case SpecDataType::MATCH_END:
            // Same: legacy stand-alone MATCH_END packet retired. The
            // SessionEvent::MATCH_END op flowing through EVENT_BATCH
            // handles match-end on the new path.
            break;
        case SpecDataType::SNAPSHOT_BEGIN: {
            // CURRENT_MATCH-mode bind path opener. Wire layout (see
            // SendSnapshotTo): hdr.start_frame = anchor INPUT-frame for
            // the post-snapshot event stream, hdr.flags = sizeof(meta) =
            // 16, payload = SnapshotMetadata.
            // v1 meta = 16 bytes (no flags/compressed_bytes); v2 = 20.
            constexpr size_t META_V1_BYTES = 16;
            if (payload_len < META_V1_BYTES) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN truncated (payload=%zu, need %zu)",
                    payload_len, META_V1_BYTES);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }
            SnapshotMetadata meta = {};
            std::memcpy(&meta, payload,
                        payload_len >= sizeof(SnapshotMetadata)
                            ? sizeof(SnapshotMetadata) : META_V1_BYTES);
            if (meta.version == 1) {
                // v1 sender: uncompressed, wire size == total.
                meta.flags            = 0;
                meta.compressed_bytes = meta.total_bytes;
            } else if (meta.version != SPECTATOR_SNAPSHOT_VERSION) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN version mismatch "
                    "(got %u, want <= %u) -- dropping snapshot, peer must "
                    "rejoin with FULL_SESSION",
                    meta.version, SPECTATOR_SNAPSHOT_VERSION);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }
            const uint32_t wire_bytes =
                (meta.flags & SNAPSHOT_FLAG_ZERO_RLE) ? meta.compressed_bytes
                                                      : meta.total_bytes;
            if (wire_bytes == 0 || wire_bytes > meta.total_bytes) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN bad wire size %u (total %u)",
                    wire_bytes, meta.total_bytes);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }

            const size_t expected_slot_size = SaveState_GetSlotByteSize();
            if (meta.total_bytes == 0 ||
                meta.total_bytes != expected_slot_size) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN size mismatch "
                    "(blob=%u, slot=%zu) -- engine variant mismatch?",
                    meta.total_bytes, expected_slot_size);
                g_state.pb_snapshot_inbox = State::SnapshotInbox{};
                break;
            }

            auto& inbox = g_state.pb_snapshot_inbox;
            const uint64_t begin_now_ms = GetTickCount64();
            // Does this BEGIN describe the SAME transfer the inbox already
            // holds? A host re-ship (destructive-reset re-JOIN path) repeats
            // byte-identical metadata AND byte-identical chunk contents, so it
            // must CONTINUE the in-progress transfer. The old unconditional
            // restart was strictly destructive: it zeroed the byte counter and
            // reallocated the blob, discarding every chunk already delivered --
            // and RC had acked those under the previous msg_seqs, so they were
            // never re-delivered. Under loss that turned a re-ship (the only
            // repair we have) into a guaranteed net loss of progress.
            const bool same_transfer =
                inbox.meta.version            == meta.version &&
                inbox.meta.flags              == meta.flags &&
                inbox.meta.total_bytes        == meta.total_bytes &&
                inbox.meta.match_index        == meta.match_index &&
                inbox.meta.compressed_bytes   == meta.compressed_bytes &&
                inbox.meta.captured_game_mode == meta.captured_game_mode &&
                inbox.anchor_frame            == hdr.start_frame;

            if (inbox.pending_apply) {
                // Already finalized and waiting on the phase gate. A duplicate
                // BEGIN for the same snapshot -- or a stale re-ship of an OLDER
                // one arriving during rebind churn -- must not wipe it. The old
                // code set pending_apply = false unconditionally here, so a
                // single late BEGIN threw away a complete, checksum-verified
                // snapshot.
                const bool older =
                    meta.match_index < inbox.meta.match_index ||
                    (meta.match_index == inbox.meta.match_index &&
                     hdr.start_frame  <  inbox.anchor_frame);
                if (same_transfer || older) {
                    static uint32_t s_last_dup_ms = 0;
                    const uint32_t nm = GetTickCount();
                    if (nm - s_last_dup_ms >= 1000) {
                        s_last_dup_ms = nm;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "SpectatorNode: SNAPSHOT_BEGIN (match=%u anchor=%u) "
                            "for an already-finalized%s snapshot -- ignoring, "
                            "the pending apply stands",
                            meta.match_index, hdr.start_frame,
                            same_transfer ? "" : " newer");
                    }
                    break;
                }
                // A genuinely NEWER snapshot supersedes the pending one.
            }

            const bool fresh_start = !(inbox.active && same_transfer);
            if (fresh_start) {
                if (inbox.active) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: SNAPSHOT_BEGIN restart -- different "
                        "snapshot (prior inbox had %zu/%u wire bytes for "
                        "match=%u anchor=%u, dropping)",
                        inbox.bytes_received, inbox.meta.total_bytes,
                        inbox.meta.match_index, inbox.anchor_frame);
                }
                inbox.bytes_received = 0;
                inbox.covered.clear();
                inbox.blob.assign(wire_bytes, 0);  // wire (possibly compressed) bytes
                inbox.end_seen       = false;
                inbox.started_ms     = begin_now_ms;
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_BEGIN duplicate for the transfer "
                    "in progress -- CONTINUING (%zu/%u wire bytes covered in "
                    "%zu range(s))",
                    inbox.bytes_received, wire_bytes, inbox.covered.size());
            }
            inbox.meta             = meta;
            inbox.anchor_frame     = hdr.start_frame;
            inbox.active           = true;
            inbox.pending_apply    = false;
            inbox.last_progress_ms = begin_now_ms;
            // Replay chunks that arrived before this BEGIN (unordered blob channel).
            {
                auto pend = std::move(inbox.pending_chunks);
                inbox.pending_chunks.clear();
                inbox.pending_bytes = 0;   // held bytes are now applied to the blob
                for (auto& pc : pend)
                    ApplySnapshotChunk(inbox, pc.first, pc.second.data(), pc.second.size());
            }
            // Replay a stashed pre-BEGIN END the same way (task #55 root #3).
            if (inbox.pending_end_valid) {
                inbox.end_checksum      = inbox.pending_end_checksum;
                inbox.end_seen          = true;
                inbox.pending_end_valid = false;
            }
            // On a CONTINUE the END may already have been seen and the missing
            // chunks may have landed in the meantime, so always re-test.
            TryFinalizeSnapshot(inbox);

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: SNAPSHOT_BEGIN match=%u, %u bytes, "
                "anchor INPUT-frame=%u, captured_game_mode=%u",
                meta.match_index, meta.total_bytes, inbox.anchor_frame,
                meta.captured_game_mode);

            // Battle-snapshot fast-walk for spec: when the snapshot is
            // captured at battle (game_mode=3000) but spec is still in
            // boot/title/CSS, the pb_queue starts at a post-CSS frame
            // and contains no CSS-confirm inputs. CssAutoConfirm gives
            // us programmatic "pick chars + lock in" so the spec's
            // local engine can transition CSS → battle on its own.
            // Targets are arbitrary (0/0/stage 0); the snapshot apply
            // at game_mode=3000 overwrites the entire char-data pool
            // with the host's actual matchup state.
            //
            // For CSS snapshots (captured_game_mode=2000), DON'T arm --
            // the snapshot itself will populate cursor positions +
            // selected chars when it applies at game_mode==2000, and
            // CssAutoConfirm's pinning logic would fight the loaded
            // state.
            // Only on a FRESH transfer: a duplicate BEGIN that merely continues
            // an in-progress download must not re-arm the pin (it was already
            // armed by the original BEGIN, and re-arming mid-CSS-walk restarts
            // the placeholder drive). Under the old unconditional-restart
            // semantics every BEGIN was a fresh start, so this is behavior-
            // preserving for every case that existed before.
            if (fresh_start && g_spectator_mode && meta.captured_game_mode == 3000u) {
                CssAutoConfirm_OnReplayMatchStart(
                    /*p1_char=*/0, /*p1_color=*/0,
                    /*p2_char=*/0, /*p2_color=*/0,
                    /*stage_id=*/0);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: armed CssAutoConfirm for spec "
                    "(battle snapshot -- drive CSS forward to game_mode "
                    "3000 with placeholder chars; snapshot apply will "
                    "overwrite)");
            }
            break;
        }
        case SpecDataType::SNAPSHOT_CHUNK: {
            // hdr.start_frame = byte offset, hdr.flags = chunk byte count. Chunks
            // arrive UNORDERED and are reassembled by offset (a lost chunk delays
            // only itself). A chunk that beats BEGIN is held and replayed on BEGIN.
            auto& inbox = g_state.pb_snapshot_inbox;
            const size_t   off     = hdr.start_frame;
            const size_t   chunk_n = hdr.flags;
            if (chunk_n == 0 || chunk_n > payload_len) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_CHUNK bad length (flags=%zu, payload=%zu)",
                    chunk_n, payload_len);
                break;
            }
            if (!inbox.active) {
                // Hold chunks that arrive before SNAPSHOT_BEGIN (cross-channel
                // reorder). Bound BOTH the count and the TOTAL BYTES so an
                // attacker on the spectator input path can't pin memory with a
                // flood of pre-BEGIN chunks.
                // The COUNT cap has to track the chunk size: at 16KB/chunk 256
                // slots held 4MB (bytes bound first), but at 960B/chunk they
                // hold only 245KB, so the count would bind first and silently
                // throw away most of a pre-BEGIN burst -- exactly the bytes RC
                // has already acked and will never re-send. 2048 slots x 960B
                // = ~2MB, so the byte cap is the binding one again.
                constexpr size_t kMaxPendingChunks = 2048;
                constexpr size_t kMaxPendingBytes  = 2u * 1024 * 1024;
                if (inbox.pending_chunks.size() < kMaxPendingChunks &&
                    inbox.pending_bytes + chunk_n <= kMaxPendingBytes) {
                    inbox.pending_chunks.emplace_back(
                        static_cast<uint32_t>(off),
                        std::vector<uint8_t>(payload, payload + chunk_n));
                    inbox.pending_bytes += chunk_n;
                    // Stamp progress so the staleness sweep can reclaim a stash
                    // whose BEGIN never arrives.
                    inbox.last_progress_ms = GetTickCount64();
                }
                break;
            }
            ApplySnapshotChunk(inbox, off, payload, chunk_n);
            TryFinalizeSnapshot(inbox);   // finalize now if this was the last chunk
            break;
        }
        case SpecDataType::SNAPSHOT_END: {
            // hdr.flags = 4, payload = uint32 fletcher32 over the host's blob.
            // UNORDERED channel: END may arrive before the last chunks, so we only
            // RECORD the checksum here; TryFinalizeSnapshot validates once the blob
            // is complete (called here and from each CHUNK).
            auto& inbox = g_state.pb_snapshot_inbox;
            if (payload_len < sizeof(uint32_t)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END truncated (payload=%zu)", payload_len);
                break;
            }
            if (!inbox.active) {
                // Unordered blob channel: END can beat BEGIN. Stash it --
                // BEGIN's init used to reset end_seen=false, permanently
                // stranding the snapshot with zero log output (task #55
                // root #3; observed as frag_recv frozen + viewer on
                // placeholder chars).
                std::memcpy(&inbox.pending_end_checksum, payload, sizeof(uint32_t));
                inbox.pending_end_valid = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: SNAPSHOT_END arrived before BEGIN -- stashed "
                    "(checksum=0x%08X)", inbox.pending_end_checksum);
                break;
            }
            std::memcpy(&inbox.end_checksum, payload, sizeof(uint32_t));
            inbox.end_seen = true;
            TryFinalizeSnapshot(inbox);   // validates now iff all chunks already present
            break;
        }
        case SpecDataType::OP_BASELINE: {
            // Phase F: op-count baseline for this connection. Seeds
            // ops_seen with the count of non-INPUT events appended before
            // this connection's backfill start (which we will never
            // receive), and (re-)arms the UDP admission epoch. Sent by the
            // host's bind path BEFORE snapshot/backfill, so by TCP
            // ordering it always precedes the first EVENT_BATCH of the
            // fresh connection.
            if (!g_state.subscribed_upstream) return;
            if (payload_len >= 4) {
                uint32_t baseline = 0;
                std::memcpy(&baseline, payload, 4);
                g_state.conn_ops_baseline = baseline;
                g_state.conn_ops_decoded  = 0;
                g_state.tcp_rejoin_pending = false;
                if (baseline > g_state.ops_seen) {
                    g_state.ops_seen = baseline;
                }
                g_state.udp_epoch_armed = SpecUdpEnabled();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: OP_BASELINE=%u received -- UDP "
                    "admission %s (ops_seen=%u)", baseline,
                    g_state.udp_epoch_armed ? "armed" : "disabled by env",
                    g_state.ops_seen);
            }
            break;
        }
        case SpecDataType::EVENT_BATCH:
        case SpecDataType::EVENT_BATCH2: {
            // EVENT_BATCH2 = EVENT_BATCH + absolute op identity in
            // hdr.frame_count (see spectator_node.h). -1 = legacy batch.
            const int32_t batch_op_base =
                (hdr.type == SpecDataType::EVENT_BATCH2)
                    ? (int32_t)hdr.frame_count : -1;
            // Primary C2+ ingest. Payload is a packed SessionEvent[] stream
            // (1-byte tag + variant payload per event). Walk the stream
            // sequentially; for INPUT events apply the contiguous-frame
            // dedup gate; for non-INPUT events push if and only if the
            // batch as a whole is at-or-after next_expected_frame (i.e. we
            // haven't already consumed past the INPUTs they precede).
            //
            // Receive-side gate: subscription is the only thing that
            // matters here. `playing_back` was the legacy
            // "between INITIAL_MATCH and MATCH_END" state -- but
            // MATCH_END now arrives in-band as an event and flips
            // playing_back=false at apply time (drain-at-head). If we
            // gated receive on playing_back we'd drop the CSS frames
            // + next-match MATCH_START that flow through the same
            // EVENT_BATCH stream right after MATCH_END.
            if (!g_state.subscribed_upstream) return;
            g_state.live_established = true;

            // Process one batch's events in host-append order. Runs for the
            // current batch and for reorder-buffered batches (drained in frame
            // order), so cross-channel arrival order is normalized to append order.
            auto process_events = [](uint32_t start_frame, int32_t op_base,
                                     const uint8_t* payload, size_t payload_len) {
                const uint32_t expected_at_entry = g_state.next_expected_frame;
                uint32_t batch_ops_decoded = 0;

            // NOTE: there is deliberately NO "fully-consumed batch" early
            // skip here. AppendOpAndFlush emits op-only batches with
            // frame_count=0 at the live edge (MATCH_END / MATCH_START use
            // the flush-then-append pattern, so the boundary ops always
            // ride such a batch). For a caught-up spectator,
            // start_frame + 0 <= next_expected_frame held, and the old
            // skip silently dropped MATCH_END and the next MATCH_START --
            // the spectator never followed into match 2. The per-event
            // walk below already dedups INPUTs positionally and gates ops
            // on cursor_input >= next_expected_frame, so redundant batches
            // cost ~8 decodes and nothing else.

            size_t off = 0;
            uint32_t  cursor_input  = start_frame;  // next INPUT in this batch
            uint32_t  pushed_inputs = 0;
            uint32_t  skipped_inputs = 0;
            uint32_t  gap_first      = 0xFFFFFFFFu;
            while (off < payload_len) {
                SessionEvent ev{};
                uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE];
                size_t consumed = SessionEvent_Decode(payload + off, payload_len - off,
                                                      &ev, hdr_buf);
                if (consumed == 0) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SpectatorNode: EVENT_BATCH decode failed at off=%zu (len=%zu)",
                        off, payload_len);
                    break;
                }

                if (ev.type == SessionEventType::INPUT) {
                    if (cursor_input < g_state.next_expected_frame) {
                        ++skipped_inputs;          // already consumed
                    } else if (cursor_input > g_state.next_expected_frame) {
                        if (gap_first == 0xFFFFFFFFu) gap_first = cursor_input;
                    } else {
                        g_state.pb_queue.push_back(ev);
                        SpectatorNode_StampInputAdmit();
                        g_state.next_expected_frame = cursor_input + 1;
                        ++pushed_inputs;
                        // Hop-1 relay.
                        SpectatorNode_OnFrameConfirmed(ev.u.input.p1, ev.u.input.p2);
                    }
                    ++cursor_input;
                } else {
                    // Op identity. EVENT_BATCH2 carries the batch's absolute
                    // op base (index over ALL non-INPUT session events in
                    // append order) -- idempotent under any re-delivery:
                    // the health watchdog's mid-session re-backfill re-ships
                    // boundary ops, and the legacy positional counter
                    // (baseline + count-of-decoded) re-NUMBERED those
                    // duplicates as fresh ops, letting MATCH_START +
                    // RESET_INPUT_STATE apply twice at the rematch seam
                    // (2026-07-17: +103-frame unreset input-buffer index ->
                    // alternating-parity full-state CRC churn from bf=28).
                    // Legacy EVENT_BATCH keeps the positional counter.
                    uint32_t conn_op_idx;
                    if (op_base >= 0) {
                        conn_op_idx = (uint32_t)op_base + batch_ops_decoded;
                    } else {
                        conn_op_idx = g_state.conn_ops_baseline
                                      + g_state.conn_ops_decoded;
                    }
                    ++batch_ops_decoded;
                    ++g_state.conn_ops_decoded;
                    if (conn_op_idx < g_state.ops_seen) {
                        off += consumed;
                        continue;  // duplicate (re-delivered / UDP-accepted)
                    }
                    g_state.ops_seen = conn_op_idx + 1;

                    // Non-INPUT event. Apply when its position (= cursor_input,
                    // i.e. the index of the next INPUT) is at or after the
                    // current dedup cursor -- meaning the INPUT it logically
                    // precedes hasn't been consumed yet. Otherwise the op is
                    // stale (the spectator has already executed past that
                    // boundary in a previous batch).
                    if (cursor_input >= g_state.next_expected_frame) {
                        if (ev.type == SessionEventType::MATCH_START) {
                            // Stash header in the side table so the playback
                            // driver can look it up by match_start_idx.
                            MatchHeader hdr_copy;
                            std::memcpy(hdr_copy.data(), hdr_buf, hdr_copy.size());
                            g_state.pb_match_headers.push_back(hdr_copy);
                            ev.u.match_start_idx =
                                static_cast<uint16_t>(g_state.pb_match_headers.size() - 1);
                        }
                        g_state.pb_queue.push_back(ev);

                        // Hop-1 relay: forward non-INPUT ops to sub-spectators
                        // via the same Append* helpers the host uses. AppendOp
                        // pushes to local session_events + flushes if we have
                        // subscribers. INPUT relay is handled separately above
                        // via SpectatorNode_OnFrameConfirmed.
                        switch (ev.type) {
                            case SessionEventType::PIN_RNG:
                                SpectatorNode_AppendPinRng(ev.u.pin_rng_seed);
                                break;
                            case SessionEventType::RESET_INPUT_STATE:
                                SpectatorNode_AppendResetInputState();
                                break;
                            case SessionEventType::SOUND_INIT:
                                SpectatorNode_AppendSoundInit();
                                break;
                            case SessionEventType::FINGERPRINT:
                                SpectatorNode_AppendFingerprint(ev.u.fingerprint_hash);
                                break;
                            case SessionEventType::MATCH_START:
                                // hdr_buf was populated by SessionEvent_Decode;
                                // re-append on this relay node so its sub-
                                // spectators see the same MATCH_START in
                                // their own backfill.
                                SpectatorNode_AppendMatchStart(hdr_buf);
                                break;
                            case SessionEventType::MATCH_END:
                                SpectatorNode_AppendMatchEnd(ev.u.match_end);
                                break;
                            case SessionEventType::SESSION_ID:
                                SpectatorNode_AppendSessionId(ev.u.session_id);
                                break;
                            case SessionEventType::CSS_ENTERED:
                                SpectatorNode_AppendCssEntered();
                                break;
                            case SessionEventType::ROUND_START: {
                                const auto& p = ev.u.round_start;
                                SpectatorNode_AppendRoundStart(
                                    p.round_idx, p.p1_hp_max, p.p2_hp_max,
                                    p.timer_seconds);
                                break;
                            }
                            case SessionEventType::ROUND_END: {
                                const auto& p = ev.u.round_end;
                                SpectatorNode_AppendRoundEnd(
                                    p.winner_idx, p.p1_hp_remaining,
                                    p.p2_hp_remaining);
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }
                off += consumed;
            }
            if (gap_first != 0xFFFFFFFFu) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: EVENT_BATCH out-of-order INPUT %u (expected=%u, "
                    "batch start=%u op_base=%d, pushed=%u skipped=%u)",
                    gap_first, expected_at_entry,
                    start_frame, op_base, pushed_inputs, skipped_inputs);
            }
            // pb_queue / batch / subs status -- diagnostic (~1 line/sec).
            // Routed via SDL_LOG_CATEGORY_CUSTOM into quill's backtrace
            // ring; only flushed to disk on a subsequent LOG_ERROR or
            // when FM2K_SPECTATOR_DEBUG=1 is set for live diagnostic.
            {
                static uint32_t last_log_tick = 0;
                uint32_t now = (uint32_t)GetTickCount64();
                if (now - last_log_tick > 1000) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_CUSTOM,
                        "SpectatorNode: pb_queue=%zu (batch op_base=%d, start=%u, subs=%zu)",
                        g_state.pb_queue.size(), op_base, start_frame,
                        g_state.subscribers.size());
                    last_log_tick = now;
                }
            }
            };  // end process_events lambda

            // --- Cross-channel reorder dispatch ---
            // Only the BULK stream (snapshot/backfill on RC_CHAN_SPEC_SNAPSHOT) or a
            // single ordered stream (rc_channel==0, i.e. TCP) may establish the frame
            // baseline. A live (RC_CHAN_SPEC) batch arriving before the baseline, or
            // any batch ahead of next_expected_frame, is buffered by start_frame and
            // drained in frame order -- so ops+inputs resume host-append order.
            const bool can_base = (rc_channel == 0 || rc_channel == RC_CHAN_SPEC_SNAPSHOT);
            auto buffer_batch = [&]() {
                if (g_state.pb_reorder.size() < 1024) {
                    auto& rb = g_state.pb_reorder[hdr.start_frame];
                    rb.op_base = batch_op_base;
                    rb.bytes.assign(payload, payload + payload_len);
                }
            };
            if (!g_state.have_frame_baseline) {
                if (!can_base) { buffer_batch(); break; }   // hold live until bulk anchors
                g_state.have_frame_baseline = true;
                g_state.next_expected_frame = hdr.start_frame;
            } else if (hdr.start_frame > g_state.next_expected_frame) {
                buffer_batch(); break;                       // future -> reorder
            }
            process_events(hdr.start_frame, batch_op_base, payload, payload_len);
            // Drain buffered batches now consumable, strictly in frame order.
            while (!g_state.pb_reorder.empty()) {
                auto it = g_state.pb_reorder.begin();
                if (it->first > g_state.next_expected_frame) break;
                uint32_t bf = it->first;
                State::ReorderBatch b = std::move(it->second);
                g_state.pb_reorder.erase(it);
                process_events(bf, b.op_base, b.bytes.data(), b.bytes.size());
            }
            break;
        }
    }
}

void SpectatorNode_HandleUdpInputDatagram(const uint8_t* buf, size_t len,
                                          const sockaddr_in& from) {
    // Narrow by design: only UDP_INPUT_BATCH, only from the current
    // upstream, only while the per-connection admission epoch is armed.
    // Never runs the full HandleSpecData parser -- its dedup/ordering
    // assumptions are TCP-shaped.
    if (!SpecUdpEnabled()) return;
    if (len < sizeof(SpecDataHeader) + 4) return;
    SpecDataHeader hdr;
    std::memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != SPEC_DATA_MAGIC) return;
    if (hdr.type != SpecDataType::UDP_INPUT_BATCH) return;
    if (!g_state.subscribed_upstream) return;
    if (!g_state.have_frame_baseline) return;
    if (!g_state.udp_epoch_armed) return;
    if (!AddrEqual(from, g_state.upstream_addr)) return;  // spoof / stale upstream
    // Test-only spectator-downlink loss (no-op unless FM2K_SPEC_DROP set).
    // Discard BEFORE the liveness stamp + admission so a "dropped" batch is
    // indistinguishable from one lost on the wire.
    if (fm2k::specimpair::ShouldDropUdpInput()) return;
    g_state.last_udp_recv_ms = GetTickCount64();

    uint32_t op_seq = 0;
    std::memcpy(&op_seq, buf + sizeof(SpecDataHeader), 4);
    const size_t frames = hdr.frame_count;
    const size_t inputs_end = sizeof(SpecDataHeader) + 4 + frames * 4;
    if (frames == 0 || len < inputs_end) return;  // malformed
    if (op_seq > g_state.udp_highest_op_seq) {
        g_state.udp_highest_op_seq = op_seq;
    }

    // Redundant ops tail: parsed here, accepted via accept_eligible()
    // interleaved with the input admission below. An op enters the queue
    // only when (a) it is the next op in global order and (b) every
    // input that precedes it positionally has been admitted -- accepting
    // early pushed boundary ops ahead of the battle-tail inputs (the
    // seam engaged early, its mask expired into leftover attack bits,
    // carried cursors insta-locked: the 17/13 battle restart,
    // 2026-06-11). The tail reships in every datagram, so deferred
    // acceptance costs nothing.
    struct TailOp { uint32_t idx; uint32_t pos; const uint8_t* w; uint8_t wlen; bool done; };
    TailOp tail_ops[State::OPS_RING] = {};
    int tail_n = 0;
    if (len > inputs_end + 1) {
        const uint8_t* t   = buf + inputs_end;
        const uint8_t* end = buf + len;
        uint8_t n = *t++;
        for (uint8_t i = 0; i < n && t + 9 <= end &&
                            tail_n < (int)State::OPS_RING; ++i) {
            TailOp& o = tail_ops[tail_n];
            std::memcpy(&o.idx, t, 4); t += 4;
            std::memcpy(&o.pos, t, 4); t += 4;
            o.wlen = *t++;
            if (t + o.wlen > end) break;
            o.w = t; o.done = false; t += o.wlen;
            ++tail_n;
        }
    }
    auto accept_eligible = [&]() {
        for (bool progress = true; progress; ) {
            progress = false;
            for (int i = 0; i < tail_n; ++i) {
                TailOp& o = tail_ops[i];
                if (o.done || o.idx != g_state.ops_seen ||
                    g_state.next_expected_frame < o.pos) continue;
                o.done = true;
                SessionEvent ev{};
                uint8_t hdr_buf[SESSION_EVENT_MATCH_HDR_SIZE];
                const size_t consumed =
                    SessionEvent_Decode(o.w, o.wlen, &ev, hdr_buf);
                if (consumed != o.wlen ||
                    ev.type == SessionEventType::INPUT) continue;
                if (ev.type == SessionEventType::MATCH_START) {
                    MatchHeader hdr_copy;
                    std::memcpy(hdr_copy.data(), hdr_buf, hdr_copy.size());
                    g_state.pb_match_headers.push_back(hdr_copy);
                    ev.u.match_start_idx =
                        (uint16_t)(g_state.pb_match_headers.size() - 1);
                }
                g_state.pb_queue.push_back(ev);
                ++g_state.ops_seen;
                progress = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SPEC-UDP] accepted op #%u type=%u at pos=%u via "
                    "datagram tail", o.idx, (unsigned)ev.type, o.pos);
                switch (ev.type) {
                    case SessionEventType::PIN_RNG:
                        SpectatorNode_AppendPinRng(ev.u.pin_rng_seed); break;
                    case SessionEventType::RESET_INPUT_STATE:
                        SpectatorNode_AppendResetInputState(); break;
                    case SessionEventType::SOUND_INIT:
                        SpectatorNode_AppendSoundInit(); break;
                    case SessionEventType::FINGERPRINT:
                        SpectatorNode_AppendFingerprint(ev.u.fingerprint_hash); break;
                    case SessionEventType::MATCH_START:
                        SpectatorNode_AppendMatchStart(hdr_buf); break;
                    case SessionEventType::MATCH_END:
                        SpectatorNode_AppendMatchEnd(ev.u.match_end); break;
                    case SessionEventType::SESSION_ID:
                        SpectatorNode_AppendSessionId(ev.u.session_id); break;
                    case SessionEventType::CSS_ENTERED:
                        SpectatorNode_AppendCssEntered(); break;
                    case SessionEventType::ROUND_START: {
                        const auto& rp = ev.u.round_start;
                        SpectatorNode_AppendRoundStart(rp.round_idx,
                            rp.p1_hp_max, rp.p2_hp_max, rp.timer_seconds);
                        break;
                    }
                    case SessionEventType::ROUND_END: {
                        const auto& rp = ev.u.round_end;
                        SpectatorNode_AppendRoundEnd(rp.winner_idx,
                            rp.p1_hp_remaining, rp.p2_hp_remaining);
                        break;
                    }
                    default: break;
                }
            }
        }
    };
    accept_eligible();

    // [SPEC-UDP] 1Hz heartbeat BEFORE the gates -- pause periods are
    // exactly when this diagnostic matters (the original tail placement
    // went silent the moment the op gate engaged).
    {
        static uint64_t s_last_log_ms = 0;
        const uint64_t now = GetTickCount64();
        if (now - s_last_log_ms >= 1000) {
            s_last_log_ms = now;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-UDP] admitted=%u paused_on_gate=%u dropped_on_gap=%u "
                "op_seq=%u ops_seen=%u q=%zu",
                g_state.udp_admitted_total, g_state.udp_paused_on_gate,
                g_state.udp_dropped_on_gap, op_seq, g_state.ops_seen,
                g_state.pb_queue.size());
        }
    }

    // Admission gate: every op the host appended before these inputs must
    // already be TCP-decoded locally (see invariant in spectator_node.h).
    // Boundary ops in flight (MATCH_END .. MATCH_START) pause admission;
    // the TCP burst that delivers them also jumps the cursor, after which
    // UDP re-engages. Self-healing by redundancy -- nothing is lost.
    {
        bool blocking_op_in_tail = false;
        for (int i = 0; i < tail_n; ++i) {
            if (!tail_ops[i].done && tail_ops[i].idx == g_state.ops_seen) {
                blocking_op_in_tail = true;
                break;
            }
        }
        if (op_seq > g_state.ops_seen && !blocking_op_in_tail) {
            // The op we need fell out of the 8-op tail window and TCP
            // hasn't delivered it -- inputs past it must wait (backfill
            // or a later tail recovers).
            ++g_state.udp_paused_on_gate;
            return;
        }
    }
    // Window entirely ahead of the cursor (loss burst exceeded the
    // redundancy window): drop whole datagram; TCP fills the hole.
    if (hdr.start_frame > g_state.next_expected_frame) {
        ++g_state.udp_dropped_on_gap;
        return;
    }

    uint32_t cursor   = hdr.start_frame;
    uint32_t admitted = 0;
    const uint8_t* p = buf + sizeof(SpecDataHeader) + 4;
    for (size_t i = 0; i < frames; ++i, ++cursor) {
        if (cursor != g_state.next_expected_frame) continue;  // already queued
        // In-order invariant per frame: if an op should precede further
        // inputs and it is not recoverable from this tail, stop here --
        // its position is unknown, so any further admit could jump it.
        if (op_seq > g_state.ops_seen) {
            bool next_op_in_tail = false;
            for (int k = 0; k < tail_n; ++k) {
                if (!tail_ops[k].done &&
                    tail_ops[k].idx == g_state.ops_seen) {
                    next_op_in_tail = true;
                    break;
                }
            }
            if (!next_op_in_tail) {
                ++g_state.udp_paused_on_gate;
                break;
            }
        }
        uint16_t p1 = 0, p2 = 0;
        std::memcpy(&p1, p + i * 4,     2);
        std::memcpy(&p2, p + i * 4 + 2, 2);
        SessionEvent ev{};
        ev.type       = SessionEventType::INPUT;
        ev.u.input.p1 = p1;
        ev.u.input.p2 = p2;
        g_state.pb_queue.push_back(ev);
        SpectatorNode_StampInputAdmit();
        SpectatorNode_StampUdpInputAdmit();  // UDP-borne: feeds the
                                             // TCP-only floor pre-arm
        g_state.next_expected_frame = cursor + 1;
        ++admitted;
        accept_eligible();  // an op positioned right after this input
        // Hop-1 relay parity with the TCP path (line ~3318): the late TCP
        // copy of these frames is positionally skipped there, so without
        // this append a relay node's downstream stream would lose them.
        SpectatorNode_OnFrameConfirmed(p1, p2);
    }
    g_state.udp_admitted_total += admitted;
}
