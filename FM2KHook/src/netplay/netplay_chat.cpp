// Netplay chat ring: fixed-size SPSC-ish message ring + send/echo. Extracted
// VERBATIM from netplay.cpp (pure move). Self-contained leaf -- touches no
// shared netplay state, only its own ring + the public control-channel send.
// CCCaster-Style Netplay Implementation
// - Control channel for CSS input sync using INPUT DELAY (not lockstep)
// - GekkoNet for battle mode rollback
// - Uses game's internal timer for frame counting
#include "netplay.h"
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
// SIMPLIFIED STATE
// =============================================================================

enum class SimpleState : uint8_t {
    DISCONNECTED,   // No connection
    CONNECTED,      // Control channel connected
    BATTLE          // GekkoNet active for battle
};


// -----------------------------------------------------------------------------
// Chat ring. Small fixed-size SPSC-ish ring since both push and pop run on
// the launcher-UI side; the only cross-thread producer is OnControlMessage
// via the control-channel poller. Size 64 is plenty for a single match's
// worth of unread messages.
// -----------------------------------------------------------------------------
static constexpr size_t CHAT_RING_CAP = 64;
static ChatEntry g_chat_ring[CHAT_RING_CAP];
static size_t    g_chat_head = 0;
static size_t    g_chat_tail = 0;

void Netplay_PushChatMessage(bool from_remote, const char* text) {
    if (!text) return;
    ChatEntry e = {};
    e.from_remote  = from_remote;
    e.timestamp_ms = (uint64_t)GetTickCount64();
    std::strncpy(e.text, text, sizeof(e.text) - 1);
    e.text[sizeof(e.text) - 1] = '\0';

    size_t next = (g_chat_head + 1) % CHAT_RING_CAP;
    if (next == g_chat_tail) {
        // Ring full -- drop oldest to keep the newest message visible.
        g_chat_tail = (g_chat_tail + 1) % CHAT_RING_CAP;
    }
    g_chat_ring[g_chat_head] = e;
    g_chat_head = next;
}

bool Netplay_PopChatMessage(ChatEntry* out) {
    if (g_chat_tail == g_chat_head) return false;
    if (out) *out = g_chat_ring[g_chat_tail];
    g_chat_tail = (g_chat_tail + 1) % CHAT_RING_CAP;
    return true;
}

void Netplay_SendChatMessage(const char* text) {
    if (!text) return;
    if (!Netplay_IsConnected()) return;
    ControlChannel_SendChat(text);
    // Echo into local ring so the sender sees their own message in the UI.
    Netplay_PushChatMessage(/*from_remote*/ false, text);
}
