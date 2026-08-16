// seam_trace.cpp -- instruments for the 967f89f match-end-seam desync.
// DIAGNOSTIC ONLY. Nothing here is a fix and nothing here may ship enabled.
//
// THE MECHANISM (Phase 1 confirmed it directly, see
// docs/dev/matchend_seam_campaign.md, phase report seam_p1c_confirmation): the LOAD-SITE park
// (Fm2k_NeutralizeMatchEndScriptObjects, called from savestate_fm2k_load.cpp
// under the crossing_teardown predicate) fired as a function of each peer's
// OWN rollback schedule. When the restored frame predated the 902 edge the
// snapshot's type-4 VMs were unparked and the park changed them, so the resim
// ticks returned at 0x411C3F before 2 of the 12 game_rand sites -- the rng
// froze at that peer's rollback start value, the resim saves became the final
// per-frame gameplay_fingerprint, and gekko mismatched. Phase 2c DELETED that
// park. The SIM-side park (round_events.cpp, post >= 902) is deterministic,
// was never under suspicion, and stays.
//
// LOGGING CONSTRAINT: hook logging is SYNCHRONOUS (quill is hardcoded off in
// FM2KHook; every SDL_Log* is fputs+fflush). So:
//   * the per-save rings are memory-only and are written to a file only from
//     paths that are already tearing the process down (desync handler,
//     harness auto-terminate);
//   * the [SEAM] marker is episodic: one always-on OPEN line per battle
//     session, and the per-load detail lines are both FM2K_SEAM_TRACE-gated
//     and hard-capped, because a burst of crossing loads is possible inside
//     the ~26-frame battle-end window.
//
// Built for BOTH engines (netplay_desync.cpp / netplay_battle.cpp are
// engine-agnostic and call Dump/Reset); only the FM2K object-pool walk is
// engine-gated, and nothing arms the ring on FM95.
#include "seam_trace.h"
#include "savestate.h"
#include "../core/globals.h"
#include "../hooks/seam_free_probe.h"

#include <SDL3/SDL_log.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// Object pool geometry, same constants round_events.cpp uses (kept local for
// the same reason it keeps its own: this walk replicates the engine's own
// address math and must not drift onto savestate.h's shifted bases).
constexpr uintptr_t ADDR_OBJECT_POOL_SEAM = 0x004701E0;
constexpr size_t    OBJ_STRIDE_SEAM       = 382;
constexpr size_t    OBJ_COUNT_SEAM        = 1024;
constexpr ptrdiff_t OFF_OBJ_TYPE_SEAM     = 0x000;
constexpr ptrdiff_t OFF_OBJ_INIT_SEAM     = 0x152;
constexpr int       OBJ_TYPE_SCRIPT_VM_SEAM = 4;

// A2's spec: hash ACTIVE slots only. A raw whole-pool digest folds in
// inactive-slot residue, which is neither saved nor restored and therefore
// mismatches cross-peer for entirely benign reasons.
struct VmDigest {
    uint32_t hash;   // FNV-1a over (slot_index, init_state) of type-4 slots
    uint16_t total;  // type-4 slots present
    uint16_t live;   // of those, init_state in {0,1} -- i.e. still parkable
};

VmDigest ComputeVmDigest() {
    VmDigest d = { 2166136261u, 0, 0 };
#if defined(ENGINE_FM95)
    return d;   // FM2K pool geometry only; nothing arms the ring on FM95
#else
    const uint8_t* pool = (const uint8_t*)ADDR_OBJECT_POOL_SEAM;
    for (size_t i = 0; i < OBJ_COUNT_SEAM; ++i) {
        const uint8_t* obj = pool + i * OBJ_STRIDE_SEAM;
        if (*(const int*)(obj + OFF_OBJ_TYPE_SEAM) != OBJ_TYPE_SCRIPT_VM_SEAM) continue;
        const int st = *(const int*)(obj + OFF_OBJ_INIT_SEAM);
        ++d.total;
        if (st == 0 || st == 1) ++d.live;
        const uint32_t mix[2] = { (uint32_t)i, (uint32_t)st };
        const uint8_t* p = (const uint8_t*)mix;
        for (size_t b = 0; b < sizeof(mix); ++b) {
            d.hash ^= p[b];
            d.hash *= 16777619u;
        }
    }
    return d;
#endif
}

// ---- per-save ring ------------------------------------------------------
// APPEND ordering (not keyed by frame) on purpose: the whole point is to see
// the resim re-saves of a frame next to its forward save, which a
// dedupe-by-frame ring would erase. 512 entries covers the seam plus its
// rollback amplification comfortably.
struct SaveEntry {
    int32_t  frame;
    uint32_t fingerprint;
    uint32_t rng;
    uint32_t p1_hp;
    uint32_t p2_hp;
    uint32_t round_timer;
    uint32_t game_timer;
    uint32_t buf_idx;
    uint32_t vm_hash;
    uint32_t game_mode;
    uint16_t p1_input;
    uint16_t p2_input;
    uint16_t vm_total;
    uint16_t vm_live;
    uint8_t  replay;
    uint8_t  rolling_back;
};
constexpr size_t SEAM_RING_CAPACITY = 512;
SaveEntry g_ring[SEAM_RING_CAPACITY];
size_t    g_ring_head  = 0;
size_t    g_ring_count = 0;

// ---- seam WINDOW buffer (survives the per-match reset) -------------------
// Attack A9 of the Phase 2b review: the primary ring is reset at
// Netplay_StartBattle and holds 512 append-ordered entries, so on a GREEN
// multi-match run that auto-terminates in the last match, the earlier matches'
// seams -- i.e. exactly the frames under test -- have already been evicted or
// reset away. The offline "last save == first save" criterion (the bit-exact
// resim test, tools/seam_ring_check.py) then has nothing to check and reads
// green for the wrong reason.
//
// Fix: a second, append-only buffer that is NOT reset per match. It is ARMED
// at the first non-identity crossing of each match (the [SEAM] observation
// point), which backfills the most recent primary entries -- the FORWARD saves
// of the seam frames, which happened before the crossing -- and then records
// the next few hundred saves, which are the RESIM saves the criterion compares
// against. Each arm is a numbered SEGMENT, and every row carries its match
// index, because g_netplay_frame restarts per battle and comparing frame 2530
// of match 1 against frame 2530 of match 2 would be nonsense.
struct WindowEntry {
    SaveEntry e;
    uint16_t  match;
    uint16_t  seg;
};
constexpr size_t SEAM_WINDOW_CAPACITY = 2048;
// Backfill covers the forward saves of the seam neighbourhood (the crossing
// targets land a handful of frames before the 902 edge); 128 is generous.
constexpr size_t SEAM_WINDOW_BACKFILL = 128;
// Append budget per arm: the battle-end window plus its rollback
// amplification. Bounded so one match cannot starve the next.
constexpr size_t SEAM_WINDOW_APPEND   = 256;

WindowEntry g_window[SEAM_WINDOW_CAPACITY];
size_t   g_window_count     = 0;   // append-only; stops at capacity
size_t   g_window_dropped   = 0;   // entries lost to capacity (honesty counter)
size_t   g_window_remaining = 0;   // append budget left in the current segment
uint16_t g_window_seg       = 0;
uint16_t g_match_idx        = 0;
bool     g_window_armed     = false;  // armed for THIS match already?

void WindowAppend(const SaveEntry& e) {
    if (g_window_count >= SEAM_WINDOW_CAPACITY) { ++g_window_dropped; return; }
    WindowEntry& w = g_window[g_window_count++];
    w.e     = e;
    w.match = g_match_idx;
    w.seg   = g_window_seg;
}

// ---- crossing_teardown episode list -------------------------------------
struct SeamEpisode {
    int32_t  frame;
    uint32_t live_mode_pre;
    uint32_t snap_mode;
    int32_t  parkable;
    uint8_t  rolling_back;
    uint8_t  park_ran;
};
constexpr size_t SEAM_EPISODES_CAPACITY = 128;
SeamEpisode g_episodes[SEAM_EPISODES_CAPACITY];
size_t      g_episode_count     = 0;   // stored (clamped at capacity)
size_t      g_episode_seen      = 0;   // total crossings this session
size_t      g_episode_nonident  = 0;   // of those, parkable > 0
bool        g_open_logged       = false;
int         g_detail_lines      = 0;

// Detail lines are capped hard: SDL_LogError here is a synchronous
// fputs+fflush and the battle-end window can produce dozens of crossing
// loads under 0.20 loss. The counts survive in the dump regardless.
constexpr int SEAM_DETAIL_LINE_CAP = 24;

// Lane A rec 3 -- the load-site afterimage clear (savestate_fm2k_load.cpp).
// Counters are unconditional (an increment each), so a run with the trace dark
// still answers "did the last schedule-dependent load-site write fire, and via
// which half of its predicate" -- which is precisely the question p4e R3b could
// not answer. `nz` (how many non-zero +0x151 bytes the clear is about to zero)
// costs a 1024-slot walk, so it is armed only under FM2K_SEAM_TRACE.
constexpr ptrdiff_t OFF_OBJ_AFTERIMAGE_SEAM = 0x151;
size_t g_ai_seen        = 0;   // total load-site clears this session
size_t g_ai_substate    = 0;   // of those, reached via live_substate_pre >= 900
size_t g_ai_crossing    = 0;   // of those, reached via crossing_teardown
size_t g_ai_rollingback = 0;   // of those, taken with g_is_rolling_back set
int    g_ai_nz_max      = -1;  // max non-zero +0x151 census seen (-1 = never armed)
int    g_ai_detail_lines = 0;
constexpr int SEAM_AI_DETAIL_LINE_CAP = 16;

}  // namespace

bool SeamTrace_Enabled() {
    static int s_cached = -1;
    if (s_cached < 0) {
        const char* v = std::getenv("FM2K_SEAM_TRACE");
        s_cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return s_cached == 1;
}

// FM2K_SEAM_LEGACY_PARK=1 -- DIAGNOSTIC A/B LEVER ONLY. Default OFF.
//
// ON restores the pre-Phase-2c blanket load-site park, i.e. the code that
// CAUSED the match-end-seam DESYNC. It exists so a single run can prove the
// deletion is the causal variable (set it, and the desync reappears at the
// proven recipe). It is not a safety switch and it must never become a
// default.
//
// SEMANTICS NOTE, deliberate: the retired FM2K_SEAM_GUARD had the opposite
// polarity (=0 meant "no park"), and every artifact of this campaign is
// annotated with that meaning. Repurposing the NAME would make old and new
// runs indistinguishable, so the old name is retired loudly instead of
// reinterpreted -- see seam_p2b_adversarial.md A8.
//
// Parse is strict and fails to OFF: OFF is the fixed behaviour, and failing an
// unrecognised value to ON would silently re-arm the very bug this campaign
// closed for the one person trying to turn it off.
bool SeamGuard_LegacyLoadParkEnabled() {
    static int s_enabled = -1;
    if (s_enabled >= 0) return s_enabled == 1;

    // Retired name: loud, once, and otherwise ignored.
    if (const char* old_raw = std::getenv("FM2K_SEAM_GUARD")) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[SEAM] FM2K_SEAM_GUARD=\"%s\" is RETIRED and IGNORED. The blanket "
            "load-site park it gated no longer exists; use "
            "FM2K_SEAM_LEGACY_PARK=1 to restore it for an A/B",
            old_raw);
    }

    const char* raw = std::getenv("FM2K_SEAM_LEGACY_PARK");
    std::string v = raw ? raw : "";
    const size_t b = v.find_first_not_of(" \t\r\n");
    const size_t e = v.find_last_not_of(" \t\r\n");
    v = (b == std::string::npos) ? std::string() : v.substr(b, e - b + 1);
    for (char& c : v) c = (char)std::tolower((unsigned char)c);

    const char* why = nullptr;
    if (v.empty()) {
        s_enabled = 0;
        why = "default; FM2K_SEAM_LEGACY_PARK unset";
    } else if (v == "1" || v == "true" || v == "on" || v == "yes" ||
               v == "enabled") {
        s_enabled = 1;
        why = "FM2K_SEAM_LEGACY_PARK set (DIAGNOSTIC A/B)";
    } else if (v == "0" || v == "false" || v == "off" || v == "no" ||
               v == "disabled") {
        s_enabled = 0;
        why = "FM2K_SEAM_LEGACY_PARK explicitly off";
    } else {
        s_enabled = 0;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[SEAM] legacy load-site park DISABLED -- "
            "FM2K_SEAM_LEGACY_PARK=\"%s\" is not a recognised value "
            "(accepted: 1/true/on/yes/enabled, 0/false/off/no/disabled). "
            "Failing SAFE to the fixed default (OFF)",
            raw ? raw : "");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SEAM] legacy load-site park %s (%s)",
        s_enabled ? "ENABLED -- REINSTATES THE 967f89f DESYNC, diagnostic only"
                  : "disabled (fixed behaviour)",
        why);
    return s_enabled == 1;
}

void SeamTrace_OnCrossingTeardown(int frame, uint32_t live_mode_pre,
                                  uint32_t snap_mode, int parkable,
                                  bool rolling_back, bool park_ran) {
    ++g_episode_seen;
    if (parkable > 0) ++g_episode_nonident;

    if (g_episode_count < SEAM_EPISODES_CAPACITY) {
        SeamEpisode& ep = g_episodes[g_episode_count++];
        ep.frame         = (int32_t)frame;
        ep.live_mode_pre = live_mode_pre;
        ep.snap_mode     = snap_mode;
        ep.parkable      = (int32_t)parkable;
        ep.rolling_back  = rolling_back ? 1 : 0;
        ep.park_ran      = park_ran ? 1 : 0;
    }

    // ARM the seam-window buffer on the first non-identity crossing of this
    // match: backfill the seam frames' FORWARD saves out of the primary ring,
    // then let the next SEAM_WINDOW_APPEND saves (the resim re-saves) land
    // beside them. This is what survives the per-match reset, and it is what
    // tools/seam_ring_check.py reads. See the buffer comment above.
    if (!g_window_armed && parkable > 0) {
        g_window_armed    = true;
        ++g_window_seg;
        g_window_remaining = SEAM_WINDOW_APPEND;
        const size_t back  = (g_ring_count < SEAM_WINDOW_BACKFILL)
                           ? g_ring_count : SEAM_WINDOW_BACKFILL;
        // Oldest-first over the last `back` appends, preserving append order
        // so "first save" really is the earliest recorded save of a frame.
        const size_t first = (g_ring_head + SEAM_RING_CAPACITY - back)
                           % SEAM_RING_CAPACITY;
        for (size_t i = 0; i < back; ++i) {
            WindowAppend(g_ring[(first + i) % SEAM_RING_CAPACITY]);
        }
    }

    // Always-on OPEN line: ONE per battle session (SeamTrace_Reset runs at
    // Netplay_StartBattle), fired on the first crossing that is NOT an
    // identity write. That is the "this run entered the hazard" contract, and
    // it is deliberately UNGATED by FM2K_SEAM_TRACE -- the predicate remains a
    // pure observation now that Phase 2c deleted the park it used to gate.
    if (!g_open_logged && parkable > 0) {
        g_open_logged = true;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[SEAM] OPEN f=%d live=%u snap=%u parkable=%d rb=%d park_ran=%d",
            frame, live_mode_pre, snap_mode, parkable,
            rolling_back ? 1 : 0, park_ran ? 1 : 0);
    }

    if (SeamTrace_Enabled() && g_detail_lines < SEAM_DETAIL_LINE_CAP) {
        ++g_detail_lines;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[SEAM] load f=%d live=%u snap=%u parkable=%d rb=%d park_ran=%d "
            "(#%zu)",
            frame, live_mode_pre, snap_mode, parkable,
            rolling_back ? 1 : 0, park_ran ? 1 : 0, g_episode_seen);
    }
}

void SeamTrace_OnAfterimageClear(int frame, int live_substate_pre,
                                 uint32_t snap_mode, bool rolling_back,
                                 bool crossing_teardown) {
    ++g_ai_seen;
    if (live_substate_pre >= 900) ++g_ai_substate;
    if (crossing_teardown)        ++g_ai_crossing;
    if (rolling_back)             ++g_ai_rollingback;

    if (!SeamTrace_Enabled()) return;

    // How much this clear actually CHANGES. 0 = a pure identity write (the
    // sim-side clear at round_events.cpp:355 already ran on this frame's tick);
    // > 0 = this load-site write is the only one doing it, i.e. the asymmetry
    // Lane A named. The write itself is one byte per slot, so a census of the
    // same field costs the same walk.
    int nz = 0;
    const uint8_t* pool = (const uint8_t*)ADDR_OBJECT_POOL_SEAM;
    for (size_t i = 0; i < OBJ_COUNT_SEAM; ++i) {
        if (pool[i * OBJ_STRIDE_SEAM + OFF_OBJ_AFTERIMAGE_SEAM] != 0) ++nz;
    }
    if (nz > g_ai_nz_max) g_ai_nz_max = nz;

    // EPISODIC, not per-call: the interesting event is a clear that changes
    // something, so unconditional lines are capped and non-zero ones get the
    // remaining budget. Hook logging is synchronous -- never per-frame.
    if (g_ai_detail_lines < SEAM_AI_DETAIL_LINE_CAP && (nz > 0 || g_ai_seen <= 2)) {
        ++g_ai_detail_lines;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[SEAM] aiclear f=%d sub=%d snap=%u rb=%d cross=%d nz=%d (#%zu)",
            frame, live_substate_pre, snap_mode, rolling_back ? 1 : 0,
            crossing_teardown ? 1 : 0, nz, g_ai_seen);
    }
}

void SeamTrace_PushSave(int frame, bool is_replay_save, bool rolling_back) {
    if (!SeamTrace_Enabled()) return;
    const auto& rc = SaveState_GetRegionChecksums();
    const VmDigest d = ComputeVmDigest();

    SaveEntry& e = g_ring[g_ring_head];
    e.frame        = (int32_t)frame;
    e.fingerprint  = rc.gameplay_fingerprint;
    e.rng          = rc.fp_inputs.rng;
    e.p1_hp        = rc.fp_inputs.p1_hp;
    e.p2_hp        = rc.fp_inputs.p2_hp;
    e.round_timer  = rc.fp_inputs.round_timer;
    e.game_timer   = rc.fp_inputs.game_timer;
    e.buf_idx      = rc.fp_inputs.buf_idx;
    e.p1_input     = rc.fp_inputs.p1_input;
    e.p2_input     = rc.fp_inputs.p2_input;
    e.vm_hash      = d.hash;
    e.vm_total     = d.total;
    e.vm_live      = d.live;
    e.game_mode    = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    e.replay       = is_replay_save ? 1 : 0;
    e.rolling_back = rolling_back ? 1 : 0;

    g_ring_head = (g_ring_head + 1) % SEAM_RING_CAPACITY;
    if (g_ring_count < SEAM_RING_CAPACITY) ++g_ring_count;

    // Mirror into the seam-window buffer while a segment's budget lasts.
    if (g_window_remaining > 0) {
        --g_window_remaining;
        WindowAppend(e);
    }
}

void SeamTrace_Dump(int player_index, const char* reason) {
    // Counts are free and always worth having, even on a run that never armed
    // the ring -- they say whether the hazard window was entered at all.
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[SEAM] summary: crossings=%zu nonidentity=%zu ring=%zu window=%zu "
        "window_dropped=%zu segs=%u match=%u aiclear=%zu aisub=%zu "
        "aicross=%zu airb=%zu ainzmax=%d (%s)",
        g_episode_seen, g_episode_nonident, g_ring_count, g_window_count,
        g_window_dropped, (unsigned)g_window_seg, (unsigned)g_match_idx,
        g_ai_seen, g_ai_substate, g_ai_crossing, g_ai_rollingback, g_ai_nz_max,
        reason ? reason : "");
    // Paired finding: a DESYNC with in_window > 0 is the signature that would
    // justify the design's Stage 2 (character-reload suppression).
    SeamFreeProbe_LogSummary(reason);

    if (g_episode_seen == 0 && g_ring_count == 0 && g_window_count == 0) return;

    char filename[256];
    std::snprintf(filename, sizeof(filename), "FM2K_P%d_seamring.csv",
                  player_index + 1);
    FILE* f = std::fopen(filename, "w");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[SEAM] failed to open %s", filename);
        return;
    }
    // NOTE the header key change: `guard=` (the retired FM2K_SEAM_GUARD, whose
    // load-bearing value was "0" = no park) is gone and `legacy_park=` takes
    // its place (load-bearing value "on" = the OLD, broken park). A CSV from
    // before Phase 2c and one from after are therefore never confusable.
    std::fprintf(f, "# seamring reason=%s crossings=%zu nonidentity=%zu "
                    "saves=%zu window=%zu window_dropped=%zu segs=%u "
                    "matches=%u legacy_park=%s trace=%s free_in_window=%u\n",
                 reason ? reason : "", g_episode_seen, g_episode_nonident,
                 g_ring_count, g_window_count, g_window_dropped,
                 (unsigned)g_window_seg, (unsigned)g_match_idx,
                 SeamGuard_LegacyLoadParkEnabled() ? "on" : "off",
                 SeamTrace_Enabled() ? "on" : "off",
                 SeamFreeProbe_InWindowFreeCount());
    std::fprintf(f, "# episodes (crossing_teardown loads, oldest first)\n");
    std::fprintf(f, "kind,frame,live_mode_pre,snap_mode,parkable,rb,park_ran\n");
    for (size_t i = 0; i < g_episode_count; ++i) {
        const SeamEpisode& ep = g_episodes[i];
        std::fprintf(f, "EP,%d,%u,%u,%d,%u,%u\n", ep.frame, ep.live_mode_pre,
                     ep.snap_mode, ep.parkable, ep.rolling_back, ep.park_ran);
    }
    // Both buffers use the same columns so one offline tool reads both. `SV`
    // = the primary per-match ring (current match only). `WN` = the seam
    // window, which survives the per-match reset -- group WN rows by
    // (match, seg, frame), never by frame alone: g_netplay_frame restarts at
    // every battle.
    std::fprintf(f, "# saves (append order: forward AND resim). "
                    "SV = primary ring (current match), "
                    "WN = seam window (survives the per-match reset)\n");
    std::fprintf(f, "kind,match,seg,frame,replay,rb,fingerprint,rng,p1_hp,"
                    "p2_hp,round_timer,game_timer,buf_idx,p1_input,p2_input,"
                    "vm_hash,vm_total,vm_live,game_mode\n");
    const size_t start = (g_ring_count < SEAM_RING_CAPACITY) ? 0 : g_ring_head;
    for (size_t i = 0; i < g_ring_count; ++i) {
        const SaveEntry& e = g_ring[(start + i) % SEAM_RING_CAPACITY];
        std::fprintf(f,
            "SV,%u,0,%d,%u,%u,0x%08X,0x%08X,%u,%u,%u,%u,%u,0x%04X,0x%04X,"
            "0x%08X,%u,%u,%u\n",
            (unsigned)g_match_idx,
            e.frame, e.replay, e.rolling_back, e.fingerprint, e.rng,
            e.p1_hp, e.p2_hp, e.round_timer, e.game_timer, e.buf_idx,
            e.p1_input, e.p2_input, e.vm_hash, e.vm_total, e.vm_live,
            e.game_mode);
    }
    for (size_t i = 0; i < g_window_count; ++i) {
        const WindowEntry& w = g_window[i];
        const SaveEntry& e = w.e;
        std::fprintf(f,
            "WN,%u,%u,%d,%u,%u,0x%08X,0x%08X,%u,%u,%u,%u,%u,0x%04X,0x%04X,"
            "0x%08X,%u,%u,%u\n",
            (unsigned)w.match, (unsigned)w.seg,
            e.frame, e.replay, e.rolling_back, e.fingerprint, e.rng,
            e.p1_hp, e.p2_hp, e.round_timer, e.game_timer, e.buf_idx,
            e.p1_input, e.p2_input, e.vm_hash, e.vm_total, e.vm_live,
            e.game_mode);
    }
    std::fclose(f);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "[SEAM] wrote %zu episodes + %zu ring saves + %zu window "
                 "saves to %s",
                 g_episode_count, g_ring_count, g_window_count, filename);
}

void SeamTrace_Reset() {
    g_ring_head      = 0;
    g_ring_count     = 0;
    g_episode_count  = 0;
    g_episode_seen   = 0;
    g_episode_nonident = 0;
    g_open_logged    = false;
    g_detail_lines   = 0;
    // Afterimage-clear probe: per-session like the crossing counters above.
    // g_ai_nz_max deliberately SURVIVES -- it is a session-high-water mark and
    // the whole point is that a late match's clear may be the interesting one.
    g_ai_seen        = 0;
    g_ai_substate    = 0;
    g_ai_crossing    = 0;
    g_ai_rollingback = 0;
    g_ai_detail_lines = 0;
    // Per-match arming state resets; the WINDOW BUFFER ITSELF DOES NOT. That
    // is the whole point of it (A9): a green multi-match run must still carry
    // match 1's and match 2's completed seams at auto-terminate.
    ++g_match_idx;
    g_window_armed     = false;
    g_window_remaining = 0;
}
