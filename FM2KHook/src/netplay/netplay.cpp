// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
#include "netplay.h"
#include "netplay_internal.h"  // shared file-scope state, externed for the split netplay_*.cpp TUs
#include "../hooks/hooks.h"   // Hook_ApplySOCD_Public for SOCD-pre-apply on spec capture
#include "../hooks/css_autoconfirm.h"  // CssAutoConfirm_OnReplayMatchStart (TEST_CSS_CHAR pin)
#include "control_channel.h"
#include "control_channel_internal.h"  // g_gekko_rx_* stamps for [BEAT]/[WEDGE] (#56)
#include "host_clock.h"
#include "game_hash.h"
#include "input.h"
#include "savestate.h"
#include "spectator_node.h"
#include "nat_traversal.h"
#include "upload_queue.h"
#include "globals.h"
#include "gekkonet.h"
#include "../audio/sound_rollback.h"
#include "../ui/shared_mem.h"  // SharedMem_PublishMatchOutcome
#include "../parity/parity_recorder.h"  // ParityRecorder::Close on harness auto-terminate
#include <SDL3/SDL_log.h>
#include <ws2tcpip.h>
#include <cstdlib>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ctime>
#include <random>
#include <cstdio>
#include <cstring>
#include <atomic>

// =============================================================================
// SIMPLIFIED STATE
// =============================================================================

// SimpleState enum now lives in netplay_internal.h (shared with the split TUs).
SimpleState g_simple_state = SimpleState::DISCONNECTED;

static NetplayState MapToLegacyState(SimpleState s) {
    switch (s) {
        case SimpleState::DISCONNECTED: return NetplayState::DISCONNECTED;
        case SimpleState::CONNECTED: return NetplayState::CSS_LOBBY;
        case SimpleState::BATTLE: return NetplayState::BATTLE_RUNNING;
        default: return NetplayState::DISCONNECTED;
    }
}

// =============================================================================
// SESSION TRACKING
// =============================================================================
//
// What kind of GekkoNet session g_session currently points at. We run two
// distinct sessions back-to-back for a single match: a CSS lockstep session
// (input_prediction_window=0, no rollback) and a battle rollback session
// (prediction_window=8 + runahead). The kind determines the per-tick driver
// and the swap-frame protocol direction. SessionKind is an alias for the
// public NetplaySessionKind enum exported from netplay.h so external callers
// (spectator_node, hub_client) can compare against the same values.

// SessionKind alias now lives in netplay_internal.h (shared with the split TUs).
SessionKind g_session_kind = SessionKind::NONE;

// CSS lockstep parameters (ported from the legacy CCCaster-style ring-buffer
// implementation). With GekkoNet's prediction=0 mode, local_delay is the
// per-player input commitment delay -- peer A's input committed at frame F
// becomes the input applied at frame F+local_delay, identical semantics to
// the previous "store at frame+delay, read at frame" model.

// CSS state -- input transport now lives inside the CSS GekkoSession
bool g_css_active        = false;  // Currently in CSS mode (game-side detection)
bool g_css_synced        = false;  // Both peers BATTLE_READY, CSS GekkoSession ready
bool g_remote_css_ready  = false;  // Remote has entered CSS
bool g_local_css_ready   = false;  // We've entered CSS
uint32_t g_css_frame     = 0;      // Last confirmed CSS AdvanceEvent frame (+1)

// Per-poll AdvanceEvent input cache. Netplay_ProcessCSS drives gekko_update_session,
// which fires AdvanceEvent only when both peers have inputs for the current frame
// (lockstep guarantee). The cached inputs are read by Hook_GetPlayerInput via
// Netplay_GetCSSInput.
uint16_t g_css_advance_p1     = 0;
uint16_t g_css_advance_p2     = 0;
bool     g_css_advance_ready  = false;

// GekkoNet session pointer + readiness flag (shared by CSS and battle --
// only one is alive at a time, distinguished by g_session_kind).
GekkoSession* g_session = nullptr;
bool g_session_ready = false;
uint16_t g_p1_input = 0;
uint16_t g_p2_input = 0;
uint32_t g_netplay_frame = 0;

// Rollback tracking
uint32_t g_rollback_count = 0;
uint32_t g_last_rollback_frame = 0;
uint32_t g_desync_count = 0;
uint32_t g_last_desync_log_tick = 0;
int g_local_delay = 1;  // Computed from RTT at battle start

// Tuning knobs -- set at battle session start, mirrored here so
// Netplay_TickHeartbeat can echo the live values into [BEAT] lines.
// `g_runahead_user_pref` is the configured "on" value (env / future
// UI default); `g_runahead_active` is the value actually applied to
// the running session right now, which can differ when the user
// hits F8 mid-match to toggle between 0 and user_pref.
int                g_pred_window               = 16;
// Runahead is DEFAULT OFF (too CPU-heavy at 100fps/10ms budget). These get
// overwritten per battle in Netplay_StartBattle (pref = local_delay, active
// = 0 unless FM2K_RUNAHEAD forces it); the static defaults just keep it off
// before/between matches.
int                g_runahead_user_pref        = 0;
std::atomic<int>   g_runahead_active{0};
std::atomic<bool>  g_runahead_toggle_requested{false};

// --- re-sim profiler (FM2K_PERF_PROFILE=1) ---------------------------------
// Times the three hot ops in the rollback loop so we can see where the 10ms
// (100 FPS) per-frame budget actually goes: SaveState_Save, SaveState_Load,
// and the game tick (process_game_inputs + update_game). Reports avg microsec
// + count every 500 advances. Off unless the env var is set, so production
// builds pay nothing. (#62/#63)
// PerfBucket/PerfScope/PerfNowNs/PerfQpcFreq now live in netplay_internal.h
// (shared with the split battle-phase TU). This TU owns the data definitions.
const bool g_perf_on = [] {
    const char* v = std::getenv("FM2K_PERF_PROFILE");
    return v && v[0] && v[0] != '0';
}();
PerfBucket g_perf_save, g_perf_load, g_perf_adv;
void PerfMaybeReport() {
    if (!g_perf_on) return;
    static uint32_t ticks = 0;
    if (++ticks % 500 != 0) return;
    auto us = [](const PerfBucket& b) { return b.n ? (double)b.ns / b.n / 1000.0 : 0.0; };
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[PERF] save n=%u avg=%.1fus | load n=%u avg=%.1fus | tick(PGI+UG) n=%u avg=%.1fus "
        "(budget is 10000us/frame at 100fps)",
        g_perf_save.n, us(g_perf_save), g_perf_load.n, us(g_perf_load),
        g_perf_adv.n, us(g_perf_adv));
}

// Rolling window for [BEAT] line. Reset every emit so the per-window
// avg + max numbers describe the most recent ~10s, not session totals.
uint32_t g_beat_window_rb_sum   = 0;
uint32_t g_beat_window_rb_max   = 0;
uint32_t g_beat_window_rb_count = 0;   // real rollbacks observed
uint64_t g_beat_last_emit_ms    = 0;


// Synthetic desync trigger -- checks FM2K_FORCE_DESYNC_AT_FRAME env
// var once at hook init. When the netplay frame counter reaches the
// configured value, calls HandleDesyncDetected with synthetic flag
// set. Both peers receive the same env var (the launcher inherits
// it), both fire at the same frame, both end up with the same
// match_token-derived match_id -- perfect smoke test for the upload
// + cross-peer pairing pipeline.
//
// Set to -1 (default) to disable.
int g_force_desync_at_frame = -1;
bool g_force_desync_inited = false;

void MaybeFireSyntheticDesync() {
    if (!g_force_desync_inited) {
        g_force_desync_inited = true;
        const char* e = std::getenv("FM2K_FORCE_DESYNC_AT_FRAME");
        if (e && *e) {
            g_force_desync_at_frame = std::atoi(e);
            if (g_force_desync_at_frame > 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SYNTHETIC-DESYNC armed: will fire at battle frame %d "
                    "(env FM2K_FORCE_DESYNC_AT_FRAME)",
                    g_force_desync_at_frame);
            }
        }
    }
    if (g_force_desync_at_frame > 0 &&
        (int)g_netplay_frame >= g_force_desync_at_frame) {
        const int target = g_force_desync_at_frame;
        g_force_desync_at_frame = -1;  // one-shot
        HandleDesyncDetected(target, 0xDEADBEEFu, 0xCAFEBABEu,
                             /*synthetic=*/true);
    }
}

// Connection-lifetime cache for the computed (auto) battle delay. Pinned
// at the FIRST battle-session start after CONNECTED and reused for every
// subsequent CSS->battle transition until disconnect. Without this we
// recomputed per-battle off whatever stale rtt_worst happened to be sitting
// in the bucket -- a single CSS spike would slam delay to 15 on a 6 ms link.
// Cleared on Netplay_Shutdown / Netplay_OnDisconnect so a reconnect picks
// a fresh value next time.
int  g_session_delay_cached       = 0;
bool g_session_delay_cache_valid  = false;

// Highest frame number we've ever recorded into the replay/spectator stream.
// Reset on each Netplay_StartBattle (g_netplay_frame also resets to 0).
// Gates the GekkoAdvance recording path against runahead duplicates -- each
// frame is advanced multiple times under runahead, but only the first
// monotonic forward crossing is "the" confirmed advance.
uint32_t g_highest_recorded_frame = 0;

// Confirmed-input recording ring (netplay battle sessions only). EVERY
// non-runahead advance -- speculative first pass AND rolling_back
// correction -- writes (frame, post-SOCD inputs) here, corrections
// overwriting predictions. The flush after the event loop emits entries
// to SpectatorNode_OnFrameConfirmed only once gekko_confirmed_frame()
// says ALL players' real inputs arrived for that frame. Without this,
// a predicting peer recorded speculative inputs into .fm2krep and the
// live spectator stream (cross-peer fork hunt, 2026-06-11).
// PendingConfirmInput struct now lives in netplay_internal.h.
PendingConfirmInput g_pending_confirm[PENDING_CONFIRM_RING];
uint32_t g_next_confirm_flush = 0;
void ResetConfirmRing() {
    for (auto& pc : g_pending_confirm) pc.frame = 0xFFFFFFFFu;
    g_next_confirm_flush = 0;
}

// Battle entry sync barrier - ensures both clients enter battle at same time.
// Also carries swap_frame negotiation for the CSS-session->battle-session swap:
// both peers propose g_css_frame + SWAP_FRAME_BUFFER on detection, exchange via
// BATTLE_ENTERING.data.sync.frame, and the agreed value is max(local, remote).
// The actual gekko_destroy(css)/gekko_create(battle) deferred until the active
// CSS session reaches g_battle_entry_swap_frame.
bool     g_local_battle_entered    = false;
bool     g_remote_battle_entered   = false;
bool     g_battle_synced           = false;
uint32_t g_battle_entry_swap_frame = 0;     // Latest agreed swap frame.

// "Are we expecting BATTLE_ENTERING right now?" gate. Set when a new CSS
// GekkoSession comes up (we're entering the CSS phase that will swap to
// battle); cleared once we've started the battle session. Without this,
// stale BATTLE_ENTERING packets from a previous match -- sent during that
// match's CSS phase but delayed in flight or kernel-buffered, arriving
// 100–200ms AFTER the previous match's Netplay_EndBattle reset us -- get
// blindly accepted and pre-poison g_battle_entry_swap_frame for the new
// match. Symptom: every subsequent match's "BATTLE SYNC: both peers
// signaled" line shows the SAME stale swap_frame, the BATTLE_ENTERING
// echo loop never terminates (both peers latch g_local_battle_entered),
// and eventually the battle GekkoNet sync stalls in one direction
// → black screen on CSS->battle transition. See logs from 2026-05-09.
bool     g_battle_entry_armed      = false;

// Mirror gate for BATTLE_END to prevent the same stale-packet-poisoning
// pattern in the battle->CSS direction.
//
// Battle exit sync barrier (battle-session -> CSS-session swap, for rematch
// or return-to-menu). Mirrors the entry barrier but reads g_netplay_frame
// instead of g_css_frame and is driven by BATTLE_END instead of BATTLE_ENTERING.
bool     g_local_battle_end_signaled  = false;
bool     g_remote_battle_end_signaled = false;
bool     g_battle_end_synced          = false;
uint32_t g_battle_end_swap_frame      = 0;
bool     g_battle_end_armed           = false;

// Barrier epoch + completion records (deafness fix, 2026-06-11). The old
// protocol had a fatal asymmetry under loss: peer B completes the barrier
// the instant it has A's signal, swaps sessions, and disarms ~30ms later.
// If B's OWN signal packets were all lost in flight, A keeps resending
// into a peer that now drops everything at the armed gate without
// answering -- A wedges in the barrier indefinitely (observed: 593
// BATTLE_ENTERING resends over 36s). With B's signal sent effectively
// once or twice in the short window, that's roughly a coin-flip per
// barrier at 20% loss. Fix: a peer that COMPLETED a barrier keeps
// ANSWERING retries for that barrier instance (identified by epoch)
// with a completed-flag signal, so the lagging peer can always finish.
// The epoch tag replaces "armed window" as the stale-vs-current
// disambiguator for these answers: both peers arm entry/end barriers in
// the same order (CSS-session-up / battle-session-up are bilateral
// rendezvous), so a uint8 counter stays in step; 0 is reserved for
// legacy peers and treated as a wildcard.
uint8_t  g_barrier_epoch        = 0;  // last assigned instance id
uint8_t  g_entry_epoch          = 0;  // armed entry barrier instance
uint8_t  g_end_epoch            = 0;  // armed end barrier instance
uint8_t  g_entry_done_epoch     = 0;  // last COMPLETED entry instance
uint8_t  g_end_done_epoch       = 0;  // last COMPLETED end instance
uint32_t g_entry_done_ms        = 0;  // completion wall-clock (legacy TTL)
uint32_t g_end_done_ms          = 0;
// Proposal pair captured for the divergence diagnostic at completion.
uint32_t g_entry_local_proposal  = 0;
uint32_t g_entry_remote_proposal = 0;
// Peer's last-announced match-settings digest (BATTLE_ENTERING.cfg_digest).
// 0 = never announced -> the settings gate stays open. Cleared wherever the
// entry barrier is armed or reset so one match's agreement can never satisfy
// the next match's gate.
uint32_t g_entry_remote_cfg_digest = 0;

uint8_t NextBarrierEpoch() {
    ++g_barrier_epoch;
    if (g_barrier_epoch == 0) g_barrier_epoch = 1;  // 0 = legacy wildcard
    return g_barrier_epoch;
}

// Frames of slack added to the proposed swap_frame so both peers have time
// to drain in-flight inputs and converge their proposals before reaching it.
// 8 @ 100 FPS = 80ms -- comfortably above typical RTT. Tunable.

// Handshake state
bool g_received_hello = false;
bool g_received_hello_ack = false;

// =============================================================================
// Note: We use our own frame counter instead of game timer
// The game timer at 0x470044 appears to be 0 during CSS
// =============================================================================

// =============================================================================
// CONTROL MESSAGE HANDLER
// =============================================================================

// Forward decls for SOCD-mode helpers in hooks.cpp (file-scope so all
// uses below -- Netplay_BroadcastHostConfig + the HOST_CONFIG receiver --
// can reach them).
extern "C" int  Hook_GetSOCDModePublic();
extern "C" void Hook_SetSOCDMode(int mode);

// Random-stage handoff to the FM95 LoadStageFile_alt hook. Set when the
// xorshift block in Netplay_StartBattleSession produces a fresh roll;
// read by Hook_LoadStageFileAlt to override the function's arg0. 0xFFFFFFFF
// means "random not enabled / no override" -- hook passes the original
// arg0 through unchanged. FM2K doesn't read this -- its ADDR_SELECTED_STAGE
// write goes to the canonical 0x43010c which the game reads natively.
uint32_t g_pending_random_stage = 0xFFFFFFFFu;
extern "C" uint32_t Netplay_PeekNextRolledStage() {
    return g_pending_random_stage;
}


// =============================================================================
// LIFECYCLE
// =============================================================================

bool Netplay_Init(int player_index, uint16_t local_port, const char* remote_addr) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Netplay: Init player=%d port=%d remote=%s",
        player_index, local_port, remote_addr);

    g_player_index = player_index;
    g_simple_state = SimpleState::DISCONNECTED;
    g_session = nullptr;
    g_session_ready = false;
    g_netplay_frame = 0;
    g_p1_input = 0;
    g_p2_input = 0;
    fm2k::hostclock::Reset();   // fresh clock offset + rift window per session

    // Reset CSS state -- input transport now lives in the CSS GekkoSession,
    // so the legacy ring-buffer fields are gone. The session itself is
    // created on first BATTLE_READY rendezvous and torn down on battle entry.
    g_session_kind      = SessionKind::NONE;
    g_css_frame         = 0;
    g_css_advance_p1    = 0;
    g_css_advance_p2    = 0;
    g_css_advance_ready = false;
    g_css_active = false;
    g_css_synced = false;
    g_remote_css_ready = false;
    g_local_css_ready = false;

    // Reset battle sync state (entry direction)
    g_local_battle_entered    = false;
    g_remote_battle_entered   = false;
    g_battle_synced           = false;
    g_battle_entry_swap_frame = 0;
    g_battle_entry_armed      = false;
    g_entry_remote_cfg_digest = 0;

    // Reset battle sync state (exit direction, for next return-to-CSS)
    g_local_battle_end_signaled  = false;
    g_remote_battle_end_signaled = false;
    g_battle_end_synced          = false;
    g_battle_end_swap_frame      = 0;
    g_battle_end_armed           = false;

    // Fresh connection -- restart the barrier epoch sequence on both sides.
    // (Netplay_EndBattle deliberately does NOT touch these: the completion
    // records must survive into the next CSS so a lagging peer's barrier
    // retries still get answered.)
    g_barrier_epoch    = 0;
    g_entry_epoch      = 0;
    g_end_epoch        = 0;
    g_entry_done_epoch = 0;
    g_end_done_epoch   = 0;
    g_entry_done_ms    = 0;
    g_end_done_ms      = 0;
    NetSocket_ClearGekkoActorPin();  // fresh connection = fresh actor identity

    // Reset handshake
    g_received_hello = false;
    g_received_hello_ack = false;

    // Store network config
    g_local_port = local_port;
    strncpy(g_remote_addr, remote_addr, sizeof(g_remote_addr) - 1);

    // Initialize control channel
    ControlChannel_SetCallback(OnControlMessage);

    // Delay-negotiation mode (#24). 0 = avg ping (mean RTT), 1 = peak
    // ping (worst RTT). Picked by the launcher's Delay combo; absent
    // env defaults to avg. Drives ControlChannel_GetLocalDelayCandidate.
    if (const char* env = std::getenv("FM2K_DELAY_MODE"); env && env[0]) {
        ControlChannel_SetDelayMode(std::atoi(env));
    } else {
        ControlChannel_SetDelayMode(0);
    }

    if (!NetSocket_IsInitialized()) {
        if (!NetSocket_Init(local_port, remote_addr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Failed to init socket");
            return false;
        }
    }

    // Initialize spectator tree node AFTER NetSocket -- its TCP listener
    // binds to the UDP socket's port number (TCP/UDP share port space).
    SpectatorNode_Init();

    // Hub-driven NAT traversal. If FM2K_HUB_* env vars are present,
    // the launcher started this match via the hub -- fire a STUN probe
    // (so the hub can reflect our public mapping) and start the
    // peer-to-peer burst-punch using the supplied match_token. If the
    // env vars aren't set, this match was a legacy direct-connect:
    // we skip and rely on the existing 0xCC HELLO loop alone.
    {
        const char* hub_udp     = std::getenv("FM2K_HUB_UDP_ADDR");
        const char* hub_user_id = std::getenv("FM2K_HUB_USER_ID");
        const char* match_tok   = std::getenv("FM2K_HUB_MATCH_TOKEN");

        if (hub_udp && hub_user_id) {
            ::fm2k::nat::SendStunProbe();
        }

        // Discover + advertise our own global IPv6 endpoint (independent of the
        // v4 hub STUN -- v6 needs no reflection). When both peers have global v6
        // this opens a DIRECT, NAT-free, relay-free path; the burst already
        // punches the peer's v6 candidate (StartPunch), this supplies OUR half.
        ::fm2k::nat::DiscoverAndPublishLocalV6();

        // Read relay env vars early so the post-burst fallback in
        // StartPunch's worker thread can engage relay mode the
        // moment direct punch fails.
        ::fm2k::nat::ConfigureRelay();

        if (match_tok && remote_addr && *remote_addr) {
            // Decode the 32-hex-char match token into 16 binary bytes.
            uint8_t token_bytes[16] = {};
            size_t hex_len = std::strlen(match_tok);
            if (hex_len > 32) hex_len = 32;
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            for (size_t i = 0; i + 1 < hex_len; i += 2) {
                int hi = nibble(match_tok[i]);
                int lo = nibble(match_tok[i + 1]);
                if (hi < 0 || lo < 0) break;
                token_bytes[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
            }

            // Parse "ip:port" from FM2K_REMOTE_ADDR (= remote_addr param).
            std::string addr(remote_addr);
            auto colon = addr.rfind(':');
            if (colon != std::string::npos) {
                std::string ip_s = addr.substr(0, colon);
                int port_i = std::atoi(addr.c_str() + colon + 1);
                in_addr peer_ia{};
                if (inet_pton(AF_INET, ip_s.c_str(), &peer_ia) == 1 &&
                    port_i > 0 && port_i <= 65535) {
                    // Same-LAN candidate: the peer's private addr, passed by the
                    // launcher (FM2K_PEER_LAN_ADDR) from the hub's local_ip
                    // exchange. Punched alongside the reflexive addr so two
                    // players behind the same router go direct over the LAN.
                    uint32_t lan_ip_be = 0;
                    uint16_t lan_port  = 0;
                    if (const char* lan = std::getenv("FM2K_PEER_LAN_ADDR")) {
                        std::string ls(lan);
                        auto lc = ls.rfind(':');
                        if (lc != std::string::npos) {
                            in_addr lia{};
                            int lp = std::atoi(ls.c_str() + lc + 1);
                            if (inet_pton(AF_INET, ls.substr(0, lc).c_str(), &lia) == 1 &&
                                lp > 0 && lp <= 65535) {
                                lan_ip_be = lia.s_addr;
                                lan_port  = static_cast<uint16_t>(lp);
                            }
                        }
                    }
                    // BUG 2: skip bursting the peer's reflexive port when the
                    // hub couldn't verify it (CGNAT advertised a local-port
                    // guess a port-rewriting NAT will have remapped).
                    const char* unver =
                        std::getenv("FM2K_PEER_REFLEXIVE_UNVERIFIED");
                    bool punch_reflexive = !(unver && unver[0] == '1');
                    // Phase 1: parse FM2K_PEER_V6_ADDR = "[2xxx::...]:port". The
                    // bracket notation disambiguates the v6 colons from the port
                    // colon. A valid v6 candidate is also-punched (CGNAT bypass).
                    uint8_t peer_v6[16] = {};
                    uint16_t v6_port = 0;
                    const uint8_t* v6_ptr = nullptr;
                    if (const char* v6e = std::getenv("FM2K_PEER_V6_ADDR")) {
                        std::string vs(v6e);
                        size_t rb = vs.find(']');
                        if (vs.size() > 4 && vs.front() == '[' &&
                            rb != std::string::npos && rb + 2 < vs.size() &&
                            vs[rb + 1] == ':') {
                            std::string host = vs.substr(1, rb - 1);
                            int vp = std::atoi(vs.c_str() + rb + 2);
                            in6_addr a6{};
                            if (inet_pton(AF_INET6, host.c_str(), &a6) == 1 &&
                                vp > 0 && vp <= 65535) {
                                std::memcpy(peer_v6, &a6, 16);
                                v6_port = static_cast<uint16_t>(vp);
                                v6_ptr = peer_v6;
                            }
                        }
                    }
                    ::fm2k::nat::StartPunch(peer_ia.s_addr,
                                             static_cast<uint16_t>(port_i),
                                             token_bytes, lan_ip_be, lan_port,
                                             punch_reflexive, v6_ptr, v6_port);
                }
            }
        }
    }

    // Send HELLO with our local game-data fingerprint (#57). Receiver
    // compares against its own and aborts the handshake on mismatch.
    ControlChannel_SendHello(static_cast<uint8_t>(player_index),
                             fm2k::game_hash::Compute());

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Initialized, connecting...");
    return true;
}

bool Netplay_InitAsSpectator(uint16_t local_port, const char* host_addr) {
    g_player_index = 2;  // sentinel -- not a player slot
    g_simple_state = SimpleState::CONNECTED;  // skip handshake; spectators don't HELLO
    g_session = nullptr;
    g_session_ready = false;
    g_netplay_frame = 0;

    // Replay-from-file mode (no peer, no network). When FM2K_REPLAY_FILE
    // is set, skip the network init + JOIN_REQ entirely and load the
    // .fm2krep / .fm2kset file directly into pb_queue. The trampoline's
    // RunSpectatorTick consumes events from pb_queue identically to a
    // live spectator, so playback Just Works through the same driver.
    if (const char* replay_path = std::getenv("FM2K_REPLAY_FILE");
        replay_path && replay_path[0]) {
        SpectatorNode_Init();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Replay: loading %s", replay_path);
        // .fm2kset default: seek to MATCH_START #1. A set body opens with
        // the session's pre-battle CSS inputs, and offline playback has no
        // JOIN_ACK to start a CSS spectate session -- the queue head could
        // never drain (observed: q frozen at 2991 while the CSS walk spun
        // forever). Anchoring at the first MATCH_START turns the stream
        // into the proven battle-slice shape; the between-match seams then
        // traverse via the same boundary machinery live multi-match uses.
        // FM2K_REPLAY_SEEK_MATCH=N picks a later match (C11 click-to-seek).
        SeekTarget seek{};
        {
            const char* ext = std::strrchr(replay_path, '.');
            const char* sm  = std::getenv("FM2K_REPLAY_SEEK_MATCH");
            if (ext && _stricmp(ext, ".fm2kset") == 0) {
                seek.kind = SeekEventKind::MATCH_START;
                seek.idx  = (sm && sm[0]) ? (uint16_t)std::atoi(sm) : 1;
                if (seek.idx == 0) seek.idx = 1;
            }
        }
        if (!SpectatorNode_LoadSessionFile(replay_path, seek)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Replay: file load failed: %s", replay_path);
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Replay: loaded -- trampoline will drive playback");
        return true;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay (spectator): Init port=%u host=%s",
                local_port, host_addr ? host_addr : "(null)");

    // Wire up the control-channel callback. OnControlMessage already
    // dispatches SPEC_JOIN_ACK / SPEC_JOIN_REDIRECT / SPEC_HEARTBEAT / SPEC_LEAVE
    // into SpectatorNode_*, so the same handler covers spectator inbound packets.
    ControlChannel_SetCallback(OnControlMessage);

    if (!NetSocket_IsInitialized()) {
        if (!NetSocket_Init(local_port, host_addr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Netplay (spectator): socket init failed");
            return false;
        }
    }

    // Stand up the spectator node (initializes its state) and request to join
    // the host. INPUT_BATCH frames flow over a TCP connection opened on
    // JOIN_ACK; UDP carries only handshake + heartbeat.
    SpectatorNode_Init();

    const sockaddr_in* upstream = NetSocket_GetRemoteAddr();
    if (!upstream || upstream->sin_port == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Netplay (spectator): no upstream addr latched");
        return false;
    }
    // Stash the original upstream as the fallback root. If our current
    // upstream later goes silent (e.g. an overflow-redirect relay died),
    // SpectatorNode_TickHealth will reconnect us here. The host always
    // serves as the root and is the always-on failback target.
    extern void SpectatorNode_SetRootAddr(const sockaddr_in& root);
    SpectatorNode_SetRootAddr(*upstream);

    // Phase 5: launcher controls the join mode via FM2K_SPECTATE_MODE env var.
    //   "current" → CURRENT_MATCH (CCCaster-style snapshot join, default)
    //   "full"    → FULL_SESSION  (replay-from-frame-0)
    // Anything else (or unset) defaults to CURRENT_MATCH -- the user-facing
    // intent for live spectating; FULL_SESSION is opt-in for streamers.
    SpecJoinMode mode = SpecJoinMode::CURRENT_MATCH;
    if (const char* env = std::getenv("FM2K_SPECTATE_MODE")) {
        if (env[0] == 'f' || env[0] == 'F') {
            mode = SpecJoinMode::FULL_SESSION;
        }
    }
    // Hub-driven NAT registration. Same as Netplay_Init's player path:
    // fire a STUN probe so the hub learns OUR external UDP mapping
    // (from the spec hook's UDP socket -- same one ControlChannel uses
    // for SPEC_JOIN_REQ + SPEC_HEARTBEAT). Without this, hub's
    // user.udp_addr is whatever an earlier game STUN landed (or
    // empty), spectator_incoming forwards the wrong port to the
    // host, and the host's UDP NAT-punch heartbeat goes nowhere --
    // spec then sits on "Connecting..." through the entire reconnect
    // backoff.
    {
        const char* hub_udp     = std::getenv("FM2K_HUB_UDP_ADDR");
        const char* hub_user_id = std::getenv("FM2K_HUB_USER_ID");
        if (hub_udp && hub_user_id) {
            ::fm2k::nat::SendStunProbe();
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay (spectator): FM2K_HUB_UDP_ADDR or FM2K_HUB_USER_ID "
                "unset -- STUN probe skipped, host's punch may target wrong "
                "port (cross-NAT spec will likely fail)");
        }
    }

    SpectatorNode_RequestJoin(*upstream, mode);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay (spectator): SPEC_JOIN_REQ sent to host (mode=%s)",
                mode == SpecJoinMode::CURRENT_MATCH ? "CURRENT_MATCH" : "FULL_SESSION");
    return true;
}

void Netplay_Shutdown() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Netplay: Shutting down");

    // Tell any subscribers we're exiting cleanly BEFORE we tear down the socket,
    // so they stop instead of treating the dropped stream as a glitch and
    // storm-reconnecting to a dead host. UDP control path is still live here.
    SpectatorNode_BroadcastSessionEnd();

    if (g_session) {
        gekko_destroy(&g_session);
        g_session = nullptr;
    }

    if (ControlChannel_IsConnected()) {
        ControlChannel_SendDisconnect();
    }

    g_simple_state = SimpleState::DISCONNECTED;
    g_received_hello = false;
    g_received_hello_ack = false;
    g_session_delay_cache_valid = false;
    g_session_delay_cached      = 0;
}

// =============================================================================
// STATE ACCESSORS
// =============================================================================

NetplayState Netplay_GetState() {
    return MapToLegacyState(g_simple_state);
}

void Netplay_SetState(NetplayState state) {
    (void)state;  // Ignored - we use simplified state
}

bool Netplay_IsConnected() {
    return g_simple_state >= SimpleState::CONNECTED;
}

bool Netplay_IsActive() {
    return g_simple_state == SimpleState::BATTLE && g_session != nullptr;
}

bool Netplay_IsSessionReady() {
    return g_session_ready;
}

// =============================================================================
// CSS PROCESSING -- GekkoNet lockstep (input_prediction_window = 0)
//
// CSS used to ride a custom CCCaster-style ring buffer over CtrlMsg::CSS_INPUT.
// It's now a GekkoGameSession with prediction_window=0, which gives true
// lockstep: gekko_update_session emits AdvanceEvent only when both peers'
// inputs for the current frame have arrived. Save/Load events are suppressed
// in lockstep mode (game_session.cpp:226-228, 365-367, 537), so CSS state is
// derived purely from the shared seed (0x12345678 reseeded on sync) +
// identical confirmed inputs -- same determinism as today, but riding a
// well-tested transport.
// =============================================================================
// LEGACY API
// =============================================================================

bool Netplay_StartGekkoSession() {
    return Netplay_StartBattle();
}

void Netplay_StopGekkoSession() {
    Netplay_EndBattle();
}

void Netplay_PollGekkoNet() {
    if (g_session) {
        gekko_network_poll(g_session);
        int event_count = 0;
        auto events = gekko_session_events(g_session, &event_count);
        for (int i = 0; i < event_count; i++) {
            auto event = events[i];
            if (event->type == GekkoSessionStarted || event->type == GekkoPlayerConnected) {
                g_session_ready = true;
            }
        }
    }
}

void Netplay_ResetCSSState() {
    // Tear down the CSS GekkoSession if it's still alive -- happens when this
    // is called outside the normal battle-entry path (e.g. peer disconnect).
    if (g_session && g_session_kind == SessionKind::CSS) {
        Netplay_EndCSSSession();
    }
    g_css_frame         = 0;
    g_css_advance_p1    = 0;
    g_css_advance_p2    = 0;
    g_css_advance_ready = false;
    g_css_active        = false;
    g_css_synced        = false;
    g_remote_css_ready  = false;
    g_local_css_ready   = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Reset state for new sync");
}

bool Netplay_IsRemoteCSSReady() {
    return g_remote_css_ready;
}

bool Netplay_IsCSSFullySynced() {
    return g_css_synced;
}

void Netplay_SetLocalCSSReady(bool ready) {
    g_local_css_ready = ready;
    if (ready) {
        ControlChannel_SendBattleReady();
    }
}

void Netplay_ProcessMenu() {
    ControlChannel_Poll();
}

const CSSState& Netplay_GetCSSState() {
    static CSSState dummy;
    return dummy;
}

uint32_t Netplay_GetFrame() {
    return g_netplay_frame;
}

// Highest battle frame gekko has REAL (non-predicted) inputs for from BOTH
// players -- i.e. the frame is locked and can never roll back. Returns the
// live frame when no session is up so non-netplay callers never gate on it.
// FM95 uses this to detect the battle->CSS phase edge on a CONFIRMED frame so
// both peers signal battle-end at the SAME deterministic frame (the live pool
// each peer classifies is post-prediction and differs per peer -> the two
// were tearing down 16 frames apart, blowing past the negotiated swap_frame).
uint32_t Netplay_GetConfirmedFrame() {
    if (!g_session) return g_netplay_frame;
    const int cf = gekko_confirmed_frame(g_session);
    return cf < 0 ? 0u : (uint32_t)cf;
}

uint32_t Netplay_GetPingMs() {
    return ControlChannel_GetRttMs();
}

int Netplay_GetLocalDelay() {
    return g_local_delay;
}


GekkoNetworkStats Netplay_GetNetworkStats() {
    GekkoNetworkStats stats = {};
    if (g_session) {
        // Get stats for the remote player (whichever slot isn't us)
        int remote_handle = (g_player_index == 0) ? 1 : 0;
        gekko_network_stats(g_session, remote_handle, &stats);
    }
    return stats;
}

uint32_t Netplay_GetDesyncCount() {
    return g_desync_count;
}

uint32_t Netplay_GetRollbackCount() {
    return g_rollback_count;
}

float Netplay_GetFramesAhead() {
    if (!g_session) return 0.0f;
    float ahead = gekko_frames_ahead(g_session);
    // Host-clock rift pacing (A/B via FM2K_HOST_CLOCK): blend the host-clock rift
    // with GekkoNet's frame-advantage for smoother pacing under high/asymmetric
    // latency. This is the value the live loop feeds SleepToTarget. GekkoNet still
    // owns rollback (the "behind => speed up" half); this only refines the
    // "ahead => slow down" sleep. Called once per tick, so the rift ring advances
    // exactly once per frame.
    if (fm2k::hostclock::Enabled()) {
        ahead = fm2k::hostclock::PacingFramesAhead(
            g_netplay_frame, ControlChannel_GetRttMs(), ahead);
    }
    return ahead;
}

void Netplay_HandleFrameTime() {
    HandleFrameTime(Netplay_GetFramesAhead());
}

// ============================================================================
// RUNAHEAD MID-MATCH TOGGLE (F8)
// ============================================================================
// WndProc subclass calls Netplay_RequestRunaheadToggle from the message-pump
// thread on VK_F8 press. The actual gekko_set_runahead call MUST happen on
// the same thread that owns g_session -- see Netplay_PollRunaheadToggle below,
// which the trampoline calls at the top of every battle tick before
// Netplay_ProcessBattleInputPhase. Atomic flag + load/store keeps the cross-
// thread handoff clean without any extra mutex.

void Netplay_RequestRunaheadToggle() {
    g_runahead_toggle_requested.store(true, std::memory_order_release);
}

void Netplay_PollRunaheadToggle() {
    if (!g_runahead_toggle_requested.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (!g_session || g_session_kind != SessionKind::BATTLE) {
        // Toggle outside an active battle session is a no-op (CSS lockstep
        // doesn't have runahead; spectator session can't change peer's
        // runahead anyway). Silently swallow so spurious F8 presses during
        // menus don't log noise.
        return;
    }
    const int cur    = g_runahead_active.load(std::memory_order_acquire);
    const int target = (cur == 0) ? g_runahead_user_pref : 0;
    gekko_set_runahead(g_session, (unsigned char)target);
    g_runahead_active.store(target, std::memory_order_release);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Runahead toggled: %d -> %d at bf=%u (user_pref=%d)",
                cur, target, g_netplay_frame, g_runahead_user_pref);
}
