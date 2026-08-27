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
//   3. cd to app_dir, extract zip via tar.exe -xf (Win10+ ships it).
//   4. start "" FM2K_RollbackLauncher.exe, exit.

#include <windows.h>
#include <tlhelp32.h>

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
    // THEN tar extracts. If tar cannot write to app_dir the user is left with
    // no launcher at all -- an unrecoverable state produced by an update that
    // was never going to succeed. That is exactly what happens on a Program
    // Files install: this updater carries app.manifest's requestedExecutionLevel
    // so it does NOT get UAC file virtualization, and tar.exe is a manifested
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

    // tar.exe refuses to overwrite a file the OS has marked busy. The
    // launcher already exited (above), but on slower systems the EXE
    // can stay locked for a beat. We can also pre-delete the targets
    // -- Windows allows deletion of a closed EXE faster than overwrite,
    // and tar then writes fresh files. Best-effort; ignore failures.
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

    // Use Win10+'s built-in tar.exe, which understands ZIP since 17063.
    // -x extract, -f file. -C is "change dir before extracting" — same
    // semantics as GNU tar. Quote both paths in case of spaces. -v so
    // the console shows what's happening; if extraction fails the user
    // can see which file blew up.
    std::string cmd = "tar.exe -xvf \"" + zip_path + "\" -C \"" + app_dir + "\"";
    std::printf("FM2KUpdater: %s\n", cmd.c_str());
    int rc = RunCmd(cmd, "");
    if (rc != 0) {
        std::printf("FM2KUpdater: tar exited with %d — update FAILED\n", rc);
        std::printf("  Most common cause: a file in app_dir is held open\n");
        std::printf("  (game running, antivirus scanning, Explorer preview).\n");
        std::printf("  Close anything touching %s and retry.\n", app_dir.c_str());
        std::printf("  Zip is still at %s — manual fallback:\n", zip_path.c_str());
        std::printf("    tar -xf \"%s\" -C \"%s\"\n", zip_path.c_str(), app_dir.c_str());
        std::printf("\n  (press any key)\n");
        std::system("pause >nul");
        return rc;
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
