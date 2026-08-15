// facing_trace.cpp -- Phase 4b [FACING] ring. See facing_trace.h for WHY.
//
// The ring records, on BOTH planes, exactly what the two facing derivations
// in hooks_getinput.cpp saw and decided, plus enough sim scalars to align the
// two processes' rings offline WITHOUT trusting either plane's own battle-frame
// counter (the spectator's s_battle_frame_at_entry is known to land one pop
// late from match 2 onward -- seam_p4a_vanpri_forensics.md 4.11 -- and the
// host's g_netplay_frame repeats across rollback re-sims).
//
// Alignment keys, strongest first:
//   rng + gtimer + rtimer  -- pure sim state, identical on both planes at the
//                             same simulated frame while they agree
//   sbf                    -- this module's OWN per-battle frame counter,
//                             incremented on the player_id==0 call. On the host
//                             it repeats during rollback re-sims: dedupe by LAST
//                             row per (match, sbf), the same convention the
//                             harness uses for the host's [CHECKSUM] emits.
//
//                             KNOWN DEFECT, MEASURED, NOT FIXED (Phase 4b
//                             diagnosis SS5.2 defect 1; re-stated here in Phase
//                             4e because a diagnostic must not carry a
//                             known-false contract in its own source): sbf is
//                             NOT "defined identically on both planes". On the
//                             PLAYER plane hooks_getinput.cpp returns early --
//                             `if (!IsBattleMode(game_mode) && Netplay_IsConnected())
//                             return capture_and_return(0);` -- BEFORE reaching
//                             FacingTrace_Note, so g_in_battle never clears on
//                             the host: the `match` column stays 1 and sbf runs
//                             monotonically for the whole session instead of
//                             resetting per battle. Cross-plane alignment on sbf
//                             is therefore WRONG today; use rng+gtimer+rtimer,
//                             and delimit matches by the dump segments. FIX THE
//                             EARLY RETURN (or note the battle transition from
//                             the other call site) BEFORE REUSING sbf.
//                             Second known defect: the spectator's final match
//                             is not dumped under TerminateProcess.
//   nf                     -- g_netplay_frame, authoritative on the host only.
//
// rend/sim additionally settle 4a's rank-4 (render:sim ratio) for free: rend is
// the engine's own render_frame_counter @ 0x4456FC (bumped at the end of every
// render_game and deliberately never rollback-restored), sim is the hook's
// g_sim_step_count. Their ratio per plane is the exact quantity the /10.0
// accumulator defect at trampoline_spectator.cpp:944 is accused of skewing.
//
// SYNC-LOGGING CONSTRAINT: hook logging is synchronous (quill is hardcoded off
// in FM2KHook). Nothing here writes a file per frame. The ring is one lazily
// malloc'd block; the only writes are at a match boundary and on exit paths.
#include "facing_trace.h"

#include "globals.h"                 // FM2K::ADDR_*, g_sim_step_count
#include "../netplay/savestate.h"    // CHAR_SLOT_BASE / CHAR_SLOT_SIZE
#include "../parity/parity_pool.h"   // ReadPlayer -- resolve fighters by scan

#include <SDL3/SDL_log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// g_netplay_frame lives in the netplay cluster's internal header; declared here
// rather than dragging netplay_internal.h into hooks/. It is authoritative on
// the player plane only (it repeats across rollback re-sims and is unused on a
// spectator, which is exactly why the ring also carries its own sbf and the
// rng/timer triple).
extern uint32_t g_netplay_frame;

namespace {

// One battle match at vanpri length is ~7000 sim frames x 2 calls/frame, and
// the host re-calls during rollback re-sims (~3x worst case observed). 131072
// rows covers a whole match with headroom; the ring is flushed AND reset at
// every match boundary so it never has to hold two.
constexpr size_t kRingCapacity = 131072u;

// Same offsets the two derivations use (relative to the CORRECTED
// CHAR_SLOT_BASE = 0x4D1D90). Duplicated deliberately rather than shared: this
// file must record what the suspect code reads, not redefine it.
constexpr size_t kCharActiveOffset = 0xDF41u;
constexpr size_t kCharFlagsOffset  = 0x7CA6u;

struct Row {
    uint32_t seq;
    uint32_t sbf;        // this module's per-battle frame index
    uint32_t nf;         // g_netplay_frame (host-authoritative only)
    uint32_t rend;       // engine render_frame_counter @ 0x4456FC
    uint32_t sim;        // hook g_sim_step_count
    uint32_t rng;        // 0x41FB1C
    uint32_t gtimer;     // 0x470044
    uint32_t rtimer;     // 0x470060
    uint32_t buf;        // 0x447EE0 input ring cursor
    int32_t  p1x;        // resolved by pool scan, not fixed slot
    int32_t  p2x;
    int32_t  p1s;        // pool slot index (-1 = no character object)
    int32_t  p2s;
    uint16_t in_post;    // input AFTER the swap, BEFORE SOCD
    uint16_t match;      // battle-session index within this process
    uint8_t  plane;      // 0 = player/host, 1 = spectator
    uint8_t  pid;        // player_id argument
    uint8_t  itype;      // input_type argument (the char-slot selector)
    uint8_t  cact;       // char_active byte the derivation read
    uint8_t  cflags;     // char_state_flags byte the derivation read
    uint8_t  facerev;    // the decision: 1 = NO swap applied
    uint8_t  pad[2];
};

Row*   g_ring       = nullptr;
size_t g_head       = 0;   // next write index
size_t g_count      = 0;   // rows live in the ring (<= capacity)
size_t g_dropped    = 0;   // rows overwritten since the last dump
uint32_t g_seq      = 0;
uint32_t g_match    = 0;
uint32_t g_sbf      = 0;
bool     g_in_battle = false;
size_t   g_dumps    = 0;

// Per-frame pool scan, done once (on the player_id == 0 call) and reused for
// the sibling call so the ring costs one pool pass per frame, not two.
int32_t g_cached_p1x = 0, g_cached_p2x = 0;
int32_t g_cached_p1s = -1, g_cached_p2s = -1;

const char* LogTag() {
    static char s_tag[16] = {0};
    if (s_tag[0]) return s_tag;
    const char* t = std::getenv("FM2K_LOG_TAG");
    if (t && t[0]) {
        std::snprintf(s_tag, sizeof(s_tag), "%s", t);
    } else {
        std::snprintf(s_tag, sizeof(s_tag), "P%d", g_player_index + 1);
    }
    return s_tag;
}

}  // namespace

bool FacingTrace_Enabled() {
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = std::getenv("FM2K_FACING_TRACE");
        s_on = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[FACING] contract: FM2K_FACING_TRACE=%s -> ring %s (capacity %zu)",
            v ? v : "(unset)", s_on ? "ARMED" : "off", kRingCapacity);
    }
    return s_on == 1;
}

uint16_t FacingTrace_Note(int plane, int player_id, int input_type,
                          uint32_t game_mode, bool facing_reversed,
                          uint16_t input_post) {
    // ENGINE GUARD (Phase 4e, review A10.7). Every address this TU reads --
    // 0x4456FC, 0x41FB1C, 0x470044, 0x470060, 0x447EE0, CHAR_SLOT_BASE+0xDF41 /
    // +0x7CA6 -- is an FM2K literal, and FM2KHOOK_SOURCES is shared by both
    // add_library(FM2KHook) and add_library(FM95Hook), so this file compiles
    // into FM95Hook.dll too. Arming FM2K_FACING_TRACE=1 on an FM95/CPW build
    // would read garbage or fault. The sibling diagnostic added in the same
    // phase (trampoline_spectator.cpp) already carries this guard; this closes
    // the recorded "ungated FM2K addrs" FM95 blocker class for the new TU.
    // Compiles away entirely on FM2K -- codegen there is unchanged.
    if constexpr (!FM2K::kIsFM2K) {
        return input_post;
    }
    if (!FacingTrace_Enabled()) return input_post;

    // Battle only. The derivation is a no-op outside [3000,4000) anyway (both
    // sites hard-code facing_reversed = true there), and CSS frames would only
    // dilute a ring sized for one match.
    if (!(game_mode >= 3000 && game_mode < 4000)) {
        if (g_in_battle) {
            // Battle -> non-battle edge seen from inside get_player_input. The
            // authoritative match-boundary dump is driven from the callers'
            // battle-end paths; this only keeps the counters honest if a plane
            // never reaches one.
            g_in_battle = false;
        }
        return input_post;
    }
    if (!g_in_battle) {
        g_in_battle = true;
        g_sbf = 0;
        ++g_match;
    } else if (player_id == 0) {
        ++g_sbf;
    }

    if (!g_ring) {
        g_ring = (Row*)std::calloc(kRingCapacity, sizeof(Row));
        if (!g_ring) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[FACING] ring allocation FAILED (%zu rows) -- trace disabled",
                kRingCapacity);
            return input_post;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[FACING] ring allocated: %zu rows x %zu B = %zu KB",
            kRingCapacity, sizeof(Row),
            (kRingCapacity * sizeof(Row)) / 1024u);
    }

    if (player_id == 0) {
        const ParityPool::PlayerView a = ParityPool::ReadPlayer(0);
        const ParityPool::PlayerView b = ParityPool::ReadPlayer(1);
        g_cached_p1x = a.pos_x; g_cached_p1s = a.slot;
        g_cached_p2x = b.pos_x; g_cached_p2s = b.slot;
    }

    const uintptr_t slot_base =
        CHAR_SLOT_BASE + (size_t)input_type * CHAR_SLOT_SIZE;

    Row& r = g_ring[g_head];
    r.seq     = g_seq++;
    r.sbf     = g_sbf;
    r.nf      = g_netplay_frame;
    r.rend    = *(uint32_t*)0x4456FC;
    r.sim     = (uint32_t)g_sim_step_count;
    r.rng     = *(uint32_t*)0x41FB1C;
    r.gtimer  = *(uint32_t*)0x470044;
    r.rtimer  = *(uint32_t*)0x470060;
    r.buf     = *(uint32_t*)0x447EE0;
    r.p1x     = g_cached_p1x;
    r.p2x     = g_cached_p2x;
    r.p1s     = g_cached_p1s;
    r.p2s     = g_cached_p2s;
    r.in_post = input_post;
    r.match   = (uint16_t)g_match;
    r.plane   = (uint8_t)plane;
    r.pid     = (uint8_t)player_id;
    r.itype   = (uint8_t)input_type;
    // Re-read the two decision bytes rather than take them as arguments: the
    // suspect code scopes them inside its `if`, and adding out-params to those
    // blocks would grow hooks_getinput.cpp past the 1000-line rule. Same
    // addresses, same instant, no side effects.
    r.cact    = *(uint8_t*)(slot_base + kCharActiveOffset);
    r.cflags  = *(uint8_t*)(slot_base + kCharFlagsOffset);
    r.facerev = facing_reversed ? 1u : 0u;

    g_head = (g_head + 1u) % kRingCapacity;
    if (g_count < kRingCapacity) ++g_count; else ++g_dropped;

    return input_post;
}

void FacingTrace_Dump(const char* reason) {
    if (!FacingTrace_Enabled()) return;
    if (g_count == 0) return;

    char filename[128];
    std::snprintf(filename, sizeof(filename), "FM2K_%s_facing.csv", LogTag());
    // Append: a multi-match run dumps once per match boundary plus once on the
    // way out, and every segment must survive.
    FILE* f = std::fopen(filename, g_dumps == 0 ? "w" : "a");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[FACING] failed to open %s", filename);
        return;
    }
    static char iobuf[1 << 18];
    std::setvbuf(f, iobuf, _IOFBF, sizeof(iobuf));

    std::fprintf(f, "# facing dump#%zu reason=%s rows=%zu dropped=%zu "
                    "match=%u seq=%u\n",
                 g_dumps, reason ? reason : "", g_count, g_dropped,
                 g_match, g_seq);
    if (g_dumps == 0) {
        std::fprintf(f,
            "seq,plane,match,sbf,nf,rend,sim,rng,gtimer,rtimer,buf,"
            "pid,itype,cact,cflags,facerev,in_post,p1x,p2x,p1s,p2s\n");
    }
    const size_t start = (g_count == kRingCapacity) ? g_head : 0;
    for (size_t i = 0; i < g_count; ++i) {
        const Row& r = g_ring[(start + i) % kRingCapacity];
        std::fprintf(f,
            "%u,%u,%u,%u,%u,%u,%u,0x%08X,%u,%u,%u,%u,%u,0x%02X,0x%02X,%u,"
            "0x%03X,%d,%d,%d,%d\n",
            r.seq, (unsigned)r.plane, (unsigned)r.match, r.sbf, r.nf,
            r.rend, r.sim, r.rng, r.gtimer, r.rtimer, r.buf,
            (unsigned)r.pid, (unsigned)r.itype, (unsigned)r.cact,
            (unsigned)r.cflags, (unsigned)r.facerev, (unsigned)r.in_post,
            r.p1x, r.p2x, r.p1s, r.p2s);
    }
    std::fclose(f);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[FACING] dump#%zu -> %s rows=%zu dropped=%zu match=%u (%s)",
        g_dumps, filename, g_count, g_dropped, g_match,
        reason ? reason : "");

    ++g_dumps;
    g_head = 0;
    g_count = 0;
    g_dropped = 0;
}
