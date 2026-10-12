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

static void __cdecl hkTick(void)
{
    pfnTickTramp();
    Parity_AfterTick();
}

static int __cdecl hkInput(int iController, int iPlayer)
{
    if (Parity_AutoplayActive())
        return Parity_AutoplayInput(iController, iPlayer);
    return pfnInputTramp(iController, iPlayer);
}

/* frame hash at the present: called from the stub below with every register saved */
void __cdecl Parity_FrameHashHook(void)
{
    frame_hash_record(*(void **)0x4246cc,       /* ppvBits: the 640x480 16-bit frame buffer */
                      *(int *)0x424704,         /* giScreenMode: 0 window (RGB555 DIB), 1 full screen (RGB565) */
                      *(int *)0x470054);        /* gkgtGameState.iGameStateNumber */
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
    if (!GetEnvironmentVariableA("FM2K_FRAME_HASH", szEnv, sizeof(szEnv)) || memcmp(pT, kAt, 5) != 0) return;
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
        { NULL, "MessageBoxA", (void *)cp932_MessageBoxA },
        { NULL, "SetWindowTextA", (void *)cp932_SetWindowTextA },
        { NULL, "SetDlgItemTextA", (void *)cp932_SetDlgItemTextA },
        { NULL, "CreateFontA", (void *)cp932_CreateFontA },
        { NULL, "mciSendCommandA", (void *)cp932_mciSendCommandA },
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
    vPatchFrameHash();
    return TRUE;
}
