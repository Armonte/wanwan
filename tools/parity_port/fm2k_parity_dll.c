/*
 * fm2k_parity_dll.c - original-side host of the FM2K parity harness.
 *
 * Injected (fm2k_parity_inject.exe) into the stock KGT2nd_GAME.exe runtime (any FM2K game exe built
 * from it, e.g. DragonPuppy_version01a.orig.exe) before it runs.  It compiles the port's
 * src/parity_core.c verbatim, so the inputs and the state dump come from the same code as the port's
 * (fm2k-port branch `parity`), and detours the two points the port calls it from:
 *   0x414340 get_player_input (iTranslateKeyPress)  -> Parity_AutoplayInput when FM2K_PARITY_AUTOPLAY=1
 *   0x404CD0 update_game_state (vProcessEngineObjects) -> original, then Parity_AfterTick
 * The prologue bytes are checked first; on a different exe nothing is patched.
 *   0x405502 in vHandleDrawing (render_game), the `mov eax, giScreenMode` right after the status
 *            display and DebugLog_DrawOverlay, where the present branch starts (full screen: Lock the
 *            back surface + copy_ppvBits_to_lpSurface + Flip; window: render_frame BitBlts the DIB)
 *            -> frame_hash_record on the frame buffer ppvBits (0x4246cc) when FM2K_FRAME_HASH is set.
 *            The port calls the same function (platform/frame_hash.c, compiled in here) at the same
 *            point of its vHandleDrawing, so hash lines, frame numbering and FM2K_FRAME_DUMP bitmaps
 *            are produced by one piece of code on both sides.
 *            Also FM2K_LATENCY_LOG=<file>: the port's platform/latency_log.c format (side=orig) -- each input
 *            read's QPC time and tick, and per present a C line (newest tick, QPC) plus a P line, so
 *            fm2k-port tools/render/presentmon/bench.sh measures input-to-photon on both sides the same way.
 *            The original draws a window-mode frame with a GDI BitBlt: PresentMon sees no present of its
 *            own, so bench.sh joins these lines with dwm.exe's presents (the compositor frame that shows it).
 * No FM2KHook involvement: the game runs as shipped plus these two detours, the IAT patches of the
 * core (GlobalAlloc tracking, the one-instance check, background window) and the locale fix below.
 *
 * Locale: the original decodes its Shift-JIS file names with the system code page, so on a
 * non-Japanese locale it cannot open its demo/player files ("GameDemo Open error") and the run is
 * invalid from boot.  FM2KHook fixes that with LocaleSpoof; here the game module's 13 ANSI imports
 * that carry text are pointed at the port's own cp932_* replacements (src/cp932.c), so both sides
 * open files and read game.ini through literally the same code.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "parity.h"
#include "cp932.h"   /* the port's Shift-JIS layer (src/cp932.c), linked in */
#include "frame_hash.h"  /* the port's FM2K_FRAME_HASH / FM2K_FRAME_DUMP (platform/frame_hash.c), linked in */

#define BSS_ADDR 0x4213c0
#define BSS_END  0x541fa0
#define ADDR_RAND_SEED 0x41fb1c

static void *pResolve(uint32_t uOrigAddr) { return (void *)(uintptr_t)uOrigAddr; }

static uint32_t uLiveToOrig(uint32_t uLive)
{
    if (uLive - BSS_ADDR < BSS_END - BSS_ADDR) return uLive;
    if (uLive - ADDR_RAND_SEED < 4) return uLive;
    return 0;
}

static void (*pfnTickTramp)(void);
static int (__cdecl *pfnInputTramp)(int, int);

/* FM2K_LATENCY_LOG (all on the game's one thread): ticks completed, the open file, the frame hash switch */
static uint32_t guLatTick;
static HANDLE ghLatLog = INVALID_HANDLE_VALUE;
static int gbFrameHash;

static void vLatWrite(const char *pszLine)
{
    DWORD dw;
    if (ghLatLog != INVALID_HANDLE_VALUE)
        WriteFile(ghLatLog, pszLine, lstrlenA(pszLine), &dw, NULL);
}

static unsigned long long u64Qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (unsigned long long)t.QuadPart;
}

static void vLatOpen(void)
{
    char szPath[MAX_PATH], szLine[96];
    LARGE_INTEGER f;
    DWORD dwLen = GetEnvironmentVariableA("FM2K_LATENCY_LOG", szPath, MAX_PATH);
    if (!dwLen || dwLen >= MAX_PATH) return;
    ghLatLog = CreateFileA(szPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    QueryPerformanceFrequency(&f);
    snprintf(szLine, sizeof(szLine), "# FM2K_LATENCY_LOG 1 side=orig qpc_freq=%llu\n", (unsigned long long)f.QuadPart);
    vLatWrite(szLine);
}

/* FM2K_PARITY_SYS15_SYNC (default on when present, 0 = off): a game folder's StudioS dsound.dll ("KGT2nd_SaveExtension",
   fm2k-port engine/fm2k/sim/compat_sys15.c) serves SYS15 (gshSystemVariables[15], 0x4456CE) from its own thread every
   10 ms, so on the original the save / load and SYS15's clear land 1-3 ticks after the script wrote it, differently
   per run (torikkaku #177: fm2k-port-fleet artifacts/torikkaku_sysvar15_2026-10-22). The port does that step at a
   fixed point, after the tick and before the dump. Here: the DLL's threads are suspended once (by start address
   inside the module) and the port's step runs after every original tick, on the same in-memory save record (read
   once from the game folder's SaveData.dat, as the port reads it at boot). Robot Heroes' WSOCK32.dll (the same
   SYS15 channel, its own commands: compat_sys15.c vRobotHeroes; config_save.txt) is synced the same way: its poll
   thread also crashed the original's heap in RoHe 0.7.1 (ucrtbase free from that thread, css_picks #048). */
#define SYS15_ADDR 0x4456CE
#define SYS_VARS_ADDR 0x4456B0
static const char kStudioSHeader[] = "StudioS-SaveData-v1.0";   /* 21 bytes in the file, no terminator */
static int giSys15 = -1;              /* -1 not checked yet, 0 off, 1 StudioS synced, 2 Robot Heroes synced */
static unsigned char gabSave[4096];   /* the save file's image (compat_sys15.h COMPAT_SYS15_FILE_MAX) */
static int giSaveLen = -1;
static unsigned char gabRhBuf[30];    /* Robot Heroes: the DLL's 30-byte buffer (zeroed, as the port's) */

static int bModuleHas(HMODULE h, const char *psz)
{
    const IMAGE_DOS_HEADER *d = (const IMAGE_DOS_HEADER *)h;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)h + d->e_lfanew);
    const char *p = (const char *)h, *e = p + nt->OptionalHeader.SizeOfImage;
    size_t n = strlen(psz);
    for (; p + n <= e; p++)
        if (*p == *psz && memcmp(p, psz, n) == 0) return 1;
    return 0;
}

/* one line appended to <dump>.dll.log */
static void vDllLog(const char *pszLine)
{
    char szPath[MAX_PATH];
    DWORD dwLen = GetEnvironmentVariableA("FM2K_PARITY_DUMP_PATH", szPath, MAX_PATH - 16), dw;
    HANDLE h;
    if (!dwLen || dwLen >= MAX_PATH - 16) return;
    lstrcatA(szPath, ".dll.log");
    h = CreateFileA(szPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, pszLine, lstrlenA(pszLine), &dw, NULL);
    WriteFile(h, "\n", 1, &dw, NULL);
    CloseHandle(h);
}

/* Suspend a DLL thread only while it is parked in Sleep (its EIP in ntdll's NtDelayExecution stub): suspended
   anywhere else it may hold the process heap's or the CRT's lock, and the game's own thread then waits on it for
   good (RoHe 0.7.1's WSOCK32.dll thread writes its log through std::ostream between sleeps). Tried for up to 2 s;
   a thread never seen parked is left suspended where it is, and that is logged. */
static void vSuspendParked(HANDLE hT)
{
    uintptr_t uLo = (uintptr_t)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtDelayExecution");
    int i;
    for (i = 0; i < 2000; i++) {
        CONTEXT c;
        if (SuspendThread(hT) == (DWORD)-1) return;
        c.ContextFlags = CONTEXT_CONTROL;
        if (uLo && GetThreadContext(hT, &c) && c.Eip >= uLo && c.Eip < uLo + 0x20)
            return;
        ResumeThread(hT);
        Sleep(1);
    }
    SuspendThread(hT);
    vDllLog("sys15 sync: a DLL thread was never seen parked in Sleep; suspended where it was");
}

/* the game folder's own copy of a module (not the system's), holding psz */
static HMODULE hGameModule(const char *pszName, const char *psz)
{
    HMODULE h = GetModuleHandleA(pszName);
    char szMod[MAX_PATH], szExe[MAX_PATH], *p;
    if (!h || !GetModuleFileNameA(h, szMod, MAX_PATH) || !GetModuleFileNameA(NULL, szExe, MAX_PATH)) return NULL;
    if ((p = strrchr(szMod, '\\')) != NULL) *p = 0;
    if ((p = strrchr(szExe, '\\')) != NULL) *p = 0;
    return lstrcmpiA(szMod, szExe) == 0 && bModuleHas(h, psz) ? h : NULL;
}

static void vSys15Init(void)
{
    HMODULE h, hNt = GetModuleHandleA("ntdll.dll");
    char szEnv[8];
    const char *pszFile;
    typedef LONG (WINAPI *QitFn)(HANDLE, int, PVOID, ULONG, PULONG);
    QitFn pfnQit = hNt ? (QitFn)(void *)GetProcAddress(hNt, "NtQueryInformationThread") : NULL;
    int iKind;
    giSys15 = 0;
    if (GetEnvironmentVariableA("FM2K_PARITY_SYS15_SYNC", szEnv, sizeof(szEnv)) && szEnv[0] == '0') return;
    if ((h = hGameModule("dsound.dll", kStudioSHeader)) != NULL) {
        iKind = 1;
        pszFile = "SaveData.dat";
    } else if ((h = hGameModule("wsock32.dll", "config_save.txt")) != NULL) {
        iKind = 2;
        pszFile = "config_save.txt";
    } else {
        return;
    }
    {   /* its threads (the SYS15 poll, the F5-F7 helpers) stop */
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)h + ((const IMAGE_DOS_HEADER *)h)->e_lfanew);
        uintptr_t uLo = (uintptr_t)h, uHi = uLo + nt->OptionalHeader.SizeOfImage;
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        if (hSnap != INVALID_HANDLE_VALUE && pfnQit && Thread32First(hSnap, &te)) {
            do {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
                HANDLE hT = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                uintptr_t uStart = 0;
                if (hT && pfnQit(hT, 9 /* ThreadQuerySetWin32StartAddress */, &uStart, sizeof(uStart), NULL) == 0
                    && uStart >= uLo && uStart < uHi)
                    vSuspendParked(hT);
                if (hT) CloseHandle(hT);
            } while (Thread32Next(hSnap, &te));
        }
        if (hSnap != INVALID_HANDLE_VALUE) CloseHandle(hSnap);
    }
    {   /* the save record as the port reads it at boot */
        HANDLE hF = CreateFileA(pszFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        DWORD dw = 0;
        if (hF != INVALID_HANDLE_VALUE) {
            if (ReadFile(hF, gabSave, sizeof(gabSave), &dw, NULL)) giSaveLen = (int)dw;
            CloseHandle(hF);
        }
    }
    giSys15 = iKind;
    vDllLog(iKind == 1 ? "sys15 sync: StudioS dsound.dll" : "sys15 sync: Robot Heroes WSOCK32.dll");
}

/* compat_sys15.c vRobotHeroes, on the original's memory */
static void vRobotHeroesStep(short *pVars)
{
    unsigned uCmd = (unsigned short)pVars[15];
    int n;
    if (uCmd == 0) return;
    if (uCmd >= 3) {
        *(unsigned char *)(uintptr_t)0x43010C = (unsigned char)((uCmd & 0xff) - 3);   /* giConfigTestplayStageNb */
        return;
    }
    if (uCmd == 1) {
        memcpy(gabRhBuf, pVars, 30);
        memcpy(gabSave, gabRhBuf, 30);
        giSaveLen = 30;
    } else if (giSaveLen >= 0) {
        n = giSaveLen < 30 ? giSaveLen : 30;
        memcpy(gabRhBuf, gabSave, (size_t)n);
        memcpy(pVars, gabRhBuf, 30);
    }
    pVars[15] = 0;
}

static void vSys15Step(void)
{
    short *pVars = (short *)(uintptr_t)SYS_VARS_ADDR;
    int i;
    if (giSys15 < 0) vSys15Init();
    if (giSys15 == 2) { vRobotHeroesStep(pVars); return; }
    if (giSys15 != 1) return;
    if (pVars[15] == 1) {
        memcpy(gabSave, kStudioSHeader, 21);
        for (i = 0; i < 15; i++) {
            uint32_t dw = (uint32_t)((int)pVars[i] ^ 0x1BF52);
            memcpy(&gabSave[21 + i * 4], &dw, 4);
        }
        giSaveLen = 21 + 15 * 4;
        pVars[15] = 0;
    } else if (pVars[15] == 2) {
        if (giSaveLen >= 21 + 15 * 4)
            for (i = 0; i < 15; i++)
                pVars[i] = (short)((gabSave[21 + i * 4] | (gabSave[21 + i * 4 + 1] << 8)) ^ 0xBF52);
        pVars[15] = 0;
    }
}

static volatile LONG glTicks;          /* original ticks run (the hang watch, below) */
static volatile DWORD gdwMainThread;   /* the thread running them */
static void __cdecl hkTick(void)
{
    if (!gdwMainThread) gdwMainThread = GetCurrentThreadId();
    InterlockedIncrement(&glTicks);
    pfnTickTramp();
    vSys15Step();   /* the port's fixed point: after the tick, before the dump */
    guLatTick++;
    Parity_AfterTick();
}

static int __cdecl hkInput(int iController, int iPlayer)
{
    int iInput = Parity_AutoplayActive() ? Parity_AutoplayInput(iController, iPlayer) : pfnInputTramp(iController, iPlayer);
    if (ghLatLog != INVALID_HANDLE_VALUE) {
        char szLine[96];
        snprintf(szLine, sizeof(szLine), "I\t%llu\t%u\t%d\t%u\n", u64Qpc(), guLatTick, iController, (unsigned)iInput);
        vLatWrite(szLine);
    }
    return iInput;
}

/* frame hash / latency stamp at the present: called from the stub below with every register saved */
void __cdecl Parity_FrameHashHook(void)
{
    if (gbFrameHash)
        frame_hash_record(*(void **)0x4246cc,       /* ppvBits: the 640x480 16-bit frame buffer */
                          *(int *)0x424704,         /* giScreenMode: 0 window (RGB555 DIB), 1 full screen (RGB565) */
                          *(int *)0x470054);        /* gkgtGameState.iGameStateNumber */
    if (ghLatLog != INVALID_HANDLE_VALUE) {
        /* the frame is complete here and goes to the screen right after (BitBlt / Flip): one list per
           frame, its id the QPC time, its newest tick the last one vGameLoop ran */
        char szLine[128];
        unsigned long long t = u64Qpc();
        snprintf(szLine, sizeof(szLine), "C\t%llu\t%u\nP\t%llu\t%llu\t%llu\n", t, guLatTick - 1u, t, t, t);
        vLatWrite(szLine);
    }
}
/* replaces `mov eax, ds:[0x424704]` (5 bytes) at 0x405502 and resumes at 0x405507 */
__asm__(".globl _Parity_FrameHashStub\n"
        "_Parity_FrameHashStub:\n"
        "  pushal\n  pushfl\n  call _Parity_FrameHashHook\n  popfl\n  popal\n"
        "  movl 0x424704, %eax\n"
        "  pushl $0x405507\n  ret\n");
void Parity_FrameHashStub(void);

static void vPatchFrameHash(void)
{
    static const unsigned char kAt[5] = { 0xa1, 0x04, 0x47, 0x42, 0x00 };
    unsigned char *pT = (unsigned char *)0x405502;
    DWORD dwOld;
    char szEnv[8];
    gbFrameHash = GetEnvironmentVariableA("FM2K_FRAME_HASH", szEnv, sizeof(szEnv)) != 0;
    if ((!gbFrameHash && ghLatLog == INVALID_HANDLE_VALUE) || memcmp(pT, kAt, 5) != 0) return;
    VirtualProtect(pT, 5, PAGE_EXECUTE_READWRITE, &dwOld);
    pT[0] = 0xe9;
    *(int32_t *)(pT + 1) = (int32_t)((unsigned char *)Parity_FrameHashStub - (pT + 5));
    VirtualProtect(pT, 5, dwOld, &dwOld);
    FlushInstructionCache(GetCurrentProcess(), pT, 5);
}

/* jmp from pTarget to pNew; the nSteal prologue bytes (no relative operands) go to a trampoline */
static void *pDetour(uint32_t uTarget, const unsigned char *pExpect, int nSteal, void *pNew)
{
    unsigned char *pT = (unsigned char *)(uintptr_t)uTarget, *pTramp;
    DWORD dwOld;
    if (memcmp(pT, pExpect, nSteal) != 0) return NULL;
    pTramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(pTramp, pT, nSteal);
    pTramp[nSteal] = 0xe9;
    *(int32_t *)(pTramp + nSteal + 1) = (int32_t)((pT + nSteal) - (pTramp + nSteal + 5));
    VirtualProtect(pT, 5, PAGE_EXECUTE_READWRITE, &dwOld);
    pT[0] = 0xe9;
    *(int32_t *)(pT + 1) = (int32_t)((unsigned char *)pNew - (pT + 5));
    VirtualProtect(pT, 5, dwOld, &dwOld);
    FlushInstructionCache(GetCurrentProcess(), pT, 5);
    return pTramp;
}

/* The original's message boxes ("insert the audio CD-ROM", load errors) under the harness: never on
   screen. Each is logged to <dump>.dialogs.txt with the safest answer (Cancel / No / Ignore / OK)
   and the run goes on -- what the port's headless mode does (platform/headless.c). */
static int WINAPI hkMessageBoxA(HWND hWnd, LPCSTR pszText, LPCSTR pszCaption, UINT uType)
{
    char szPath[MAX_PATH];
    int iRet;
    DWORD dwLen = GetEnvironmentVariableA("FM2K_PARITY_DUMP_PATH", szPath, MAX_PATH - 16);
    (void)hWnd;
    switch (uType & MB_TYPEMASK) {
    case MB_OKCANCEL: case MB_RETRYCANCEL: case MB_YESNOCANCEL: iRet = IDCANCEL; break;
    case MB_YESNO: iRet = IDNO; break;
    case MB_ABORTRETRYIGNORE: iRet = IDIGNORE; break;
    default: iRet = IDOK; break;
    }
    if (dwLen && dwLen < MAX_PATH - 16) {
        HANDLE h;
        lstrcatA(szPath, ".dialogs.txt");
        h = CreateFileA(szPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD dw;
            char szLine[600];
            wsprintfA(szLine, "[%.100s] %.400s -> %d\r\n", pszCaption ? pszCaption : "", pszText ? pszText : "", iRet);
            WriteFile(h, szLine, lstrlenA(szLine), &dw, NULL);
            CloseHandle(h);
        }
    }
    return iRet;
}

/* the port's cp932.c (linked in) routes its own message boxes through msgbox_utf8 (str_cp932.c in the
   port, SDL-based); here they go to the same log as the original's */
int msgbox_utf8(const char *pszText, const char *pszCaption, UINT uType)
{
    return hkMessageBoxA(NULL, pszText, pszCaption, uType);
}


/* The original's own crash at 0x40CCAE (sprite_rendering_engine, mov ax,[edx+3]): a destroyed object drawn
   once more against a frame table freed and reloaded under it reads past the new table and faults when the
   read straddles the end of a committed page (The Favorite Fighters 2.5 at character select, tick 57).
   Players never see it: FM2KHook's VectoredRenderGuard skips that draw by jumping to the function's
   epilogue. The same here, so the original side runs on; draw only, no world state. Each recovery is
   logged to <dump>.dialogs.txt.  0x40CDEC (mov ax,[esi+3]) is the same draw reading an afterimage trail's frame
   step: a trail left in use when its file was freed keeps dangling steps (MK Immortal at tick 65). */
/* Any other crash of the original: the address goes to <dump>.dialogs.txt and the process ends without the
   Windows error report (no minidump on C:, closeout rules "ARTIFACT BUDGET"). */
/* " at <eip> (thread <id>, <module>+<off>) <ret addrs>": the module of the address and of the first 24 return
   addresses on the stack (which code it is in), appended to szLine (4096 bytes), with the line end */
static void vFormatAt(char *szLine, const CONTEXT *pc, DWORD dwThread)
{
    DWORD i, n = 0;
    char szMod[MAX_PATH];
    const DWORD *pStack = (const DWORD *)pc->Esp;
    HMODULE hm;
    wsprintfA(szLine + lstrlenA(szLine), " at 0x%08lx (thread %lu", pc->Eip, dwThread);
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc->Eip, &hm) && GetModuleFileNameA(hm, szMod, MAX_PATH))
        wsprintfA(szLine + lstrlenA(szLine), ", %s+0x%lx", szMod, pc->Eip - (DWORD)hm);
    lstrcatA(szLine, ")");
    for (i = 0; i < 2048 && n < 24 && !IsBadReadPtr(pStack + i, 4); i++) {
        if (pStack[i] < 0x10000u || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pStack[i], &hm)
            || !GetModuleFileNameA(hm, szMod, MAX_PATH))
            continue;
        {
            const char *pszBase = szMod + lstrlenA(szMod);
            while (pszBase > szMod && pszBase[-1] != '\\') pszBase--;
            wsprintfA(szLine + lstrlenA(szLine), " <%s+0x%lx", pszBase, pStack[i] - (DWORD)hm);
            n++;
        }
    }
    lstrcatA(szLine, "\r\n");
}

static void vDialogLine(const char *pszLine)
{
    char szPath[MAX_PATH];
    DWORD dwLen = GetEnvironmentVariableA("FM2K_PARITY_DUMP_PATH", szPath, MAX_PATH - 16), dw;
    HANDLE h;
    if (!dwLen || dwLen >= MAX_PATH - 16) return;
    lstrcatA(szPath, ".dialogs.txt");
    h = CreateFileA(szPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, pszLine, lstrlenA(pszLine), &dw, NULL);
    CloseHandle(h);
}

/* FM2K_PARITY_HANG_SECS (default 20, 0 = off): a game whose tick has not advanced for that long once it ticked
   (a hang, not a crash) has its main thread's place and stack written to <dump>.dialogs.txt ("[hang] ..."),
   then the process ends as a crash does: the run says where it stopped instead of only timing out */
/* the hang report runs while the main thread is suspended, maybe holding the heap or the loader lock: everything
   that could take one (the file, the module list) is done before it is suspended, the report itself only formats */
typedef struct HangMod { DWORD dwBase, dwSize; char szName[64]; } HangMod;
static HangMod gaHangMods[256];
static int giHangMods;
static const char *pszHangMod(DWORD a, DWORD *pdwOff)
{
    int i;
    for (i = 0; i < giHangMods; i++)
        if (a - gaHangMods[i].dwBase < gaHangMods[i].dwSize) { *pdwOff = a - gaHangMods[i].dwBase; return gaHangMods[i].szName; }
    return NULL;
}
static DWORD WINAPI dwHangWatch(LPVOID pv)
{
    DWORD dwSecs = (DWORD)(uintptr_t)pv, dwSame = 0;
    LONG lPrev = -1;
    HANDLE hF = INVALID_HANDLE_VALUE, hT = NULL;
    while (glTicks == 0) Sleep(100);
    {   /* at the first tick, while nothing is stuck: the report's file, the main thread's handle and the module list
           (a stuck main thread may hold the process heap's lock: CreateFile / a toolhelp snapshot would wait on it) */
        char szPath[MAX_PATH];
        DWORD dwLen = GetEnvironmentVariableA("FM2K_PARITY_DUMP_PATH", szPath, MAX_PATH - 16);
        HANDLE hSnap;
        MODULEENTRY32 me;
        if (dwLen && dwLen < MAX_PATH - 16) {
            lstrcatA(szPath, ".dialogs.txt");
            hF = CreateFileA(szPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        }
        hT = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, gdwMainThread);
        hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        me.dwSize = sizeof(me);
        if (hSnap != INVALID_HANDLE_VALUE && Module32First(hSnap, &me))
            do {
                gaHangMods[giHangMods].dwBase = (DWORD)(uintptr_t)me.modBaseAddr;
                gaHangMods[giHangMods].dwSize = me.modBaseSize;
                lstrcpynA(gaHangMods[giHangMods].szName, me.szModule, 64);
            } while (++giHangMods < 256 && Module32Next(hSnap, &me));
        if (hSnap != INVALID_HANDLE_VALUE) CloseHandle(hSnap);
    }
    for (;;) {
        Sleep(1000);
        if (glTicks != lPrev) { lPrev = glTicks; dwSame = 0; continue; }
        if (++dwSame < dwSecs) continue;
        {
            char szLine[4096];
            DWORD dw, i, n = 0, dwOff;
            CONTEXT c;
            const char *pszM;
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (hT && SuspendThread(hT) != (DWORD)-1 && GetThreadContext(hT, &c)) {
                const DWORD *pStack = (const DWORD *)c.Esp;
                wsprintfA(szLine, "[hang] no tick for %lu s after tick %ld at 0x%08lx (thread %lu", dwSecs, glTicks, c.Eip, gdwMainThread);
                if ((pszM = pszHangMod(c.Eip, &dwOff)) != NULL) wsprintfA(szLine + lstrlenA(szLine), ", %s+0x%lx", pszM, dwOff);
                lstrcatA(szLine, ")");
                for (i = 0; i < 4096 && n < 32 && !IsBadReadPtr(pStack + i, 4); i++)
                    if (pStack[i] >= 0x10000u && (pszM = pszHangMod(pStack[i], &dwOff)) != NULL) {
                        wsprintfA(szLine + lstrlenA(szLine), " <%s+0x%lx", pszM, dwOff);
                        n++;
                    }
                lstrcatA(szLine, "\r\n");
                if (hF != INVALID_HANDLE_VALUE) WriteFile(hF, szLine, lstrlenA(szLine), &dw, NULL);
            }
            if (hF != INVALID_HANDLE_VALUE) CloseHandle(hF);
            TerminateProcess(GetCurrentProcess(), 0xdead);
        }
    }
    return 0;
}

static LONG WINAPI lLastChance(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        char szLine[4096];
        wsprintfA(szLine, "[crash] exception 0x%08lx", ep->ExceptionRecord->ExceptionCode);
        vFormatAt(szLine, ep->ContextRecord, GetCurrentThreadId());
        vDialogLine(szLine);
    }
    TerminateProcess(GetCurrentProcess(), 0xdead);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI lRenderGuard(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord
        || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || (ep->ContextRecord->Eip != 0x40ccae && ep->ContextRecord->Eip != 0x40cdec))
        return EXCEPTION_CONTINUE_SEARCH;
    {
        static int n;
        char szPath[MAX_PATH];
        DWORD dwLen = GetEnvironmentVariableA("FM2K_PARITY_DUMP_PATH", szPath, MAX_PATH - 16);
        if (++n <= 3 && dwLen && dwLen < MAX_PATH - 16) {
            HANDLE h;
            lstrcatA(szPath, ".dialogs.txt");
            h = CreateFileA(szPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD dw;
                char szLine[128];
                wsprintfA(szLine, "[render guard] AV at 0x%08lx skipped (#%d, edx=0x%08lx)\r\n", ep->ContextRecord->Eip, n, ep->ContextRecord->Edx);
                WriteFile(h, szLine, lstrlenA(szLine), &dw, NULL);
                CloseHandle(h);
            }
        }
    }
    ep->ContextRecord->Eip = 0x40d7a8;   /* the epilogue */
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* MCI under the harness: the CD audio device is never opened (no disc probe, so no prompt); the
   original then plays no CD track, exactly like the port without track files */
static MCIERROR WINAPI hkMciSendCommandA(MCIDEVICEID dev, UINT uMsg, DWORD_PTR dwFlags, DWORD_PTR dwParam)
{
    (void)dev; (void)dwFlags; (void)dwParam;
    return uMsg == MCI_OPEN ? MCIERR_DEVICE_NOT_READY : 0;
}

static void vPatchCp932(HMODULE hGame)
{
    HMODULE k = GetModuleHandleA("kernel32.dll"), u = LoadLibraryA("user32.dll");
    HMODULE g = LoadLibraryA("gdi32.dll"), w = LoadLibraryA("winmm.dll");
    static const struct { HMODULE *pMod; const char *pszName; void *pfnNew; } kTab[] = {
        { NULL, "CreateFileA", (void *)cp932_CreateFileA },
        { NULL, "DeleteFileA", (void *)cp932_DeleteFileA },
        { NULL, "SetCurrentDirectoryA", (void *)cp932_SetCurrentDirectoryA },
        { NULL, "GetCurrentDirectoryA", (void *)cp932_GetCurrentDirectoryA },
        { NULL, "GetPrivateProfileIntA", (void *)cp932_GetPrivateProfileIntA },
        { NULL, "GetPrivateProfileStringA", (void *)cp932_GetPrivateProfileStringA },
        { NULL, "WritePrivateProfileStringA", (void *)cp932_WritePrivateProfileStringA },
        { NULL, "TextOutA", (void *)cp932_TextOutA },
        { NULL, "MessageBoxA", (void *)hkMessageBoxA },
        { NULL, "SetWindowTextA", (void *)cp932_SetWindowTextA },
        { NULL, "SetDlgItemTextA", (void *)cp932_SetDlgItemTextA },
        { NULL, "CreateFontA", (void *)cp932_CreateFontA },
        { NULL, "mciSendCommandA", (void *)hkMciSendCommandA },
    };
    HMODULE aMods[] = { k, k, k, k, k, k, k, g, u, u, u, g, w };
    char szLog[512];
    int i, n = 0;
    (void)kTab[0].pMod;
    for (i = 0; i < (int)(sizeof(kTab) / sizeof(kTab[0])); i++) {
        void *pReal = (void *)GetProcAddress(aMods[i], kTab[i].pszName);
        if (pReal && Parity_PatchIat(hGame, pReal, kTab[i].pfnNew)) n++;
    }
    wsprintfA(szLog, "fm2k_parity: cp932 imports patched: %d of 13\n", n);
    OutputDebugStringA(szLog);
    {
        char szPath[MAX_PATH];
        DWORD dwLen = GetEnvironmentVariableA("FM2K_PARITY_DUMP_PATH", szPath, MAX_PATH - 16);
        if (dwLen && dwLen < MAX_PATH - 16) {
            HANDLE h;
            lstrcatA(szPath, ".dll.log");
            h = CreateFileA(szPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD dw;
                WriteFile(h, szLog, lstrlenA(szLog), &dw, NULL);
                CloseHandle(h);
            }
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD dwReason, LPVOID pReserved)
{
    static const unsigned char kTick[9] = { 0x53, 0x33, 0xdb, 0x89, 0x1d, 0xfc, 0x46, 0x42, 0x00 };
    static const unsigned char kInput[5] = { 0xa1, 0x54, 0x00, 0x47, 0x00 };
    ParityHost host;
    (void)hDll; (void)pReserved;
    if (dwReason != DLL_PROCESS_ATTACH) return TRUE;
    host.pszSide = "orig";
    host.pfnResolve = pResolve;
    host.pfnLiveToOrig = uLiveToOrig;
    host.hGameModule = GetModuleHandleA(NULL);
    if (memcmp((void *)0x404cd0, kTick, 9) != 0 || memcmp((void *)0x414340, kInput, 5) != 0) {
        OutputDebugStringA("fm2k_parity: not the KGT2nd_GAME.exe build this harness knows, nothing patched\n");
        return TRUE;
    }
    if (!Parity_Init(&host)) return TRUE;
    vPatchCp932(host.hGameModule);
    pfnTickTramp = (void (*)(void))pDetour(0x404cd0, kTick, 9, (void *)hkTick);
    pfnInputTramp = (int (__cdecl *)(int, int))pDetour(0x414340, kInput, 5, (void *)hkInput);
    vLatOpen();
    vPatchFrameHash();
    AddVectoredExceptionHandler(1, lRenderGuard);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(lLastChance);
    {
        char szEnv[16];
        DWORD dwSecs = GetEnvironmentVariableA("FM2K_PARITY_HANG_SECS", szEnv, sizeof(szEnv)) ? (DWORD)atoi(szEnv) : 20u;
        if (dwSecs) CloseHandle(CreateThread(NULL, 0, dwHangWatch, (LPVOID)(uintptr_t)dwSecs, 0, NULL));
    }
    return TRUE;
}
