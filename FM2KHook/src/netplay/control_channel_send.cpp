// Control Channel -- packet builders (ControlChannel_Send* convenience fns).
// Split from control_channel.cpp; each fills a CtrlPacket and hands it to
// ControlChannel_Send. Shares state via control_channel_internal.h.
// ENGINE-AGNOSTIC.
#include "control_channel.h"
#include "control_channel_internal.h"
#include "host_clock.h"
#include <SDL3/SDL_log.h>
#include <cstring>

// =============================================================================
// CONTROL CHANNEL - CONVENIENCE FUNCTIONS
// =============================================================================

void ControlChannel_SendHello(uint8_t player_id, uint32_t game_hash) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::HELLO;
    pkt.data.hello.version = NETPLAY_PROTOCOL_VERSION;
    pkt.data.hello.player_id = player_id;
    pkt.data.hello.game_hash = game_hash;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent HELLO (player=%d, hash=0x%08X)",
                player_id, game_hash);
}

void ControlChannel_SendHelloAck(uint8_t player_id) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::HELLO_ACK;
    pkt.data.hello.version = NETPLAY_PROTOCOL_VERSION;
    pkt.data.hello.player_id = player_id;
    ControlChannel_Send(pkt);

    g_connected = true;
    g_connected_at_ms = GetTimeMs();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent HELLO_ACK, connected!");
}

void ControlChannel_SendPing() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::PING;
    pkt.data.ping.send_ms = GetTimeMs();  // overlaps sync.frame -- send time for RTT
    // Carry this node's host-clock send stamp + current battle frame so the peer can
    // sync its clock and project our frame (jitter-free frame-advantage).
    uint64_t h = fm2k::hostclock::Enabled() ? fm2k::hostclock::StampOutbound() : 0;
    pkt.data.ping.host_us_lo = (uint32_t)(h & 0xFFFFFFFFu);
    pkt.data.ping.host_us_hi = (uint32_t)(h >> 32);
    pkt.data.ping.frame      = fm2k::hostclock::Enabled() ? fm2k::hostclock::LocalFrame() : 0;
    ControlChannel_Send(pkt);
    g_ping_send_time = GetTimeMs();
}

void ControlChannel_SendCursor(uint8_t x, uint8_t y) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CSS_CURSOR;
    pkt.data.cursor.x = x;
    pkt.data.cursor.y = y;
    ControlChannel_Send(pkt);
}

void ControlChannel_SendCharSelect(uint8_t slot, uint8_t color) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CSS_CHAR_SELECT;
    pkt.data.character.slot = slot;
    pkt.data.character.color = color;
    ControlChannel_Send(pkt);
}

void ControlChannel_SendCharLock(uint8_t slot, uint8_t color) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CSS_LOCK;
    pkt.data.character.slot = slot;
    pkt.data.character.color = color;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent CSS_LOCK (slot=%d, color=%d)",
                slot, color);
}

void ControlChannel_SendCharUnlock() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CSS_UNLOCK;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent CSS_UNLOCK");
}

void ControlChannel_SendCSSStart() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CSS_START;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent CSS_START");
}

void ControlChannel_SendBattleReady() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_READY;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent BATTLE_READY");
}

void ControlChannel_SendChat(const char* text) {
    if (!text) return;
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::CHAT;
    // Truncate to 23 chars + guaranteed NUL terminator.
    std::strncpy(pkt.data.chat.text, text, sizeof(pkt.data.chat.text) - 1);
    pkt.data.chat.text[sizeof(pkt.data.chat.text) - 1] = '\0';
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: Sent CHAT \"%s\"", pkt.data.chat.text);
}

void ControlChannel_SendBattleAck() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_ACK;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent BATTLE_ACK");
}

// cfg_digest is supplied by the caller (the netplay layer) rather than read
// here, so this TU stays ENGINE-AGNOSTIC -- the digest is computed from FM2K
// engine globals and must not be reachable from the transport code.
void ControlChannel_SendBattleEntering(uint32_t swap_frame, uint8_t epoch,
                                       uint8_t flags, uint32_t cfg_digest) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_ENTERING;
    pkt.data.sync.frame = swap_frame;
    pkt.data.sync.epoch = epoch;
    pkt.data.sync.flags = flags;
    pkt.data.sync.cfg_digest = cfg_digest;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: Sent BATTLE_ENTERING (swap_frame=%u epoch=%u flags=0x%02X cfg=0x%08X)",
                swap_frame, epoch, flags, cfg_digest);
}

void ControlChannel_SendBattleStart(uint32_t start_frame) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_START;
    pkt.data.sync.frame = start_frame;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent BATTLE_START (frame=%u)",
                start_frame);
}

void ControlChannel_SendBattleEnd(uint32_t swap_frame, uint8_t epoch,
                                  uint8_t flags) {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::BATTLE_END;
    pkt.data.sync.frame = swap_frame;
    pkt.data.sync.epoch = epoch;
    pkt.data.sync.flags = flags;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: Sent BATTLE_END (swap_frame=%u epoch=%u flags=0x%02X)",
                swap_frame, epoch, flags);
}

void ControlChannel_SendHostConfig(uint32_t selected_stage,
                                   uint32_t round_count,
                                   uint32_t round_time_sec,
                                   uint32_t game_speed_pct,
                                   uint8_t  socd_mode)
{
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::HOST_CONFIG;
    pkt.data.host_config.selected_stage  = selected_stage;
    pkt.data.host_config.round_count     = round_count;
    pkt.data.host_config.round_time_sec  = round_time_sec;
    pkt.data.host_config.game_speed_pct  = game_speed_pct;
    pkt.data.host_config.socd_mode       = socd_mode;
    ControlChannel_Send(pkt);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ControlChannel: Sent HOST_CONFIG (stage=%u rounds=%u time=%u speed=%u socd=%u)",
                selected_stage, round_count, round_time_sec, game_speed_pct,
                (unsigned)socd_mode);
}

void ControlChannel_SendDisconnect() {
    CtrlPacket pkt = {};
    pkt.header.type = CtrlMsg::DISCONNECT;
    ControlChannel_Send(pkt);

    g_connected = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ControlChannel: Sent DISCONNECT");
}
