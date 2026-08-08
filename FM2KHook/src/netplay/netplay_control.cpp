// Netplay control-channel handling: build/broadcast HOST_CONFIG packets +
// OnControlMessage (the peer/spectator message dispatcher). Extracted VERBATIM
// from netplay.cpp; shares state via netplay_internal.h.
// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
#include "netplay.h"
#include "netplay_internal.h"  // shared file-scope state, externed for the split netplay_*.cpp TUs
#include "../hooks/hooks.h"   // Hook_ApplySOCD_Public for SOCD-pre-apply on spec capture
#include "../hooks/css_autoconfirm.h"  // CssAutoConfirm_OnReplayMatchStart (TEST_CSS_CHAR pin)
#include "control_channel.h"
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
// MATCH SETTINGS: host-authoritative delivery + the barrier digest
// =============================================================================
//
// WHY THERE IS A DIGEST AT ALL
// ----------------------------
// HOST_CONFIG carries the five settings that decide how the SIM runs (round
// time, round count, game speed, selected stage, SOCD mode). It used to be a
// fire-and-forget broadcast with no ack and no retry: when both datagrams were
// lost the guest silently kept its own game.ini values, RSS_BATTLE_INIT stamped
// g_score_value = 100 * round_time - 1 with a DIFFERENT round time on each peer,
// and the two sims ran ~1500 frames of a battle holding divergent round-timer
// state before diverging observably in the round-end tail.
//
// The fix is not "send it more times" -- it is to make agreement a PRECONDITION
// of entering battle. Both peers announce this digest of their LIVE engine
// globals in every BATTLE_ENTERING, and the entry barrier (netplay_barriers.cpp)
// refuses to complete until the two announcements are equal. Because neither
// peer runs a single battle sim frame until its barrier completes (both call
// sites -- trampoline_battle.cpp and hooks_update.cpp -- return early), agreement
// is ordered strictly BEFORE the first vs_round_function call, i.e. before
// RSS_BATTLE_INIT stamps anything.
//
// Hashing the LIVE globals rather than the packet contents is deliberate: it
// verifies that the settings were APPLIED, not merely transmitted.
namespace {

// Host-only test-harness overrides. Idempotent (writes a constant), and kept
// HOST-ONLY on purpose even though the harness exports the env var to both
// processes: the guest must reach the same round time through HOST_CONFIG, so
// the harness keeps exercising the real delivery path instead of masking it.
// (FM2K_TEST_ROUNDS is separately forced on every peer every frame from
// hooks_update.cpp -- that predates this and is left alone.)
void HostApplyMatchSettingOverrides() {
    if constexpr (FM2K::kIsFM2K) {
        if (g_player_index != 0) return;
        // FM2K_TEST_ROUNDS=N: force N-round matches. g_default_round (0x430124)
        // is loaded from game.ini at boot and persists, so writing it here --
        // before sampling it -- makes the host's own game AND the
        // HOST_CONFIG-synced client run N-round matches. 1 => fast
        // css->battle->css cycles for the multi-match-under-loss e2e harness.
        static const int s_test_rounds = []{
            const char* v = std::getenv("FM2K_TEST_ROUNDS");
            return (v && v[0]) ? std::atoi(v) : 0;
        }();
        if (s_test_rounds > 0) *(uint32_t*)0x430124 = (uint32_t)s_test_rounds;
        // FM2K_TEST_ROUND_TIME=S: force an S-second round timer (g_round_time @
        // 0x430114, from TestPlay.time). The autoplay rarely KOs, so a short
        // timer makes rounds TIMEOUT fast -> with --rounds 1, matches end in
        // seconds -> many fast css->battle->css cycles for the harness.
        // -1 = unset (leave game default); >= 0 forces. 0 = OFF/infinite timer
        // -- wanwan needs this: a non-zero custom timer leaves the round-timer
        // counter at 0 on subsequent battles (they start at 0 time), so 1-round
        // matches there end on KO with the timer off rather than the buggy
        // short timer.
        static const int s_test_round_time = []{
            const char* v = std::getenv("FM2K_TEST_ROUND_TIME");
            return (v && v[0]) ? std::atoi(v) : -1;
        }();
        if (s_test_round_time >= 0) *(uint32_t*)0x430114 = (uint32_t)s_test_round_time;
    }
}

// Count of HOST_CONFIG packets this node has APPLIED. Surfaced in the
// per-round settings line so "did the guest ever get the config" is one grep
// (the triage card in the Phase 1 diagnosis depends on it).
uint32_t s_host_config_rx = 0;

}  // namespace

// Digest of the five sim-relevant match settings, read from the LIVE engine
// globals -- the same words RSS_BATTLE_INIT and process_game_inputs consume.
// Never returns 0 for a real reading: 0 is the wire sentinel for "not
// announced" and must stay distinguishable.
//
// FM95 returns 0 (gate disabled): round time / round count / game speed have no
// mapped globals there (RE-3 in docs/FM95_Support_Status.md), the HOST_CONFIG
// receiver is already a no-op, and ADDR_SELECTED_STAGE is 0 -- so there is
// nothing to agree on and nothing to read without inventing addresses.
//
// PURE READ. It deliberately does NOT apply the host's test-harness overrides:
// this is called from inside vs_round_function's detour (the [ROUND-START]
// stamp) and from the barrier, and a function that quietly writes engine
// globals from either of those places is a trap. The host applies its overrides
// in BuildHostConfigPacket, which always runs first -- at CheckFullyConnected
// (handshake) and again at the top of Netplay_SignalBattleEntry, both strictly
// before any digest is announced.
uint32_t Netplay_MatchSettingsDigest() {
    if constexpr (!FM2K::kIsFM2K) {
        return 0;
    } else {
        struct __attribute__((packed)) Settings {
            uint32_t selected_stage;
            uint32_t round_count;
            uint32_t round_time_sec;
            uint32_t game_speed_pct;
            uint32_t socd_mode;
        } s = {
            *(uint32_t*)FM2K::ADDR_SELECTED_STAGE,
            *(uint32_t*)0x430124,
            *(uint32_t*)0x430114,
            *(uint32_t*)0x430104,
            (uint32_t)Hook_GetSOCDModePublic(),
        };
        // FNV-1a over 20 bytes -- cheap, and this runs at most a few times per
        // frame during a barrier wait (never inside the sim loop).
        uint32_t h = 0x811C9DC5u;
        const uint8_t* p = (const uint8_t*)&s;
        for (size_t i = 0; i < sizeof(s); ++i) { h ^= p[i]; h *= 0x01000193u; }
        return h ? h : 1u;  // 0 is reserved for "not announced"
    }
}

uint32_t Netplay_HostConfigRxCount() { return s_host_config_rx; }

// Build the current HOST_CONFIG packet from live engine state. Shared
// by Netplay_BroadcastHostConfig (fans to peer + all subs at battle-
// start moments) and Netplay_SendHostConfigToSpec (one-shot push when a
// spectator binds mid-match, so they don't miss the broadcast that
// fired before they were subscribed). Only meaningful on the host side.
CtrlPacket BuildHostConfigPacket() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::HOST_CONFIG;
    pkt.data.host_config.selected_stage  = *(uint32_t*)FM2K::ADDR_SELECTED_STAGE;
    pkt.data.host_config.socd_mode       = (uint8_t)Hook_GetSOCDModePublic();
    // Loaded-from-game.ini engine globals. hit_judge_set_function reads
    // game.ini at boot into these -- spec's local game.ini gives spec's
    // defaults, but host's authoritative values must override or specs
    // get wrong timer / round count (pkmncc default time=60, host had
    // time=0 / infinite, spec ended up running with 60s rounds).
    if constexpr (FM2K::kIsFM2K) {
    HostApplyMatchSettingOverrides();
    pkt.data.host_config.round_time_sec  = *(uint32_t*)0x430114; // lParam
    pkt.data.host_config.round_count     = *(uint32_t*)0x430124; // g_default_round
    pkt.data.host_config.game_speed_pct  = *(uint32_t*)0x430104; // uValue
    } else {
    // FM95: rounds/time/game-speed globals are unmapped (RE-3 in
    // docs/FM95_Support_Status.md -- LoadIniSettings@0x402920 not yet RE'd).
    // Send per-field "unset" sentinels so receivers keep their local defaults.
    pkt.data.host_config.round_time_sec  = 0xFFFFFFFFu;
    pkt.data.host_config.round_count     = 0xFFFFFFFFu;
    pkt.data.host_config.game_speed_pct  = 0xFFFFFFFFu;
    }
    return pkt;
}

// One-shot push: snapshot current host settings and ship to a single
// subscriber addr. Called from SpectatorNode's TCP-bound handler so a
// mid-match spec joiner gets the current rules (stage, SOCD) without
// having to wait for the next match-start broadcast. No-op when the
// local peer isn't host (spec doesn't have authoritative config).
void Netplay_SendHostConfigToSpec(const sockaddr_in& to) {
    if (g_player_index != 0) return;
    CtrlPacket pkt = BuildHostConfigPacket();
    ControlChannel_SendTo(pkt, to);
}

// Snapshot host's current settings and ship them to the remote peer +
// any subscribed spectators. Called from CheckFullyConnected (initial
// rendezvous) and from Netplay_StartBattle (every new match) so settings
// changes mid-session propagate. No-op when the local peer isn't host.
void Netplay_BroadcastHostConfig() {
    if (g_player_index != 0) return;  // only host pushes config
    CtrlPacket pkt = BuildHostConfigPacket();
    const auto& hc = pkt.data.host_config;
    ControlChannel_SendHostConfig(
        /*selected_stage*/  hc.selected_stage,
        /*round_count*/     hc.round_count,
        /*round_time_sec*/  hc.round_time_sec,
        /*game_speed_pct*/  hc.game_speed_pct,
        /*socd_mode*/       hc.socd_mode);

    // Also push to subscribed spectators on the same multiplex channel.
    auto subs = SpectatorNode_GetSubscriberAddrs();
    for (const auto& s : subs) {
        ControlChannel_SendTo(pkt, s);
    }
}

// Peer-only re-push, driven by the battle-entry barrier while the two peers'
// settings digests disagree. This is the retry-until-acked HOST_CONFIG never
// had: the "ack" is the guest's next BATTLE_ENTERING, which announces the
// digest of what it actually applied, so the loop terminates on APPLICATION
// rather than on delivery. Deliberately skips the spectator fan-out --
// subscribers already got the copy from Netplay_BroadcastHostConfig and this
// runs on a 100ms cadence, so fanning out would be pure chatter.
void Netplay_ResendHostConfigToPeer() {
    if (g_player_index != 0) return;
    CtrlPacket pkt = BuildHostConfigPacket();
    const auto& hc = pkt.data.host_config;
    ControlChannel_SendHostConfig(hc.selected_stage, hc.round_count,
                                  hc.round_time_sec, hc.game_speed_pct,
                                  hc.socd_mode);
}

static void CheckFullyConnected() {
    if (g_received_hello && g_received_hello_ack) {
        if (g_simple_state != SimpleState::CONNECTED) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Full handshake complete - CONNECTED!");
            g_simple_state = SimpleState::CONNECTED;
            ControlChannel_SetConnected(true);

            // Sync RNG immediately
            *(uint32_t*)FM2K::ADDR_RANDOM_SEED = 0x12345678;
            SpectatorNode_AppendPinRng(0x12345678);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Synced RNG=0x12345678");

            // Push host's authoritative match config so client adopts the
            // same stage/SOCD/etc settings without manual mirroring.
            Netplay_BroadcastHostConfig();
        }
    }
}

void OnControlMessage(const CtrlPacket* packet, const sockaddr_in& from) {
    switch (packet->header.type) {
        case CtrlMsg::HELLO: {
            const uint32_t local_hash = fm2k::game_hash::Compute();
            const uint32_t peer_hash  = packet->data.hello.game_hash;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HELLO from player %d (peer_hash=0x%08X local_hash=0x%08X)",
                packet->data.hello.player_id,
                peer_hash, local_hash);
            // Game-data hash check (#57). 0 on either side means the
            // peer is older / we couldn't enumerate -- fall through to
            // the existing handshake flow so we don't break legacy
            // clients during rollout. Both sides nonzero + different
            // = abort: write a DISCONNECT outcome so the launcher's
            // PollMatchOutcome surfaces a toast and closes the game.
            if (local_hash != 0 && peer_hash != 0 && local_hash != peer_hash) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: GAME DATA MISMATCH -- peer=0x%08X us=0x%08X (%s). "
                    "Aborting handshake; have both peers send each other their "
                    "FM2K_*_Debug.log file and diff the 'GameHash: manifest' "
                    "section to find which file differs.",
                    peer_hash, local_hash, fm2k::game_hash::DescribeLocal());
                // Re-dump the local manifest right next to the error so users
                // who scroll up from the bottom of the log see exactly what
                // we hashed without hunting for the original "GameHash:
                // manifest" line up at boot time. Multi-line so a peer
                // reading the log can quickly spot a different size or
                // content hash on a specific filename.
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: local manifest follows (compare against peer's "
                    "log line-by-line):");
                // Iterate the cached entries vector directly. We used
                // to split the cached manifest STRING line-by-line via
                // strchr('\n'), but that path turned out to corrupt one
                // entry's render in some installs (placeholder22.player
                // showed up as "placeholder22|-", missing extension and
                // size). Going through entries gets bytes byte-equivalent
                // to the boot-time per-entry log.
                fm2k::game_hash::ForEachManifestEntry(
                    [](const char* name, uint64_t size, uint64_t content_hash,
                       void* /*user*/) {
                        if (content_hash != 0) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                "  %s|%llu|%016llx",
                                name,
                                (unsigned long long)size,
                                (unsigned long long)content_hash);
                        } else {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                "  %s|%llu|-",
                                name,
                                (unsigned long long)size);
                        }
                    }, nullptr);
                SharedMem_PublishMatchOutcome(FM2K_MATCH_OUTCOME_HASH_MISMATCH);
                break;
            }
            ControlChannel_SendHelloAck(static_cast<uint8_t>(g_player_index));
            g_received_hello = true;
            CheckFullyConnected();
            break;
        }

        case CtrlMsg::HELLO_ACK:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HELLO_ACK");
            g_received_hello_ack = true;
            CheckFullyConnected();
            break;

        case CtrlMsg::CSS_INPUT:
            // CSS_INPUT is dead code post-redesign -- CSS lockstep now lives
            // inside a GekkoGameSession with prediction_window=0, so inputs
            // flow through GekkoNet's transport. The enum value + this case
            // are kept as a no-op for backward compatibility with peers that
            // still send the old packet (they'll be silently ignored).
            break;

        case CtrlMsg::BATTLE_READY: {
            // After CSS GekkoSession is fully up (g_css_synced=true) the
            // BATTLE_READY rendezvous is over -- any leftover packets are
            // network-buffered echoes from the rendezvous window and can
            // be silently dropped. Without this gate the unconditional
            // echo below would ping-pong forever between both peers,
            // logging "Sent / Received BATTLE_READY" every ~10ms for the
            // entire CSS phase.
            if (g_css_synced) {
                break;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_READY from remote");
            g_remote_css_ready = true;

            // Loss-tolerant echo -- same pattern as BATTLE_ENTERING /
            // BATTLE_END. When peers return to CSS at slightly
            // different wall-clock times (one finishes battle-end-sync
            // ~300 ms before the other), the ahead peer creates its CSS
            // GekkoSession on the first BATTLE_READY it sees, then
            // STOPS sending its own BATTLE_READY. The lagging peer's
            // BATTLE_READYs after that point arrive here and need an
            // echo back, otherwise the lagging peer never sees our
            // signal and stays stuck resending forever. Bounded by the
            // !g_css_synced gate above -- echo only happens during the
            // rendezvous window, terminates when sync completes.
            if (g_local_css_ready) {
                ControlChannel_SendBattleReady();
            }
            break;
        }

        case CtrlMsg::BATTLE_ENTERING: {
            const uint32_t remote_proposal = packet->data.sync.frame;
            const uint8_t  remote_epoch    = packet->data.sync.epoch;
            const bool     remote_done     = (packet->data.sync.flags & 0x1) != 0;
            // Spectator-side handling: this is host telling us about the
            // upcoming CSS->battle swap. Flip our SpectateSession to battle
            // config. (Spectators don't participate in proposal convergence --
            // they passively follow whatever the host announces.)
            if (g_session_kind == SessionKind::SPECTATE) {
                Netplay_OnHostBattleEntering(remote_proposal);
                break;
            }
            // Reject stale carryover from a previous match. g_battle_entry_armed
            // is true ONLY between "new CSS session up" and "battle session
            // started"; the epoch tag additionally rejects packets from a
            // DIFFERENT barrier instance that happen to land inside our
            // armed window.
            const bool epoch_current =
                (remote_epoch == 0) || (remote_epoch == g_entry_epoch);
            if (!g_battle_entry_armed || !epoch_current) {
                // Answer-after-complete: the peer is still retrying a
                // barrier WE already passed (its copies of our signal were
                // lost). Answer with a completed-flag signal so it can
                // finish too -- without this the lagging peer wedges
                // forever resending into a disarmed gate. Never answer a
                // sender that is itself completed (storm termination), and
                // for legacy epoch-0 peers bound the answers to a 10s TTL
                // after our completion so true stale packets die out.
                const bool answers =
                    !remote_done && g_entry_done_epoch != 0 &&
                    (remote_epoch == g_entry_done_epoch ||
                     (remote_epoch == 0 &&
                      GetTickCount() - g_entry_done_ms < 10000));
                if (answers) {
                    static uint32_t s_last_answer_ms = 0;
                    const uint32_t now_ms = GetTickCount();
                    if (now_ms - s_last_answer_ms >= 250) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: answering completed entry-barrier retry "
                            "(epoch=%u swap=%u)", remote_epoch, remote_proposal);
                        // Carry our live settings digest here too: a peer that
                        // is still holding on config agreement (see
                        // netplay_barriers.cpp) learns our value from this
                        // answer even though we already disarmed.
                        ControlChannel_SendBattleEntering(
                            remote_proposal, g_entry_done_epoch, 0x1,
                            Netplay_MatchSettingsDigest());
                        s_last_answer_ms = now_ms;
                    }
                } else {
                    static uint32_t s_drop_count = 0;
                    if (s_drop_count++ < 8 || (s_drop_count & 0x3F) == 0) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: ignoring out-of-window BATTLE_ENTERING "
                            "(swap=%u epoch=%u done=%d, drop#%u) -- armed=%d our_epoch=%u",
                            remote_proposal, remote_epoch, (int)remote_done,
                            (unsigned)s_drop_count, (int)g_battle_entry_armed,
                            g_entry_epoch);
                    }
                }
                break;
            }
            // Player-side handling: convergence on max(local, remote) swap.
            const uint32_t prev_agreed = g_battle_entry_swap_frame;
            if (remote_proposal > g_battle_entry_swap_frame) {
                g_battle_entry_swap_frame = remote_proposal;
            }
            g_remote_battle_entered = true;
            g_entry_remote_proposal = remote_proposal;
            // Match-settings agreement term. 0 = peer did not announce (BTB /
            // a build without the field) and leaves the gate open; anything
            // else is compared against our own live digest by
            // Netplay_IsBattleSynced before the barrier may complete.
            g_entry_remote_cfg_digest = packet->data.sync.cfg_digest;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_ENTERING (remote_swap=%u, prev_agreed=%u, agreed=%u, epoch=%u, done=%d, cfg=0x%08X local_cfg=0x%08X)",
                remote_proposal, prev_agreed, g_battle_entry_swap_frame,
                remote_epoch, (int)remote_done,
                g_entry_remote_cfg_digest, Netplay_MatchSettingsDigest());

            // Echo our own BATTLE_ENTERING back if we've already signaled
            // locally -- needed for the lossy-network case where remote
            // received our signal but their echo to us was dropped.
            // Skipped when the sender is already completed (it has our
            // signal by definition). Without rate-limiting, both peers
            // echo every echo from the other and we get an infinite
            // ping-pong storm (observed hundreds of sends in a single
            // millisecond). 100ms cap is far below the swap-frame
            // transition window so packet-loss recovery still works, but
            // the storm can't run away.
            if (g_local_battle_entered && !remote_done) {
                static uint32_t last_echo_ms = 0;
                const uint32_t now_ms = GetTickCount();
                if (now_ms - last_echo_ms >= 100) {
                    ControlChannel_SendBattleEntering(
                        g_battle_entry_swap_frame, g_entry_epoch,
                        g_battle_synced ? 0x1 : 0x0,
                        Netplay_MatchSettingsDigest());
                    last_echo_ms = now_ms;
                }
            }
            break;
        }

        case CtrlMsg::BATTLE_END: {
            const uint32_t remote_proposal = packet->data.sync.frame;
            const uint8_t  remote_epoch    = packet->data.sync.epoch;
            const bool     remote_done     = (packet->data.sync.flags & 0x1) != 0;
            if (g_session_kind == SessionKind::SPECTATE) {
                Netplay_OnHostBattleEnd(remote_proposal);
                break;
            }
            // Same stale-carryover gate + epoch check + answer-after-
            // complete as BATTLE_ENTERING (see that handler for the full
            // rationale). Armed when the battle GekkoSession comes up;
            // cleared in Netplay_EndBattle.
            const bool epoch_current =
                (remote_epoch == 0) || (remote_epoch == g_end_epoch);
            if (!g_battle_end_armed || !epoch_current) {
                const bool answers =
                    !remote_done && g_end_done_epoch != 0 &&
                    (remote_epoch == g_end_done_epoch ||
                     (remote_epoch == 0 &&
                      GetTickCount() - g_end_done_ms < 10000));
                if (answers) {
                    static uint32_t s_last_answer_ms = 0;
                    const uint32_t now_ms = GetTickCount();
                    if (now_ms - s_last_answer_ms >= 250) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: answering completed end-barrier retry "
                            "(epoch=%u swap=%u)", remote_epoch, remote_proposal);
                        ControlChannel_SendBattleEnd(
                            remote_proposal, g_end_done_epoch, 0x1);
                        s_last_answer_ms = now_ms;
                    }
                } else {
                    static uint32_t s_drop_count = 0;
                    if (s_drop_count++ < 8 || (s_drop_count & 0x3F) == 0) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Netplay: ignoring out-of-window BATTLE_END "
                            "(swap=%u epoch=%u done=%d, drop#%u) -- armed=%d our_epoch=%u",
                            remote_proposal, remote_epoch, (int)remote_done,
                            (unsigned)s_drop_count, (int)g_battle_end_armed,
                            g_end_epoch);
                    }
                }
                break;
            }
            const uint32_t prev_agreed = g_battle_end_swap_frame;
            if (remote_proposal > g_battle_end_swap_frame) {
                g_battle_end_swap_frame = remote_proposal;
            }
            g_remote_battle_end_signaled = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received BATTLE_END (remote_swap=%u, prev_agreed=%u, agreed=%u, epoch=%u, done=%d)",
                remote_proposal, prev_agreed, g_battle_end_swap_frame,
                remote_epoch, (int)remote_done);

            // Same rate-limited echo as BATTLE_ENTERING; skipped when the
            // sender is already completed.
            if (g_local_battle_end_signaled && !remote_done) {
                static uint32_t last_echo_ms = 0;
                const uint32_t now_ms = GetTickCount();
                if (now_ms - last_echo_ms >= 100) {
                    ControlChannel_SendBattleEnd(
                        g_battle_end_swap_frame, g_end_epoch,
                        g_battle_end_synced ? 0x1 : 0x0);
                    last_echo_ms = now_ms;
                }
            }
            break;
        }

        case CtrlMsg::DISCONNECT:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Remote disconnected");
            ControlChannel_SetConnected(false);
            g_simple_state = SimpleState::DISCONNECTED;
            // Drop the pinned auto-delay so the next connection measures
            // fresh (peer might be on a different network now).
            g_session_delay_cache_valid = false;
            g_session_delay_cached      = 0;
            break;

        case CtrlMsg::HOST_CONFIG: {
            // Host's authoritative match settings -- adopt locally so this
            // peer (client OR spectator) runs with identical rules.
            // Per-field "unset" sentinels: 0xFFFFFFFF for selected_stage,
            // 0 for the count/time/speed fields, 0xFF for socd_mode.
            const auto& hc = packet->data.host_config;
            ++s_host_config_rx;
            const uint32_t cfg_before = Netplay_MatchSettingsDigest();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: Received HOST_CONFIG #%u (stage=%u rounds=%u time=%u speed=%u socd=%u)",
                s_host_config_rx,
                hc.selected_stage, hc.round_count, hc.round_time_sec,
                hc.game_speed_pct, (unsigned)hc.socd_mode);

            // Stage selection -- direct memcpy to FM2K's selected-stage
            // global (FM2K::ADDR_SELECTED_STAGE; IDA-verified in WW as
            // 0x43010c, the var that vs_round_function reads when
            // calling LoadStageFile(wParam)). The previous addr
            // 0x470188 had no xrefs and writes were silently ignored.
            if (hc.selected_stage != 0xFFFFFFFF) {
                *(uint32_t*)FM2K::ADDR_SELECTED_STAGE = hc.selected_stage;
            }

            // SOCD mode -- wire through the runtime setter. Persists for
            // the rest of the session unless host changes it again.
            if (hc.socd_mode != 0xFF) {
                Hook_SetSOCDMode((int)hc.socd_mode);
            }

            // Game-ini-derived settings. Engine's hit_judge_set_function
            // (0x414930) loaded these from the LOCAL game.ini at boot --
            // for spec mode that's spec's local .ini which doesn't know
            // about the host's per-match overrides. Host's authoritative
            // values must clobber here so timer / round count / speed
            // match. Sentinel 0xFFFFFFFF means "host left default, don't
            // override". 0 IS a valid value for round_time_sec (= no
            // timer / infinite), which is why we can't use 0 as unset.
            if constexpr (FM2K::kIsFM2K) {
                if (hc.round_time_sec != 0xFFFFFFFFu) {
                    *(uint32_t*)0x430114 = hc.round_time_sec;  // lParam (TIMER_SET)
                }
                if (hc.round_count != 0xFFFFFFFFu) {
                    *(uint32_t*)0x430124 = hc.round_count;     // g_default_round (1v1)
                }
                if (hc.game_speed_pct != 0xFFFFFFFFu) {
                    *(uint32_t*)0x430104 = hc.game_speed_pct;  // uValue (GameSpeed)
                }
            } else {
                // FM95: rounds/time/game-speed globals unmapped (RE-3) --
                // the host-config apply is a no-op until LoadIniSettings@
                // 0x402920's globals are RE'd. Log once, not per-packet.
                static bool s_re3_logged = false;
                if (!s_re3_logged) {
                    s_re3_logged = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "FM95: host-config apply pending RE-3");
                }
            }

            // Announce the post-apply digest whenever it MOVED. The battle-entry
            // barrier compares digests, so a config that lands mid-barrier must
            // reach the host promptly or the host keeps retrying on its 100ms
            // cadence for no reason. The barrier's own resend loop would also
            // carry it, but only while WE are still un-agreed -- applying the
            // config can make us agreed in the same instant, and then this is
            // the packet that tells the host.
            const uint32_t cfg_after = Netplay_MatchSettingsDigest();
            if (cfg_after != cfg_before) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Netplay: match settings adopted from host "
                    "(cfg 0x%08X -> 0x%08X)", cfg_before, cfg_after);
                // g_player_index <= 1 keeps a spectator (index 2, and it
                // receives HOST_CONFIG too) from ever emitting a player-side
                // barrier packet. The armed/entered pair is already false
                // there; this is belt and braces.
                if (g_player_index <= 1 &&
                    g_battle_entry_armed && g_local_battle_entered) {
                    ControlChannel_SendBattleEntering(
                        g_battle_entry_swap_frame, g_entry_epoch,
                        g_battle_synced ? 0x1 : 0x0, cfg_after);
                }
            }
            break;
        }

        case CtrlMsg::CHAT: {
            // Inbound peer chat. Append to the chat log ring; launcher UI
            // reads via Netplay_PopChatMessage on its own cadence.
            const char* text = packet->data.chat.text;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Netplay: CHAT from remote: \"%s\"", text);
            Netplay_PushChatMessage(/*from_remote*/ true, text);
            break;
        }

        case CtrlMsg::SPEC_JOIN_REQ:
            // Older spectator builds send no payload (zero-init bytes), which
            // resolves to mode=FULL_SESSION -- the existing replay-from-frame-0
            // path. New builds set mode explicitly. Range-clamp anything
            // beyond the highest known enum value back to FULL_SESSION so a
            // future-versioned spectator pointed at this older host stays on
            // the safe path.
            {
                const uint8_t mode_byte = packet->data.spec_join_req.mode;
                const SpecJoinMode mode =
                    (mode_byte == static_cast<uint8_t>(SpecJoinMode::CURRENT_MATCH))
                        ? SpecJoinMode::CURRENT_MATCH
                        : SpecJoinMode::FULL_SESSION;
                uint32_t resume = 0;
                std::memcpy(&resume,
                            &packet->data.spec_join_req.reserved[1], 4);
                SpectatorNode_HandleJoinReq(from, mode,
                    packet->data.spec_join_req.reserved[0], resume,
                    packet->data.spec_join_req.reserved[5],
                    packet->data.spec_join_req.reserved[6]);
            }
            break;

        case CtrlMsg::SPEC_JOIN_ACK:
            SpectatorNode_HandleJoinAck(from,
                                        packet->data.spec_join_ack.host_session_kind,
                                        packet->data.spec_join_ack.host_tcp_port,
                                        packet->data.spec_join_ack.host_p1_char,
                                        packet->data.spec_join_ack.host_p2_char,
                                        packet->data.spec_join_ack.host_stage,
                                        packet->data.spec_join_ack.host_p1_color,
                                        packet->data.spec_join_ack.host_p2_color);
            break;

        case CtrlMsg::SPEC_JOIN_REDIRECT:
            SpectatorNode_HandleJoinRedirect(
                from,
                packet->data.spec_redirect.redirect_ip,
                packet->data.spec_redirect.redirect_port);
            break;

        case CtrlMsg::SPEC_HEARTBEAT:
            SpectatorNode_HandleHeartbeat(from);
            break;

        case CtrlMsg::SPEC_LEAVE:
            SpectatorNode_HandleLeave(from);
            break;

        case CtrlMsg::SPEC_SESSION_END:
            SpectatorNode_HandleSessionEnd(from);
            break;

        case CtrlMsg::SPEC_SNAPSHOT_REQ:
            // Host-side only in practice: the handler walks g_state.subscribers,
            // which is empty in any process that is not hosting spectators, so
            // a stray packet is a no-op rather than a state change.
            SpectatorNode_HandleSnapshotReq(
                from,
                packet->data.spec_snapshot_req.anchor_frame,
                packet->data.spec_snapshot_req.match_index,
                packet->data.spec_snapshot_req.flags);
            break;

        default:
            break;
    }
}
