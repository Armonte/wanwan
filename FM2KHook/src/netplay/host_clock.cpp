// host_clock.cpp -- host-clock sync + "rift" frame pacing. See host_clock.h.
#include "host_clock.h"
#include "control_channel_internal.h"   // g_rtt_ms, g_local_player_id
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdlib>

namespace fm2k { namespace hostclock {

namespace {
constexpr uint64_t US_PER_FRAME      = 10000;   // 100fps
constexpr int64_t  SNAP_THRESHOLD_US = 150000;  // re-latch offset only on a >=150ms jump
constexpr uint64_t VALIDITY_US       = 250000;  // one-way estimate rejected past 250ms => RTT/2
constexpr uint64_t OFFSET_FRESH_US   = 5000000; // offset considered stale after 5s w/o sync
constexpr int      RIFT_WINDOW       = 24;       // rift smoothing ring (matches reference)

// Peer's estimated offset so that: host_clock ≈ LocalMicros() + offset_us.
int64_t  g_offset_us       = 0;
bool     g_have_offset     = false;
uint64_t g_last_sync_local = 0;   // LocalMicros() when the offset was last refreshed
uint64_t g_last_oneway_us  = 0;   // most recent one-way latency (host-clock path)
bool     g_have_oneway     = false;

float  g_rift_ring[RIFT_WINDOW] = {0};
int    g_rift_count = 0;          // samples pushed (caps at RIFT_WINDOW for the average)
int    g_rift_head  = 0;

// Offset candidate ring: each sample = hostSendStamp - localRecv = trueOffset - transit.
// The MAX over the window (= least-delayed packet) is the stablest offset estimate;
// jitter only lowers candidates, so the windowed max rejects delayed-packet noise.
constexpr int CAND_WINDOW = 16;   // ~2s of PING+PONG samples (~8Hz)
int64_t g_cand_ring[CAND_WINDOW] = {0};
int     g_cand_count = 0;
int     g_cand_head  = 0;

// Once-per-frame gate for pacing: the outer loop calls the pacing hook many times
// per sim frame (and batch-advances several frames in one call during catch-up).
uint32_t g_last_pace_frame = 0xFFFFFFFFu;
float    g_last_pace_out   = 0.0f;

// Jitter-free frame projection: the remote stamps its current battle frame with its
// host-clock time on every PING/PONG; we project it forward by the elapsed host-clock
// time so the frame-advantage tracks the remote's PRODUCTION progress, not arrival
// jitter. g_local_frame is our own frame stamped outbound.
uint32_t g_local_frame        = 0;
uint32_t g_remote_frame       = 0;
uint64_t g_remote_frame_stamp = 0;   // host-clock µs when the remote was at g_remote_frame
bool     g_have_remote_frame  = false;
constexpr uint64_t REMOTE_FRAME_STALE_US = 1000000;  // >1s old => fall back to gekko
constexpr uint64_t PROJECT_CLAMP_US      = 500000;   // never project more than 0.5s ahead

// Pacing-jitter telemetry: accumulate the spread of the projected advantage vs
// GekkoNet's arrival-based advantage so an A/B run reports one "hunt" number.
double   g_stat_sum_g = 0, g_stat_sq_g = 0;   // gekko_frames_ahead
double   g_stat_sum_a = 0, g_stat_sq_a = 0;   // projected advantage
uint32_t g_stat_n = 0;

int g_enabled = -1;  // tri-state cache of FM2K_HOST_CLOCK

// QPC epoch captured on first use so timestamps stay small.
uint64_t g_qpc_freq  = 0;
uint64_t g_qpc_epoch = 0;
}  // namespace

uint64_t LocalMicros() {
    if (g_qpc_freq == 0) {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        g_qpc_freq  = (uint64_t)f.QuadPart;
        g_qpc_epoch = (uint64_t)c.QuadPart;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint64_t ticks = (uint64_t)c.QuadPart - g_qpc_epoch;
    // ticks * 1e6 / freq without overflow for multi-hour sessions.
    return (ticks / g_qpc_freq) * 1000000ull + (ticks % g_qpc_freq) * 1000000ull / g_qpc_freq;
}

bool Enabled() {
    if (g_enabled < 0) {
        const char* e = std::getenv("FM2K_HOST_CLOCK");
        g_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return g_enabled != 0;
}

bool IsHost() { return g_local_player_id == 0; }

uint64_t HostClockMicros() {
    return IsHost() ? LocalMicros() : (uint64_t)((int64_t)LocalMicros() + g_offset_us);
}

bool HasAccurateOffset() { return IsHost() || g_have_offset; }

uint64_t StampOutbound() { return HostClockMicros(); }

void     SetLocalFrame(uint32_t f) { g_local_frame = f; }
uint32_t LocalFrame()              { return g_local_frame; }

// Projected remote frame-advantage: localSimFrame - remoteFrameNow, where
// remoteFrameNow = last stamped remote frame + elapsed host-clock time / 10ms.
// Positive => local sim is AHEAD of the remote (slow down). Returns a large negative
// sentinel when there is no fresh remote-frame sample (caller falls back to gekko).
static float ProjectedFrameAdvantage(uint32_t local_sim_frame) {
    if (!g_have_remote_frame) return -1e9f;
    uint64_t now_hc = HostClockMicros();
    if (now_hc < g_remote_frame_stamp) return -1e9f;   // clock went backwards; skip
    uint64_t elapsed = now_hc - g_remote_frame_stamp;
    if (elapsed > REMOTE_FRAME_STALE_US) return -1e9f;  // sample too old
    if (elapsed > PROJECT_CLAMP_US) elapsed = PROJECT_CLAMP_US;
    float remote_now = (float)g_remote_frame + (float)elapsed / (float)US_PER_FRAME;
    return (float)local_sim_frame - remote_now;
}

void OnInboundTimestamp(uint64_t peer_host_clock_send_us, uint32_t peer_frame,
                        uint64_t local_recv_us) {
    // Record the remote-frame projection sample (host-clock stamp + the remote's
    // battle frame at that stamp). Both roles project the other's frame. Reject
    // out-of-order (reordered/duplicated) samples: only accept a stamp NEWER than the
    // one held, so a late older packet can't regress the projection. (A battle reset
    // makes the frame jump down but always with a newer stamp, so it's still accepted.)
    if (peer_frame != 0 &&
        (!g_have_remote_frame || peer_host_clock_send_us > g_remote_frame_stamp)) {
        g_remote_frame       = peer_frame;
        g_remote_frame_stamp = peer_host_clock_send_us;
        g_have_remote_frame  = true;
    }
    // --- offset estimation (peer only; the host IS the reference clock) ---
    if (!IsHost()) {
        // candidate = hostSendStamp - localRecv = trueOffset - transit. Push into
        // the ring and take the windowed MAX (least-delayed sample) as the offset.
        int64_t candidate = (int64_t)peer_host_clock_send_us - (int64_t)local_recv_us;
        g_cand_ring[g_cand_head] = candidate;
        g_cand_head = (g_cand_head + 1) % CAND_WINDOW;
        if (g_cand_count < CAND_WINDOW) ++g_cand_count;
        int64_t mx = g_cand_ring[0];
        for (int i = 1; i < g_cand_count; ++i) if (g_cand_ring[i] > mx) mx = g_cand_ring[i];

        if (!g_have_offset) {
            g_offset_us   = mx;
            g_have_offset = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "HostClock: offset LOCKED %lld us", (long long)g_offset_us);
        } else {
            int64_t d = mx - g_offset_us, ad = d < 0 ? -d : d;
            if (ad >= SNAP_THRESHOLD_US)   // large step => log it (clock jump / route change)
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "HostClock: offset step %lld us (delta %lld)", (long long)mx, (long long)d);
            g_offset_us = mx;              // windowed max is already jitter-stable
        }
        g_last_sync_local = local_recv_us;
    }

    // --- one-way latency (both roles) ---
    // recv-time in host-clock domain minus the send stamp = transit above the
    // windowed-min baseline (~0 for the least-delayed packet, grows with jitter/
    // congestion/asymmetry). This is what the rift's latency term reflects.
    uint64_t local_recv_hostclock =
        IsHost() ? local_recv_us : (uint64_t)((int64_t)local_recv_us + g_offset_us);
    if (local_recv_hostclock > peer_host_clock_send_us) {
        uint64_t ow = local_recv_hostclock - peer_host_clock_send_us;
        if (ow < VALIDITY_US) { g_last_oneway_us = ow; g_have_oneway = true; }
    }
}

float OneWayLatencyFrames(uint32_t rtt_ms) {
    // Honest baseline (see header): RTT/2 in frames (ms/2 / 10ms-per-frame). We do
    // NOT use the host-clock excess here -- it's biased ~one transit and would push
    // the rift toward "behind", hurting exactly the cross-region case we care about.
    return ((float)rtt_ms * 0.5f) / 10.0f;
}

float HostClockOneWayFramesDiag() {
    bool fresh = g_have_oneway &&
                 (IsHost() || (g_have_offset &&
                               (LocalMicros() - g_last_sync_local) < OFFSET_FRESH_US));
    if (fresh && g_last_oneway_us < VALIDITY_US)
        return (float)g_last_oneway_us / (float)US_PER_FRAME;
    return -1.0f;
}

float RiftFrames(uint32_t remote_confirmed_frame, uint32_t local_sim_frame,
                 uint32_t rtt_ms) {
    float one_way = OneWayLatencyFrames(rtt_ms);
    if (one_way > 30.0f) one_way = 30.0f;   // >300ms one-way is beyond useful pacing
    // Frame-advantage term, clamped: during a catch-up wait the confirmed horizon
    // can transiently lead the sim frame (and a cold session may report bogus
    // frames), which must not blow up the rift.
    int64_t adv = (int64_t)remote_confirmed_frame - (int64_t)local_sim_frame;
    if (adv >  64) adv =  64;
    if (adv < -64) adv = -64;
    // rift > 0 => local sim is BEHIND where the remote already is (speed up);
    // rift < 0 => local ahead (slow down).
    return (float)adv + one_way + 1.0f;
}

float PacingFramesAhead(uint32_t local_sim_frame, uint32_t rtt_ms,
                        float gekko_frames_ahead) {
    // Keep our outbound-stamped frame current (async PINGs read this).
    SetLocalFrame(local_sim_frame);

    // Resample ONCE per advanced sim frame. The outer loop calls this many times
    // per frame; only recompute (and advance the ring) when the sim frame changed,
    // so the smoothing window is per-frame not per-poll.
    if (local_sim_frame == g_last_pace_frame) return g_last_pace_out;
    g_last_pace_frame = local_sim_frame;

    // Primary signal: the JITTER-FREE frame-advantage projected from the remote's
    // clock-stamped frame (production time, immune to arrival jitter). Fall back to
    // GekkoNet's arrival-based frame-advantage when there's no fresh remote sample.
    float jf_adv   = ProjectedFrameAdvantage(local_sim_frame);
    bool  have_jf  = jf_adv > -1e8f;
    float advantage = have_jf ? jf_adv : gekko_frames_ahead;  // >0 => local ahead => slow down

    // Light smoothing over the window softens the small step when a fresh sample lands.
    g_rift_ring[g_rift_head] = advantage;
    g_rift_head = (g_rift_head + 1) % RIFT_WINDOW;
    if (g_rift_count < RIFT_WINDOW) ++g_rift_count;
    float sum = 0.0f;
    for (int i = 0; i < g_rift_count; ++i) sum += g_rift_ring[i];
    float out = sum / (float)g_rift_count;
    if (out >  16.0f) out =  16.0f;
    if (out < -16.0f) out = -16.0f;
    g_last_pace_out = out;

    // Telemetry: compare raw-signal spread (projected advantage vs gekko) while the
    // projection is the live signal. Lower projected stddev = less pacing hunt.
    if (have_jf) {
        g_stat_sum_g += gekko_frames_ahead; g_stat_sq_g += (double)gekko_frames_ahead * gekko_frames_ahead;
        g_stat_sum_a += jf_adv;             g_stat_sq_a += (double)jf_adv * jf_adv;
        ++g_stat_n;
    }

    static uint32_t s_log_tick = 0;
    if ((s_log_tick++ % 50u) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "HostClock: adv=%.2f(%s) smoothed=%.2f rmt_f=%u loc_f=%u hc_owl=%.2ff "
                    "rtt=%ums off=%lld%s gekko=%.2f -> ahead=%.2f",
                    advantage, have_jf ? "proj" : "gekko", out, g_remote_frame, local_sim_frame,
                    HostClockOneWayFramesDiag(), rtt_ms, (long long)g_offset_us,
                    g_have_offset ? "" : "(cold)", gekko_frames_ahead, out);
    }
    if (g_stat_n >= 500) {
        double mg = g_stat_sum_g / g_stat_n, ma = g_stat_sum_a / g_stat_n;
        double vg = g_stat_sq_g / g_stat_n - mg * mg, va = g_stat_sq_a / g_stat_n - ma * ma;
        double sg = vg > 0 ? __builtin_sqrt(vg) : 0, sa = va > 0 ? __builtin_sqrt(va) : 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "HostClock PACING SUMMARY (n=%u): gekko_std=%.2f projected_std=%.2f "
                    "hunt_reduction=%.0f%%", g_stat_n, sg, sa,
                    sg > 0 ? (1.0 - sa / sg) * 100.0 : 0.0);
        g_stat_sum_g = g_stat_sq_g = g_stat_sum_a = g_stat_sq_a = 0; g_stat_n = 0;
    }
    return out;
}

void Reset() {
    g_offset_us = 0; g_have_offset = false; g_last_sync_local = 0;
    g_last_oneway_us = 0; g_have_oneway = false;
    g_rift_count = 0; g_rift_head = 0;
    for (int i = 0; i < RIFT_WINDOW; ++i) g_rift_ring[i] = 0.0f;
    g_cand_count = 0; g_cand_head = 0;
    for (int i = 0; i < CAND_WINDOW; ++i) g_cand_ring[i] = 0;
    g_last_pace_frame = 0xFFFFFFFFu; g_last_pace_out = 0.0f;
    g_local_frame = 0; g_remote_frame = 0; g_remote_frame_stamp = 0;
    g_have_remote_frame = false;
    g_stat_sum_g = g_stat_sq_g = g_stat_sum_a = g_stat_sq_a = 0; g_stat_n = 0;
}

}}  // namespace fm2k::hostclock
