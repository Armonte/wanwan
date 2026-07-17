#include "FM2K_GameInstance.h"
#include "FM2K_DDrawRedirect.h"
#include "FM2K_GameIni.h"
#include "FM2K_Integration.h"
#include "FM2KHook/src/ui/shared_mem.h"  // FM2KSharedMemData (read-only stats from hook)
// DLL injection approach - no direct hooks needed
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <locale>
#include <codecvt>
#include <vector>
#include <windows.h>

// Save state profile removed - now using optimized FastGameState system

// FM2KSharedMemData struct now comes from FM2KHook/src/ui/shared_mem.h

namespace {

// Constants
constexpr uint32_t PROCESS_MONITOR_INTERVAL_MS = 100;
constexpr uint32_t DLL_INIT_TIMEOUT_MS = 5000;
constexpr uint32_t IPC_EVENT_TIMEOUT_MS = 100;

// Helper functions
[[maybe_unused]] static std::wstring GetDLLPath(FM2K::Engine engine = FM2K::Engine::FM2K) {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::filesystem::path exe_path(buffer);
    const wchar_t* dll_name = (engine == FM2K::Engine::FM95) ? L"FM95Hook.dll" : L"FM2KHook.dll";
    return exe_path.parent_path() / dll_name;
}

// Helper function to convert UTF-8 to wide string using Windows API
std::wstring UTF8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), wstr.data(), size_needed);
    return wstr;
}

// Build a CREATE_UNICODE_ENVIRONMENT block from the parent's current
// environment with `prepend_path_dir` spliced onto the front of PATH (with a
// `;` separator), and any `extra_vars` appended (each as a "KEY=VALUE"
// wide string). Returns the doubly-NUL-terminated wide buffer ready to
// hand to CreateProcessW. Empty vector on failure (caller falls back to
// nullptr / inherited env).
//
// Why a fresh block instead of mutating the parent: the existing flow at
// the call site sets FM2K_* env vars on the parent, calls CreateProcess,
// then unsets them. That's racy across rapid-fire dual-client launches and
// doesn't compose with the cnc-ddraw redirect (we don't want the parent's
// PATH permanently mutated). Building our own block sidesteps both. We
// snapshot the parent env *after* the FM2K_* vars are pushed in, so they
// flow through to the child unchanged.
std::vector<wchar_t> BuildEnvBlockWithPathPrepend(
    const std::wstring& prepend_path_dir,
    const std::vector<std::wstring>& extra_vars = {}) {
    LPWCH parent_env = GetEnvironmentStringsW();
    if (!parent_env) return {};

    // Walk the doubly-NUL-terminated parent block, copying each KEY=VALUE
    // entry into `out`. When we hit `PATH=...`, prepend prepend_path_dir.
    // Comparison is case-insensitive — Windows env-var names are.
    std::vector<wchar_t> out;
    bool path_seen = false;
    for (LPWCH p = parent_env; *p; ) {
        size_t len = wcslen(p);
        bool is_path = false;
        if (len >= 5) {
            // "PATH=" prefix, case-insensitive.
            wchar_t k0 = p[0], k1 = p[1], k2 = p[2], k3 = p[3];
            if ((k0 == L'P' || k0 == L'p') && (k1 == L'A' || k1 == L'a') &&
                (k2 == L'T' || k2 == L't') && (k3 == L'H' || k3 == L'h') &&
                p[4] == L'=') {
                is_path = true;
            }
        }
        if (is_path) {
            path_seen = true;
            // Emit "PATH=<prepend>;<rest_of_value>\0"
            const wchar_t kPrefix[] = L"PATH=";
            out.insert(out.end(), kPrefix, kPrefix + 5);
            out.insert(out.end(), prepend_path_dir.begin(), prepend_path_dir.end());
            // Existing value starts at p+5. Only insert ';' separator if
            // the existing PATH is non-empty (avoids a stray trailing ';').
            const wchar_t* existing = p + 5;
            if (*existing) {
                out.push_back(L';');
                out.insert(out.end(), existing, existing + (len - 5));
            }
            out.push_back(L'\0');
        } else {
            out.insert(out.end(), p, p + len + 1);  // includes the entry's NUL
        }
        p += len + 1;
    }
    FreeEnvironmentStringsW(parent_env);

    if (!path_seen) {
        // No PATH= in parent env (rare but possible): synthesize one.
        const wchar_t kPrefix[] = L"PATH=";
        out.insert(out.end(), kPrefix, kPrefix + 5);
        out.insert(out.end(), prepend_path_dir.begin(), prepend_path_dir.end());
        out.push_back(L'\0');
    }
    // Append extra KEY=VALUE entries. Caller is responsible for not
    // duplicating keys already in the parent env — we don't dedupe here
    // (CreateProcess takes the first occurrence). For our use case (a
    // launch-scoped CNC_DDRAW_CONFIG_FILE that the user almost never
    // sets in their shell), the first-wins rule is fine.
    for (const auto& kv : extra_vars) {
        out.insert(out.end(), kv.begin(), kv.end());
        out.push_back(L'\0');
    }
    out.push_back(L'\0');  // doubly-NUL-terminated
    return out;
}

} // anonymous namespace

FM2KGameInstance::FM2KGameInstance()
    : process_handle_(nullptr)
    , process_id_(0)
    , shared_memory_handle_(nullptr)
    , shared_memory_data_(nullptr)
    , last_processed_frame_(0)
{
    process_info_ = {};
}

FM2KGameInstance::~FM2KGameInstance() {
    Terminate();
}

bool FM2KGameInstance::Initialize() {
    // Initialize SDL if not already done
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0) {
        if (SDL_Init(SDL_INIT_EVENTS) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Failed to initialize SDL: %s", SDL_GetError());
            return false;
        }
    }

    // Set up logging
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
    
    // Note: IPC initialization moved to after game launch to avoid race condition
    
    return true;
}

bool FM2KGameInstance::Launch(const std::string& exe_path, FM2K::Engine engine) {
    engine_ = engine;
    // Stash for Terminate() — RestoreFromBackup needs the exe path to
    // locate the game.ini we mutated.
    game_exe_path_ = exe_path;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "FM2KGameInstance::Launch engine=%s exe=%s",
                FM2K::EngineName(engine), exe_path.c_str());
    // Use SDL3's cross-platform filesystem helpers for existence checks.
    if (!SDL_GetPathInfo(exe_path.c_str(), nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Game executable not found: %s (%s)",
            exe_path.c_str(), SDL_GetError());
        return false;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating game process in suspended state...");

    // Convert path to Windows format for CreateProcess
    std::string exe_path_win = exe_path;
    std::replace(exe_path_win.begin(), exe_path_win.end(), '/', '\\');

    // Path strings come in as UTF-8 (SDL_GlobDirectory / Windows shell drag-
    // drop / config files). Construct the filesystem::path directly from the
    // wide form so the parent_path / stem operations work on Unicode-correct
    // data — going `path(std::string)` then `parent_path().string()` on
    // MinGW round-trips through the system ANSI codepage, mangling JP
    // names like ＣＰＷ.exe into '_'-soup before we ever reach CreateProcess.
    std::wstring wide_exe_path     = UTF8ToWide(exe_path_win);
    std::filesystem::path exe_file_path(wide_exe_path);
    std::wstring wide_working_dir  = exe_file_path.parent_path().wstring();
    std::wstring wide_cmd_line     = L"\"" + wide_exe_path + L"\"";

    // /F boot-to-battle. The engine's WinMain parses /F and sets
    // g_debug_mode=3, which makes its slot-0 boot dispatcher skip
    // splash/title/CSS and jump straight into a battle-init game
    // object. The hook side populates g_iniFile_nameOverride so the
    // dispatcher's kgt loader has a path.
    //
    // FM2K-ONLY. FM95/CPW ALSO parses /F (WinMain @0x40AB60 sets
    // g_cmdline_init_flag=3 for a /F or /f arg), but there the flag
    // means editor test-play mode: the boot path expects an editor
    // process to have pre-populated the kgt filename in shared memory,
    // and the hook has no FM95 boot-to-battle prime (per_game_patches_
    // fm95's InstallBootToBattleHook is a stub). So /F on CPW derives an
    // EMPTY kgt name → "ゲーム読み込みエラー[]" (game-load error) on
    // launch. Never pass /F to an FM95 game — it boots to title normally.
    //
    // Check the per-instance environment_variables_ map FIRST (where
    // LaunchRemoteSpectator and other per-spawn-config call sites
    // store their FM2K_BOOT_TO_BATTLE=1), then fall back to the
    // launcher's own process env (where the dev checkbox sets it
    // directly via ::SetEnvironmentVariableA). The map gets applied
    // to the process env at line ~235, but that's AFTER we build
    // the cmdline here, so we must read the map for per-spawn vars.
    if (engine_ != FM2K::Engine::FM95) {
        bool boot_to_battle = false;
        auto it = environment_variables_.find("FM2K_BOOT_TO_BATTLE");
        if (it != environment_variables_.end()) {
            boot_to_battle = (it->second == "1");
        } else {
            char buf[4] = {};
            DWORD n = ::GetEnvironmentVariableA("FM2K_BOOT_TO_BATTLE",
                                                buf, sizeof(buf));
            boot_to_battle = (n > 0 && n < sizeof(buf) &&
                              std::strcmp(buf, "1") == 0);
        }
        if (boot_to_battle) {
            wide_cmd_line += L" /F";
        }
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Convert wide_working_dir back to UTF-8 just for the debug log
    // (SDL_LogDebug takes narrow). The actual CreateProcessW call below
    // uses the wide form so JP-named directories don't mangle.
    std::string working_dir_utf8;
    {
        int n = WideCharToMultiByte(CP_UTF8, 0,
                                    wide_working_dir.data(),
                                    (int)wide_working_dir.size(),
                                    nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            working_dir_utf8.assign((size_t)n, '\0');
            WideCharToMultiByte(CP_UTF8, 0,
                                wide_working_dir.data(),
                                (int)wide_working_dir.size(),
                                working_dir_utf8.data(), n,
                                nullptr, nullptr);
        }
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating process: %s", exe_path_win.c_str());
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Working directory: %s", working_dir_utf8.c_str());

    // Set environment variables in current process (child will inherit them)
    for (const auto& [name, value] : environment_variables_) {
        if (!::SetEnvironmentVariableA(name.c_str(), value.c_str())) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to set environment variable %s", name.c_str());
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Set environment variable %s=%s", name.c_str(), value.c_str());
        }
    }

    // When the cnc-ddraw redirect is active, build a private env block with
    // our cnc-ddraw dir prepended to PATH. The child process's loader walks
    // PATH when resolving the patched `2DFMD.dll` import, so this is the
    // load-bearing piece — without it the IAT rewrite produces a
    // "DLL not found" failure. The block is constructed *after* the
    // FM2K_* var injection above, so those flow through to the child too.
    const bool ddraw_redirect_enabled = FM2K::ddraw_redirect::ShouldRedirect();

    // Pin FM2K to fullscreen mode in its [GamePlay] ini before the game
    // reads it at startup. Lets cnc-ddraw own actual window-mode
    // presentation via its own `windowmode` ini key. RestoreFromBackup
    // in the launcher's StopSession path unwinds this same backup.
    if (ddraw_redirect_enabled) {
        std::filesystem::path exe_fs(UTF8ToWide(exe_path));
        if (!fm2k::game_ini::ForceFullscreenForLaunch(exe_fs)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "GameIni: ForceFullscreenForLaunch failed — game may launch windowed and fight cnc-ddraw");
        }
    }
    std::wstring cnc_ddraw_dir;
    std::vector<wchar_t> env_block;
    LPVOID lp_environment = nullptr;
    // CREATE_NO_WINDOW prevents the spawned game from inheriting (or
    // creating) a visible console window. The launcher hides its own
    // console early in SDL_AppInit, but the OS would otherwise pop a
    // fresh console window for the child since it's a console-subsystem
    // EXE inheriting our console handle. FM2K games render via DDraw/
    // D3D9; they don't need stdout, so suppressing the console entirely
    // matches user expectation. Bug v0.2.32 → fixed v0.2.33.
    DWORD  creation_flags = CREATE_SUSPENDED | CREATE_NO_WINDOW;
    if (ddraw_redirect_enabled) {
        cnc_ddraw_dir = FM2K::ddraw_redirect::ResolveCncDdrawDir();
        if (!cnc_ddraw_dir.empty()) {
            // Pin cnc-ddraw's ini path to <our_dir>\ddraw.ini via the
            // documented CNC_DDRAW_CONFIG_FILE env var (config.c:1994).
            // Without this pin, cnc-ddraw resolves the ini relative to
            // its own dll path (which is also our dir, so it'd land on
            // the same file in practice) — but if the game folder also
            // has a stray ddraw.ini from a prior manual install, the
            // fallback at config.c:2011 (".\\ddraw.ini" = CWD = game
            // folder) can grab that instead. Setting the env var is
            // the unambiguous answer and matches our "config lives next
            // to our renamed dll" model.
            std::wstring ini_path_var =
                L"CNC_DDRAW_CONFIG_FILE=" + cnc_ddraw_dir + L"\\ddraw.ini";
            std::vector<std::wstring> extras = { ini_path_var };
            env_block = BuildEnvBlockWithPathPrepend(cnc_ddraw_dir, extras);
            if (!env_block.empty()) {
                lp_environment = env_block.data();
                creation_flags |= CREATE_UNICODE_ENVIRONMENT;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "DDrawRedirect: PATH prepended with %ls; "
                    "CNC_DDRAW_CONFIG_FILE=%ls\\ddraw.ini",
                    cnc_ddraw_dir.c_str(), cnc_ddraw_dir.c_str());
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "DDrawRedirect: enabled but ResolveCncDdrawDir returned empty — falling back to inherited env, redirect will likely fail with DLL not found");
        }
    }

    if (!CreateProcessW(
        wide_exe_path.c_str(),    // Application name
        const_cast<LPWSTR>(wide_cmd_line.c_str()), // Command line
        nullptr,                   // Process handle not inheritable
        nullptr,                   // Thread handle not inheritable
        FALSE,                     // Set handle inheritance to FALSE
        creation_flags,            // CREATE_SUSPENDED + optional CREATE_UNICODE_ENVIRONMENT
        lp_environment,            // Custom env block when redirect enabled, else inherited
        wide_working_dir.c_str(), // Use game's directory as starting directory
        &si,                       // Pointer to STARTUPINFO structure
        &pi                        // Pointer to PROCESS_INFORMATION structure
    )) {
        DWORD error = GetLastError();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CreateProcess failed for %s with error: %lu",
            exe_path.c_str(), error);
        return false;
    }

    process_info_ = pi;
    process_handle_ = pi.hProcess;
    process_id_ = pi.dwProcessId;

    // Clean up environment variables to avoid conflicts with next client
    for (const auto& [name, value] : environment_variables_) {
        ::SetEnvironmentVariableA(name.c_str(), nullptr);  // Remove the variable
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Process created with ID: %lu", process_id_);

    // Patch the IAT before any user code runs — the loader hasn't resolved
    // imports yet at this point (LdrInitializeThunk runs on first
    // ResumeThread). On a successful patch, when ResumeThread fires below,
    // the loader will look for `2DFMD.dll` instead of `DDRAW.dll`, fall
    // through KnownDlls (no match), and find our renamed cnc-ddraw via the
    // PATH we prepended above. On failure (no DDRAW.dll import in this
    // EXE, or RPM/WPM error) we fall through and let the process resume
    // unmodified — the loader resolves DDRAW.dll the normal way and the
    // game runs without cnc-ddraw, which is the same as redirect-off.
    if (ddraw_redirect_enabled) {
        if (!FM2K::ddraw_redirect::RedirectImport(process_handle_)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "DDrawRedirect: patch did not apply — game will load stock ddraw.dll");
        }
    }

    // Inject hook DLL — FM2KHook.dll for FM2K, FM95Hook.dll for FM95.
    std::wstring dll_path = GetDLLPath(engine_);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using DLL path: %s (engine=%s)",
                std::string(dll_path.begin(), dll_path.end()).c_str(),
                FM2K::EngineName(engine_));
    
    if (!SetupProcessForHooking(std::string(dll_path.begin(), dll_path.end()))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to inject hook DLL");
        TerminateProcess(process_handle_, 1);
        CloseHandle(process_info_.hProcess);
        CloseHandle(process_info_.hThread);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Game process launched successfully");

    // Simple DLL injection complete - no IPC needed
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hook DLL injected successfully");

    // Note: Shared memory is no longer used - config is passed via environment variables
    // (FM2K_PLAYER_INDEX, FM2K_LOCAL_PORT, FM2K_REMOTE_ADDR, etc.)

    // If the redirect was applied, re-read the patch site one more time
    // before resuming the main thread. If the bytes have reverted between
    // RedirectImport and now (which would point at AV / runtime-hook
    // interference), this catches it. Note: the FM2KHook injection above
    // ran a remote thread which itself triggered process-init in the
    // target; if the loader-side static-import resolution happened on
    // that thread, the EXE's ddraw thunks have ALREADY been bound by
    // this point and we'd see whichever name won.
    if (ddraw_redirect_enabled) {
        std::string seen = FM2K::ddraw_redirect::VerifyPatch(process_handle_);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect: post-injection patch site reads as '%s'",
            seen.c_str());
    }

    // Resume the game process (hook DLL will start running now)
    ResumeThread(process_info_.hThread);

    return true;
}

void FM2KGameInstance::Terminate() {
    UninstallHooks();

    // Cleanup shared memory
    CleanupSharedMemory();

    if (process_handle_) {
        TerminateProcess(process_handle_, 0);
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
    }

    // Restore the pristine game.ini from any .fm2krollback_bak made by
    // ApplyForLaunch (online clamps) or ForceFullscreenForLaunch
    // (cnc-ddraw redirect). Done after the game process is killed so
    // we don't race a still-running game holding its own ini open.
    // No-op when no backup exists. Online StopSession also calls
    // RestoreFromBackup separately — second call is a no-op since
    // the backup is already consumed.
    if (!game_exe_path_.empty()) {
        std::filesystem::path exe_fs(UTF8ToWide(game_exe_path_));
        fm2k::game_ini::RestoreFromBackup(exe_fs);
    }

    if (process_info_.hThread) {
        CloseHandle(process_info_.hThread);
        process_info_.hThread = nullptr;
    }

    process_id_ = 0;
    memset(&process_info_, 0, sizeof(process_info_));
}

bool FM2KGameInstance::InstallHooks() {
    // Hooks are installed via DLL injection in SetupProcessForHooking
    // No additional action needed from launcher side
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks managed by injected DLL");
    return true;
}

bool FM2KGameInstance::UninstallHooks() {
    // Hooks will be uninstalled when DLL is unloaded (process termination)
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hooks will be uninstalled with process termination");
    return true;
}

bool FM2KGameInstance::SetupProcessForHooking(const std::string& dll_path) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Setting up process for DLL injection...");

    if (dll_path.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DLL path is empty");
        return false;
    }

    // Allocate memory in the target process for the DLL path
    SIZE_T path_size = dll_path.length() + 1;
    LPVOID remote_memory = VirtualAllocEx(process_handle_, nullptr, path_size, 
                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_memory) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "VirtualAllocEx failed: %lu", GetLastError());
        return false;
    }

    // Write the DLL path to the allocated memory
    SIZE_T bytes_written;
    if (!WriteProcessMemory(process_handle_, remote_memory, dll_path.c_str(), 
                           path_size, &bytes_written)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "WriteProcessMemory failed: %lu", GetLastError());
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    // Get LoadLibraryA address
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (!kernel32) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get kernel32 handle");
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    LPTHREAD_START_ROUTINE load_library = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryA");
    if (!load_library) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get LoadLibraryA address");
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    // Create remote thread to load the DLL
    DWORD thread_id;
    HANDLE remote_thread = CreateRemoteThread(process_handle_, nullptr, 0, 
                                             load_library, remote_memory, 0, &thread_id);
    if (!remote_thread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CreateRemoteThread failed: %lu", GetLastError());
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    // Wait for the LoadLibrary remote-thread to return. Poll in 250 ms
    // increments instead of one big WaitForSingleObject so we can give
    // a richer error message on timeout. Total budget is 30 s to absorb
    // first-load Defender scans, which can run 8-25 s on freshly-built
    // DLLs and *much* longer in OneDrive / Controlled Folder Access
    // paths. Original 5 s budget was killing legitimate inject attempts
    // on every release because each rebuild gets a new file hash and
    // Defender re-scans from scratch. Logs a progress line every 5 s so
    // a user staring at the launcher knows it's still working.
    constexpr int kInjectTimeoutMs = 30000;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Inject: waiting up to %ds for LoadLibrary in target (pid=%lu) "
        "— if this takes more than a few seconds, Defender is scanning "
        "the DLL on first load",
        kInjectTimeoutMs / 1000,
        (unsigned long)::GetProcessId(process_handle_));
    DWORD wait_result = WAIT_TIMEOUT;
    for (int slept_ms = 0; slept_ms < kInjectTimeoutMs; slept_ms += 250) {
        wait_result = WaitForSingleObject(remote_thread, 250);
        if (wait_result == WAIT_OBJECT_0) {
            if (slept_ms > 1000) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Inject: LoadLibrary completed after %.1fs (likely AV scan)",
                    (slept_ms + 250) / 1000.0);
            }
            break;
        }
        if ((slept_ms + 250) % 5000 == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Inject: still waiting on LoadLibrary (%ds elapsed of %ds)…",
                (slept_ms + 250) / 1000, kInjectTimeoutMs / 1000);
        }
        // Process still alive?
        if (WaitForSingleObject(process_handle_, 0) == WAIT_OBJECT_0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Inject: target process exited mid-injection (after %dms). "
                "Game crashed during start-up — Windows SmartScreen, antivirus "
                "(Defender, Bitdefender, Kaspersky), or DEP often kills FM2K "
                "binaries on first run. Right-click the EXE + DLL → Properties "
                "→ tick Unblock, and add an exclusion for the games folder.",
                slept_ms);
            TerminateThread(remote_thread, 1);
            CloseHandle(remote_thread);
            VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
            return false;
        }
    }
    if (wait_result != WAIT_OBJECT_0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Inject: LoadLibrary in target hung past 30s (wait_result=%lu). "
            "DllMain is stuck — most common causes:\n"
            "  1. FM2KHook.dll is BLOCKED by Windows. Right-click the DLL in "
            "Explorer → Properties → tick \"Unblock\" → OK. Re-launch.\n"
            "  2. Antivirus is scanning the DLL (Defender often does this on "
            "first load). Add an exclusion for the launcher's folder.\n"
            "  3. Missing VC++ runtime in the target process (rare for FM2K — "
            "the games are vintage and don't need modern runtimes).\n"
            "  4. FM2KHook.dll is in a path with non-ASCII characters and the "
            "ANSI LoadLibraryA we use can't open it. Move the launcher to "
            "C:\\games\\ or similar.\n"
            "DLL was: %s",
            (unsigned long)wait_result, dll_path.c_str());
        TerminateThread(remote_thread, 1);
        CloseHandle(remote_thread);
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    // Get the return value (module handle)
    DWORD exit_code;
    if (!GetExitCodeThread(remote_thread, &exit_code)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GetExitCodeThread failed: %lu", GetLastError());
        CloseHandle(remote_thread);
        VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);
        return false;
    }

    CloseHandle(remote_thread);
    VirtualFreeEx(process_handle_, remote_memory, 0, MEM_RELEASE);

    if (exit_code == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Inject: LoadLibraryA returned NULL — the DLL was rejected by "
            "Windows. Most common cause: the file is BLOCKED (Mark of the "
            "Web). Right-click FM2KHook.dll → Properties → \"Unblock\" → OK. "
            "Other possibilities: file corrupted (re-download), wrong "
            "architecture (FM2K games are 32-bit; FM2KHook.dll must be 32-bit "
            "too — it is in stock builds), or the path can't be resolved. "
            "DLL was: %s", dll_path.c_str());
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "Inject: DLL loaded (module handle 0x%lx)", exit_code);
    return true;
}

bool FM2KGameInstance::LoadGameExecutable(const std::filesystem::path& exe_path) {
    (void)exe_path; // Unused for now
    // TODO: Implement game executable loading
    return true;
}

bool FM2KGameInstance::ExecuteRemoteFunction(HANDLE process, uintptr_t function_address) {
    HANDLE thread = CreateRemoteThread(process, 
                                     nullptr, 
                                     0, 
                                     reinterpret_cast<LPTHREAD_START_ROUTINE>(function_address),
                                     nullptr, 
                                     0, 
                                     nullptr);
    
    if (!thread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to create remote thread at 0x%08X: %lu",
            function_address, GetLastError());
        return false;
    }

    // Wait for the function to complete
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return true;
}



