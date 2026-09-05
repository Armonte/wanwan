// hooks_game_mode.cpp -- game-mode detection (IsCSSMode/IsBattleMode) + CheckGameModeTransition. Split from hooks.cpp (pure move).
#include "hooks.h"
#include "round_events.h"     // C3.5 -- vs_round_function detour install
#include "css_autoconfirm.h"  // CSS lock-and-confirm for offline replay playback
#include "css_fastsound.h"    // FM2K_FPK_CSS_FASTSOUND: lazy DSound buffers (CSS dip fix)
#include "per_game_patches.h" // damage multiplier MinHook + team-size override
#include "title_mode_select.h" // resolve the real VS menu index; refuse story-only content
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
#include "../ui/screenshot.h"               // FM2KCapture::RememberFrame for the auto-banner pipeline
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
#include "../core/fm95_structs.h"   // typed FM95 pool/round-state mirrors

// ============================================================================

static uint32_t g_last_game_mode = 0;

// Engine-aware phase detection.
// FM2K encodes phase via g_game_mode magic numbers (2000=CSS, 3000+=Battle).
// FM95 keeps g_game_mode near 0/1/10 -- phase lives inside per-object slots
// in the 256-entry pool (type==19 sub_state ∈ [0x28,0xC9] = CSS, type==16
// sub_state ∈ [10,31] = Battle). Walk the pool once per call; could be
// frame-cached if it shows up hot in profiles.
//
// The `mode` argument is preserved so existing call sites keep compiling
// without change. On FM2K it's still load-bearing; on FM95 it's ignored
// and we read the object pool directly.
namespace {
    enum class FM95Phase { Boot, Title, CSS, PostCSS, Battle, MatchEnd, Other };

    inline FM95Phase Fm95ClassifyPhase() {
        const fm95::Fm95ObjectSlot* pool = fm95::Pool();
        for (size_t i = 0; i < fm95::kObjectPoolCount; ++i) {
            const fm95::Fm95ObjectSlot& slot = pool[i];
            uint32_t type = slot.type;
            if (type < 2) continue;            // 0=empty, 1=disabled
            uint32_t sub  = (uint32_t)slot.sub_state;
            if (type == 19) {                  // title_screen_state_machine
                if (sub >= 0x28 && sub <= 0xC9) return FM95Phase::CSS;
                return FM95Phase::Title;
            }
            if (type == 16) {                  // vs_round_function
                if (sub >= 10 && sub <= 31)    return FM95Phase::Battle;
                return FM95Phase::MatchEnd;
            }
            if (type == 21) return FM95Phase::PostCSS;
            if (type == 30 || type == 15) return FM95Phase::Boot;
        }
        return FM95Phase::Other;
    }
}

// Exported (non-static) so main_loop_trampoline.cpp's ClassifyPhase can use
// the same engine-aware logic. Forward-declared in hooks.h.
bool IsCSSMode(uint32_t mode) {
    if constexpr (FM2K::kIsFM2K) {
        return mode == 2000;
    } else {
        (void)mode;
        return Fm95ClassifyPhase() == FM95Phase::CSS;
    }
}

bool IsBattleMode(uint32_t mode) {
    if constexpr (FM2K::kIsFM2K) {
        return mode >= 3000 && mode < 4000;
    } else {
        (void)mode;
        return Fm95ClassifyPhase() == FM95Phase::Battle;
    }
}

// Deterministic FM95 match-END classifier (re-verified from disasm 2026-07-11,
// see docs/dev/fm95_re_findings.md RE-2b). Neither g_game_mode nor the type-16
// pool sub_state cleanly marks "match in progress": FM95 resets g_game_mode to 0
// and cycles the round object at the START OF EVERY ROUND (vs_round_function
// @0x4114A0 cases 0/1/10/11 + obj_post_css_round_intro all write game_mode=0),
// so both signals FLAP at round boundaries -- which is exactly why the earlier
// heuristics tore the session down mid-match. The real match decision is a
// MONOTONIC sim scalar: vs_round_function case 30 ends the match when a win
// counter reaches the round cap. Those live in the saved+fingerprinted
// PLAYER_ROUND_STATE block (0x5E98A0..0x5E9A40), so both peers compute the
// identical decided-frame. Returns: 1 = active round play (game_mode==1, the
// "we're really in a match" gate), 2 = MATCH DECIDED (a win counter >= cap),
// 0 = neither. The ring scan fires the battle-end edge on the first 2 seen
// after any 1 -- exactly once, at the true match end, never at a round break.
int Fm95MatchPhaseByte() {
    if constexpr (FM2K::kIsFM2K) {
        return IsBattleMode(0) ? 1 : 0;
    } else {
        const uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;   // 0x425558
        const fm95::Fm95RoundStateBlock* round = fm95::Round();
        const uint32_t round_count_max = round->round_count_max;
        const uint32_t p1_wins         = round->players[1].win_counter;
        const uint32_t p2_wins         = round->players[2].win_counter;
        if (round_count_max != 0 &&
            (p1_wins >= round_count_max || p2_wins >= round_count_max)) {
            return 2;   // match decided
        }
        return (game_mode == 1) ? 1 : 0;   // 1 = active round play
    }
}

// Battle sync state - ensures both clients start GekkoNet together.
// Exposed non-static so the trampoline (main_loop_trampoline.cpp) can see it;
// the trampoline replaces main_game_loop wholesale and needs to drive the
// battle-entry handshake.
bool g_battle_entry_signaled_pub = false;

// Called every frame to check for game mode transitions
// Public shim so the trampoline (main_loop_trampoline.cpp) can invoke the
// same transition detector the hooks use.
extern "C" void Hook_CheckGameModeTransition_Public();
void CheckGameModeTransition();
extern "C" void Hook_CheckGameModeTransition_Public() { CheckGameModeTransition(); }


// ---- auto-capture: arm on mode entry, shoot once settled ----------------
//
// Settle frames exist because a state's first frame is never its
// representative one. FM2K runs at 100 Hz, so the default is ~0.6 s.
namespace {

enum CapShot { kCapTitle = 0, kCapCss = 1, kCapBattle = 2, kCapCount = 3 };

// EVERY STATE IS A SERIES NOW, not just the front end. The owner reviews
// the shots in the launcher (picks.txt) and wants choice: a CSS with the
// cursors mid-wander and then locked in, a battle at the intro and then
// mid-fight on a random stage. One settled shot per state gave one chance
// per game and it was often the wrong frame.
//
// How many frames to emit per state before moving on. Six front-end
// samples at a ~1s window covers a couple of splash cards plus the title
// on every game measured.
constexpr int kFrontMax = 6;
// CSS: three samples. The first waits the full settle (the roster has
// animated in), the next two follow at a window that lands while the
// autoplay cursors (FM2K_AUTOPLAY_CSS_DWELL) are still browsing and then
// after they lock -- so at least one shot shows the versus layout with
// both cursors placed.
constexpr int kCssMax = 3;
constexpr int kCssWindow = 120;
// Battle: three samples. Settle covers the round intro; the later windows
// are mid-fight with the autoplay inputs, which is where the action is.
constexpr int kBattleMax = 3;
// Measured on colors: at 250 the second battle shot still lands on the VS
// splash; 400 puts it in the fight.
constexpr int kBattleWindow = 400;

// ── Per-character sweep (FM2K_CAPTURE_CHARS=1, FM2K only) ──
// Walks the roster by pressing RIGHT once per cell and shooting the
// highlighted portrait: char_01.png .. char_NN.png, N = the number of
// non-empty slots in g_char_slot_data (capped). Engine-agnostic about the
// grid: the game moves the cursor, we only count. Runs at CSS entry, before
// the CSS series, with the autoplay frozen (screenshot.h).
// STILLNESS, the thing a frame count cannot express. These games fade and
// wipe between screens, and a shot taken mid-transition is a smeared
// half-frame nobody would ever pick as a game's tile. So the dwell below is
// only a FLOOR: once it elapses we additionally wait for the picture to stop
// moving (FM2KCapture::FramesSettled, a coarse thumbnail diff, so an
// animating portrait does not count as motion). kStillWaitMax bounds it, for
// a screen that genuinely never stops moving -- a scrolling attract loop.
constexpr int kStillNeed    = 10;
constexpr int kStillWaitMax = 240;

constexpr int kSweepSettle = 60;    // frames after CSS entry before the first shot
// FRAMES PER CELL: press, let the screen FINISH changing, shoot. Twelve was
// not a dwell, it was a flinch -- the big portrait crossfades in over rather
// more than that, so a good fraction of the roster came out mid-dissolve.
// The stillness gate below is what actually decides; this is the floor.
constexpr int kSweepStep   = 30;
constexpr int kSweepMax    = 50;
bool s_sweep_done = false;
int  s_sweep_left = 0;      // cells still to shoot
int  s_sweep_frames = 0;    // frames into the current cell
int  s_sweep_seq = 0;
// WHERE THE CURSOR IS, not what the screen looks like. Comparing frames
// cannot work here: the CSS animates its fighters, so two frames on the
// same cell differ in thousands of pixels and a pixel hash never repeats
// (measured: 34 shots, every one "changed"). The cursor pair at
// ADDR_P1_CURSOR_POS is the real signal -- a press that does not move it
// hit a row edge, and a press that returns to a cell already shot means
// the grid wrapped and the roster is done. The .player count is only an
// upper bound: colors has 34 entries and about a dozen cells.
int  s_sweep_cells[kSweepMax][2] = {};
int  s_sweep_cell_n = 0;
// RIGHT alone walks ONE ROW: it wraps back to the row's first cell, and
// the roster is a grid (colors: 7 across, more below). So a repeat means
// "row done" -- press DOWN once and keep going. Only a repeat that
// survives a row change means every reachable cell has been shot.
bool s_sweep_tried_down = false;

void SweepReadCursor(int* x, int* y) {
    const int* c = reinterpret_cast<const int*>(FM2K::ADDR_P1_CURSOR_POS);
    *x = c[0];
    *y = c[1];
}

bool SweepSeenCell(int x, int y) {
    for (int i = 0; i < s_sweep_cell_n; ++i) {
        if (s_sweep_cells[i][0] == x && s_sweep_cells[i][1] == y) return true;
    }
    return false;
}

bool SweepWanted() {
    static int s = -1;
    if (s < 0) {
        const char* e = std::getenv("FM2K_CAPTURE_CHARS");
        s = (e && e[0] == '1' && !FM2K::kIsFM95) ? 1 : 0;
    }
    return s == 1;
}

int RosterCount() {
    if constexpr (FM2K::kIsFM95) return 0;
    int n = 0;
    for (size_t i = 0; i < FM2K::CHAR_FILENAME_COUNT && n < kSweepMax; ++i) {
        const char* slot = reinterpret_cast<const char*>(FM2K::ADDR_CHAR_FILENAME_TABLE + i * FM2K::CHAR_FILENAME_STRIDE);
        if (slot[0] != '\0') ++n;
    }
    return n;
}

// Returns true while the sweep owns the CSS (the series waits).
bool SweepTick(uint32_t mode) {
    if (s_sweep_done || !SweepWanted()) return false;
    if (mode != 2000u) {
        // Left CSS before finishing (or never got there): give up quietly.
        if (FM2KCapture::g_sweep_active) { FM2KCapture::g_sweep_active = false; s_sweep_done = true; }
        return false;
    }
    if (!FM2KCapture::g_sweep_active) {
        const int n = RosterCount();
        if (n <= 0) { s_sweep_done = true; return false; }
        FM2KCapture::g_sweep_active = true;
        s_sweep_left = n;
        s_sweep_frames = -kSweepSettle;   // settle first
        s_sweep_seq = 0;
        s_sweep_cell_n = 0;
        s_sweep_tried_down = false;
    }
    ++s_sweep_frames;
    if (s_sweep_frames < 0) return true;
    FM2KCapture::RememberFrame();
    // Same rule as the state series: the step is a floor, and the portrait
    // must have stopped crossfading before this cell is worth writing.
    if (s_sweep_frames >= kSweepStep &&
        FM2KCapture::FramesSettled() < kStillNeed &&
        s_sweep_frames < kSweepStep + kStillWaitMax) {
        return true;
    }
    if (s_sweep_frames >= kSweepStep) {
        int cx = 0, cy = 0;
        SweepReadCursor(&cx, &cy);
        const bool fresh = !SweepSeenCell(cx, cy);
        if (fresh) {
            if (s_sweep_cell_n < kSweepMax) {
                s_sweep_cells[s_sweep_cell_n][0] = cx;
                s_sweep_cells[s_sweep_cell_n][1] = cy;
                ++s_sweep_cell_n;
            }
            s_sweep_tried_down = false;
            char name[32];
            std::snprintf(name, sizeof name, "char_%02d.png", ++s_sweep_seq);
            FM2KCapture::WriteRememberedFrame(name);
        } else if (!s_sweep_tried_down) {
            // The row wrapped: drop to the next one and keep walking.
            s_sweep_tried_down = true;
            FM2KCapture::ResetHeldScore();
            FM2KCapture::g_sweep_nudge_down = true;
            s_sweep_frames = 0;
            FM2KCapture::ResetSettle();
            return true;
        } else {
            // A row change reached nothing new either: the grid is done.
            FM2KCapture::g_sweep_active = false;
            s_sweep_done = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "FM2KCapture: roster sweep ended after %d cell(s)", s_sweep_seq);
            return false;
        }
        FM2KCapture::ResetHeldScore();
        if (--s_sweep_left <= 0) {
            FM2KCapture::g_sweep_active = false;
            s_sweep_done = true;
            return false;
        }
        FM2KCapture::g_sweep_nudge = true;   // step to the next cell
        s_sweep_frames = 0;
    }
    return true;
}
// SAMPLE THE FRONT END DENSELY, because it is SHORT. Measured with the boot
// patch verifiably skipped (boot.txt confirms the flag reaches the hook),
// wanwan still enters mode 1000 at frame 0 and leaves at 57 -- the game has
// no long intro on this path and nothing is patching one away. A 50-frame
// window across a ~55-frame front end therefore yielded two near-identical
// shots. Eight frames per sample spreads six captures over the whole
// sequence instead, which is what actually separates a splash card from the
// title behind it.
constexpr int kFrontWindow = 24;


int  s_cap_seq[kCapCount] = {0, 0, 0};

int CapMax(int i) {
    return i == kCapTitle ? kFrontMax : i == kCapCss ? kCssMax : kBattleMax;
}
int CapWindow(int i) {
    return i == kCapTitle ? kFrontWindow : i == kCapCss ? kCssWindow : kBattleWindow;
}

// Numbered per state: front_01.., css_01.., battle_01.. (css_initial and
// battle used to be single shots; the picker and the review UI go by the
// prefix, so the numbering costs nothing downstream).
void CapFileName(int i, char* out, size_t n) {
    const char* prefix = (i == kCapTitle) ? "front" : (i == kCapCss) ? "css" : "battle";
    std::snprintf(out, n, "%s_%02d.png", prefix, ++s_cap_seq[i]);
}

int CapShotForMode(uint32_t mode) {
    // EVERYTHING BELOW CSS IS "THE FRONT END", mode 0 included. The splash
    // cards live in mode 0, not 1000 -- vanpri's publisher card was the
    // richest frame we could find and mode 1000 itself only lasts ~55
    // frames -- so treating 1000 alone as "the title" both misses the intro
    // entirely and gives the sampler no room to walk a sequence.
    if (mode < 2000u) return kCapTitle;
    if (mode == 2000u) return kCapCss;
    if (mode >= 3000u && mode < 4000u) return kCapBattle;
    return -1;
}

bool s_cap_done[kCapCount] = {false, false, false};
int  s_cap_pending = -1;
int  s_cap_settle_left = 0;
int  s_cap_still_wait  = 0;   // frames spent waiting for the picture to settle
int  s_cap_settle = -1;   // resolved from env on first use

// Mode trace, written next to the captures. Deliberately NOT going through
// the hook's SDL log: that lands at a CWD-relative "logs\\" path, and on the
// --offline launch path nothing appeared anywhere on disk, so the one
// diagnostic that would explain a missed capture was itself unavailable.
// This writes where we already know we can write, and only on transitions,
// so it is not per-frame logging.
void CaptureTraceMode(uint32_t from, uint32_t to) {
    const char* dir = std::getenv("FM2K_CAPTURE_DIR");
    if (!dir || !*dir) return;
    FILE* f = std::fopen((std::string(dir) + "/modes.txt").c_str(), "ab");
    if (!f) return;
    // Frame-stamped: "how long did mode 1000 last" is the question that
    // separates "the title is skipped" from "the title renders blank", and
    // a bare transition list cannot answer it.
    const uint32_t frame = *(volatile uint32_t*)FM2K::ADDR_FRAME_COUNTER;
    std::fprintf(f, "f=%u  %u -> %u\n", frame, from, to);
    std::fclose(f);
}

// Capture is driven ENTIRELY from the per-frame tick. It used to arm from
// the mode-transition branch, which ran first and overwrote the pending
// state before the tick could notice the previous state had ended -- so the
// outgoing shot was silently dropped every time.
// Sentinel the capture-runner polls. Called after EVERY successful write,
// not just on a state exit: battle is the last shot and it completes on its
// DWELL, never on an exit (the game keeps running), so an exit-only check
// meant the sentinel was never written and every run burned its full
// timeout instead of finishing in seconds.
void CaptureMaybeSignalDone() {
}

void CaptureTick(uint32_t mode) {
    if (s_cap_settle < 0) {
        // Dwell, in frames. FM2K runs at 100 Hz, so 300 is ~3 seconds: long
        // enough that a CSS has finished animating in and a title has run
        // its fade, which "capture on entry" never waits for.
        s_cap_settle = 300;
        if (const char* e = std::getenv("FM2K_CAPTURE_DWELL")) {
            const int v = std::atoi(e);
            if (v >= 0 && v < 100000) s_cap_settle = v;
        }
    }

    const int here = CapShotForMode(mode);

    // The per-character sweep goes first in CSS; the CSS series waits.
    if (here == kCapCss && !s_cap_done[kCapCss] && SweepTick(mode)) return;

    if (s_cap_pending < 0) {
        if (here >= 0 && !s_cap_done[here]) {
            s_cap_pending = here;
            // Front end samples on a shorter window so the series spreads
            // across splash+title instead of landing twice on one card.
            s_cap_settle_left = (here == kCapTitle) ? kFrontWindow : s_cap_settle;
            // The screen just changed on purpose; do not count the frames
            // before this as stillness.
            FM2KCapture::ResetSettle();
            s_cap_still_wait = 0;
        }
        return;
    }

    // Still in the armed state: hold this frame and count down. Holding
    // every frame is what makes the shot correct on a SHORT state too --
    // if the state ends before the dwell elapses we still have its last
    // fully-drawn frame, instead of nothing.
    if (here == s_cap_pending) {
        FM2KCapture::RememberFrame();
        if (s_cap_settle_left > 0) { --s_cap_settle_left; return; }
        // Dwell done. Now hold until the screen stops moving, so the shot
        // lands on the finished picture instead of inside its transition.
        if (FM2KCapture::FramesSettled() < kStillNeed &&
            s_cap_still_wait < kStillWaitMax) {
            ++s_cap_still_wait;
            return;
        }
        s_cap_still_wait = 0;
        char name[64];
        CapFileName(s_cap_pending, name, sizeof name);
        if (FM2KCapture::WriteRememberedFrame(name)) {
            if (s_cap_seq[s_cap_pending] < CapMax(s_cap_pending)) {
                FM2KCapture::ResetHeldScore();
                // Keep sampling this state: re-arm for the next window so
                // the series walks the state (splash -> title, cursors
                // browsing -> locked, intro -> mid-fight) rather than
                // stopping at the first drawn frame.
                s_cap_settle_left = CapWindow(s_cap_pending);
                FM2KCapture::ResetSettle();
            } else {
                s_cap_done[s_cap_pending] = true;
                s_cap_pending = -1;
                CaptureMaybeSignalDone();
            }
        }
        return;
    }

    // State ended before the dwell: commit the last frame we held in it.
    {
        char name[64];
        CapFileName(s_cap_pending, name, sizeof name);
        if (FM2KCapture::WriteRememberedFrame(name)) {
            s_cap_done[s_cap_pending] = true;
            CaptureMaybeSignalDone();
        }
    }
    s_cap_pending = -1;
    if (here >= 0 && !s_cap_done[here]) {
        s_cap_pending = here;
        s_cap_settle_left = s_cap_settle;
    }

    for (int i = 0; i < kCapCount; ++i) if (!s_cap_done[i]) return;
    const char* dir = std::getenv("FM2K_CAPTURE_DIR");
    FILE* f = std::fopen((std::string(dir ? dir : ".") + "/.capture_done").c_str(), "wb");
    if (f) std::fclose(f);
}

}  // namespace

void CheckGameModeTransition() {
    if constexpr (FM2K::kIsFM95) {
        // FM95: CSS↔battle transitions do NOT move the game_mode scalar (title,
        // CSS, and battle all sit near 0) and IsBattleMode/IsCSSMode pool-walk
        // the CURRENT state (ignoring their mode arg), so the scalar-edge
        // detector below can NEVER see the transition -- its `!was && is` test
        // compares the current phase to itself. That left g_battle_entry_
        // signaled false forever → Netplay_StartBattle never fired → the CPW
        // netplay match hung at the CSS→battle transition (both peers stuck in
        // battle phase with no session). Drive the entry/exit signals off the
        // CLASSIFIED phase edge instead. Runs every tick (RunCss/BattleTick
        // call this). Scalar-based session_kind + screenshots are FM2K-only.
        static int s_last_phase = -1;   // -1 init, 0 other, 1 css, 2 battle
        const int phase = IsBattleMode(0) ? 2 : (IsCSSMode(0) ? 1 : 0);

        // DETERMINISTIC battle-END for real netplay. The live pool edge (`phase`)
        // is read post-prediction and differs per peer by up to the prediction
        // window, so signaling end off it tore the two peers down frames apart
        // (drain timeout, mis-aligned rematch). Instead scan the CONFIRMED phase
        // ring (recorded in the gekko advance handler) for the first
        // battle->non-battle edge -- identical frame on both peers -- and signal
        // with a swap_frame derived from it. Fires once per battle session.
        if (!g_spectator_mode && !g_offline_mode && Netplay_IsActive()) {
            uint32_t end_frame = 0;
            if (Netplay_Fm95PollConfirmedBattleEnd(&end_frame)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    ">>> FM95 LEAVING BATTLE (confirmed edge f=%u) -- signaling peer",
                    end_frame);
                Netplay_SignalBattleEndAtFrame(end_frame);
                g_battle_entry_signaled = false;
            }
        }

        if (phase != s_last_phase) {
            const int prev = s_last_phase;
            s_last_phase = phase;
            SharedMem_PublishSessionKind((uint8_t)phase);
            // Leaving battle hides the HUD score box again (no publisher
            // on FM95 yet, so this is only ever a no-op reset here).
            if (phase != 2) SharedMem_ClearHudScores();
            if (g_spectator_mode) return;
            if (phase == 2 && prev != 2 &&
                !g_battle_entry_signaled && !Netplay_IsActive()) {
                // ENTER battle ONCE per match. The pool phase dips to non-battle
                // at every round boundary (round intro/result cycles the type-16
                // object) and returns -- re-firing this edge mid-battle re-ran
                // the CSS->battle swap barrier and DESYNCED (seen f=373 right
                // after a 2nd ENTERING BATTLE). Gate on !g_battle_entry_signaled
                // (covers the handshake window) AND !Netplay_IsActive() (covers
                // the active battle + match-end drain): entry can only fire from
                // CSS with no live battle session. The deterministic win-counter
                // END poll above is the ONLY thing that clears the flag, so a
                // round-boundary dip/return never re-enters.
                if (!g_offline_mode && Netplay_IsConnected()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        ">>> FM95 ENTERING BATTLE (phase edge) -- signaling peer");
                    Netplay_SignalBattleEntry();
                    g_battle_entry_signaled = true;
                }
            } else if (prev == 2 && phase != 2) {
                // LEAVE battle. Netplay teardown + the g_battle_entry_signaled
                // reset are owned by the deterministic win-counter poll above, so
                // we must NOT clear it here on a round-boundary dip. Only the
                // offline / stray-session case (no peer barrier) tears down and
                // clears on this live edge.
                if (g_offline_mode && Netplay_IsActive()) {
                    Netplay_EndBattle();
                    g_battle_entry_signaled = false;
                }
            }
        }
        return;
    }

    uint32_t current_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;

    // Title-menu VS resolution + the story-only refusal. Deliberately OUTSIDE
    // the mode-change branch and NOT in the input hook alone: a HOST parked in
    // the pre-rendezvous HELLO loop reaches game_mode 1000 and then runs no
    // input frames at all (measured on DragonPuppy -- the guest refused in 90ms
    // while the host span on HELLO for the full harness timeout). The
    // trampolines drive this function every outer-loop iteration, so it is the
    // one place that ticks whether or not the sim does. Self-latching: the call
    // is three reads until it resolves, then nothing.
    TitleModeSelect_Tick(current_mode, /*from_input_hook=*/false);

    if (current_mode != g_last_game_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: game_mode changed: %u -> %u", g_last_game_mode, current_mode);

        // Publish session_kind to SharedMem so the launcher can forward
        // it to the hub. Used by the spectator-join /F decision: when
        // someone requests to spectate us, the hub returns our current
        // session_kind in spectate_grant so their launcher knows
        // whether to set FM2K_BOOT_TO_BATTLE=1 (we're in battle) or
        // not (we're in CSS -- spec needs natural CSS init for the
        // CSS-state snapshot to apply cleanly at mode==2000).
        uint8_t kind = 0;  // menu / unknown
        if (current_mode == 2000u) kind = 1;            // CSS
        else if (current_mode >= 3000u && current_mode < 4000u)
            kind = 2;                                    // battle
        SharedMem_PublishSessionKind(kind);

        // HUD score box lives exactly as long as the battle does. Without
        // this the last match's score would sit over the CSS and the title
        // screen until the next round 1 overwrote it. round_events.cpp
        // publishes it; this is the only place that takes it away.
        if (kind != 2) SharedMem_ClearHudScores();

        // Auto-capture banner pipeline. A mode boundary ARMS a shot; the
        // tick below takes it. No-op when FM2K_AUTO_CAPTURE wasn't set.
        //
        // WHY ARMING AND CAPTURING ARE SEPARATE. This used to take the shot
        // right here, on the transition frame, and latch the "done" flag
        // regardless of the return value. Both halves were wrong and they
        // compounded: the transition frame is the WORST moment to photograph
        // a state -- the title has not drawn, the CSS portraits are not
        // blitted, and early on the game window does not exist yet, so the
        // capture dropped the frame -- and latching on failure made that
        // permanent. Measured: battle captured fine (late, window up) while
        // title and css_initial never appeared at all, on every game.
        if (FM2KCapture::IsActive()) {
            CaptureTraceMode(g_last_game_mode, current_mode);
        }

    }

    // Per-frame: take an armed shot once it has settled. Outside the
    // transition branch on purpose -- see CaptureTick.
    if (FM2KCapture::IsActive()) {
        CaptureTick(current_mode);
    }

    if (current_mode != g_last_game_mode) {
        // Whenever the game crosses any CSS↔battle boundary, kick off a
        // 300-frame (3 second @ 100 Hz) state dump so we can diff
        // working games (WonderfulWorld) vs broken ones (SFZ, StudioS
        // Fighters) at battle entry. Reset the per-window counter.
        // BATTLE-DIAG window: gated on FM2K_BATTLE_DIAG=1. Off by default
        // because each open-window dumps 300 frames of [BD ##] state into
        // the log per CSS↔battle transition -- useful for diffing broken
        // FM2K variants at battle entry but pure noise during normal play.
        // Cached once at first call.
        static int s_battle_diag_enabled = -1;
        if (s_battle_diag_enabled < 0) {
            const char* v = std::getenv("FM2K_BATTLE_DIAG");
            s_battle_diag_enabled = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
        }
        bool boundary = s_battle_diag_enabled &&
            ((IsCSSMode(g_last_game_mode)    && IsBattleMode(current_mode)) ||
             (IsBattleMode(g_last_game_mode) && IsCSSMode(current_mode)));
        if (boundary) {
            g_battle_diag_frames_remaining = 300;
            g_battle_diag_frame_idx = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                ">>> BATTLE-DIAG window OPEN (300 frames) <<<");
        }

        // Spectator: state init no longer mirrors via local game_mode flips.
        // Host emits PIN_RNG / RESET_INPUT_STATE / SOUND_INIT ops as part of
        // the SessionEvent stream (see Netplay_StartBattle, Netplay_ProcessCSS,
        // CheckFullyConnected); the spectator applies them in
        // SpectatorNode_PopFrameInputs's head-drain at the moment its local
        // sim is about to consume the corresponding INPUT -- same logical
        // frame the host's pin happened. Eliminates the off-by-N race
        // between host-side write and spectator-side game_mode flip.
        //
        // Just bail before any host-only player-state-machine work runs,
        // and let the BATTLE-DIAG window + capture pipeline above still
        // observe the boundary for diagnostics.
        if (g_spectator_mode) {
            g_last_game_mode = current_mode;
            return;
        }

        bool was_battle = IsBattleMode(g_last_game_mode);
        bool is_battle = IsBattleMode(current_mode);

        if (!was_battle && is_battle) {
            // ENTERING BATTLE MODE - Signal entry, but DON'T start GekkoNet yet
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                ">>> ENTERING BATTLE MODE - Signaling remote, waiting for sync");

            // Drain the trailing CSS-frame capture before the battle session
            // gates capture_and_return out. The pair sitting in g_capture_p[]
            // is the LAST CSS frame -- the one whose confirm flipped game_mode.
            // Without this flush, spectator never sees that frame, never
            // flips its own game_mode, and desyncs at battle entry.
            // #66: under CSS rollback capture_and_return was gated out (CSS
            // recorded via the pending-confirm ring), so g_capture_p is stale;
            // drain the ring's remaining confirmed CSS frames instead.
            if (g_css_rollback) {
                Netplay_CssFlushRemaining();
            } else {
                extern void Hook_FlushPendingCapture();
                Hook_FlushPendingCapture();
            }

            if (!g_offline_mode && Netplay_IsConnected()) {
                // Boot-to-battle (test/dev) skips the CSS rendezvous that
                // normally arms the battle-entry barrier. Arm it here so the
                // two direct-to-battle peers accept each other's signal
                // instead of deadlocking at "waiting for sync". No-op (and
                // never reached as an arm) on the production CSS path.
                static const bool s_btb = []{
                    const char* e = std::getenv("FM2K_BOOT_TO_BATTLE");
                    return e && e[0] == '1' && e[1] == '\0';
                }();
                if (s_btb) {
                    extern void Netplay_ArmBattleEntryBarrier();
                    Netplay_ArmBattleEntryBarrier();
                }
                Netplay_SignalBattleEntry();
                g_battle_entry_signaled = true;
                // NOTE: GekkoNet will be started in Hook_UpdateGameState
                // after both clients have entered battle mode
            }
        } else if (was_battle && !is_battle) {
            // LEAVING BATTLE MODE - Signal exit; trampoline tears down the
            // GekkoNet session at the agreed swap_frame so both peers
            // (and any spectators) destroy in lockstep. Synchronous
            // teardown here would leave a few frames of mismatched session
            // state on the wire and risk spectator desync at the boundary.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "<<< LEAVING BATTLE MODE - Signaling swap_frame exit");
            if (!g_offline_mode && Netplay_IsActive()) {
                Netplay_SignalBattleEnd();
            } else if (Netplay_IsActive()) {
                // Offline / stress paths still tear down synchronously --
                // there's no remote peer to negotiate with.
                Netplay_EndBattle();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GekkoNet session stopped (offline path)");
            }
            g_battle_entry_signaled = false;
        }

        g_last_game_mode = current_mode;
    }
}

