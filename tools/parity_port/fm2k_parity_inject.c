/*
 * fm2k_parity_inject.c - start a game exe suspended, load a DLL into it before its first
 * instruction, resume it and wait for it to exit (exit code passed through).
 *   fm2k_parity_inject.exe <game.exe> <absolute dll path> [game arguments]
 * The working directory is the exe's folder; the environment is inherited.
 * The game runs on its own desktop "fm2kparity" of the caller's window station (created on first use),
 * so its window never shows on, or takes input from, the user's desktop (clicking it hung the original);
 * inputs come from the DLL.  FM2K_PARITY_VISIBLE=1 (run_parity.sh: PASS_ENV=FM2K_PARITY_VISIBLE) runs it
 * on the caller's desktop instead, for debugging.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char szDir[MAX_PATH], szCmd[2048], *pSlash;
    STARTUPINFOA si;
    HDESK hDesk = NULL;
    char szVis[8];
    PROCESS_INFORMATION pi;
    LPVOID pRemote;
    HANDLE hThread;
    DWORD dwRet = 0, dwExit = 0;
    size_t n;
    int i;
    if (argc < 3) {
        fprintf(stderr, "usage: %s <game.exe> <dll> [args]\n", argv[0]);
        return 2;
    }
    GetFullPathNameA(argv[1], MAX_PATH, szDir, NULL);
    snprintf(szCmd, sizeof(szCmd), "\"%s\"", szDir);
    for (i = 3; i < argc; i++) {
        strncat(szCmd, " ", sizeof(szCmd) - strlen(szCmd) - 1);
        strncat(szCmd, argv[i], sizeof(szCmd) - strlen(szCmd) - 1);
    }
    if ((pSlash = strrchr(szDir, '\\')) != NULL) *pSlash = 0;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!GetEnvironmentVariableA("FM2K_PARITY_VISIBLE", szVis, sizeof(szVis)) || szVis[0] != '1') {
        hDesk = CreateDesktopA("fm2kparity", NULL, NULL, 0, GENERIC_ALL, NULL);
        if (hDesk) si.lpDesktop = "fm2kparity";
        else fprintf(stderr, "CreateDesktop failed: %lu (running on the caller's desktop)\n", GetLastError());
    }
    if (!CreateProcessA(NULL, szCmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, szDir, &si, &pi)) {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 3;
    }
    n = strlen(argv[2]) + 1;
    pRemote = VirtualAllocEx(pi.hProcess, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(pi.hProcess, pRemote, argv[2], n, NULL);
    hThread = CreateRemoteThread(pi.hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"),
        pRemote, 0, NULL);
    if (!hThread) {
        fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        return 4;
    }
    WaitForSingleObject(hThread, INFINITE);
    GetExitCodeThread(hThread, &dwRet);
    CloseHandle(hThread);
    if (!dwRet) {
        fprintf(stderr, "LoadLibrary of %s failed in the target\n", argv[2]);
        TerminateProcess(pi.hProcess, 1);
        return 5;
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (hDesk) CloseDesktop(hDesk);
    GetExitCodeProcess(pi.hProcess, &dwExit);
    printf("fm2k_parity_inject: game exited with %lu\n", dwExit);
    return (int)dwExit;
}
