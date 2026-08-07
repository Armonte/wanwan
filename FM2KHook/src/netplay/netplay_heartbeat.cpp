// netplay_heartbeat.cpp -- the [BEAT] diagnostic heartbeat, split verbatim
// out of netplay.cpp (which had crept back over the 1000-line limit).
//
// Pure move: no behavior change. Every global it touches was already externed
// in netplay_internal.h / control_channel_internal.h / globals.h, so this
// needed no de-static'ing. SessionRoleStr() came along because the heartbeat
// was its only caller.

#include "netplay.h"
#include "netplay_internal.h"   // g_beat_*, g_session, g_session_kind, g_pred_window...
#include "control_channel.h"    // NetSocket_GetRemoteActorString
#include "control_channel_internal.h"  // g_gekko_rx_* stamps, g_remote_sockaddr
#include "nat_traversal.h"      // fm2k::nat::IsRelayMode
#include "globals.h"            // g_player_index, g_remote_addr

#include <SDL3/SDL_log.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>

// ============================================================================
// HEARTBEAT -- [BEAT] line every ~10s
// ============================================================================
// Single INFO line per ~10s of battle wall-clock. Echoes the values support
// staff would otherwise have to dig the entire log for: version, game id,
// role, ping, frames_ahead, configured delay, live runahead/pred, last-window
// rollback stats, stall count. Format is space-delimited key=value pairs so
// `grep '^\[BEAT\]'` and a one-liner awk script can extract a time series.
//
// Cadence: based on wall-clock ms, not frame count, so battles paused on
// menus or stalled in CSS don't shift the cadence. Reset window stats on
// emit so the avg/max describe the last 10s, not session totals.

static const char* SessionRoleStr() {
    if (g_session_kind == SessionKind::SPECTATE) return "SPEC";
    return (g_player_index == 0) ? "P1" : "P2";
}

void Netplay_TickHeartbeat() {
    if (!g_session || g_session_kind != SessionKind::BATTLE) return;

    const uint64_t now_ms = (uint64_t)GetTickCount64();
    if (g_beat_last_emit_ms == 0) {
        g_beat_last_emit_ms = now_ms;
        return;
    }

    // task #56 [WEDGE] watch: the known-bad state is "battle frame frozen,
    // prediction window exhausted, control channel still alive" -- which in
    // wild logs looks like nothing at all (BEAT keeps printing frozen bf).
    // Warn explicitly, with the two counters that split the failure in half:
    // gekko_rx_age growing = peer datagrams stopped REACHING us (socket /
    // addr-pin / path problem); gekko_rx_age small while conf is frozen =
    // packets arrive but gekko refuses to admit them (actor-string mismatch,
    // internal stall). 5s threshold: normal seams either change session kind
    // or keep bf ticking; a 5s freeze mid-battle is unambiguous.
    {
        static uint32_t s_wedge_last_bf      = 0;
        static uint64_t s_wedge_bf_stamp_ms  = 0;
        static uint64_t s_wedge_last_warn_ms = 0;
        if (g_netplay_frame != s_wedge_last_bf) {
            s_wedge_last_bf     = g_netplay_frame;
            s_wedge_bf_stamp_ms = now_ms;
        } else if (s_wedge_bf_stamp_ms != 0 &&
                   now_ms - s_wedge_bf_stamp_ms >= 5000 &&
                   now_ms - s_wedge_last_warn_ms >= 10000) {
            s_wedge_last_warn_ms = now_ms;
            GekkoNetworkStats ws = {};
            gekko_network_stats(g_session,
                                (g_player_index == 0) ? 1 : 0, &ws);
            const uint64_t rx_ms  = g_gekko_rx_last_ms.load(std::memory_order_relaxed);
            const uint64_t rx_age = rx_ms ? (now_ms - rx_ms) : 0;
            // Wire-stage counters (task #56): the stage whose tally is
            // frozen/zero names the drop point -- magic gate, addr match,
            // contiguity reject, or the msg->sync drain.
            unsigned long long wire[GEKKO_WS_COUNT] = {};
            gekko_wire_stats(wire);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[WEDGE] battle frame FROZEN for %llus at bf=%u conf=%d "
                "gekko_rx_age=%llums gekko_rx_total=%llu ping=%ums FA=%+.1f "
                "wire[magic_drop=%llu in_pkts=%llu addr_miss=%llu admit=%llu "
                "contig_rej=%llu acks_out=%llu acks_in=%llu drained=%llu] "
                "-- input exchange dead while session alive (task #56)",
                (unsigned long long)((now_ms - s_wedge_bf_stamp_ms) / 1000ULL),
                g_netplay_frame, gekko_confirmed_frame(g_session),
                (unsigned long long)rx_age,
                (unsigned long long)g_gekko_rx_total.load(std::memory_order_relaxed),
                ws.last_ping, gekko_frames_ahead(g_session),
                wire[GEKKO_WS_MAGIC_DROPS], wire[GEKKO_WS_INPUT_PKTS],
                wire[GEKKO_WS_INPUT_ADDR_MISS], wire[GEKKO_WS_INPUT_ADMIT],
                wire[GEKKO_WS_INPUT_CONTIG_REJECT], wire[GEKKO_WS_ACKS_SENT],
                wire[GEKKO_WS_ACKS_RECV], wire[GEKKO_WS_SYNC_DRAINED]);
        }
    }

    if (now_ms - g_beat_last_emit_ms < 10000ULL) return;
    g_beat_last_emit_ms = now_ms;

    const float fa = gekko_frames_ahead(g_session);
    GekkoNetworkStats stats = {};
    // Query the REMOTE peer's handle, not a hardcoded 0. For the HOST
    // (player_index 0) the LOCAL slot is handle 0, so querying 0 returned the
    // host's OWN stats -- ping pinned at 0ms. That cosmetic bug made the host
    // look latency-free while the guest (index 1, whose remote IS handle 0)
    // showed the real RTT, masking/mimicking the genuine "last_ping pins at 0
    // -> runs ahead -> one-sided rollback" failure. Mirror
    // Netplay_GetNetworkStats. For 1p offline/stress there's no remote so
    // stats stay zero; BEAT still shows local state.
    const int beat_remote_handle = (g_player_index == 0) ? 1 : 0;
    gekko_network_stats(g_session, beat_remote_handle, &stats);

    const uint32_t rb_count = g_beat_window_rb_count;
    const double   rb_avg   = rb_count ? (double)g_beat_window_rb_sum / rb_count : 0.0;
    const uint32_t rb_max   = g_beat_window_rb_max;

    // conf/gko_rx appended for #56 forensics: conf = gekko input-exchange
    // frontier (frozen conf with climbing gko_rx = admit-side stall);
    // gko_rx_age = ms since a gekko-classified datagram last reached us.
    const uint64_t beat_rx_ms = g_gekko_rx_last_ms.load(std::memory_order_relaxed);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[BEAT] bf=%u role=%s ping=%ums jit=%.1fms FA=%+.1f "
        "delay=%d ra=%d pred=%d rb_total=%u rb_win=%u rb_avg=%.1f rb_max=%u "
        "conf=%d gko_rx=%llu gko_rx_age=%llums",
        g_netplay_frame, SessionRoleStr(),
        stats.last_ping, stats.jitter, fa,
        g_local_delay,
        g_runahead_active.load(std::memory_order_acquire),
        g_pred_window,
        g_rollback_count, rb_count, rb_avg, rb_max,
        gekko_confirmed_frame(g_session),
        (unsigned long long)g_gekko_rx_total.load(std::memory_order_relaxed),
        (unsigned long long)(beat_rx_ms ? (now_ms - beat_rx_ms) : 0));

    // [RELAY-RTT-DIAG] Under relay, surface whether gekko's RTT-match address
    // (the live g_remote_sockaddr stamp) still equals the string we registered
    // with gekko_add_actor (g_remote_addr). If they diverge, gekko's
    // NetworkHealth addr.Equals(actor) fails -> last_ping pins at 0 -> this peer
    // runs ahead -> one-sided rollback -> desync amplification (relay desync
    // cluster, 2026-06-13). Diagnostic only -- no behavior change. Formatting
    // mirrors g_remote_addr's (inet_ntop + "%s:%u") so the compare is byte-exact.
    if (::fm2k::nat::IsRelayMode()) {
        // Family-aware live stamp via the SAME formatter as the actor register
        // + recv stamp, so the byte-compare is valid for v4 and v6 alike.
        std::string live = NetSocket_GetRemoteActorString();
        const bool match = (live == g_remote_addr);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[BEAT-RELAY] gekko_ping=%ums actor='%s' live_stamp='%s' addr_match=%s%s",
            stats.last_ping, g_remote_addr, live.c_str(), match ? "yes" : "NO",
            (!match || stats.last_ping == 0)
                ? "  <-- RTT-match BROKEN (ping pins at 0 -> one-sided rollback)" : "");
    }

    // Reset window so next emit describes only the next ~10s.
    g_beat_window_rb_sum   = 0;
    g_beat_window_rb_max   = 0;
    g_beat_window_rb_count = 0;
}
