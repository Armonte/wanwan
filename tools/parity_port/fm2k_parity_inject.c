/*
 * fm2k_parity_inject.c - start a game exe suspended, load a DLL into it before its first
 * instruction, resume it and wait for it to exit (exit code passed through).
 *   fm2k_parity_inject.exe <game.exe> <absolute dll path> [game arguments]
 * The working directory is the exe's folder; the environment is inherited.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char szDir[MAX_PATH], szCmd[2048], *pSlash;
    STARTUPINFOA si;
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
    GetExitCodeProcess(pi.hProcess, &dwExit);
    printf("fm2k_parity_inject: game exited with %lu\n", dwExit);
    return (int)dwExit;
}
