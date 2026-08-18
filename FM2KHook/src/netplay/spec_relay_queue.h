#pragma once

// Spec hub-relay shared-memory queues.
//
// When the host hook runs in relay mode (FM2K_SPEC_TRANSPORT=relay), spec
// data no longer goes out via P2P TCP. Instead the hook enqueues each
// SpecDataBinary frame into an SPSC ring buffer in shared memory; the
// launcher polls the ring once per tick and forwards each frame to the
// hub as a WebSocket binary message. Hub fans out to subscribed specs.
//
// On the spec side, the inverse: hub sends binary frames to the spec's
// WS, the spec launcher writes them into a second ring buffer in shared
// memory, and the spec hook polls and dispatches through the existing
// receive-handler path (the same handlers that today consume bytes from
// the direct reliable channel).
//
// Two rings per process:
//   Outbound: hook -> launcher (hook produces, launcher consumes)
//   Inbound:  launcher -> hook (launcher produces, hook consumes)
//
// Mappings are named per-game-PID so multiple game instances on the
// same host don't collide:
//   FM2K_SpecRelayOut_<pid>
//   FM2K_SpecRelayIn_<pid>
//
// SPSC discipline: only one producer and one consumer per ring, so no
// locking required. write_idx / read_idx are atomic 64-bit counters
// modulo capacity (capacity is a power of two for cheap masking).
//
// Wire format of Slot mirrors hub.py:SpecDataBinary (see Phase 1 commit
// 33a7890 in fm2k-hub). The launcher packs Slot fields into the over-
// the-wire format right before WS send; the hub un-packs the same way.

#include <cstdint>

namespace fm2k::spec_relay {

// Magic = "SPBG" (SPec Buffer Generation). Distinguishes our mapping
// from a stale one with a different layout. CleanupQueues invalidates
// the magic before unmapping so a stale launcher polling sees the
// invalidation and stops.
constexpr uint32_t QUEUE_MAGIC   = 0x53504247;
constexpr uint32_t QUEUE_VERSION = 1;

// 128 slots * 32 KB = 4 MB per ring. Sizing rationale (audited 2026-05-18):
//
//   * SPECTATOR_SNAPSHOT_CHUNK_BYTES = 16384 (16 KB). Each chunk is
//     wrapped in a 10-byte SpecDataHeader before being handed to
//     OutboundBroadcast / OutboundSendTo. So per-slot payload = up to
//     16394 bytes -- which is why SLOT_PAYLOAD_MAX must be > 16384.
//     Earlier 16384 cap silently dropped every snapshot chunk. We pad
//     to 32 KB for growth headroom + alignment ease.
//   * 850 KB snapshot / 16 KB chunk = ~54 chunks. 128-slot ring fits
//     two back-to-back snapshots with margin; bursty live event batches
//     between snapshots have room.
//   * Backfill EVENT_BATCH chunks are capped at BACKFILL_CHUNK_BYTES =
//     8192 (also +10 SpecDataHeader = 8202 bytes; trivially fits).
//
// Capacity must be a power of two so write_idx % capacity simplifies to
// write_idx & (capacity-1).
constexpr uint32_t QUEUE_CAPACITY    = 128;
constexpr uint32_t SLOT_PAYLOAD_MAX  = 32768;

// Slot targeting kinds. Mirror Phase 1 hub.py SpecDataBinary target_kind:
//   BROADCAST = fan out to every spec subscribed to this host
//   DIRECT    = send only to the spec named by spec_user_id (e.g. the
//               initial snapshot to a freshly-subscribed newcomer)
enum : uint32_t {
    TARGET_BROADCAST = 0,
    TARGET_DIRECT    = 1,
};

// One enqueued frame. Mirrors fm2k-hub's SpecDataBinary wire shape so
// the launcher's pack-and-send step is a single memcpy + struct pack
// per Slot. spec_user_id is fixed-size to keep the slot POD-trivially-
// copyable across the shared-mem mapping; 32 chars covers Discord
// user-id strings (typically 18 digits + nul) with room.
struct Slot {
    uint32_t target_kind;        // TARGET_BROADCAST | TARGET_DIRECT
    char     spec_user_id[32];   // empty for TARGET_BROADCAST; ASCII, NUL-terminated
    uint32_t spec_data_type;     // SpecDataType (INITIAL_MATCH, INPUT_BATCH, ...)
    uint32_t frame_count;        // mirrors SpecDataHeader.frame_count
    uint32_t spec_data_flags;    // mirrors SpecDataHeader.flags
    uint32_t payload_len;        // valid bytes in payload[]
    uint8_t  payload[SLOT_PAYLOAD_MAX];
};

// SPSC ring. write_idx and read_idx are monotonically increasing 64-bit
// counters; slots[] is the underlying storage indexed by counter %
// CAPACITY. Ring is empty when write_idx == read_idx; full when
// write_idx - read_idx == CAPACITY.
//
// Memory ordering:
//   Producer: fill slot fields, then store write_idx with release.
//   Consumer: load write_idx with acquire, read slot fields, then store
//             read_idx with release.
// On 32-bit Windows x86 (our target), 64-bit volatile loads/stores are
// not atomic but our ring uses them inside the lock-coupled compare
// (write_idx - read_idx) which is fine for SPSC -- the only race is
// producer-vs-consumer on opposite counters. Each side only writes its
// own counter; each side reads the other's counter and may see stale
// values (consumer sees "ring less full than it really is" = safe to
// produce more; producer sees "ring more full than it really is" = safe
// drop). No torn reads across counters because they're only ever
// produced by one side.
//
// (For 32-bit, MSVC / GCC's std::atomic<uint64_t> falls back to
// cmpxchg8b which is lock-free but not single-instruction. We use
// alignas(8) + volatile and rely on the SPSC invariant rather than
// pulling std::atomic which complicates cross-DLL layout.)
struct Ring {
    uint32_t magic;       // QUEUE_MAGIC; check before any access
    uint32_t version;     // QUEUE_VERSION; bump if Slot layout changes
    uint32_t capacity;    // QUEUE_CAPACITY (mirror so consumer can sanity-check)
    uint32_t slot_size;   // sizeof(Slot) (mirror; sanity-check across builds)
    alignas(8) volatile uint64_t write_idx;
    alignas(8) volatile uint64_t read_idx;
    // Statistics counters (purely diagnostic; not load-bearing). Useful
    // for the launcher's status line to show "spec relay: 1.2 MB sent,
    // 3 drops" without rummaging through individual slots.
    alignas(8) volatile uint64_t total_enqueued;
    alignas(8) volatile uint64_t total_dropped;   // ring-full drops at producer side
    alignas(8) volatile uint64_t total_dequeued;
    uint8_t  _reserved[40];   // round struct out + reserve for future fields
    Slot     slots[QUEUE_CAPACITY];
};

static_assert(sizeof(Slot) > 16380, "Slot size sanity");
static_assert(QUEUE_CAPACITY != 0 && (QUEUE_CAPACITY & (QUEUE_CAPACITY - 1)) == 0,
              "QUEUE_CAPACITY must be a power of two");

// Build the shared-mapping name for a given direction + target pid.
// `is_outbound`: true = "FM2K_SpecRelayOut_<pid>", false = "FM2K_SpecRelayIn_<pid>".
// Out-of-line in header so both DLL and EXE get the same string (no
// risk of ODR issues from inline + static).
void MakeMappingName(char* buf, size_t buf_sz, bool is_outbound, uint32_t pid);

// Create the outbound mapping for the current process. Called from the
// hook side when relay mode is detected. The hook is the producer for
// "outbound" (data leaving toward the hub via the launcher).
// Returns nullptr on failure; logs cause via SDL_Log.
Ring* CreateOutboundHere();

// Open an existing outbound mapping for the named game pid. Called
// from the launcher side, where the launcher is the consumer.
// Returns nullptr if mapping doesn't exist (e.g. game hasn't booted
// hook yet, or hook is in TCP mode and never created the mapping).
Ring* OpenOutboundFor(uint32_t game_pid);

// Symmetric pair for the inbound direction (launcher producer, hook
// consumer). Spec-side use.
Ring* CreateInboundHere();
Ring* OpenInboundFor(uint32_t game_pid);

// Unmap + close the handle. Safe to call with nullptr ring. The mapping
// object is reference-counted by Windows; the last process to close it
// frees the kernel object.
void Close(Ring* ring);

// Producer: enqueue one SpecDataBinary frame.
//   target_kind:  TARGET_BROADCAST or TARGET_DIRECT
//   spec_user_id: required (non-empty) for TARGET_DIRECT; ignored for
//                 TARGET_BROADCAST. Must fit in 31 chars + NUL.
//   payload:      up to SLOT_PAYLOAD_MAX bytes
//
// Returns true on success. Returns false (and bumps total_dropped) if
// the ring is full -- caller may retry next tick or accept the drop.
// For snapshot transfer, drops are bad (recipient gets a corrupt state);
// the upstream framing should pace itself against the consumer's
// throughput. For INPUT_BATCH at 12 batches/sec, drops are fine (next
// batch's redundancy window re-delivers).
bool Enqueue(Ring* ring,
             uint32_t target_kind,
             const char* spec_user_id,
             uint32_t spec_data_type,
             uint32_t frame_count,
             uint32_t spec_data_flags,
             const void* payload,
             uint32_t payload_len);

// Consumer: peek at the slot at read_idx without advancing. Returns
// nullptr when the ring is empty. The returned pointer remains valid
// until Release() is called.
const Slot* PeekFront(Ring* ring);

// Consumer: advance read_idx past one slot consumed via PeekFront.
void PopFront(Ring* ring);

}  // namespace fm2k::spec_relay
