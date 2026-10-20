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
#include <stdio.h>
#include <stdint.h>
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

static void __cdecl hkTick(void)
{
    pfnTickTramp();
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
   logged to <dump>.dialogs.txt. */
static LONG WINAPI lRenderGuard(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord
        || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || ep->ContextRecord->Eip != 0x40ccae)
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
                wsprintfA(szLine, "[render guard] AV at 0x40ccae skipped (#%d, edx=0x%08lx)\r\n", n, ep->ContextRecord->Edx);
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
    return TRUE;
}
