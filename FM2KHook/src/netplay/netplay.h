// Netplay - State Machine + GekkoNet Integration
// Two-layer networking:
//   1. Control Channel (always active when connected) - CSS sync, transitions
//   2. GekkoNet Session (battle only) - full rollback netcode
#pragma once

#include "netplay_state.h"
#include <cstdint>

// =============================================================================
// LIFECYCLE
// =============================================================================

// Initialize the netplay system (control channel + state machine)
// player_index: 0 = P1 (host), 1 = P2 (client)
// local_port: UDP port to listen on
// remote_addr: "ip:port" of remote player
// Returns true on success
bool Netplay_Init(int player_index, uint16_t local_port, const char* remote_addr);

// Spectator-mode init. No HELLO/HELLO_ACK handshake — we're a viewer, not a
// player. Sets up the multiplexed UDP socket bound to local_port with
// host_addr ("ip:port") as the latched remote, registers OnControlMessage
// for SPEC_JOIN_ACK / SPEC_JOIN_REDIRECT dispatch, and sends SPEC_JOIN_REQ
// to start the subscription.
bool Netplay_InitAsSpectator(uint16_t local_port, const char* host_addr);

// Shutdown everything (control channel + GekkoNet if active)
void Netplay_Shutdown();

// =============================================================================
// STATE MACHINE
// =============================================================================

// Get current netplay state
NetplayState Netplay_GetState();

// Set netplay state (for internal use, triggers logging)
void Netplay_SetState(NetplayState state);

// Get CSS state (cursor positions, character locks, etc.)
const CSSState& Netplay_GetCSSState();

// Reset CSS state (call when returning from battle for rematch)
void Netplay_ResetCSSState();

// CSS sync functions - for coordinating CSS entry
bool Netplay_IsRemoteCSSReady();
void Netplay_SetLocalCSSReady(bool ready);
bool Netplay_IsCSSFullySynced();

// =============================================================================
// PER-FRAME PROCESSING (called from hooks)
// =============================================================================

// Process CSS mode frame (lockstep sync)
// Call when game_mode == 2000
// Returns true if game should advance, false if waiting for sync
bool Netplay_ProcessCSS();

// Check if CSS can advance (have remote input for current frame)
// Called by BOTH ProcessGameInputs and UpdateGameState hooks to freeze game during stalls
bool Netplay_CanAdvanceCSS();

// Poll CSS network (call during stalls to receive pending data)
void Netplay_PollCSS();

// Process battle input phase - called from Hook_ProcessGameInputs
// Polls GekkoNet, handles Save/Load events, sets synced inputs
// For rollback with multiple AdvanceEvents, runs complete frames for all but the last
// Returns true if ready to advance (AdvanceEvent received), false if waiting
bool Netplay_ProcessBattleInputPhase();

// Process menu/other mode frame (just keepalive)
// Call for other game modes
void Netplay_ProcessMenu();

// =============================================================================
// BATTLE MODE SYNC BARRIER
// =============================================================================

// Signal that we're entering battle mode (game_mode changed to 3000+).
// Sends BATTLE_ENTERING with a proposed swap_frame = current CSS frame +
// SWAP_FRAME_BUFFER; resender keeps the latest agreed (max(local, remote))
// value flowing until both sides converge. Returns immediately.
void Netplay_SignalBattleEntry();

// Boot-to-battle (FM2K_BOOT_TO_BATTLE) only: arm the battle-entry barrier when
// the CSS rendezvous was skipped, so the two direct-to-battle peers will accept
// each other's BATTLE_ENTERING instead of rejecting them as out-of-window.
// No-op if already armed (the normal CSS path). See definition for details.
void Netplay_ArmBattleEntryBarrier();

// Check if the CSS->battle swap is ready to fire: both peers have signaled
// AND the active CSS-session frame has reached the agreed swap_frame.
// Callers should perform gekko_destroy(css) -> gekko_create(battle) once
// this returns true.
bool Netplay_IsBattleSynced();

// Returns the agreed swap_frame for the CSS->battle direction (max of local
// and remote proposals). Valid once g_local_battle_entered is true.
uint32_t Netplay_GetBattleEntrySwapFrame();

// Poll for battle entry sync messages (call while waiting for sync).
// Resends BATTLE_ENTERING with the latest agreed swap_frame until remote
// acknowledges via its own BATTLE_ENTERING.
void Netplay_PollBattleSync();

// Signal that we're leaving battle mode (game_mode changed out of [3000,4000)).
// Sends BATTLE_END with a proposed swap_frame = current battle frame +
// SWAP_FRAME_BUFFER; convergence is the same max(local, remote) protocol.
void Netplay_SignalBattleEnd();

// Check if the battle->CSS swap is ready to fire: both peers have signaled
// AND the active battle-session frame has reached the agreed swap_frame.
// Callers should perform gekko_destroy(battle) -> gekko_create(css) once
// this returns true.
bool Netplay_IsBattleEndSynced();

// Returns the agreed swap_frame for the battle->CSS direction.
// Valid once g_local_battle_end_signaled is true.
uint32_t Netplay_GetBattleEndSwapFrame();

// FM95 deterministic battle-end signal: propose swap_frame from the CONFIRMED
// battle-end edge (identical both peers) instead of the per-peer live frame.
void Netplay_SignalBattleEndAtFrame(uint32_t end_frame);

// FM95 per-frame phase ring (written in the gekko advance handler): scan up to
// the confirmed horizon for the first battle->non-battle edge. Returns true
// (latched) with the deterministic end frame. Reset via the companion fn on
// each new battle session.
bool Netplay_Fm95PollConfirmedBattleEnd(uint32_t* out_end_frame);
void Netplay_Fm95ResetBattleEndScan();

// Poll for battle-end sync messages (call while waiting for sync).
void Netplay_PollBattleEndSync();

// =============================================================================
// GEKKONET SESSION LIFECYCLE
// =============================================================================

// Start GekkoNet session when entering battle mode
// Call when game_mode transitions from CSS (2000) to Battle (3000+)
// Returns true if session started successfully
bool Netplay_StartBattle();

// Start a single-instance GekkoStressSession (no network, both players local).
// Used when FM2K_STRESS_MODE=1 / g_stress_mode=true. GekkoNet artificially
// rolls back every `check_distance` frames and verifies state determinism
// via checksum. Any desync fired here is a local-determinism bug in our
// save/load or sim path — not a network issue.
// Returns true if session started successfully.
bool Netplay_StartStressBattle();

// End GekkoNet session when leaving battle mode
// Keeps control channel connected for rematch
void Netplay_EndBattle();

// =============================================================================
// SPECTATOR SESSION LIFECYCLE
// =============================================================================

// SessionKind enum is internal to netplay.cpp; re-declared here for the
// spectator-facing API. Values must match netplay.cpp's enum class.
enum class NetplaySessionKind : uint8_t {
    NONE     = 0,
    CSS      = 1,
    BATTLE   = 2,
    STRESS   = 3,
    SPECTATE = 4,
};

// Create a GekkoSpectateSession matching the host's current session phase.
// Called from SpectatorNode_HandleJoinAck after the host accepts our
// SPEC_JOIN_REQ. host_kind comes from the JOIN_ACK payload (1=CSS, 2=BATTLE);
// host_addr is the formatted "ip:port" string.
bool Netplay_StartSpectateSession(NetplaySessionKind host_kind, const char* host_addr);

// Tear down the spectator session. Called when the host signals phase change
// (BATTLE_ENTERING / BATTLE_END) or when the spectator disconnects.
void Netplay_EndSpectateSession();

// Spectator-side phase-flip handlers. Called from OnControlMessage when the
// receiver is currently a spectator and the host announced a swap.
void Netplay_OnHostBattleEntering(uint32_t swap_frame);
void Netplay_OnHostBattleEnd(uint32_t swap_frame);

// Returns true if this peer is currently running a GekkoSpectateSession.
bool Netplay_IsSpectatorSession();

// Drive the spectator's GekkoSpectateSession for one tick: poll network,
// drain session events, drain Save/Load/Advance events. On AdvanceEvent
// the input cache (g_p1_input / g_p2_input) is updated and the game's
// process_game_inputs + update_game functions run — same as the host's
// path. Returns true if the sim advanced this tick (caller renders);
// false if the session is stalled waiting for confirmed inputs.
bool Netplay_ProcessSpectatorPhase();

// (SpectatorSaveLoadMirror was tried + removed — GekkoNet's SpectatorSession
// emits only AdvanceEvent, never Save/Load. See spectator_session.cpp
// UpdateSession. The desync fix moved to INPUT_BATCH redundancy +
// gap-detection on the spectator-node side instead.)

// Returns the current session kind (for trampoline branching).
NetplaySessionKind Netplay_GetSessionKind();

// Returns the active GekkoNet session pointer (or nullptr). Used by
// SpectatorNode_HandleJoinReq to add late joiners as GekkoSpectator actors.
struct GekkoSession;
GekkoSession* Netplay_GetActiveSession();

// Legacy API (delegates to StartBattle/EndBattle)
bool Netplay_StartGekkoSession();
void Netplay_StopGekkoSession();

// Check if GekkoNet session is ready (synced with remote)
bool Netplay_IsSessionReady();

// Poll GekkoNet for events without advancing game
void Netplay_PollGekkoNet();

// =============================================================================
// CHAT (peer-to-peer over control channel)
// =============================================================================

// Send a chat message from the local player to the remote peer. Truncated
// to 23 chars + NUL. Safe to call when disconnected (dropped silently).
void Netplay_SendChatMessage(const char* text);

// Push a received/sent chat message into the local ring. Called by
// OnControlMessage (from_remote=true) and Netplay_SendChatMessage
// (from_remote=false) so the local UI sees both sides of the conversation.
void Netplay_PushChatMessage(bool from_remote, const char* text);

struct ChatEntry {
    bool     from_remote;
    uint64_t timestamp_ms;      // GetTickCount64()
    char     text[24];
};

// Pop the oldest unread chat entry. Returns false if the ring is empty.
bool Netplay_PopChatMessage(ChatEntry* out);

// =============================================================================
// INPUT
// =============================================================================

// Get synchronized input for a player (0 = P1, 1 = P2)
// Only valid during BATTLE_RUNNING state
uint16_t Netplay_GetInput(int player_id);

// Get CSS input for a player (0 = P1, 1 = P2)
// Returns remote player's input via control channel, 0 for local
uint16_t Netplay_GetCSSInput(int player_id);

// =============================================================================
// STATUS QUERIES
// =============================================================================

// Check if GekkoNet session is active
bool Netplay_IsActive();

// Check if control channel is connected
bool Netplay_IsConnected();

// Get current frame counter
uint32_t Netplay_GetFrame();

// Highest confirmed (non-rollbackable) battle frame; == live frame if no session.
uint32_t Netplay_GetConfirmedFrame();

// Get round-trip time in milliseconds
uint32_t Netplay_GetPingMs();

// Get desync and rollback counts for UI display
uint32_t Netplay_GetDesyncCount();
uint32_t Netplay_GetRollbackCount();

// Get how many frames ahead of remote we are (for frame pacing)
// Positive = we're ahead (should slow down), negative = behind
float Netplay_GetFramesAhead();
int Netplay_GetLocalDelay();

// GekkoNet network stats (ping, jitter, etc.)
#include "gekkonet.h"
GekkoNetworkStats Netplay_GetNetworkStats();

// Precise frame pacing - call after render (matches GekkoNet examples' handle_frame_time)
void Netplay_HandleFrameTime();

// =============================================================================
// RUNAHEAD MID-MATCH TOGGLE + HEARTBEAT
// =============================================================================

// F8 hotkey toggles runahead between 0 and the user-configured "on" value.
// WndProc subclass calls Netplay_RequestRunaheadToggle from the message-pump
// thread; the actual gekko_set_runahead happens on the trampoline thread via
// Netplay_PollRunaheadToggle, called at the top of every battle tick.
void Netplay_RequestRunaheadToggle();
void Netplay_PollRunaheadToggle();

// [BEAT] heartbeat. Emits at most one INFO line per ~10s of battle
// wall-clock with role/ping/jitter/FA/delay/ra/pred/rollback stats.
// Safe to call every tick — internally rate-limited. No-op outside an
// active battle session.
void Netplay_TickHeartbeat();
