#pragma once

// FM2K_DDrawRedirect -- patch the suspended target's IAT so its static
// `DDRAW.dll` import resolves to a non-KnownDll name we control. This
// lets us ship cnc-ddraw inside our launcher tree (renamed) instead of
// staging a real `ddraw.dll` next to the game exe.
//
// Why this is necessary:
//   `ddraw.dll` is a KnownDll. The Windows loader binds KnownDlls from
//   the kernel-side section table, which beats `SetDllDirectory`, the
//   PATH env var, and any `LoadLibrary` we do post-creation. The only
//   loader rule that supersedes KnownDlls is "DLL of the same name in
//   the application directory" -- which would mean writing `ddraw.dll`
//   into the FM2K game folder. Renaming the import in the IAT sidesteps
//   the whole KnownDll path: a name like `2DFMD.dll` isn't a KnownDll,
//   so the loader falls through to the standard search order and
//   honours the env-block PATH we hand to CreateProcess.
//
// Caveat: cnc-ddraw self-checks the host's IAT for the literal string
// `"ddraw.dll"` in two places (hook.c hook_got_ddraw_import + utils.c
// util_caller_is_ddraw_wrapper). With the import renamed those checks
// return FALSE, which means cnc-ddraw stays in `hook=4` mode instead of
// switching to `hook=3`, and skips its chained-wrapper detection. Both
// paths still work for our integration; documented here so the next
// reader doesn't think it's a bug.

#include <windows.h>
#include <string>

namespace FM2K::ddraw_redirect {

// Runtime toggle, default off. Set by the launcher UI's debug checkbox or
// programmatically by tests. The env var `FM2K_TEST_IAT_REWRITE=1` forces
// it on regardless. Read by `FM2KGameInstance::Launch` to decide whether
// to call `RedirectImport` between CreateProcess(SUSPENDED) and ResumeThread.
void SetForceRedirect(bool enabled);
bool GetForceRedirect();

// True if the redirect path should run for the next Launch -- i.e. either
// the UI toggle is on, or the FM2K_TEST_IAT_REWRITE env var is set to "1".
bool ShouldRedirect();

// Resolve the directory that holds our renamed cnc-ddraw `2DFMD.dll`.
// Order:
//   1. `FM2K_DDRAW_DIR` env var (for shell-driven testing).
//   2. `<launcher_exe_dir>\cnc-ddraw\` (the production location).
// Returns a wide path with no trailing separator. Empty string on failure
// (e.g. GetModuleFileName failed). The folder is not created here -- that's
// the downloader's job.
std::wstring ResolveCncDdrawDir();

// Patches the `DDRAW.dll` (case-insensitive) import name in `process`'s
// in-memory PE image so the loader resolves it as `new_name` instead.
//
// Constraints:
//   - Must be called between CreateProcess(CREATE_SUSPENDED) and
//     ResumeThread. After that, LdrInitializeThunk has already read
//     the original name and bound the IAT.
//   - `new_name` plus its trailing NUL must fit in 10 bytes (the
//     original `DDRAW.dll\0` slot). Default `2DFMD.dll` (10 bytes
//     incl. NUL) satisfies this.
//   - `process` must be a 32-bit (WOW64 or native i386) process --
//     FM2K and FM95 are both i686. Behavior on a 64-bit target is
//     undefined; we read IMAGE_NT_HEADERS32.
//
// Returns true on a successful patch, false if the import descriptor
// table contains no DDRAW.dll entry (the EXE doesn't statically link
// it) or if any RPM/WPM step fails. On false the process is left
// untouched and the caller can still ResumeThread -- the loader will
// just resolve the original DDRAW.dll via KnownDlls as it would have
// without us.
bool RedirectImport(HANDLE process, const char* new_name = "2DFMD.dll");

// Re-read the IMPORT descriptor's name string after the fact, log it,
// and return what was observed. Used to verify the patch persisted past
// FM2KHook injection (some AVs revert .rdata writes asynchronously).
// The returned string is whatever the loader will see when it walks
// the IAT -- empty on read failure.
std::string VerifyPatch(HANDLE process);

}  // namespace FM2K::ddraw_redirect
