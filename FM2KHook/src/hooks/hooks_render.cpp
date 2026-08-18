// hooks_render.cpp -- render path: RenderGame (RNG/state isolation) + sound dispatch
// + blit/sprite SIMD reimpl + FPS/title diagnostics + game-window find. Split from hooks.cpp.
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

// Find our game window
static HWND g_cached_window = NULL;

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        char class_name[64];
        if (GetClassNameA(hwnd, class_name, sizeof(class_name))) {
            // FM2K registers "KGT2KGAME"; FM95/CPW registers "KGT95GAME"
            // (same convention as wndproc_subclass.cpp / input.cpp).
            const char* expect_cls = FM2K::kIsFM95 ? "KGT95GAME" : "KGT2KGAME";
            if (strcmp(class_name, expect_cls) == 0) {
                *(HWND*)lParam = hwnd;
                return FALSE;
            }
        }
    }
    return TRUE;
}

static HWND GetOurGameWindow() {
    if (g_cached_window && IsWindow(g_cached_window)) {
        return g_cached_window;
    }
    g_cached_window = NULL;
    EnumWindows(EnumWindowsProc, (LPARAM)&g_cached_window);
    return g_cached_window;
}

// FPS tracking. `g_current_fps` is read by fc_hud (via extern decl
// in imgui_overlay.cpp) so the always-on HUD shows live framerate;
// it's still file-scope-visible for everything inside this TU.
static DWORD g_last_fps_time = 0;
static int g_fps_frame_count = 0;
int g_current_fps = 0;

// Sim-fps: sampled from g_sim_step_count (incremented once per update_game
// tick across all paths). Render-fps counts renders; sim-fps counts logic
// ticks. On a heavy stage render drops but sim must hold 100 -- showing both
// makes that explicit instead of one ambiguous number. g_current_sim_fps is
// read by the title bar + overlay HUD.
static uint32_t s_last_sim_step_sample = 0;
int g_current_sim_fps = 0;

// Hook: RenderGame
// Set in the GekkoNet AdvanceEvent handler (netplay.cpp). Each advance
// produces exactly one new simulation tick; this flag says "that tick is
// unrendered". Cleared inside Hook_RenderGame after original_render_game()
// has drawn it. Any extra Hook_RenderGame invocations between advances skip
// the real render entirely, so render count cannot outpace sim count on
// either peer -- both peers render exactly as many frames as GekkoNet has
// advanced. Without this gate, render mutates object-pool animation counters
// on wall-clock cadence, producing asymmetric state that feeds back into
// the next sim tick's RNG draws.
bool g_frame_pending_render = false;

// Public -- called by the trampoline's render step so FPS + title bar stats
// keep updating even though Hook_RenderGame is bypassed in battle mode.
extern "C" void Hook_RenderDiagnostics_Tick();
extern "C" void Hook_RenderDiagnostics_Tick() {
    CheckOverlayHotkey();

    // Track FPS -- render (this tick fires once per render) + sim (delta of
    // the global logic-tick counter over the same 1s window).
    g_fps_frame_count++;
    DWORD now = GetTickCount();
    if (now - g_last_fps_time >= 1000) {
        g_current_fps = g_fps_frame_count;
        g_fps_frame_count = 0;
        const uint32_t sim_now = g_sim_step_count;
        g_current_sim_fps = (int)(sim_now - s_last_sim_step_sample);
        s_last_sim_step_sample = sim_now;
        g_last_fps_time = now;
    }

    // Update window title with BBBR-style stats (throttled to 500ms).
    // Format follows the layout user-spec'd 2026-05-05:
    //   <game> | P1 (BATTLE) | 100fps RTT 12ms | D2 FA0.5 RB5 | vs Nick 12-3-1
    // Game-name prefix is the executable's stem (e.g. "WonderfulWorld_
    // ver_0946") instead of a literal "FM2K", so multi-game lobbies
    // are visually distinct in the taskbar. P1/P2 replaces [HOST]/
    // [CLIENT] for casual readability. W/L/D suffix comes from the
    // launcher via shared mem (FM2KSharedMemData::ui_*); -1 sentinels
    // mean "not yet known" → suffix is omitted.
    static DWORD last_title_update = 0;
    DWORD title_now = GetTickCount();
    if (title_now - last_title_update >= 500) {
        last_title_update = title_now;
        HWND game_window = GetOurGameWindow();
        if (game_window) {
            // Cache the game-name prefix once. Use the .exe's stem so it
            // works for any FM2K game, not just WonderfulWorld. Strips
            // the directory, drops the ".exe" suffix.
            //
            // Stored as UTF-8 because the rest of the title-format code
            // (peer nicks from shm, the final MultiByteToWideChar at the
            // bottom) assumes UTF-8. Going through GetModuleFileNameW
            // first avoids the SJIS-vs-UTF-8 mojibake we'd hit if we
            // pulled bytes directly via GetModuleFileNameA -- those bytes
            // are SJIS (our locale spoof preserves SJIS via
            // WC_NO_BEST_FIT_CHARS) and re-interpreting them as UTF-8
            // turns JP names into garbage in the title bar.
            static char s_game_prefix[256] = {};
            if (s_game_prefix[0] == '\0') {
                wchar_t wbuf[MAX_PATH];
                DWORD n = GetModuleFileNameW(nullptr, wbuf, MAX_PATH);
                if (n > 0 && n < MAX_PATH) {
                    wchar_t* wslash = wcsrchr(wbuf, L'\\');
                    if (!wslash) wslash = wcsrchr(wbuf, L'/');
                    wchar_t* wstem = wslash ? wslash + 1 : wbuf;
                    wchar_t* wdot = wcsrchr(wstem, L'.');
                    if (wdot) *wdot = L'\0';
                    WideCharToMultiByte(CP_UTF8, 0, wstem, -1,
                                        s_game_prefix, sizeof(s_game_prefix),
                                        nullptr, nullptr);
                }
                if (s_game_prefix[0] == '\0') {
                    snprintf(s_game_prefix, sizeof(s_game_prefix), "FM2K");
                }
            }

            // Format spec (user-visible 2026-05-05):
            //   <game> | P1 vs <peer> 0-0-0 (BATTLE) | 100fps RTT 12ms | D2 FA0.5 RB5
            //   <game> | P2 vs <peer> 0-0-0 (CSS)    | 100fps RTT 12ms
            //   <game> | P1 (TITLE)   | 100fps RTT 12ms | 47-30-2
            //   <game> | Offline | 100fps
            // The peer + W/L/D moves up to live next to the role tag; the
            // (STATE) trails. Falls back to overall W-L-D when no active
            // peer (idle in title / between sessions).
            char title[384];
            const char* role =
                g_spectator_mode      ? "Spec" :
                (g_player_index == 0) ? "P1"   : "P2";
            bool active = Netplay_IsActive();
            bool connected = Netplay_IsConnected();

            // Build the role+vs+wld lead chunk that sits between the game
            // prefix and the STATE / fps section. Three forms:
            //   (a) "P1 vs Armonté 12-3-1" -- active match, peer + record known
            //   (b) "P1 (overall 47-30-2)" -- no active peer, only personal record
            //   (c) "P1"                   -- no record yet (pre-first-query)
            char lead[160] = {};
            FM2KSharedMemData* shm = GetSharedMemory();
            const bool have_peer = shm && shm->ui_peer_nick[0] != '\0' &&
                                   shm->ui_vs_wins >= 0;
            const bool have_overall = shm && shm->ui_wins >= 0;
            if (have_peer) {
                snprintf(lead, sizeof(lead),
                    "%s vs %s %d-%d-%d",
                    role, shm->ui_peer_nick,
                    shm->ui_vs_wins, shm->ui_vs_losses, shm->ui_vs_draws);
            } else if (have_overall) {
                snprintf(lead, sizeof(lead),
                    "%s (%d-%d-%d)",
                    role, shm->ui_wins, shm->ui_losses, shm->ui_draws);
            } else {
                snprintf(lead, sizeof(lead), "%s", role);
            }

            if (g_spectator_mode) {
                size_t qd = SpectatorNode_PendingFrameCount();
                // Real playback rate (popped sim frames per second), NOT
                // the trampoline loop rate: during a q:0 hold the loop
                // keeps spinning at 100/s re-rendering the same frame,
                // so g_current_fps read "100fps" while playback was
                // visibly frozen (user report 2026-06-11 18:1x). pops/s
                // is the truth the viewer cares about.
                extern uint32_t g_spec_pop_total;
                static uint32_t s_pop_prev    = 0;
                static DWORD    s_pop_prev_ms = 0;
                uint32_t play_fps = 0;
                {
                    const DWORD nowp = GetTickCount();
                    if (s_pop_prev_ms != 0 && nowp > s_pop_prev_ms) {
                        play_fps = (g_spec_pop_total - s_pop_prev) * 1000u /
                                   (nowp - s_pop_prev_ms);
                    }
                    s_pop_prev    = g_spec_pop_total;
                    s_pop_prev_ms = nowp;
                }
                // Differentiate offline-replay (file-driven, no peer) from
                // live-spec (subscribed to a host's stream). Replay mode
                // never has an upstream so "Connecting..." would be a lie
                // -- it's already loaded everything from disk.
                static int s_replay_cached = -1;
                if (s_replay_cached < 0) {
                    const char* rp = std::getenv("FM2K_REPLAY_FILE");
                    s_replay_cached = (rp && rp[0]) ? 1 : 0;
                }
                if (s_replay_cached == 1) {
                    uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
                    const char* phase =
                        (mode == 0)                          ? "BOOT" :
                        (mode == 1000)                       ? "TITLE" :
                        (mode == 2000)                       ? "CSS" :
                        (mode >= 3000 && mode < 4000)        ? "BATTLE" :
                                                               "POST";
                    snprintf(title, sizeof(title),
                        "%s | Replay (%s) | %ufps | q:%zu",
                        s_game_prefix, phase, play_fps, qd);
                } else {
                    // Three-state: a TCP heal while the subscription rides
                    // on UDP is "Resyncing..." -- NOT a cold "Connecting..."
                    // (the old binary label flapped to Connecting during
                    // transport churn even though playback never stopped,
                    // which read as a full drop to the user).
                    const bool sub = SpectatorNode_IsSubscribedUpstream();
                    // A hold or a catch-up drain names ITSELF (snapshot %,
                    // frames behind) instead of showing a healthy-looking
                    // "Subscribed" over a frozen picture -- see
                    // SpectatorNode_ResyncStatus, which returns false in every
                    // other state so the labels below are unchanged.
                    char resync[64] = {};
                    const bool resyncing =
                        sub && SpectatorNode_ResyncStatus(resync, sizeof(resync));
                    const char* status =
                        !sub        ? "Connecting..."
                        : resyncing ? resync
                                    : "Subscribed";
                    snprintf(title, sizeof(title),
                        "%s | %s %s | %ufps | q:%zu",
                        s_game_prefix, role, status,
                        play_fps, qd);
                }
            } else if (active) {
                GekkoNetworkStats stats = Netplay_GetNetworkStats();
                float ahead = Netplay_GetFramesAhead();
                int delay = Netplay_GetLocalDelay();
                uint32_t desyncs = Netplay_GetDesyncCount();
                uint32_t rollbacks = Netplay_GetRollbackCount();
                const char* tag = g_stress_mode ? "STRESS" : "BATTLE";
                if (desyncs > 0) {
                    snprintf(title, sizeof(title),
                        "%s | %s (%s) | %dsim/%drdr RTT %ums | D%d FA%.1f RB%u | DESYNC x%u",
                        s_game_prefix, lead, tag, g_current_sim_fps, g_current_fps,
                        stats.last_ping, delay, ahead, rollbacks, desyncs);
                } else {
                    snprintf(title, sizeof(title),
                        "%s | %s (%s) | %dsim/%drdr RTT %ums | D%d FA%.1f RB%u",
                        s_game_prefix, lead, tag, g_current_sim_fps, g_current_fps,
                        stats.last_ping, delay, ahead, rollbacks);
                }
            } else if (connected) {
                uint32_t ping = Netplay_GetPingMs();
                uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
                const char* phase = nullptr;
                char phase_buf[16] = {};
                if      (mode == 0)               phase = "BOOT";
                else if (mode == 1000)            phase = "TITLE";
                else if (mode == 2000)            phase = "CSS";
                else if (mode >= 3000 && mode < 4000) phase = "BATTLE";
                else {
                    std::snprintf(phase_buf, sizeof(phase_buf), "MODE %u",
                                  (unsigned)mode);
                    phase = phase_buf;
                }
                snprintf(title, sizeof(title),
                    "%s | %s (%s) | %dsim/%drdr RTT %ums",
                    s_game_prefix, lead, phase, g_current_sim_fps, g_current_fps, ping);
            } else if (!g_offline_mode) {
                snprintf(title, sizeof(title),
                    "%s | %s | Connecting... | %dsim/%drdr",
                    s_game_prefix, lead, g_current_sim_fps, g_current_fps);
            } else {
                snprintf(title, sizeof(title),
                    "%s | Offline | %dsim/%drdr", s_game_prefix,
                    g_current_sim_fps, g_current_fps);
            }

            // SetWindowTextW + UTF-8 → UTF-16 conversion: SetWindowTextA
            // interprets the byte stream as the system's ANSI codepage,
            // turning UTF-8 "é" (C3 A9) into garbled "Ã©" on a CP1252
            // install. Using the wide path keeps "Armonté" / "ＣＰＷ" /
            // "テスト" intact regardless of system locale.
            wchar_t wtitle[384];
            int wn = MultiByteToWideChar(CP_UTF8, 0, title, -1,
                                         wtitle,
                                         (int)(sizeof(wtitle) / sizeof(wtitle[0])));
            if (wn > 0) {
                // DefWindowProcW(WM_SETTEXT) bypasses the WNDPROC chain so
                // a third-party subclass (cnc-ddraw) that reverted the
                // window's IsUnicode flag can't apply a CP_ACP=1252 W→A
                // bridge here and destroy JP chars on the way to storage.
                DefWindowProcW(game_window, WM_SETTEXT, 0, (LPARAM)wtitle);
            } else {
                SetWindowTextA(game_window, title);  // shouldn't happen
            }
        }
    }
}

// Mike Z sound rollback: intercept the SFX branch of FM2K's script sound
// dispatcher. During battle, instead of playing immediately we record the
// requested play into `desired[channel]`. Once per displayed frame (after
// the advance batch completes) SoundRollback::SyncAfterAdvance reconciles
// desired ↔ actual and issues real DSound stops/plays with the rollback-
// window filter that prevents erased/re-triggered sounds from clipping.
//
// Script item layout (42 bytes, from DispatchScriptSoundCommand decomp):
//   +36  void*  SoundBufferArray ptr       (SFX case)
//   +40  uint8  cmd byte (low nibble: 0=stop 1=SFX 2=MIDI 3=CD; bit 0x10=volume flag)
//   +41  uint8  CD track number             (CD case)
//
// MIDI and CD paths (music) pass through unchanged -- music-restart on
// rollback is a v2 concern.
typedef int(__cdecl* DispatchScriptSoundFunc)(int);
static DispatchScriptSoundFunc original_dispatch_script_sound = nullptr;

int __cdecl Hook_DispatchScriptSoundCommand(int script_item) {
    // FM95 (Gap 4a, audio-under-rollback unported): everything below is
    // FM2K-tuned -- the script-item field offsets, the channel table, and
    // the SyncAfterAdvance reconcile that re-fires a recorded script_item
    // pointer into sim memory a rollback may have rewound. Under stress
    // rollback that re-fire AV'd inside CPW's own
    // StopAllSoundsInBufferArray@0x4016A0 (NULL buffer array read).
    // Full passthrough until the FM95 audio port lands: cost is possible
    // music cut-in/out across FM95 rollbacks, never a crash.
    if constexpr (FM2K::kIsFM95) {
        return original_dispatch_script_sound(script_item);
    }
    // Spectator catch-up mute (C5.5). While the spectator is burning
    // through queued events to reach live edge, we run sim only -- no
    // render, no audio. Without this, joining 30k events late would
    // blast 5 minutes of compressed audio in the few seconds it takes
    // to drain. SoundRollback's dedup table still updates on the host
    // pass (RecordDesired runs as part of the sim), so steady-state
    // dispatch stays correct once catch-up clears.
    if (g_spectator_catchup) {
        return 0;
    }

    // Mute gates -- applied UNCONDITIONALLY (offline + online + spectator).
    // Re-checks the audio.ini file ~once per second so the launcher's
    // toggle reaches the running game without a separate IPC channel.
    {
        static uint32_t s_last_check = 0;
        const uint32_t now_ms = GetTickCount();
        if (now_ms - s_last_check > 1000) {
            s_last_check = now_ms;
            SoundRollback::RefreshMuteFromDisk();
        }
    }
    if (script_item != 0) {
        const uint8_t cmd_byte = *reinterpret_cast<uint8_t*>(script_item + 40);
        const uint8_t cmd_low  = cmd_byte & 0xF;
        const bool    looping  = (cmd_byte & 0x10) != 0;

        // Audio classification -- verified against WonderfulWorld disasm
        // (0x403430 dispatcher) + LilithPort's BGM_VOLUME / SE_VOLUME
        // hook sites (stdafx.h:152-159, MainForm.cpp:2719-2733):
        //   cmd_low 0           → stop everything (never muted)
        //   cmd_low 1, loop=0   → SFX (one-shot WAV, vtable Play(..., 0))
        //   cmd_low 1, loop=1   → BGM as looping WAV (Play(..., 1))
        //   cmd_low 2           → BGM via MIDI (WriteTempMIDIAndPlay)
        //   cmd_low 3           → BGM via CD audio (InitializeCDAudio)
        // Most 2DFM games play BGM via case 1 + loop; case 2/3 are rare.
        const bool is_bgm = (cmd_low == 2) ||
                            (cmd_low == 3) ||
                            (cmd_low == 1 && looping);
        const bool is_sfx = (cmd_low == 1 && !looping);
        // Diagnostic: log EVERY BGM/stop dispatch (before any routing).
        if (is_bgm || cmd_low == 0)
            SoundRollback::TraceBgmDispatch(cmd_byte, Netplay_IsActive(),
                IsBattleMode(*reinterpret_cast<uint32_t*>(FM2K::ADDR_GAME_MODE)));
        if (is_bgm && SoundRollback::IsMusicMuted()) return 0;
        if (is_sfx && SoundRollback::IsSfxMuted())   return 0;
    }

    if (!Netplay_IsActive() || script_item == 0) {
        return original_dispatch_script_sound(script_item);
    }

    // Defer sound to the rollback sync layer ONLY while the game is actually in
    // the battle phase (game_mode 3000-3999), where SyncAfterAdvance runs. We key
    // on the LIVE game phase, not a battle-session latch that lingers past the
    // transition. That matters on the battle->CSS transition frame: the game
    // stops the battle BGM (cmd 0) and its freshly re-created CSS controller
    // re-dispatches the CSS BGM at game_mode==2000 -- keying on the live phase lets
    // that CSS BGM play IMMEDIATELY instead of being deferred into a battle sync
    // that's ending (which silenced CSS-return music).
    const bool in_battle_phase =
        IsBattleMode(*reinterpret_cast<uint32_t*>(FM2K::ADDR_GAME_MODE));

    uint8_t cmd = *reinterpret_cast<uint8_t*>(script_item + 40);
    const uint8_t cmd_low = cmd & 0xF;

    if (cmd_low == 0) {
        // Full stop-all (ControlSoundSystem(0)). This MUST run in order at its
        // sim frame -- the game issues it to clear pre-round audio right before
        // the battle BGM starts. Deferring it to SyncAfterAdvance ran it AFTER
        // the SFX layer replayed the battle BGM, silencing the whole battle
        // (observed: cmd_low=0 @ bf182 every match). Fire immediately on the
        // forward pass; skip during rollback replay so it isn't re-applied.
        // Re-firing on the save-ring forward re-scroll is harmless -- a repeated
        // stop is just silence (no audible cut, unlike a repeated play).
        if (!g_is_rolling_back)
            return original_dispatch_script_sound(script_item);
        return 0;
    }

    if (cmd_low != 1) {
        // BGM play -- MIDI (case 2) or CD audio (case 3). DEFER to the rollback
        // layer only while the battle sync tick is live: there the desired-vs-
        // actual identity check stops the save-ring scroll from restarting MCI
        // (the old cut-in/out), and bgm_desired rides the save ring so a
        // rollback across a music start restores it. OUTSIDE battle there is no
        // sync tick, so play immediately or it is recorded and never heard.
        if (in_battle_phase) {
            //   +36 = buffer_array ptr (unused by MIDI/CD but valid to read)
            //   +41 = CD track number (case 3 only)
            uint32_t payload = *reinterpret_cast<uint32_t*>(script_item + 36)
                             ^ *reinterpret_cast<uint8_t*> (script_item + 41);
            SoundRollback::RecordDesiredBgm(script_item, cmd_low, payload,
                                            Netplay_GetFrame());
            return 1;  // deferred to SyncAfterAdvance
        }
        return original_dispatch_script_sound(script_item);  // immediate (non-battle)
    }

    // SFX (cmd 1, incl. looping-WAV BGM). The loop bit (0x10) marks looping-WAV
    // music (CSS/battle BGM) vs a one-shot SFX -- trace it to see the CSS-return
    // dispatch behaviour.
    if ((cmd & 0x10) != 0)
        SoundRollback::TraceWavMusicDispatch(Netplay_GetFrame(), in_battle_phase);
    // Same gate as BGM: only defer to the Mike-Z desired/actual layer while the
    // battle sync tick is live. Outside battle (CSS cursor SFX, CSS looping-WAV
    // music -- incl. the CSS-BGM re-dispatch on the battle->CSS return) play
    // immediately -- else the deferred dispatch is recorded and never heard.
    if (!in_battle_phase) {
        return original_dispatch_script_sound(script_item);
    }

    void* arr = *reinterpret_cast<void**>(script_item + 36);
    if (!arr || !SoundRollback::RecordDesired(arr, script_item, Netplay_GetFrame())) {
        // Unknown / null channel -- not in g_sound_channel_table. Fall through
        // to the original dispatcher so the sound still plays (without
        // rollback tracking). The vast majority of FM2K SFX buffer_arrays
        // appear to be allocated outside the system table; we only Mike-Z the
        // ones we can identify.
        return original_dispatch_script_sound(script_item);
    }
    // Known channel -- desired[] updated; defer the real play to
    // SoundRollback::SyncAfterAdvance at end of the displayed frame.
    return 1;
}

// Render reimplementation config (FM2K_BLIT_SIMD) + profiler gate, set at
// hook install. g_blit_cfg.mode==Off means profiler-only (FM2K_RENDER_PROFILE).
static fm2k::render_simd::Config g_blit_cfg;

// Verify mode: render the original blit, snapshot the dirty box, restore the
// pre-blit pixels, render OUR reimplementation, and compare. Leaves OUR output
// in the framebuffer (so the displayed frame is the reimpl, eyeballable) and
// logs the first mismatches. Box-scoped so the per-blit cost stays bounded.
static void BlitVerify(int sprite_desc, int src_pixels, int palette_lut,
                       int dst_x, int dst_y, int width, int height, short flags) {
    static uint16_t s_pre[640 * 480];
    static uint16_t s_org[640 * 480];
    static int s_logged = 0;
    uint8_t* fb = *(uint8_t**)0x4246CC;
    int bx0 = dst_x < 0 ? 0 : dst_x, by0 = dst_y < 0 ? 0 : dst_y;
    int bx1 = dst_x + width, by1 = dst_y + height;
    if (bx1 > 640) bx1 = 640;
    if (by1 > 480) by1 = 480;
    if (bx0 >= bx1 || by0 >= by1) {
        original_blit_sprite(sprite_desc, src_pixels, palette_lut, dst_x, dst_y, width, height, flags);
        return;
    }
    const int bw = bx1 - bx0, bh = by1 - by0;
    const int rowbytes = bw * 2;
    for (int y = 0; y < bh; ++y) memcpy(&s_pre[y*bw], fb + 1280*(by0+y) + 2*bx0, rowbytes);
    original_blit_sprite(sprite_desc, src_pixels, palette_lut, dst_x, dst_y, width, height, flags);
    for (int y = 0; y < bh; ++y) memcpy(&s_org[y*bw], fb + 1280*(by0+y) + 2*bx0, rowbytes);
    for (int y = 0; y < bh; ++y) memcpy(fb + 1280*(by0+y) + 2*bx0, &s_pre[y*bw], rowbytes);
    // allow_threads=true so verify exercises the row-band path -> validates the
    // band-split math is bit-exact against the original, not just the inline path.
    fm2k::render_simd::BlitSprite(sprite_desc, src_pixels, palette_lut, dst_x, dst_y,
                                  width, height, flags,
                                  g_blit_cfg.mode == fm2k::render_simd::Mode::Simd, true);
    for (int y = 0; y < bh; ++y) {
        const uint16_t* row = (const uint16_t*)(fb + 1280*(by0+y) + 2*bx0);
        const uint16_t* org = &s_org[y*bw];
        for (int xx = 0; xx < bw; ++xx) {
            if (row[xx] != org[xx]) {
                if (s_logged < 40) {
                    ++s_logged;
                    const uint32_t obj = *(uint32_t*)0x4CFA00;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[BLIT-VERIFY] MISMATCH blend=%d flags=0x%04X @ (%d,%d) "
                        "expected=0x%04X got=0x%04X (box %dx%d at %d,%d)",
                        obj ? *(int*)(obj+0x54) : -1, (unsigned)(uint16_t)flags,
                        bx0+xx, by0+y, org[xx], row[xx], bw, bh, bx0, by0);
                }
                return;  // one report per blit
            }
        }
    }
}

// BlitSpriteWithBlendMode @ 0x40C140 hook. Three roles by env:
//   FM2K_BLIT_SIMD=simd/scalar -> replace with our reimplementation (the fix)
//   ...,verify                 -> double-render + pixel-diff vs original
//   FM2K_RENDER_PROFILE=1 only -> time/count/area/blend census (g_blit_cfg Off)
// Pure display path -- never touches sim/rollback state, so this is desync-safe.
static int __cdecl Hook_BlitSpriteWithBlendMode(int sprite_desc, int src_pixels,
                                                int palette_lut, int dst_x, int dst_y,
                                                int width, int height, short flags) {
    if (g_blit_cfg.mode != fm2k::render_simd::Mode::Off) {
        if (g_blit_cfg.verify) {
            BlitVerify(sprite_desc, src_pixels, palette_lut, dst_x, dst_y, width, height, flags);
            return 0;
        }
        if (g_blit_cfg.mode == fm2k::render_simd::Mode::Simd) {
            // LARGE blits (>=128kpx, >=64 tall) -> our reimpl with row-band
            // threading across cores, ALL modes: this is the only way to beat
            // copy mode (at the scalar floor) and what gets the copy-bound
            // heaviest stages to 100fps.
            const int aw = width  < 0 ? -width  : width;
            const int ah = height < 0 ? -height : height;
            if ((long)aw * ah >= 131072 && ah >= 64)
                return fm2k::render_simd::BlitSprite(sprite_desc, src_pixels, palette_lut,
                                                     dst_x, dst_y, width, height, flags, true, true);
            // Small blits: SSE2 wins only on the read-modify-write blend modes
            // (50%/add/sub). Copy (0) needs NO dst read (engine just does
            // `if(c) *dst=c`) and alpha (4) needs 32-bit products epi16 can't
            // hold -- both stay on the engine's already-optimal scalar loops.
            const uint32_t obj = *(volatile uint32_t*)0x4CFA00;
            const int bm = obj ? *(volatile int*)(obj + 0x54) : 0;
            if (bm >= 1 && bm <= 3)
                return fm2k::render_simd::BlitSprite(sprite_desc, src_pixels, palette_lut,
                                                     dst_x, dst_y, width, height, flags, true, false);
            return original_blit_sprite(sprite_desc, src_pixels, palette_lut,
                                        dst_x, dst_y, width, height, flags);
        }
        // Scalar reimplementation (FM2K_BLIT_SIMD=scalar) -- debug/verify path.
        return fm2k::render_simd::BlitSprite(sprite_desc, src_pixels, palette_lut, dst_x, dst_y,
                                             width, height, flags, false);
    }
    // Profiler-only census path.
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const int ret = original_blit_sprite(sprite_desc, src_pixels, palette_lut,
                                         dst_x, dst_y, width, height, flags);
    QueryPerformanceCounter(&t1);
    g_rp_blit_ns += (uint64_t)(t1.QuadPart - t0.QuadPart);
    ++g_rp_blit_calls;
    int w = width  < 0 ? -width  : width;
    int h = height < 0 ? -height : height;
    g_rp_blit_area += (uint64_t)(unsigned)w * (uint64_t)(unsigned)h;
    const uint32_t obj = *(volatile uint32_t*)0x4CFA00;  // g_object_data_ptr
    if (obj) {
        const int m = *(volatile int*)(obj + 0x54);      // 0=copy 1=50% 2=add 3=sub 4=alpha
        if (m >= 0 && m <= 4) ++g_rp_blit_mode[m];
    }
    return ret;
}

// sprite_rendering_engine @ 0x40CC30 hook (FM2K_BLIT_SIMD only). Intercepts
// the case -10 full-screen feedback blur (render_mode obj[+0x10]==-10, which
// does ONLY the blur), runs our reimplementation, and skips the original. All
// other render modes fall through untouched.
static void __cdecl Hook_SpriteRenderingEngine(int mode_arg) {
    const uint32_t obj = *(uint32_t*)0x4CFA00;
    if (obj && *(int*)(obj + 0x10) == -10) {
        const int passes = *(int*)(obj + 342) / 20;       // obj[+342]/20 (case -10)
        if (passes > 0) {
            uint16_t* fb = *(uint16_t**)0x4246CC;
            const bool fmt565 = (*(int*)0x424704 != 0);
            const bool simd = (g_blit_cfg.mode == fm2k::render_simd::Mode::Simd);
            if (g_blit_cfg.verify) {
                // Full-screen verify: original blur, snapshot, restore, ours, diff.
                static uint16_t s_pre[640 * 480];
                static uint16_t s_org[640 * 480];
                static int s_blurlog = 0;
                const int N = 640 * 480;
                memcpy(s_pre, fb, N * 2);
                original_sprite_render_engine(mode_arg);   // runs original blur
                memcpy(s_org, fb, N * 2);
                memcpy(fb, s_pre, N * 2);
                fm2k::render_simd::BlurFullscreen(fb, passes, fmt565, simd);
                if (memcmp(fb, s_org, N * 2) != 0 && s_blurlog < 10) {
                    ++s_blurlog;
                    int fi = 0; for (; fi < N; ++fi) if (fb[fi] != s_org[fi]) break;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[BLUR-VERIFY] MISMATCH passes=%d fmt565=%d @px%d expected=0x%04X got=0x%04X",
                        passes, (int)fmt565, fi, s_org[fi], fb[fi]);
                }
            } else {
                fm2k::render_simd::BlurFullscreen(fb, passes, fmt565, simd);
            }
            return;  // skip original (case -10 does nothing but the blur)
        }
    }
    original_sprite_render_engine(mode_arg);
}

void __cdecl Hook_RenderGame() {
    // FM95 host-driven trampoline: if Hook_UpdateGameState already drove
    // RenderFrameWithSnapshot inside the tick (non-NATIVE phase), the
    // host's natural render_game call would render twice. Skip the body
    // and clear the flag.
    if constexpr (FM2K::kIsFM95) {
        if (g_fm95_skip_next_render) {
            g_fm95_skip_next_render = false;
            return;
        }
    }

    // In trampoline mode, render goes through RenderFrameWithSnapshot in
    // main_loop_trampoline.cpp. This hook still catches direct calls from
    // the game (e.g. init/menu paths) -- run diagnostics and pass through.
    // Also fires under FM2K_BYPASS_TRAMPOLINE=1 (vanilla main_game_loop)
    // so [EB] diag works for A/B testing the trampoline against the
    // vanilla render path.
    Hook_RenderDiagnostics_Tick();
    EbDiag_Dump("PRE-SAVE");

    // CRITICAL: Save/restore RNG around render to prevent render-path RNG
    // consumption from breaking determinism. ProcessShakeEffect and
    // ProcessColorInterpolation call game_rand() during rendering, and
    // Render-path state protection.
    //
    // Stress-mode desync dump (FM2K_stress_desync_f158.log) showed that
    // after a LOAD+replay the four regions below diverged from the forward
    // save even though memcpy restore ran correctly. Cause: the render
    // path (ProcessShakeEffect / ProcessColorInterpolation / sprite
    // updates) mutates these regions, and our render gate skips render
    // during replay frames. Forward ran N renders, replay ran 0 renders,
    // so render-side mutations accumulated only in forward. Result:
    //   RNG_Seed, ObjectPool, AfterimagePool, InputTracking all drifted.
    // CharDynamic / GameState / Object topology stayed matched because
    // render doesn't touch them.
    //
    // Fix: snapshot these regions before render, restore after render.
    // Same idea as the existing RNG protection, extended to the other
    // three. That way render can freely update visual counters but the
    // gameplay-authoritative memory image is unchanged across renders.
    //
    // SPECTATOR FIX: include SpectatorNode_IsPlayingBack() -- without it,
    // Hook_RenderGame (this function) ran UNPROTECTED on spectators when
    // FM2K-internal code triggered render directly (instead of through the
    // trampoline's RenderFrameWithSnapshot, which already had this check).
    // ProcessShakeEffect / ProcessColorInterpolation / sprite_rendering_engine
    // call game_rand() -- those calls accumulated into spectator's RNG but
    // were rolled back on host. RNG drifted by exactly that delta over time,
    // showing up as paired [HOST-FP]/[SPEC-FP] divergence with all other
    // sim state matching (HP/timer/pos/input identical, only RNG differed).
    // FM2K-ONLY. The protect save/restore below memcpys the afterimage pool
    // (WaveCAddrs::AFTERIMAGE_POOL) and the input-tracking block at 0x447EE0 --
    // both FM2K addresses. On FM95 AFTERIMAGE_POOL / _SZ are 0-sentinels, so
    // the slices become a read from null + a ~4GB size_t underflow + a write
    // to the FM2K input-tracking address = guaranteed AV the first render
    // frame with an active session. FM95's render-side protected regions are
    // RE-4 pending; until then FM95 renders without protection (acceptable --
    // same as RenderFrameWithSnapshot, which gates its own block on kIsFM2K).
    [[maybe_unused]] bool protect_regions =
        Netplay_IsActive() || SpectatorNode_IsPlayingBack();

    static uint8_t s_saved_object_pool[0x5F800];
    static uint8_t s_saved_afterimage_pool[WaveCAddrs::AFTERIMAGE_POOL_SZ];
    static uint8_t s_saved_input_tracking[0xA0];

    // Carve-outs: SHAKE_EFFECTS @ 0x447DA9 (40 B) and EFFECT_SYS1 @
    // 0x447D7D (42 B, palette flash 1) MUST NOT be restored, or render's
    // per-frame mutations to those regions get reverted every render.
    // Mirrors the carve-outs in main_loop_trampoline.cpp::
    // RenderFrameWithSnapshot. Both regions sit inside the afterimage_pool
    // slice; EFFECT_SYS1 is just before the shake block.
    constexpr uintptr_t kPflash1Addr   = 0x447D7D;
    constexpr size_t    kPflash1Size   = 42;
    constexpr size_t    kPflash1Offset = kPflash1Addr - WaveCAddrs::AFTERIMAGE_POOL;
    constexpr size_t    kPflash1End    = kPflash1Offset + kPflash1Size;
    constexpr uintptr_t kShakeAddr     = 0x447DA9;
    constexpr size_t    kShakeSize     = 40;
    constexpr size_t    kShakeOffset   = kShakeAddr - WaveCAddrs::AFTERIMAGE_POOL;  // 0x479
    constexpr size_t    kShakeEnd      = kShakeOffset + kShakeSize;
    static_assert(kPflash1End <= kShakeOffset, "EFFECT_SYS1 must end before shake block");
    if constexpr (FM2K::kIsFM2K) if (protect_regions) {
        // RNG intentionally NOT saved/restored -- render's game_rand calls
        // need to propagate so palette mode 3 / shake mode 4 produce
        // animated random values matching vanilla. See trampoline comment.
        //
        // Object pool also NOT saved/restored -- palette mode 1
        // (Tyrogue fade-to-black) needs ProcessColorInterpolation's per-frame
        // writes to g_object_data_ptr+68/72/76/80 to PERSIST into the next
        // frame's object pool, otherwise the fade visually undoes itself
        // when the timer reaches 0.
        (void)s_saved_object_pool;
        // Afterimage save: three slices skipping both EFFECT_SYS1 and shake.
        memcpy(s_saved_afterimage_pool,
               (void*)WaveCAddrs::AFTERIMAGE_POOL,
               kPflash1Offset);
        memcpy(s_saved_afterimage_pool + kPflash1End,
               (void*)(WaveCAddrs::AFTERIMAGE_POOL + kPflash1End),
               kShakeOffset - kPflash1End);
        memcpy(s_saved_afterimage_pool + kShakeEnd,
               (void*)(WaveCAddrs::AFTERIMAGE_POOL + kShakeEnd),
               WaveCAddrs::AFTERIMAGE_POOL_SZ - kShakeEnd);
        memcpy(s_saved_input_tracking,  (void*)0x447EE0,                     0xA0);
    }

    // In battle mode under GekkoNet, render only when a new sim tick has
    // been produced since the last render. Otherwise render mutates
    // object-pool animation state on wall-clock cadence and desyncs peers.
    bool gate_render = Netplay_IsActive();
    bool do_render = !gate_render || g_frame_pending_render;
    EbDiag_Dump("PRE-RENDER");
    if (do_render && original_render_game) {
        // Render RNG isolation: re-seed the render stream from the gameplay
        // seed, then route render's game_rand draws to it (see globals.h /
        // Hook_GameRand). Gameplay seed is left untouched by render.
        g_render_rng_seed = *(uint32_t*)FM2K::ADDR_RANDOM_SEED;
        g_in_render_rng = true;
        original_render_game();
        g_in_render_rng = false;
        g_frame_pending_render = false;
    }
    EbDiag_Dump("POST-RENDER");

    if constexpr (FM2K::kIsFM2K) if (protect_regions) {
        // (RNG and object_pool restores removed -- see save block.)
        // Afterimage restore: mirror of the 3-slice split save -- both
        // EFFECT_SYS1 and shake regions in live memory keep whatever
        // render-side code just wrote.
        memcpy((void*)WaveCAddrs::AFTERIMAGE_POOL,
               s_saved_afterimage_pool,
               kPflash1Offset);
        memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + kPflash1End),
               s_saved_afterimage_pool + kPflash1End,
               kShakeOffset - kPflash1End);
        memcpy((void*)(WaveCAddrs::AFTERIMAGE_POOL + kShakeEnd),
               s_saved_afterimage_pool + kShakeEnd,
               WaveCAddrs::AFTERIMAGE_POOL_SZ - kShakeEnd);
        memcpy((void*)0x447EE0,                    s_saved_input_tracking,  0xA0);
    }
    EbDiag_Dump("POST-RESTORE");

    // Update shared memory with current stats for launcher
    SharedMem_Update();

    // GekkoNet frame-pacing drift correction now lives in the
    // trampoline's SleepToTarget -- that function applies the 1.6%
    // slowdown when ahead of peer. The Sleep(extra_ms) trick that
    // used to be in Netplay_HandleFrameTime was unreliable (Sleep
    // granularity, fights with QPC-based outer loop) and didn't
    // actually converge frames_ahead.
}

// Hook: RunGameLoop
// Detours to the main-loop trampoline; we own the outer game loop from this
// point forward. Pre-trampoline side effects (VS-mode patch) fire before the

bool InstallRenderHooks() {
    // Hook RenderGame
    if (MH_CreateHook((void*)FM2K::ADDR_RENDER_GAME, (void*)Hook_RenderGame,
                      (void**)&original_render_game) != MH_OK ||
        MH_QueueEnableHook((void*)FM2K::ADDR_RENDER_GAME) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks: Failed to hook RenderGame");
        return false;
    }

    // Render reimplementation (FM2K_BLIT_SIMD) and/or profiler census
    // (FM2K_RENDER_PROFILE). FM2K-only; installed only when one of those envs
    // is set, so normal play carries zero extra hooks. The blit-leaf hook
    // serves both roles; the sprite-engine hook (case -10 blur) only matters
    // for the reimplementation. All display-only -> desync-safe.
    if constexpr (FM2K::kIsFM2K) {
        g_blit_cfg = fm2k::render_simd::ParseConfig();
        const char* rp = std::getenv("FM2K_RENDER_PROFILE");
        const bool want_profile = (rp && rp[0] == '1');
        const bool want_simd    = (g_blit_cfg.mode != fm2k::render_simd::Mode::Off);
        if (want_profile || want_simd) {
            if (MH_CreateHook((void*)FM2K::ADDR_BLIT_SPRITE,
                              (void*)Hook_BlitSpriteWithBlendMode,
                              (void**)&original_blit_sprite) != MH_OK ||
                MH_QueueEnableHook((void*)FM2K::ADDR_BLIT_SPRITE) != MH_OK) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Hooks: Failed to hook BlitSpriteWithBlendMode");
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: BlitSpriteWithBlendMode @ 0x%08X hooked (simd=%d verify=%d profile=%d)",
                    (unsigned)FM2K::ADDR_BLIT_SPRITE,
                    (int)(g_blit_cfg.mode == fm2k::render_simd::Mode::Simd),
                    (int)g_blit_cfg.verify, (int)want_profile);
            }
        }
        if (want_simd) {
            if (MH_CreateHook((void*)FM2K::ADDR_SPRITE_RENDER_ENGINE,
                              (void*)Hook_SpriteRenderingEngine,
                              (void**)&original_sprite_render_engine) != MH_OK ||
                MH_QueueEnableHook((void*)FM2K::ADDR_SPRITE_RENDER_ENGINE) != MH_OK) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Hooks: Failed to hook sprite_rendering_engine (case -10 blur)");
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Hooks: sprite_rendering_engine @ 0x%08X hooked (case -10 blur reimpl)",
                    (unsigned)FM2K::ADDR_SPRITE_RENDER_ENGINE);
            }
        }
    }

    // Hook DispatchScriptSoundCommand -- Mike Z desired/actual sound layer.
    // During battle the hook records `desired[channel]` instead of playing;
    // SoundRollback::SyncAfterAdvance reconciles once per displayed frame by
    // calling back through the original trampoline.
    if (MH_CreateHook((void*)FM2K::ADDR_DISPATCH_SCRIPT_SOUND,
                      (void*)Hook_DispatchScriptSoundCommand,
                      (void**)&original_dispatch_script_sound) != MH_OK ||
        MH_QueueEnableHook((void*)FM2K::ADDR_DISPATCH_SCRIPT_SOUND) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Hooks: Failed to hook DispatchScriptSoundCommand");
        return false;
    }
    SoundRollback::SetOriginalDispatcher(original_dispatch_script_sound);
    return true;
}

