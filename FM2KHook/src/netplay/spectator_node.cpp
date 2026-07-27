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
// EVENT_BATCH stream; late joiners get it via SendSessionBackfillTo.


// Legacy SendMatchEndToAll / MATCH_END packet path removed in C12.
// MATCH_END flows as a SessionEvent op (see SpectatorNode_AppendMatchEnd).

// (SendInputRequest + RespondToInputRequest deleted: TCP guarantees in-order
// delivery exactly once, so the spectator-side gap-recovery handshake is
// dead code. The whole class of UDP-loss recovery — REDUNDANCY_WINDOW,
// INPUT_REQUEST_POLL_MS in TickHealth, the on-gap immediate request inside
// HandleSpecData — has been removed.)

} // namespace

// =============================================================================
// SESSION EVENT WIRE FORMAT (C1)
// =============================================================================
//
// Pure byte-level encoders/decoders. No socket / state side effects — these
// just mediate between SessionEvent values and packed wire bytes. Production
// integration (vector<SessionEvent> session_history, head-of-queue drain in
// RunSpectatorTick, etc.) is layered on top in C2+.


// =============================================================================
// PUBLIC API
// =============================================================================

void SpectatorNode_Init() {
    g_state = State{};
    g_state.capacity = SPECTATOR_DEFAULT_CAPACITY;

    // Wire the ReliableChannel deliver dispatcher for the FM2K_SPEC_RC A/B path
    // (harmless if nothing ever arrives on RC_CHAN_SPEC). Runs regardless of
    // transport mode below.
    SpectatorNode_RegisterRcDeliver();

    // FM2K_SPEC_TRANSPORT (Phase 2b of v0.3 spec rebuild). "relay" =
    // route spec data through hub WS binary frames instead of P2P TCP.
    // When set, we skip the entire TCP-listener + TCP-STUN dance below
    // -- no listener to bind, no external port to discover, no NAT
    // punch dance to coordinate. Spec data plane comes online once the
    // launcher's shared-mem queue is wired (Phase 2c).
    //
    // Default "tcp" preserves legacy behavior for every client up
    // through v0.2.57. Read once here at Init -- env changes mid-run
    // wouldn't be safe (existing subscribers expect one mode).
    if (const char* transport = std::getenv("FM2K_SPEC_TRANSPORT");
        transport && std::strcmp(transport, "relay") == 0) {
        g_state.spec_transport_relay = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: FM2K_SPEC_TRANSPORT=relay -- skipping TCP "
            "listener + TCP-STUN, creating shared-mem queues for "
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

    // Bring up the TCP listener for the host→spectator INPUT_BATCH stream.
    //
    // Bind strategy is tiered because Windows reserves chunks of the
    // dynamic port range for Hyper-V / WSL2 / docker / etc., and the
    // reserved chunks shift across reboots. Three attempts in order:
    //
    //   1. UDP_port + 100  (deterministic, gives predictable port for
    //      debugging when it succeeds)
    //   2. UDP_port + 1000 (different bucket; usually clear of the
    //      WSL-reserved range)
    //   3. port = 0        (OS picks any free port; always succeeds)
    //
    // JOIN_ACK includes our actual listener port via
    // SpectatorTCP::GetListenPort(), so spectators learn whichever port
    // we ended up on with no client-side coordination. The deterministic
    // attempts are first only to keep logs readable; the OS-picked
    // fallback is what guarantees we never come up listener-less.
    //
    // Real-world repro on pkmncc 2026-05-13: P1 udp=51376 → +100=51476
    // failed with WSAEACCES (Windows-reserved range), needed +1000 OR
    // port=0 to bind successfully.
    //
    // Idempotent — re-Init won't double-bind.
    const uint16_t udp_port = NetSocket_GetLocalPort();
    const uint16_t candidate_ports[] = {
        (uint16_t)(udp_port + 100),
        (uint16_t)(udp_port + 1000),
        0,  // OS picks
    };
    bool spec_listener_bound = false;
    for (uint16_t cand : candidate_ports) {
        if (SpectatorTCP::StartListener(cand)) {
            spec_listener_bound = true;
            if (cand == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SpectatorNode: TCP listener bound via OS-picked port "
                    "(actual port %u — preferred offsets all hit "
                    "WSAEACCES, usually Windows reserved-port range)",
                    (unsigned)SpectatorTCP::GetListenPort());
            }
            break;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: TCP listener bind on port %u failed — "
            "trying next candidate", (unsigned)cand);
    }
    if (!spec_listener_bound) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: Init — TCP listener failed to bind on ANY "
            "candidate port (udp=%u, +100, +1000, OS-picked all failed); "
            "spectator subscriptions will fail",
            (unsigned)udp_port);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SpectatorNode: Init (capacity=%zu, batch=%zu frames, "
                "tcp_port=%u)",
                g_state.capacity, BROADCAST_BATCH_FRAMES,
                (unsigned)SpectatorTCP::GetListenPort());

    // TCP-STUN — discover external TCP addr by source-binding an outbound
    // connect to (hub:tcp_stun) from our listener port. Only meaningful
    // for spectators (where the cross-NAT punch path needs the *external*
    // tcp_port for the host-side punch to fire at the right port). Hosts
    // run this too for symmetry — costs ~50ms once and pre-populates
    // the field if the host ever later acts as a spectator (daisy chain).
    // Failure here is logged but non-fatal; punching falls back to local
    // listener port which works on port-preserving NATs.
    SpectatorTCP::PerformTcpStun();
}

void SpectatorNode_Shutdown() {
    // C7: write full session log on shutdown if there's anything to flush.
    // Skipped on the spectator side where session_events is the relay log
    // (correct to write — the relay's local view IS the canonical session
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
    g_state.ops_seen               = 0;
    g_state.udp_epoch_armed        = false;
    g_state.udp_highest_op_seq     = 0;
    g_state.udp_admitted_total     = 0;
    g_state.udp_paused_on_gate     = 0;
    g_state.udp_dropped_on_gap     = 0;
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
    SpectatorTCP::Shutdown();
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
// logical instant the host emitted MATCH_START — before any battle
// frame has run, so the spectator's state on Load is "match just
// starting, frame 0 input pending." That keeps the wire-anchor clean:
// snapshot.input_frame == g_state.total_input_count, and the very next
// INPUT event the host appends becomes the spectator's first popped
// frame after Load.

void SpectatorNode_StashSnapshot() {
    // Skip during a rollback rewind — the FM2K state at this moment isn't
    // the canonical battle-start state we want to capture. In practice
    // StashSnapshot is called from Netplay_StartBattle which is the seam
    // frame between CSS and battle (no preceding battle inputs to roll
    // back), so g_is_rolling_back should never be true here. Belt-and-
    // suspenders: if a future caller invokes us mid-battle, this gate
    // prevents handing spectators a partially-rewound state. Match cache
    // stays as the previous match's (or empty if first match).
    if (g_is_rolling_back) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: StashSnapshot skipped — called during rollback "
            "rewind (snapshot cache stays %s)",
            g_state.current_snapshot.valid ? "previous match's" : "empty");
        return;
    }

    // Force a Save to populate the rollback buffer's slot 0 with the
    // current FM2K state. Sets g_initial_sync_done in savestate.cpp;
    // GekkoNet's later first-Save is a no-op for the initial-sync reset.
    if (!SaveState_Save(0)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: StashSnapshot — SaveState_Save(0) failed; "
            "snapshot cache stays %s",
            g_state.current_snapshot.valid ? "the previous match's" : "empty");
        return;
    }

    const uint8_t* slot_bytes = SaveState_PeekLastSavedSlotBytes();
    const size_t   slot_size  = SaveState_GetSlotByteSize();
    if (!slot_bytes || slot_size == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: StashSnapshot — slot bytes unavailable post-Save");
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
    // Phase E: record game_mode at capture time so the spec-side apply
    // can wait for a matching mode. CSS captures get applied during the
    // spec's CSS; battle captures wait for battle.
    cache.captured_game_mode = *(const uint32_t*)FM2K::ADDR_GAME_MODE;
    cache.valid       = true;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: snapshot cached (match=%u, %zu bytes, "
        "input_frame=%u, fletcher32=0x%08X, captured_game_mode=%u)",
        cache.match_index, cache.blob.size(),
        cache.input_frame, cache.checksum, cache.captured_game_mode);

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
    if (!inbox.pending_apply) return;

    // Wait until the spectator's local engine has reached the SAME
    // phase the snapshot was captured at. The savestate captures the
    // engine state — object pool, character data, DDraw surfaces, etc.
    // — and applying it before the local engine has done its own
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
        // Re-join snapshots NEVER re-apply. Backward anchors would rewind
        // the sim under a live-edge queue; forward anchors (re-join lands
        // in a LATER match) would overwrite the char slots with the new
        // match's dynamic data while the locally loaded .player files are
        // still the old characters -- the sprite renderer reads mismatched
        // image descriptors and AVs (observed 2026-06-11, 0x40CD47, 100ms
        // after a fast-forward apply). Proper forward-jump support needs a
        // re-BTB (reload files for the announced chars) -- future work.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: discarding re-join snapshot (anchor=%u, "
            "already initialized this session) -- continuing on the "
            "event stream",
            inbox.anchor_frame);
        inbox = State::SnapshotInbox{};
        return;
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
            "apply (match=%u, %zu bytes, mode=%u) — discarding snapshot",
            inbox.meta.match_index, inbox.blob.size(), game_mode);
        inbox = State::SnapshotInbox{};
        return;
    }

    const uint32_t anchor = inbox.anchor_frame;

    // Anchor the EVENT_BATCH stream cursor at the snapshot's INPUT-frame
    // position. Subsequent batches start at this frame index — see
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
    // Only reset state when our cursor is BEHIND the anchor — that's the
    // FULL_SESSION → CURRENT_MATCH renegotiation case where pb_queue
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
        CssAutoConfirm_SetSeamHold(true, 0xFF, 0xFF);  // mask confirm bits only
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SpectatorNode: CSS-landing snapshot -- re-armed confirm mask "
            "(10 pops) to eat the first-input rising-confirm (0/0 flash fix)");
    } else {
        CssAutoConfirm_SetSeamHold(false);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SpectatorNode: SNAPSHOT applied (match=%u, %zu bytes) — "
        "anchor INPUT-frame=%u, local game_mode=%u",
        inbox.meta.match_index, inbox.blob.size(),
        anchor, game_mode);

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
//   1. Heartbeat to current upstream every HEARTBEAT_INTERVAL_MS — lets
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
