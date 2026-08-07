// dllmain_logging.cpp -- async (quill) file logging + crash/SEH handlers.
// Split VERBATIM from dllmain.cpp. Init/Install/Shutdown are the entry
// points DllMain calls (declared in dllmain_internal.h); the logger state
// + LogOutputFunction/CrashHandler/VectoredRenderGuard stay file-local.
// Clean DLL entry point with persistent network connection
#include "globals.h"
#include "dllmain_internal.h"
#include "hooks.h"
#include "netplay.h"
#include "netplay_state.h"
#include "control_channel.h"
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
// detoured on game_state_manager -- sharing the same MinHook entry).
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

// Quill -- async logging backend. SDL_Log call sites stay unchanged; our
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
// LOG-FILE PATH HELPER
// =============================================================================
// All hook-side log files (FM2K_P*_Debug.log + EB diag + rng trace + parity
// recorder + replay diffs) route through here so they land in
// `<cwd>/logs/<filename>` instead of cluttering the game folder. Lazily
// creates the logs directory.
bool Fm2k_BuildLogPath(char* out, size_t out_size, const char* filename) {
    if (!out || out_size == 0 || !filename) return false;
    static bool s_dir_created = false;
    if (!s_dir_created) {
        // CreateDirectoryA returns FALSE + ERROR_ALREADY_EXISTS if it's
        // already there; either way the dir is usable afterwards.
        CreateDirectoryA("logs", nullptr);
        s_dir_created = true;
    }
    int n = std::snprintf(out, out_size, "logs\\%s", filename);
    return n > 0 && (size_t)n < out_size;
}

// =============================================================================
// FILE LOGGING (quill async backend)
// =============================================================================
//
// Pre-quill: each SDL_Log call → LogOutputFunction → fprintf(g_log_file,
// ...) + fflush(). Synchronous file I/O on the sim thread; under heavy
// log volume (~3 lines/sec SPEC-FP plus ROLLBACK stats plus SoundRollback
// chatter) the per-line WriteFile + fflush adds up to visible jitter.
//
// Now: SDL_Log → LogOutputFunction formats the line → LOG_INFO into a
// quill Logger. Quill's backend thread drains the queue and writes to
// disk async on its own thread, so the sim never blocks on disk.
//
// We use a custom PatternFormatter ("%(message)") that emits the raw
// formatted message verbatim -- no quill-prefixed timestamp/level dup,
// since LogOutputFunction already builds the "[hh:mm:ss.mmm] [P1] [INFO]"
// prefix in the same shape the user/CI grep tools expect.
static quill::Logger* g_quill_logger = nullptr;

// Fallback FILE* for the case where quill init fails (rare -- only seen on
// some 32-bit injected DLL paths where the backend thread can't spawn).
// LogOutputFunction prefers g_quill_logger when non-null; falls through
// to this synchronous-but-reliable file write otherwise. Bug v0.2.32:
// silent quill init failures left users with no log file at all on
// game launch. Belt-and-suspenders fallback added v0.2.33.
static FILE* g_log_file_fallback = nullptr;

static void SDLCALL LogOutputFunction(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    // Get timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);

    const char* priority_str = "INFO";
    switch (priority) {
        case SDL_LOG_PRIORITY_VERBOSE: priority_str = "VERBOSE"; break;
        case SDL_LOG_PRIORITY_DEBUG: priority_str = "DEBUG"; break;
        case SDL_LOG_PRIORITY_INFO: priority_str = "INFO"; break;
        case SDL_LOG_PRIORITY_WARN: priority_str = "WARN"; break;
        case SDL_LOG_PRIORITY_ERROR: priority_str = "ERROR"; break;
        case SDL_LOG_PRIORITY_CRITICAL: priority_str = "CRITICAL"; break;
        default: break;
    }

    // PII scrub on the message body BEFORE we splice it into the
    // output line. We don't scrub the whole formatted string because
    // the prefix (`[hh:mm:ss.mmm] [P1] [INFO]`) has no user data and
    // running regex on a smaller buffer is cheaper.
    char scrubbed_msg[2048];
    fm2k::pii::ScrubInto(message ? message : "",
                         scrubbed_msg, sizeof(scrubbed_msg));

    // Format: [HH:MM:SS.mmm] [TAG] [PRIORITY] message. FM2K_LOG_TAG overrides the
    // default "P<index+1>" tag -- e.g. a spectator launches with FM2K_LOG_TAG=S1
    // so its log reads [S1] not [P3], making multi-spectator output legible.
    static const char* s_log_tag = []() -> const char* {
        const char* t = std::getenv("FM2K_LOG_TAG");
        return (t && t[0]) ? t : nullptr;
    }();
    char formatted[2048];
    if (s_log_tag) {
        snprintf(formatted, sizeof(formatted), "[%02d:%02d:%02d.%03d] [%s] [%s] %s\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 s_log_tag, priority_str, scrubbed_msg);
    } else {
        snprintf(formatted, sizeof(formatted), "[%02d:%02d:%02d.%03d] [P%d] [%s] %s\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 g_player_index + 1, priority_str, scrubbed_msg);
    }

    // File-only by default -- fputs to stdout is synchronous and lags the
    // sim under heavy log volume (SoundRollback / ROLLBACK stats /
    // BATTLE STATUS / SPEC-FP cadence). Spectator instances historically
    // mirrored to console for live protocol tracing; gated now behind
    // FM2K_SPECTATOR_DEBUG=1 so release-mode spectators stay quiet (per-
    // session log file is still on disk for post-hoc inspection).
    if (g_spectator_mode) {
        static int s_console_mirror_env = -1;
        if (s_console_mirror_env < 0) {
            const char* v = std::getenv("FM2K_SPECTATOR_DEBUG");
            s_console_mirror_env =
                (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
        }
        if (s_console_mirror_env == 1) {
            fputs(formatted, stdout);
            fflush(stdout);
        }
    }

    // Quill async path: one snprintf'd line per call, hands off to the
    // backend thread for actual disk I/O. Pattern is "%(message)" so the
    // line lands in the file verbatim -- same byte stream as the prior
    // fprintf path, just async. Strip the trailing \n we added since
    // quill always appends one of its own.
    if (g_quill_logger) {
        size_t len = std::strlen(formatted);
        if (len > 0 && formatted[len - 1] == '\n') {
            formatted[len - 1] = '\0';
        }

        // Diagnostic backtrace routing.
        //
        // SDL_LOG_CATEGORY_CUSTOM (and beyond) marks high-frequency
        // diagnostic lines that we want CAPTURED but not always WRITTEN.
        // Currently used by [SPEC-FP] / [HOST-FP] / [SPEC-TRACE] /
        // [HOST-TRACE] / pb_queue=N (~3 lines/sec each). Quill's
        // backtrace ring (init_backtrace below) buffers the last 4096
        // such lines in memory; LOG_ERROR through any channel auto-
        // flushes the ring to disk just before the error line, giving
        // us "diag-armed forever, zero disk IO until something breaks."
        //
        // FM2K_SPECTATOR_DEBUG=1 routes diag straight to disk like a
        // normal log line -- keep the existing opt-in path for users
        // who want continuous capture during a debug session.
        //
        // Routing for non-CUSTOM (regular APPLICATION) categories: WARN
        // and below go to LOG_INFO (file); ERROR/CRITICAL go to LOG_ERROR
        // (file + auto-flush backtrace).
        if (category >= SDL_LOG_CATEGORY_CUSTOM) {
            static int s_spec_diag_to_file = -1;
            if (s_spec_diag_to_file < 0) {
                const char* v = std::getenv("FM2K_SPECTATOR_DEBUG");
                s_spec_diag_to_file =
                    (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
            }
            if (s_spec_diag_to_file == 1) {
                LOG_INFO(g_quill_logger, "{}", formatted);
            } else {
                LOG_BACKTRACE(g_quill_logger, "{}", formatted);
            }
        } else if (priority >= SDL_LOG_PRIORITY_ERROR) {
            // ERROR / CRITICAL -- flushes the backtrace ring as a side
            // effect of init_backtrace's flush_level being set to Error.
            LOG_ERROR(g_quill_logger, "{}", formatted);
        } else {
            LOG_INFO(g_quill_logger, "{}", formatted);
        }
    } else if (g_log_file_fallback) {
        // Quill init failed -- write directly. Re-add the trailing newline
        // since the LOG_BACKTRACE path strips it but fprintf needs it back.
        size_t len = std::strlen(formatted);
        bool had_newline = (len > 0 && formatted[len - 1] == '\n');
        fputs(formatted, g_log_file_fallback);
        if (!had_newline) fputc('\n', g_log_file_fallback);
        fflush(g_log_file_fallback);
    }
}

void InitFileLogging() {
    // Capture USERNAME etc. before the first SDL_Log call hits the
    // scrubber. Idempotent -- the launcher's logger init also calls this.
    fm2k::pii::Init();

    char base_name[64];
    if (const char* tag = std::getenv("FM2K_LOG_TAG"); tag && tag[0]) {
        snprintf(base_name, sizeof(base_name), "FM2K_%s_Debug.log", tag);
    } else {
        snprintf(base_name, sizeof(base_name), "FM2K_P%d_Debug.log", g_player_index + 1);
    }
    char filename[MAX_PATH];
    if (!Fm2k_BuildLogPath(filename, sizeof(filename), base_name)) {
        // Fallback to cwd if the helper failed (extremely unlikely -- only
        // CreateDirectoryA would have to fail with something other than
        // ERROR_ALREADY_EXISTS, e.g. permission denied).
        snprintf(filename, sizeof(filename), "%s", base_name);
    }

    // v0.2.33: quill init removed from the hook DLL.
    //
    // Why: InitFileLogging runs from DLL_PROCESS_ATTACH, which Windows
    // serializes under the loader lock. quill::Backend::start spawns a
    // std::thread for its writer; CreateThread blocks until DllMain
    // returns, deadlocking the loader. Symptom in v0.2.32: AllocConsole
    // (a few lines above) succeeds → "FM2K P*N* Console" window appears →
    // hook hangs in Backend::start → game's WinMain never resumes → no
    // game window, no log file. try/catch can't help: it's a hang, not a
    // throw.
    //
    // The launcher (which inits quill from main(), not DllMain) is fine.
    // The hook just uses synchronous fopen below -- same as v0.2.31 and
    // earlier. Quill stays vendored; if we want it back in the hook a
    // future commit can defer init to a worker spawned AFTER DllMain
    // returns (e.g. on first frame-hook callback).
    bool quill_ok = false;
    g_quill_logger = nullptr;

    if (!quill_ok) {
        g_log_file_fallback = fopen(filename, "w");
        if (g_log_file_fallback) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(g_log_file_fallback,
                    "=== FM2K Hook Debug Log - Player %d ===\n",
                    g_player_index + 1);
            fprintf(g_log_file_fallback,
                    "Session started: %04d-%02d-%02d %02d:%02d:%02d\n",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
            fprintf(g_log_file_fallback,
                    "==========================================\n");
            fflush(g_log_file_fallback);
        }
    }

    // Wire SDL's logger to dispatch through our function so each
    // SDL_LogInfo / Warn / Error call site (~150 across the hook) reaches
    // the quill backend without changing call-site code.
    SDL_SetLogOutputFunction(LogOutputFunction, nullptr);
}

// Crash dumper -- last-chance handler that records faulting address +
// exception code + a hex stack-backtrace to the same log file the rest
// of the hook writes to. No DbgHelp dependency: we capture raw frame
// addresses (RtlCaptureStackBackTrace) and dump the module name + RVA
// for each so you can post-process against the IDA / .map / .pdb later.
//
// Why this exists: alt-tab during init or modal title-drag mid-boot
// occasionally crashes the game right after the main pump resumes.
// Without a stack we can only guess at root cause (D3D9 device-lost,
// rollback catchup explosion, ImGui state mismatch -- all plausible).
// This handler turns the next incident into a concrete trace.
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    FILE* f = g_log_file_fallback;  // shared with normal logging
    if (!f) {
        // Last-resort fallback -- open a dedicated crash file in case the
        // main log handle is lost. Same logs/ dir.
        char path[MAX_PATH];
        char base[64];
        std::snprintf(base, sizeof(base),
                      "FM2K_P%d_Crash.log", g_player_index + 1);
        if (Fm2k_BuildLogPath(path, sizeof(path), base)) {
            f = fopen(path, "w");
        }
    }
    if (!f) return EXCEPTION_CONTINUE_SEARCH;

    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD code = ep && ep->ExceptionRecord
                 ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep && ep->ExceptionRecord
                 ? ep->ExceptionRecord->ExceptionAddress : nullptr;

    fprintf(f,
        "\n=== CRASH at %02d:%02d:%02d.%03d (P%d) ===\n"
        "ExceptionCode    = 0x%08lX\n"
        "ExceptionAddress = %p\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        g_player_index + 1,
        (unsigned long)code, addr);

    if (ep && ep->ContextRecord) {
        const CONTEXT* c = ep->ContextRecord;
        fprintf(f,
            "EIP=0x%08lX EAX=0x%08lX EBX=0x%08lX ECX=0x%08lX EDX=0x%08lX\n"
            "ESI=0x%08lX EDI=0x%08lX EBP=0x%08lX ESP=0x%08lX\n",
            (unsigned long)c->Eip, (unsigned long)c->Eax,
            (unsigned long)c->Ebx, (unsigned long)c->Ecx,
            (unsigned long)c->Edx, (unsigned long)c->Esi,
            (unsigned long)c->Edi, (unsigned long)c->Ebp,
            (unsigned long)c->Esp);

        // ExceptionInformation[0] = 0 read, 1 write, 8 DEP.
        // ExceptionInformation[1] = the inaccessible address. Critical
        // for AVs -- tells us "tried to {read,write} <addr>".
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR rw   = ep->ExceptionRecord->ExceptionInformation[0];
            ULONG_PTR vbad = ep->ExceptionRecord->ExceptionInformation[1];
            fprintf(f, "AV: %s 0x%08lX\n",
                    rw == 0 ? "read" : rw == 1 ? "write" : "exec",
                    (unsigned long)vbad);
        }
    }

    // Backtrace: RtlCaptureStackBackTrace gives us up to 62 frames on
    // 32-bit Windows. Walk and resolve each to module+RVA via
    // GetModuleHandleEx with FROM_ADDRESS (no DbgHelp needed).
    void* frames[64] = {};
    USHORT n = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
    fprintf(f, "Backtrace (%u frames):\n", (unsigned)n);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char modname[MAX_PATH] = {0};
        ULONG_PTR rva = 0;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)frames[i], &mod) && mod) {
            char fullpath[MAX_PATH] = {0};
            if (GetModuleFileNameA(mod, fullpath, sizeof(fullpath))) {
                const char* p = strrchr(fullpath, '\\');
                std::snprintf(modname, sizeof(modname),
                              "%s", p ? p + 1 : fullpath);
            }
            rva = (ULONG_PTR)frames[i] - (ULONG_PTR)mod;
        }
        fprintf(f, "  [%02u] %p  %s+0x%08lX\n",
                (unsigned)i, frames[i],
                modname[0] ? modname : "?",
                (unsigned long)rva);
    }
    fprintf(f, "=== END CRASH ===\n\n");
    fflush(f);

    // Drop an upload manifest so the launcher posts the crash artifact
    // to the hub on next launch (the launcher polls upload_queue/
    // every tick -- see PollUploadQueue). We're inside an unhandled
    // exception filter so the heap may be unstable; the std::vector
    // + std::string allocations Enqueue does are small (~520 bytes
    // worst case) and use the same allocator the existing fprintf
    // calls above already touched without crashing, so the marginal
    // risk is low. If it ever blows up in the wild we can switch to
    // an inline fprintf-only manifest writer here.
    {
        char cwd[MAX_PATH] = {};
        GetCurrentDirectoryA(sizeof(cwd), cwd);
        char debug_path[MAX_PATH], crash_path[MAX_PATH];
        std::snprintf(debug_path, sizeof(debug_path),
                      "%s\\logs\\FM2K_P%d_Debug.log",
                      cwd, g_player_index + 1);
        std::snprintf(crash_path, sizeof(crash_path),
                      "%s\\logs\\FM2K_P%d_Crash.log",
                      cwd, g_player_index + 1);

        char exe_path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
        const char* basename = exe_path;
        for (char* p = exe_path; *p; ++p) {
            if (*p == '\\' || *p == '/') basename = p + 1;
        }
        char game_id[128] = {};
        std::strncpy(game_id, basename, sizeof(game_id) - 1);
        if (char* dot = std::strrchr(game_id, '.')) *dot = '\0';

        fm2k::upload_queue::Manifest m;
        m.kind = "crash";
        m.frame = -1;  // unknown -- crashes can fire from any phase
        m.session_id = SpectatorNode_GetSessionId();
        m.player_index = g_player_index;
        m.game_id = game_id;
        // Reference both candidate log paths. The launcher's processor
        // silently skips paths that don't exist, so listing both lets
        // it pick up whichever one CrashHandler actually wrote to
        // (fallback path used Crash.log; primary path appended to
        // Debug.log via the shared g_log_file_fallback handle).
        m.file_paths.emplace_back(debug_path);
        m.file_paths.emplace_back(crash_path);
        fm2k::upload_queue::Enqueue(m);
    }

    return EXCEPTION_CONTINUE_SEARCH;  // let Windows' default handler
                                       // pop the WER dialog as usual
}

// Vectored exception handler for known vanilla-game render bugs that we
// can't (yet) prevent at the source.
//
// Currently catches: AV inside sprite_rendering_engine @ 0x40CC30 reading
// from `[g_effect_character_data_base + 0x114] + frame_idx*16 - 0x10`.
// Repro: alt-tab / drag title bar of one launcher instance during the CSS
// demo phase; the OTHER instance occasionally trips this AV. Stack
// register dump showed EDI=3 (case 3 = effect render path), EBX pointing
// at g_effect_character_data_base, EDX = a near-page-aligned heap-ish
// pointer that didn't extend 3 more bytes for the `mov ax, [edx+3]` read.
// Likely a vanilla edge case where an effect's sprite-table pointer is
// either uninitialized or off-by-one against the loaded sprite stream.
//
// Recovery strategy: jump EIP to a known full-epilogue position inside
// the function (0x40D7A8: `pop edi; pop esi; pop ebp; pop ebx; add esp,
// 0x6C; ret`). At the crash address the function has only executed its
// prologue + a handful of loads -- no extra stack pushes -- so the epilogue
// unwinds cleanly to the caller. Effect renders for that one frame are
// skipped; game keeps running.
//
// Throttle: log first 3 recoveries, then 1-per-1000 thereafter, to avoid
// blasting the log if the crash repeats every render frame.
static LONG WINAPI VectoredRenderGuard(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    constexpr DWORD kSpriteEngineStart    = 0x0040CC30;
    constexpr DWORD kSpriteEngineEnd      = 0x0040E3E8;  // start + 0x17B8
    constexpr DWORD kSpriteEngineEpilogue = 0x0040D7A8;
    constexpr DWORD kKnownCrashSite       = 0x0040CCAE;  // mov ax, [edx+3]

    CONTEXT* c = ep->ContextRecord;
    if (c->Eip < kSpriteEngineStart || c->Eip >= kSpriteEngineEnd) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Only recover at the known crash site for now -- broad jump-to-epilogue
    // from arbitrary EIPs inside the function risks unwinding past
    // mid-call stack pushes (later in the function). Tightens the
    // recovery to "the exact frame-decode AV we've observed" and lets
    // any new crash pattern surface as a real fault we can dump.
    if (c->Eip != kKnownCrashSite) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static std::atomic<uint32_t> s_count{0};
    const uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 1000) == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "VectoredRenderGuard: recovered AV at 0x%08lX in sprite_rendering_engine "
            "(occurrence #%u, EDX=0x%08lX) -- skipping render via epilogue jump",
            (unsigned long)c->Eip, (unsigned)n, (unsigned long)c->Edx);
    }
    c->Eip = kSpriteEngineEpilogue;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void InstallCrashHandler() {
    // Vectored handlers run BEFORE SEH-style filters and BEFORE the
    // unhandled-exception filter, so they get first crack at recovering
    // known-bad code paths. SetUnhandledExceptionFilter remains as the
    // last-resort dumper for everything we don't recover.
    AddVectoredExceptionHandler(/*FirstHandler=*/1, VectoredRenderGuard);
    SetUnhandledExceptionFilter(CrashHandler);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CrashHandler: installed (vectored render-guard + last-resort dumper)");
}

void ShutdownFileLogging() {
    if (g_quill_logger) {
        LOG_INFO(g_quill_logger, "{}", "=== Session ended ===");
        // Don't stop the backend thread here -- quill's Backend::start
        // installs a std::atexit handler that drains + stops cleanly on
        // normal process exit. Stopping early could swallow pending log
        // lines from late shutdown paths (gekko_destroy etc).
        g_quill_logger = nullptr;
    }
    if (g_log_file_fallback) {
        fprintf(g_log_file_fallback, "=== Session ended ===\n");
        fclose(g_log_file_fallback);
        g_log_file_fallback = nullptr;
    }
}

