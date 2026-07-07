// Minimal shared memory: hook -> launcher status reporting
#include "shared_mem.h"
#include "globals.h"
#include "netplay.h"
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

static HANDLE g_shared_mem_handle = nullptr;
static FM2KSharedMemData* g_shared_mem = nullptr;

bool InitializeSharedMemory() {
    char name[64];
    snprintf(name, sizeof(name), "FM2K_SharedMem_%d", GetCurrentProcessId());

    size_t size = sizeof(FM2KSharedMemData);

    g_shared_mem_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, (DWORD)size, name);

    if (!g_shared_mem_handle) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SharedMem: CreateFileMapping failed: %lu", GetLastError());
        return false;
    }

    g_shared_mem = (FM2KSharedMemData*)MapViewOfFile(
        g_shared_mem_handle, FILE_MAP_ALL_ACCESS, 0, 0, size);

    if (!g_shared_mem) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SharedMem: MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(g_shared_mem_handle);
        g_shared_mem_handle = nullptr;
        return false;
    }

    memset(g_shared_mem, 0, size);
    g_shared_mem->magic = FM2K_SHARED_MEM_MAGIC;
    g_shared_mem->version = FM2K_SHARED_MEM_VERSION;
    g_shared_mem->player_index = (uint8_t)g_player_index;
    g_shared_mem->match_p1_char_id = 0xFFFFFFFFu;
    g_shared_mem->match_p2_char_id = 0xFFFFFFFFu;
    g_shared_mem->match_stage_id   = 0xFFFFFFFFu;
    g_shared_mem->match_chars_seq  = 0;
    g_shared_mem->ui_wins = g_shared_mem->ui_losses = g_shared_mem->ui_draws = -1;
    g_shared_mem->ui_vs_wins = g_shared_mem->ui_vs_losses = g_shared_mem->ui_vs_draws = -1;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "SharedMem: Created '%s' (%zu bytes)", name, size);
    return true;
}

void CleanupSharedMemory() {
    if (g_shared_mem) {
        UnmapViewOfFile(g_shared_mem);
        g_shared_mem = nullptr;
    }
    if (g_shared_mem_handle) {
        CloseHandle(g_shared_mem_handle);
        g_shared_mem_handle = nullptr;
    }
}

FM2KSharedMemData* GetSharedMemory() {
    return g_shared_mem;
}

void SharedMem_Update() {
    if (!g_shared_mem) return;

    g_shared_mem->player_index = (uint8_t)g_player_index;
    g_shared_mem->game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    g_shared_mem->frame_number = Netplay_GetFrame();
    g_shared_mem->rollback_count = Netplay_GetRollbackCount();
    g_shared_mem->desync_count = Netplay_GetDesyncCount();
    g_shared_mem->frames_ahead = Netplay_GetFramesAhead();
    g_shared_mem->rng_seed = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;

    g_shared_mem->netplay_state = Netplay_IsActive() ? 2 :
                                  Netplay_IsConnected() ? 1 : 0;
    g_shared_mem->session_ready = Netplay_IsSessionReady() ? 1 : 0;
}

void SharedMem_PublishMatchOutcome(FM2KMatchOutcome outcome) {
    if (!g_shared_mem) return;
    g_shared_mem->match_outcome     = static_cast<uint8_t>(outcome);
    // Bump the seq AFTER writing the outcome value. Launcher polls in a
    // separate process; a write tear (seq incremented but outcome still
    // stale) would cause it to miss the new value. Modern Win32 has
    // strong memory ordering for x86 stores, so a release-after-store
    // pattern is sufficient.
    g_shared_mem->match_outcome_seq += 1;
}

void SharedMem_PublishUiStats(int32_t wins, int32_t losses, int32_t draws,
                              int32_t vs_wins, int32_t vs_losses, int32_t vs_draws,
                              const char* peer_nick_utf8,
                              const char* my_nick_utf8) {
    if (!g_shared_mem) return;
    g_shared_mem->ui_wins      = wins;
    g_shared_mem->ui_losses    = losses;
    g_shared_mem->ui_draws     = draws;
    g_shared_mem->ui_vs_wins   = vs_wins;
    g_shared_mem->ui_vs_losses = vs_losses;
    g_shared_mem->ui_vs_draws  = vs_draws;
    auto stash = [](char* dst, size_t cap, const char* src) {
        if (cap == 0) return;
        if (!src) { dst[0] = '\0'; return; }
        size_t n = strnlen(src, cap - 1);
        memcpy(dst, src, n);
        dst[n] = '\0';
    };
    stash(g_shared_mem->ui_peer_nick, sizeof(g_shared_mem->ui_peer_nick), peer_nick_utf8);
    stash(g_shared_mem->ui_my_nick,   sizeof(g_shared_mem->ui_my_nick),   my_nick_utf8);
}

void SharedMem_PublishMatchChars(uint32_t p1_char_id, uint32_t p2_char_id,
                                 const char* p1_name_utf8,
                                 const char* p2_name_utf8) {
    if (!g_shared_mem) return;
    g_shared_mem->match_p1_char_id = p1_char_id;
    g_shared_mem->match_p2_char_id = p2_char_id;

    auto stash = [](char* dst, size_t cap, const char* src) {
        if (cap == 0) return;
        if (!src || !*src) { dst[0] = '\0'; return; }
        size_t n = strnlen(src, cap - 1);
        memcpy(dst, src, n);
        dst[n] = '\0';
    };
    stash(g_shared_mem->match_p1_char_name,
          FM2K_MATCH_CHAR_NAME_MAX, p1_name_utf8);
    stash(g_shared_mem->match_p2_char_name,
          FM2K_MATCH_CHAR_NAME_MAX, p2_name_utf8);
}

void SharedMem_PublishMatchStage(uint32_t stage_id) {
    if (!g_shared_mem) return;
    g_shared_mem->match_stage_id = stage_id;
    // Bump the chars+stage publish seq AFTER the final field of the
    // battle-start snapshot is in place. Launcher uses this as the
    // "fire match_progress now" trigger — by gating on seq advance
    // instead of value change, we avoid the rotate-window race where
    // shared-mem still holds the prev battle's chars and the launcher
    // would otherwise fire under the rotated token with stale data.
    g_shared_mem->match_chars_seq += 1;
}

void SharedMem_PublishMatchSession(uint64_t session_id,
                                   uint8_t  match_index_in_session) {
    if (!g_shared_mem) return;
    g_shared_mem->match_session_id = session_id;
    g_shared_mem->match_index_in_session = match_index_in_session;
    g_shared_mem->match_rounds_count = 0;  // fresh match, fresh rounds
}

void SharedMem_PublishRoundResult(uint8_t  winner_idx,
                                  uint16_t p1_hp_remaining,
                                  uint16_t p2_hp_remaining,
                                  uint32_t frames_elapsed) {
    if (!g_shared_mem) return;
    const uint8_t i = g_shared_mem->match_rounds_count;
    if (i >= 8) return;  // ring is bounded — silently drop overflow
    FM2KRoundResult& r = g_shared_mem->match_rounds[i];
    r.frames_elapsed   = frames_elapsed;
    r.p1_hp_remaining  = p1_hp_remaining;
    r.p2_hp_remaining  = p2_hp_remaining;
    r.winner_idx       = winner_idx;
    r._reserved[0]     = 0;
    r._reserved[1]     = 0;
    r._reserved[2]     = 0;
    g_shared_mem->match_rounds_count = i + 1;
}

void SharedMem_PublishSpectatorPunchTarget(uint32_t ip_be, uint16_t udp_port,
                                           uint16_t tcp_port) {
    if (!g_shared_mem) return;
    g_shared_mem->spectator_punch_ip_be    = ip_be;
    g_shared_mem->spectator_punch_port     = udp_port;
    g_shared_mem->spectator_punch_tcp_port = tcp_port;
    // Bump seq AFTER writing the addr fields so a hook poll racing the
    // launcher's write reads either the old (seq=N) or new (seq=N+1)
    // addr cleanly, never half-written. x86 stores are sequentially
    // consistent so no fence needed here.
    g_shared_mem->spectator_punch_seq  += 1;
}

void SharedMem_PublishExternalTcp(uint32_t ip_be, uint16_t port) {
    if (!g_shared_mem) return;
    if (g_shared_mem->tcp_stun_ext_ip_be == ip_be &&
        g_shared_mem->tcp_stun_ext_port  == port  &&
        g_shared_mem->tcp_stun_seq       != 0) {
        return;  // unchanged — don't bump seq
    }
    g_shared_mem->tcp_stun_ext_ip_be = ip_be;
    g_shared_mem->tcp_stun_ext_port  = port;
    g_shared_mem->tcp_stun_seq      += 1;
}

void SharedMem_PublishLocalV6(const uint8_t addr[16], uint16_t port_be) {
    if (!g_shared_mem) return;
    if (std::memcmp(g_shared_mem->local_v6_addr, addr, 16) == 0 &&
        g_shared_mem->local_v6_port == port_be &&
        g_shared_mem->local_v6_seq  != 0) {
        return;  // unchanged — don't bump seq
    }
    std::memcpy(g_shared_mem->local_v6_addr, addr, 16);
    g_shared_mem->local_v6_port = port_be;
    g_shared_mem->local_v6_seq += 1;
}

void SharedMem_PublishSessionKind(uint8_t kind) {
    if (!g_shared_mem) return;
    if (g_shared_mem->session_kind == kind &&
        g_shared_mem->session_kind_seq != 0) {
        return;  // unchanged — don't bump seq (launcher only forwards on change)
    }
    g_shared_mem->session_kind     = kind;
    g_shared_mem->session_kind_seq += 1;
}

void SharedMem_PublishHudScores(uint16_t p1, uint16_t p2) {
    if (!g_shared_mem) return;
    g_shared_mem->hud_score_p1 = p1;
    g_shared_mem->hud_score_p2 = p2;
    // Bump AFTER the values land so a fc_hud read that races us only
    // ever sees a fully-coherent {p1, p2, seq} triple.
    g_shared_mem->hud_score_seq += 1;
}

void SharedMem_PublishHudSystemMessage(const char* text_utf8, uint32_t ttl_ms) {
    if (!g_shared_mem) return;
    if (!text_utf8 || ttl_ms == 0) {
        g_shared_mem->hud_system_message[0] = '\0';
        g_shared_mem->hud_system_message_expiry_tick = 0;
        // Bump anyway so fc_hud notices the clear.
        g_shared_mem->hud_system_message_seq += 1;
        return;
    }
    // Truncate-with-null on overflow rather than refusing to publish.
    char* dst = g_shared_mem->hud_system_message;
    const size_t cap = sizeof(g_shared_mem->hud_system_message);
    const size_t n = strnlen(text_utf8, cap - 1);
    memcpy(dst, text_utf8, n);
    dst[n] = '\0';
    // Expiry is computed off `GetTickCount()` because both writers
    // (launcher and hook) need the same wall-clock-ish reference and
    // Windows ticks are universally available without a syscall.
    g_shared_mem->hud_system_message_expiry_tick = GetTickCount() + ttl_ms;
    g_shared_mem->hud_system_message_seq += 1;
}

void SharedMem_PublishHudSpectatorCount(uint16_t n) {
    if (!g_shared_mem) return;
    g_shared_mem->hud_spectator_count = n;
    // No seq for spectator count — fc_hud reads the raw value every
    // frame and renders if non-zero. No coherency concern: the value
    // is a 16-bit aligned write, atomic on x86.
}
