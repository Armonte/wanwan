// host_hub.h -- the seam between the real hub client and the Slint UI.
//
// This exists so the spike talks to the SAME fm2k::HubClient the shipping
// launcher does, rather than to a second implementation of the protocol
// written to match a UI. It is a sibling TU rather than more of host.cpp
// because host.cpp is already near the repo's 1000-line ceiling.
//
// IT IS OPT-IN AND HAS NO DEFAULT HOST. Nothing connects unless a host is
// passed explicitly. A UI spike that quietly dials the production hub
// would put a test client into the real lobby under the operator's own
// identity, and the launcher's own connect path carries a comment about
// exactly that failure mode.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hostgui {

// The games this launcher can start, for a match_start: stems (lowercase
// exe names, what the hub calls game_id), exe paths and engines, parallel.
void HubSetLibrary(const std::vector<std::string>& stems,
                   const std::vector<std::string>& paths,
                   const std::vector<std::string>& engines);

// Tell the hub the match is over (the game exited). No-op when idle.
void HubMatchEnded();

// Per frame while an online game runs: read the hook's match outcome
// from its shared memory (FM2K_MatchOutcome.h) and report to the hub --
// match_progress when a battle starts, match_result when one resolves.
// The no-record kinds (CSS abort, desync, hash mismatch, no VS mode) and
// a peer disconnect close the local game.
void HubPollOutcome(uint32_t game_pid);

// Sit in this game's room (its exe stem is the hub's room id; `title` is
// what the room is called). Picking a game on the rail or in GAMES calls
// this; it is remembered for the next connect, and a no-op when the hub
// is not up or the room is already the current one.
void HubJoinRoom(const char* game_id, const char* title);

// THE ROOMS ON THE RAIL, by exe stem -- the channel list. The player is
// IN all of them: their chat and rosters stream in, they appear in each
// one's user list, and switching rooms only changes which is on screen.
// Capped hub-side; sending the same list twice is harmless.
void HubSetRooms(const std::vector<std::string>& stems);

// Leave one room (the rail's right-click). Drops it from the list the hub
// holds too, so it stays gone.
void HubLeaveRoom(const std::string& stem);

// Unread chat in the room this game's stem maps to, 0 when none or when
// the hub cannot watch rooms. The count clears when that room becomes the
// one being shown (HubJoinRoom).
int HubUnreadFor(const std::string& stem);

// True once after any unread count changes. The rail draws those counts
// and is otherwise only republished when the LIBRARY changes, so without
// this the badge would not appear until something unrelated moved.
bool HubTakeUnreadDirty();

// Returns false (and connects to nothing) when `host` is null or empty.
bool HubStart(const char* host, const char* nick, const char* room);

// Drain events, refresh the UI. Call once per frame; cheap when idle.
void HubTick();

// Act on a roster row the user clicked, by its position in the last
// roster we published.
void HubChallenge(int index);

void HubSendChat(const char* text);

// Answer the match modal on the wire. Takes the same action codes the UI
// reports: 1 accept, 2 decline, 3 cancel.
void HubMatchAction(int action);

// Who the match in progress is against, and their last measured ping.
// Empty / 0 when there is no match. The end-of-match card reads these
// rather than inventing a name.
const char* HubMatchNick();
int         HubMatchRtt();

// Context-menu verbs against a roster row: spectate them, or stop showing
// their chat.
void HubSpectate(int index);
void HubIgnore(int index);

// Stronger than ignore: hides their chat AND refuses their challenges.
void HubBlock(int index);

bool HubActive();

// How many people the hub reports in a game's room, or 0 when it has no
// room / we are not connected. `game_id` is matched case-insensitively,
// since the room id is an exe stem and nobody agrees on its capitalisation.
int HubRoomUsers(const char* game_id);

// True once since the room list last changed, so the caller can republish
// the library without diffing it every frame. Consumes the flag.
bool HubTakeRoomsDirty();

void HubStop();

// Drop any existing connection and dial `host` fresh. Safe to call from
// the main loop; NOT from inside an event handler, because tearing the
// client down joins its I/O thread.
bool HubRestart(const char* host, const char* nick, const char* room);

}  // namespace hostgui
