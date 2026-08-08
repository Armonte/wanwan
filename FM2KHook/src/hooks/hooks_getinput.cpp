// hooks_getinput.cpp -- get_player_input detour (FM2K) + FM95 split-input hooks + binder sample. Split from hooks.cpp.
#include "hooks.h"
#include "hooks_internal.h"
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
#include "hooks_internal.h"

// ============================================================================
// GAME MODE DETECTION


// ============================================================================
// HOOK IMPLEMENTATIONS
// ============================================================================

// ============================================================================
// FM95 INPUT HOOKS
// FM95 splits the input read into two single-arg functions instead of FM2K's
// dispatched (player_id, input_type) pair. Each FM95 hook drives the same
// path the FM2K Hook_GetPlayerInput offline branch does:
//   1. Sample our binder (FM2KInputBinder::Sample_Win32) for the local
//      player's slot. Picks up our remappable keyboard + XInput gamepad
//      bindings -- eventually the only input surface the user sees once
//      we hide CPW's titlebar and wrap it in our UI.
//   2. Apply facing flip via g_p_facing_snap[25*player_idx] -- the SAME
//      logic CPW's native get_player_input_p1/p2 does, ported here so
//      we can return the engine-relative bits the input ring expects.
//   3. Mask START on CSS (matches FM2K's ~0x400 strip on game_mode 2000).
//   4. Apply SOCD.
// FM95 doesn't have CSS-magic-mode 2000, so the START mask uses the
// engine-aware IsCSSMode (object pool walk) instead.
//
// Netplay rollback path: same as FM2K -- the rollback driver overrides
// ProcessGameInputs (single-arg, FM95-compat) to write into the input
// history rings post-poll. Hook_GetPlayerInput_FM95_P*'s job here is the
// LOCAL input read for both offline and the local input queued into
// GekkoNet via AddLocalInput.
// ============================================================================
//
// Note on facing flip: FM95's input ring stores engine-relative bits
// (forward/back), not screen-relative (left/right). The native fn applies
// L↔R swap when facing is flipped. We do the same so our binder output
// matches what CPW's downstream (motion table, character_state_machine)
// expects.

constexpr uintptr_t FM95_FACING_SNAP_BASE = 0x5E98A8;  // g_p_facing_snap

// True when the binder has ANY binding (primary or alt) for this player
// slot. The binder must only override players it has bindings FOR -- an
// all-NONE slot has to fall back to the engine's own input read, or that
// player is silently fed a permanent 0 ("P2 controls & pause dead in
// offline" report: a profile with P2 rows cleared).
static bool BinderSlotHasBindings(int s) {
    const auto& sb = FM2KInputBinder::Bindings(s);
    for (size_t i = 0; i < (size_t)FM2KInputBinder::Bit::COUNT; ++i) {
        if (sb.bits[i].source     != FM2KInputBinder::Binding::Source::NONE ||
            sb.bits_alt[i].source != FM2KInputBinder::Binding::Source::NONE) {
            return true;
        }
    }
    return false;
}

// Fold FM95 facing (g_p_facing_snap[25*idx]) into a RAW 11-bit input, then
// mask START on CSS and apply SOCD. Shared by the binder-sample path and the
// netplay synced-input path so both produce engine-relative bits the same way
// the native get_player_input_p1/p2 do. facing_idx = CPW per-player index (1
// for P1, 2 for P2).
static uint16_t Fm95FoldFacingSocd(uint16_t input, int facing_idx) {
    const uint8_t facing = *(const uint8_t*)
        (FM95_FACING_SNAP_BASE + (uintptr_t)facing_idx * 25u);
    if (facing) {
        const uint16_t left_bit  = (input & 0x001);
        const uint16_t right_bit = (input & 0x002);
        input = (input & ~0x003) | (left_bit << 1) | (right_bit >> 1);
    }
    if (IsCSSMode(*(uint32_t*)FM2K::ADDR_GAME_MODE)) {
        input &= (uint16_t)~0x400u;
    }
    return Hook_ApplySOCD(input);
}

// Netplay/stress consume path. When a battle session is active, the FM95
// input hooks must return gekko's SYNCED input (not the live binder), or the
// rollback re-sim re-polls local input every AdvanceEvent → gekko's stream is
// ignored and any held key across a rollback boundary re-samples differently
// = guaranteed desync. gekko carries RAW input (Gap-3 convention); we fold
// facing HERE at inject time off the savestate-covered facing snap, so a
// rollback across a turnaround re-maps correctly. slot = 0 (P1) / 1 (P2).
static bool Fm95TryNetplayInput(int slot, int facing_idx, int* out) {
    const uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    // Battle: gekko-synced RAW input, fold facing at inject (Gap-3).
    if (Netplay_IsActive() && IsBattleMode(mode)) {
        *out = (int)Fm95FoldFacingSocd(Netplay_GetInput(slot), facing_idx);
        return true;
    }
    // CSS lockstep: the control-channel-synced input pair. BOTH peers' sims
    // must consume the SAME Netplay_GetCSSInput or they confirm characters at
    // different frames and phase-diverge (one reaches battle, the other stalls
    // in CSS -- the 3b symptom). Mirror of FM2K Hook_GetPlayerInput's CSS
    // branch: SOCD only, NO facing fold (CSS cursor nav is screen-relative).
    // The LOCAL input (autoplay-nav / binder) is captured separately in
    // Netplay_ProcessCSS and fed to the gekko CSS session; here we return what
    // that session DELIVERED, so the sim runs lockstep.
    if (Netplay_IsConnected() && IsCSSMode(mode)) {
        *out = (int)Hook_ApplySOCD(Netplay_GetCSSInput(slot));
        return true;
    }
    return false;
}

// FM95 autoplay title→CSS walk (FM2K_PARITY_AUTOPLAY). RE-9: the whole
// pre-battle flow is edge-detected from the input ring, so marching to battle
// = pulse an attack bit into both players' rings. Title needs TWO confirms
// (wake, then commit) each with a ~10-frame input-RELEASE gap (obj_title_demo_
// loop result[120]/[30] waits), so this uses a slow 3-on / 21-off pattern
// rather than FM2K's tight 4-tick edge -- reliability over speed. CSS then
// confirms each player's cursor on char 0. Battle inputs come from
// Hook_ComputeAutoplayBattleInput via the netplay consume path once the stress
// session starts. Returns -1 when autoplay-nav is off / already in battle.
// Non-static: the netplay CSS path (netplay_css.cpp) feeds the same pulse into
// the CSS gekko lockstep session (FM2K's Hook_ComputeAutoplayCssInput is
// game_mode==2000-gated and returns 0 on FM95).
int Fm95ComputeAutoplayNav() {
    static int s_cache = -1;
    if (s_cache < 0) {
        const char* v = std::getenv("FM2K_PARITY_AUTOPLAY");
        s_cache = (v && std::strcmp(v, "1") == 0) ? 1 : 0;
    }
    if (s_cache != 1) return -1;
    // Once battle is reached the netplay/autoplay-battle path owns input.
    if (IsBattleMode(*(uint32_t*)FM2K::ADDR_GAME_MODE)) return -1;
    const uint32_t buf_idx = *(uint32_t*)FM2K::ADDR_INPUT_BUFFER_INDEX;
    return ((buf_idx % 24u) < 3u) ? 0x010 : 0x000;   // button1 pulse
}

static uint16_t Fm95SampleBinderForPlayer(int binder_slot, int facing_idx) {
    // binder_slot picks which set of bindings to read (0 = P1 bindings, 1 =
    // P2 bindings). Earlier this was hardcoded to 0 for both players, which
    // meant P2 mirrored P1 and any second controller went unused.
    //
    // Focus behavior: Sample_Win32 is focus-correct by construction
    // (queue-synced keyboard + gated gamepads), so an unfocused FM95
    // game reads all-zero here -- no caller-side gate.
    return Fm95FoldFacingSocd(FM2KInputBinder::Sample_Win32(binder_slot),
                              facing_idx);
}

int __cdecl Hook_GetPlayerInput_FM95_P1(int player_idx) {
    // Slice F: while the chat input box is open the local fighter
    // shouldn't react to typed keys. Suppressed here for offline /
    // single-client testing -- full netplay correctness needs the
    // gate at the input-binder Sample call instead so the zero
    // makes it onto the wire (otherwise the peer sees real inputs).
    if (fc_hud::IsChatInputActive() && g_player_index == 0) return 0;
    // Netplay/stress battle: gekko's synced input wins over the live binder.
    if (int synced; Fm95TryNetplayInput(/*slot=*/0, player_idx, &synced)) {
        return synced;
    }
    // Autoplay title/CSS walk (unattended stress/parity runs).
    if (int nav = Fm95ComputeAutoplayNav(); nav >= 0) return nav;
    // First-time binder init mirrors the FM2K path. Done lazily so we don't
    // race with CPW's window/SDL/etc. init. Cheap once warmed up.
    static bool s_warmed = false;
    if (!s_warmed) {
        char buf[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, buf, sizeof(buf)) > 0) {
            const char* slash = std::strrchr(buf, '\\');
            if (!slash) slash = std::strrchr(buf, '/');
            const char* base = slash ? slash + 1 : buf;
            std::string stem = base;
            auto dot = stem.find_last_of('.');
            if (dot != std::string::npos) stem.resize(dot);
            FM2KInputBinder::SetGameProfile(stem.c_str());
        }
        FM2KInputBinder::Init();
        FM2KInputBinder::Load();
        s_warmed = true;
    }
    // Per-player takeover -- mirror of the FM2K binder branch: an all-NONE
    // binder slot falls back to CPW's native input read instead of a
    // permanent 0.
    if (!BinderSlotHasBindings(0)) {
        return original_get_player_input_p1
            ? original_get_player_input_p1(player_idx) : 0;
    }
    return (int)Fm95SampleBinderForPlayer(/*binder_slot=*/0, player_idx);
}

int __cdecl Hook_GetPlayerInput_FM95_P2(int player_idx) {
    if (fc_hud::IsChatInputActive() && g_player_index == 1) return 0;
    // Netplay/stress battle: gekko's synced input (slot 1) wins over the
    // live binder -- same consume path as P1.
    if (int synced; Fm95TryNetplayInput(/*slot=*/1, player_idx, &synced)) {
        return synced;
    }
    // Autoplay title/CSS walk -- same pulse both players so title confirms
    // (OR of both rings) and each CSS cursor locks.
    if (int nav = Fm95ComputeAutoplayNav(); nav >= 0) return nav;
    // Reads the P2 binder slot (slot 1) so a second device -- or fallback
    // to keyboard P2 bindings -- drives the second player on offline /
    // dual-client tests where both players are local.
    if (!BinderSlotHasBindings(1)) {
        return original_get_player_input_p2
            ? original_get_player_input_p2(player_idx) : 0;
    }
    return (int)Fm95SampleBinderForPlayer(/*binder_slot=*/1, player_idx);
}

// Hook: GetPlayerInput
// CSS: return synced input from control channel
// Battle: return synchronized input from GekkoNet with facing adjustment
int __cdecl Hook_GetPlayerInput(int player_id, int input_type) {
    // Slice F: chat-mode input gate. Same caveat as the FM95 hooks
    // above -- works for offline single-client testing; netplay
    // correctness wants the gate at the input-binder Sample call.
    if (fc_hud::IsChatInputActive() && player_id == g_player_index) return 0;
    uint32_t game_mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    // (Removed FM2K_INPUT_DUMP block -- calling original_get_player_input
    // for diagnostic purposes had a SIDE EFFECT: it ran FM2K's keyboard-
    // poll → .ini-binding pipeline, which leaked .ini-bound key presses
    // into FM2K's internal edge-detection state EVEN WHEN our binder was
    // supposed to be the sole input source. Symptom was that custom
    // binder binds AND game.ini binds both fired simultaneously. Bit
    // mappings are confirmed (A=0x010 .. F=0x200, START=0x400 by
    // induction); diagnostic served its purpose, now it's gone.)

    // Single capture-and-return funnel. EVERY return path goes through
    // this lambda so we record the (p1, p2) input pair into the host's
    // session_history every time the input-buffer-index ticks (= one
    // full FM2K frame). Recording starts at FM2K boot -- captures title-
    // screen no-ops, auto-mash, pre-rendezvous CSS, post-rendezvous
    // GekkoNet-merged inputs, battle frames -- one canonical log spanning
    // the entire connection. Late-joining spectators get this whole log
    // via SendSessionBackfillTo and replay deterministically from frame 0.
    //
    // Spectators (g_spectator_mode) DO NOT record -- they consume from
    // pb_queue, not produce. Stress / offline DO record but the log is
    // never sent (no subscribers).
    // capture_and_return: every returned input from this hook on the HOST
    // side is the source of truth for the spectator stream. We pair the
    // current frame's (p1, p2) returns and emit them via
    // SpectatorNode_OnFrameConfirmed at the moment the frame boundary
    // ticks (g_input_buffer_index advances). The spectator drives its
    // local FM2K from that exact same input pair, popped one per sim
    // tick. Because every change to FM2K's state is input-driven from a
    // canonical default state at boot, replaying the input log in order
    // produces a 1:1 sim -- title-screen auto-mash, CSS cursor moves,
    // battle commands all included.
    //
    // The pending pair lives at file scope (g_capture_*) instead of
    // lambda statics so Hook_FlushPendingCapture() can drain the trailing
    // CSS frame at CSS→battle transition -- without that flush, the LAST
    // CSS frame's pair (the one whose confirm input flips game_mode) sits
    // in g_capture_p[] forever because the next frame's capture is gated
    // out by Netplay_IsActive once the battle session starts. Spectator
    // never sees that frame, never flips game_mode, desync.
    //
    // SKIP CONDITIONS:
    //   * g_spectator_mode: spectator only consumes, never produces.
    //   * Netplay_IsActive() (battle): GekkoNet runahead+rollback fires
    //       this hook ~5x per real frame. Battle confirmed-frame capture
    //       is gated in netplay.cpp's AdvanceEvent handler instead.
    auto capture_and_return = [player_id](int result) -> int {
        if (g_spectator_mode) return result;
        if (Netplay_IsActive()) return result;
        // #66 CSS rollback: once the CSS gekko session is driving advances,
        // this hook fires speculatively during re-sim (like battle). Skip it
        // and let Netplay_CssFlushConfirmed record confirmed-only inputs
        // through the pending-confirm ring -- otherwise speculative CSS inputs
        // leak into .fm2krep + the live spectator stream (the e5fe11f class).
        // Pre-session / title CSS frames (no synced session) still record here.
        if (Netplay_IsCssRollbackRecording()) return result;
        const uint32_t cur_idx = *(uint32_t*)0x447EE0;
        if (cur_idx != g_capture_recorded_idx) {
            if (g_capture_recorded_idx != UINT32_MAX) {
                SpectatorNode_OnFrameConfirmed(g_capture_p[0], g_capture_p[1]);
            }
            g_capture_recorded_idx = cur_idx;
        }
        g_capture_p[player_id & 1] = (uint16_t)result;
        return result;
    };

    // Title-screen menu-cursor write. Must fire on BOTH host AND spectator --
    // it's a state side-effect of the auto-title-skip protocol, not an
    // input. session_history only records returned input values, so a
    // spectator replaying host's recorded auto-mash button-A pulses would
    // navigate from g_menu_selection=0 (default = "VS CPU"/first option)
    // and end up in the wrong scene tree. We force g_menu_selection=1
    // ("VS Player") on every node so the same recorded input pattern
    // resolves to the same menu transitions everywhere.
    {
        static const char* env_skip = std::getenv("FM2K_AUTO_TITLE_SKIP");
        const bool auto_skip = !(env_skip && std::strcmp(env_skip, "0") == 0);
        static bool s_cursor_set_global = false;
        if (auto_skip && !s_cursor_set_global && game_mode == 1000) {
            *(uint32_t*)0x424780 = 1;  // g_menu_selection = VS Player
            s_cursor_set_global = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "TitleMenuCursor: pre-set g_menu_selection=1 (host or spectator)");
        }
    }

    // Spectator process -- SINGLE source of truth: the popped input pair
    // (host's recorded p1/p2 for the current sim frame). No keyboard read,
    // no auto-mash, no fall-through to anything else. The hook is only
    // ever called from inside RunSpectatorTick → original_process_game_inputs,
    // which we only invoke after popping a frame from the queue.
    //
    // Battle-mode facing fix mirrors host's branch -- same 11-bit input,
    // same left/right swap when char_active && !state_flag_8.
    if (g_spectator_mode) {
        uint16_t input = (player_id == 0)
            ? SpectatorNode_GetCurrentP1Input()
            : SpectatorNode_GetCurrentP2Input();

        constexpr size_t CHAR_ACTIVE_FLAG_OFFSET = 0xDF41;
        constexpr size_t CHAR_STATE_FLAGS_OFFSET = 0x7CA6;
        bool facing_reversed = true;
        if (game_mode >= 3000 && game_mode < 4000) {
            uintptr_t slot_base = CHAR_SLOT_BASE + (size_t)input_type * CHAR_SLOT_SIZE;
            uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET);
            if (char_active != 0) {
                uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET);
                if ((char_flags & 8) == 0) facing_reversed = false;
            }
        }
        if (!facing_reversed) {
            uint16_t left_bit  = (input & 0x001);
            uint16_t right_bit = (input & 0x002);
            input = (input & ~0x003) | (left_bit << 1) | (right_bit >> 1);
        }
        input = Hook_ApplySOCDForSpecConsume(input);  // see hooks_input.cpp
        return (int)input;  // spectator path doesn't record
    }

    // Auto-mash through title screen → menu → CSS. Default ON unless
    // FM2K_AUTO_TITLE_SKIP=0. With the boot-to-title patch (push 0x0C)
    // the game starts in title_screen_manager (g_game_mode=1000). We
    // pre-set g_menu_selection=1 (VS Player is always index 1 in
    // g_titleMenu_modeList[]) and pulse button A (bit 4 = 0x010) on
    // alternate frames until g_game_mode flips to 2000 (CSS reached).
    //
    // Critical: alternate per-FRAME, not per-CALL. get_player_input
    // is called twice per frame (once for each player) -- if we
    // increment a counter on every call and use parity, P1 and P2
    // get opposite values and neither sees a rising edge after frame
    // 1. Use g_input_buffer_index @ 0x447EE0 instead -- it ticks once
    // per frame from the game's own input pipeline, so both players'
    // calls in the same frame return the same value.
    {
        static const char* env_skip = std::getenv("FM2K_AUTO_TITLE_SKIP");
        const bool auto_skip = !(env_skip && std::strcmp(env_skip, "0") == 0);
        static bool s_done = false;
        static bool s_cursor_set = false;
        static uint32_t s_started_frame = 0;
        if (auto_skip && !s_done) {
            if (game_mode >= 2000) {
                s_done = true;
                uint32_t now = *(uint32_t*)0x447EE0;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "AutoTitleSkip: reached g_game_mode=%u, handing input "
                    "back to user (took %u input-buffer ticks)",
                    game_mode, now - s_started_frame);
            } else if (game_mode == 1000) {
                if (!s_cursor_set) {
                    // Menu cursor itself is now written in the hoisted
                    // block at the top of this function (runs on host AND
                    // spectator). Here we just record the start-frame for
                    // the auto-mash duration log and flip s_cursor_set.
                    s_cursor_set = true;
                    s_started_frame = *(uint32_t*)0x447EE0;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "AutoTitleSkip: starting button-A mash from input_buf=%u",
                        s_started_frame);
                }
                // 4-tick pattern (0x010, 0x010, 0, 0) ensures a rising
                // edge every 4 frames: prev=0 → cur=0x010 fires the
                // edge detector. Holding the button for 2 frames lets
                // the title menu's "any-button" check (& 0x3F0) sample
                // a stable value before the release. ~25 Hz of
                // confirms -- fast enough to march title → menu → CSS
                // in ~16 frames, slow enough not to skip past states
                // the menu hasn't latched yet.
                uint32_t buf_idx = *(uint32_t*)0x447EE0;
                return capture_and_return(((buf_idx >> 1) & 1) ? 0 : 0x010);
            }
        }
    }


    // FM2K_PARITY_AUTOPLAY: drive title→CSS→battle via a deterministic
    // input sequence. Short-circuits the netplay/CSS/spectator branches
    // -- autoplay owns input completely. Phase-aware (uses live
    // game_mode to pick inputs per CSS section). The game's CSS state
    // machine (game_state_manager @ 0x406FC0) reads:
    //   g_processed_input[i] for direction (bits 0..3 = L/R/U/D)
    //   g_input_changes[i] & 0x3F0 for attack-button rising-edge (confirm)
    // The original get_player_input returns RAW input; the game's
    // process_game_inputs converts raw → processed_input and computes
    // changes from prev frame. So returning a clean rising-edge of
    // bit 0x10 (button A) on a CSS frame triggers the confirm path.
    {
        static const char* env_autoplay = std::getenv("FM2K_PARITY_AUTOPLAY");
        if (env_autoplay && std::strcmp(env_autoplay, "1") == 0) {
            static uint32_t s_call_count = 0;
            const uint32_t call = s_call_count++;
            const uint32_t frame = call / 2u;
            const uint16_t Z = 0x010u;
            uint16_t out = 0u;

            // WW game_mode states (per vs_round_function @ 0x4086A0 +
            // game_state_manager @ 0x406FC0):
            //   0     = boot/title intro
            //   2000  = CSS active
            //   3000  = battle active
            // No 4000+ values; battle stays at 3000 throughout match.
            // For clean idle-parity capture, send Z every 30 frames
            // until we reach battle (advances title prompts + confirms
            // both CSS slots) and then idle (out=0) so the captured
            // frames show pure-idle physics with no synthetic attack
            // inputs polluting the comparison against kgt's idle run.
            if (game_mode < 3000u) {
                // FM2K_AUTOPLAY_CSS_DWELL=<seconds>: browse the CSS like a
                // human before locking in -- wander the cursor (d-pad
                // only, no confirm bits) for the dwell window, then
                // confirm with a deterministic per-player button so color
                // variety still gets exercised. Default 0 = legacy
                // instant-confirm. Real players move around at CSS for
                // 5-30s; the instant mash never exercised the spectator
                // seam hold (or the host's own CSS phase) at realistic
                // durations. Body lives in Hook_ComputeAutoplayCssInput so
                // the netplay path (Netplay_ProcessCSS) can feed the SAME
                // values into the CSS GekkoSession for the local player.
                if (game_mode == 2000u) {
                    out = Hook_ComputeAutoplayCssInput(player_id);
                } else if ((frame % 30u) == 0u) {
                    out = Z;
                }
            }
            // mode >= 3000: idle (out stays 0)
            //
            // FM2K_PARITY_AUTOPLAY_BATTLE=1 keeps injecting inputs during
            // battle. Pattern: every 12 frames pulse button A + a random-
            // looking direction so character_state_machine fires attack
            // edges and projectile spawns. Used by autonomous Phase F
            // stress runs to exercise RNG-consuming actions that idle
            // stress doesn't cover (~hours of idle stress passes clean,
            // but user-reported desyncs cluster on active gameplay).
            else if (game_mode >= 3000u && game_mode < 4000u) {
                static int s_battle_play_cached = -1;
                if (s_battle_play_cached < 0) {
                    const char* v = std::getenv("FM2K_PARITY_AUTOPLAY_BATTLE");
                    s_battle_play_cached = (v && v[0] && v[0] != '0') ? 1 : 0;
                }
                // Stress + autoplay coexistence: when a battle netplay
                // session is active, SKIP this autoplay path entirely and
                // let the netplay branch downstream pick up g_p?_input
                // (which netplay.cpp populated with the same autoplay
                // values via gekko). Without this skip, the engine and the
                // .fm2krep see different input streams: engine sims with
                // autoplay-computed values, .fm2krep records the
                // gekko-delivered values. The replay re-runs with the
                // .fm2krep values and produces different state.
                if (s_battle_play_cached == 1 && Netplay_IsActive()) {
                    // out stays 0 here; the netplay-active gate at the
                    // bottom of this block will skip the autoplay return
                    // so the netplay branch can supply g_p?_input.
                } else if (s_battle_play_cached == 1) {
                    // Pseudo-random battle inputs DETERMINISTIC under
                    // rollback. Derived from g_input_buffer_index (the
                    // engine's authoritative per-tick counter at
                    // 0x447EE0, part of saved InputTracking region) +
                    // player_id, hashed through a splitmix32-like
                    // function. Same frame → same input, every time,
                    // forward and replay both. Different per-frame
                    // entropy than a fixed pattern, so the test
                    // exercises hit-stop, super-cancel, projectile
                    // spawn, throw whiff, etc. -- the RNG-consuming
                    // code paths a fixed cycle wouldn't reach.
                    //
                    // ABSOLUTELY do not pull from a static counter
                    // (s_call_count etc.) -- that counter advances
                    // EVERY Hook_GetPlayerInput call including replay
                    // re-invocations, so forward sim_N and replay
                    // sim_N would see different "random" outputs and
                    // the test would diverge from itself. Last
                    // iteration of this code did exactly that and
                    // produced a frame-4 false-positive desync.
                    uint32_t seed = *(uint32_t*)0x447EE0;
                    seed ^= (uint32_t)player_id * 0x9E3779B9u;
                    // splitmix32
                    seed = (seed ^ (seed >> 16)) * 0x7feb352du;
                    seed = (seed ^ (seed >> 15)) * 0x846ca68bu;
                    seed = seed ^ (seed >> 16);
                    // Holdable buttons: A (0x10), B (0x20), C (0x40),
                    // D (0x80). Hold for ~10-15 frames so commands
                    // register, then release. Directions: L (0x01),
                    // R (0x02), U (0x04), D (0x08).
                    //
                    // Phase split into 3 bits: which input "mode" the
                    // PRNG picks this frame.
                    const uint32_t phase = (seed >> 28) & 0x7u;
                    const uint32_t dirbits = (seed >> 8) & 0xFu;
                    const uint32_t btnbits = (seed >> 4) & 0xFu;
                    switch (phase) {
                        case 0:  // idle (give engine time to settle)
                        case 1:
                            out = 0; break;
                        case 2:  // pure directional (movement)
                            out = (uint16_t)(dirbits & 0xFu); break;
                        case 3:  // single button tap
                            out = (uint16_t)((1u << (4 + (btnbits & 3u)))); break;
                        case 4:  // direction + button (special move setup)
                            out = (uint16_t)((dirbits & 0xFu) | (1u << (4 + (btnbits & 3u)))); break;
                        case 5:  // multi-button (super / parry attempt)
                            out = (uint16_t)((1u << (4 + (btnbits & 3u))) | (1u << (4 + ((btnbits >> 2) & 3u))));
                            break;
                        case 6:  // long-hold of one direction (walk)
                            out = (uint16_t)(1u << (dirbits & 3u)); break;
                        case 7:  // jump-cancel-style (UP + button)
                            out = (uint16_t)(0x4u | (1u << (4 + (btnbits & 3u))));
                            break;
                    }
                    // SOCD-cleaner expects no L+R or U+D simultaneously;
                    // strip the conflicts at the source so the engine
                    // doesn't have to reject them (same effect as
                    // Hook_ApplySOCD mode 1, applied earlier).
                    if ((out & 0x3u) == 0x3u) out &= ~0x3u;
                    if ((out & 0xCu) == 0xCu) out &= ~0xCu;
                }
            }

            /* Diagnostic: log on first hit + every game_mode change AND
             * every 120 frames once we hit battle (game_mode >= 3000) so
             * we can see HP/active-flag populating after CSS exit.
             * HP source corrected: g_p1_hp=0x4DFC85, g_p2_hp=0x4EDCC4
             * (verified via IDA xref of vs_round_function @ 0x4086A0). */
            static uint32_t s_last_logged_mode = 0xFFFFFFFFu;
            static uint32_t s_last_periodic = 0u;
            const bool mode_changed = (game_mode != s_last_logged_mode);
            const bool periodic = (game_mode >= 3000u) &&
                                  ((frame - s_last_periodic) >= 120u);
            if (mode_changed || periodic) {
                const uint32_t p1_action = *(uint32_t*)0x47019Cu;
                const uint32_t p2_action = *(uint32_t*)0x4701A0u;
                // Selected character indexes -- IDA-renamed to
                // g_p1_selected_char_idx / g_p2_selected_char_idx
                // (was misleadingly g_player_stage_positions[]).
                const int32_t  p1_char   = *(int32_t*)FM2K::ADDR_P1_SELECTED_CHAR;
                const int32_t  p2_char   = *(int32_t*)FM2K::ADDR_P2_SELECTED_CHAR;
                const uint32_t timer     = *(uint32_t*)0x424F00u;
                const uint8_t  char0_act = *(uint8_t*) 0x4DFCD1u;
                const uint8_t  char1_act = *(uint8_t*)(0x4DFCD1u + 57407u);
                const uint32_t p1_hp     = *(uint32_t*)0x4DFC85u;
                const uint32_t p1_max_hp = *(uint32_t*)0x4DFC91u;
                const uint32_t p2_hp     = *(uint32_t*)0x4EDCC4u;
                const uint32_t p2_max_hp = *(uint32_t*)0x4EDCD0u;
                const int32_t  cam_x     = *(int32_t*) 0x447F2Cu;
                const int32_t  cam_y     = *(int32_t*) 0x447F30u;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[AUTOPLAY] frame=%u mode=%u p1act=%u p2act=%u "
                    "p1char=%d p2char=%d ctimer=%u "
                    "char0_act=%u char1_act=%u "
                    "p1_hp=%u/%u p2_hp=%u/%u cam=(%d,%d) out=0x%03X",
                    frame, game_mode, p1_action, p2_action,
                    p1_char, p2_char, timer,
                    char0_act, char1_act,
                    p1_hp, p1_max_hp, p2_hp, p2_max_hp,
                    cam_x, cam_y, out);
                s_last_logged_mode = game_mode;
                if (periodic) s_last_periodic = frame;
            }
            // Stress + autoplay + active battle netplay: don't short-circuit
            // here. Fall through to the netplay branch below so the engine
            // consumes g_p?_input (the gekko-delivered value, which is the
            // SAME autoplay value via the netplay.cpp gekko_add_local_input
            // path). End result: engine input == spec-stream input == .fm2krep
            // input -- replay reproduces record deterministically.
            //
            // Same rule for CSS netplay (2026-06-11 split-brain fix): the
            // engine must consume Netplay_GetCSSInput (the lockstep-
            // delivered pair) -- Netplay_ProcessCSS feeds our local
            // autoplay value into the session via
            // Hook_ComputeAutoplayCssInput. Short-circuiting here ran each
            // peer's sim on locally-hashed inputs for BOTH players; local
            // counter skew under packet loss made P1's sim lock chars at
            // css_frame=733 while P2's sim browsed to 3796 -- different
            // chars, different colors, doomed rematch.
            if (game_mode >= 3000u && game_mode < 4000u && Netplay_IsActive()) {
                // Skip return; let the netplay branch handle it.
            } else if (IsCSSMode(game_mode) && Netplay_IsConnected()) {
                // Skip return; let the CSS netplay branch handle it.
            } else {
                return capture_and_return((int)out);
            }
        }
    }

    // (Spectator branch lifted to the top of the function -- runs before
    // auto-mash so spectator's local FM2K replays host's recorded inputs
    // instead of generating its own auto-mash sequence.)

    // Battle mode with GekkoNet active - return synced input with facing fix
    if (Netplay_IsActive()) {
        // Swap-window input guard (CSS-2 confirm leak, 2026-06-11 15:50):
        // the battle session outlives the local game's exit from battle
        // mode by a few hundred ms (battle-end barrier + EndBattle drain).
        // Hook calls in that window served the LAST gekko-delivered battle
        // inputs to the freshly-initialized rematch CSS -- held attack
        // bits read as confirm edges, asymmetrically per peer (each froze
        // on a different final input pair), locking ghost chars before
        // lockstep even started (P1 opened CSS-2 at act=1/1 timer=53, P2
        // at act=0/1: P1 auto-flipped to battle at css_frame=50, P2
        // waited forever). game_state_manager's CSS init already
        // canonicalizes act/timer/sel, so neutral here keeps the new CSS
        // pristine; battle re-sims read game_mode 3000 from restored
        // state and are unaffected.
        if (!IsBattleMode(game_mode) && Netplay_IsConnected()) {
            return capture_and_return(0);
        }
        uint16_t input = Netplay_GetInput(player_id);

        // Apply facing direction swap (same logic as original get_player_input).
        // During battle (3000-3999), if character is active and not in special
        // state, left/right are swapped based on facing direction.
        //
        // CRITICAL: these are OFFSETS inside the character slot, NOT absolute
        // addresses. Hard-coding absolute addresses broke when we corrected
        // CHAR_SLOT_BASE from 0x4D1D80 to 0x4D1D90 -- the hook was reading
        // from 16 bytes into the wrong memory, decisions were garbage, and
        // the two peers could pick different facing-swap values from
        // non-deterministic residue. This is almost certainly the "HP
        // differs by 2 after a hit" signature we've been chasing.
        //
        // Offsets are relative to the CORRECTED base CHAR_SLOT_BASE=0x4D1D90.
        // First attempt computed these against the old 0x4D1D80 base, which was
        // 16 bytes too low for the new base -- that made facing-swap read the
        // wrong bytes and the symptom was "left/right flip when you switch
        // sides". Absolute addresses of the fields are unchanged:
        //   0x4DFCD1 - 0x4D1D90 = 0xDF41   (char_active)
        //   0x4D9A36 - 0x4D1D90 = 0x7CA6   (char_state_flags)
        constexpr size_t CHAR_ACTIVE_FLAG_OFFSET = 0xDF41;
        constexpr size_t CHAR_STATE_FLAGS_OFFSET = 0x7CA6;

        bool facing_reversed = true;  // Default: no swap (normal directions)
        if (game_mode >= 3000 && game_mode < 4000) {
            uintptr_t slot_base = CHAR_SLOT_BASE + (size_t)input_type * CHAR_SLOT_SIZE;
            uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET);
            if (char_active != 0) {
                uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET);
                if ((char_flags & 8) == 0) {
                    facing_reversed = false;  // Character active, facing applies
                }
            }
        }

        if (!facing_reversed) {
            // Swap left (bit 0 = 0x001) and right (bit 1 = 0x002)
            uint16_t left_bit = (input & 0x001);
            uint16_t right_bit = (input & 0x002);
            input = (input & ~0x003) | (left_bit << 1) | (right_bit >> 1);
        }
        input = Hook_ApplySOCD(input);

        // Log only the first 4 calls (initial handshake verification). After
        // that stay silent -- Hook_GetPlayerInput fires 2x per sim tick, and
        // during stress-mode rollback replay that's thousands of calls per
        // second. Per-100 throttling was still showing up on screen.
        static uint32_t battle_log_count = 0;
        if (battle_log_count < 4) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[BATTLE INPUT #%u] player=%d type=%d -> 0x%03X (facing=%s)",
                battle_log_count, player_id, input_type, input,
                facing_reversed ? "normal" : "swapped");
        }
        battle_log_count++;

        return capture_and_return((int)input);
    }

    // CSS mode with connection - return CSS input from control channel
    if (IsCSSMode(game_mode) && Netplay_IsConnected()) {
        uint16_t input = Netplay_GetCSSInput(player_id);
        input = Hook_ApplySOCD(input);
        return capture_and_return((int)input);
    }

    // Offline or menu: use the binder if active, else fall through to FM2K's
    // own get_player_input. Same gating as Input_CaptureLocal -- Init() is
    // idempotent and resolves to %APPDATA%\FM2K_Rollback\fm2k_inputs.ini
    // (matching the launcher's save path) so launcher-bound keys / pads
    // drive offline play here, GekkoNet-online play through Input_CaptureLocal.
    {
        static int  s_last_check_tick = 0;
        static bool s_binder_active   = false;
        static bool s_profile_routed  = false;
        const int now_tick = (int)GetTickCount();
        if ((now_tick - s_last_check_tick) > 1000 || s_last_check_tick == 0) {
            s_last_check_tick = now_tick;
            // Per-game profile routing -- v0.2.43 fix (Sheriel's bug
            // report) restored here after an intervening edit removed
            // it. Without this, Hook_GetPlayerInput's binder Init/Load
            // resolves to the DEFAULT fm2k_inputs.ini and the launcher's
            // "Use override for X" per-game profile is silently
            // ignored offline. Mirrors input.cpp's Input_CaptureLocal.
            if (!s_profile_routed) {
                s_profile_routed = true;
                char buf[MAX_PATH] = {};
                if (GetModuleFileNameA(nullptr, buf, sizeof(buf)) > 0) {
                    const char* slash = std::strrchr(buf, '\\');
                    if (!slash) slash = std::strrchr(buf, '/');
                    const char* base = slash ? slash + 1 : buf;
                    std::string stem = base;
                    auto dot = stem.find_last_of('.');
                    if (dot != std::string::npos) stem.resize(dot);
                    FM2KInputBinder::SetGameProfile(stem.c_str());
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Hook_GetPlayerInput: routed binder to per-game profile '%s'",
                        stem.c_str());
                }
            }
            FM2KInputBinder::Init();
            // Re-Load every tick so launcher-side Save() reaches the
            // running game without restart. Init() only Load()s once
            // (gated by g_initialized); we need the periodic re-read.
            FM2KInputBinder::Load();
            // Hot-plug refresh (#25, Suicidal Muffin's bug): pick up
            // freshly-attached pads + drop removed handles on the same
            // 1 s cadence as the binder reload, so users don't have to
            // restart the session after plugging in a controller.
            FM2KInputBinder::RefreshGamepads();
            const auto& pb = FM2KInputBinder::Bindings(0);
            s_binder_active = false;
            for (const auto& b : pb.bits) {
                if (b.source != FM2KInputBinder::Binding::Source::NONE) {
                    s_binder_active = true;
                    break;
                }
            }
        }
        // Per-player takeover: the binder only overrides players it has
        // bindings FOR (BinderSlotHasBindings). s_binder_active keys on
        // P1's rows alone, but this branch used to return for BOTH
        // players -- a profile whose P2 rows are all NONE (e.g. a per-game
        // override saved with P2 cleared) fed that player a permanent 0
        // and never reached the vanilla fallback below. Vanilla FM2K
        // would have read the game's own configured P2 keys; now an
        // unbound slot falls through to original_get_player_input for
        // exactly that behavior. ("P2 controls & pause dead in offline"
        // report.)
        if (s_binder_active && BinderSlotHasBindings(input_type & 1)) {
            // input_type is the character slot (same convention the battle /
            // spectator branches above use to compute slot_base for facing-fix).
            // 0 = P1 character → P1 bindings, 1 = P2 character → P2 bindings.
            // Without this distinction both players get the SAME input from
            // P1's bindings -- the bug we just fixed.
            //
            // Focus behavior: Sample_Win32 is focus-correct BY CONSTRUCTION
            // (keyboard via thread-queue-synced GetKeyboardState, gamepads
            // behind its own foreground gate) -- an unfocused game samples
            // all-zero here, no caller-side gating needed.
            int slot = (input_type & 1);
            uint16_t bound = FM2KInputBinder::Sample_Win32(slot);
            // OPTION title-screen submode cycle -- fires on the binder path
            // too, before masking off meta-bits so PerGamePatches sees the
            // full 14-bit value (OPTION = 0x800).
            if (player_id == 0) {
                PerGamePatches_OnTitleInputTick(bound, game_mode);
            }
            // Solo-driver CSS takeover (vs_cpu / cpu_vs_cpu / training):
            // when CSS is open AND we're being asked for P2's input AND
            // any solo-driver mode is engaged, the pipe is GATED via
            // PerGamePatches_GatedP2CssInput:
            //   - P1 not confirmed → P2 = 0
            //   - P1 confirmed but attack still held → P2 = 0
            //   - P1 confirmed AND released attack → P2 = P1's input
            // The override has to live INSIDE the binder branch because
            // the binder path returns before PerGamePatches_TryOverrideInput
            // is reached.
            if (game_mode == 2000u && player_id == 1) {
                const bool any_solo_driver =
                    PerGamePatches_IsVsCpuModeActive() ||
                    PerGamePatches_IsCpuVsCpuModeActive() ||
                    PerGamePatches_IsTrainingModeActive();
                if (any_solo_driver) {
                    const uint16_t p1_bound =
                        FM2KInputBinder::Sample_Win32(0)
                            & FM2KInputBinder::kEngineInputMask;
                    bound = PerGamePatches_GatedP2CssInput(p1_bound);
                    slot = 0;
                }
            }

            // Solo-driver BATTLE override. Same reason -- binder path
            // shortcircuits PerGamePatches_TryOverrideInput, so we have
            // to invoke the battle helper here too. Zeros P2 for VS CPU /
            // CPU vs CPU so the engine's script-driven AI takes over;
            // applies training-mode P2 behavior for training. P1 is
            // overridden too in CPU vs CPU.
            if (game_mode >= 3000u && game_mode < 4000u &&
                (PerGamePatches_IsVsCpuModeActive() ||
                 PerGamePatches_IsCpuVsCpuModeActive() ||
                 PerGamePatches_IsTrainingModeActive())) {
                // Gated on an active solo-driver mode so normal binder users
                // don't pay the extra Sample_Win32(0) every battle frame (#63
                // sibling to the no-binder DirectInput fix).
                const uint16_t p1_bound_battle =
                    FM2KInputBinder::Sample_Win32(0)
                        & FM2KInputBinder::kEngineInputMask;
                const int over = PerGamePatches_BattleInputOverride(
                    player_id, p1_bound_battle);
                if (over >= 0) {
                    // The solo-driver overrides exist to drive the CHARACTER
                    // (directions + attacks) so AI scripts can take over. They
                    // must NOT eat the human's system buttons. PAUSE (0x400) is
                    // carried through from this slot's OWN sampled input:
                    // BattleInputOverride returns 0 for BOTH players in
                    // CPU-vs-CPU, so a plain replacement silently kills the
                    // pause button for the person sitting there watching, with
                    // no other symptom to explain it.
                    //
                    // This preserves a bit the user actually pressed on their
                    // own binding -- it does not synthesize input from anywhere
                    // outside our config.
                    bound = (uint16_t)over | (uint16_t)(bound & 0x400u);
                }
            }
            // Apply the same battle facing-fix the original_get_player_input
            // path applies (so offline matches behave the same way as the
            // game's own input flow).
            constexpr size_t CHAR_ACTIVE_FLAG_OFFSET_OFL = 0xDF41;
            constexpr size_t CHAR_STATE_FLAGS_OFFSET_OFL = 0x7CA6;
            bool facing_reversed = true;
            if (game_mode >= 3000 && game_mode < 4000) {
                uintptr_t slot_base = CHAR_SLOT_BASE + (size_t)input_type * CHAR_SLOT_SIZE;
                uint8_t char_active = *(uint8_t*)(slot_base + CHAR_ACTIVE_FLAG_OFFSET_OFL);
                if (char_active != 0) {
                    uint8_t char_flags = *(uint8_t*)(slot_base + CHAR_STATE_FLAGS_OFFSET_OFL);
                    if ((char_flags & 8) == 0) facing_reversed = false;
                }
            }
            if (!facing_reversed) {
                uint16_t left_bit  = (bound & 0x001);
                uint16_t right_bit = (bound & 0x002);
                bound = (bound & ~0x003) | (left_bit << 1) | (right_bit >> 1);
            }
            bound = Hook_ApplySOCD(bound);
            // Strip meta-bits (OPTION/FN1/FN2) before passing to engine so
            // they don't leak into game state. Hook-side features that
            // consume those bits did so above (or read via Sample directly).
            bound &= FM2KInputBinder::kEngineInputMask;
            return capture_and_return((int)bound);
        }
    }

    // OPTION-button title-screen submode cycle (no-binder fallback path).
    // Gated on the OPTION-mode selector actually being active. When it's
    // off (the default) OnTitleInputTick is a no-op, but the
    // original_get_player_input(0,0) read below still cost a redundant
    // DirectInput poll EVERY frame for player 0 -- a chunk of the 0.2.46
    // fps regression (#63), since that poll's cost scales with the user's
    // HID/driver stack (machine-specific 95fps). Skip the read entirely
    // unless the selector is engaged.
    if (player_id == 0 && original_get_player_input &&
        PerGamePatches_IsOptionModeSelectorActive()) {
        const uint16_t raw = (uint16_t)(original_get_player_input(0, 0) & 0x7FF);
        PerGamePatches_OnTitleInputTick(raw, game_mode);
    }

    // Per-game mode overrides -- VS CPU, CPU vs CPU, training. Returns -1
    // when no toggle applies. Only fires on the offline path (netplay /
    // spectator branches return earlier above), so we don't accidentally
    // override authoritative input streams during a hub match. SOCD is
    // applied uniformly on the override path too.
    {
        int o = PerGamePatches_TryOverrideInput(player_id, game_mode);
        if (o >= 0) {
            o = (int)Hook_ApplySOCD((uint16_t)o);
            return capture_and_return(o);
        }
    }

    // No binder config -- vanilla FM2K input path.
    int orig = original_get_player_input
        ? original_get_player_input(player_id, input_type)
        : 0;

    orig = (int)Hook_ApplySOCD((uint16_t)orig);
    return capture_and_return(orig);
}

bool InstallInputHooks() {
    // Hook GetPlayerInput -- FM2K only.
    //
    // FM2K's get_player_input is `int __cdecl(int player_id, int input_type)`,
    // a single function called for both players with input_type selecting
    // which control mode to use. Our Hook_GetPlayerInput matches that
    // signature.
    //
    // FM95 splits this into two SEPARATE single-arg functions --
    // get_player_input_p1 (0x408AE0) and get_player_input_p2 (0x408D60),
    // each `int __cdecl(int player_idx)`. Hooking either with our 2-arg
    // shape would corrupt the stack on entry and inject the wrong
    // FM2K-style keybindings into CPW's native input read.
    //
    // The right surface for FM95 input injection is Hook_ProcessGameInputs
    // (0x408FF0, single arg, hooked below) -- it writes directly into
    // g_p1/p2_input_history[buf_idx] AFTER the natural keyboard/joystick
    // poll, so we can override for netplay without disrupting the host's
    // ini-driven key bindings or joyGetPosEx gamepad path.
    if constexpr (FM2K::kIsFM2K) {
        if (MH_CreateHook((void*)FM2K::ADDR_GET_PLAYER_INPUT, (void*)Hook_GetPlayerInput,
                          (void**)&original_get_player_input) != MH_OK ||
            MH_QueueEnableHook((void*)FM2K::ADDR_GET_PLAYER_INPUT) != MH_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook GetPlayerInput");
            return false;
        }
    } else {
        // FM95: hook BOTH split single-arg functions so our binder layer
        // applies to both players. Same effect as FM2K's single hook --
        // user-rebindable keyboard + XInput gamepad, facing flip applied
        // engine-relative, START stripped on CSS.
        if (MH_CreateHook((void*)FM2K::ADDR_GET_PLAYER_INPUT,    // 0x408AE0 P1
                          (void*)Hook_GetPlayerInput_FM95_P1,
                          (void**)&original_get_player_input_p1) != MH_OK ||
            MH_QueueEnableHook((void*)FM2K::ADDR_GET_PLAYER_INPUT) != MH_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook get_player_input_p1");
            return false;
        }
        if (MH_CreateHook((void*)FM2K::ADDR_GET_PLAYER_INPUT_P2, // 0x408D60 P2
                          (void*)Hook_GetPlayerInput_FM95_P2,
                          (void**)&original_get_player_input_p2) != MH_OK ||
            MH_QueueEnableHook((void*)FM2K::ADDR_GET_PLAYER_INPUT_P2) != MH_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook get_player_input_p2");
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: Installed FM95 split input hooks (p1=0x%08X, p2=0x%08X) "
                    "→ FM2KInputBinder + facing flip via g_p_facing_snap",
                    (unsigned)FM2K::ADDR_GET_PLAYER_INPUT,
                    (unsigned)FM2K::ADDR_GET_PLAYER_INPUT_P2);
    }
    return true;
}

