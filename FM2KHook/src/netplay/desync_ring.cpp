#include "desync_ring.h"

#include "../core/globals.h"

#include <SDL3/SDL_log.h>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace fm2k::desync_ring {
namespace {

// ~40 s at 100 Hz. Enough to cover a whole match's approach to a desync
// without the buffer itself being a memory event (4096 * 24 B = 96 KB).
constexpr size_t kCapacity = 4096;

struct Entry {
    int32_t  frame;
    int32_t  confirmed;
    uint32_t rng;
    uint32_t p1_hp;
    uint32_t p2_hp;
    uint32_t round;
    // The DRAW COUNTS, because the divergence measured on 2026-09-06 is rng
    // ONLY -- hp and round still agree at the diverging frame, so the sims
    // had not visibly diverged; one peer simply drew a different number of
    // gameplay randoms. These two say whether the extra draw was made on the
    // gameplay stream or diverted to the render stream (render draws are
    // isolated behind g_render_rng_seed, but only while g_in_render_rng is
    // set -- a draw on a render path that misses that flag advances the
    // GAMEPLAY seed, and render frequency legitimately differs per peer).
    uint32_t gp_calls;
    uint32_t render_calls;
    // Cumulative gameplay draws split by EXACT game_rand call site (globals.h).
    // The measured divergence is ONE extra gameplay draw with hp/round still
    // identical, so the only remaining question is which site drew it. Six
    // counters answer that outright; gp_calls above only says that it happened.
    uint32_t site[kGpRandSiteCount];
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
        std::fprintf(f, "frame,confirmed,rng,p1_hp,p2_hp,round,gp_calls,render_calls,"
                        "site_camera,site_hitdet,site_csm,site_scriptrand,site_ai,site_other\n");

        // Oldest first.
        const size_t start = (g_count == kCapacity) ? g_next : 0;
        for (size_t i = 0; i < g_count; ++i) {
            const Entry& e = g_ring[(start + i) % kCapacity];
            std::fprintf(f, "%d,%d,0x%08X,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                         e.frame, e.confirmed, e.rng, e.p1_hp, e.p2_hp,
                         e.round, e.gp_calls, e.render_calls,
                         e.site[kGpSiteCameraManager], e.site[kGpSiteHitDetection],
                         e.site[kGpSiteCharStateMachine], e.site[kGpSiteScriptRandBranch],
                         e.site[kGpSiteAiInput], e.site[kGpSiteOther]);
        }
        std::fclose(f);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "DesyncRing: wrote %zu frames to %s -- diff it against the peer's "
            "copy to name the field that diverged", g_count, path);
    }
}

}  // namespace fm2k::desync_ring
