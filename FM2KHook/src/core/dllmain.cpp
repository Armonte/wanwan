// Clean DLL entry point with persistent network connection
#include "globals.h"
#include "dllmain_internal.h"
#include "hooks.h"
#include "netplay.h"
#include "netplay_state.h"
#include "control_channel.h"
#include "../netplay/nat_traversal.h"  // ConfigureRelay for spectator relay fallback (#58)
#include "shared_mem.h"
#include "../ui/screenshot.h"
#include "../locale/locale_spoof.h"
#include "../util/pii_scrub.h"

// Forward-declare just the new patch function. We can't include
// game_patches.h here because dllmain.cpp keeps static duplicates of
// the older patch functions and the extern declarations would conflict.
extern void PatchVsRoundCase200T4FalsePositive();
extern void NeuterFullscreenTogglesForCncDdraw();

// Per-game patches that delegate to css_autoconfirm.cpp's hook (already
// detoured on game_state_manager — sharing the same MinHook entry).
#include "../hooks/css_autoconfirm.h"
#include "../hooks/round_events.h"
#include "../hooks/per_game_patches.h"
#include "../netplay/upload_queue.h"
#include "../netplay/spectator_node.h"  // SpectatorNode_GetSessionId
#include "version_local.h"  // fm2k::kAppBranch / kAppVersion / kAppRevision
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <cctype>
#include <string>

// Quill — async logging backend. SDL_Log call sites stay unchanged; our
// SDLCustomLogOutput function (LogOutputFunction below) routes the formatted
// message into a quill Logger which the backend thread writes to disk
// asynchronously. Fixes the "fprintf + fflush per log line" stall that was
// noticeable on the spectator under SPEC-FP / ROLLBACK-stats cadence.
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/FileSink.h"
#include "quill/core/PatternFormatterOptions.h"


// =============================================================================
// GAME PATCHES
// =============================================================================

// Forward declarations for game patches (from original game_patches.cpp)

// Both engines refuse to start when another instance's window exists:
// WinMain does FindWindowA(<class>, 0) and, if found, MessageBox + exit.
// Patch the guarding `jz short` (0x74) into `jmp short` (0xEB) so the
// "already running" body is always skipped — required for 2-instance
// local testing on one PC. Runs from DllMain before ResumeThread, so it
// lands before WinMain executes.
//
//   FM2K (WonderfulWorld_ver_0946): FindWindowA("KGT2KGAME") gate,
//     jz at 0x405D05 (74 0E).
//   FM95 (CPW): FindWindowA("KGT95GAME") gate in _WinMain@16 @0x40AB60,
//     test eax; jz short loc_40ABA9 at 0x40AB7F (74 28).
static void BypassMultiInstanceCheck() {
    constexpr uintptr_t kJzSite = FM2K::kIsFM95 ? 0x40AB7F : 0x405D05;
    uint8_t* jz_addr = (uint8_t*)kJzSite;

    if (IsBadReadPtr(jz_addr, 1) || *jz_addr != 0x74) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: multi-instance jz site 0x%08X has 0x%02X (expected 0x74) — "
            "binary doesn't match the validated build; skipping bypass",
            (unsigned)kJzSite, IsBadReadPtr(jz_addr, 1) ? 0u : *jz_addr);
        return;
    }
    DWORD old_protect;
    if (VirtualProtect(jz_addr, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *jz_addr = 0xEB;  // jz -> jmp short
        VirtualProtect(jz_addr, 1, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Patch: Bypassed multi-instance check");
    }
}

static void ApplyBootToCharacterSelectPatches() {
    // Three boot strategies, all hit the same patch site at 0x409CD9
    // (the `push <type>` immediate inside InitializeGameFromCommandLine).
    //
    //   no patch (FM2K_SKIP_BOOT_CSS_PATCH=1)
    //     vanilla path: push 0x11 → ProcessIntroCutscene (3 bitmaps,
    //     ~300-frame splash) → title_screen_manager → CSS.
    //
    //   default (safe boot)
    //     push 0x0C → title_screen_manager directly. Skips the splash.
    //     Title runs its init (ResetObjectsAndCalculateSpeed, populates
    //     g_titleMenu_modeList, etc.). The hook then auto-mashes button
    //     A while g_game_mode == 1000 and lands on CSS with VS Player
    //     selected. Universally compatible — every game we've tested
    //     boots fine through the title path.
    //
    //   fast boot (FM2K_BOOT_TO_CSS_DIRECT=1)
    //     push 0x0A → game_state_manager (CSS) directly. Skips title
    //     entirely. *NOT SAFE* on every game — StudioS Fighters /
    //     Strip Fighter Zero characters trip on uninitialized
    //     g_char_velocity_multiplier and self-execute KGT_OP_THROW_EXECUTE
    //     (opcode 0x15) on frame 0 of battle. Only enable for games
    //     verified to behave (WW, etc.).
    if (const char* e = std::getenv("FM2K_SKIP_BOOT_CSS_PATCH"); e && std::strcmp(e, "1") == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: FM2K_SKIP_BOOT_CSS_PATCH=1 — leaving vanilla intro "
            "path (cutscene + title screen).");
        return;
    }

    uint8_t boot_type = 0x0C;  // default: title_screen_manager
    const char* fast = std::getenv("FM2K_BOOT_TO_CSS_DIRECT");
    if (fast && std::strcmp(fast, "1") == 0) {
        boot_type = 0x0A;  // game_state_manager (CSS direct)
    }

    uint8_t* init_object_ptr = (uint8_t*)0x409CD9;
    DWORD old_protect;
    if (VirtualProtect(init_object_ptr, 2, PAGE_EXECUTE_READWRITE, &old_protect)) {
        init_object_ptr[0] = 0x6A;          // push imm8 (unchanged)
        init_object_ptr[1] = boot_type;
        VirtualProtect(init_object_ptr, 2, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Patch: Boot to %s (push 0x%02X)",
            boot_type == 0x0A ? "CSS direct (FAST)" :
            boot_type == 0x0C ? "title screen (SAFE)" : "?",
            boot_type);
    }
}

#if defined(ENGINE_FM95)
// FM95 counterpart of ApplyBootToCharacterSelectPatches. CPW's obj_init_main_
// state (type 3 @0x40F0D0) spawns the 3-logo splash (type 30 obj_logo_intro_
// state, ~700 frames / ~7 s) on a normal boot via `push 0x1E` @0x40F1B9. Patch
// the immediate 0x1E -> 0x0F so it spawns type 15 (obj_title_demo_loop) =
// TITLE-DIRECT: skips the logos but STILL runs scan_player_files + the full
// roster/demo/round init, so the versus CSS that follows is safe. (CSS-direct
// -- imm 0x13/type-19 -- is UNSAFE: only type 15 calls scan_player_files, so
// the roster stays unpopulated and the versus-CSS scan loop reads garbage; we
// deliberately never take that route.) Escape hatch: FM95_KEEP_LOGOS=1.
static void Fm95ApplyBootSkip() {
    // OPT-IN. Title-direct is NOT yet validated across CPW builds -- real-play
    // testing surfaced a "player error" (title-15 may skip demo/roster setup
    // the logo path establishes on some builds), so the default is the vanilla
    // 3-logo boot (known-good). Enable with FM95_SKIP_LOGOS=1 once a build is
    // confirmed clean. Netplay works without it (entry just waits out the
    // attract path), so this is a pure entry-latency optimization, not required.
    if (const char* e = std::getenv("FM95_SKIP_LOGOS"); !(e && e[0] == '1')) {
        return;  // default: vanilla boot
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "PATCH: FM95_SKIP_LOGOS=1 — applying title-direct boot-skip (opt-in)");
    uint8_t* imm = (uint8_t*)0x40F1BA;   // imm8 of `push 0x1E` at 0x40F1B9
    DWORD old = 0;
    if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Patch: FM95 boot-skip VirtualProtect failed @0x40F1BA");
        return;
    }
    const uint8_t was = *imm;
    if (was == 0x1E) {                   // byte-guard: only patch the expected push imm
        *imm = 0x0F;                     // type 30 (logos) -> type 15 (title-direct)
        FlushInstructionCache(GetCurrentProcess(), imm, 1);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Patch: FM95 boot -> title-direct (skip logos), push 0x1E->0x0F @0x40F1BA");
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Patch: FM95 boot-skip SKIPPED — unexpected byte 0x%02X @0x40F1BA (want 0x1E)", was);
    }
    VirtualProtect(imm, 1, old, &old);
}
#endif  // ENGINE_FM95

static void ApplyCharacterSelectModePatches() {
    if (const char* e = std::getenv("FM2K_SKIP_VS_MODE_PATCH"); e && std::strcmp(e, "1") == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: FM2K_SKIP_VS_MODE_PATCH=1 — not forcing VS player mode.");
        return;
    }
    // Set character select mode to VS player (1) instead of VS CPU (0)
    uint8_t* char_select_mode_ptr = (uint8_t*)0x470058;  // CHARACTER_SELECT_MODE_ADDR

    DWORD old_protect;
    if (VirtualProtect(char_select_mode_ptr, 1, PAGE_READWRITE, &old_protect)) {
        *char_select_mode_ptr = 1;
        VirtualProtect(char_select_mode_ptr, 1, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Patch: Set VS player mode");
    }
}

static void DisableCursorHiding() {
    // In InitializeDirectDraw, the game hides cursor and clips it to 1x1 pixel in fullscreen:
    //   0x4049e0: call ds:ClipCursor   ; clips to tiny rect
    //   0x4049e6: push ebx (0)         ; arg for ShowCursor
    //   0x4049e7: call ds:ShowCursor   ; hides cursor
    //
    // Patch 1: Replace ClipCursor call with pop eax + 5 NOPs (removes pushed rect arg)
    // Patch 2: Replace push + ShowCursor call with 7 NOPs

    DWORD old_protect;

    // Patch ClipCursor at 0x4049e0 (6 bytes)
    // Original: FF 15 AC C1 41 00 (call ds:ClipCursor)
    // Patched:  58 90 90 90 90 90 (pop eax + 5 nops) - pops the pushed rect pointer
    uint8_t* clip_cursor_addr = (uint8_t*)0x4049e0;
    if (VirtualProtect(clip_cursor_addr, 6, PAGE_EXECUTE_READWRITE, &old_protect)) {
        clip_cursor_addr[0] = 0x58;  // pop eax
        clip_cursor_addr[1] = 0x90;  // nop
        clip_cursor_addr[2] = 0x90;  // nop
        clip_cursor_addr[3] = 0x90;  // nop
        clip_cursor_addr[4] = 0x90;  // nop
        clip_cursor_addr[5] = 0x90;  // nop
        VirtualProtect(clip_cursor_addr, 6, old_protect, &old_protect);
    }

    // Patch ShowCursor at 0x4049e6 (7 bytes: push + call)
    // Original: 53 FF 15 74 C1 41 00 (push ebx; call ds:ShowCursor)
    // Patched:  90 90 90 90 90 90 90 (7 nops)
    uint8_t* show_cursor_addr = (uint8_t*)0x4049e6;
    if (VirtualProtect(show_cursor_addr, 7, PAGE_EXECUTE_READWRITE, &old_protect)) {
        show_cursor_addr[0] = 0x90;  // nop (was push ebx)
        show_cursor_addr[1] = 0x90;  // nop
        show_cursor_addr[2] = 0x90;  // nop
        show_cursor_addr[3] = 0x90;  // nop
        show_cursor_addr[4] = 0x90;  // nop
        show_cursor_addr[5] = 0x90;  // nop
        show_cursor_addr[6] = 0x90;  // nop
        VirtualProtect(show_cursor_addr, 7, old_protect, &old_protect);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Patch: Disabled cursor hiding in fullscreen");
}

// Runtime fix for the GameSpeed (Editor.TestPlay.GameSpeed INI key) setting
// breaking character / effect / VS-intro scripts at any value other than 10.
//
// Root cause (init_round_state_and_apply_gamespeed @ 0x406450):
//
//   v3 = (gs <= 10) ? (5*gs + 50) : (10*gs);   // gs=10 -> 100, gs=5 -> 75
//   g_gamespeed_move_scalar  (0x541F78) = 0x10000 / v3
//   g_gamespeed_accel_scalar (0x445700) = 3932160 / (v3*v3)
//   g_gamespeed_pic_step     (0x445704) = v3
//
// op_MOVE in the script interpreter (character_state_machine @ 0x411bf0)
// uses MOVE/ACCEL scalars to scale velocity per frame — fine. But op_PIC
// (case 0x0C) ADDS `v3 * keepTime` to a per-object wait timer that gets
// DECREMENTED by a HARDCODED 100 per frame at the top of the dispatch
// loop. At gs=10 the two cancel exactly; at any other gs they don't:
//
//   keepTime  gs=10 frames  gs=5 frames
//      1          2             1        (2x faster)
//      2          3             2        (1.5x)
//      3          4             3        (1.33x)
//
// Small waits collapse disproportionately and desync against unscaled
// systems (hitstop, hurtbox lifetimes, BG_FX timers, g_game_timer).
// Vanpri-style scripts with tight OP_PIC chains hit the dispatch loop's
// 300-iter safety break and leave objects stuck.
//
// Fix: NOP the `mov [g_gamespeed_pic_step], ecx` write at 0x40649A and
// pin g_gamespeed_pic_step = 100. MOVE/ACCEL scalars still vary with gs
// (so the game still feels faster at lower gs) but the script-time clock
// stays exact.
//
// Opt-out: FM2K_KEEP_GAMESPEED_PIC=1 leaves vanilla behavior for re-test.
static void FixGameSpeedDesync() {
    if (const char* e = std::getenv("FM2K_KEEP_GAMESPEED_PIC"); e && std::strcmp(e, "1") == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: FM2K_KEEP_GAMESPEED_PIC=1 — leaving vanilla "
            "gs-pic-timer behavior (gs != 10 will desync scripts).");
        return;
    }

    // NOP the 6-byte write `mov ds:[0x445704], ecx` at 0x40649A
    // (89 0D 04 57 44 00 -> 90 90 90 90 90 90).
    uint8_t* pic_write = (uint8_t*)0x40649A;
    DWORD old_protect;
    if (VirtualProtect(pic_write, 6, PAGE_EXECUTE_READWRITE, &old_protect)) {
        std::memset(pic_write, 0x90, 6);
        VirtualProtect(pic_write, 6, old_protect, &old_protect);
    }

    // Pin g_gamespeed_pic_step = 100 so OP_PIC behaves as if gs=10 from
    // frame 0 onward (init_round_state_and_apply_gamespeed no longer
    // overwrites it, but we still need a defined initial value).
    uint32_t* pic_step = (uint32_t*)0x445704;
    if (VirtualProtect(pic_step, 4, PAGE_READWRITE, &old_protect)) {
        *pic_step = 100;
        VirtualProtect(pic_step, 4, old_protect, &old_protect);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Patch: GameSpeed PIC desync fix applied "
        "(pic_step pinned to 100; MOVE/ACCEL still scale with gs)");
}

// Configure per-game patches that live as runtime flags inside other
// hook modules (rather than byte patches). Reads env vars set by the
// launcher's ApplyGamePatchEnvVars before CreateProcess and forwards
// them to the owning module.
static void ApplyPerGameRuntimePatches() {
    // Team-mode CSS duplicate-slot lock — handled inside
    // css_autoconfirm.cpp's existing game_state_manager detour.
    if (const char* e = std::getenv("FM2K_TEAM_CSS_DUPE_LOCK");
        e && std::strcmp(e, "1") == 0) {
        CssAutoConfirm_SetTeamDupeLock(true);
    }

    // KOF-style HP/meter retention — handled inside round_events.cpp's
    // vs_round_function detour.
    if (const char* e = std::getenv("FM2K_TEAM_KOF_RETENTION");
        e && std::strcmp(e, "1") == 0) {
        RoundEvents_SetKofRetention(true);
    }

    // Team size override + stubs (vs_cpu / cpu_vs_cpu / training /
    // option_mode_selector). The damage multiplier MinHook is installed
    // separately from InitializeHooks (it needs MH_Initialize to have
    // run already).
    PerGamePatches_ApplyRuntime();
}

// ---------------------------------------------------------------------------
// DirectDrawCreate IAT-thunk repair (SJIS-folder silent-fallback fix)
//
// FM2K statically imports exactly one ddraw function — DirectDrawCreate — at a
// single IAT slot. The launcher renames the import descriptor DDRAW.dll ->
// 2DFMD.dll before injection so the loader resolves cnc-ddraw. But the loader
// binds that thunk on the FM2KHook injection thread (process-init runs there),
// and we observed that on SJIS-folder launches the *bound function pointer*
// ends up in STOCK ddraw even though the descriptor name reads 2DFMD.dll and
// 2DFMD is loaded — so the game's DirectDrawCreate call silently bypasses
// cnc-ddraw (no D3D9 device, no F4 fullscreen, no overlay). Renaming the
// descriptor is not enough; the already-bound pointer wins.
//
// We run in DllMain on the injection thread — AFTER import binding but BEFORE
// ResumeThread — so the game hasn't called DirectDrawCreate yet. Read the
// bound thunk, log where it points, and if it isn't 2DFMD's DirectDrawCreate,
// rewrite it. Idempotent (no-op when already correct) and folder-path-agnostic.
static void RepairDDrawImportThunk() {
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return;
    auto* base = reinterpret_cast<uint8_t*>(exe);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const DWORD imp_rva =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva) return;

    HMODULE wrapper = GetModuleHandleA("2DFMD.dll");
    if (!wrapper) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawThunk: 2DFMD.dll not loaded — cannot repair (redirect off?).");
        return;
    }
    auto* correct = reinterpret_cast<void*>(GetProcAddress(wrapper, "DirectDrawCreate"));
    if (!correct) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawThunk: 2DFMD.dll exports no DirectDrawCreate — cannot repair.");
        return;
    }
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imp_rva);
    for (; desc->Name; ++desc) {
        const char* dllname = reinterpret_cast<const char*>(base + desc->Name);
        std::string low = dllname;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.find("2dfmd") == std::string::npos &&
            low.find("ddraw") == std::string::npos)
            continue;

        auto* iat = reinterpret_cast<void**>(base + desc->FirstThunk);
        auto* oft = desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->OriginalFirstThunk)
            : nullptr;
        for (int i = 0; iat[i]; ++i) {
            const char* fname = "?";
            if (oft && oft[i].u1.AddressOfData &&
                !(oft[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32)) {
                auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + oft[i].u1.AddressOfData);
                fname = reinterpret_cast<const char*>(ibn->Name);
            }
            // FM2K imports only DirectDrawCreate from this DLL. When we can read
            // the name, restrict the rewrite to it; otherwise (bound-only IAT)
            // fix the single slot regardless — a ddraw-wrapper descriptor only
            // matters for DirectDrawCreate anyway.
            const bool is_ddc = (std::strcmp(fname, "?") == 0) ||
                                (std::strcmp(fname, "DirectDrawCreate") == 0);
            void* cur = iat[i];
            // Common case: thunk already bound to 2DFMD — stay silent, no-op.
            // Only log/repair when a DirectDrawCreate slot is actually misbound
            // (would silently route the game to stock ddraw → GDI-ish fallback).
            if (cur == correct || !is_ddc) continue;
            DWORD op = 0;
            if (VirtualProtect(&iat[i], sizeof(void*), PAGE_READWRITE, &op)) {
                iat[i] = correct;
                VirtualProtect(&iat[i], sizeof(void*), op, &op);
                FlushInstructionCache(GetCurrentProcess(), &iat[i], sizeof(void*));
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "DDrawThunk: RE-POINTED %s::DirectDrawCreate %p -> 2DFMD %p "
                    "(was misbound to stock ddraw; game now wraps through cnc-ddraw)",
                    dllname, cur, correct);
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "DDrawThunk: VirtualProtect failed on slot %p: %lu",
                    (void*)&iat[i], GetLastError());
            }
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        {
            DisableThreadLibraryCalls(hModule);

            // Force 1ms timer resolution for accurate 100fps frame timing.
            // Without this, timeGetTime() has ~15.6ms resolution and
            // background windows drop to ~64fps (appears as 60fps).
            timeBeginPeriod(1);

            // Read player index early so the log filename is right.
            if (char* env_player = std::getenv("FM2K_PLAYER_INDEX")) {
                g_player_index = std::atoi(env_player);
            }

            // Init console + file logging FIRST so every later step
            // (locale spoof, FM2K patches, hook init) is captured in
            // <game_dir>/logs/FM2K_P*_Debug.log.
            //
            // Console window is gated on FM2K_DEV_MODE=1 or
            // FM2K_SPECTATOR_DEBUG=1 — release-mode users don't want a
            // stray "FM2K P*N* Console" window next to their game. Log
            // file is always written either way.
            {
                const char* dev = std::getenv("FM2K_DEV_MODE");
                const char* spec_dbg = std::getenv("FM2K_SPECTATOR_DEBUG");
                bool want_console =
                    (dev && dev[0] == '1' && dev[1] == '\0') ||
                    (spec_dbg && spec_dbg[0] == '1' && spec_dbg[1] == '\0');
                if (want_console) {
                    FreeConsole();
                    AllocConsole();
                    FILE* fDummy;
                    freopen_s(&fDummy, "CONOUT$", "w", stdout);
                    freopen_s(&fDummy, "CONOUT$", "w", stderr);
                    freopen_s(&fDummy, "CONIN$", "r", stdin);
                    char title[64];
                    std::snprintf(title, sizeof(title),
                                  "FM2K P%d Console", g_player_index + 1);
                    SetConsoleTitleA(title);
                }
            }
            InitFileLogging();
            InstallCrashHandler();
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== FM2K Hook Starting (Player %d) ===", g_player_index + 1);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "FM2KHook build: %s %s rev %s (%s)",
                fm2k::kAppBranch, fm2k::kAppVersion,
                fm2k::kAppRevision, fm2k::kAppBuildTime);

            // ddraw-redirect diagnostic: enumerate every loaded module and
            // log the names + paths of anything matching ddraw / 2dfmd. This
            // runs from the FM2KHook injection thread, so process init has
            // already happened — the EXE's static imports are loaded by now,
            // including whichever ddraw flavor the loader picked. If our
            // patch worked, we should see 2DFMD.dll in the list. If we see
            // DDRAW.dll instead (from system32 / KnownDlls), the patch
            // didn't influence import resolution.
            {
                HMODULE mods[256] = {};
                DWORD needed = 0;
                HANDLE proc = GetCurrentProcess();
                if (EnumProcessModulesEx(proc, mods, sizeof(mods), &needed,
                                         LIST_MODULES_ALL)) {
                    const DWORD count = needed / sizeof(HMODULE);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "DDrawDiag: %lu modules loaded; scanning for ddraw/2dfmd:",
                        (unsigned long)count);
                    bool any_match = false;
                    bool has_our_wrapper = false;   // 2DFMD.dll = our renamed cnc-ddraw
                    bool has_stock_ddraw = false;   // a ddraw.dll resolved from a Windows system dir
                    char stock_ddraw_path[MAX_PATH] = {};
                    for (DWORD i = 0; i < count; ++i) {
                        char name[MAX_PATH] = {};
                        if (GetModuleBaseNameA(proc, mods[i], name, sizeof(name))) {
                            // Case-insensitive substring match
                            std::string lower = name;
                            for (auto& c : lower) c = (char)tolower((unsigned char)c);
                            const bool is_2dfmd = lower.find("2dfmd") != std::string::npos;
                            const bool is_ddraw = lower.find("ddraw") != std::string::npos;
                            if (is_ddraw || is_2dfmd) {
                                char path[MAX_PATH] = {};
                                GetModuleFileNameExA(proc, mods[i], path, sizeof(path));
                                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "DDrawDiag:   MATCH base='%s' path='%s' handle=0x%p",
                                    name, path, (void*)mods[i]);
                                any_match = true;
                                if (is_2dfmd) has_our_wrapper = true;
                                if (is_ddraw && !is_2dfmd) {
                                    // A module literally named ddraw.dll — classify by
                                    // its path. Resolved from a Windows system dir means
                                    // the loader picked STOCK DirectDraw (the redirect
                                    // never took), which renders exclusive-fullscreen.
                                    std::string lp = path;
                                    for (auto& c : lp) c = (char)tolower((unsigned char)c);
                                    if (lp.find("\\windows\\")  != std::string::npos ||
                                        lp.find("\\system32\\") != std::string::npos ||
                                        lp.find("\\syswow64\\") != std::string::npos) {
                                        has_stock_ddraw = true;
                                        std::strncpy(stock_ddraw_path, path,
                                                     sizeof(stock_ddraw_path) - 1);
                                    }
                                }
                            }
                        }
                    }
                    // Loud verdict — the silent-fullscreen class (June 2026: SJIS-folder
                    // games "went fullscreen" with no error because nothing flagged that
                    // the cnc-ddraw redirect had missed). Never fail silently again.
                    if (has_our_wrapper) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "DDrawDiag: OK — cnc-ddraw (2DFMD.dll) is loaded; the game "
                            "renders through our wrapper (windowed presentation).");
                    } else if (has_stock_ddraw) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                            "DDrawDiag: *** cnc-ddraw redirect FAILED *** game loaded "
                            "STOCK DirectDraw ('%s') — it will render EXCLUSIVE-FULLSCREEN "
                            "with no rollback overlay. Check: cnc-ddraw installed next to "
                            "the launcher, and the IAT patch DDRAW.dll->2DFMD.dll applied.",
                            stock_ddraw_path);
                    } else if (!any_match) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "DDrawDiag: NO ddraw/2dfmd module loaded — "
                            "neither system DDRAW.dll nor our 2DFMD.dll "
                            "is present (game might not have static-imported it).");
                    }
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "DDrawDiag: EnumProcessModulesEx failed: %lu",
                        GetLastError());
                }
            }

            // Repair the DirectDrawCreate IAT thunk if the loader mis-bound it
            // to stock ddraw (the SJIS-folder silent-fallback class). Runs here
            // — after import binding, before ResumeThread — so the fix lands
            // before the game's first DirectDrawCreate call. FM2K-only: the
            // 0x41c000 import layout is engine-specific, and the helper keys on
            // the ddraw/2dfmd descriptor name so it's a no-op on anything else.
            if constexpr (FM2K::kIsFM2K) {
                RepairDDrawImportThunk();
            }

            // Locale spoofing — Japanese codepage / LCID + GDI/USER32 ANSI
            // text rendering hooks. Always-on for BOTH FM2K and FM95 builds:
            // re-encoding ASCII strings through CP932 round-trips identically
            // so English-only games stay byte-equivalent, while JP games
            // (CPW, WW, AOB, etc.) get proper text rendering instead of
            // mojibake. Opt-OUT via FM2K_NO_JP_LOCALE=1 for diagnosis.
            // The legacy FM2K_JP_LOCALE env var is now a no-op (still
            // tolerated so older configs don't error).
            //
            // MUST run before host CRT init — i.e. before ResumeThread fires
            // the game's main thread. DllMain in our injection thread is
            // the right place.
            {
                const char* env_off = std::getenv("FM2K_NO_JP_LOCALE");
                const bool disabled = (env_off && std::strcmp(env_off, "1") == 0);
                if (!disabled) {
                    InstallLocaleSpoof();
                } else {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "LocaleSpoof: DISABLED via FM2K_NO_JP_LOCALE=1");
                }
            }

            // Multi-instance bypass is engine-aware (FM2K 0x405D05 /
            // FM95 0x40AB7F) and expected-byte-guarded — runs on both.
            BypassMultiInstanceCheck();

            // FM2K-only binary patches. The literal addresses (0x409CD9,
            // 0x470058, 0x4049e0/e6) are FM2K-engine-specific — applying
            // them to FM95 (CPW) just stomps random bytes in its address
            // space and breaks the host. Gate on the engine flag so the
            // FM95 build is a no-op here.
            if constexpr (FM2K::kIsFM2K) {
                ApplyBootToCharacterSelectPatches();
                ApplyCharacterSelectModePatches();
                DisableCursorHiding();
                FixGameSpeedDesync();
                ApplyPerGameRuntimePatches();
            }
#if defined(ENGINE_FM95)
            // FM95 boot-skip: title-direct (skip the 3-logo splash). Byte-
            // guarded + FM95_KEEP_LOGOS escape hatch. Its address (0x40F1BA) is
            // CPW-specific, so it lives behind the engine macro like the FM2K
            // block above.
            Fm95ApplyBootSkip();
#endif
            // Neuter F4 / Alt+Enter game-side fullscreen toggles when
            // cnc-ddraw is loaded — its keyboard hook owns those keys
            // now, the game's redundant toggle would fight cnc-ddraw's
            // render state. Detection: 2DFMD.dll is present in the
            // process iff the launcher applied the IAT redirect at
            // suspended-process start. Engine-aware: FM2K patches F4 +
            // Alt+Enter, FM95 (CPW) patches Alt+Enter only (no F4
            // binding in CPW's WndProc).
            if (GetModuleHandleA("2DFMD.dll") != nullptr) {
                NeuterFullscreenTogglesForCncDdraw();
            }
            // [DISABLED] t4-walk patch was a workaround that masked the
            // real bug — StudioS chars take script-driven damage on the
            // very first frame of battle (HW write bp on g_p1_hp caught
            // health_damage_manager+0x85 zeroing HP without input).
            // Patching the round controller's t4 transition only stops
            // the bail-to-CSS but lets the underlying damage corruption
            // continue. Re-enable only behind FM2K_FORCE_T4_PATCH=1
            // for diagnostic comparisons.
            if (const char* env_force = std::getenv("FM2K_FORCE_T4_PATCH");
                env_force && std::strcmp(env_force, "1") == 0)
            {
                PatchVsRoundCase200T4FalsePositive();
            }

            // (Console + file logging are set up at the top of DLL_PROCESS_ATTACH
            // so locale-spoof and patch logs land in the file too.)

            // Initialize shared memory for launcher status reporting
            if (!InitializeSharedMemory()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SharedMem: Init failed (non-fatal)");
            }

            // Auto-capture banner pipeline. No-op unless
            // FM2K_AUTO_CAPTURE=1 + FM2K_CAPTURE_DIR=<path> are set
            // in env (= the launcher's banner-capture session). When
            // active, main_loop_trampoline emits screenshots at
            // intro / title / CSS / battle phase boundaries.
            FM2KCapture::Init();

            // Parse network config from environment
            char* env_offline = getenv("FM2K_TRUE_OFFLINE");
            g_offline_mode = (env_offline && strcmp(env_offline, "1") == 0);

            // Stress-test mode: single-instance GekkoStressSession determinism check.
            // Mutually exclusive with true offline (stress still needs GekkoNet active).
            char* env_stress = getenv("FM2K_STRESS_MODE");
            g_stress_mode = (env_stress && strcmp(env_stress, "1") == 0);
            if (g_stress_mode) {
                g_offline_mode = false;  // stress requires GekkoNet session to be alive
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Stress-test mode ENABLED (GekkoStressSession): "
                    "single-instance determinism check, no network");
            }

            // Spectator mode: passive viewer subscribing to a remote host
            // OR offline-replay file player. Both share the same hook init
            // path (SpectatorNode + pb_queue drain), but the wire/log
            // messaging differs — replay has no network, no peer, no
            // JOIN_REQ. Distinguish via FM2K_REPLAY_FILE.
            char* env_spectator = getenv("FM2K_SPECTATOR_MODE");
            g_spectator_mode = (env_spectator && strcmp(env_spectator, "1") == 0);
            const char* env_replay = getenv("FM2K_REPLAY_FILE");
            const bool is_offline_replay =
                g_spectator_mode && env_replay && env_replay[0];
            if (g_spectator_mode) {
                g_offline_mode = false;
                g_stress_mode  = false;
                if (is_offline_replay) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Replay mode ENABLED — playing %s (no network, no peer)",
                                env_replay);
                } else {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Spectator mode ENABLED — will SPEC_JOIN_REQ host on startup");
                }
            }

            if (g_spectator_mode) {
                // task #58: read the spec-relay creds (FM2K_HUB_RELAY_ADDR/
                // SESSION, set by the launcher from the grant's spec_relay
                // block). ConfigureRelay historically only ran in the
                // NETPLAY init path, so a viewer never armed its relay and
                // the 4s auto-fallback silently had nothing to engage.
                if (!is_offline_replay) {
                    ::fm2k::nat::ConfigureRelay();
                }
                // Spectator path: socket + control callback + SpectatorNode.
                // Replay mode short-circuits inside Netplay_InitAsSpectator
                // (no network setup), so the port/host config is unused but
                // we still pass valid placeholders to keep the call shape.
                char* env_port   = getenv("FM2K_LOCAL_PORT");
                char* env_remote = getenv("FM2K_REMOTE_ADDR");
                g_local_port = env_port ? (uint16_t)atoi(env_port) : 7002;
                if (env_remote) {
                    strncpy(g_remote_addr, env_remote, sizeof(g_remote_addr) - 1);
                } else {
                    snprintf(g_remote_addr, sizeof(g_remote_addr), "127.0.0.1:7000");
                }
                if (!is_offline_replay) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Spectator config: port=%u host=%s",
                                g_local_port, g_remote_addr);
                }
                if (!Netplay_InitAsSpectator(g_local_port, g_remote_addr)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "%s init failed — falling back to offline",
                                 is_offline_replay ? "Replay" : "Spectator");
                    g_spectator_mode = false;
                    g_offline_mode   = true;
                }
            } else if (!g_offline_mode && !g_stress_mode) {
                char* env_port = getenv("FM2K_LOCAL_PORT");
                char* env_remote = getenv("FM2K_REMOTE_ADDR");

                g_local_port = env_port ? (uint16_t)atoi(env_port) : (7000 + g_player_index);

                if (env_remote) {
                    strncpy(g_remote_addr, env_remote, sizeof(g_remote_addr) - 1);
                } else {
                    uint16_t remote_port = 7000 + (1 - g_player_index);
                    snprintf(g_remote_addr, sizeof(g_remote_addr), "127.0.0.1:%d", remote_port);
                }

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Network config: port=%d remote=%s", g_local_port, g_remote_addr);

                // ================================================================
                // PERSISTENT CONNECTION: Initialize netplay at DLL load
                // This sets up socket, callback, and starts connection handshake
                // ================================================================
                if (!Netplay_Init(g_player_index, g_local_port, g_remote_addr)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to initialize netplay!");
                    // Continue anyway - will run offline
                }
            } else if (g_stress_mode) {
                // Stress-test mode: no network, no socket, no control channel.
                // GekkoStressSession runs single-instance with both players local.
                // The battle-mode handler in Hook_UpdateGameState will call
                // Netplay_StartStressBattle() directly when game enters battle,
                // bypassing the remote-peer sync barrier.
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Stress mode - skipping socket init, GekkoStressSession will start at battle entry");
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Offline mode - netplay disabled");
            }

            // Initialize hooks
            if (!InitializeHooks()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize hooks!");
                return FALSE;
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "=== FM2K Hook Ready ===");
            break;
        }

    case DLL_PROCESS_DETACH:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K Hook shutting down...");

        // Restore default timer resolution
        timeEndPeriod(1);

        // Locale spoof teardown (idempotent if not installed).
        UninstallLocaleSpoof();

        // Shutdown in reverse order
        ShutdownHooks();

        // Tear down GDI+ for the auto-capture writer (no-op when
        // capture wasn't active this run).
        FM2KCapture::Shutdown();

        // Cleanup shared memory
        CleanupSharedMemory();

        // Shutdown netplay (GekkoNet session if active)
        Netplay_Shutdown();

        // Shutdown network socket (persistent connection)
        NetSocket_Shutdown();

        // Close log file
        ShutdownFileLogging();
        break;
    }
    return TRUE;
}
