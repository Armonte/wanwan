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
#include "envelope_shadow.h"
#include "savestate.h"
#include "savestate_internal.h"   // EffectAddrs, ADDR_GAME_STATE, Fletcher32
#include "../core/globals.h"
#include "../hooks/seam_free_probe.h"

#include <SDL3/SDL_log.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Current netplay frame -- the rng call-site ring keys on it.
extern uint32_t g_netplay_frame;
#include <string>

// Cumulative gameplay-seed game_rand draws, bumped by Hook_GameRand's
// gameplay branch. GLOBAL (not the anonymous namespace below) because
// hooks_rng.cpp increments it. Never reset: the ring wants deltas between
// rows, and the per-frame counter g_gameplay_rand_calls is already owned and
// reset by parity_recorder, so reusing it would race that consumer.
uint32_t g_seam_rand_total = 0;
// Cumulative RENDER-stream draws (Hook_GameRand's g_in_render_rng branch).
uint32_t g_seam_rand_render = 0;
// Per-caller cumulative split, same 8 buckets as [FULLFP] fn=. KEPT but no
// longer emitted: every gameplay draw classified into bucket 6 because
// __builtin_return_address(0) is always the 0x4139A8 wrapper, which lives
// inside the character_state_machine range -- the classifier needs the
// caller-OF-caller to say anything, and the frame walk that gets it is only
// safe on the (slow) rng-trace path. Left in place so the counter and the
// [FULLFP] view keep sharing one classifier.
uint32_t g_seam_rand_by_fn[8] = {0,0,0,0,0,0,0,0};

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

// ---- broad memory snapshot ----------------------------------------------
// See seam_trace.h. Fixed range chosen to span every FM2K global the tree
// references: round_end_flag 0x424718, input history 0x4280E0, afterimage
// 0x447930, effect sys 0x4456B0, object pool 0x4701E0, char slots 0x4D1D90
// (+8*0x0E03F), menu input state 0x541F80. 0x420000..0x580000 covers all of
// them with headroom.
constexpr uintptr_t MEMSNAP_LO   = 0x00420000;
constexpr uintptr_t MEMSNAP_HI   = 0x00580000;
constexpr size_t    MEMSNAP_SIZE = MEMSNAP_HI - MEMSNAP_LO;   // 1.375 MB
// Only the frames straddling the divergence. Every violation ever observed
// has been at frame 827, so 825..828 brackets it without paying for the match.
constexpr int MEMSNAP_LO_FRAME = 826;   // last agreeing frame
constexpr int MEMSNAP_HI_FRAME = 827;   // first diverging frame
// MATCH GATE -- load-bearing. g_netplay_frame RESTARTS every battle, so
// frames 826/827 exist in EVERY match. Without this the budget is spent
// entirely inside match 1 and the run yields 12 snapshots of a match that
// never violates (observed: every .bin from the first attempt was m1).
constexpr uint16_t MEMSNAP_MATCH = 3;
constexpr size_t MEMSNAP_MAX = 16;   // ~22 MB resident, dumped at teardown
struct MemSnap { int32_t frame; uint8_t replay; uint16_t match; uint8_t* bytes; };
MemSnap g_memsnap[MEMSNAP_MAX];
size_t  g_memsnap_count = 0;

// ---- script-VM opcode ring ----------------------------------------------
// See seam_trace.h. `bytes` is the raw script blob AT THE VM CURSOR: if two
// passes sit at the same (script_idx, item_idx) yet read different bytes, the
// divergence is heap-resident script data -- the one place the full
// data-segment diff structurally could not look.
struct OpEntry {
    uint32_t seq;
    int32_t  frame;
    uint32_t obj;
    uint32_t item_idx;
    uint32_t script_idx;
    int32_t  f3c;
    uint8_t  opcode;
    uint8_t  bytes[8];
};
constexpr size_t OPRING_CAPACITY = 96000;   // ~2.2 MB; window is ~58k entries
OpEntry  g_opring[OPRING_CAPACITY];
size_t   g_opring_count = 0;
bool     g_opring_overflow = false;

// ---- rng call-site ring -------------------------------------------------
// See seam_trace.h. Narrow window, memory-only, dumped with the save ring.
struct RngSite {
    uint16_t match;
    int32_t  frame;
    uint8_t  replay;      // 1 while gekko is resimulating
    uint32_t ra1;         // always the 0x4139A8 wrapper -- kept for proof
    uint32_t ra2;         // first plausible caller above the wrapper
    uint32_t ra3;         // second candidate (the scan is heuristic)
};
constexpr size_t RNGSITE_CAPACITY = 4096;
RngSite  g_rngsite[RNGSITE_CAPACITY];
size_t   g_rngsite_count = 0;
bool     g_rngsite_overflow = false;
// The window. Chosen around the ONLY frame this has ever fired at (827,
// every violation, every run) with room either side to see the run-up and
// the tail. Deliberately not the whole match: ~10 draws/frame * 21 frames *
// every pass that crosses it still fits the ring with headroom.
constexpr int RNGSITE_FRAME_LO = 815;
constexpr int RNGSITE_FRAME_HI = 845;

// ---- per-save ring ------------------------------------------------------
// APPEND ordering (not keyed by frame) on purpose: the whole point is to see
// the resim re-saves of a frame next to its forward save, which a
// dedupe-by-frame ring would erase. 512 entries covers the seam plus its
// rollback amplification comfortably.
struct SaveEntry {
    int32_t  frame;
    uint32_t fingerprint;
    uint32_t rng;
    // THE THREE DELIBERATE NON-ROLLBACK CARVE-OUTS (savestate_fm2k_save.cpp):
    // shake_effects, effect_sys1 (palette flash 1) and effect_sys2's
    // palette-flash-2 slice are zeroed in the saved copy and SKIPPED on load,
    // so they free-run across rollback by design. That design is why they are
    // recorded here and NOT compared: the open question for the intermittent
    // seam violation is whether a resim whose ONLY differing recorded field is
    // `rng` also differs in one of these -- i.e. whether free-running effect
    // state is steering gameplay-seed draw counts. Diagnostic columns; a
    // difference here is never itself a violation.
    uint32_t shake;
    uint32_t fx1;
    uint32_t fx2;
    // Cumulative gameplay-seed game_rand draws at save time. The per-frame
    // draw count is the delta against the previous row of the same episode,
    // which is what actually names "this resim drew a different number of
    // times" instead of leaving it inferred from a changed seed.
    uint32_t rand_total;
    uint32_t rand_render;
    // FRESH per-save region hashes. NOT SaveState_GetRegionChecksums(): those
    // are throttled to once per second (savestate_fm2k_save.cpp's
    // full_crcs_due), so reading them here returned a value up to a second
    // stale and made every row in a rollback window look identical -- which
    // is exactly the false "these regions match" reading that sent the first
    // pass of this investigation down the wrong path. Recomputed here, per
    // save, over the regions the savestate actually SAVES, so a mismatch
    // names the region whose save is incomplete. ~280 KB of Fletcher32 per
    // save (~120 us); diagnostic build only.
    uint32_t h_gs;      // game_state        (544 B)
    uint32_t h_it;      // input_tracking    (~4 KB)
    uint32_t h_obj;     // object_pool, ACTIVE slots only (~4 KB)
    uint32_t h_char;    // char_dynamic, loaded slots (~115 KB)
    uint32_t h_ai;      // afterimage_pool   (~163 KB)
    uint32_t h_lists;   // object list heads/tails + current_object_ptr
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

void SeamTrace_MemSnap(int frame, bool is_replay_save) {
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = std::getenv("FM2K_SEAM_MEMSNAP");
        s_on = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (s_on != 1) return;
    if (frame < MEMSNAP_LO_FRAME || frame > MEMSNAP_HI_FRAME) return;
    if (g_match_idx != MEMSNAP_MATCH) return;
    if (g_memsnap_count >= MEMSNAP_MAX) return;
    MemSnap& m = g_memsnap[g_memsnap_count];
    if (!m.bytes) {
        m.bytes = (uint8_t*)std::malloc(MEMSNAP_SIZE);
        if (!m.bytes) return;        // out of memory: record nothing, never crash
    }
    m.frame  = (int32_t)frame;
    m.replay = is_replay_save ? 1 : 0;
    m.match  = g_match_idx;
    std::memcpy(m.bytes, (const void*)MEMSNAP_LO, MEMSNAP_SIZE);
    ++g_memsnap_count;
}

uint16_t SeamTrace_MatchIdx() { return g_match_idx; }

void SeamTrace_NoteOpcode(uint32_t seq, int32_t frame, uint32_t obj,
                          uint32_t item_idx, uint32_t script_idx,
                          int32_t f3c, uint8_t opcode,
                          const uint8_t* script_bytes) {
    if (g_opring_count >= OPRING_CAPACITY) { g_opring_overflow = true; return; }
    OpEntry& e = g_opring[g_opring_count++];
    e.seq = seq; e.frame = frame; e.obj = obj;
    e.item_idx = item_idx; e.script_idx = script_idx;
    e.f3c = f3c; e.opcode = opcode;
    for (int i = 0; i < 8; ++i) e.bytes[i] = script_bytes ? script_bytes[i] : 0;
}

bool SeamTrace_RngSiteWanted(int frame) {
    static int s_cached = -1;
    if (s_cached < 0) {
        const char* v = std::getenv("FM2K_SEAM_RNGSITE");
        s_cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (s_cached != 1) return false;
    return frame >= RNGSITE_FRAME_LO && frame <= RNGSITE_FRAME_HI;
}

void SeamTrace_NoteRngDraw(uint32_t ra1, uint32_t ra2, uint32_t ra3) {
    if (g_rngsite_count >= RNGSITE_CAPACITY) { g_rngsite_overflow = true; return; }
    RngSite& r = g_rngsite[g_rngsite_count++];
    r.match  = g_match_idx;
    r.frame  = (int32_t)g_netplay_frame;
    // A draw made while gekko is resimulating belongs to a replay pass. The
    // save ring's rows carry the same flag, so the two views join on
    // (match, frame, replay) offline.
    r.replay = g_is_rolling_back ? 1 : 0;
    r.ra1    = ra1;
    r.ra2    = ra2;
    r.ra3    = ra3;
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
    e.shake        = rc.shake_effects;
    e.fx1          = rc.effect_sys1;
    e.fx2          = rc.effect_sys2;
    e.rand_total   = g_seam_rand_total;
    e.rand_render  = g_seam_rand_render;
    e.shake = e.fx1 = e.fx2 = 0;
    e.h_gs = e.h_it = e.h_obj = e.h_char = e.h_ai = e.h_lists = 0;
#if !defined(ENGINE_FM95)
    // FM2K_SEAM_HASH=1, DEFAULT OFF, and the default is load-bearing rather
    // than merely tidy. These hashes cost ~6 us per save, and that is enough
    // to SUPPRESS the intermittent seam_ring_check violation they exist to
    // diagnose: 0 violations in 12 fully-covered runs against a 40 % base
    // rate (p ~= 0.002). Leaving them on would turn the seamdesync gate
    // false-green -- blind to exactly the bug it is there to catch. So the
    // gate runs unperturbed and an investigator opts in per-run.
    // See docs/dev/seam_ring_intermittent.md.
    static int s_seam_hash = -1;
    if (s_seam_hash < 0) {
        const char* v = std::getenv("FM2K_SEAM_HASH");
        s_seam_hash = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (s_seam_hash == 1) {
        e.shake = Fletcher32((const uint8_t*)EffectAddrs::SHAKE_EFFECTS,
                             EffectAddrs::SHAKE_EFFECTS_SZ);
        e.fx1   = Fletcher32((const uint8_t*)EffectAddrs::EFFECT_SYS1,
                             EffectAddrs::EFFECT_SYS1_SZ);
        e.fx2   = Fletcher32((const uint8_t*)EffectAddrs::EFFECT_SYS2,
                             EffectAddrs::EFFECT_SYS2_SZ);
        e.h_gs  = Fletcher32((const uint8_t*)ADDR_GAME_STATE, SIZE_GAME_STATE);
        uint32_t it = Fletcher32((const uint8_t*)0x447EE0, 0x20);
        it ^= Fletcher32((const uint8_t*)0x447F00, 0x20);
        it ^= Fletcher32((const uint8_t*)0x447F40, 0x40);
        it ^= Fletcher32((const uint8_t*)0x4280E0, 0x800);
        it ^= Fletcher32((const uint8_t*)0x4290E0, 0x800);
        e.h_it = it;
        // Active slots only -- mirrors what Save() actually copies, so a
        // mismatch here means the SAVED bytes differ, not merely that some
        // dead slot holds different residue.
        uint32_t oh = 2166136261u;
        const uint8_t* pool = (const uint8_t*)ADDR_OBJECT_POOL;
        for (size_t i = 0; i < SaveStateData::SavedRegionCRCs::OBJ_SLOT_COUNT; ++i) {
            const uint8_t* o = pool + i * OBJECT_POOL_STRIDE;
            if (o[0] == 0) continue;
            oh ^= Fletcher32(o, OBJECT_POOL_STRIDE) + (uint32_t)i;
            oh *= 16777619u;
        }
        e.h_obj = oh;
        // STRIDED for the two big regions (char_dynamic ~115 KB, afterimage
        // ~163 KB). Full Fletcher32 over both cost ~120 us per save, and that
        // SUPPRESSED THE BUG: the same recipe that failed 4/10 on the plain
        // build passed 6/6 with the heavy hashes in (p ~= 0.047 by chance).
        // This is a timing-sensitive rollback race -- uniform per-save cost
        // shifts which frames gekko rolls back to, so a heavy instrument
        // rolls different dice rather than measuring the same ones. Sampling
        // every 16th dword keeps the screen at ~6 us. It can miss a
        // difference narrower than 64 bytes, which is a real limit of this
        // pass, not a claim of full coverage: a MISMATCH here is proof, a
        // MATCH here is only "nothing big moved".
        // Combine slots with INDEX MIXING, never a bare XOR. The bare XOR
        // read 0x00000000 at frames 825 and 826 in every pass -- not "no
        // difference" but two loaded slots whose slice bytes were identical
        // cancelling each other out. Frame 826 is the LAST AGREEING FRAME,
        // i.e. exactly the one that has to be trustworthy, and the check
        // there was vacuous. A zero that means "blind" is worse than no
        // column at all.
        uint32_t ch = 2166136261u;
        for (size_t i = 0; i < NUM_CHAR_SLOTS; ++i) {
            uintptr_t base = CHAR_SLOT_BASE + i * CHAR_SLOT_SIZE;
            if (*(const uint8_t*)base == 0) continue;
            // ROTATING EXACT SLICE. Hashing all ~115 KB exactly cost ~50 us
            // per save and SUPPRESSED the bug (5 valid runs, 0 violations,
            // against a 40% base rate); the full-region build before it did
            // the same at ~120 us. This is a Heisenbug -- any material
            // per-save cost reshuffles which frames gekko rolls back to.
            //
            // So hash ONE SIXTEENTH exactly, chosen by FRAME NUMBER rather
            // than a rolling counter. Keying on the frame is what makes the
            // result comparable: the same frame always hashes the same slice,
            // so frame 827 in a good pass and frame 827 in a bad pass are
            // hashing identical byte ranges. Across the divergent window
            // (827..832+, and the divergence persists) successive frames
            // cover successive slices, so the region gets swept without any
            // single save paying for it. ~2 us.
            constexpr size_t kSlices = 16;
            const size_t slice = (size_t)((uint32_t)frame % kSlices);
            const size_t slice_sz = CHAR_SLOT_DYNAMIC_SIZE / kSlices;
            ch ^= Fletcher32(
                (const uint8_t*)(base + CHAR_SLOT_DYNAMIC_OFFSET + slice * slice_sz),
                slice_sz) + (uint32_t)(i + 1);
            ch *= 16777619u;
        }
        e.h_char = ch;
        // Afterimage: the every-64-bytes stride read a CONSTANT 0x1952025A
        // across every frame and every pass in the run6 violation -- it was
        // sampling only bytes that never move, so it was reporting "match"
        // without ever looking at anything live. Same frame-keyed rotating
        // exact slice as char_dynamic: one sixteenth per save (~10 KB, ~4 us),
        // identical byte range for a given frame across passes, and the whole
        // region swept as the divergent window advances.
        {
            constexpr size_t kAiSlices = 16;
            const size_t sl = (size_t)((uint32_t)frame % kAiSlices);
            const size_t sz = WaveCAddrs::AFTERIMAGE_POOL_SZ / kAiSlices;
            e.h_ai = Fletcher32(
                (const uint8_t*)(WaveCAddrs::AFTERIMAGE_POOL + sl * sz), sz);
        }
        uint32_t lh = Fletcher32((const uint8_t*)WaveCAddrs::OBJECT_LIST_HEADS,
                                 WaveCAddrs::OBJECT_LIST_HEADS_SZ);
        lh ^= Fletcher32((const uint8_t*)WaveCAddrs::OBJECT_NODE_POOL,
                         WaveCAddrs::OBJECT_NODE_POOL_SZ);
        lh ^= *(const uint32_t*)WaveCAddrs::CURRENT_OBJECT_PTR;
        e.h_lists = lh;
    }
#endif
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
    // Envelope-inversion phase 1 (shadow mode) rides the same fan-out. This
    // function is the campaign's canonical "we are about to terminate, flush the
    // diagnostic rings" point -- it has exactly two callers, HandleDesyncDetected
    // and the harness auto-terminate path, which are precisely the two paths the
    // shadow dump is legal from. It is called from HERE rather than from those
    // two sites directly for one concrete reason: netplay_battle_events.cpp is at
    // 999 lines and the 1000-line-per-TU rule is a hard rule. Same precedent as
    // SeamFreeProbe_LogSummary above. Dark by default: on an unarmed run this is
    // one all-zero summary line and no file.
    EnvelopeShadow_Dump(player_index, reason);

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
                    "vm_hash,vm_total,vm_live,game_mode,shake,fx1,fx2,"
                    "rand_total,rand_render,h_gs,h_it,h_obj,h_char,h_ai,h_lists\n");
    const size_t start = (g_ring_count < SEAM_RING_CAPACITY) ? 0 : g_ring_head;
    for (size_t i = 0; i < g_ring_count; ++i) {
        const SaveEntry& e = g_ring[(start + i) % SEAM_RING_CAPACITY];
        std::fprintf(f,
            "SV,%u,0,%d,%u,%u,0x%08X,0x%08X,%u,%u,%u,%u,%u,0x%04X,0x%04X,"
            "0x%08X,%u,%u,%u,0x%08X,0x%08X,0x%08X,%u,"
            "%u,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
            (unsigned)g_match_idx,
            e.frame, e.replay, e.rolling_back, e.fingerprint, e.rng,
            e.p1_hp, e.p2_hp, e.round_timer, e.game_timer, e.buf_idx,
            e.p1_input, e.p2_input, e.vm_hash, e.vm_total, e.vm_live,
            e.game_mode, e.shake, e.fx1, e.fx2, e.rand_total,
            e.rand_render, e.h_gs, e.h_it, e.h_obj, e.h_char, e.h_ai,
            e.h_lists);
    }
    for (size_t i = 0; i < g_window_count; ++i) {
        const WindowEntry& w = g_window[i];
        const SaveEntry& e = w.e;
        std::fprintf(f,
            "WN,%u,%u,%d,%u,%u,0x%08X,0x%08X,%u,%u,%u,%u,%u,0x%04X,0x%04X,"
            "0x%08X,%u,%u,%u,0x%08X,0x%08X,0x%08X,%u,"
            "%u,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
            (unsigned)w.match, (unsigned)w.seg,
            e.frame, e.replay, e.rolling_back, e.fingerprint, e.rng,
            e.p1_hp, e.p2_hp, e.round_timer, e.game_timer, e.buf_idx,
            e.p1_input, e.p2_input, e.vm_hash, e.vm_total, e.vm_live,
            e.game_mode, e.shake, e.fx1, e.fx2, e.rand_total,
            e.rand_render, e.h_gs, e.h_it, e.h_obj, e.h_char, e.h_ai,
            e.h_lists);
    }
    // RNG call-site rows. Separate section, own header, so the offline tool
    // reads them without disturbing the SV/WN width contract.
    if (g_rngsite_count) {
        std::fprintf(f, "# rng draw call sites (window %d..%d)%s\n",
                     RNGSITE_FRAME_LO, RNGSITE_FRAME_HI,
                     g_rngsite_overflow ? " -- RING OVERFLOWED, TRUNCATED" : "");
        std::fprintf(f, "kind,match,frame,replay,ra1,ra2,ra3\n");
        for (size_t i = 0; i < g_rngsite_count; ++i) {
            const RngSite& r = g_rngsite[i];
            std::fprintf(f, "RS,%u,%d,%u,0x%08X,0x%08X,0x%08X\n",
                         (unsigned)r.match, r.frame, (unsigned)r.replay,
                         r.ra1, r.ra2, r.ra3);
        }
    }
    if (g_opring_count) {
        std::fprintf(f, "# script-VM opcodes (match 3 seam window)%s\n",
                     g_opring_overflow ? " -- RING OVERFLOWED, TRUNCATED" : "");
        std::fprintf(f, "kind,seq,frame,obj,item,script,f3c,opcode,bytes\n");
        for (size_t i = 0; i < g_opring_count; ++i) {
            const OpEntry& e = g_opring[i];
            std::fprintf(f,
                "OP,%u,%d,0x%08X,%u,%u,%d,0x%02X,"
                "%02X%02X%02X%02X%02X%02X%02X%02X\n",
                e.seq, e.frame, e.obj, e.item_idx, e.script_idx, e.f3c,
                e.opcode, e.bytes[0], e.bytes[1], e.bytes[2], e.bytes[3],
                e.bytes[4], e.bytes[5], e.bytes[6], e.bytes[7]);
        }
    }
    std::fclose(f);
    // Raw snapshots beside the CSV. Named so the offline differ can join them
    // to the SV rows by (match, frame, replay) and to their pass by ordinal.
    for (size_t i = 0; i < g_memsnap_count; ++i) {
        const MemSnap& m = g_memsnap[i];
        if (!m.bytes) continue;
        char sp[512];
        std::snprintf(sp, sizeof(sp), "%s.memsnap%02u_m%u_f%d_r%u.bin",
                      filename, (unsigned)i, (unsigned)m.match, m.frame,
                      (unsigned)m.replay);
        if (FILE* sf = std::fopen(sp, "wb")) {
            std::fwrite(m.bytes, 1, MEMSNAP_SIZE, sf);
            std::fclose(sf);
        }
    }
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
