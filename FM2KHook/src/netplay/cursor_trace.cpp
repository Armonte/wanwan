// cursor_trace.cpp -- CROSS-PEER SAVE TRACE. OBSERVE ONLY.
// Nothing here writes game memory and nothing here changes shipping behaviour.
// See cursor_trace.h for the question it exists to answer.
#include "cursor_trace.h"

#if defined(ENGINE_FM95)
// FM2K addresses only. FM95 has its own savestate body and a different image
// layout; nothing arms this there.
bool CursorTrace_Enabled() { return false; }
void CursorTrace_OnSave(int, bool, bool) {}
void CursorTrace_Dump(int, const char*) {}
#else

#include "savestate.h"
#include "savestate_internal.h"
#include "seam_trace.h"            // SeamTrace_MatchIdx
#include "../core/globals.h"

#include <SDL3/SDL_log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

extern uint32_t g_netplay_frame;

namespace {

// The engine's live input cursor and the two 1024-entry uint32 rings it
// indexes. These are the exact three constants SaveState_CalculateFingerprint
// reads; they are repeated here rather than shared so that a change to one and
// not the other shows up as a disagreement in the CSV instead of silently
// tracking the bug being measured.
constexpr uintptr_t kInputCursor = 0x00447EE0;   // g_input_buffer_index
constexpr uintptr_t kP1Ring      = 0x004280E0;   // 1024 x uint32
constexpr uintptr_t kP2Ring      = 0x004290E0;   // 1024 x uint32, = base + 0x1000
constexpr uint32_t  kRingMask    = 0x3FF;

// 250k rows x 60 B = ~15 MB, allocated lazily at ARM time so a dark run costs
// one cached bool test per save and no memory at all. A 6000-frame multi-match
// session at this profile produces on the order of 40k-120k saves (forward
// plus every resim generation), so the cap is roughly 2x headroom; overflow is
// COUNTED and reported in the header line, never silently dropped -- a
// truncated trace that looked complete would make "no mismatch found" mean
// nothing.
constexpr size_t kCapacity = 250000;

struct Row {
    int32_t  frame;          // gekko's save frame == the cross-peer key
    uint32_t netplay_frame;  // g_netplay_frame at save time (resim depth)
    uint32_t seq;            // monotonic per-process save counter
    uint32_t buf_idx;        // RAW cursor, not masked
    uint32_t p1_word;        // full ring entry at idx
    uint32_t p2_word;
    uint32_t round_timer;    // 0x470060
    uint32_t game_timer;     // 0x470044
    uint32_t rng;            // 0x41FB1C
    uint32_t p1_hp;
    uint32_t p2_hp;
    uint32_t fingerprint;    // OLD arm: the pre-a3c1173 wire value
    uint32_t wire_fp;        // NEW arm: rng + both hp + round number only
    uint32_t round_limit;    // 0x470048 g_round_limit      -- Q3 candidate
    uint32_t round_count;    // 0x470050 g_round_timer_countdown -- Q3 candidate
    uint32_t game_mode;
    uint16_t p1_prev, p2_prev;   // low half at (idx - 1) & 0x3FF
    uint16_t p1_next, p2_next;   // low half at (idx + 1) & 0x3FF
    uint16_t match;
    uint8_t  replay;
    uint8_t  rolling_back;
};

Row*   g_rows      = nullptr;
size_t g_count     = 0;
size_t g_dropped   = 0;
uint32_t g_seq     = 0;

}  // namespace

bool CursorTrace_Enabled() {
    // Strict parse, default OFF. Unrecognised values fail to OFF with an Error
    // line, and exactly one contract line is emitted at the first call: an
    // armed run must say so in its own log, because the offline comparator
    // cannot otherwise tell "armed and clean" from "never armed".
    static int s_cached = -1;
    if (s_cached < 0) {
        const char* v = std::getenv("FM2K_CURSOR_TRACE");
        if (!v || !v[0] || (v[0] == '0' && v[1] == '\0')) {
            s_cached = 0;
        } else if (v[0] == '1' && v[1] == '\0') {
            s_cached = 1;
        } else {
            s_cached = 0;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[CURSOR] FM2K_CURSOR_TRACE=\"%s\" is not 0 or 1 -- treating "
                "as OFF. The trace is NOT recording.", v);
        }
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[CURSOR] cross-peer save trace %s (FM2K_CURSOR_TRACE)",
            s_cached == 1 ? "ARMED" : "off");
    }
    return s_cached == 1;
}

void CursorTrace_OnSave(int frame, bool is_replay_save, bool rolling_back) {
    if (!CursorTrace_Enabled()) return;
    if (!g_rows) {
        g_rows = (Row*)std::calloc(kCapacity, sizeof(Row));
        if (!g_rows) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[CURSOR] out of memory for %zu rows -- trace disabled",
                kCapacity);
            return;
        }
    }
    const uint32_t seq = g_seq++;
    if (g_count >= kCapacity) { ++g_dropped; return; }

    // fp_inputs was populated by SaveState_CalculateFingerprint earlier in this
    // same SaveState_Save call, from the same live memory. Reading it back
    // (rather than re-reading the addresses) is what makes a row a faithful
    // record of the value that actually crossed the wire.
    const auto& rc = SaveState_GetRegionChecksums();
    const uint32_t buf_idx = *(const uint32_t*)kInputCursor;
    const uint32_t idx     = buf_idx & kRingMask;
    const uint32_t prev    = (idx - 1) & kRingMask;
    const uint32_t next    = (idx + 1) & kRingMask;
    const uint32_t* p1 = (const uint32_t*)kP1Ring;
    const uint32_t* p2 = (const uint32_t*)kP2Ring;

    Row& r = g_rows[g_count++];
    r.frame         = (int32_t)frame;
    r.netplay_frame = g_netplay_frame;
    r.seq           = seq;
    r.buf_idx       = buf_idx;
    r.p1_word       = p1[idx];
    r.p2_word       = p2[idx];
    r.round_timer   = rc.fp_inputs.round_timer;
    r.game_timer    = rc.fp_inputs.game_timer;
    r.rng           = rc.fp_inputs.rng;
    r.p1_hp         = rc.fp_inputs.p1_hp;
    r.p2_hp         = rc.fp_inputs.p2_hp;
    r.fingerprint   = rc.gameplay_fingerprint;
    r.wire_fp       = rc.wire_fingerprint;
    // The two fields a3c1173 deliberately did NOT put on the wire yet. They
    // are recorded RAW so the offline comparator can synthesise the candidate
    // checksum that would include them and report what it WOULD have done --
    // measuring a proposed wire change without shipping it, which is the only
    // way to tell a new false positive apart from a failure to fix the old one.
    r.round_limit   = *(const uint32_t*)0x00470048;   // g_round_limit
    r.round_count   = *(const uint32_t*)0x00470050;   // g_round_timer_countdown
    r.game_mode     = *(const uint32_t*)FM2K::ADDR_GAME_MODE;
    r.p1_prev       = (uint16_t)(p1[prev] & 0xFFFF);
    r.p2_prev       = (uint16_t)(p2[prev] & 0xFFFF);
    r.p1_next       = (uint16_t)(p1[next] & 0xFFFF);
    r.p2_next       = (uint16_t)(p2[next] & 0xFFFF);
    r.match         = SeamTrace_MatchIdx();
    r.replay        = is_replay_save ? 1 : 0;
    r.rolling_back  = rolling_back ? 1 : 0;

    // SANITY, recorded not asserted: fp_inputs.buf_idx is the cursor the
    // fingerprint itself sampled. If it ever disagrees with the read above,
    // something moved the cursor between the two reads inside one save and the
    // whole trace is untrustworthy -- so it is loud, once.
    static bool s_warned = false;
    if (!s_warned && rc.fp_inputs.buf_idx != buf_idx) {
        s_warned = true;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[CURSOR] cursor moved INSIDE one save: fingerprint sampled %u, "
            "trace read %u at f=%d. Rows are not a faithful record.",
            rc.fp_inputs.buf_idx, buf_idx, frame);
    }
}

void CursorTrace_Dump(int player_index, const char* reason) {
    // Unconditional one-liner: a dark run reporting 0 rows is the evidence
    // that dark means dark.
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[CURSOR] summary: armed=%s rows=%zu dropped=%zu saves=%u (%s)",
        CursorTrace_Enabled() ? "yes" : "no", g_count, g_dropped,
        (unsigned)g_seq, reason ? reason : "");
    if (g_count == 0) return;

    char filename[256];
    std::snprintf(filename, sizeof(filename), "FM2K_P%d_cursortrace.csv",
                  player_index + 1);
    FILE* f = std::fopen(filename, "w");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[CURSOR] failed to open %s", filename);
        return;
    }
    // ~15 MB of fprintf at teardown. TerminateProcess is a few statements
    // behind the caller, so the cost cannot hitch a live match; a 1 MB stdio
    // buffer keeps it to a couple of seconds instead of tens.
    static char iobuf[1 << 20];
    std::setvbuf(f, iobuf, _IOFBF, sizeof(iobuf));

    std::fprintf(f, "# cursortrace player=%d reason=%s rows=%zu dropped=%zu "
                    "saves=%u capacity=%zu\n",
                 player_index + 1, reason ? reason : "", g_count, g_dropped,
                 (unsigned)g_seq, kCapacity);
    std::fprintf(f, "# join P1 against P2 on (match,frame). `seq` orders the "
                    "GENERATIONS of a frame within one process.\n");
    // Column names follow the RE settlement (artifacts/fingerprint_re_2026-09-03.md),
    // not the old wrong guesses: 0x470060 is `vsmode_dead` (a write-only copy of
    // this peer's game.ini Editor.TestPlay.VSMode, sole write 0x40703B, zero
    // readers) and 0x470044 is `round_number` (incremented once per round at
    // 0x408986), NOT "round_timer" and "game_timer".
    std::fprintf(f, "match,frame,seq,replay,rb,netplay_frame,game_mode,"
                    "fingerprint,wire_fp,rng,p1_hp,p2_hp,vsmode_dead,"
                    "round_number,round_limit,round_count,"
                    "buf_idx,idx,p1_word,p2_word,p1_prev,p2_prev,"
                    "p1_next,p2_next\n");
    for (size_t i = 0; i < g_count; ++i) {
        const Row& r = g_rows[i];
        std::fprintf(f,
            "%u,%d,%u,%u,%u,%u,%u,0x%08X,0x%08X,0x%08X,%u,%u,%u,%u,%u,%u,%u,%u,"
            "0x%08X,0x%08X,0x%04X,0x%04X,0x%04X,0x%04X\n",
            (unsigned)r.match, r.frame, r.seq, r.replay, r.rolling_back,
            r.netplay_frame, r.game_mode, r.fingerprint, r.wire_fp, r.rng,
            r.p1_hp, r.p2_hp, r.round_timer, r.game_timer,
            r.round_limit, r.round_count,
            r.buf_idx, r.buf_idx & kRingMask, r.p1_word, r.p2_word,
            r.p1_prev, r.p2_prev, r.p1_next, r.p2_next);
    }
    std::fclose(f);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[CURSOR] wrote %s (%zu rows)", filename, g_count);
}

#endif  // ENGINE_FM95
