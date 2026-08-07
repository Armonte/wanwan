// Control Channel - Multiplexed UDP Socket for Netplay
// Shares single socket between control messages and GekkoNet
#pragma once

#include "netplay_state.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>

// Forward declaration for GekkoNet
struct GekkoNetAdapter;

// =============================================================================
// SOCKET MANAGEMENT (shared between control channel and GekkoNet)
// =============================================================================

// Initialize the shared UDP socket
// Called at DLL load, stays open for entire session
bool NetSocket_Init(uint16_t local_port, const char* remote_addr);

// Shutdown the shared socket
// Called at DLL unload
void NetSocket_Shutdown();

// Check if socket is initialized
bool NetSocket_IsInitialized();

// Get the raw socket handle (for debugging)
SOCKET NetSocket_GetHandle();

// Get the bound local UDP port (host byte order). Returns 0 if not initialized.
uint16_t NetSocket_GetLocalPort();

// Get remote address info (for GekkoNet adapter)
const sockaddr_in* NetSocket_GetRemoteAddr();
// Canonical family-aware actor string for the current peer (v4 byte-identical
// to the legacy form, v6 "[..]:port"). Use this for gekko_add_actor.
std::string NetSocket_GetRemoteActorString();

// Pin the CURRENT learned peer address as the gekko actor identity (call
// right after handing its string to gekko_add_actor). Keeps the in-flight
// gekko session alive across a carrier-NAT mid-session port remap: inbound
// same-IP packets are stamped with the pinned addr (gekko accepts +
// RTT-matches), outbound sends addressed to it follow the live learned
// mapping (task #52).
void NetSocket_PinGekkoActorAddr();
void NetSocket_ClearGekkoActorPin();

// =============================================================================
// CONTROL CHANNEL (0xCC-prefixed packets)
// =============================================================================

// Poll for incoming control packets and process them
// Should be called every frame when connected
// Updates CSS state, handles connection keepalive
void ControlChannel_Poll();

// Borrow the underlying UDP SOCKET for nat_traversal/STUN. Returns
// INVALID_SOCKET if the channel hasn't been initialized. Caller must
// not closesocket() -- the socket is owned by control_channel.
SOCKET ControlChannel_GetSocket();

// Latch a verified peer address into the slot used by ControlChannel_Send.
// Called from nat_traversal when the first authenticated CTRL_PUNCH packet
// lands -- the existing 0xCC HELLO/HELLO_ACK loop then sends to the right
// place. Idempotent; subsequent calls overwrite (which is fine, the peer
// addr should only update on legitimate authentication).
void ControlChannel_LatchPeerAddr(const sockaddr_storage& peer);

// Send a control packet to remote peer
void ControlChannel_Send(const CtrlPacket& packet);

// Send a control packet to a specific address, bypassing the latched
// remote-peer slot. Required for spectator-tree replies (SPEC_JOIN_ACK,
// SPEC_JOIN_REDIRECT) where the destination is a subscriber that isn't
// the gameplay opposite-peer; using ControlChannel_Send for those would
// route the ACK to the wrong client and put it in an unintended state.
void ControlChannel_SendTo(const CtrlPacket& packet, const sockaddr_in& dest);

// Send a raw datagram (no CtrlPacket header) on the shared control socket.
// Used by the Phase F spectator UDP input accelerator (0xCE
// UDP_INPUT_BATCH datagrams to subscriber addresses). The receiver's
// RawReceive demux routes by first byte, so the payload must begin with
// its own magic.
void ControlChannel_SendRawTo(const void* buf, size_t len, const sockaddr_in& dest);

// Check if control channel is connected (received HELLO_ACK)
bool ControlChannel_IsConnected();

// Get time since last received packet (for timeout detection)
uint32_t ControlChannel_GetLastRecvMs();

// Get round-trip time estimate (ping)
uint32_t ControlChannel_GetRttMs();

// Worst RTT observed since last ResetWorstRttMs() call. Used by
// Netplay_StartBattleSession to compute CCCaster-style local delay
// (delay covers the worst spike, not just the mean) at session
// creation time.
uint32_t ControlChannel_GetWorstRttMs();
uint32_t ControlChannel_GetRttSampleCount();
void     ControlChannel_ResetWorstRttMs();

// --- Input-delay negotiation (#24) -------------------------------------
// Each peer periodically broadcasts its own delay candidate over the
// control channel during CSS. Netplay_StartBattleSession then EITHER runs a
// manual FM2K_LOCAL_DELAY verbatim (purely local -- each player owns their
// delay; asymmetric is safe, just more rollback for the lower side) OR, when
// neither side pinned a value, adopts max(local, remote) so both AUTO peers
// land on the same smooth delay.

// Delay-mode formula: 0 = avg ping (mean RTT), 1 = peak ping (worst
// RTT). Set once at init from the launcher's FM2K_DELAY_MODE env var.
void ControlChannel_SetDelayMode(int mode);
int  ControlChannel_GetDelayMode();

// This peer's delay candidate: the manual override (FM2K_LOCAL_DELAY)
// if set, otherwise computed from measured RTT per the delay mode.
// Returns -1 if no RTT has been measured yet and no manual override.
int  ControlChannel_GetLocalDelayCandidate();

// Last delay candidate received from the peer via DELAY_PROPOSAL.
// Returns -1 if none received (e.g. the peer runs an older build).
int  ControlChannel_GetRemoteDelayCandidate();

// Compute and broadcast this peer's DELAY_PROPOSAL. No-op until
// connected with a candidate available. Driven on the ping cadence.
void ControlChannel_SendDelayProposal();

// =============================================================================
// CONTROL CHANNEL - CONVENIENCE FUNCTIONS
// =============================================================================

// Send HELLO to initiate connection
void ControlChannel_SendHello(uint8_t player_id, uint32_t game_hash);

// Send HELLO_ACK to accept connection
void ControlChannel_SendHelloAck(uint8_t player_id);

// Send PING for keepalive
void ControlChannel_SendPing();

// CSS raw inputs were sent via the control channel as a custom CCCaster-style
// ring buffer. They now ride a dedicated GekkoNet CSS session (prediction=0
// lockstep). The CtrlMsg::CSS_INPUT enum and the css_input payload struct are
// kept as deprecated stubs for backward-compat with peers running older code.

// Send CSS cursor position update [deprecated - use CSS input instead]
void ControlChannel_SendCursor(uint8_t x, uint8_t y);

// Send CSS character selection (highlighted, not locked)
void ControlChannel_SendCharSelect(uint8_t slot, uint8_t color);

// Send CSS character lock (confirmed selection)
void ControlChannel_SendCharLock(uint8_t slot, uint8_t color);

// Send CSS character unlock (cancelled selection)
void ControlChannel_SendCharUnlock();

// Send CSS start signal (both players synced, start counting frames NOW)
void ControlChannel_SendCSSStart();

// Send battle ready signal (for CSS sync). No payload -- just a CSS-sync
// rendezvous signal. Input delay is negotiated separately over
// DELAY_PROPOSAL (see ControlChannel_SendDelayProposal) so both peers
// run an identical value rather than each picking its own.
void ControlChannel_SendBattleReady();

// Send battle acknowledgment
void ControlChannel_SendBattleAck();

// Send battle entering signal (game_mode changed to battle).
// swap_frame carries the proposed CSS-session frame at which to swap CSS
// session for battle session. Both peers compute swap_frame = local_css_frame
// + SWAP_FRAME_BUFFER and exchange; receiver takes max(local, remote).
// epoch = barrier instance counter (0 for legacy peers); flags bit0 =
// sender has completed this barrier (see CtrlPacket.sync).
void ControlChannel_SendBattleEntering(uint32_t swap_frame, uint8_t epoch = 0,
                                       uint8_t flags = 0);

// Send battle start (both sides ready)
void ControlChannel_SendBattleStart(uint32_t start_frame);

// Send battle end (game_mode left the [3000,4000) range).
// swap_frame carries the proposed battle-session frame at which to destroy
// the battle session and create a CSS session for the next match. Same
// max(local, remote) convergence as BATTLE_ENTERING; same epoch/flags
// semantics.
void ControlChannel_SendBattleEnd(uint32_t swap_frame, uint8_t epoch = 0,
                                  uint8_t flags = 0);

// Host pushes its current match settings (selected stage, round count, time
// limit, game speed, SOCD mode) to client + spectators. Receiver mem-writes
// the address-mapped fields and adopts SOCD mode locally. Use 0xFFFFFFFF /
// 0 / 0xFF for "leave at default; don't write" on per-field basis.
void ControlChannel_SendHostConfig(uint32_t selected_stage,
                                   uint32_t round_count,
                                   uint32_t round_time_sec,
                                   uint32_t game_speed_pct,
                                   uint8_t  socd_mode);

// Send clean disconnect
void ControlChannel_SendDisconnect();

// Send a short chat message to the remote peer (truncated to 23 chars + NUL).
// Peer-to-peer, low-latency -- intended for in-match quick chat only.
// Full chat (history, lobby-scoped) goes over the lobby TCP channel when
// the matchmaking server lands.
void ControlChannel_SendChat(const char* text);

// =============================================================================
// GEKKONET CUSTOM ADAPTER
// Creates an adapter that shares our socket and filters control packets
// =============================================================================

// Create custom GekkoNet adapter that uses our shared socket
// Control packets (0xCC prefix) are filtered out and queued for ControlChannel_Poll
// GekkoNet packets are passed through normally
GekkoNetAdapter* CreateMultiplexAdapter();

// Destroy the custom adapter (just nulls the static, doesn't free socket)
void DestroyMultiplexAdapter();

// =============================================================================
// CALLBACKS - Set by netplay.cpp to handle received control messages
// =============================================================================

// Callback function type for control messages. Source address is included so
// spectator-tree join paths (SPEC_JOIN_REQ from unknown peers, SPEC_JOIN_ACK
// from upstream) can identify the peer without relying on the latched
// remote_sockaddr used for gameplay.
typedef void (*ControlMsgCallback)(const CtrlPacket* packet, const sockaddr_in& from);

// Set callback for when control messages are received
void ControlChannel_SetCallback(ControlMsgCallback callback);

// =============================================================================
// INTERNAL HELPERS (called by netplay.cpp message handler)
// =============================================================================

// Mark connection as established (called when HELLO_ACK received)
void ControlChannel_SetConnected(bool connected);

// Handle incoming PONG to calculate RTT
void ControlChannel_HandlePong(uint32_t sent_time);
