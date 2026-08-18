#include "spectator_node.h"
#include "spectator_node_internal.h"  // shared State model + g_state (split for sibling TUs)
#include "spec_wire.h"            // zero-RLE codec (SessionEvent_* live in spectator_node.h)
#include "spec_relay_queue.h"     // hub-relay outbound queue (Phase 2c)
#include "spec_pool_sync.h"       // Phase 4c match-start pool resync (apply-side arm/latch)
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
#include <cstddef>       // std::ptrdiff_t (pre-anchor queue trim)
#include <cstdlib>
#include <cstdio>
#include <ctime>

// The spectator-internal helpers now live in namespace specnode (split across
// spec_*.cpp). Pull them into scope so call sites here stay unqualified.
using namespace specnode;

// =============================================================================
// MODULE STATE
// =============================================================================

// struct State + member structs/constants now live in spectator_node_internal.h
// so sibling TUs (spec_*.cpp) can share the state. This file owns the single def.
State g_state;

namespace {




// Legacy SendInitialMatchTo / INITIAL_MATCH packet path removed in C12.
// MATCH_START flows as a SessionEvent op interleaved with INPUTs in the
// EVENT_BATCH stream; late joiners get it via SendSessionBackfillFromStart.


// Legacy SendMatchEndToAll / MATCH_END packet path removed in C12.
// MATCH_END flows as a SessionEvent op (see SpectatorNode_AppendMatchEnd).

// (SendInputRequest + RespondToInputRequest deleted: TCP guarantees in-order
// delivery exactly once, so the spectator-side gap-recovery handshake is
// dead code. The whole class of UDP-loss recovery -- REDUNDANCY_WINDOW,
// INPUT_REQUEST_POLL_MS in TickHealth, the on-gap immediate request inside
// HandleSpecData -- has been removed.)

} // namespace

// =============================================================================
// SESSION EVENT WIRE FORMAT (C1)
// =============================================================================
//
// Pure byte-level encoders/decoders. No socket / state side effects -- these
// just mediate between SessionEvent values and packed wire bytes. Production
// integration (vector<SessionEvent> session_history, head-of-queue drain in
// RunSpectatorTick, etc.) is layered on top in C2+.


// =============================================================================
// PUBLIC API
// =============================================================================

void SpectatorNode_Init() {
    g_state = State{};
    g_state.capacity = SPECTATOR_DEFAULT_CAPACITY;

    // Wire the ReliableChannel deliver dispatcher (harmless if nothing ever
    // arrives on RC_CHAN_SPEC). Runs regardless of transport mode below.
    SpectatorNode_RegisterRcDeliver();

    // FM2K_SPEC_TRANSPORT: "relay" routes spec data through hub WS binary
    // frames instead of the direct reliable channel. Anything else (and
    // unset) means direct. The launcher sets it from the hub's grant; a
    // user never picks it. Read once here at Init -- env changes mid-run
    // wouldn't be safe (existing subscribers expect one mode).
    if (const char* transport = std::getenv("FM2K_SPEC_TRANSPORT");
        transport && std::strcmp(transport, "relay") == 0) {
        g_state.spec_transport_relay = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: FM2K_SPEC_TRANSPORT=relay -- direct reliable "
            "channel suppressed, creating shared-mem queues for "
            "launcher <-> hub forwarding");
        // Create BOTH outbound (hook->launcher) and inbound
        // (launcher->hook) rings unconditionally. Either could be used
        // depending on whether this process ends up acting as host or
        // spec for a given match. ~2 MB of shared mem total per process
        // -- cheap, and avoids late-bound role-detection logic.
        g_state.spec_relay_out = fm2k::spec_relay::CreateOutboundHere();
        g_state.spec_relay_in  = fm2k::spec_relay::CreateInboundHere();
        if (!g_state.spec_relay_out || !g_state.spec_relay_in) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: spec_relay mapping(s) failed (out=%s in=%s); "
                "spec data plane degraded",
                g_state.spec_relay_out ? "ok" : "fail",
                g_state.spec_relay_in  ? "ok" : "fail");
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: Init (capacity=%zu, batch=%zu frames, "
            "transport=relay, out=%s, in=%s)",
            g_state.capacity, BROADCAST_BATCH_FRAMES,
            g_state.spec_relay_out ? "ok" : "failed",
            g_state.spec_relay_in  ? "ok" : "failed");
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Init (capacity=%zu, batch=%zu frames, "
                "transport=rc/udp)",
                g_state.capacity, BROADCAST_BATCH_FRAMES);
}

void SpectatorNode_Shutdown() {
    // C7: write full session log on shutdown if there's anything to flush.
    // Skipped on the spectator side where session_events is the relay log
    // (correct to write -- the relay's local view IS the canonical session
    // for any sub-spectators, even if we'd be one of two writers when
    // host + spectator both run on this machine; per-process file paths
    // already include timestamp + pid disambiguation).
    if (!g_state.session_events.empty()) {
        char ts[64] = {};
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &now);
        std::strftime(ts, sizeof(ts), "sessions/%Y-%m-%d_%H%M%S.fm2kset", &tm_buf);
        CreateDirectoryA("sessions", nullptr);
        SpectatorNode_WriteSessionFile(ts);
    }

    // Best-effort: notify all subscribers before tearing down so they
    // can fail over to root immediately instead of waiting out their
    // silence timer.
    for (const auto& sub : g_state.subscribers) {
        CtrlPacket leave = {};
        leave.header.type = CtrlMsg::SPEC_LEAVE;
        ControlChannel_SendTo(leave, sub.addr);
    }
    // And tell our upstream we're going away (frees their subscriber slot).
    if (g_state.subscribed_upstream) {
        CtrlPacket leave = {};
        leave.header.type = CtrlMsg::SPEC_LEAVE;
        ControlChannel_SendTo(leave, g_state.upstream_addr);
    }

    g_state.subscribers.clear();
    g_state.session_events.clear();
    g_state.session_events.shrink_to_fit();
    g_state.match_headers.clear();
    g_state.match_headers.shrink_to_fit();
    g_state.last_flushed_event_idx = 0;
    g_state.flushed_input_count    = 0;
    g_state.total_input_count      = 0;
    g_state.total_op_count         = 0;
    // The CSS/deep-join anchors are INDICES INTO session_events, which the
    // clear() above just emptied, and nothing here ever reset them. Every
    // existing consumer happened to bounds-check its way out of the stale
    // pair, but the gap-fill scan-start hints added for candidate A1 read
    // css_anchor_input_frame as a scan CURSOR, so a stale one has to be
    // impossible rather than merely usually-caught.
    g_state.have_css_anchor        = false;
    g_state.css_anchor_event_idx   = 0;
    g_state.css_anchor_input_frame = 0;
    g_state.css_anchor_op_count    = 0;
    g_state.have_prior_match       = false;
    g_state.ops_seen               = 0;
    g_state.broadcasting = false;
    g_state.subscribed_upstream = false;
    g_state.ever_subscribed     = false;   // task #70
    g_state.playing_back = false;
    g_state.pb_boundary         = State::PbBoundary::NONE;
    g_state.pending_reset_input = false;
    g_state.pending_sound_init  = false;
    g_state.pb_snapshot_applied_once = false;
    g_state.pb_started               = false;
    CssAutoConfirm_SetSeamHold(false);
    // Tear down both relay rings if we created them. Close handles
    // nullptr. Kernel mapping refcount keeps the object alive while
    // launcher still has it mapped; we just drop our side.
    if (g_state.spec_relay_out) {
        fm2k::spec_relay::Close(g_state.spec_relay_out);
        g_state.spec_relay_out = nullptr;
    }
    if (g_state.spec_relay_in) {
        fm2k::spec_relay::Close(g_state.spec_relay_in);
        g_state.spec_relay_in = nullptr;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SpectatorNode: Shutdown");
}


void SpectatorNode_SetCapacity(size_t max_direct) {
    g_state.capacity = max_direct;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Capacity set to %zu", max_direct);
}


// =============================================================================
// SNAPSHOT CACHE (task #18 phase 2)
// =============================================================================
//
// Capture a fresh SaveState blob at battle entry so a CURRENT_MATCH-mode
// spectator joining mid-set can SaveState_Load directly to the current
// match's start instead of replaying every previous battle's events.
//
// Why call SaveState_Save(0) ourselves: the GekkoNet driver normally
// triggers Save in its first AdvanceEvent (a few sim frames after
// Netplay_StartBattle), but we want the snapshot CAPTURED at the same
// logical instant the host emitted MATCH_START -- before any battle
// frame has run, so the spectator's state on Load is "match just
// starting, frame 0 input pending." That keeps the wire-anchor clean:
// snapshot.input_frame == g_state.total_input_count, and the very next
// INPUT event the host appends becomes the spectator's first popped
// frame after Load.

void SpectatorNode_StashSnapshot() {
    // Skip during a rollback rewind -- the FM2K state at this moment isn't
    // the canonical battle-start state we want to capture. In practice
    // StashSnapshot is called from Netplay_StartBattle which is the seam
    // frame between CSS and battle (no preceding battle inputs to roll
    // back), so g_is_rolling_back should never be true here. Belt-and-
    // suspenders: if a future caller invokes us mid-battle, this gate
    // prevents handing spectators a partially-rewound state. Match cache
    // stays as the previous match's (or empty if first match).
    if (g_is_rolling_back) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: StashSnapshot skipped -- called during rollback "
            "rewind (snapshot cache stays %s)",
            g_state.current_snapshot.valid ? "previous match's" : "empty");
        return;
    }

    // Force a Save to populate the rollback buffer's slot 0 with the
    // current FM2K state. Sets g_initial_sync_done in savestate.cpp;
    // GekkoNet's later first-Save is a no-op for the initial-sync reset.
    if (!SaveState_Save(0)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: StashSnapshot -- SaveState_Save(0) failed; "
            "snapshot cache stays %s",
            g_state.current_snapshot.valid ? "the previous match's" : "empty");
        return;
    }

    const uint8_t* slot_bytes = SaveState_PeekLastSavedSlotBytes();
    const size_t   slot_size  = SaveState_GetSlotByteSize();
    if (!slot_bytes || slot_size == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: StashSnapshot -- slot bytes unavailable post-Save");
        return;
    }

    auto& cache = g_state.current_snapshot;
    cache.blob.assign(slot_bytes, slot_bytes + slot_size);
    cache.input_frame = g_state.total_input_count;
    // match_index = 0-based count of MATCH_STARTs emitted so far.
    // AppendMatchStart pushes to match_headers BEFORE StashSnapshot
    // is called (Netplay_StartBattle calls OnMatchStart → AppendMatchStart
    // → THEN StashSnapshot), so size already reflects this match.
    cache.match_index = g_state.match_headers.empty() ? 0u
                        : (uint32_t)(g_state.match_headers.size() - 1);
    cache.checksum    = Fletcher32(cache.blob.data(), cache.blob.size());
    // Compress ONCE, here, instead of once per recipient inside SendSnapshotTo.
    // That call ran ZeroRleCompress over ~1MB on the host main loop while
    // holding g_poll_mutex, for EVERY viewer AND every re-JOIN re-ship, into a
    // function-local `static` scratch (also a latent aliasing hazard if two
    // ships ever interleaved). SNAPSHOT_END still carries the RAW fletcher32,
    // so the viewer's verification path is unchanged. Keep the raw blob: it is
    // the source of meta.total_bytes and of SpectatorNode_GetSnapshotInfo.
    {
        std::vector<uint8_t> rle;
        ZeroRleCompress(cache.blob, rle);
        if (rle.size() < cache.blob.size()) {
            cache.wire_blob.swap(rle);
            cache.wire_flags = SNAPSHOT_FLAG_ZERO_RLE;
        } else {
            cache.wire_blob = cache.blob;   // compression lost; ship raw
            cache.wire_flags = 0;
        }
    }
    // Phase E: record game_mode at capture time so the spec-side apply
    // can wait for a matching mode. CSS captures get applied during the
    // spec's CSS; battle captures wait for battle.
    cache.captured_game_mode = *(const uint32_t*)FM2K::ADDR_GAME_MODE;
    cache.valid       = true;

    // [SPEC-SNAP] wire_bytes + chunk count are the numbers every snapshot
    // bandwidth decision depends on -- log them at capture so they are a
    // first-class measurement instead of being inferred per recipient.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SPEC-SNAP] snapshot cached (match=%u, raw=%zu bytes, wire=%zu bytes "
        "%s, %zu chunks of %zu, input_frame=%u, fletcher32=0x%08X, "
        "captured_game_mode=%u)",
        cache.match_index, cache.blob.size(), cache.wire_blob.size(),
        (cache.wire_flags & SNAPSHOT_FLAG_ZERO_RLE) ? "zero-RLE" : "uncompressed",
        (cache.wire_blob.size() + SPECTATOR_SNAPSHOT_CHUNK_BYTES - 1) /
            SPECTATOR_SNAPSHOT_CHUNK_BYTES,
        SPECTATOR_SNAPSHOT_CHUNK_BYTES,
        cache.input_frame, cache.checksum, cache.captured_game_mode);

    // Wave 4: arm the bounded deep-join push for every subscriber that is still
    // uncorrected. Must run AFTER the cache refresh above so the arm names the
    // NEW battle, and it deliberately re-arms on EVERY battle entry -- that is
    // what serves a joiner whose first battle ended before its snapshot landed,
    // and what serves a viewer that joined between matches 2 and 3 with battle
    // 3's blob. The actual ship is paced one sub per maintenance tick by
    // DeepJoinPushTick; nothing is sent from here.
    DeepJoinArmSubscribers();

    // Session-kind-change re-broadcast: battle just started, so the
    // chars/stage in a JOIN_ACK are now real. Viewers that joined during
    // CSS hold their /F boot until this arrives (their first ACK said
    // kind=CSS with no chars); HandleJoinAck's kind==BATTLE path seeds
    // their runtime BTB overrides. Idempotent for already-running
    // viewers (their dial guard skips reconnecting).
    if (!g_state.subscribers.empty()) {
        const CtrlPacket ack = BuildJoinAckPacket();
        for (const auto& sub : g_state.subscribers) {
            ControlChannel_SendTo(ack, sub.addr);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: battle-entry JOIN_ACK re-broadcast to %zu sub(s)",
            g_state.subscribers.size());
    }
}

bool SpectatorNode_HasSnapshot() {
    return g_state.current_snapshot.valid;
}

void SpectatorNode_ApplyPendingPinRng() {
    // Seam-deferred battle-init ops first (match-boundary path only; see
    // pending_reset_input). Order mirrors the host's StartBattle: input
    // state reset + sound layer init, then the RNG pin last so nothing
    // can disturb the seed before frame 0's PGI.
    if (g_state.pending_reset_input) {
        ApplyResetInputState();
        g_state.pending_reset_input = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: applied deferred RESET_INPUT_STATE at battle entry");
    }
    if (g_state.pending_sound_init) {
        SoundRollback::Init();
        g_state.pending_sound_init = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: applied deferred SOUND_INIT at battle entry");
    }
    if (!g_state.pending_pin_rng_valid) return;
    *(uint32_t*)0x41FB1C = g_state.pending_pin_rng_seed;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: applied deferred PIN_RNG=0x%08X at battle entry",
        g_state.pending_pin_rng_seed);
    g_state.pending_pin_rng_valid = false;
}

void SpectatorNode_ApplyPendingSnapshot() {
    auto& inbox = g_state.pb_snapshot_inbox;
    // Reclaim a transfer that stopped making progress. This is the only tick
    // the inbox gets: everything else in the snapshot path is arrival-driven,
    // so a dead transfer used to stay `active` forever -- holding ~1MB, making
    // "slow" and "dead" indistinguishable in the log, and (before the
    // BEGIN-continue rule) leaving the next BEGIN to blow away whatever
    // progress a healthy retry had made.
    if (SpectatorNode_SweepStaleSnapshotInbox()) return;
    if (!inbox.pending_apply) return;

    // Wait until the spectator's local engine has reached the SAME
    // phase the snapshot was captured at. The savestate captures the
    // engine state -- object pool, character data, DDraw surfaces, etc.
    // -- and applying it before the local engine has done its own
    // init-for-that-phase lands the captured bytes into structurally-
    // incompatible memory:
    //   - DDraw/D3D9 surfaces sized for the wrong phase layout
    //   - Audio sample handles for the wrong phase's audio set
    //   - Char-data block has content the local engine hasn't loaded
    // Symptom: spectator crashes on the next render frame after apply.
    //
    // By waiting for game_mode >= captured_game_mode the local engine
    // has already performed its own init for the captured phase; the
    // apply just overlays dynamic state on top.
    //
    // Phase E (v0.2.42+): host writes captured_game_mode into the
    // SnapshotMetadata so the spec knows whether to wait for CSS (2000)
    // or battle (3000). Pre-Phase-E hosts (v0.2.41) left this field 0
    // and always captured at battle entry; the captured==0 fallback
    // keeps that wire compat with the v0.2.41 default of
    // `game_mode >= 3000`.
    // Re-join discard runs BEFORE the phase wait: a snapshot we will
    // never apply must not sit in pending_apply waiting for a phase
    // the held pops can never reach. (Captured-at-3000 snapshot +
    // viewer at CSS = circular deadlock: apply waits for mode 3000,
    // mode 3000 needs pops, pops wait on pending_apply. Froze the
    // viewer at q=395 with the stream healthy, 2026-06-11.)
    // Re-join guard (Phase F): a TCP-death re-JOIN makes the host re-ship
    // its cached snapshot. If our sim has already CONSUMED past the
    // snapshot's anchor, loading it would rewind the engine to the anchor
    // while the queue/cursor stay at the live edge -- corrupted playback.
    // Consumption position = receipt cursor minus the INPUTs still queued
    // (highest_consumed_frame is vestigial -- never updated). Strict >
    // so a fresh join (consumed == anchor, nothing popped yet) applies,
    // and a forward jump (anchor ahead of us, e.g. re-join landing in the
    // NEXT match) also applies.
    if (g_state.pb_snapshot_applied_once || g_state.pb_started ||
        g_state.natural_boot) {
        // WAVE 4 NARROWING -- the bounded deep-join path, and only it.
        //
        // A bounded deep joiner is natural_boot AND pb_started BY
        // CONSTRUCTION: it aborts /F, walks the anchored char-select live and
        // pops those CSS INPUTs. So the guard above rejects the one snapshot
        // that path exists to receive, which is why Wave 3.1 had to gate the
        // whole feature off.
        //
        // The exact test the guard was approximating is anchor equality:
        // applicable iff the snapshot's anchor == our CONSUMED INPUT position
        // (next_expected_frame minus the INPUTs still queued -- the same
        // arithmetic the pre-anchor trim below calls head_frame, now literally
        // the same function). That is true exactly when our sim sits at the
        // snapshot's logical frame having consumed nothing past it, i.e. the
        // battle-entry hold. It refuses backward rewinds (anchor < consumed --
        // the 2026-06-11 "battle restarted mid-stream") outright and can never
        // let a forward jump land (anchor > consumed -- the 0x40CD47
        // mismatched-char AV), so it is STRICTLY NARROWER than !pb_started
        // here, never wider. DeepJoinSnapshotVerdict also asserts char identity
        // against the MATCH_START header rather than trusting it.
        //
        // spec_deep_join is an ADDITIONAL belt, not the guarantee: it is set
        // only by a GRANT carrying SPEC_ACK_DEEP_JOIN, i.e. only for a viewer
        // the HOST decided to serve a bounded backfill. A from-frame-0 or
        // continuing viewer therefore cannot reach the admit even in the one
        // case where the rule alone would have said yes -- a stale re-ship
        // landing exactly on its cursor at a rematch boundary, which is a green
        // path today and must not start taking snapshots.
        //
        // WAIT is the third answer and it is load-bearing: the host pushes at
        // ITS battle entry, typically a second or two before our engine
        // finishes the pin walk, so the common case is a CORRECT snapshot that
        // is merely early. Keeping the inbox (rather than discarding and making
        // the viewer re-request) is what makes the healthy path cost one
        // transfer. PopFrameInputs' parked-snapshot freeze is exempted for this
        // walk (DeepJoinWalkingToAnchor) so keeping it cannot deadlock.
        //
        // PHASE 4c WIDENING -- the match-start pool resync, and only it. A
        // pool-resync viewer is in exactly the position the applicability rule
        // was written for: it sits at a battle entry having consumed nothing
        // past the host's snapshot anchor, and the blob it is being offered is
        // the one the host captured at THAT battle entry. The rule is what
        // makes the widening safe rather than the flag: anchor equality refuses
        // every rewind, the phase gate refuses every forward jump, and the
        // character-identity assert refuses the 0x40CD47 shape. What it repairs
        // is the object pool's slot assignment, which nothing in the event
        // stream carries and which no amount of simulation can re-derive (see
        // spec_pool_sync.cpp). PoolSync_Armed() is false for offline replay,
        // for a deep joiner, and for a CURRENT_MATCH joiner whose own join
        // snapshot has not landed yet, so no existing path changes.
        const uint32_t consumed = ConsumedInputPos();
        const DeepJoinVerdict verdict =
            (g_state.spec_deep_join || PoolSync_Armed())
                ? DeepJoinSnapshotVerdict(inbox)
                : DeepJoinVerdict::REFUSE;
        if (verdict == DeepJoinVerdict::WAIT) return;   // keep the inbox
        if (verdict != DeepJoinVerdict::APPLY) {
            // Re-join snapshots NEVER re-apply. Backward anchors would rewind
            // the sim under a live-edge queue; forward anchors (re-join lands
            // in a LATER match) would overwrite the char slots with the new
            // match's dynamic data while the locally loaded .player files are
            // still the old characters -- the sprite renderer reads mismatched
            // image descriptors and AVs (observed 2026-06-11, 0x40CD47, 100ms
            // after a fast-forward apply). Proper forward-jump support needs a
            // re-BTB (reload files for the announced chars) -- future work.
            //
            // A held deep joiner landing here is a STALE push (its battle moved
            // on, or the host re-armed for the next one): the inbox is dropped
            // but pb_deep_join_await stays set, so the hold survives and the
            // host's per-battle re-arm gets another chance.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: discarding re-join snapshot (anchor=%u, "
                "already initialized this session) -- continuing on the "
                "event stream (consumed=%u deep_join=%d hold=%d)",
                inbox.anchor_frame, consumed,
                (int)g_state.spec_deep_join, (int)g_state.pb_deep_join_await);
            inbox = State::SnapshotInbox{};
            return;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[%s] admitting snapshot past the re-join guard -- "
            "anchor=%u == consumed=%u and chars match, so this rewinds nothing "
            "(natural_boot=%d pb_started=%d applied_once=%d)",
            g_state.spec_deep_join ? "SPEC-DEEPJOIN" : "POOLSYNC",
            inbox.anchor_frame, consumed, (int)g_state.natural_boot,
            (int)g_state.pb_started, (int)g_state.pb_snapshot_applied_once);
    }

    const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    const uint32_t captured  = inbox.meta.captured_game_mode;
    const uint32_t apply_gate = (captured == 0u) ? 3000u : captured;
    if (game_mode < apply_gate) return;
    // task #60 ordering fix: the sim's first-3000-iteration init
    // (initial-sync + PIN_RNG) must run BEFORE the snapshot overlays --
    // in para's wild 2.82 session the apply landed first and the init
    // then CLOBBERED it (viewer replayed real tail inputs over a
    // fresh-boot battle = replay-style desync). Gating the apply on the
    // popped-frame init deadlocks instead (snapshot joins hold pops until
    // the apply -- the 2026-06-11 circle). So sequence it HERE at apply
    // time: init -> pin -> overlay, then mark THIS battle's entry init
    // consumed so the popped-frame path doesn't rerun it over the applied
    // state. One-shot: later battles' entries init normally.
    if (apply_gate >= 3000u) {
        extern bool g_spec_skip_next_battle_init;
        SaveState_DoInitialSync();
        SpectatorNode_ApplyPendingPinRng();
        g_spec_skip_next_battle_init = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: pre-apply init (initial-sync + PIN) sequenced "
            "at snapshot apply -- popped-frame entry init suppressed once");
    }

    if (!SaveState_LoadFromBytes(inbox.blob.data(), inbox.blob.size())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: SaveState_LoadFromBytes failed at deferred "
            "apply (match=%u, %zu bytes, mode=%u) -- discarding snapshot",
            inbox.meta.match_index, inbox.blob.size(), game_mode);
        inbox = State::SnapshotInbox{};
        return;
    }

    const uint32_t anchor = inbox.anchor_frame;

    // Anchor the EVENT_BATCH stream cursor at the snapshot's INPUT-frame
    // position. Subsequent batches start at this frame index -- see
    // SendSessionBackfillFromFrame on the host side.
    //
    // 2026-05-17 fix: don't reflexively clear pb_queue + reset
    // next_expected_frame. The CURRENT_MATCH bind path DOES pre-send
    // anchor-onward events (SendSessionBackfillFromFrame fires right
    // after SendSnapshotTo on the host), and live EVENT_BATCH broadcasts
    // continue flowing through the deferred-apply window. Those events
    // are EXACTLY what we want: pb_queue holds anchor..live, ready for
    // sim catch-up the moment snapshot applies. Clearing them wiped
    // ~750 events of valid backfill+live → spec stuck at expected=anchor
    // with all subsequent live batches "out-of-order" (host at frame
    // 1100+, spec expecting 333).
    //
    // Only reset state when our cursor is BEHIND the anchor -- that's the
    // grant-renegotiation case where pb_queue
    // holds stale pre-anchor frames the snapshot supersedes.
    if (!g_state.have_frame_baseline ||
        g_state.next_expected_frame < anchor)
    {
        g_state.pb_queue.clear();
        g_state.pb_match_headers.clear();
        // Drop reorder-buffered batches BELOW the anchor (superseded by the
        // snapshot); batches >= anchor stay and drain as the cursor reaches them.
        g_state.pb_reorder.erase(g_state.pb_reorder.begin(),
                                 g_state.pb_reorder.lower_bound(anchor));
        g_state.next_expected_frame = anchor;
    } else {
        // Cursor is at/past the anchor, so the branch above trusts pb_queue as
        // "anchor..live". That trust holds for a normal CURRENT_MATCH bind --
        // the host ships only the tail -- but NOT after a session-start ->
        // CURRENT_MATCH re-pin: the pre-subscribe stash is deliberately kept
        // across a re-JOIN to the same upstream, so the replayed queue can
        // still start at INPUT-frame 0 while live batches have already run the
        // cursor past the anchor. The test above then reads "caught up" and
        // keeps the whole from-frame-0 CSS backlog in front of a mid-battle
        // anchor; the viewer simulates those CSS frames as battle frames on
        // top of the snapshot (the hp=690/700 timer=1 bad head, off-205).
        //
        // Drop ONLY the strictly-pre-anchor prefix. pb_queue carries no frame
        // numbers -- INPUT events are positional, exactly one per frame -- so
        // the head frame is (next_expected_frame - queued INPUT count).
        //
        // INVARIANT this relies on: every writer that pushes an INPUT onto
        // pb_queue advances next_expected_frame by exactly one in the same
        // step, and non-INPUT ops never touch it. Maintained at the two live
        // admit sites, spec_recv.cpp's EVENT_BATCH path and its UDP
        // accelerator path. Do not add a third INPUT writer without keeping
        // the cursor in step, or this trim loses its frame reference. (The
        // invariant is now recorded once, on ConsumedInputPos in
        // spectator_node_internal.h, because Wave 4's applicability rule rests
        // on exactly the same arithmetic and the two must never drift.)
        //
        // We cut after the Nth INPUT where N = anchor - head_frame, which leaves the
        // anchor's own INPUT and every event after it untouched. Ops sitting
        // between the last dropped INPUT and the anchor's INPUT are KEPT (the
        // cut stops at the INPUT count, never past it), so the error direction
        // is always "keep", never "lose". Nothing at or after the anchor can
        // be dropped by construction.
        //
        // COMPOSITION WITH THE DEEP-JOIN APPLY: that apply requires
        // head_frame == anchor, so `head_frame < anchor` is false and this trim
        // is a no-op there. The two are the same arithmetic reaching the same
        // answer, not two policies that have to be reconciled.
        const uint32_t head_frame = ConsumedInputPos();
        if (head_frame < anchor) {
            const uint32_t skip = anchor - head_frame;
            size_t cut = 0, seen = 0;
            while (cut < g_state.pb_queue.size() && seen < (size_t)skip) {
                if (g_state.pb_queue[cut].type == SessionEventType::INPUT) ++seen;
                ++cut;
            }
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: dropping %zu pre-anchor event(s) (%u INPUT "
                "frame(s) from head=%u) ahead of snapshot anchor=%u -- a "
                "from-frame-0 backlog survived a CURRENT_MATCH re-pin; %zu "
                "event(s) at/after the anchor kept",
                cut, skip, head_frame, anchor, g_state.pb_queue.size() - cut);
            g_state.pb_queue.erase(g_state.pb_queue.begin(),
                                   g_state.pb_queue.begin() +
                                       static_cast<std::ptrdiff_t>(cut));
        }
    }
    g_state.have_frame_baseline    = true;
    g_state.highest_consumed_frame = 0;
    g_state.playing_back           = true;
    g_state.pb_snapshot_applied_once = true;
    // Snapshot supersedes any in-flight boundary state (e.g. a stale SEAM
    // from a renegotiated join) -- the restored state IS the new baseline.
    g_state.pb_boundary         = State::PbBoundary::NONE;
    g_state.pending_reset_input = false;
    g_state.pending_sound_init  = false;
    // A CSS-landing snapshot (first join while the host is NAVIGATING char
    // select) drops us into mode 2000 with the parked cursor at char 0. If we
    // clear the confirm mask here, the engine's CSS edge detector reads the
    // first replayed input's carried confirm bit (0x10..0x200) as a RISING
    // confirm for both players at char 0 -> the "Ryu/Ryu 0/0 confirm flash then
    // un-confirm" on first join. The natboot mask (PopFrameInputs) only arms on
    // the viewer's FIRST mode>=2000, which a boot-then-snapshot viewer already
    // passed, so it never fires here. Re-arm the short confirm mask (same 10-pop
    // window the lean-seam path uses; auto-released by the pb_post_css_mask_pops
    // countdown) so that first edge is eaten. A battle-landing snapshot has no
    // CSS edge to eat -- clear any stale seam as before.
    if (game_mode >= 2000u && game_mode < 3000u) {
        g_state.pb_post_css_mask_pops = 10;
        CssAutoConfirm_SetSeamHold(true);   // mask confirm bits only
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: CSS-landing snapshot -- re-armed confirm mask "
            "(10 pops) to eat the first-input rising-confirm (0/0 flash fix)");
    } else {
        CssAutoConfirm_SetSeamHold(false);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: SNAPSHOT applied (match=%u, %zu bytes) -- "
        "anchor INPUT-frame=%u, local game_mode=%u",
        inbox.meta.match_index, inbox.blob.size(),
        anchor, game_mode);

    // Wave 4: releases the battle-entry hold, clears the deep-join latch (this
    // viewer is bit-exact from here and continues by simulation) and tells the
    // host to stop pushing. No-op for every other join shape.
    DeepJoinOnSnapshotApplied(anchor);
    // Phase 4c: releases the bounded pool-resync hold and latches "a snapshot
    // has landed on this viewer" (which is what promotes a CURRENT_MATCH joiner
    // out of its join flow). Called for EVERY apply, deliberately.
    PoolSync_OnSnapshotApplied(anchor);

    inbox = State::SnapshotInbox{};
}

SpectatorSnapshotInfo SpectatorNode_GetSnapshotInfo() {
    SpectatorSnapshotInfo out = {};
    if (!g_state.current_snapshot.valid) return out;
    out.input_frame = g_state.current_snapshot.input_frame;
    out.match_index = g_state.current_snapshot.match_index;
    out.total_bytes = (uint32_t)g_state.current_snapshot.blob.size();
    out.checksum    = g_state.current_snapshot.checksum;
    return out;
}


// -----------------------------------------------------------------------------
// SPECTATOR-SIDE OP APPLY (C3)
// -----------------------------------------------------------------------------
//
// Called from PopFrameInputs head-drain loop. Each non-INPUT event at the
// head dispatches here before the next INPUT pops; the local memory write
// happens at the same logical frame the host's write happened, eliminating
// the game_mode-driven mirror race that lived in CheckGameModeTransition.


void SpectatorNode_LeaveUpstream() {
    if (!g_state.subscribed_upstream) return;
    CtrlPacket leave = {};
    leave.header.type = CtrlMsg::SPEC_LEAVE;
    ControlChannel_SendTo(leave, g_state.upstream_addr);
    g_state.subscribed_upstream = false;
}

void SpectatorNode_SetRootAddr(const sockaddr_in& root) {
    g_state.root_addr = root;
}

// Periodic health tick. Three jobs:
//   1. Heartbeat to current upstream every HEARTBEAT_INTERVAL_MS -- lets
//      upstream's expiry sweep know we're still alive (otherwise it'd
//      drop us after SUBSCRIBER_EXPIRY_MS of silence).
//   2. Failover on silence: if no inbound from upstream for
//      SILENCE_FAILOVER_MS, assume upstream died. Send SPEC_LEAVE
//      (best-effort) to current upstream, then re-RequestJoin against
//      root_addr. Throttle reconnect attempts to once per RECONNECT_BACKOFF.
//   3. Upstream-side sweep: drop subscribers that haven't sent a
//      heartbeat in SUBSCRIBER_EXPIRY_MS (notify via SPEC_LEAVE so they
//      fail over instantly instead of waiting for their own silence
//      timer).
//
// Cheap to call every iter; the work is gated on the time deltas.
