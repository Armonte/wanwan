// FM2KUpdater.exe — companion EXE that performs the in-place update
// after the launcher hands it off and exits. Same pattern CCCaster uses
// (tools/Updater.cpp): a tiny separate binary so the running launcher
// can be replaced.
//
// Args (positional):
//   argv[1]   parent PID — we WaitForSingleObject on this so we don't
//             try to overwrite the launcher EXE while it's still loaded.
//   argv[2]   app dir — where FM2K_RollbackLauncher.exe + FM2KHook.dll
//             live, also our extraction target.
//   argv[3]   zip path — full path to the downloaded fm2k_v<ver>.zip
//             in %TEMP%.
//
// Strategy:
//   1. Wait for parent to exit (5s timeout, then TerminateProcess as a
//      last resort).
//   2. Also kill any FM2KHook-injected FM2K processes — the DLL holds
//      a handle to FM2KHook.dll on disk, so a running game blocks
//      replacement.
//   3. Extract the zip into app_dir in-process with miniz. (This used to
//      shell out to tar.exe, which only exists on Windows 10 1803+; on
//      Windows 7 the update deleted the launcher and then failed.)
//   4. start "" FM2K_RollbackLauncher.exe, exit.

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <tlhelp32.h>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// All-in-one terminate helper. Returns true if the process is gone.
bool WaitOrKill(DWORD pid, DWORD wait_ms) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
                           FALSE, pid);
    if (!h) return true;  // already gone, or no permission — best effort
    DWORD r = WaitForSingleObject(h, wait_ms);
    if (r != WAIT_OBJECT_0) {
        std::printf("FM2KUpdater: parent pid %lu didn't exit in %lums — killing\n",
                    (unsigned long)pid, (unsigned long)wait_ms);
        TerminateProcess(h, 1);
        WaitForSingleObject(h, 2000);
    }
    CloseHandle(h);
    return true;
}

// Walk the process list looking for FM2K-side processes that might
// have FM2KHook.dll loaded. Heuristic: FM2K processes are 32-bit
// processes with FM2KHook.dll in their module list. tlhelp32 gives
// us PIDs cheaply; module enumeration confirms.
void KillFM2KGames() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    if (!Process32First(snap, &pe)) {
        CloseHandle(snap);
        return;
    }

    do {
        // Quick name filter — most FM2K hooks live in processes named
        // something other than FM2K_RollbackLauncher.exe. We don't want
        // to nuke unrelated processes; only ones that have our DLL
        // loaded.
        if (_stricmp(pe.szExeFile, "FM2K_RollbackLauncher.exe") == 0) continue;
        if (_stricmp(pe.szExeFile, "FM2KUpdater.exe") == 0) continue;

        HANDLE mods = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pe.th32ProcessID);
        if (mods == INVALID_HANDLE_VALUE) continue;

        MODULEENTRY32 me = {};
        me.dwSize = sizeof(me);
        bool has_our_dll = false;
        if (Module32First(mods, &me)) {
            do {
                if (_stricmp(me.szModule, "FM2KHook.dll") == 0) {
                    has_our_dll = true;
                    break;
                }
            } while (Module32Next(mods, &me));
        }
        CloseHandle(mods);

        if (has_our_dll) {
            std::printf("FM2KUpdater: terminating %s (pid %lu) — has FM2KHook loaded\n",
                        pe.szExeFile, (unsigned long)pe.th32ProcessID);
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,
                                   FALSE, pe.th32ProcessID);
            if (h) {
                TerminateProcess(h, 1);
                WaitForSingleObject(h, 2000);
                CloseHandle(h);
            }
        }
    } while (Process32Next(snap, &pe));

    CloseHandle(snap);
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// Rejects absolute paths, drive letters and ".." so a hostile archive cannot
// write outside app_dir. (Release zips are signature-checked before we get
// here, but the extractor should not depend on that.)
bool SafeRelative(const std::wstring& rel) {
    if (rel.empty() || rel[0] == L'\\' || rel[0] == L'/') return false;
    if (rel.find(L':') != std::wstring::npos) return false;
    size_t start = 0;
    while (start <= rel.size()) {
        size_t p = rel.find_first_of(L"\\/", start);
        if (p == std::wstring::npos) p = rel.size();
        if (rel.compare(start, p - start, L"..") == 0) return false;
        start = p + 1;
    }
    return true;
}

bool MakeDirs(const std::wstring& dir) {
    for (size_t i = 0; i < dir.size(); ++i) {
        if ((dir[i] == L'\\' || dir[i] == L'/') && i > 2) {
            CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
        }
    }
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// Extracts every entry of `zip` into `dest`. Returns 0 on success, else the
// number of entries that failed (or -1 if the archive could not be opened).
int ExtractZip(const std::wstring& zip, const std::wstring& dest) {
    HANDLE h = CreateFileW(zip.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("FM2KUpdater: cannot open the update zip (error %lu)\n",
                    (unsigned long)GetLastError());
        return -1;
    }
    LARGE_INTEGER size;
    std::vector<unsigned char> data;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1LL << 30)) {
        data.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr) || got != data.size()) {
            data.clear();
        }
    }
    CloseHandle(h);
    if (data.empty()) {
        std::printf("FM2KUpdater: could not read the update zip\n");
        return -1;
    }

    mz_zip_archive za;
    std::memset(&za, 0, sizeof(za));
    if (!mz_zip_reader_init_mem(&za, data.data(), data.size(), 0)) {
        std::printf("FM2KUpdater: the update zip is corrupt\n");
        return -1;
    }
    int failures = 0;
    const mz_uint count = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) { ++failures; continue; }
        const std::wstring rel = Utf8ToWide(st.m_filename);
        if (!SafeRelative(rel)) {
            std::printf("FM2KUpdater: skipping unsafe path %s\n", st.m_filename);
            ++failures;
            continue;
        }
        std::wstring out = dest + L"\\" + rel;
        for (wchar_t& c : out) if (c == L'/') c = L'\\';
        if (mz_zip_reader_is_file_a_directory(&za, i)) {
            if (!MakeDirs(out)) ++failures;
            continue;
        }
        const size_t slash = out.find_last_of(L'\\');
        if (slash != std::wstring::npos) MakeDirs(out.substr(0, slash));

        size_t len = 0;
        void* buf = mz_zip_reader_extract_to_heap(&za, i, &len, 0);
        if (!buf) {
            std::printf("FM2KUpdater: x %s (decompress failed)\n", st.m_filename);
            ++failures;
            continue;
        }
        HANDLE f = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD wrote = 0;
        const bool ok = f != INVALID_HANDLE_VALUE &&
                        WriteFile(f, buf, (DWORD)len, &wrote, nullptr) && wrote == len;
        const DWORD err = ok ? 0 : GetLastError();
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        mz_free(buf);
        if (ok) {
            std::printf("FM2KUpdater: x %s\n", st.m_filename);
        } else {
            std::printf("FM2KUpdater: x %s FAILED (error %lu)\n", st.m_filename,
                        (unsigned long)err);
            ++failures;
        }
    }
    mz_zip_reader_end(&za);
    return failures;
}

int RunCmd(const std::string& cmdline, const std::string& cwd) {
    STARTUPINFOA        si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;

    // CreateProcess wants a writable cmdline buffer.
    std::vector<char> mut(cmdline.begin(), cmdline.end());
    mut.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, mut.data(),
                             nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(),
                             &si, &pi);
    if (!ok) {
        std::printf("FM2KUpdater: CreateProcess failed (%lu): %s\n",
                    (unsigned long)GetLastError(), cmdline.c_str());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::printf("FM2KUpdater: usage: FM2KUpdater.exe <parent_pid> <app_dir> <zip_path>\n");
        std::printf("  (this binary is invoked by the launcher — don't run it manually)\n");
        Sleep(3000);
        return 2;
    }
    const DWORD       parent_pid = (DWORD)std::strtoul(argv[1], nullptr, 10);
    const std::string app_dir    = argv[2];
    const std::string zip_path   = argv[3];
    // The narrow argv is in the ANSI code page and cannot represent every
    // install path; extraction uses the UTF-16 originals.
    std::wstring app_dir_w, zip_path_w;
    {
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv && wargc >= 4) {
            app_dir_w  = wargv[2];
            zip_path_w = wargv[3];
        }
        if (wargv) LocalFree(wargv);
        if (app_dir_w.empty()) {   // cannot happen in practice; ANSI is a lossy fallback
            for (const char* c = argv[2]; *c; ++c) app_dir_w += (wchar_t)(unsigned char)*c;
            for (const char* c = argv[3]; *c; ++c) zip_path_w += (wchar_t)(unsigned char)*c;
        }
    }

    std::printf("FM2KUpdater: applying %s -> %s\n", zip_path.c_str(), app_dir.c_str());

    WaitOrKill(parent_pid, 5000);
    KillFM2KGames();

    // Tiny extra grace so any in-flight file handles flush. NTFS
    // sometimes refuses to overwrite a freshly-closed EXE for a few
    // hundred ms after the holder exits.
    Sleep(400);

    // WRITABILITY PRE-FLIGHT -- must run BEFORE the pre-delete below.
    //
    // The pre-delete removes the launcher, the hook and this updater, and
    // THEN we extract. If we cannot write to app_dir the user is left with
    // no launcher at all -- an unrecoverable state produced by an update that
    // was never going to succeed. That is exactly what happens on a Program
    // Files install: this updater carries app.manifest's requestedExecutionLevel
    // so it does NOT get UAC file virtualization, and tar.exe (the old extractor) was a manifested
    // 64-bit Microsoft binary that does not virtualize either, so both hit a
    // hard ACCESS_DENIED with no VirtualStore fallback.
    //
    // Probe first, refuse early, delete nothing. A failed update must leave the
    // install exactly as it found it.
    {
        const std::string probe = app_dir + "\\.fm2k_update_probe";
        HANDLE h = CreateFileA(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            std::printf("FM2KUpdater: cannot write to \"%s\" (error %lu) -- "
                        "update REFUSED, nothing was deleted.\n",
                        app_dir.c_str(), (unsigned long)err);
            if (err == ERROR_ACCESS_DENIED) {
                std::printf("  This install directory is not writable by you.\n");
                std::printf("  Most common cause: the app lives under "
                            "C:\\Program Files. Move the install to a normal\n");
                std::printf("  folder such as C:\\games\\ and update again.\n");
                std::printf("  Do NOT run as administrator -- it breaks Discord "
                            "sign-in and hides your existing replays.\n");
            }
            std::printf("FM2KUpdater: press Enter to close.\n");
            (void)std::getchar();
            return 1;
        }
        CloseHandle(h);
        DeleteFileA(probe.c_str());
    }

    // Overwriting a file the OS still has marked busy fails. The launcher
    // already exited (above), but on slower systems the EXE can stay locked
    // for a beat. Pre-delete the targets -- Windows allows deletion of a
    // closed EXE faster than overwrite, and extraction then writes fresh
    // files. Best-effort; ignore failures.
    {
        const char* victims[] = {
            "FM2K_RollbackLauncher.exe",
            "FM2KHook.dll",
            "FM2KUpdater.exe",
        };
        for (const char* v : victims) {
            std::string p = app_dir + "\\" + v;
            DeleteFileA(p.c_str());
        }
    }

    // In-process extraction (miniz): works on every Windows version, unlike
    // tar.exe (Windows 10 1803+ only). -v style listing so the console shows
    // which file failed if one does.
    const int failed = ExtractZip(zip_path_w, app_dir_w);
    if (failed != 0) {
        std::printf("FM2KUpdater: extraction FAILED (%d problem(s)) -- update incomplete\n",
                    failed);
        std::printf("  Most common cause: a file in app_dir is held open\n");
        std::printf("  (game running, antivirus scanning, Explorer preview).\n");
        std::printf("  Close anything touching %s and retry.\n", app_dir.c_str());
        std::printf("  Zip is still at %s -- manual fallback:\n", zip_path.c_str());
        std::printf("    open it in Explorer and copy everything into the folder above.\n");
        std::printf("\n  (press any key)\n");
        std::system("pause >nul");
        return 1;
    }

    // Done — relaunch the (now updated) launcher and exit.
    const std::string launcher = app_dir + "\\FM2K_RollbackLauncher.exe";
    std::string relaunch = "cmd /C start \"\" \"" + launcher + "\"";
    std::printf("FM2KUpdater: relaunching %s\n", launcher.c_str());
    RunCmd(relaunch, app_dir);

    // Try to delete the zip. Best-effort; %TEMP% gets swept anyway.
    DeleteFileA(zip_path.c_str());

    return 0;
}
