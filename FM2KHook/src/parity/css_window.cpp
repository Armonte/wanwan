/* SPDX-License-Identifier: Apache-2.0 */
/* [CSS-WIN] / [CSS-OBJ] emitter. Contract and rationale in css_window.h. */

#include "css_window.h"
#include "parity_pool.h"
#include "../core/globals.h"   /* FM2K::kIsFM2K */

#include <SDL3/SDL_log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace CssWindow {

namespace {

/* g_game_mode value for the character-select / results window. Battle is 3000;
 * see parity_recorder.cpp's ADDR_MATCH_PHASE note. */
constexpr int32_t kPhaseCss = 2000;

/* One [CSS-WIN] sample line every this many in-window frames. The window is
 * ~940-1060 frames long, so this is ~32-36 lines per window per plane -- dense
 * enough to align two planes, cheap enough for synchronous logging. */
constexpr uint32_t kEmitPeriod = 30u;

/* Format version of the [CSS-WIN] line. Bumped whenever a field's MEANING
 * changes, so the harness can refuse to compare two different quantities that
 * share a name -- the tv=2 lesson from the 4c top=/bind= split (a comparison
 * across the split was a guaranteed false RED, not a verdict). */
constexpr unsigned kLineVersion = 1u;

/* Fall-dump trigger: how far in PIXELS a player object must travel BELOW the
 * level it has been resting at before the [CSS-OBJ] detail dump fires.
 *
 * This is deliberately SELF-REFERENTIAL rather than an absolute y. The first
 * implementation used an absolute 600 px, which is wrong on contact with real
 * data: the healthy resting level is 535 px on vanguard-princess but 960 px on
 * wanwan, so a single absolute number either fires on wanwan's very first
 * character-select frame (measured -- it dumped the window-open state on every
 * window and never the interesting one) or misses vanguard-princess entirely.
 * Comparing against the window's OWN established resting level works on both,
 * and on the wanwan constant-descent object as well.
 *
 * Retunable via FM2K_CSS_FALL_DELTA. It only decides WHEN a diagnostic dump
 * fires, never a verdict: the harness gate compares the spectator against the
 * HOST's own measured resting level and does not read this number at all. */
constexpr int32_t kDefaultFallDelta = 32;

/* Cap on per-slot records kept for the detail dump. The pool is 1024 slots;
 * character-select carries ~40 active on wanwan and up to ~160 on
 * vanguard-princess. Anything above the cap is reported as a shortfall
 * (n= vs nobj=) rather than silently dropped. */
constexpr size_t kMaxRecords = 256u;

/* Per-line payload budget for a chunked [CSS-OBJ] dump. */
constexpr size_t kChunkChars = 900u;

struct SlotRec {
    uint16_t slot;
    uint16_t player_slot;
    uint32_t type;
    int32_t  owner;
    int32_t  script;
    int32_t  kind;
    int32_t  px;      /* pixels, pos_x >> 16 */
    int32_t  py;      /* pixels, pos_y >> 16 */
};

/* ---- state (single-threaded: the game loop is the only caller) ---------- */

int      s_enabled     = -1;    /* -1 = env not yet read */
int32_t  s_fall_delta  = kDefaultFallDelta;

bool     s_in_window   = false;
uint32_t s_window_ord  = 0;
uint32_t s_frames      = 0;     /* frames seen inside the current window */
uint32_t s_emits       = 0;
bool     s_fall_dumped = false;

int      s_max_slot[2] = { -1, -1 };
int32_t  s_max_y[2]    = { 0, 0 };
bool     s_have_y[2]   = { false, false };

/* Per-player RESTING LEVEL tracking: the y held for the longest consecutive
 * run so far in this window, y == 0 excluded (an object that exists but has
 * not been placed sits at the origin and is not resting anywhere). */
int32_t  s_rest_y[2]   = { 0, 0 };
uint32_t s_rest_run[2] = { 0, 0 };
int32_t  s_cur_y[2]    = { 0, 0 };
uint32_t s_cur_run[2]  = { 0, 0 };

SlotRec  s_last[kMaxRecords];
size_t   s_last_n      = 0;     /* records actually stored */
uint32_t s_last_nobj   = 0;     /* true active-slot count at that sample */
uint32_t s_last_seq    = 0;

/* Snapshot of the pool taken at the frame where the per-window maximum player
 * pos_y was observed. The close-time dump cannot serve that role: the falling
 * object is already GONE by the last in-window frame (measured -- script_idx
 * goes to -1 one to three frames before the phase change), so a dump taken at
 * close names everything except the object we are hunting. */
SlotRec  s_peak[kMaxRecords];
size_t   s_peak_n      = 0;
uint32_t s_peak_nobj   = 0;
uint32_t s_peak_seq    = 0;
int32_t  s_peak_y      = 0;
bool     s_have_peak   = false;

/* ---- BATTLE-FRAME CENSUS (FM2K_BATTLE_F1) ------------------------------
 * Separate buffer and separate env gate from the CSS window on purpose:
 *  - Buffer, because the census fires at battle frames 0/1 (mode 3000) while
 *    the CSS window's CloseWindow() still owes a dump of ITS last in-window
 *    sample. Sharing s_last[] would let the census clobber the close dump
 *    depending on which of the two call sites runs first in the frame.
 *  - Env gate, because FM2K_CSS_WIN is deliberately HOST-ONLY (Wave-2 review
 *    B2: the guest's [CSS-WIN] lines were read by nothing and the synchronous
 *    log IO perturbed a timing-sensitive phase), and the census is worthless
 *    unless it runs on BOTH players -- comparing the two is the whole point. */
int      s_census_en   = -1;    /* -1 = env not yet read */
SlotRec  s_census[kMaxRecords];
uint32_t s_battle_ord  = 0;     /* battles seen, 1-based, this process */
int      s_census_done = 0;     /* bit 0 = f=0 emitted, bit 1 = f=1 emitted */
int      s_census_last = -2;    /* last frame seen, for battle-restart detect */

inline int32_t ToPixels(int32_t fixed_16_16) {
    /* Arithmetic shift keeps negatives sane; the field is 16.16 (verified:
     * the host's resting 35061760 == 535.0 px, the observed fall peak
     * 60350640 == 920.9 px). */
    return fixed_16_16 >> 16;
}

void ReadEnv() {
    if (s_enabled >= 0) return;
    if constexpr (!FM2K::kIsFM2K) {
        s_enabled = 0;
        return;
    }
    const char* v = std::getenv("FM2K_CSS_WIN");
    s_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
    if (const char* t = std::getenv("FM2K_CSS_FALL_DELTA"); t && t[0]) {
        const long parsed = std::strtol(t, nullptr, 10);
        if (parsed > 0 && parsed < 100000) s_fall_delta = static_cast<int32_t>(parsed);
    }
}

/* One ascending pass over the pool into `dst`, one dword load per inactive
 * slot; no allocation, no formatting. Shared by the CSS window sampler and the
 * battle-frame census so the two instruments can never drift in what they
 * capture (or how they truncate). */
void SampleInto(SlotRec* dst, size_t& out_n, uint32_t& out_nobj) {
    size_t n = 0;
    uint32_t nobj = 0;
    for (size_t i = 0; i < ParityPool::kSlotCount; ++i) {
        const uint8_t* obj = reinterpret_cast<const uint8_t*>(
            ParityPool::kPoolBase + i * ParityPool::kSlotStride);
        const uint32_t type = *reinterpret_cast<const uint32_t*>(obj + ParityPool::kOffType);
        if (type == 0u) continue;
        ++nobj;
        if (n >= kMaxRecords) continue;
        SlotRec& r = dst[n++];
        r.slot        = static_cast<uint16_t>(i);
        r.type        = type;
        r.owner       = *reinterpret_cast<const int32_t*>(obj + ParityPool::kOffOwner);
        r.script      = *reinterpret_cast<const int32_t*>(obj + ParityPool::kOffScriptId);
        r.player_slot = *reinterpret_cast<const uint16_t*>(obj + ParityPool::kOffPlayerSlot);
        r.kind        = *reinterpret_cast<const int32_t*>(obj + ParityPool::kOffEntityKind);
        r.px          = ToPixels(*reinterpret_cast<const int32_t*>(obj + ParityPool::kOffPosX));
        r.py          = ToPixels(*reinterpret_cast<const int32_t*>(obj + ParityPool::kOffPosY));
    }
    out_n    = n;
    out_nobj = nobj;
}

/* Refresh s_last[] from the live pool. */
void SampleSlots(uint32_t seq) {
    SampleInto(s_last, s_last_n, s_last_nobj);
    s_last_seq = seq;
}

/* Emit a buffered slot listing, chunked. Every record appears in exactly one
 * part; part=k/n makes an incomplete capture obvious instead of silent.
 * `tag` selects the line prefix ([CSS-OBJ] vs [BATTLE-OBJ]) and `win` the
 * window/battle ordinal, so the battle census reuses this verbatim rather than
 * growing a second, drifting copy of the chunker. */
void DumpSlotsAs(const char* tag, unsigned win,
                 const char* why, const SlotRec* recs, size_t n,
                 uint32_t nobj, uint32_t seq) {
    /* Count parts first so part=k/n is honest on the very first line. */
    char rec[96];
    size_t parts = 1, used = 0;
    for (size_t i = 0; i < n; ++i) {
        const SlotRec& r = recs[i];
        const int len = std::snprintf(rec, sizeof(rec), "%u:%u:%d:%d:%u:%d:%d:%d ",
                                      (unsigned)r.slot, (unsigned)r.type, r.owner,
                                      r.script, (unsigned)r.player_slot, r.kind,
                                      r.px, r.py);
        /* snprintf returns the length it WOULD have written. A hostile/garbage
         * slot could produce more than sizeof(rec)-1 characters, and using the
         * unclamped return as a memcpy length would read past `rec`. */
        size_t l = (len > 0) ? (size_t)len : 0u;
        if (l > sizeof(rec) - 1) l = sizeof(rec) - 1;
        if (used + l > kChunkChars && used > 0) { ++parts; used = 0; }
        used += l;
    }

    char buf[kChunkChars + 64];
    size_t at = 0, part = 1;
    for (size_t i = 0; i < n; ++i) {
        const SlotRec& r = recs[i];
        const int len = std::snprintf(rec, sizeof(rec), "%u:%u:%d:%d:%u:%d:%d:%d ",
                                      (unsigned)r.slot, (unsigned)r.type, r.owner,
                                      r.script, (unsigned)r.player_slot, r.kind,
                                      r.px, r.py);
        /* snprintf returns the length it WOULD have written. A hostile/garbage
         * slot could produce more than sizeof(rec)-1 characters, and using the
         * unclamped return as a memcpy length would read past `rec`. */
        size_t l = (len > 0) ? (size_t)len : 0u;
        if (l > sizeof(rec) - 1) l = sizeof(rec) - 1;
        if (at + l > kChunkChars && at > 0) {
            buf[at] = '\0';
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "%s win=%u why=%s seq=%u tv=%u nobj=%u n=%u part=%u/%u f=slot:type:owner:script:pslot:kind:x:y d=%s",
                tag, win, why, seq, kLineVersion, nobj,
                (unsigned)n, (unsigned)part, (unsigned)parts, buf);
            ++part;
            at = 0;
        }
        std::memcpy(buf + at, rec, l);
        at += l;
    }
    buf[at] = '\0';
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "%s win=%u why=%s seq=%u tv=%u nobj=%u n=%u part=%u/%u f=slot:type:owner:script:pslot:kind:x:y d=%s",
        tag, win, why, seq, kLineVersion, nobj,
        (unsigned)n, (unsigned)part, (unsigned)parts, buf);
}

/* The CSS window's own dumps, unchanged: [CSS-OBJ] tagged, keyed on the
 * character-select window ordinal. */
void DumpSlots(const char* why, const SlotRec* recs, size_t n,
               uint32_t nobj, uint32_t seq) {
    DumpSlotsAs("[CSS-OBJ]", s_window_ord, why, recs, n, nobj, seq);
}

void CloseWindow() {
    if (!s_in_window) return;
    /* PEAK first: the sample from the frame with the deepest player object.
     * This is the one that names a falling object, because by the last
     * in-window frame the object is already gone (script_idx -1). */
    if (s_have_peak) {
        DumpSlots("peak", s_peak, s_peak_n, s_peak_nobj, s_peak_seq);
    }
    /* CLOSE: the last in-window sample, so a window that never tripped and
     * never moved still leaves one full listing for an offline
     * host-vs-spectator presence diff. Buffered rather than re-scanned: by the
     * time the phase leaves 2000 the pool has already been re-authored. */
    if (s_last_n > 0 || s_last_nobj > 0) {
        DumpSlots("close", s_last, s_last_n, s_last_nobj, s_last_seq);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[CSS-WIN] win=%u ev=close tv=%u frames=%u emits=%u "
        "p1s=%d p1maxy=%d p1rest=%d p2s=%d p2maxy=%d p2rest=%d falldelta=%d",
        s_window_ord, kLineVersion, s_frames, s_emits,
        s_max_slot[0], s_have_y[0] ? ToPixels(s_max_y[0]) : -1,
        s_rest_run[0] ? ToPixels(s_rest_y[0]) : -1,
        s_max_slot[1], s_have_y[1] ? ToPixels(s_max_y[1]) : -1,
        s_rest_run[1] ? ToPixels(s_rest_y[1]) : -1,
        s_fall_delta);
    s_in_window = false;
}

void ReadCensusEnv() {
    if (s_census_en >= 0) return;
    if constexpr (!FM2K::kIsFM2K) {
        /* FM95's unrelated 0x426A40 pool must never be scanned through the
         * WonderfulWorld pool literals -- same rule as ReadEnv(). */
        s_census_en = 0;
        return;
    }
    const char* v = std::getenv("FM2K_BATTLE_F1");
    s_census_en = (v && v[0] && v[0] != '0') ? 1 : 0;
}

}  /* namespace */

bool Enabled() {
    ReadEnv();
    return s_enabled == 1;
}

void OnCapture(uint32_t seq, int32_t match_phase,
               int p1_slot, int32_t p1_pos_y, int32_t p1_script,
               int p2_slot, int32_t p2_pos_y, int32_t p2_script) {
    ReadEnv();
    if (s_enabled != 1) return;

    if (match_phase != kPhaseCss) {
        CloseWindow();
        return;
    }

    if (!s_in_window) {
        s_in_window   = true;
        ++s_window_ord;
        s_frames      = 0;
        s_emits       = 0;
        s_fall_dumped = false;
        s_last_n      = 0;
        s_last_nobj   = 0;
        s_peak_n      = 0;
        s_peak_nobj   = 0;
        s_peak_y      = 0;
        s_have_peak   = false;
        s_max_slot[0] = s_max_slot[1] = -1;
        s_max_y[0]    = s_max_y[1]    = 0;
        s_have_y[0]   = s_have_y[1]   = false;
        s_rest_y[0]   = s_rest_y[1]   = 0;
        s_rest_run[0] = s_rest_run[1] = 0;
        s_cur_y[0]    = s_cur_y[1]    = 0;
        s_cur_run[0]  = s_cur_run[1]  = 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[CSS-WIN] win=%u ev=open tv=%u seq=%u", s_window_ord, kLineVersion, seq);
    }

    /* Per-frame: buffer the pool and track each player object's maximum pos_y.
     * `script == -1` is FillPlayerSnapshot's not-found sentinel; a slot of -1
     * means the same thing from the other direction. Frames with no object are
     * not evidence in either direction and are skipped, not scored as 0. */
    SampleSlots(seq);

    const int     slots[2]  = { p1_slot, p2_slot };
    const int32_t ys[2]     = { p1_pos_y, p2_pos_y };
    const int32_t scripts[2]= { p1_script, p2_script };
    bool trip = false, new_peak = false;
    for (int p = 0; p < 2; ++p) {
        if (slots[p] < 0 || scripts[p] == -1) continue;
        if (!s_have_y[p] || ys[p] > s_max_y[p]) {
            s_max_y[p]    = ys[p];
            s_max_slot[p] = slots[p];
            if (ys[p] > s_peak_y || !s_have_peak) { s_peak_y = ys[p]; new_peak = true; }
        }
        s_have_y[p] = true;

        /* Resting level = the y held for the longest consecutive run so far.
         * y == 0 is skipped: an object that exists but has not been placed
         * sits at the origin, and letting the origin become a resting level
         * made every wanwan window trip on its first placement (measured). */
        if (ys[p] != 0) {
            if (ys[p] == s_cur_y[p]) {
                ++s_cur_run[p];
            } else {
                s_cur_y[p] = ys[p];
                s_cur_run[p] = 1;
            }
            if (s_cur_run[p] > s_rest_run[p]) {
                s_rest_run[p] = s_cur_run[p];
                s_rest_y[p]   = s_cur_y[p];
            }
            /* Trip once the object is kFallDelta px BELOW its own resting
             * level. Self-referential, so it works at 535 px (vanpri) and at
             * 960 px (wanwan) without a per-game constant. */
            if (s_rest_run[p] > 0 &&
                ToPixels(ys[p] - s_rest_y[p]) > s_fall_delta) {
                trip = true;
            }
        }
    }

    /* Keep the pool snapshot from the deepest frame: the falling object is
     * gone from the pool by the time the window closes. */
    if (new_peak) {
        std::memcpy(s_peak, s_last, s_last_n * sizeof(SlotRec));
        s_peak_n    = s_last_n;
        s_peak_nobj = s_last_nobj;
        s_peak_seq  = s_last_seq;
        s_have_peak = true;
    }

    if (trip && !s_fall_dumped) {
        s_fall_dumped = true;
        DumpSlots("fall", s_last, s_last_n, s_last_nobj, s_last_seq);
    }

    if ((s_frames % kEmitPeriod) == 0u) {
        /* ComputeTopology honours FM2K_CK_TOPOLOGY and returns the documented
         * all-zero "not computed" sentinel when disabled -- the harness tests
         * map==0 exactly the way the [CHECKSUM] POOL terms do, so a run with
         * the topology hatch set goes RED instead of vacuously green. */
        const ParityPool::Topology t = ParityPool::ComputeTopology();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[CSS-WIN] win=%u i=%u seq=%u tv=%u nobj=%u map=0x%08X bind=0x%08X "
            "p1s=%d p1y=%d p2s=%d p2y=%d",
            s_window_ord, s_frames, seq, kLineVersion,
            t.active_count, t.digest, t.binding,
            p1_slot, (p1_slot >= 0 && p1_script != -1) ? ToPixels(p1_pos_y) : -1,
            p2_slot, (p2_slot >= 0 && p2_script != -1) ? ToPixels(p2_pos_y) : -1);
        ++s_emits;
    }
    ++s_frames;
}

bool CensusEnabled() {
    ReadCensusEnv();
    return s_census_en == 1;
}

void OnBattleFrame(int frame) {
    ReadCensusEnv();
    if (s_census_en != 1) return;

    /* Battle restart re-arms the latch. Two spellings, because the frame
     * numbering the call site sees depends on how far up the save path it
     * sits: some callers pass the f=-1 battle-entry marker through, others
     * clamp it to 0 before we ever see it. So re-arm on EITHER a negative
     * frame OR the frame going backwards past our last census point -- a new
     * battle always does one or the other. Rollback re-sim also replays
     * f=0/f=1, which is exactly why the latch exists: one census per battle
     * per player, taken at the FORWARD pass, so the two players' lines are
     * directly comparable and the synchronous log IO cannot repeat inside a
     * re-sim burst.
     *
     * The backwards test is DELIBERATELY narrow. A rollback also steps the
     * frame backwards, and a naive `frame < s_census_last` would re-arm (and
     * inflate the battle ordinal) on every one of them. The save-state ring is
     * MAX_ROLLBACK_FRAMES = 64 deep, so no rollback can ever carry a peer from
     * beyond that back to frame 0/1 -- but a new battle always does. */
    constexpr int kRestartFloor = 64;   // = MAX_ROLLBACK_FRAMES
    if (frame < 0 || (frame <= 1 && s_census_last > kRestartFloor)) {
        s_census_done = 0;
        ++s_battle_ord;
    }
    s_census_last = frame;

    /* f=0 is the ANCHOR: measured byte-identical between peers in both clean
     * and desyncing sessions, so a difference here would relocate the bug to
     * before battle entry. f=1 is the DIVERGENCE point: the guest has been
     * observed carrying exactly two more live objects than the host from this
     * frame onward, constant for the whole match (ab_rate_verdict.md 5.1b). */
    int bit;
    if (frame == 0)      bit = 1;
    else if (frame == 1) bit = 2;
    else                 return;
    if (s_census_done & bit) return;
    s_census_done |= bit;

    /* A battle that begins without an f=-1 marker (mid-session re-entry) still
     * gets an ordinal, so no two censuses in one log share win= by accident. */
    if (s_battle_ord == 0) s_battle_ord = 1;

    size_t n = 0;
    uint32_t nobj = 0;
    SampleInto(s_census, n, nobj);
    DumpSlotsAs("[BATTLE-OBJ]", s_battle_ord,
                (frame == 0) ? "f0" : "f1",
                s_census, n, nobj, static_cast<uint32_t>(frame));

    /* [BATTLE-CFG] -- the LATCHED sim values next to the CONFIG SOURCES they
     * were derived from. Emitted as its own line so [BATTLE-OBJ]'s format
     * stays byte-identical to [CSS-OBJ]'s.
     *
     * Why these seven: a battle-init RE pass identified the object-count
     * mechanism as `g_round_limit` (0x470048). ui_state_manager runs TWO
     * symmetric win-pip creation loops, `for i in 0..g_round_limit`, one per
     * side (0x409DE1 and 0x409EC8), on its FIRST tick -- which is battle frame
     * 1. So a peer whose g_round_limit is one higher creates exactly TWO extra
     * type-4 objects, at exactly frame 1, constant for the match (the value is
     * only re-derived at the next match) -- and the SAME variable is the
     * match-over predicate at 0x409710, which is why it only detonates at
     * match end.
     *
     * The pairing is the point. g_round_limit is LATCHED from g_default_round
     * at 0x4087DA, which runs BEFORE the g_game_mode = 3000 write at 0x40895E
     * that the battle-entry settings barrier is armed on. Netplay_MatchSettings
     * Digest hashes the SOURCES (0x430124/0x430114/0x43010C); the sim consumes
     * the latched COPIES (0x470048/0x470050/0x470040). A late HOST_CONFIG apply
     * therefore fixes the source, satisfies the digest, releases the barrier --
     * and leaves the latched copy stale for the whole match. Logging both
     * halves makes "source agrees, latch does not" directly readable, which no
     * existing line can show ([ROUND-START] logs only the source).
     * FM2K addresses; this whole function is FM2K-gated. */
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[BATTLE-CFG] win=%u why=%s f=%d tv=%u "
        "latch_rounds=%u latch_score=%d latch_stage=%d mode_flag=%d "
        "cfg_rounds=%u cfg_rtime=%u cfg_stage=%d",
        s_battle_ord, (frame == 0) ? "f0" : "f1", frame, kLineVersion,
        *(const uint32_t*)0x470048,   /* g_round_limit    (latched sim value) */
        *(const int32_t*) 0x470050,   /* g_score_value / game timer countdown */
        *(const int32_t*) 0x470040,   /* g_active_stage_id                    */
        *(const int32_t*) 0x470058,   /* g_game_mode_flag                     */
        *(const uint32_t*)0x430124,   /* g_default_round  (config source)     */
        *(const uint32_t*)0x430114,   /* round time sec   (config source)     */
        *(const int32_t*) 0x43010C);  /* g_selected_stage (config source)     */
}

}  /* namespace CssWindow */
