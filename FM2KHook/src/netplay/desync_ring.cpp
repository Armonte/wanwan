#include "desync_ring.h"

#include "../core/globals.h"
#include "pool_delta_trace.h"

#include <SDL3/SDL_log.h>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace fm2k::desync_ring {
namespace {

// ~40 s at 100 Hz. Enough to cover a whole match's approach to a desync
// without the buffer itself being a memory event (4096 * 60 B = 240 KB).
constexpr size_t kCapacity = 4096;

constexpr int kSpawnPtrCount = 10;   // char_slot spawn_slot_ptrs, see below

struct Entry {
    int32_t  frame;
    int32_t  confirmed;
    uint32_t rng;
    uint32_t p1_hp;
    uint32_t p2_hp;
    uint32_t round;
    // The DRAW COUNTS. The divergence measured on 2026-09-06 is rng ONLY --
    // hp and round still agree at the diverging frame -- and stepping the LCG
    // from one peer's seed to the other's shows it is EXACTLY ONE extra
    // gameplay draw.
    //
    // These were originally here to test whether a render draw was escaping
    // onto the gameplay seed. That hypothesis is REFUTED: the LCG-derived draw
    // count matches gp_calls exactly (0 mismatches / 246 frames on a peer that
    // never speculated), and the three render-only call sites are reachable
    // only from render_game, which always runs inside g_in_render_rng. They are
    // kept because they still cross-check the seed walk for free.
    uint32_t gp_calls;
    uint32_t render_calls;
    // Cumulative gameplay draws split by EXACT game_rand call site (globals.h).
    // The measured divergence is ONE extra gameplay draw with hp/round still
    // identical, so the only remaining question is which site drew it. Six
    // counters answer that outright; gp_calls above only says that it happened.
    uint32_t site[kGpRandSiteCount];
    // Live objects dispatched this frame. The engine already computes this for
    // free: update_game_state @0x404CD0 zeroes 0x4246FC at the top and bumps it
    // once per object it hands to update_game_object. It matters because the
    // script VM random branch -- the source of every gameplay draw -- runs
    // inside those dispatched objects, so one peer holding one extra live
    // object is the most direct way to get one extra draw. One load per row.
    uint32_t objs;
    // Digest of the live object set (see pool_delta_trace.h). A SNAPSHOT, so
    // a re-simulated frame recomputes it from the restored pool and it stays
    // comparable between peers -- unlike a cumulative hash, which cannot be.
    uint32_t pool;
    // The live-slot bitmap itself, not just its digest. The digest says THAT
    // the pools differ; this says WHICH SLOTS, which is the whole question
    // once the pool is known to be the earliest divergence. 128 B/row.
    uint32_t pool_bits[fm2k::pool_delta_trace::kBitmapWords];
    // Object TYPE of each live slot, ascending slot order. The live SETS match
    // at the diverging frame while the digest does not, so the divergence is a
    // slot holding a different TYPE on the two peers.
    uint8_t  pool_types[fm2k::pool_delta_trace::kMaxLiveTypes];
    uint16_t pool_scripts[fm2k::pool_delta_trace::kMaxLiveTypes];
    uint32_t pool_cursors[fm2k::pool_delta_trace::kMaxLiveTypes];
    uint16_t pool_waits[fm2k::pool_delta_trace::kMaxLiveTypes];
    int32_t  pool_live;
    uint32_t sysin;      // g_system_input_state -- the rising-edge word
    uint32_t in0, in1;   // this frame's P1/P2 pad words
    uint32_t prev0, prev1;
    uint32_t spawn0[kSpawnPtrCount];   // char slot 0's child handles
    uint32_t spawn1[kSpawnPtrCount];   // char slot 1's
    uint32_t gs_src, gs_move, gs_accel, gs_pic;
    int32_t  pool_xs[fm2k::pool_delta_trace::kMaxLiveTypes];
    int32_t  pool_ys[fm2k::pool_delta_trace::kMaxLiveTypes];
};

Entry  g_ring[kCapacity];
size_t g_next  = 0;      // next slot to write
size_t g_count = 0;      // how many are valid (saturates at kCapacity)
int    g_dumps = 0;

// The four fields wire_fingerprint hashes. Literals rather than aliases so an
// ENGINE_FM95 build can never silently point this at FM95's unrelated
// addresses -- the entry points below compile to nothing there anyway.
constexpr uintptr_t kRng   = 0x41FB1C;
constexpr uintptr_t kP1Hp  = 0x4DFC85;
constexpr uintptr_t kP2Hp  = 0x4EDCC4;
constexpr uintptr_t kRound = 0x470044;
// update_game_state's per-frame live-object counter (IDA:
// g_objects_updated_this_frame).
constexpr uintptr_t kObjsUpdated = 0x4246FC;
// THE INPUT-EDGE CHAIN, which is what actually retires the objects.
// vs_round_function @0x4098C8 does `test ah,4` on g_system_input_state and, on
// a rising edge of bit 0x0400 (button 7), calls DeactivateObjectsByParam --
// flipping EVERY type-4 object with the matching script_id from 4 to 1 in one
// sweep, and clearing the pause flag. That is exactly the observed shape: two
// objects, both script 941, retiring together on one peer only.
//
// g_system_input_state is not a pad word: process_game_inputs computes it as
// the OR over all 8 slots of current & (prev ^ current), i.e. NEWLY PRESSED
// bits. So the pad words AND the previous words both matter, and a divergence
// can come from either.
constexpr uintptr_t kSysInputState = 0x4280D8;   // g_system_input_state
constexpr uintptr_t kSlotInput0    = 0x4259C0;   // g_story_char_config[664] (P1 pad)
constexpr uintptr_t kSlotInput1    = 0x4259C4;   // g_story_char_config[665] (P2 pad)
constexpr uintptr_t kPrevInput0    = 0x447F00;   // g_prev_input[0]
constexpr uintptr_t kPrevInput1    = 0x447F04;   // g_prev_input[1]

// SPAWN HANDLES -- the parent-to-child pointers a script uses to retire the
// objects it spawned. character_state_machine @0x412A8E reads
// spawn_slot_ptrs[index] (char_slot + 0xDFBF), scans the pool for the object
// AT THAT ADDRESS, and sets its type to 1 (retire) when the opcode's label
// operand is zero, then nulls the handle.
//
// This is where the divergence has to live. Across all 98 live objects the
// FIRST difference of any kind between the two peers is the type flip itself
// at f=96 -- every script, cursor and wait matches right up to it, characters
// included. So nothing in the object pool diverged first; the state that did
// is in the CHARACTER SLOT, and this array is the only thing there that can
// retire a pool object without touching its script.
constexpr uintptr_t kCharSlot0     = 0x4D1D80;
constexpr size_t    kCharSlotSize  = 57407;
constexpr ptrdiff_t kOffSpawnPtrs  = 0xDFBF;   // 10 pointers, unaligned

// GAME-SPEED LATCHES, not the source. ResetObjectsAndCalculateSpeed @0x406450
// DERIVES these from g_cfg_game_speed once, and everything that moves uses the
// derived values thereafter. The [CFG] log line prints the SOURCE (0x430104),
// so a peer that latched while the source was still wrong -- HOST_CONFIG #1
// carries speed=0 before the real value is known -- reports a correct speed
// while moving objects at the wrong rate. Comparing the latches is the only
// way to see that.
constexpr uintptr_t kCfgGameSpeed  = 0x430104;   // source (Editor.TestPlay.GameSpeed)
constexpr uintptr_t kMoveScalar    = 0x541F78;   // 0x10000 / frame_mult
constexpr uintptr_t kAccelScalar   = 0x445700;   // 3932160 / frame_mult^2
constexpr uintptr_t kPicStep       = 0x445704;   // frame_mult (we pin this to 100)

}  // namespace

void Note(int32_t frame, int32_t confirmed_frame) {
    if constexpr (FM2K::kIsFM95) {
        (void)frame; (void)confirmed_frame;
        return;
    } else {
        Entry& e = g_ring[g_next];
        e.frame     = frame;
        e.confirmed = confirmed_frame;
        e.rng       = *(const uint32_t*)kRng;
        e.p1_hp     = *(const uint32_t*)kP1Hp;
        e.p2_hp     = *(const uint32_t*)kP2Hp;
        e.round     = *(const uint32_t*)kRound;
        e.gp_calls     = g_gameplay_rand_calls;
        e.render_calls = g_render_rand_calls;
        for (int i = 0; i < kGpRandSiteCount; ++i) e.site[i] = g_gp_rand_by_site[i];
        e.objs = *(const uint32_t*)kObjsUpdated;
        e.sysin = *(const uint32_t*)kSysInputState;
        e.in0   = *(const uint32_t*)kSlotInput0;
        e.in1   = *(const uint32_t*)kSlotInput1;
        e.prev0 = *(const uint32_t*)kPrevInput0;
        e.prev1 = *(const uint32_t*)kPrevInput1;
        // memcpy, not a uint32_t* read: +0xDFBF is not 4-byte aligned.
        std::memcpy(e.spawn0, (const void*)(kCharSlot0 + kOffSpawnPtrs),
                    sizeof(e.spawn0));
        std::memcpy(e.spawn1,
                    (const void*)(kCharSlot0 + kCharSlotSize + kOffSpawnPtrs),
                    sizeof(e.spawn1));
        e.gs_src   = *(const uint32_t*)kCfgGameSpeed;
        e.gs_move  = *(const uint32_t*)kMoveScalar;
        e.gs_accel = *(const uint32_t*)kAccelScalar;
        e.gs_pic   = *(const uint32_t*)kPicStep;
        e.pool = fm2k::pool_delta_trace::Note(e.pool_bits, e.pool_types,
                                              e.pool_scripts, e.pool_cursors,
                                              e.pool_waits, e.pool_xs, e.pool_ys,
                                              &e.pool_live);
        g_next = (g_next + 1) % kCapacity;
        if (g_count < kCapacity) ++g_count;
    }
}

void Dump(const char* reason, int32_t desync_frame) {
    if constexpr (FM2K::kIsFM95) {
        (void)reason; (void)desync_frame;
        return;
    } else {
        if (g_count == 0) return;

        // Next to the debug logs, named by player index, sequence-suffixed so
        // a second desync cannot overwrite the first one's evidence.
        char dir[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        char* slash = std::strrchr(dir, '\\');
        if (slash) *slash = '\0';

        // The filename carries a WALL-CLOCK STAMP, not just a per-process
        // sequence number. Two peers of one run desync within a second of each
        // other, while separate runs are minutes apart, so the stamp is what
        // makes a pair identifiable. Learned the hard way: two captures both
        // landed as "..._0.csv" from DIFFERENT runs (one peer at 19:50, the
        // other at 20:16) and could not be diffed at all, because g_dumps is
        // per-process and says nothing about which run a file belongs to.
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        const int player = (int)g_player_index + 1;
        char path[MAX_PATH + 64];
        std::snprintf(path, sizeof(path),
                      "%s\\logs\\FM2K_P%d_desyncring_%02d%02d%02d_%d.csv",
                      dir, player, st.wHour, st.wMinute, st.wSecond, g_dumps);
        ++g_dumps;

        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "DesyncRing: could not open %s", path);
            return;
        }
        std::fprintf(f, "# desync ring player=%d reason=%s desync_frame=%d "
                        "rows=%zu\n", player, reason ? reason : "",
                     desync_frame, g_count);
        std::fprintf(f, "# THESE FOUR FIELDS ARE THE CROSS-PEER TERM -- what "
                        "wire_fingerprint hashes and what the two machines "
                        "must agree on. Diff this file against the peer's by "
                        "frame, keeping only rows where frame <= confirmed "
                        "(later rows are speculative runahead saves). The "
                        "first frame where a field differs AND never re-agrees "
                        "is the divergence; a field that differs and then "
                        "matches again is a rollback artifact, not a desync.\n");
        std::fprintf(f, "# rng is the raw LCG seed (seed = seed*0x343FD + 0x269EC3), "
                        "so the number of draws between two frames can be recovered "
                        "from the seed column ALONE by stepping one row's seed until "
                        "it reaches the next -- no peer file needed. The site_* "
                        "columns are cumulative draws per exact game_rand call site; "
                        "difference consecutive rows to see which site drew.\n");
        // COLUMN ORDER MUST MATCH THE EMIT ORDER BELOW EXACTLY. This has been
        // wrong twice: once when sysin.. was added and again when spawn*/gs_*
        // were, and both times the shifted columns produced a confident wrong
        // answer (the second read the always-zero site counters as the spawn
        // handles and "proved" they never differ). The emit is one fprintf
        // followed by appended groups, so the groups go at the END, in the
        // order they are appended -- never interleaved into the middle here.
        std::fprintf(f, "frame,confirmed,rng,p1_hp,p2_hp,round,gp_calls,render_calls,"
                        "sysin,in0,in1,prev0,prev1,"
                        "site_camera,site_hitdet,site_csm,site_scriptrand,site_ai,site_other,"
                        "objs,pool,"
                        "spawn0,spawn1,gs_src,gs_move,gs_accel,gs_pic,"
                        "pool_live,pool_bits,pool_types,pool_scripts,"
                        "pool_cursors,pool_waits,pool_xs,pool_ys\n");

        // Oldest first.
        const size_t start = (g_count == kCapacity) ? g_next : 0;
        for (size_t i = 0; i < g_count; ++i) {
            const Entry& e = g_ring[(start + i) % kCapacity];
            std::fprintf(f, "%d,%d,0x%08X,%u,%u,%u,%u,%u,0x%X,0x%X,0x%X,0x%X,0x%X,"
                         "%u,%u,%u,%u,%u,%u,%u,0x%08X",
                         e.frame, e.confirmed, e.rng, e.p1_hp, e.p2_hp,
                         e.round, e.gp_calls, e.render_calls, e.sysin, e.in0, e.in1,
                         e.prev0, e.prev1,
                         e.site[kGpSiteCameraManager], e.site[kGpSiteHitDetection],
                         e.site[kGpSiteCharStateMachine], e.site[kGpSiteScriptRandBranch],
                         e.site[kGpSiteAiInput], e.site[kGpSiteOther], e.objs, e.pool);
            std::fputc(',', f);
            for (int i = 0; i < kSpawnPtrCount; ++i)
                std::fprintf(f, "%08X", e.spawn0[i]);
            std::fputc(',', f);
            for (int i = 0; i < kSpawnPtrCount; ++i)
                std::fprintf(f, "%08X", e.spawn1[i]);
            std::fprintf(f, ",%u,%u,%u,%u", e.gs_src, e.gs_move, e.gs_accel, e.gs_pic);
            std::fprintf(f, ",%d,", e.pool_live);
            for (int w = 0; w < fm2k::pool_delta_trace::kBitmapWords; ++w)
                std::fprintf(f, "%08X", e.pool_bits[w]);
            std::fputc(',', f);
            const int nt = e.pool_live < fm2k::pool_delta_trace::kMaxLiveTypes
                         ? e.pool_live : fm2k::pool_delta_trace::kMaxLiveTypes;
            for (int i = 0; i < nt; ++i)
                std::fprintf(f, "%02X", e.pool_types[i]);
            std::fputc(',', f);
            for (int i = 0; i < nt; ++i)
                std::fprintf(f, "%04X", e.pool_scripts[i]);
            std::fputc(',', f);
            for (int i = 0; i < nt; ++i)
                std::fprintf(f, "%08X", e.pool_cursors[i]);
            std::fputc(',', f);
            for (int i = 0; i < nt; ++i)
                std::fprintf(f, "%04X", e.pool_waits[i]);
            std::fputc(',', f);
            for (int i = 0; i < nt; ++i)
                std::fprintf(f, "%08X", (uint32_t)e.pool_xs[i]);
            std::fputc(',', f);
            for (int i = 0; i < nt; ++i)
                std::fprintf(f, "%08X", (uint32_t)e.pool_ys[i]);
            std::fputc('\n', f);
        }
        std::fclose(f);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "DesyncRing: wrote %zu frames to %s -- diff it against the peer's "
            "copy to name the field that diverged", g_count, path);
    }
}

}  // namespace fm2k::desync_ring
