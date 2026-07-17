// hooks_game_mode.cpp -- game-mode detection (IsCSSMode/IsBattleMode) + CheckGameModeTransition. Split from hooks.cpp (pure move).
#include "hooks.h"
#include "round_events.h"     // C3.5 — vs_round_function detour install
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
#include "../core/main_loop_trampoline.h"  // TrampolineMainLoop — owns the outer loop
#include "../audio/sound_rollback.h"        // Mike Z desired/actual sound layer
#include "../netplay/spectator_node.h"      // spectator playback queue accessors
#include "../ui/input_binder.h"             // FM2KInputBinder::Sample_Win32 + Bindings
#include "../ui/screenshot.h"               // FM2KCapture::SaveScreenshot for the auto-banner pipeline
#include "../ui/fc_hud.h"                   // IsChatInputActive — gate local input during typing
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
// FM95 keeps g_game_mode near 0/1/10 — phase lives inside per-object slots
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

// Current FM95 phase as a small ordinal: 2=battle, 1=CSS, 0=other. Classifies
// the LIVE object pool -- callers that need a DETERMINISTIC (cross-peer-equal)
// phase must record this per confirmed frame in the gekko advance handler, not
// read it off the live (post-prediction) pool. On FM2K this maps the game_mode
// scalar the same way so the ring path compiles for both engines.
int Fm95CurrentPhaseByte() {
    return IsBattleMode(0) ? 2 : (IsCSSMode(0) ? 1 : 0);
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


void CheckGameModeTransition() {
    if constexpr (FM2K::kIsFM95) {
        // FM95: CSS↔battle transitions do NOT move the game_mode scalar (title,
        // CSS, and battle all sit near 0) and IsBattleMode/IsCSSMode pool-walk
        // the CURRENT state (ignoring their mode arg), so the scalar-edge
        // detector below can NEVER see the transition — its `!was && is` test
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
                    ">>> FM95 LEAVING BATTLE (confirmed edge f=%u) — signaling peer",
                    end_frame);
                Netplay_SignalBattleEndAtFrame(end_frame);
                g_battle_entry_signaled = false;
            }
        }

        if (phase != s_last_phase) {
            const int prev = s_last_phase;
            s_last_phase = phase;
            SharedMem_PublishSessionKind((uint8_t)phase);
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
                        ">>> FM95 ENTERING BATTLE (phase edge) — signaling peer");
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

    if (current_mode != g_last_game_mode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Hooks: game_mode changed: %u -> %u", g_last_game_mode, current_mode);

        // Publish session_kind to SharedMem so the launcher can forward
        // it to the hub. Used by the spectator-join /F decision: when
        // someone requests to spectate us, the hub returns our current
        // session_kind in spectate_grant so their launcher knows
        // whether to set FM2K_BOOT_TO_BATTLE=1 (we're in battle) or
        // not (we're in CSS — spec needs natural CSS init for the
        // CSS-state snapshot to apply cleanly at mode==2000).
        uint8_t kind = 0;  // menu / unknown
        if (current_mode == 2000u) kind = 1;            // CSS
        else if (current_mode >= 3000u && current_mode < 4000u)
            kind = 2;                                    // battle
        SharedMem_PublishSessionKind(kind);

        // Auto-capture banner pipeline. Drives one screenshot at each
        // mode-boundary the launcher's capture-runner cares about,
        // then writes a "DONE" sentinel file the launcher polls
        // before terminating the game. No-op when FM2K_AUTO_CAPTURE
        // wasn't set (FM2KCapture::IsActive() short-circuits).
        if (FM2KCapture::IsActive()) {
            static bool s_captured_title = false;
            static bool s_captured_css   = false;
            static bool s_captured_battle = false;
            if (!s_captured_title && current_mode == 1000) {
                FM2KCapture::SaveScreenshot("title.png");
                s_captured_title = true;
            }
            if (!s_captured_css && current_mode == 2000) {
                FM2KCapture::SaveScreenshot("css_initial.png");
                s_captured_css = true;
            }
            if (!s_captured_battle && current_mode >= 3000
                && current_mode < 4000) {
                FM2KCapture::SaveScreenshot("battle.png");
                s_captured_battle = true;
                // All three core captures done — touch a sentinel so
                // the launcher's capture-runner sees "ready to kill".
                // Empty zero-byte file; the launcher polls for its
                // existence on a 250 ms cadence.
                FILE* f = std::fopen(
                    (std::string(std::getenv("FM2K_CAPTURE_DIR") ?
                                 std::getenv("FM2K_CAPTURE_DIR") : ".")
                     + "/.capture_done").c_str(), "wb");
                if (f) std::fclose(f);
            }
        }

        // Whenever the game crosses any CSS↔battle boundary, kick off a
        // 300-frame (3 second @ 100 Hz) state dump so we can diff
        // working games (WonderfulWorld) vs broken ones (SFZ, StudioS
        // Fighters) at battle entry. Reset the per-window counter.
        // BATTLE-DIAG window: gated on FM2K_BATTLE_DIAG=1. Off by default
        // because each open-window dumps 300 frames of [BD ##] state into
        // the log per CSS↔battle transition — useful for diffing broken
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
        // sim is about to consume the corresponding INPUT — same logical
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
            // is the LAST CSS frame — the one whose confirm flipped game_mode.
            // Without this flush, spectator never sees that frame, never
            // flips its own game_mode, and desyncs at battle entry.
            extern void Hook_FlushPendingCapture();
            Hook_FlushPendingCapture();

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
                // Offline / stress paths still tear down synchronously —
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

