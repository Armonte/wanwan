// envelope_shadow.cpp -- ENVELOPE INVERSION PHASE 1: SHADOW MODE.
// OBSERVE ONLY. Nothing here writes game memory, and nothing here changes a
// single byte of shipping behaviour. See envelope_shadow.h for the method.
//
// WHY A HASH RING **AND** A BYTE WITNESS. The block-hash ring answers "which
// 256-byte blocks differ", cheaply, for every one of the 64 rollback slots. But
// 256 bytes is too coarse for the question the study actually asks: block 111
// (0x424F00..0x424FFF) holds BOTH g_round_timer_counter @0x424F00 AND the
// 0x424F24/unk_424F28[] family the study nominates as the top uncovered
// category-(a) sim state, and the deliberate carve-outs E1..E5 collapse into
// just two blocks (630 and 669). So a bounded ring of full-image byte witnesses
// rides alongside, and every divergent block that still has its witness gets
// refined to a 64-bit mask of exactly which dwords differed. That mask is what
// makes the offline report able to name a scalar instead of a neighbourhood.
//
// MEMORY, all lazily allocated at ARM time so dark costs nothing:
//   hash ring    64 slots x 4704 blocks x 4 B  =  1.20 MB
//   witness ring 16 slots x 1,204,224 B        = 19.27 MB
//   block table  4704 x 24 B                   =  0.11 MB
// The witness ring is 16 deep because a replay save of frame f follows the
// forward save of f by at most the rollback depth, and the prediction window is
// well inside that. Misses are counted, never silently dropped.
#include "envelope_shadow.h"

#if defined(ENGINE_FM95)
// FM2K .data geometry only. FM95 has a different image layout and a separate
// savestate body; nothing arms this there.
bool EnvelopeShadow_Enabled() { return false; }
void EnvelopeShadow_OnSave(int, bool) {}
void EnvelopeShadow_Dump(int, const char*) {}
void EnvelopeShadow_Reset() {}
#else

#include "savestate.h"
#include "savestate_internal.h"   // Fletcher32 (an XXH3 wrapper), MAX_ROLLBACK_FRAMES

#include <SDL3/SDL_log.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <windows.h>

namespace {

// ---- image geometry -----------------------------------------------------
// The writable image, as the IDA segment table gives it: .data is the only
// perm=6 segment and there is no separate .bss (MSVC folded the uninitialised
// globals in), so "the writable image" is one contiguous object.
constexpr uintptr_t kDataBase   = 0x0041E000;
constexpr uintptr_t kDataEnd    = 0x00544000;
constexpr size_t    kDataSpan   = kDataEnd - kDataBase;      // 1,204,224
constexpr size_t    kBlockSize  = 256;
constexpr size_t    kBlockCount = kDataSpan / kBlockSize;    // 4,704 exactly
constexpr size_t    kDwordsPerBlock = kBlockSize / 4;        // 64 -> one uint64 mask

constexpr size_t kHashSlots    = (size_t)MAX_ROLLBACK_FRAMES;  // 64
constexpr size_t kWitnessSlots = 16;

// ---- the current envelope, as ranges ------------------------------------
// The eleven regions the save path actually copies (envelope_inversion_study
// section 1.1, cross-checked against savestate.h / savestate_internal.h /
// WaveCAddrs). Overlaps are fine; this is used only to answer "is this byte
// covered by SOMETHING".
//
// HONEST CAVEAT, carried into the report: the object-pool and char-slot copies
// are SPARSE (active slots / loaded slots only). A block inside those ranges is
// marked covered here even if its particular slot was skipped this frame. That
// makes "inside the envelope" the OPTIMISTIC reading, which is the right bias:
// it can only under-report holes, never invent them.
struct Region { uintptr_t base; size_t size; };
constexpr Region kEnvelope[] = {
    { 0x0041FB1C, 4 },                                  // rng seed
    { 0x00424718, 4 },                                  // round_end_flag
    { 0x004259A8, 4 },                                  // current_object_ptr
    { 0x004280D8, SIZE_INPUT_HISTORY },                 // input history ring, 8200
    { WaveCAddrs::OBJECT_LIST_HEADS, WaveCAddrs::OBJECT_LIST_HEADS_SZ },
    { EffectAddrs::EFFECT_SYS2, EffectAddrs::EFFECT_SYS2_SZ },
    { WaveCAddrs::AFTERIMAGE_POOL, WaveCAddrs::AFTERIMAGE_POOL_SZ },
    { ADDR_GAME_STATE, SIZE_GAME_STATE },
    { ADDR_OBJECT_POOL, SIZE_OBJECT_POOL },
    { WaveCAddrs::OBJECT_NODE_POOL, WaveCAddrs::OBJECT_NODE_POOL_SZ },
    { CHAR_SLOT_BASE, NUM_CHAR_SLOTS * CHAR_SLOT_SIZE },
};

// The two SPARSE members of that list. Both save and load walk these per slot
// and skip the ones that are not active / not loaded (`savestate_fm2k_save.cpp`
// :369-377 by .kgt magic byte, :395-403 by active flag; the load mirrors it), so
// a byte here is inside a "covered" range yet may not have been copied at all
// this frame. Divergence in these is NOT a restore bug -- it is exactly the
// residue a declared envelope would start covering.
constexpr Region kSparse[] = {
    { ADDR_OBJECT_POOL, SIZE_OBJECT_POOL },
    { CHAR_SLOT_BASE, NUM_CHAR_SLOTS * CHAR_SLOT_SIZE },
};

// ---- the deliberate carve-outs (E1..E5 of the study's exclusion list) ----
// These are INSIDE the envelope and are deliberately NOT restored, so they must
// light up forward-vs-replay. If they do not, the instrument is broken -- that
// is the self-validation, and it is the reason this table lives in the hook as
// well as in the offline report.
constexpr Region kKnownExclusions[] = {
    { 0x004456FC, 4  },   // E1 g_render_frame_counter
    { 0x00447D7D, 42 },   // E2 effect_sys1 / palette-flash-1
    { 0x004456D0, 44 },   // E3 palette-flash-2 (inside effect_sys2)
    { 0x00447DA9, 40 },   // E4 g_shake_effect_1/_2
    { 0x00447DD4, 4  },   // E5 g_last_frame_time
};
constexpr size_t kKnownCount = sizeof(kKnownExclusions) / sizeof(kKnownExclusions[0]);

// Per-carve-out census, so self-validation can distinguish the two very
// different reasons a carve-out might not show up:
//   diverged > 0                -> FOUND. The instrument sees it.
//   diverged = 0, active = 0    -> INACTIVE. The timer held zero for the whole
//                                  run (no palette flash / no screen shake was
//                                  ever running at a sampled instant), so there
//                                  was nothing that COULD diverge. Not a
//                                  failure of the instrument.
//   diverged = 0, active = 1    -> MISSING. It held a live value and still came
//                                  back identical. That IS a broken instrument
//                                  (or a carve-out that no longer carves).
uint64_t g_ke_diverged[kKnownCount];
uint8_t  g_ke_active[kKnownCount];

bool InAnyRegion(const Region* tab, size_t n, uintptr_t a, size_t len) {
    for (size_t i = 0; i < n; ++i) {
        if (a < tab[i].base + tab[i].size && tab[i].base < a + len) return true;
    }
    return false;
}

// Bytes of [a, a+len) covered by the envelope. Used once per block at dump
// time, never on the hot path.
size_t CoveredBytes(uintptr_t a, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        if (InAnyRegion(kEnvelope, sizeof(kEnvelope) / sizeof(kEnvelope[0]), a + i, 1)) ++n;
    }
    return n;
}

// ---- accumulated findings ------------------------------------------------
struct BlockStat {
    uint64_t dword_mask;   // bit i set = dword i of this block differed at least once
    uint32_t hits;         // replay-save comparisons in which this block differed
    int32_t  first_frame;
    uint32_t first_match;
    uint32_t fwd_sample;   // the first differing dword, forward value
    uint32_t rep_sample;   // ... and its replay value
    int32_t  first_off;    // byte offset of that dword within the block (-1 = no witness)
};
BlockStat g_block[kBlockCount];   // ~113 KB of BSS; dark or armed, it is static

// ---- lazily allocated rings ---------------------------------------------
uint32_t* g_hash       = nullptr;   // kHashSlots * kBlockCount
int32_t   g_hash_frame[kHashSlots];
uint8_t   g_hash_valid[kHashSlots];
uint32_t  g_hash_fp[kHashSlots];    // gameplay fingerprint of the forward save

uint8_t*  g_witness    = nullptr;   // kWitnessSlots * kDataSpan
int32_t   g_wit_frame[kWitnessSlots];
uint8_t   g_wit_valid[kWitnessSlots];
size_t    g_wit_next   = 0;

uint32_t* g_scratch    = nullptr;   // kBlockCount, the replay-side hash vector

// ---- counters ------------------------------------------------------------
uint64_t g_ev_forward   = 0;   // forward saves hashed
uint64_t g_ev_replay    = 0;   // replay saves seen
uint64_t g_ev_paired    = 0;   // of those, ones with a live forward vector
uint64_t g_ev_badinput  = 0;   // paired but DISCARDED: the resim consumed different inputs
uint64_t g_ev_badfp     = 0;   // paired but DISCARDED: the gameplay fingerprint diverged
uint64_t g_ev_compared  = 0;   // paired AND input-clean AND fingerprint-clean = usable
uint64_t g_ev_nowitness = 0;   // compared, but the byte witness had aged out
uint64_t g_ev_divergent = 0;   // compared events with >= 1 differing block
uint64_t g_blocks_diff  = 0;   // sum of differing blocks over all comparisons
uint32_t g_worst_event  = 0;   // most differing blocks in a single comparison
uint32_t g_match_idx    = 0;
bool     g_arm_failed   = false;
bool     g_first_cmp_logged = false;
size_t   g_span         = kDataSpan;   // clamped at arm time if the tail is not committed

// ---- env gates -----------------------------------------------------------
bool ParseFlag(const char* raw, bool dflt, bool* recognised) {
    std::string v = raw ? raw : "";
    const size_t b = v.find_first_not_of(" \t\r\n");
    const size_t e = v.find_last_not_of(" \t\r\n");
    v = (b == std::string::npos) ? std::string() : v.substr(b, e - b + 1);
    for (char& c : v) c = (char)std::tolower((unsigned char)c);
    *recognised = true;
    if (v.empty()) return dflt;
    if (v == "1" || v == "true" || v == "on" || v == "yes" || v == "enabled") return true;
    if (v == "0" || v == "false" || v == "off" || v == "no" || v == "disabled") return false;
    *recognised = false;
    return dflt;
}

bool WitnessEnabled() {
    static int s = -1;
    if (s < 0) {
        bool ok = true;
        s = ParseFlag(std::getenv("FM2K_ENVELOPE_SHADOW_WITNESS"), true, &ok) ? 1 : 0;
        if (!ok) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[ENVSHADOW] FM2K_ENVELOPE_SHADOW_WITNESS not recognised -- "
                "failing to the default (ON, dword resolution kept)");
            s = 1;
        }
    }
    return s == 1;
}

// Arm on first use. Returns false forever if allocation fails, so a low-memory
// process degrades to "instrument off", never to a crash or a partial ring that
// would silently under-report.
bool EnsureArmed() {
    if (g_arm_failed) return false;
    if (g_hash) return true;

    // Confirm the whole span is committed and readable before we ever touch it.
    // .data's VirtualSize (0x12533C) rounds up to the segment end, but a probe
    // is four lines and turns a would-be AV in a diagnostic build into a clamp.
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t probe = kDataBase;
    size_t reach = 0;
    while (probe < kDataEnd && VirtualQuery((LPCVOID)probe, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const bool readable = (mbi.State == MEM_COMMIT) &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (!readable) break;
        const uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= probe) break;
        reach = (region_end >= kDataEnd) ? kDataSpan : (size_t)(region_end - kDataBase);
        probe = region_end;
    }
    g_span = (reach < kDataSpan) ? (reach / kBlockSize) * kBlockSize : kDataSpan;
    if (g_span < kBlockSize) {
        g_arm_failed = true;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[ENVSHADOW] DISABLED -- .data at 0x%08X is not readable", (unsigned)kDataBase);
        return false;
    }

    g_hash    = (uint32_t*)std::calloc(kHashSlots * kBlockCount, sizeof(uint32_t));
    g_scratch = (uint32_t*)std::calloc(kBlockCount, sizeof(uint32_t));
    if (WitnessEnabled()) g_witness = (uint8_t*)std::calloc(kWitnessSlots, kDataSpan);
    if (!g_hash || !g_scratch) {
        g_arm_failed = true;
        std::free(g_hash);    g_hash = nullptr;
        std::free(g_scratch); g_scratch = nullptr;
        std::free(g_witness); g_witness = nullptr;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[ENVSHADOW] DISABLED -- ring allocation failed");
        return false;
    }
    std::memset(g_hash_frame, -1, sizeof(g_hash_frame));
    std::memset(g_hash_valid,  0, sizeof(g_hash_valid));
    std::memset(g_wit_frame,  -1, sizeof(g_wit_frame));
    std::memset(g_wit_valid,   0, sizeof(g_wit_valid));
    for (size_t i = 0; i < kBlockCount; ++i) g_block[i].first_off = -1;

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[ENVSHADOW] ARMED: .data 0x%08X..0x%08X span=%zu blocks=%zu block_size=%zu "
        "hashring=%zuKB witness=%s(%zu slots, %zuMB). OBSERVE ONLY -- nothing is restored",
        (unsigned)kDataBase, (unsigned)(kDataBase + g_span), g_span,
        g_span / kBlockSize, kBlockSize,
        (kHashSlots * kBlockCount * sizeof(uint32_t)) / 1024,
        g_witness ? "on" : "off", g_witness ? kWitnessSlots : 0,
        g_witness ? (kWitnessSlots * kDataSpan) / (1024 * 1024) : 0);
    return true;
}

void HashImage(uint32_t* out) {
    const uint8_t* p = (const uint8_t*)kDataBase;
    const size_t n = g_span / kBlockSize;
    for (size_t i = 0; i < n; ++i) out[i] = Fletcher32(p + i * kBlockSize, kBlockSize);
}

// Is each carve-out holding a LIVE value right now? Answers the only question
// that separates "the instrument is broken" from "there was nothing to see":
// a shake timer that is zero all run cannot possibly diverge.
void CensusCarveOuts() {
    for (size_t k = 0; k < kKnownCount; ++k) {
        if (g_ke_active[k]) continue;
        const uint8_t* p = (const uint8_t*)kKnownExclusions[k].base;
        for (size_t i = 0; i < kKnownExclusions[k].size; ++i) {
            if (p[i]) { g_ke_active[k] = 1; break; }
        }
    }
}

int FindWitness(int frame) {
    if (!g_witness) return -1;
    for (size_t i = 0; i < kWitnessSlots; ++i) {
        if (g_wit_valid[i] && g_wit_frame[i] == frame) return (int)i;
    }
    return -1;
}

}  // namespace

bool EnvelopeShadow_Enabled() {
    static int s_cached = -1;
    if (s_cached >= 0) return s_cached == 1;
    bool ok = true;
    const char* raw = std::getenv("FM2K_ENVELOPE_SHADOW");
    const bool on = ParseFlag(raw, false, &ok);
    if (!ok) {
        s_cached = 0;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[ENVSHADOW] DISABLED -- FM2K_ENVELOPE_SHADOW=\"%s\" is not a recognised "
            "value (accepted: 1/true/on/yes/enabled, 0/false/off/no/disabled). "
            "Failing SAFE to the default (OFF)", raw ? raw : "");
        return false;
    }
    s_cached = on ? 1 : 0;
    if (on) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[ENVSHADOW] ENABLED (FM2K_ENVELOPE_SHADOW) -- full-image shadow hashing. "
            "DIAGNOSTIC POSTURE ONLY: ~360us added per forward save. Never a shipping default");
    }
    return on;
}

void EnvelopeShadow_OnSave(int frame, bool is_replay_save) {
    if (!EnvelopeShadow_Enabled()) return;
    if (!EnsureArmed()) return;
    if (frame < 0) return;

    const size_t slot = (size_t)frame % kHashSlots;
    const size_t nblocks = g_span / kBlockSize;

    CensusCarveOuts();

    if (!is_replay_save) {
        // FORWARD save: this is the reference. Store the block-hash vector, the
        // gameplay fingerprint (the input-correction control, see below) and,
        // if the witness ring is on, a full byte image so the replay side can
        // refine any divergent block to exact dwords.
        HashImage(g_hash + slot * kBlockCount);
        g_hash_frame[slot] = frame;
        g_hash_valid[slot] = 1;
        g_hash_fp[slot]    = SaveState_CalculateFingerprint();
        if (g_witness) {
            const size_t w = g_wit_next;
            g_wit_next = (g_wit_next + 1) % kWitnessSlots;
            std::memcpy(g_witness + w * kDataSpan, (const void*)kDataBase, g_span);
            g_wit_frame[w] = frame;
            g_wit_valid[w] = 1;
        }
        ++g_ev_forward;
        return;
    }

    // REPLAY save of a frame we already have forward hashes for. Everything
    // below is pure measurement.
    ++g_ev_replay;
    if (!g_hash_valid[slot] || g_hash_frame[slot] != frame) return;
    ++g_ev_paired;

    HashImage(g_scratch);
    const uint32_t* fwd = g_hash + slot * kBlockCount;
    const int wi = FindWitness(frame);
    const uint8_t* wbytes = (wi >= 0) ? (g_witness + (size_t)wi * kDataSpan) : nullptr;
    const uint8_t* live = (const uint8_t*)kDataBase;

    // ---- THE INPUT-CORRECTION CONTROL ----------------------------------
    // Forward-vs-replay divergence has TWO sources, and the study's phase-1
    // definition ("a block that differs is a hole") only accounts for one:
    //
    //   1. state the envelope failed to restore  <- what we are hunting
    //   2. THE RESIM CONSUMED DIFFERENT INPUTS   <- the whole point of rollback
    //
    // When the forward pass ran on a PREDICTED remote input and the resim ran on
    // the CORRECTED one, the two sims are simply different simulations. Every
    // downstream byte -- positions, HP, rng, the object pool, the derived list
    // topology -- legitimately differs, and calling any of it an envelope hole
    // is a category error. Measured on run R2: the input history ring diverged
    // with an all-ones dword mask in thousands of comparisons, which is exactly
    // this and nothing else. (The seam campaign already knows this confound from
    // the other end -- seam_ring_check.py's INPUT-CORRECTION classifier.)
    //
    // So a comparison is USABLE only if the resim consumed identical inputs AND
    // converged on the same gameplay fingerprint. Two independent checks, both
    // cheap, both fail-closed:
    //   * the input history ring (0x4280D8 +0x2008) is byte-identical -- the
    //     complete record of what the sim actually consumed;
    //   * the 7-scalar gameplay fingerprint matches -- the same criterion gekko
    //     itself compares, so a mismatch means the sims genuinely diverged.
    // Discarded comparisons are COUNTED, never silently dropped: their ratio is
    // itself a measurement (how much of a lossy run is input-corrected).
    if (wbytes) {
        constexpr size_t kIhOff = 0x4280D8 - kDataBase;
        if (kIhOff + SIZE_INPUT_HISTORY <= g_span &&
            std::memcmp(wbytes + kIhOff, live + kIhOff, SIZE_INPUT_HISTORY) != 0) {
            ++g_ev_badinput;
            return;
        }
    } else {
        // No witness: fall back to the block hashes over the ring's blocks. This
        // is CONSERVATIVE (the first and last blocks straddle the ring's edges,
        // so it can discard a usable comparison) and never the reverse.
        const size_t b0 = (0x4280D8 - kDataBase) / kBlockSize;
        const size_t b1 = (0x4280D8 + SIZE_INPUT_HISTORY - 1 - kDataBase) / kBlockSize;
        for (size_t b = b0; b <= b1 && b < nblocks; ++b) {
            if (g_scratch[b] != fwd[b]) { ++g_ev_badinput; return; }
        }
    }
    if (SaveState_CalculateFingerprint() != g_hash_fp[slot]) { ++g_ev_badfp; return; }

    ++g_ev_compared;
    if (!wbytes) ++g_ev_nowitness;

    uint32_t ndiff = 0;
    for (size_t b = 0; b < nblocks; ++b) {
        if (g_scratch[b] == fwd[b]) continue;
        ++ndiff;
        BlockStat& s = g_block[b];
        if (s.hits == 0) {
            s.first_frame = frame;
            s.first_match = g_match_idx;
        }
        ++s.hits;
        if (!wbytes) continue;
        // Dword-resolution refinement. This is what lets the offline report say
        // "0x424F24" instead of "somewhere in 0x424F00..0x424FFF".
        const uint32_t* fw = (const uint32_t*)(wbytes + b * kBlockSize);
        const uint32_t* lv = (const uint32_t*)(live   + b * kBlockSize);
        for (size_t d = 0; d < kDwordsPerBlock; ++d) {
            if (fw[d] == lv[d]) continue;
            if (s.first_off < 0) {
                s.first_off  = (int32_t)(d * 4);
                s.fwd_sample = fw[d];
                s.rep_sample = lv[d];
            }
            s.dword_mask |= (uint64_t)1 << d;
            for (size_t k = 0; k < kKnownCount; ++k) {
                if (InAnyRegion(&kKnownExclusions[k], 1,
                                kDataBase + b * kBlockSize + d * 4, 4)) {
                    ++g_ke_diverged[k];
                }
            }
        }
    }
    g_blocks_diff += ndiff;
    if (ndiff) ++g_ev_divergent;
    if (ndiff > g_worst_event) g_worst_event = ndiff;

    // EPISODIC, exactly once per process: proof the instrument fired, without
    // any per-frame logging. Everything else waits for the dump.
    if (!g_first_cmp_logged) {
        g_first_cmp_logged = true;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[ENVSHADOW] first forward-vs-replay comparison: f=%d blocks_differing=%u "
            "witness=%s", frame, ndiff, wbytes ? "yes" : "AGED OUT");
    }
}

void EnvelopeShadow_Dump(int player_index, const char* reason) {
    // Counts first. A DARK run emits this line with zeroes, which is the
    // positive evidence that dark means dark -- but ONCE per process, not once
    // per match: an unarmed instrument has nothing new to say at match 2, and a
    // shipping build should not grow three log lines per session for a
    // diagnostic nobody turned on. Armed runs report at every dump site.
    static bool s_dark_summary_done = false;
    if (!g_hash) {
        if (s_dark_summary_done) return;
        s_dark_summary_done = true;
    }
    // Counted at DWORD resolution wherever a witness gave us one, because the
    // block is the wrong unit for this verdict: block 669 holds three carve-outs
    // AND ordinary afterimage bytes, so "does this 256 B block contain anything
    // unexplained" is true almost by construction and would raise a permanent
    // false RESTORE BUG alarm. The unit that matters is the dword.
    size_t seen = 0, out_blocks = 0, part_blocks = 0, in_blocks = 0;
    size_t dw_out = 0, dw_known = 0, dw_sparse = 0, dw_unexplained = 0, dw_nowitness = 0;
    for (size_t b = 0; b < kBlockCount; ++b) {
        if (!g_block[b].hits) continue;
        ++seen;
        const uintptr_t addr = kDataBase + b * kBlockSize;
        const size_t cov = CoveredBytes(addr, kBlockSize);
        if (cov == 0) ++out_blocks;
        else if (cov < kBlockSize) ++part_blocks;
        else ++in_blocks;
        if (!g_block[b].dword_mask) { ++dw_nowitness; continue; }
        for (size_t d = 0; d < kDwordsPerBlock; ++d) {
            if (!(g_block[b].dword_mask & ((uint64_t)1 << d))) continue;
            const uintptr_t a = addr + d * 4;
            if (InAnyRegion(kKnownExclusions, kKnownCount, a, 4)) { ++dw_known; continue; }
            if (!InAnyRegion(kEnvelope, sizeof(kEnvelope) / sizeof(kEnvelope[0]), a, 4)) {
                ++dw_out; continue;
            }
            // Inside a region that is only copied for ACTIVE/LOADED slots. Not a
            // restore bug: the envelope never claimed those bytes this frame.
            // It is, precisely, what a declared envelope would start covering.
            if (InAnyRegion(kSparse, sizeof(kSparse) / sizeof(kSparse[0]), a, 4)) {
                ++dw_sparse; continue;
            }
            ++dw_unexplained;
        }
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[ENVSHADOW] summary: armed=%s fwd=%llu replay=%llu paired=%llu "
        "drop_input=%llu drop_fp=%llu compared=%llu nowitness=%llu "
        "divergent_events=%llu blocks_seen=%zu out=%zu part=%zu in=%zu "
        "dw_out=%zu dw_known=%zu dw_sparse=%zu dw_UNEXPLAINED=%zu "
        "ke=E1:a%ud%llu,E2:a%ud%llu,E3:a%ud%llu,E4:a%ud%llu,E5:a%ud%llu "
        "worst_event=%u matches=%u (%s)",
        g_hash ? "yes" : "no",
        (unsigned long long)g_ev_forward, (unsigned long long)g_ev_replay,
        (unsigned long long)g_ev_paired, (unsigned long long)g_ev_badinput,
        (unsigned long long)g_ev_badfp, (unsigned long long)g_ev_compared,
        (unsigned long long)g_ev_nowitness, (unsigned long long)g_ev_divergent,
        seen, out_blocks, part_blocks, in_blocks,
        dw_out, dw_known, dw_sparse, dw_unexplained,
        g_ke_active[0], (unsigned long long)g_ke_diverged[0],
        g_ke_active[1], (unsigned long long)g_ke_diverged[1],
        g_ke_active[2], (unsigned long long)g_ke_diverged[2],
        g_ke_active[3], (unsigned long long)g_ke_diverged[3],
        g_ke_active[4], (unsigned long long)g_ke_diverged[4],
        g_worst_event, g_match_idx, reason ? reason : "");
    if (dw_unexplained > 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[ENVSHADOW] *** %zu dword(s) diverge INSIDE a DENSELY-restored save region "
            "and are NOT a known carve-out. On an input-clean, fingerprint-clean "
            "comparison that is a RESTORE BUG, not a hole -- see the CSV ***",
            dw_unexplained);
    }
    if (!g_hash || seen == 0) return;

    char filename[256];
    std::snprintf(filename, sizeof(filename), "FM2K_P%d_envshadow.csv", player_index + 1);
    FILE* f = std::fopen(filename, "w");
    if (!f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[ENVSHADOW] failed to open %s", filename);
        return;
    }
    std::fprintf(f, "# envshadow reason=%s base=0x%08X span=%zu block_size=%zu blocks=%zu "
                    "witness=%s fwd=%llu replay=%llu paired=%llu drop_input=%llu "
                    "drop_fp=%llu compared=%llu nowitness=%llu divergent_events=%llu "
                    "worst_event=%u matches=%u blocks_seen=%zu out=%zu part=%zu in=%zu "
                    "dw_out=%zu dw_known=%zu dw_sparse=%zu dw_unexplained=%zu "
                    "ke=E1:a%ud%llu,E2:a%ud%llu,E3:a%ud%llu,E4:a%ud%llu,E5:a%ud%llu\n",
                 reason ? reason : "", (unsigned)kDataBase, g_span, kBlockSize,
                 g_span / kBlockSize, g_witness ? "on" : "off",
                 (unsigned long long)g_ev_forward, (unsigned long long)g_ev_replay,
                 (unsigned long long)g_ev_paired, (unsigned long long)g_ev_badinput,
                 (unsigned long long)g_ev_badfp, (unsigned long long)g_ev_compared,
                 (unsigned long long)g_ev_nowitness,
                 (unsigned long long)g_ev_divergent, g_worst_event, g_match_idx, seen,
                 out_blocks, part_blocks, in_blocks,
                 dw_out, dw_known, dw_sparse, dw_unexplained,
                 g_ke_active[0], (unsigned long long)g_ke_diverged[0],
                 g_ke_active[1], (unsigned long long)g_ke_diverged[1],
                 g_ke_active[2], (unsigned long long)g_ke_diverged[2],
                 g_ke_active[3], (unsigned long long)g_ke_diverged[3],
                 g_ke_active[4], (unsigned long long)g_ke_diverged[4]);
    // cover = bytes of this block inside the current save envelope (0..256).
    // dword_mask = which 4-byte words within the block ever differed (hex, 0 =
    // the witness had aged out on every hit, so block resolution only).
    std::fprintf(f, "block,addr,hits,first_frame,first_match,cover,first_off,fwd,rep,dword_mask\n");
    for (size_t b = 0; b < kBlockCount; ++b) {
        const BlockStat& s = g_block[b];
        if (!s.hits) continue;
        const uintptr_t addr = kDataBase + b * kBlockSize;
        std::fprintf(f, "%zu,0x%08X,%u,%d,%u,%zu,%d,0x%08X,0x%08X,0x%016llX\n",
                     b, (unsigned)addr, s.hits, s.first_frame, s.first_match,
                     CoveredBytes(addr, kBlockSize), s.first_off, s.fwd_sample,
                     s.rep_sample, (unsigned long long)s.dword_mask);
    }
    std::fclose(f);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[ENVSHADOW] wrote %zu divergent blocks to %s", seen, filename);
}

void EnvelopeShadow_Reset() {
    ++g_match_idx;
    // Frame numbers restart at every battle, so every cached vector is now a
    // trap. The accumulated BLOCK TABLE deliberately survives: the hole list is
    // a session-level result and a green multi-match run must still carry
    // match 1's findings at auto-terminate.
    std::memset(g_hash_frame, -1, sizeof(g_hash_frame));
    std::memset(g_hash_valid,  0, sizeof(g_hash_valid));
    std::memset(g_wit_frame,  -1, sizeof(g_wit_frame));
    std::memset(g_wit_valid,   0, sizeof(g_wit_valid));
    g_wit_next = 0;
}

#endif  // ENGINE_FM95
