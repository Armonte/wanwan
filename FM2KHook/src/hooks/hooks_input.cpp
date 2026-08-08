// hooks_input.cpp -- SOCD cleaning + autoplay (CSS/battle) + capture/battle-diag. Split from hooks.cpp (pure move).
#include "hooks.h"
#include "round_events.h"     // C3.5 -- vs_round_function detour install
#include "css_autoconfirm.h"  // CSS lock-and-confirm for offline replay playback
#include "css_fastsound.h"    // FM2K_FPK_CSS_FASTSOUND: lazy DSound buffers (CSS dip fix)
#include "per_game_patches.h" // damage multiplier MinHook + team-size override
#include "render_simd.h"      // FM2K_BLIT_SIMD: blit + case -10 blur reimplementation
#include "globals.h"

#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <list>
#include <thread>
#include <condition_variable>
#include <atomic>
#include "netplay.h"
#include "control_channel.h"
#include "../netplay/game_hash.h"
#include "imgui_overlay.h"
#include "shared_mem.h"
#include "savestate.h"  // CHAR_SLOT_BASE, CHAR_SLOT_SIZE (corrected by Wave C audit)
#include "../core/main_loop_trampoline.h"  // TrampolineMainLoop -- owns the outer loop
#include "../audio/sound_rollback.h"        // Mike Z desired/actual sound layer
#include "../netplay/spectator_node.h"      // spectator playback queue accessors
#include "../ui/input_binder.h"             // FM2KInputBinder::Sample_Win32 + Bindings
#include "../ui/screenshot.h"               // FM2KCapture::SaveScreenshot for the auto-banner pipeline
#include "../ui/fc_hud.h"                   // IsChatInputActive -- gate local input during typing
#include "../vfs/fpk_reader.h"              // FM2K_FPK_VFS: inflate a slim .fpk -> original asset bytes
#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cfloat>   // _controlfp_s, _PC_53, _MCW_PC, _RC_NEAR, _MCW_RC, _MCW_EM
#include <cstdint>
#include <string>

// Pin the x87 FPU control word to a fixed precision + rounding mode on the
// game thread. IDA audit found the binary never calls _controlfp / fldcw and
// DirectDraw's SetCooperativeLevel is invoked without DDSCL_FPUPRESERVE, so
// the default precision is whatever DirectDraw/driver/OS happens to leave.
// That varies across machines and is almost certainly why peer simulations
// diverge on movement (velocity, collision, normalization all use floats).
// Call this before every gameplay tick to override any mid-frame changes.
// MXCSR bit layout (SSE control/status register):
//   bit 15 FZ (flush-to-zero)
//   bits 13-14 RC (round control): 00 nearest, 01 down, 10 up, 11 truncate
//   bits 7-12 exception masks (we set all = masked)
//   bit 6 DAZ (denormals-are-zero)
//   bits 0-5 exception flags (sticky, we clear)
// We want: round-to-nearest-even, all exceptions masked, no FZ/DAZ, flags clear.
// Value 0x1F80 is the x86 default but we pin it explicitly to ensure both
#include "hooks_internal.h"

// =============================================================================
// SOCD (Simultaneous Opposite Cardinal Direction) cleaner
// =============================================================================
//
// Ported from CCCaster's filterSimulDirState() in
// targets/DllControllerUtils.hpp. Applied to the 11-bit FM2K input before
// returning from Hook_GetPlayerInput, AFTER the facing-direction swap so
// modes operate on game-internal F/B/U/D semantics rather than raw stick.
//
// FM2K bits: LEFT=0x001 RIGHT=0x002 UP=0x004 DOWN=0x008.
//
// Modes (FM2K_SOCD_MODE env var, default = 1):
//   0 -- Default        L+R held → R wins  | U+D held → U wins
//   1 -- L/R Cancel     L+R held → neutral | U+D held → U wins   <-- DEFAULT (Hitbox-style)
//   2 -- U/D Cancel     L+R held → R wins  | U+D held → neutral
//   3 -- Both Cancel    L+R held → neutral | U+D held → neutral
//   4 -- Up Bias        L+R held → R wins  | U+D held → U wins   (= 0 today)
//   5 -- Hitbox+Up      L+R held → neutral | U+D held → U wins   (= 1 today)
//
// Modes 4/5 are kept for CCCaster compat; behavior currently matches 0/1
// because FM2K already drops DOWN on U+D (no separate "down-wins" branch
// to disambiguate). If we want a DOWN-priority crouch-bias mode later
// it'd be a 7th value.
static int g_socd_mode_runtime = -1;  // -1 = uninitialized; runtime override of env var.

static int Hook_GetSOCDMode() {
    if (g_socd_mode_runtime < 0) {
        const char* env = std::getenv("FM2K_SOCD_MODE");
        g_socd_mode_runtime = 1;  // tournament default
        if (env && env[0] >= '0' && env[0] <= '5' && env[1] == '\0') {
            g_socd_mode_runtime = env[0] - '0';
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SOCD: mode=%d (FM2K_SOCD_MODE override='%s')",
            g_socd_mode_runtime, env ? env : "<unset>");
    }
    return g_socd_mode_runtime;
}

// Set the SOCD mode at runtime. Used by HOST_CONFIG receiver to adopt
// host's mode, and by future settings UI. Mode must be 0..5; out-of-range
// values are ignored.
extern "C" void Hook_SetSOCDMode(int mode) {
    if (mode < 0 || mode > 5) return;
    if (g_socd_mode_runtime != mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "SOCD: mode change %d -> %d", g_socd_mode_runtime, mode);
        g_socd_mode_runtime = mode;
    }
}

// Public accessor for the SOCD mode -- needed by netplay.cpp's HOST_CONFIG
// broadcaster which can't reach the static.
extern "C" int Hook_GetSOCDModePublic() {
    return Hook_GetSOCDMode();
}

// Public wrapper around Hook_ApplySOCD (declared in hooks.h). Used by
// netplay.cpp to pre-apply SOCD on the host's confirmed inputs before
// storing them into the spectator stream / .fm2krep file -- eliminates
// SOCD-mode-mismatch between host and spec (or original-vs-replay) as
// a source of sim divergence. Defined later in this TU but forward-
// declared here so the trivial wrapper compiles.
uint16_t Hook_ApplySOCD(uint16_t input);
uint16_t Hook_ApplySOCD_Public(uint16_t input) {
    return Hook_ApplySOCD(input);
}

// Public autoplay-input computer. Mirrors the body of the
// FM2K_PARITY_AUTOPLAY_BATTLE block inside Hook_GetPlayerInput so the
// stress-mode `gekko_add_local_input` call site can feed gekko the
// same per-player input the engine's Hook_GetPlayerInput would
// dispatch. Keep these two implementations identical -- they are the
// shape of an autoplay determinism contract.
uint16_t Hook_ComputeAutoplayBattleInput(int player_id) {
    static int s_cache = -1;
    if (s_cache < 0) {
        const char* v = std::getenv("FM2K_PARITY_AUTOPLAY_BATTLE");
        s_cache = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (s_cache != 1) return 0;
    // Engine-aware battle gate: FM2K battle = game_mode 3000-3999; FM95
    // battle = game_mode 0 (IsBattleMode walks the object pool). The old
    // raw `< 3000` check returned 0 for every FM95 frame → autoplay never
    // fired on CPW.
    const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (!IsBattleMode(game_mode)) return 0;

    // Deterministic per-frame seed from the engine's input-buffer index
    // (FM2K 0x447EE0 / FM95 0x437700, part of the saved input-tracking
    // region so forward sim and rollback replay hash identically).
    uint32_t seed = *(uint32_t*)FM2K::ADDR_INPUT_BUFFER_INDEX;
    seed ^= (uint32_t)player_id * 0x9E3779B9u;
    seed = (seed ^ (seed >> 16)) * 0x7feb352du;
    seed = (seed ^ (seed >> 15)) * 0x846ca68bu;
    seed = seed ^ (seed >> 16);
    const uint32_t phase = (seed >> 28) & 0x7u;
    const uint32_t dirbits = (seed >> 8) & 0xFu;
    const uint32_t btnbits = (seed >> 4) & 0xFu;
    uint16_t out = 0;
    switch (phase) {
        case 0:
        case 1:
            out = 0; break;
        case 2:
            out = (uint16_t)(dirbits & 0xFu); break;
        case 3:
            out = (uint16_t)((1u << (4 + (btnbits & 3u)))); break;
        case 4:
            out = (uint16_t)((dirbits & 0xFu) | (1u << (4 + (btnbits & 3u)))); break;
        case 5:
            out = (uint16_t)((1u << (4 + (btnbits & 3u))) |
                             (1u << (4 + ((btnbits >> 2) & 3u))));
            break;
        case 6:
            out = (uint16_t)(1u << (dirbits & 3u)); break;
        case 7:
            out = (uint16_t)(0x4u | (1u << (4 + (btnbits & 3u))));
            break;
    }
    if ((out & 0x3u) == 0x3u) out &= ~0x3u;
    if ((out & 0xCu) == 0xCu) out &= ~0xCu;
    return out;
}

// Dwell anchor state for Hook_ComputeAutoplayCssInput -- file scope so
// the netplay CSS-sync path can reset it deterministically (the
// in-function gap heuristic only covers the offline path).
static bool g_css_autoplay_in_css = false;
void Hook_AutoplayCssResetDwell() {
    g_css_autoplay_in_css = false;
}

// CSS counterpart of Hook_ComputeAutoplayBattleInput: the wander/dwell/
// confirm synthetic input for ONE player. Two call sites:
//   - Hook_GetPlayerInput's FM2K_PARITY_AUTOPLAY block (offline / stress:
//     called for both player_ids, drives the engine directly), and
//   - Netplay_ProcessCSS (netplay: called for the LOCAL player only, fed
//     into the CSS GekkoSession so BOTH peers' sims consume the identical
//     lockstep stream).
// The netplay call site is the fix for the 2026-06-11 split-brain: the
// old in-hook block short-circuited the CSS netplay branch and each
// peer's sim ran on locally-hashed inputs for BOTH players. The hashes
// key off local counters (buf_idx, dwell entry tick) which skew under
// packet loss, so the two sims locked different chars at different
// frames (P1 flipped at css_frame=733, P2 at 3796) and every rematch
// downstream failure traced here. All cadence/state below derives from
// g_input_buffer_index (0x447EE0), never call counters, so the value is
// stable within a tick and replay-deterministic.
uint16_t Hook_ComputeAutoplayCssInput(int player_id) {
    static int s_dwell = -1;
    if (s_dwell < 0) {
        const char* v = std::getenv("FM2K_AUTOPLAY_CSS_DWELL");
        s_dwell = v ? (int)(std::atof(v) * 100.0) : 0;
        if (s_dwell < 0) s_dwell = 0;
    }
    const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (game_mode != 2000u) {
        g_css_autoplay_in_css = false;
        return 0;
    }
    // g_input_buffer_index is an index into the 1024-frame input ring --
    // it WRAPS (975 -> 39 observed mid-CSS, 2026-06-11 16:11), so it can
    // never be used as an elapsed-time anchor: the old `buf - anchor`
    // dwell wrapped to ~4e9 at the ring boundary and the function fell
    // into the confirm phase by accident (that wrap, not the dwell, is
    // what had been locking chars all along). Instead keep our own
    // monotonic per-CSS tick counter, incremented whenever the ring
    // index moves.
    const uint32_t buf_idx = *(uint32_t*)0x447EE0;
    static uint32_t s_prev_buf = 0;
    static uint32_t s_ticks = 0;
    static uint32_t s_session_salt = 0;
    if (!g_css_autoplay_in_css) {
        // Fresh CSS phase: reset elapsed time, bump the salt so the
        // browse path and confirm buttons differ between matches.
        g_css_autoplay_in_css = true;
        s_prev_buf = buf_idx;
        s_ticks = 0;
        ++s_session_salt;
    } else if (buf_idx != s_prev_buf) {
        ++s_ticks;
        s_prev_buf = buf_idx;
    }
    const uint32_t in_css = s_ticks;
    const uint32_t salt = s_session_salt * 0x68E31DA4u;
    // 1Hz diag: which phase is the autoplay in, and is the clock sane?
    {
        static uint32_t s_diag_last_ms = 0;
        const uint32_t now_ms = GetTickCount();
        if (now_ms - s_diag_last_ms >= 1000) {
            s_diag_last_ms = now_ms;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[CSSAP] buf=%u ticks=%u salt=%u dwell=%d phase=%s",
                buf_idx, in_css, s_session_salt, s_dwell,
                (s_dwell > 0 && in_css < (uint32_t)s_dwell) ? "wander"
                                                            : "confirm");
        }
    }
    if (s_dwell > 0 && in_css < (uint32_t)s_dwell) {
        // Wander: direction (or idle) stable for ~10-frame steps (rapid browse
        // -- 2x the old 20-frame cadence) and only idle 1/4 of the time so the
        // cursor keeps moving back-to-back. Per-player offset + per-match salt
        // give independent, varied browsing. This stress-walks the per-move
        // char reload as fast as the engine will load.
        const uint32_t step = in_css / 10u;
        uint32_t h = (step + 1u) * 0x9E3779B9u ^ ((uint32_t)player_id << 16)
                   ^ salt;
        h = (h ^ (h >> 16)) * 0x7feb352du;
        h ^= h >> 15;
        return (h & 3u) ? (uint16_t)(1u << ((h >> 1) & 3u)) : (uint16_t)0u;
    }
    if (s_dwell == 0) {
        return ((in_css % 30u) == 0u) ? (uint16_t)0x010u : (uint16_t)0u;
    }
    // Post-dwell lock-in walk: confirm pulse every 30 ticks; if the
    // cursor parked on an empty grid cell (sel=-1, confirm is a no-op)
    // the offset direction nudge steps it to a neighbor before the next
    // pulse. Once the engine latches the confirm (action state set) it
    // stops reading this player's input, so the nudges are inert after
    // lock-in.
    const uint32_t k = in_css % 30u;
    if (k == 0u) {
        uint32_t h = (in_css / 64u + 1u) * 0x85EBCA6Bu
                   ^ ((uint32_t)player_id << 8) ^ salt;
        h = (h ^ (h >> 13)) * 0xC2B2AE35u;
        return (uint16_t)(1u << (4 + (h & 3u)));
    }
    if (k == 15u) {
        uint32_t h = (in_css / 30u + 1u) * 0x9E3779B9u
                   ^ ((uint32_t)player_id << 16) ^ salt;
        h = (h ^ (h >> 16)) * 0x7feb352du;
        return (uint16_t)(1u << (h & 3u));
    }
    return 0;
}

uint16_t Hook_ApplySOCD(uint16_t input) {
    constexpr uint16_t LEFT  = 0x001;
    constexpr uint16_t RIGHT = 0x002;
    constexpr uint16_t UP    = 0x004;
    constexpr uint16_t DOWN  = 0x008;
    const int mode = Hook_GetSOCDMode();

    if ((input & (LEFT | RIGHT)) == (LEFT | RIGHT)) {
        if (mode & 1) {
            input &= ~(LEFT | RIGHT);  // cancel both
        } else {
            input &= ~LEFT;            // R wins
        }
    }
    if ((input & (UP | DOWN)) == (UP | DOWN)) {
        if (mode & 2) {
            input &= ~(UP | DOWN);     // cancel both
        } else {
            input &= ~DOWN;            // U wins
        }
    }
    return input;
}

// SOCD for the SPECTATOR/REPLAY consume path (hooks_getinput.cpp's
// g_spectator_mode branch, the ONLY caller). Lives here so the comment does
// not push hooks_getinput.cpp past the 1000-line cap.
//
// WHAT THIS PATH FEEDS: inputs popped from pb_queue, i.e. values the HOST's
// engine already consumed. They were SOCD-resolved at capture on the host
// (Hook_ApplySOCD_Public in netplay_battle_events.cpp:524/804), so re-cleaning
// them here is a second application on top of a resolved value.
//
// THAT DOUBLE-APPLY IS NOT A DESYNC SOURCE, and this change does not claim to
// fix one. Hook_ApplySOCD is a pure conflict-STRIP: after one pass no L+R and
// no U+D remain, so a second pass is the identity function for EVERY mode --
// which is exactly the property netplay_battle_events.cpp:796-804 already
// relies on ("Spec's later SOCD-application becomes idempotent ... so it
// doesn't matter what mode the spec is in"). The facing swap applied just
// before it exchanges L and R, which cannot create a conflict either.
//
// So why guard it at all: a mode that needs HISTORY rather than a pure strip
// (the "last direction wins" families people keep asking for) would NOT be
// idempotent, and the second application would then silently rewrite recorded
// input using the VIEWER's setting. Skipping it on the replay path -- where
// the viewer's setting has nothing to do with the recording -- removes that
// trapdoor before someone falls through it.
//
// SCOPE, deliberately narrow: offline replay only (FM2K_REPLAY_FILE set on
// this process, i.e. LaunchReplayPlayer). A LIVE spectator keeps re-applying,
// so its sim stays byte-identical to today; live netplay and local play never
// reach this function at all (they take the Netplay_IsActive / CSS / binder /
// vanilla branches, each with its own Hook_ApplySOCD call, all untouched).
uint16_t Hook_ApplySOCDForSpecConsume(uint16_t input) {
    static int s_offline_replay = -1;
    if (s_offline_replay < 0) {
        const char* v = std::getenv("FM2K_REPLAY_FILE");
        s_offline_replay = (v && v[0]) ? 1 : 0;
    }
    if (s_offline_replay == 1) return input;   // already resolved by the recorder
    return Hook_ApplySOCD(input);
}

// Pending input pair captured by Hook_GetPlayerInput's capture_and_return,
// at file scope so Hook_FlushPendingCapture can drain it from outside the
// hook (specifically at CSS→battle transition, before the battle session
// gates capture_and_return out). Without this drain the trailing CSS frame
// -- the one carrying the confirm input that flips game_mode 2000→3000 --
// stays trapped in g_capture_p[] and the spectator never receives it.
uint16_t g_capture_p[2] = {0, 0};
uint32_t g_capture_recorded_idx = UINT32_MAX;

// Flush the pending (p1, p2) capture to the spectator stream and clear
// the pending state so the next capture starts from scratch. Idempotent:
// safe to call when nothing is pending (no-op).
extern "C" void Hook_FlushPendingCapture() {
    if (g_capture_recorded_idx == UINT32_MAX) return;
    SpectatorNode_OnFrameConfirmed(g_capture_p[0], g_capture_p[1]);
    g_capture_p[0] = g_capture_p[1] = 0;
    g_capture_recorded_idx = UINT32_MAX;
}

// Battle-transition diagnostic. Activates a frame-by-frame state-dump
// window centered on every game_mode transition so we can compare what
// games like StudioS Fighters / SFZ are doing differently from
// WonderfulWorld at battle entry. The window is a few hundred frames so
// we catch the bail-back-to-CSS pattern (typical: ~140 frames in
// "battle" before reverting).
int  g_battle_diag_frames_remaining = 0;  // counts down per call
uint32_t g_battle_diag_frame_idx = 0;     // counter for log lines

extern "C" void Hook_BattleDiag_TickIfActive() {
    if (g_battle_diag_frames_remaining <= 0) return;

    uint32_t mode  = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    uint32_t rng   = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
    // g_input_buffer_index @ 0x447EE0
    uint32_t buf_idx = *(uint32_t*)0x447EE0;
    // g_last_frame_time @ 0x447DD4
    uint32_t lft   = *(uint32_t*)0x447DD4;
    // g_frame_time_ms @ 0x41E2F0
    uint32_t fr_ms = *(uint32_t*)0x41E2F0;

    // Char slot active flags (0xDF41 from each slot base) + state flags
    // (0x7CA6) -- battle init reads these to decide whether to stay in
    // battle. Slots 0 and 1 are P1/P2.
    constexpr uintptr_t SLOT_BASE = 0x4D1D90;
    constexpr size_t    SLOT_SZ   = 57407;
    uint8_t s0_active = *(uint8_t*)(SLOT_BASE + 0xDF41);
    uint8_t s1_active = *(uint8_t*)(SLOT_BASE + SLOT_SZ + 0xDF41);
    uint8_t s0_flags  = *(uint8_t*)(SLOT_BASE + 0x7CA6);
    uint8_t s1_flags  = *(uint8_t*)(SLOT_BASE + SLOT_SZ + 0x7CA6);
    // The two main_game_loop prologue writes -- slot+0xDF75, slot+0xDF79
    uint32_t s0_dF75 = *(uint32_t*)(SLOT_BASE + 0xDF75);
    uint32_t s0_dF79 = *(uint32_t*)(SLOT_BASE + 0xDF79);

    // Round timer + HPs (real addresses from IDA: g_p1_hp = 0x4DFC85,
    // p2 = +0xDF40 stride. The ones at 0x47010C / 0x47030C are
    // g_demo_mode_player_id, NOT player HP. Wrong addresses earlier
    // showed hp=0/0 always which masked an important signal.)
    uint32_t round_timer = *(uint32_t*)0x470060;
    constexpr uintptr_t HP_BASE = 0x4DFC85;
    constexpr uintptr_t HP_STRIDE = 57407;
    uint32_t p1_hp = *(uint32_t*)(HP_BASE);
    uint32_t p2_hp = *(uint32_t*)(HP_BASE + HP_STRIDE);

    // vs_round_function state machine -- these decide whether battle
    // proceeds or bails back to CSS:
    //   g_score_value @ 0x470050 -- countdown timer; case 200 decrements
    //     each frame, reaching -1 routes to case 300 (round end → CSS).
    //     If battle init sets this to a small/negative value, battle
    //     immediately bails. Almost certainly StudioS's bug signature.
    //   round_state_field -- *(g_object_data_ptr + 338) -- the case
    //     selector itself (1 → 100 → 110 → 200 → 300 etc).
    //   g_round_end_flag @ 0x424718 -- case 200 also bails to 300 if
    //     this is set.
    //   g_round_state @ 0x47004C -- top-level round phase (0/1/2).
    //   g_game_mode_flag @ 0x470058 -- VS / Story / Team selector that
    //     changes which init-path runs in case 1.
    //   g_round_limit @ 0x470048 -- number of rounds to play.
    //   g_game_timer @ 0x470044 -- match clock.
    int32_t score_value     = *(int32_t*)0x470050;
    uint32_t round_end_flag = *(uint32_t*)0x424718;
    uint32_t round_state    = *(uint32_t*)0x47004C;
    uint8_t  game_mode_flag = *(uint8_t*)0x470058;
    uint32_t round_limit    = *(uint32_t*)0x470048;
    uint32_t game_timer     = *(uint32_t*)0x470044;
    // LABEL_201 in vs_round_function: bail to CSS when these hit round_limit
    // in Story/Team mode. Both incremented by case 410/420/430/510/520/530.
    // g_match_phase @ 0x4DFC6D (in P1 char slot), g_round_sub_state @ 0x4EDCAC
    uint32_t match_phase    = *(uint32_t*)0x4DFC6D;
    uint32_t round_sub      = *(uint32_t*)0x4EDCAC;
    // g_round_timer_counter @ 0x424F00 -- used by game_state_manager's
    // CSS-confirm timer (counts to 100 then transitions to battle)
    uint32_t round_tick_ctr = *(uint32_t*)0x424F00;
    uint32_t obj0_state338  = 0;
    int32_t  obj0_state342  = 0;  // case-110/112 countdown timer
    void* obj_data_ptr = *(void**)0x4CFA00;
    if (obj_data_ptr) {
        obj0_state338 = *(uint32_t*)((uint8_t*)obj_data_ptr + 338);
        obj0_state342 = *(int32_t*)((uint8_t*)obj_data_ptr + 342);
    }
    // lParam @ 0x430114 -- case-100 Story-mode score = 100*lParam - 1
    uint32_t lparam_save = *(uint32_t*)0x430114;
    // Object pool active type-4 fighter count -- case-200 Story-mode bails
    // when this < 2. Replicate the EXACT walk from vs_round_function:
    //   if (entry.type == 4 &&
    //       entry[+4] == 0 &&
    //       hp[char_slot[entry[+342]]] != 0) ++count;
    // Pool: 0x4701E0, 1024 entries × 382 bytes. Per-entry +342 holds
    // the char-slot index this object belongs to; HP at HP_BASE +
    // slot_idx * 57407.
    // Re-derived from the actual asm at 0x408E90:
    //   pool base = 0x4701E0 (g_object_pool.payload)
    //   eax starts at base + 0x156, iterates +0x17E (382) per entry.
    //   cmp [eax-0x156], 4   → entry+0   = type     (uint32)
    //   cmp [eax+4],     0   → entry+0x15A (=346) = alive_flag (uint32, 0=alive)
    //   mov ebp, [eax]       → entry+0x156 (=342) = slot_idx (uint32)
    //   * 57407, +g_p1_hp, hp != 0
    int active_type4 = 0;
    int total_type4 = 0;
    {
        const uint8_t* pool = (const uint8_t*)0x4701E0;
        for (int i = 0; i < 1024; i++) {
            const uint8_t* e = pool + i * 382;
            uint32_t type = *(const uint32_t*)(e + 0);
            if (type != 4) continue;
            total_type4++;
            uint32_t alive_flag = *(const uint32_t*)(e + 346);
            if (alive_flag != 0) continue;
            uint32_t slot_idx = *(const uint32_t*)(e + 342);
            if (slot_idx >= 8) continue;
            uint32_t slot_hp = *(const uint32_t*)(HP_BASE + slot_idx * HP_STRIDE);
            if (slot_hp == 0) continue;
            active_type4++;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[BD %4u] mode=%u rng=0x%08X "
        "mp=%u rs=%u rtc=%u "         // LABEL_201 trigger: mp/rs hitting rlim
        "rs338=%u score=%d t4=%d/%d "
        "ref=%u gmf=%u rlim=%u gt=%u "
        "hp=%u/%u",
        g_battle_diag_frame_idx, mode, rng,
        match_phase, round_sub, round_tick_ctr,
        obj0_state338, score_value, active_type4, total_type4,
        round_end_flag, game_mode_flag, round_limit, game_timer,
        p1_hp, p2_hp);
    (void)fr_ms; (void)s0_dF75; (void)s0_dF79;
    (void)buf_idx; (void)lft; (void)round_timer;
    (void)obj0_state342; (void)round_state; (void)lparam_save;
    (void)s0_active; (void)s0_flags; (void)s1_active; (void)s1_flags;

    g_battle_diag_frame_idx++;
    g_battle_diag_frames_remaining--;
}

