#include "launcher_log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>   // std::set_terminate / current_exception / rethrow_exception
#include <mutex>
#include <stdexcept>
#include <stdlib.h>    // _set_invalid_parameter_handler / _set_purecall_handler (MinGW CRT)
#include <string>

#include "version_local.h"                  // fm2k::kAppVersion / kAppBranch / kAppRevision
#include "FM2KHook/src/util/pii_scrub.h"    // fm2k::pii::ScrubInto

namespace fm2k::launcher_log {

namespace {

HANDLE     g_file = INVALID_HANDLE_VALUE;
std::mutex g_mu;  // guards normal-path WriteFile; the crash path skips it on purpose

// Previous top-level filter, so the crash handler chains instead of swallowing.
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;

// Previous std::terminate handler, chained after we log.
std::terminate_handler g_prev_terminate = nullptr;

const char* PriorityName(int p) {
    switch (p) {
        case SDL_LOG_PRIORITY_TRACE:    return "TRACE";
        case SDL_LOG_PRIORITY_VERBOSE:  return "VERB";
        case SDL_LOG_PRIORITY_DEBUG:    return "DEBUG";
        case SDL_LOG_PRIORITY_INFO:     return "INFO";
        case SDL_LOG_PRIORITY_WARN:     return "WARN";
        case SDL_LOG_PRIORITY_ERROR:    return "ERROR";
        case SDL_LOG_PRIORITY_CRITICAL: return "CRIT";
        default:                        return "?";
    }
}

// Append one already-scrubbed line. No-op if the sink is disabled.
void WriteLine(int priority, const char* msg) {
    if (g_file == INVALID_HANDLE_VALUE) {
        return;
    }
    char buf[2304];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = std::snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d][%s] %s\n",
                          st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                          PriorityName(priority), msg ? msg : "");
    if (n < 0) {
        return;
    }
    if (n > static_cast<int>(sizeof(buf))) {
        n = static_cast<int>(sizeof(buf));  // snprintf returns the untruncated length
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file == INVALID_HANDLE_VALUE) {
        return;  // closed under us between the early check and the lock
    }
    DWORD written = 0;
    WriteFile(g_file, buf, static_cast<DWORD>(n), &written, nullptr);
}

// Crash-context raw write: no lock, no alloc -- the faulting thread may already
// hold g_mu, and WriteFile straight to the OS cache survives process death.
void CrashWriteRaw(const char* buf, int n) {
    if (g_file == INVALID_HANDLE_VALUE || n <= 0) {
        return;
    }
    DWORD written = 0;
    WriteFile(g_file, buf, static_cast<DWORD>(n), &written, nullptr);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        void* addr = er->ExceptionAddress;

        // Resolve the faulting module + offset so the log alone points at the
        // bad DLL/EXE (e.g. "FM2KHook.dll+0x8C6B"). UNCHANGED_REFCOUNT: don't
        // touch the module's refcount from a crash context.
        char       modpath[MAX_PATH] = {0};
        uintptr_t  base = 0;
        HMODULE    mod  = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
            base = reinterpret_cast<uintptr_t>(mod);
            GetModuleFileNameA(mod, modpath, MAX_PATH);
        }
        const char* slash   = std::strrchr(modpath, '\\');
        const char* modname = slash ? slash + 1 : modpath;
        unsigned long long off = base ? (reinterpret_cast<uintptr_t>(addr) - base) : 0ull;

        char line[512];
        int n = std::snprintf(line, sizeof(line),
                              "[CRASH] unhandled exception code=0x%08lX addr=%p "
                              "module=%s+0x%llX flags=0x%lX\n",
                              static_cast<unsigned long>(er->ExceptionCode), addr,
                              modname[0] ? modname : "?", off,
                              static_cast<unsigned long>(er->ExceptionFlags));
        if (n > static_cast<int>(sizeof(line))) {
            n = static_cast<int>(sizeof(line));
        }
        CrashWriteRaw(line, n);
    }
    // Chain to whatever was installed before us (WER / SDL), so normal crash
    // handling still runs -- we only added a breadcrumb.
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// std::terminate path -- the launcher dying via an UNCAUGHT C++ EXCEPTION (the
// big blind spot of SetUnhandledExceptionFilter: a thrown std::exception that
// nobody catches calls std::terminate, NOT a structured exception, so it never
// reaches CrashFilter). Re-throw the in-flight exception to recover its what()
// so the log names the actual failure -- e.g. a std::filesystem_error on a
// OneDrive cloud-placeholder path, a std::bad_alloc, a std::system_error.
void TerminateHandler() {
    char what[1024] = "(no active exception)";
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            std::snprintf(what, sizeof(what), "%s", ex.what());
        } catch (...) {
            std::snprintf(what, sizeof(what), "(non-std exception type)");
        }
    }
    char line[1280];
    int n = std::snprintf(line, sizeof(line),
                          "[TERMINATE] std::terminate -- uncaught C++ exception: %s\n",
                          what);
    if (n > static_cast<int>(sizeof(line))) {
        n = static_cast<int>(sizeof(line));
    }
    CrashWriteRaw(line, n);
    if (g_prev_terminate) {
        g_prev_terminate();  // chain (usually -> abort, which our CrashFilter then sees)
    }
    std::abort();
}

// CRT fast-fail path -- a bad arg to a CRT function (e.g. a malformed format,
// an out-of-range index in a checked call) routes here instead of crashing, and
// also bypasses CrashFilter. Best-effort narrow of the wide CRT strings.
void InvalidParamHandler(const wchar_t* expr, const wchar_t* func,
                         const wchar_t* file, unsigned int line_no, uintptr_t) {
    char e[256] = {0};
    char f[256] = {0};
    if (expr) WideCharToMultiByte(CP_UTF8, 0, expr, -1, e, sizeof(e) - 1, nullptr, nullptr);
    if (func) WideCharToMultiByte(CP_UTF8, 0, func, -1, f, sizeof(f) - 1, nullptr, nullptr);
    char line[640];
    int n = std::snprintf(line, sizeof(line),
                          "[CRT] invalid parameter: expr='%s' func='%s' line=%u\n",
                          e[0] ? e : "?", f[0] ? f : "?", line_no);
    if (n > static_cast<int>(sizeof(line))) {
        n = static_cast<int>(sizeof(line));
    }
    CrashWriteRaw(line, n);
}

// Pure virtual call (calling a virtual through a partially-destroyed object).
void PurecallHandler() {
    static const char msg[] = "[CRT] pure virtual function call\n";
    CrashWriteRaw(msg, static_cast<int>(sizeof(msg) - 1));
    std::abort();
}

}  // namespace

void Init() {
    if (g_file != INVALID_HANDLE_VALUE) {
        return;  // idempotent
    }

    wchar_t exe[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    std::wstring path(exe, len);
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    std::wstring logpath  = dir + L"\\launcher.log";
    std::wstring prevpath = dir + L"\\launcher.prev.log";

    // FM2K_LAUNCHER_LOG -- absolute path override. The default path is derived
    // from the EXE directory, so N launchers sharing one install (the
    // documented two-local-instances dev flow, and the hub-brokered spectate
    // harness which runs THREE) all open the same file with CREATE_ALWAYS and
    // then write through independent file pointers: every instance truncates
    // the others and the result is unreadable. Pointing each instance at its
    // own file is the only way to get a per-instance log out of a multi-
    // instance session. Unset in every normal run, so the shipped behavior
    // (launcher.log next to the EXE, rotating to launcher.prev.log) is
    // untouched.
    if (const char* over = std::getenv("FM2K_LAUNCHER_LOG"); over && over[0]) {
        const int wn = MultiByteToWideChar(CP_UTF8, 0, over, -1, nullptr, 0);
        if (wn > 1) {
            std::wstring w(static_cast<size_t>(wn - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, over, -1, w.data(), wn);
            logpath  = w;
            prevpath = w + L".prev";
        }
    }

    // Keep the previous run around (best-effort -- ignore failure).
    MoveFileExW(logpath.c_str(), prevpath.c_str(), MOVEFILE_REPLACE_EXISTING);

    // FILE_SHARE_READ so a tester can copy/send the log while the launcher
    // still runs; FILE_SHARE_DELETE so the next run's rotate isn't blocked.
    g_file = CreateFileW(logpath.c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        return;  // read-only dir etc -- silently disabled, never fatal
    }

    char hdr[256];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = std::snprintf(hdr, sizeof(hdr),
                          "=== FM2K Rollback Launcher v%s (%s/%s) -- session "
                          "%04d-%02d-%02d %02d:%02d:%02d ===\n",
                          fm2k::kAppVersion, fm2k::kAppBranch, fm2k::kAppRevision,
                          st.wYear, st.wMonth, st.wDay,
                          st.wHour, st.wMinute, st.wSecond);
    if (n > 0) {
        if (n > static_cast<int>(sizeof(hdr))) {
            n = static_cast<int>(sizeof(hdr));
        }
        DWORD written = 0;
        WriteFile(g_file, hdr, static_cast<DWORD>(n), &written, nullptr);
    }
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void SdlLogOutput(void* /*userdata*/, int /*category*/, SDL_LogPriority priority,
                  const char* message) {
    char scrubbed[2048];
    fm2k::pii::ScrubInto(message ? message : "", scrubbed, sizeof(scrubbed));
    WriteLine(static_cast<int>(priority), scrubbed);
}

void InstallCrashHandler() {
    // SEH faults (access violation, etc.).
    g_prev_filter = SetUnhandledExceptionFilter(CrashFilter);
    // Uncaught C++ exceptions -- the SetUnhandledExceptionFilter blind spot
    // and the most likely "window shows then silently vanishes" cause.
    g_prev_terminate = std::set_terminate(TerminateHandler);
    // CRT fast-fail paths (also invisible to the SEH filter).
    _set_invalid_parameter_handler(InvalidParamHandler);
    _set_purecall_handler(PurecallHandler);
}

}  // namespace fm2k::launcher_log
