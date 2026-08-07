// See main_loop_trampoline.h for rationale.
//
// We own the outer game loop. main_game_loop @0x405AD0 is hooked and detours
// to TrampolineMainLoop(); the original is never called. Eliminating the
// native wrapper eliminates its per-iteration state writes (g_last_frame_time,
// per-slot buf-idx fan-out, etc.) that run at a different cadence on
// forward-sim versus rollback-replay. All sim state transitions happen
// inside GekkoAdvanceEvent (in netplay.cpp) which is deterministic.
//
// Phase dispatch per iteration:
//   TRAMPOLINE_BATTLE -- Netplay_ProcessBattleInputPhase drives sim via gekko,
//                       render fires post-advance, pacing via HandleFrameTime.
//   CSS               -- lockstep via control channel; native sim functions
//                       called directly; pacing via a 10 ms sleep.
//   NATIVE            -- menu/intro/results; native sim functions called
//                       directly; pacing via a 10 ms sleep.
#include "main_loop_trampoline.h"
#include "globals.h"
#include "../hooks/hooks.h"
#include "../hooks/wndproc_subclass.h"
#include "../netplay/netplay.h"
#include "../netplay/control_channel.h"
#include "../netplay/game_hash.h"
#include "../netplay/spectator_node.h"
#include "../ui/shared_mem.h"
#include "../parity/parity_recorder.h"
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include "trampoline_internal.h"  // shared externs + cross-TU phase helpers


// ============================================================================
// UTILITIES
// ============================================================================

static LoopPhase ClassifyPhase() {
    // Spectator mode pins the phase: regardless of game_mode or whether the
    // upstream JOIN_ACK has arrived yet, run RunSpectatorTick. Pre-ACK the
    // queue is empty so the trampoline holds the boot frame; once inputs
    // start flowing the sim animates through CSS, locks chars, transitions
    // to battle, etc. -- all driven by the streamed input pipe.
    if (g_spectator_mode || SpectatorNode_IsPlayingBack()) {
        return LoopPhase::SPECTATOR_PLAYBACK;
    }

    // Battle session still alive (e.g. game_mode just flipped 3000→2000 inside
    // the final battle UG, but the GekkoNet battle session hasn't been
    // destroyed yet -- it's waiting on Netplay_IsBattleEndSynced). Stay in the
    // BATTLE phase so RunBattleTick keeps polling the swap-frame gate and
    // calling Netplay_EndBattle. Without this, the battle session sticks
    // forever and Netplay_StartCSSSession spins in a "session already exists"
    // retry loop.
    // STRESS pins too: the stress session lives across the whole match, but
    // on FM95 IsBattleMode only returns true for round-ACTIVE substates
    // (type-16 sub_state in [10,31]); round-result / round-intro substates
    // would classify NATIVE and drop the live stress session mid-match →
    // double-tick + un-skipped host render. FM2K stress rides the raw
    // game_mode 3000 so it never flapped; pin both engines here.
    if (Netplay_GetSessionKind() == NetplaySessionKind::BATTLE ||
        Netplay_GetSessionKind() == NetplaySessionKind::STRESS) {
        return LoopPhase::TRAMPOLINE_BATTLE;
    }

    // Engine-aware phase mapping. On FM2K g_game_mode hits 2000/3000+; on
    // FM95 it stays near 0 and phase lives in the object pool -- IsCSSMode /
    // IsBattleMode (hooks.cpp, declared in hooks.h) do the right walk for
    // each engine.
    uint32_t mode = *(uint32_t*)FM2K::ADDR_GAME_MODE;
    if (IsBattleMode(mode)) return LoopPhase::TRAMPOLINE_BATTLE;
    if (IsCSSMode(mode))    return LoopPhase::CSS;
    return LoopPhase::NATIVE;
}

// Non-blocking Windows message pump. Mirrors main_game_loop @ 0x405B0D.
// Returns false on WM_QUIT so the trampoline can exit cleanly.
static bool PumpMessages() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            // Surface the wParam (exit code from PostQuitMessage) AND
            // who the message was for. wParam=0 is normal close;
            // anything else is "something inside the game asked to
            // quit immediately." A user reporting WM_QUIT 4 ms after
            // 'main loop ACTIVE' means the game window self-destructed
            // during creation -- usually DirectDraw/DirectInput init
            // failure on the user's machine (driver, fullscreen, or
            // antivirus interference).
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PumpMessages: WM_QUIT received (wParam=%u hwnd=%p) -- main loop will exit",
                (unsigned)msg.wParam, (void*)msg.hwnd);
            return false;
        }
        // Diagnostic: dump cheat-relevant key messages coming through.
        // FM2K's WindowProc maps F1-F12 to debug cheats (F1 hit-boxes, F5/F6
        // instant-KO, F12 force round-end). StudioS games are mysteriously
        // receiving F12 events the user isn't actually pressing, so we want to
        // *see* the phantom F-keys.
        //
        // Default: only the function keys (VK_F1=0x70 .. VK_F24=0x87) on
        // key-up/down -- that's the entire point of this probe. Logging every
        // keystroke at INFO floods the log with movement keys (each WASD tap is
        // a KEYDOWN + KEYUP + WM_CHAR), so the firehose is gated behind
        // FM2K_MSG_DIAG=1 for when someone's actively debugging input routing.
        static const bool s_msg_diag = []() {
            const char* v = std::getenv("FM2K_MSG_DIAG");
            return v && v[0] == '1';
        }();
        const bool is_key = (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP
            || msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP);
        if (s_msg_diag && (is_key || msg.message == WM_CHAR))
        {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[MSG] msg=0x%04X wParam=0x%02X (%u) lParam=0x%08lX hwnd=%p",
                        (unsigned)msg.message, (unsigned)msg.wParam,
                        (unsigned)msg.wParam, (unsigned long)msg.lParam,
                        msg.hwnd);
        }
        // Suppress solo-Alt menu activation. When the user taps Alt with
        // no follow-up key, Windows sends WM_SYSCOMMAND/SC_KEYMENU which
        // makes the game's WindowProc enter modal menu mode (DefWindowProc
        // blocks the message pump until the user picks a menu item or
        // presses Esc). FM2K has no menu, so the game just appears to
        // freeze for a beat. Discard the no-key SC_KEYMENU but let other
        // SC_* commands (SC_CLOSE on the X button, SC_RESTORE on un-min,
        // SC_KEYMENU with an actual hotkey) pass through.
        if (msg.message == WM_SYSCOMMAND
            && (msg.wParam & 0xFFF0) == SC_KEYMENU
            && msg.lParam == 0)
        {
            continue;  // swallowed
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return true;
}

// ============================================================================
// ENTRY POINT
// ============================================================================

// Per-frame engine work, callable from either the FM2K trampoline loop OR
// FM95's host-driven update_game callsite. Returns the LoopPhase actually
// executed so callers can decide whether the host's natural render call
// should still fire (NATIVE) or be skipped (BATTLE / CSS -- we already
// drove update + render via AdvanceEvent / RenderFrameWithSnapshot).
//
// FM2K caller: TrampolineMainLoop wraps this with QPC-anchored pacing +
// SleepToTarget. Host main_game_loop is replaced wholesale.
//
// FM95 caller (Hook_UpdateGameState path -- Phase 2 wiring): the host's
// natural WinMain pumps messages and runs its own timeGetTime catchup
// loop, so the FM95 path skips PumpMessages and SleepToTarget -- calls
// this directly to drive one frame of GekkoNet save/load/advance dispatch.
LoopPhase TrampolineFrameTick() {
    LoopPhase phase = ClassifyPhase();
    switch (phase) {
        case LoopPhase::TRAMPOLINE_BATTLE:
            RunBattleTick();
            break;
        case LoopPhase::CSS:
            RunCssTick();
            break;
        case LoopPhase::NATIVE:
            RunNativeTick();
            break;
        case LoopPhase::SPECTATOR_PLAYBACK:
            RunSpectatorTick();
            break;
    }
    return phase;
}

LoopPhase TrampolineFrameTickHostDriven() {
    // Same as TrampolineFrameTick but NEVER runs RunNativeTick -- the FM95
    // host's WinMain owns the native update, so we'd double-tick title/menus
    // if we ran it here too. Non-NATIVE phases sim via gekko/CSS and suppress
    // the host render; NATIVE returns immediately for the caller to fall
    // through to its single original_update_game().
    LoopPhase phase = ClassifyPhase();
    // Mark the sim-tick window so Hook_timeGetTime virtualizes the clock only
    // here (not for CPW's WinMain pacing reads outside the tick).
    g_fm95_in_sim_tick = true;
    switch (phase) {
        case LoopPhase::TRAMPOLINE_BATTLE:   RunBattleTick();     break;
        case LoopPhase::CSS:                 RunCssTick();        break;
        case LoopPhase::SPECTATOR_PLAYBACK:  RunSpectatorTick();  break;
        case LoopPhase::NATIVE:              /* host drives it */ break;
    }
    g_fm95_in_sim_tick = false;
    return phase;
}

// .player file cache pre-warmer (background thread).
//
// FM2K's CSS code path opens a .player file synchronously every time the
// player's cursor hovers a new character. Cold-cache reads on Windows take
// 100-500ms per file -- fine for a human cursor (one move = one load) but
// disastrous for the spectator's input-replay-driven CSS replay, where
// 30+ cursor moves get replayed back-to-back as fast as the trampoline
// can run them. Pre-warming pulls every *.player file in the game
// directory into the OS page cache once at startup so subsequent reads
// hit RAM. No game-state side effects -- purely passive.
//
// Dev-mode gated. Enable via FM2K_DEV_MODE=1.
static DWORD WINAPI PreloadPlayerFiles_Worker(LPVOID) {
    const char* dev = std::getenv("FM2K_DEV_MODE");
    if (!(dev && dev[0] == '1' && dev[1] == '\0')) return 0;
    char cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryA(MAX_PATH, cwd) == 0) return 0;
    char pattern[MAX_PATH] = {};
    std::snprintf(pattern, sizeof(pattern), "%s\\*.player", cwd);

    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int warmed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char full[MAX_PATH] = {};
        std::snprintf(full, sizeof(full), "%s\\%s", cwd, fd.cFileName);
        // Open with read-share so we don't fight FM2K's own open if it
        // happens to race us. FILE_FLAG_SEQUENTIAL_SCAN hints the cache
        // manager to read-ahead aggressively.
        HANDLE f = CreateFileA(full, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        // Drain into a small scratch buffer -- we don't care about the
        // bytes, only that the OS pages them in.
        char scratch[64 * 1024];
        DWORD n = 0;
        while (ReadFile(f, scratch, sizeof(scratch), &n, nullptr) && n > 0) {}
        CloseHandle(f);
        ++warmed;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PlayerPreload: warmed OS cache for %d .player file(s) in %s",
                warmed, cwd);
    return 0;
}

BOOL TrampolineMainLoop() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Trampoline: main loop ACTIVE -- GekkoNet-driven outer loop");

    // Kick off background .player file cache pre-warmer. Runs once;
    // typically completes within a second or two. Subsequent FM2K opens
    // of these files (during CSS cursor moves) hit the OS page cache.
    {
        HANDLE t = CreateThread(nullptr, 0, PreloadPlayerFiles_Worker,
                                nullptr, 0, nullptr);
        if (t) CloseHandle(t);  // detached
    }

    // g_frame_time_ms (FM2K 0x41E2F0 / FM95 0x422F7C, routed via globals.h)
    // was set to 10 by the original main_game_loop at 0x405AD3. We no longer
    // run that code, so this global stays at whatever the DLL-load-time
    // default was (0 or garbage). Any game code still reading it (CSS
    // frame-skip math, debug BATTLE STATUS log) would misbehave. Set it to
    // the expected 100-fps target on trampoline entry. (TrampolineMainLoop
    // itself only runs on FM2K; routed anyway so no bare literal remains.)
    *(uint32_t*)FM2K::ADDR_FRAME_TIME_MS = 10;

    // Parity recorder: if FM2K_PARITY_RECORD_PATH is set, open the .pty file
    // here (DllMain runs too early -- the game globals aren't initialized yet,
    // and we want to record AFTER the warmup ticks below). MaybeAutoOpen
    // is a no-op if the env var isn't set, so this costs nothing in normal
    // play. Pairs with kgtengine's recorder for byte-level cross-engine
    // parity gates via tools/kgt_diff_pty.
    if (ParityRecorder::MaybeAutoOpen()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "ParityRecorder: opened .pty output (FM2K_PARITY_RECORD_PATH)");
    }

    // One-time warmup -- matches main_game_loop's 8× update_game_state at
    // 0x405AF3. Required to initialize game subsystems before any input.
    // FM2K-only: FM95's WinMain does init_game_state ONCE then loops (no 8x
    // warmup), and we take over AT the loop head (0x40AD67) after that init has
    // already run, so an extra 8x here would diverge from CPW's boot cadence.
    if constexpr (FM2K::kIsFM2K) {
        if (original_update_game) {
            for (int i = 0; i < 8; i++) original_update_game();
        }
    }

    // Main loop. Runs forever until WM_QUIT.
    while (true) {
        // QPC (microsecond-resolution) anchor for the frame pacing sleep.
        // SDL_GetTicks()'s 1ms resolution was causing pacing to land at
        // 10.5-11 ms per frame on average due to SDL_Delay overshoot --
        // capping the loop at ~90 FPS. Using QPC + a busy-wait tail on
        // the final ~1 ms hits 100 Hz cleanly.
        uint64_t tick_start = SDL_GetPerformanceCounter();

        if (!PumpMessages()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Trampoline: WM_QUIT -- exiting");
            // Patch the .pty header's frame_count and close the file before
            // process teardown. DllMain's DLL_PROCESS_DETACH runs at a point
            // where CRT FILE* state may already be torn down, so close here.
            ParityRecorder::Close();
            return TRUE;
        }

        // Try installing the WndProc subclass each tick until it sticks.
        // Cheap: bails immediately once already-installed. The window
        // class "KGT2KGAME" comes up shortly after WinMain registers its
        // class, so this typically lands within the first few frames.
        // Subclass swallows Alt-tap menu activation + drives a
        // WM_TIMER inside DefWindowProc's modal loops so window drag /
        // system menu open don't pause our networking.
        FM2KWndProc::TryInstall();

        // Run one frame's worth of engine work. The body lives in
        // TrampolineFrameTick so the FM95 host-driven path can reuse it.
        LoopPhase phase = TrampolineFrameTick();

        // Sample work-vs-frame timing (offline-inclusive, #63). Must run
        // before the pacing sleep so `work` excludes the sleep itself.
        MaybeLogFrametime(tick_start);

        // Phase-specific pacing.
        switch (phase) {
            case LoopPhase::TRAMPOLINE_BATTLE:
                // Pass the live frames_ahead so SleepToTarget applies the
                // 1.6% slowdown when the host is ahead of the client (or
                // vice versa). Without this the host runs rigid 10 ms
                // forever, the client trails by 7-8 frames permanently,
                // and FA stays pinned at ±8.5 -- exactly the symptom we
                // saw. Stress / offline / pre-session paths all return
                // 0.0 from Netplay_GetFramesAhead so the slowdown is a
                // no-op there.
                SleepToTarget(tick_start, 10, Netplay_GetFramesAhead());
                break;
            case LoopPhase::CSS:
            case LoopPhase::NATIVE:
                SleepToTarget(tick_start, 10);
                break;
            case LoopPhase::SPECTATOR_PLAYBACK: {
                // Always 100 Hz outer tick. Catchup behaviour lives entirely
                // INSIDE RunSpectatorTick's inner loop (one-shot at join,
                // bounded at 250ms wall-clock per outer tick) -- that path
                // drains 5–25 sim frames per outer tick during the burst,
                // which is plenty to chew through a typical join-time
                // backfill in a couple seconds. We deliberately don't
                // override the outer sleep here: the previous queue-depth-
                // based fast-forward (sleep 1ms when qd > 100) ignored
                // the user's "don't speed up after lag/loss" requirement
                // and re-engaged superspeed every reconnect. Holding 100
                // Hz here means once the initial catchup completes the
                // spectator simply tracks the live edge with whatever
                // delay the network gives it.
                //
                // Layer-2 render pacing (2026-06-23): pace the outer tick to the
                // host's MEASURED production period instead of a rigid 100Hz.
                // 100fps host -> 10ms (unchanged); a render-bound heavy-stage
                // host confirms inputs at ~70fps -> ~14ms, so the spectator
                // renders one frame per produced frame instead of out-running
                // production, draining the bank, and juddering on duplicate
                // frames. The production-rate window clamps to 10ms during the
                // catch-up flood so the outer tick never crawls.
                SleepToTarget(tick_start, SpectatorNode_RenderPeriodMs());
                break;
            }
        }
    }
}

#if defined(ENGINE_FM95)
// ============================================================================
// FM95 LOOP OWNERSHIP
//
// FM2K owns the loop by MinHook-ing main_game_loop (a standalone function).
// CPW's frame loop is INLINED inside WinMain (@0x40AB60), so there is no
// function entry to detour. Instead we inline-patch a 5-byte `jmp rel32` at the
// loop head 0x40AD67 (the `mov eax, g_quit_flag` that begins each iteration --
// exactly 5 bytes). WinMain's init (window/heap/init_game_state/frame-time
// setup, through 0x40AD62) all runs first; when execution reaches the loop head
// it jumps here and never returns to CPW's loop. On WM_QUIT we replicate
// WinMain's cleanup tail (0x40AEB0) and exit the process.
// ============================================================================
namespace {
    constexpr uintptr_t kFm95LoopHead            = 0x40AD67;  // while(1) head
    constexpr uintptr_t kFm95DirectDrawCleanup   = 0x41A380;  // directdraw_cleanup
    constexpr uintptr_t kFm95ShutdownResources   = 0x408440;  // ShutdownGameResources
    constexpr uintptr_t kFm95MainHeapHandle      = 0x437764;  // g_main_heap_handle
}

// Entered via the inline jmp from WinMain's loop head (no return address is
// pushed -- that is fine, we never RET; the loop runs until WM_QUIT and then we
// ExitProcess). [[noreturn]] so the compiler emits no reachable tail return.
[[noreturn]] static void __cdecl Fm95OwningLoopEntry() {
    TrampolineMainLoop();   // owns the loop; returns TRUE only on WM_QUIT

    // Replicate WinMain's cleanup tail so a clean quit restores the display
    // and frees the heap, matching CPW's own exit path.
    reinterpret_cast<void(__cdecl*)()>(kFm95DirectDrawCleanup)();
    reinterpret_cast<void(__cdecl*)()>(kFm95ShutdownResources)();
    HGLOBAL h = *reinterpret_cast<HGLOBAL*>(kFm95MainHeapHandle);
    if (h) { GlobalUnlock(h); GlobalFree(h); }
    ExitProcess(0);
}

// Install the inline patch. Called from Setup_Hooks (before WinMain runs), so
// the jmp is in place by the time CPW's WinMain reaches the loop head.
bool Fm95InstallOwningLoop() {
    uint8_t* p = reinterpret_cast<uint8_t*>(kFm95LoopHead);
    // rel32 from the end of our 5-byte jmp to the entry thunk. On 32-bit the
    // whole address space is < 4 GB so a DLL->EXE rel32 always reaches.
    const intptr_t rel = reinterpret_cast<intptr_t>(&Fm95OwningLoopEntry)
                       - static_cast<intptr_t>(kFm95LoopHead + 5);
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "FM95 loop-patch: VirtualProtect(0x%08X) failed", (unsigned)kFm95LoopHead);
        return false;
    }
    p[0] = 0xE9;  // jmp rel32
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(rel);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    g_fm95_loop_owned = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "FM95 loop-patch: jmp @0x%08X -> owning loop (rel=%d)",
                (unsigned)kFm95LoopHead, (int)rel);
    return true;
}
#endif  // ENGINE_FM95
