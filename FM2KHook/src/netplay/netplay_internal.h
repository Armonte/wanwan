#pragma once

// Battle RNG seed (default 0x12345678; FM2K_TEST_BATTLE_SEED overrides) --
// defined in netplay_battle.cpp, used by netplay_css.cpp's CSS->battle pin.
uint32_t Netplay_TestBattleSeed();

// netplay.cpp shared state, externed so the split netplay_*.cpp TUs can share it.
// Pure linkage move (no g_foo -> g_state.foo churn). Definitions live in netplay.cpp.
#include "netplay.h"
#include "control_channel.h"
#include "gekkonet.h"
#include <atomic>
#include <cstdint>
#include <windows.h>

// ---- shared types (were defined in netplay.cpp) ----
enum class SimpleState : uint8_t { DISCONNECTED, CONNECTED, BATTLE };
using SessionKind = NetplaySessionKind;   // public enum from netplay.h
struct PendingConfirmInput { uint32_t frame; uint16_t p1, p2; };

// ---- compile-time constants (were file-static constexpr) ----
inline constexpr int CSS_LOCAL_DELAY = 6;
// Ceiling for a manually-pinned local input delay (FM2K_LOCAL_DELAY). Not a
// gameplay clamp -- the player owns their delay; this only keeps it clear of
// GekkoNet's input ring (DEFAULT_BUFF_SIZE=128).
inline constexpr int MAX_LOCAL_DELAY = 120;
inline constexpr int CSS_CONFIRM_LOCKOUT = 150;   // block confirm for first N frames
inline constexpr uint32_t PENDING_CONFIRM_RING = 128;
inline constexpr uint32_t SWAP_FRAME_BUFFER = 8;  // barrier swap-frame lead
// FM95 battle-end swap lead. Must exceed the gekko prediction window (16) so
// that swap_frame = confirmed_end_frame + this is provably ahead of BOTH peers'
// live frames (gekko caps live <= confirmed + prediction_window), making the
// battle->CSS swap land on one deterministic frame on every node regardless of
// which peer led. 32 = window(16) + 16 margin.
inline constexpr uint32_t FM95_END_SWAP_BUFFER = 32;

// ---- runtime shared state (were file-static) ----
extern SimpleState g_simple_state;
extern SessionKind g_session_kind;
extern bool g_css_synced;  // Both peers BATTLE_READY, CSS GekkoSession ready
extern bool g_remote_css_ready;  // Remote has entered CSS
extern bool g_local_css_ready;  // We've entered CSS
extern uint16_t g_css_advance_p1;
extern uint16_t g_css_advance_p2;
extern bool     g_css_advance_ready;
extern GekkoSession* g_session;
extern bool g_session_ready;
extern uint16_t g_p1_input;
extern uint16_t g_p2_input;
extern uint32_t g_netplay_frame;
extern uint32_t g_rollback_count;
extern uint32_t g_last_rollback_frame;
extern uint32_t g_desync_count;
extern uint32_t g_last_desync_log_tick;
extern int g_local_delay;  // Computed from RTT at battle start
extern int                g_pred_window;
extern int                g_runahead_user_pref;
extern std::atomic<int>   g_runahead_active;
extern std::atomic<bool>  g_runahead_toggle_requested;
extern uint32_t g_beat_window_rb_sum;
extern uint32_t g_beat_window_rb_max;
extern uint32_t g_beat_window_rb_count;   // real rollbacks observed
extern uint64_t g_beat_last_emit_ms;
extern int g_force_desync_at_frame;
extern bool g_force_desync_inited;
extern int  g_session_delay_cached;
extern bool g_session_delay_cache_valid;
extern uint32_t g_highest_recorded_frame;
extern PendingConfirmInput g_pending_confirm[PENDING_CONFIRM_RING];
extern uint32_t g_next_confirm_flush;
extern bool     g_local_battle_entered;
extern bool     g_remote_battle_entered;
extern bool     g_battle_synced;
extern uint32_t g_battle_entry_swap_frame;     // Latest agreed swap frame.
extern bool     g_battle_entry_armed;
extern bool     g_local_battle_end_signaled;
extern bool     g_remote_battle_end_signaled;
extern bool     g_battle_end_synced;
extern uint32_t g_battle_end_swap_frame;
extern bool     g_battle_end_armed;
extern uint8_t  g_barrier_epoch;  // last assigned instance id
extern uint8_t  g_entry_epoch;  // armed entry barrier instance
extern uint8_t  g_end_epoch;  // armed end barrier instance
extern uint8_t  g_entry_done_epoch;  // last COMPLETED entry instance
extern uint8_t  g_end_done_epoch;  // last COMPLETED end instance
extern uint32_t g_end_done_ms;
extern uint32_t g_entry_local_proposal;
extern uint32_t g_entry_remote_proposal;
// Match-settings digest last ANNOUNCED by the peer in a BATTLE_ENTERING.
// 0 = not announced. The entry barrier will not complete while this
// disagrees with Netplay_MatchSettingsDigest() -- see netplay_barriers.cpp.
extern uint32_t g_entry_remote_cfg_digest;
extern bool g_received_hello;
extern bool g_received_hello_ack;
extern uint32_t g_pending_random_stage;
extern NetplaySessionKind g_pending_swap_kind;
extern uint32_t           g_pending_swap_frame;
extern LARGE_INTEGER g_perf_freq;
extern LARGE_INTEGER g_frame_start;
extern bool g_frame_timer_initialized;

// stragglers (had parens in their comments, so the bulk generator skipped them):
extern bool     g_css_active;
extern uint32_t g_css_frame;
extern uint32_t g_entry_done_ms;

// ---- re-sim profiler (FM2K_PERF_PROFILE) -- shared so netplay.cpp AND the
// split battle-phase TU can both time the rollback hot ops ----
struct PerfBucket { uint64_t ns = 0; uint32_t n = 0; };
extern const bool g_perf_on;
extern PerfBucket g_perf_save, g_perf_load, g_perf_adv;
inline uint64_t PerfQpcFreq() {
    static uint64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (uint64_t)q.QuadPart; }();
    return f;
}
inline uint64_t PerfNowNs() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((long double)c.QuadPart * 1e9L / (long double)PerfQpcFreq());
}
struct PerfScope {
    PerfBucket* b; uint64_t t0;
    explicit PerfScope(PerfBucket* x) : b(g_perf_on ? x : nullptr), t0(b ? PerfNowNs() : 0) {}
    ~PerfScope() { if (b) { b->ns += PerfNowNs() - t0; b->n++; } }
};
void PerfMaybeReport();

// ---- internal functions exposed across the split netplay_*.cpp TUs ----
// control channel (netplay_control.cpp), called from the lifecycle/battle code:
CtrlPacket BuildHostConfigPacket();
void       Netplay_BroadcastHostConfig();
// Host-only peer re-push, driven by the battle-entry barrier's retry loop
// while the two peers' settings digests disagree. No spectator fan-out.
void       Netplay_ResendHostConfigToPeer();
void       OnControlMessage(const CtrlPacket* packet, const sockaddr_in& from);
// SOCD accessors (defined in hooks/input; used by control-channel host-config):
extern "C" int  Hook_GetSOCDModePublic();
extern "C" void Hook_SetSOCDMode(int mode);
// helpers that stay in netplay.cpp but are called by the split battle TU:
void AddSubscribedSpectatorsToSession();
void ResetConfirmRing();
uint8_t NextBarrierEpoch();
bool    Netplay_StartCSSSession();
void    Netplay_EndCSSSession();
// desync diagnostics (defined in netplay.cpp), called by the battle-phase TU:
void HandleDesyncDetected(int frame, uint32_t local_chk, uint32_t remote_chk, bool synthetic);
void MaybeFireSyntheticDesync();
// cross-TU between netplay.cpp and the battle-phase TU:
bool MaybeSwapPendingSpectator(uint32_t advanced_frame);  // defined in netplay.cpp
void HandleFrameTime(float frames_ahead);                 // defined in netplay_battle_phase.cpp

// Rollback update-event handlers (netplay_battle_events.cpp), dispatched by the
// engine-agnostic driver in netplay_battle_phase.cpp. Engine-agnostic in
// STRUCTURE -- save/load route through SaveState_* (engine-split), the sim
// ticks through the engine globals; the FM2K-specific palette-flash guard
// inside the Advance handler is compiled out on FM95 (FM2K::kIsFM95), and the
// remaining FM2K addresses are env-gated diagnostics ([FM2K-DIAG]).
void Netplay_HandleSaveEvent(GekkoGameEvent* update);
void Netplay_HandleLoadEvent(GekkoGameEvent* update, int& load_events_in_batch);
void Netplay_HandleAdvanceEvent(GekkoGameEvent* update, bool& has_advance,
                                uint32_t& earliest_advance);
