// Locale spoofing for Japanese-only 2DFM titles (CPW.exe / FM95-era games).
//
// Trace results from CPW.exe IDA + iteration on real boot:
//   GetACP / GetOEMCP / GetCPInfo / MultiByteToWideChar / WideCharToMultiByte
//     -- the CRT codepage path. Hook returns CP932 / translates CP_ACP -> 932.
//   GetUserDefaultLCID / GetSystemDefaultLCID / GetThreadLocale /
//     GetUserDefaultUILanguage / GetSystemDefaultUILanguage
//     -- LCID resolution path used by GUI APIs and the CRT. Return 0x0411 (ja-JP).
//   IsDBCSLeadByte / IsDBCSLeadByteEx
//     -- character iteration; return TRUE for SJIS lead bytes (0x81-9F, 0xE0-FC).
//   IsValidCodePage / IsValidLocale
//     -- always TRUE (game probes the codepage before using it).
//   SetWindowTextA
//     -- window-title rendering: convert SJIS bytes to wide via CP932, call
//        SetWindowTextW directly. Without this, the system-ANSI codepage (1252
//        on US Windows) is used to interpret SJIS bytes -> mojibake titlebar.
//
// This independently reimplements the locale-spoofing TECHNIQUE (which Win32
// APIs to intercept and what each must return for a CP932 / ja-JP environment)
// in C++. The API set was derived from our own IDA traces of CPW.exe (above)
// plus general knowledge of how locale shims work. Locale-Emulator
// (https://github.com/xupefei/Locale-Emulator) was a conceptual reference for
// WHICH APIs matter, but no LE code is copied, translated, or linked -- LE is
// C#/GPL; this is original C#-free C++. We add more APIs as games need them.
//
// Hook timing: must run BEFORE the host CRT initializes its locale cache. We
// inject pre-ResumeThread (CreateProcess with CREATE_SUSPENDED + LoadLibrary
// remote thread), so DllMain in the LoadLibrary thread runs before the host's
// main thread starts executing — that's the right window.

#include "locale_spoof.h"

#include "MinHook.h"
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>

namespace {

constexpr UINT   kSpoofedCodePage = 932;       // Shift-JIS / CP932
constexpr LANGID kSpoofedLangID   = 0x0411;    // Japanese (ja-JP)
constexpr LCID   kSpoofedLCID     = MAKELCID(kSpoofedLangID, SORT_DEFAULT);

// CRT codepage path
using GetACP_t              = UINT  (WINAPI*)(void);
using GetOEMCP_t            = UINT  (WINAPI*)(void);
using GetCPInfo_t           = BOOL  (WINAPI*)(UINT, LPCPINFO);
using MultiByteToWideChar_t = int   (WINAPI*)(UINT, DWORD, LPCCH, int, LPWSTR, int);
using WideCharToMultiByte_t = int   (WINAPI*)(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);

// LCID / language path
using GetUserDefaultLCID_t        = LCID   (WINAPI*)(void);
using GetSystemDefaultLCID_t      = LCID   (WINAPI*)(void);
using GetUserDefaultUILanguage_t  = LANGID (WINAPI*)(void);
using GetSystemDefaultUILanguage_t= LANGID (WINAPI*)(void);
using GetThreadLocale_t           = LCID   (WINAPI*)(void);

// DBCS / validity
using IsDBCSLeadByte_t       = BOOL (WINAPI*)(BYTE);
using IsDBCSLeadByteEx_t     = BOOL (WINAPI*)(UINT, BYTE);
using IsValidCodePage_t      = BOOL (WINAPI*)(UINT);
using IsValidLocale_t        = BOOL (WINAPI*)(LCID, DWORD);

// USER32 + GDI32 — ANSI rendering / windowing surfaces we redirect to W.
using SetWindowTextA_t   = BOOL    (WINAPI*)(HWND, LPCSTR);
using TextOutA_t         = BOOL    (WINAPI*)(HDC, int, int, LPCSTR, int);
using MessageBoxA_t      = int     (WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
using SetDlgItemTextA_t  = BOOL    (WINAPI*)(HWND, int, LPCSTR);
using CreateWindowExA_t  = HWND    (WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD,
                                             int, int, int, int,
                                             HWND, HMENU, HINSTANCE, LPVOID);
using DialogBoxParamA_t  = INT_PTR (WINAPI*)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);

// kernel32 path APIs — fix Windows' default best-fit char conversion
// (which collapses fullwidth Ｃ→C etc.) by routing through W variants and
// converting via CP932 ourselves.
using GetModuleFileNameA_t   = DWORD (WINAPI*)(HMODULE, LPSTR, DWORD);
using GetCurrentDirectoryA_t = DWORD (WINAPI*)(DWORD, LPSTR);
using GetFullPathNameA_t     = DWORD (WINAPI*)(LPCSTR, DWORD, LPSTR, LPSTR*);
using GetCommandLineA_t      = LPSTR (WINAPI*)(void);
// File-OPEN side: the game passes narrow paths (SJIS under our 932 spoof) to
// these; the kernel's ANSI thunk converts them back with the REAL system ACP
// (1252 on a US box), NOT our hooked GetACP — so kanji in the install DIRECTORY
// path (D:\games\fm2k\闘闘\...) is mangled and the open fails. Convert the SJIS
// bytes ourselves via CP932 and call the W variant. (The path-RETRIEVAL hooks
// above only fixed the game GETTING its path; these fix it USING the path.)
using CreateFileA_t          = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using FindFirstFileA_t       = HANDLE (WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using FindNextFileA_t        = BOOL   (WINAPI*)(HANDLE, LPWIN32_FIND_DATAA);
// INI/profile READ+WRITE side: FM2K reads GameScreenMode / key configs / net
// names via GetPrivateProfile*A, building the ini path from the (now CP932)
// GetCurrentDirectoryA buffer. The kernel's ANSI thunk re-converts that path
// with the REAL system ACP (1252 on a US box), NOT our hooked GetACP — so the
// SJIS install DIRECTORY mangles, the ini isn't found, and GameScreenMode
// falls back to its default 0 (GDI mode). The game then never calls
// DirectDrawCreate, so cnc-ddraw can't engage and F4 fullscreen is dead.
// Convert the path (and section/key/value tokens) via CP932 and call the W
// variant, exactly as on a JP-locale system.
using GetPrivateProfileIntA_t      = UINT  (WINAPI*)(LPCSTR, LPCSTR, INT, LPCSTR);
using GetPrivateProfileStringA_t   = DWORD (WINAPI*)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
using WritePrivateProfileStringA_t = BOOL  (WINAPI*)(LPCSTR, LPCSTR, LPCSTR, LPCSTR);

// Trampolines
GetACP_t                     p_GetACP                    = nullptr;
GetOEMCP_t                   p_GetOEMCP                  = nullptr;
GetCPInfo_t                  p_GetCPInfo                 = nullptr;
MultiByteToWideChar_t        p_MultiByteToWideChar       = nullptr;
WideCharToMultiByte_t        p_WideCharToMultiByte       = nullptr;
GetUserDefaultLCID_t         p_GetUserDefaultLCID        = nullptr;
GetSystemDefaultLCID_t       p_GetSystemDefaultLCID      = nullptr;
GetUserDefaultUILanguage_t   p_GetUserDefaultUILanguage  = nullptr;
GetSystemDefaultUILanguage_t p_GetSystemDefaultUILanguage= nullptr;
GetThreadLocale_t            p_GetThreadLocale           = nullptr;
IsDBCSLeadByte_t             p_IsDBCSLeadByte            = nullptr;
IsDBCSLeadByteEx_t           p_IsDBCSLeadByteEx          = nullptr;
IsValidCodePage_t            p_IsValidCodePage           = nullptr;
IsValidLocale_t              p_IsValidLocale             = nullptr;
SetWindowTextA_t             p_SetWindowTextA            = nullptr;
TextOutA_t                   p_TextOutA                  = nullptr;
MessageBoxA_t                p_MessageBoxA               = nullptr;
SetDlgItemTextA_t            p_SetDlgItemTextA           = nullptr;
CreateWindowExA_t            p_CreateWindowExA           = nullptr;
DialogBoxParamA_t            p_DialogBoxParamA           = nullptr;
GetModuleFileNameA_t         p_GetModuleFileNameA        = nullptr;
GetCurrentDirectoryA_t       p_GetCurrentDirectoryA      = nullptr;
GetFullPathNameA_t           p_GetFullPathNameA          = nullptr;
GetCommandLineA_t            p_GetCommandLineA           = nullptr;
CreateFileA_t                p_CreateFileA               = nullptr;
FindFirstFileA_t             p_FindFirstFileA            = nullptr;
FindNextFileA_t              p_FindNextFileA             = nullptr;
GetPrivateProfileIntA_t      p_GetPrivateProfileIntA     = nullptr;
GetPrivateProfileStringA_t   p_GetPrivateProfileStringA  = nullptr;
WritePrivateProfileStringA_t p_WritePrivateProfileStringA= nullptr;

bool g_installed = false;

// Forward decl — installs a wrapper WNDPROC on a freshly-created window so
// the OS treats it as Unicode and SetWindowTextW preserves wide titles.
// Defined further down (after the dialog-template translator).
void InstallLocaleProcOnWindow(HWND hwnd) noexcept;

inline UINT TranslateCodePage(UINT cp) noexcept {
    if (cp == CP_ACP || cp == CP_OEMCP) return kSpoofedCodePage;
    return cp;
}

// --- CRT codepage hooks ---
UINT WINAPI Hook_GetACP(void)   { return kSpoofedCodePage; }
UINT WINAPI Hook_GetOEMCP(void) { return kSpoofedCodePage; }

BOOL WINAPI Hook_GetCPInfo(UINT cp, LPCPINFO info) {
    return p_GetCPInfo(TranslateCodePage(cp), info);
}

int WINAPI Hook_MultiByteToWideChar(UINT cp, DWORD flags,
                                    LPCCH src, int src_len,
                                    LPWSTR dst, int dst_len) {
    return p_MultiByteToWideChar(TranslateCodePage(cp), flags,
                                 src, src_len, dst, dst_len);
}

int WINAPI Hook_WideCharToMultiByte(UINT cp, DWORD flags,
                                    LPCWCH src, int src_len,
                                    LPSTR dst, int dst_len,
                                    LPCCH default_char, LPBOOL used_default) {
    return p_WideCharToMultiByte(TranslateCodePage(cp), flags,
                                 src, src_len, dst, dst_len,
                                 default_char, used_default);
}

// --- LCID / language hooks ---
LCID   WINAPI Hook_GetUserDefaultLCID(void)         { return kSpoofedLCID; }
LCID   WINAPI Hook_GetSystemDefaultLCID(void)       { return kSpoofedLCID; }
LANGID WINAPI Hook_GetUserDefaultUILanguage(void)   { return kSpoofedLangID; }
LANGID WINAPI Hook_GetSystemDefaultUILanguage(void) { return kSpoofedLangID; }
LCID   WINAPI Hook_GetThreadLocale(void)            { return kSpoofedLCID; }

// --- DBCS / validity hooks ---
BOOL WINAPI Hook_IsDBCSLeadByte(BYTE b) {
    // Shift-JIS lead bytes: 0x81-9F or 0xE0-FC.
    return ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) ? TRUE : FALSE;
}

BOOL WINAPI Hook_IsDBCSLeadByteEx(UINT cp, BYTE b) {
    UINT eff = TranslateCodePage(cp);
    if (eff == kSpoofedCodePage) return Hook_IsDBCSLeadByte(b);
    return p_IsDBCSLeadByteEx(cp, b);
}

BOOL WINAPI Hook_IsValidCodePage(UINT cp) {
    if (cp == kSpoofedCodePage || cp == CP_ACP || cp == CP_OEMCP) return TRUE;
    return p_IsValidCodePage(cp);
}

BOOL WINAPI Hook_IsValidLocale(LCID lcid, DWORD flags) {
    if (lcid == kSpoofedLCID || lcid == LOCALE_USER_DEFAULT ||
        lcid == LOCALE_SYSTEM_DEFAULT) return TRUE;
    return p_IsValidLocale(lcid, flags);
}

// --- kernel32 path APIs ---
// Windows' default GetModuleFileNameA/GetCurrentDirectoryA/GetFullPathNameA
// applies BEST-FIT character mapping when converting wide paths to ANSI on
// non-JP-locale systems. Fullwidth Ｃ Ｐ Ｗ collapse to ASCII C P W. CPW
// then derives "CPW.kgt" from its module path and tries to open that file —
// which doesn't exist (the actual file on disk is "ＣＰＷ.kgt"). Result:
// silent exit at startup. Fix: route through the W variants and convert via
// CP932 ourselves with WC_NO_BEST_FIT_CHARS so the SJIS bytes survive.
DWORD WINAPI Hook_GetModuleFileNameA(HMODULE mod, LPSTR buffer, DWORD size) {
    if (!buffer || size == 0) return p_GetModuleFileNameA(mod, buffer, size);
    std::vector<wchar_t> wbuf(size);
    DWORD wlen = GetModuleFileNameW(mod, wbuf.data(), size);
    if (wlen == 0) return 0;
    int alen = WideCharToMultiByte(kSpoofedCodePage, WC_NO_BEST_FIT_CHARS,
                                   wbuf.data(), (int)wlen,
                                   buffer, (int)size, nullptr, nullptr);
    if (alen <= 0) return p_GetModuleFileNameA(mod, buffer, size);
    if ((DWORD)alen < size) buffer[alen] = '\0';
    return (DWORD)alen;
}

DWORD WINAPI Hook_GetCurrentDirectoryA(DWORD size, LPSTR buffer) {
    DWORD wneed = GetCurrentDirectoryW(0, nullptr);
    if (wneed == 0) return p_GetCurrentDirectoryA(size, buffer);
    std::vector<wchar_t> wbuf(wneed);
    DWORD wlen = GetCurrentDirectoryW(wneed, wbuf.data());
    if (wlen == 0) return p_GetCurrentDirectoryA(size, buffer);
    int alen = WideCharToMultiByte(kSpoofedCodePage, WC_NO_BEST_FIT_CHARS,
                                   wbuf.data(), (int)wlen + 1,
                                   buffer, (int)size, nullptr, nullptr);
    if (alen <= 0) return p_GetCurrentDirectoryA(size, buffer);
    return (DWORD)(alen - 1);  // exclude NUL terminator from return value
}

DWORD WINAPI Hook_GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buffer, LPSTR* file_part) {
    if (!name) return p_GetFullPathNameA(name, size, buffer, file_part);
    int wname_len = MultiByteToWideChar(kSpoofedCodePage, 0, name, -1, nullptr, 0);
    if (wname_len <= 1) return p_GetFullPathNameA(name, size, buffer, file_part);
    std::vector<wchar_t> wname(wname_len);
    if (MultiByteToWideChar(kSpoofedCodePage, 0, name, -1, wname.data(), wname_len) <= 0) {
        return p_GetFullPathNameA(name, size, buffer, file_part);
    }
    DWORD wneed = GetFullPathNameW(wname.data(), 0, nullptr, nullptr);
    if (wneed == 0) return p_GetFullPathNameA(name, size, buffer, file_part);
    std::vector<wchar_t> wbuf(wneed);
    LPWSTR wfp = nullptr;
    DWORD wlen = GetFullPathNameW(wname.data(), wneed, wbuf.data(), &wfp);
    if (wlen == 0) return p_GetFullPathNameA(name, size, buffer, file_part);
    int alen = WideCharToMultiByte(kSpoofedCodePage, WC_NO_BEST_FIT_CHARS,
                                   wbuf.data(), (int)wlen + 1,
                                   buffer, (int)size, nullptr, nullptr);
    if (alen <= 0) return p_GetFullPathNameA(name, size, buffer, file_part);
    if (file_part && wfp && buffer) {
        // Compute file_part offset by re-finding the last separator in
        // the converted ANSI buffer (cheap and avoids re-conversion math).
        char* sep = buffer;
        for (char* p = buffer; *p; ++p) {
            if (*p == '\\' || *p == '/') sep = p + 1;
        }
        *file_part = (sep > buffer && *sep) ? sep : nullptr;
    }
    return (DWORD)(alen - 1);
}

// --- INI / profile APIs (CP932 path + token conversion) ---
// SJIS(CP932) narrow -> wide. Returns false on any conversion failure.
static bool SjisToWideVec(LPCSTR s, std::vector<wchar_t>& out) {
    int n = MultiByteToWideChar(kSpoofedCodePage, 0, s, -1, nullptr, 0);
    if (n <= 0) return false;
    out.resize(n);
    return MultiByteToWideChar(kSpoofedCodePage, 0, s, -1, out.data(), n) > 0;
}

UINT WINAPI Hook_GetPrivateProfileIntA(LPCSTR app, LPCSTR key, INT def, LPCSTR file) {
    UINT result;
    std::vector<wchar_t> wapp, wkey, wfile;
    if (file && SjisToWideVec(file, wfile)) {
        const wchar_t* pApp = (app && SjisToWideVec(app, wapp)) ? wapp.data() : nullptr;
        const wchar_t* pKey = (key && SjisToWideVec(key, wkey)) ? wkey.data() : nullptr;
        result = GetPrivateProfileIntW(pApp, pKey, def, wfile.data());
    } else {
        result = p_GetPrivateProfileIntA(app, key, def, file);
    }
    // cnc-ddraw can only wrap the game when the game is in DirectDraw mode
    // (GameScreenMode != 0). Now that the read is CP932-correct we honor the
    // real ini, but ALSO pin DirectDraw when cnc-ddraw (2DFMD.dll) is present
    // — matching the launcher's "pin GameScreenMode=1" intent and making the
    // wrap engage regardless of what actually landed on disk for kanji-path
    // games (their launcher ini-write may have mangled). Without this the game
    // runs GDI-only: no ddraw, no overlay, no F4 fullscreen.
    if (key && result == 0 && lstrcmpiA(key, "GameScreenMode") == 0 &&
        GetModuleHandleA("2DFMD.dll") != nullptr) {
        result = 1;
    }
    return result;
}

DWORD WINAPI Hook_GetPrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR def,
                                           LPSTR ret, DWORD size, LPCSTR file) {
    if (!file || !ret || size == 0)
        return p_GetPrivateProfileStringA(app, key, def, ret, size, file);
    std::vector<wchar_t> wapp, wkey, wdef, wfile;
    if (!SjisToWideVec(file, wfile))
        return p_GetPrivateProfileStringA(app, key, def, ret, size, file);
    const wchar_t* pApp = (app && SjisToWideVec(app, wapp)) ? wapp.data() : nullptr;
    const wchar_t* pKey = (key && SjisToWideVec(key, wkey)) ? wkey.data() : nullptr;
    const wchar_t* pDef = (def && SjisToWideVec(def, wdef)) ? wdef.data() : L"";
    std::vector<wchar_t> wout(size);
    DWORD wn = GetPrivateProfileStringW(pApp, pKey, pDef, wout.data(), size, wfile.data());
    // Encode the wide result back to CP932. wn excludes the final terminator;
    // when app or key is NULL the buffer is a double-NUL list — converting wn
    // chars carries the embedded NULs through faithfully.
    int alen = WideCharToMultiByte(kSpoofedCodePage, 0, wout.data(), (int)wn,
                                   ret, (int)size, nullptr, nullptr);
    if (alen < 0) alen = 0;
    if ((DWORD)alen < size) ret[alen] = '\0';
    return (DWORD)alen;
}

BOOL WINAPI Hook_WritePrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR val, LPCSTR file) {
    if (!file) return p_WritePrivateProfileStringA(app, key, val, file);
    std::vector<wchar_t> wapp, wkey, wval, wfile;
    if (!SjisToWideVec(file, wfile))
        return p_WritePrivateProfileStringA(app, key, val, file);
    const wchar_t* pApp = (app && SjisToWideVec(app, wapp)) ? wapp.data() : nullptr;
    const wchar_t* pKey = (key && SjisToWideVec(key, wkey)) ? wkey.data() : nullptr;
    const wchar_t* pVal = (val && SjisToWideVec(val, wval)) ? wval.data() : nullptr;
    return WritePrivateProfileStringW(pApp, pKey, pVal, wfile.data());
}

// CPW's load_kgt_from_cmdline (0x406750) calls GetCommandLineA, walks the
// returned bytes looking for '.' to find the exe basename, then builds
// "<basename>.kgt". Without spoofing, Windows best-fit-converts the
// fullwidth ＣＰＷ in the cmdline path to ASCII "CPW", making CPW look for
// the wrong file. Re-encode the wide cmdline via CP932 with no-best-fit so
// the SJIS bytes for ＣＰＷ are preserved verbatim.
LPSTR WINAPI Hook_GetCommandLineA(void) {
    static char s_cached[8192];
    static bool s_filled = false;
    if (!s_filled) {
        LPCWSTR wide = GetCommandLineW();
        if (wide) {
            int alen = WideCharToMultiByte(kSpoofedCodePage, WC_NO_BEST_FIT_CHARS,
                                           wide, -1, s_cached, sizeof(s_cached),
                                           nullptr, nullptr);
            if (alen <= 0) {
                LPSTR orig = p_GetCommandLineA ? p_GetCommandLineA() : nullptr;
                if (orig) {
                    SDL_strlcpy(s_cached, orig, sizeof(s_cached));
                } else {
                    s_cached[0] = '\0';
                }
            }
        }
        // Re-encode SJIS -> UTF-8 for the log line so multibyte paths
        // render in any UTF-8-aware viewer instead of mojibake.
        char utf8[8192] = {0};
        wchar_t log_wide[4096] = {0};
        if (MultiByteToWideChar(kSpoofedCodePage, 0, s_cached, -1, log_wide, 4096) > 0) {
            WideCharToMultiByte(CP_UTF8, 0, log_wide, -1, utf8, sizeof(utf8), nullptr, nullptr);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "LocaleSpoof: GetCommandLineA -> '%s'",
                    utf8[0] ? utf8 : s_cached);
        s_filled = true;
    }
    return s_cached;
}

// --- USER32 windowing surrogate ---
// Window titles render via the system ANSI codepage, NOT via GetACP. So even
// with our codepage hooks in place, SetWindowTextA on a Shift-JIS string
// produces mojibake on US Windows. Translate the bytes via CP932 ourselves
// and forward to SetWindowTextW, which renders correctly regardless of system
// locale.
BOOL WINAPI Hook_SetWindowTextA(HWND hwnd, LPCSTR text) {
    if (!text) return p_SetWindowTextA(hwnd, text);

    // First call from the host's main thread: pin the thread locale to JP so
    // any other GDI API that branches on GetThreadLocale also picks Japanese.
    static thread_local bool s_thread_locale_set = false;
    if (!s_thread_locale_set) {
        SetThreadLocale(kSpoofedLCID);
        s_thread_locale_set = true;
    }

    int wlen = MultiByteToWideChar(kSpoofedCodePage, 0, text, -1, nullptr, 0);
    if (wlen <= 1) return p_SetWindowTextA(hwnd, text);
    std::vector<wchar_t> wide(wlen);
    if (MultiByteToWideChar(kSpoofedCodePage, 0, text, -1, wide.data(), wlen) <= 0) {
        return p_SetWindowTextA(hwnd, text);
    }
    // One-shot diagnostic: confirm our hook fires with what the game
    // actually passes, and confirm the window is Unicode at this point
    // (i.e. our locale wrapper successfully promoted it).
    static int s_logged = 0;
    if (s_logged < 4) {
        char utf8[1024] = {0};
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, utf8, sizeof(utf8),
                            nullptr, nullptr);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "LocaleSpoof: SetWindowTextA[%d] hwnd=%p IsUnicode=%d title='%s'",
            s_logged, (void*)hwnd, IsWindowUnicode(hwnd) ? 1 : 0, utf8);
        ++s_logged;
    }
    // Bypass the WNDPROC chain entirely. SetWindowTextW would post
    // WM_SETTEXT to the window's current WNDPROC; if a third-party
    // subclass (e.g. cnc-ddraw via SetWindowLongPtrA in its init path)
    // has reverted the window's IsUnicode flag, the W→A bridge mangles
    // wide chars via the SYSTEM CP_ACP (CP1252 on US Windows) — even
    // though our wrapper would have stored wide correctly, that wrapper
    // is no longer the top of the WNDPROC chain by the time the game
    // first sets a title.
    //
    // DefWindowProcW(WM_SETTEXT) goes straight to NtUserSetWindowText
    // with the wide string, storing it verbatim in the kernel window
    // object regardless of the user-mode A/W flag. The non-client area
    // gets a redraw automatically.
    DefWindowProcW(hwnd, WM_SETTEXT, 0, (LPARAM)wide.data());
    return TRUE;
}

// --- GDI32 in-game text rendering ---
// FM2K's render_game calls TextOutA up to 5× per frame for HUD overlays,
// CSS labels, and debug text. Without this hook, JP byte sequences get
// rendered using the system ANSI codepage (CP1252 on US Windows) and
// produce mojibake. Cheap path: ASCII-only strings (≤32 bytes, all bytes
// < 0x80) skip conversion and pass to original. SJIS strings convert
// via CP932 → TextOutW.
BOOL WINAPI Hook_TextOutA(HDC dc, int x, int y, LPCSTR text, int byte_len) {
    if (!text || byte_len <= 0) {
        return p_TextOutA(dc, x, y, text, byte_len);
    }
    // Fast path: short pure-ASCII string. Hot in render_game (frame counters,
    // score digits). Avoid per-call MB→W cost.
    bool ascii_only = (byte_len <= 32);
    if (ascii_only) {
        for (int i = 0; i < byte_len; ++i) {
            if ((unsigned char)text[i] >= 0x80) { ascii_only = false; break; }
        }
    }
    if (ascii_only) {
        return p_TextOutA(dc, x, y, text, byte_len);
    }
    // Per-frame allocation kills perf. Use a thread-local stack-style buffer
    // sized for typical FM2K render strings (max ~256 wide chars).
    thread_local wchar_t s_wbuf[512];
    int wlen = MultiByteToWideChar(kSpoofedCodePage, 0, text, byte_len,
                                   s_wbuf, (int)(sizeof(s_wbuf) / sizeof(s_wbuf[0])));
    if (wlen <= 0) {
        return p_TextOutA(dc, x, y, text, byte_len);
    }
    return TextOutW(dc, x, y, s_wbuf, wlen);
}

// --- USER32 dialog / popup text ---
// MessageBoxA carries both the body text and the caption; convert both.
// Cold path (only fires on errors / dialogs); std::vector is fine here.
int WINAPI Hook_MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type) {
    auto convert = [](LPCSTR s, std::vector<wchar_t>& out) -> LPCWSTR {
        if (!s) return nullptr;
        int wlen = MultiByteToWideChar(kSpoofedCodePage, 0, s, -1, nullptr, 0);
        if (wlen <= 1) return nullptr;  // empty / failure
        out.resize(wlen);
        if (MultiByteToWideChar(kSpoofedCodePage, 0, s, -1, out.data(), wlen) <= 0) {
            return nullptr;
        }
        return out.data();
    };
    std::vector<wchar_t> wtext, wcap;
    LPCWSTR wt = convert(text, wtext);
    LPCWSTR wc = convert(caption, wcap);
    if (!wt && !wc) return p_MessageBoxA(hwnd, text, caption, type);
    return MessageBoxW(hwnd, wt, wc, type);
}

// SetDlgItemTextA — config dialog labels (key bind / joystick bind dialogs
// in FM2K games typically ship JP labels). HWND + control id pass through;
// only the string parameter goes through CP932 → SetDlgItemTextW.
BOOL WINAPI Hook_SetDlgItemTextA(HWND hwnd, int item_id, LPCSTR text) {
    if (!text) return p_SetDlgItemTextA(hwnd, item_id, text);
    int wlen = MultiByteToWideChar(kSpoofedCodePage, 0, text, -1, nullptr, 0);
    if (wlen <= 1) return p_SetDlgItemTextA(hwnd, item_id, text);
    std::vector<wchar_t> wide(wlen);
    if (MultiByteToWideChar(kSpoofedCodePage, 0, text, -1, wide.data(), wlen) <= 0) {
        return p_SetDlgItemTextA(hwnd, item_id, text);
    }
    return SetDlgItemTextW(hwnd, item_id, wide.data());
}

// CreateWindowExA — main window + config dialogs. Class name can be either
// a string pointer OR an ATOM (low 16 bits used as integer, returned by
// RegisterClassA). ATOMs must pass through unchanged or the class lookup
// fails. Per MSDN: an ATOM has its high 16 bits zero and (uintptr_t < 0x10000).
HWND WINAPI Hook_CreateWindowExA(DWORD ex_style, LPCSTR class_name,
                                 LPCSTR window_name, DWORD style,
                                 int x, int y, int w, int h,
                                 HWND parent, HMENU menu,
                                 HINSTANCE inst, LPVOID lparam) {
    // Convert window_name (caption) via CP932 if non-empty/non-null.
    std::vector<wchar_t> wname;
    LPCWSTR wname_p = nullptr;
    if (window_name) {
        int wlen = MultiByteToWideChar(kSpoofedCodePage, 0, window_name, -1, nullptr, 0);
        if (wlen > 1) {
            wname.resize(wlen);
            if (MultiByteToWideChar(kSpoofedCodePage, 0, window_name, -1,
                                    wname.data(), wlen) > 0) {
                wname_p = wname.data();
            }
        }
    }

    // Class name handling: ATOM (low 16 bits) → pass through to ExA.
    // String pointer → convert via CP932 and call ExW. String pointer to
    // a class name like "BUTTON" or "EDIT" is ASCII, round-trips identically.
    bool class_is_atom = ((uintptr_t)class_name < 0x10000);
    if (class_is_atom) {
        // ATOMs only valid in the SAME variant (ExA-registered → ExA call).
        // Forward to ExA so the lookup succeeds, then flip the resulting
        // window to Unicode so SetWindowTextW preserves wide titles.
        HWND hwnd = p_CreateWindowExA(ex_style, class_name, window_name, style,
                                      x, y, w, h, parent, menu, inst, lparam);
        if (hwnd) InstallLocaleProcOnWindow(hwnd);
        return hwnd;
    }

    std::vector<wchar_t> wclass;
    LPCWSTR wclass_p = nullptr;
    if (class_name) {
        int wlen = MultiByteToWideChar(kSpoofedCodePage, 0, class_name, -1, nullptr, 0);
        if (wlen > 1) {
            wclass.resize(wlen);
            if (MultiByteToWideChar(kSpoofedCodePage, 0, class_name, -1,
                                    wclass.data(), wlen) > 0) {
                wclass_p = wclass.data();
            }
        }
    }

    // If we couldn't convert either, fall back to ExA so the call still works.
    if (!wclass_p && !wname_p) {
        HWND hwnd = p_CreateWindowExA(ex_style, class_name, window_name, style,
                                      x, y, w, h, parent, menu, inst, lparam);
        if (hwnd) InstallLocaleProcOnWindow(hwnd);
        return hwnd;
    }
    HWND hwnd = CreateWindowExW(ex_style, wclass_p, wname_p, style,
                                x, y, w, h, parent, menu, inst, lparam);
    if (hwnd) {
        InstallLocaleProcOnWindow(hwnd);
        // The WM_NCCREATE / WM_CREATE messages fired during CreateWindowExW
        // were delivered to the (still-ANSI) WNDPROC via the W→A bridge,
        // which mangled wname_p through the SYSTEM CP_ACP (CP1252 on US
        // Windows). Now that our wrapper has promoted the window to
        // Unicode, re-set the title with the correct wide string so the
        // wrapper's WM_SETTEXT path stores it as wide.
        if (wname_p) SetWindowTextW(hwnd, wname_p);
    }
    return hwnd;
}

// --- Window Unicode promotion ---
//
// FM2K (and FM95) register their window class via RegisterClassA, which
// marks the class — and every window created from it — as ANSI. When a
// later SetWindowTextA(hwnd, sjis_bytes) is intercepted by our hook and
// re-emitted as SetWindowTextW(hwnd, wide), the WM_SETTEXT message has to
// cross the W→A bridge to reach the ANSI WNDPROC. That bridge uses the
// SYSTEM ANSI codepage (CP1252 on US Windows), NOT our user-mode GetACP
// hook — so Japanese chars are destroyed in storage and the title bar
// renders garbage.
//
// Fix: after CreateWindowExW returns, install a W-flavored wrapper WNDPROC
// via SetWindowLongPtrW. This flips the window to Unicode at the OS level.
// The wrapper handles WM_SETTEXT / WM_GETTEXT / WM_GETTEXTLENGTH directly
// via DefWindowProcW so the title is stored as wide chars (correct), and
// forwards everything else to the original ANSI WNDPROC via
// CallWindowProcW (the system handles W→A for non-text messages without
// data loss).
//
// Bypassing the game's WNDPROC for text messages is safe: FM2K's
// main_window_proc (and FM95's equivalent) just forward those to
// DefWindowProcA — there's no game-side handling to preserve.

constexpr wchar_t kOrigWndProcProp[] = L"FM2KLocaleOrigWP";

LRESULT CALLBACK LocaleSpoofWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETTEXT:
        case WM_GETTEXT:
        case WM_GETTEXTLENGTH:
            // Wide-native title path — bypass the game's ANSI WNDPROC so
            // the system never collapses our wide title via CP1252.
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_NCDESTROY: {
            // Drop our per-window prop on destroy. We still forward the
            // message so the original chain can clean up too.
            HANDLE h = RemovePropW(hwnd, kOrigWndProcProp);
            WNDPROC orig = (WNDPROC)h;
            if (orig) return CallWindowProcW(orig, hwnd, msg, wp, lp);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }
    WNDPROC orig = (WNDPROC)GetPropW(hwnd, kOrigWndProcProp);
    if (!orig) return DefWindowProcW(hwnd, msg, wp, lp);
    // CallWindowProcW handles W→A param conversion if `orig` was
    // registered as ANSI (the FM2K/FM95 case via RegisterClassA).
    return CallWindowProcW(orig, hwnd, msg, wp, lp);
}

// --- kernel32 file-OPEN hooks (kanji install-path traversal) ---
// Convert a narrow (CP932/SJIS) path to wide. Returns false if the caller's
// path isn't valid SJIS (fall back to the original ANSI API then).
static bool NarrowSjisToWide(LPCSTR name, std::vector<wchar_t>& out) {
    if (!name) return false;
    int wlen = MultiByteToWideChar(kSpoofedCodePage, 0, name, -1, nullptr, 0);
    if (wlen <= 1) return false;   // empty or invalid
    out.resize(wlen);
    return MultiByteToWideChar(kSpoofedCodePage, 0, name, -1, out.data(), wlen) > 0;
}

static void FindDataW2A(const WIN32_FIND_DATAW& w, WIN32_FIND_DATAA& a) noexcept {
    a.dwFileAttributes = w.dwFileAttributes;
    a.ftCreationTime   = w.ftCreationTime;
    a.ftLastAccessTime = w.ftLastAccessTime;
    a.ftLastWriteTime  = w.ftLastWriteTime;
    a.nFileSizeHigh    = w.nFileSizeHigh;
    a.nFileSizeLow     = w.nFileSizeLow;
    a.dwReserved0      = w.dwReserved0;
    a.dwReserved1      = w.dwReserved1;
    WideCharToMultiByte(kSpoofedCodePage, 0, w.cFileName, -1,
                        a.cFileName, sizeof(a.cFileName), nullptr, nullptr);
    WideCharToMultiByte(kSpoofedCodePage, 0, w.cAlternateFileName, -1,
                        a.cAlternateFileName, sizeof(a.cAlternateFileName), nullptr, nullptr);
}

HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                               LPSECURITY_ATTRIBUTES sa, DWORD disp,
                               DWORD flags, HANDLE tmpl) {
    std::vector<wchar_t> w;
    if (!NarrowSjisToWide(name, w))
        return p_CreateFileA(name, access, share, sa, disp, flags, tmpl);
    return CreateFileW(w.data(), access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI Hook_FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA fd) {
    std::vector<wchar_t> w;
    if (!fd || !NarrowSjisToWide(name, w)) return p_FindFirstFileA(name, fd);
    WIN32_FIND_DATAW wfd;
    HANDLE h = FindFirstFileW(w.data(), &wfd);   // W handle; FindNextFileW/FindClose accept it
    if (h != INVALID_HANDLE_VALUE) FindDataW2A(wfd, *fd);
    return h;
}

BOOL WINAPI Hook_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd) {
    if (!fd) return p_FindNextFileA(h, fd);
    WIN32_FIND_DATAW wfd;
    BOOL r = FindNextFileW(h, &wfd);   // h came from our Hook_FindFirstFileA => a W handle
    if (r) FindDataW2A(wfd, *fd);
    return r;
}

void InstallLocaleProcOnWindow(HWND hwnd) noexcept {
    if (!hwnd) return;
    if (GetPropW(hwnd, kOrigWndProcProp)) return;  // already installed
    // Do NOT promote the MAIN game window (KGT2KGAME/KGT95GAME) to Unicode.
    // Promoting it before cnc-ddraw's ddraw-init races cnc-ddraw's own
    // SetWindowLongA subclass and, on JP-title games, leaves cnc-ddraw unable
    // to bind its ddraw state to the window (observed: hWndTo=0, SrcWidth=0),
    // which kills its fullscreen path entirely (F4/Alt+Enter). Skipping the
    // promotion lets cnc-ddraw attach normally. The JP titlebar is unaffected:
    // Hook_SetWindowTextA sets the wide title via DefWindowProcW(WM_SETTEXT),
    // which stores it correctly on an ANSI window too. Dialogs/config windows
    // are still promoted. Escape hatch: FM2K_FORCE_MAIN_WINDOW_PROMOTE=1.
    {
        static const int s_force_promote = []{
            const char* e = std::getenv("FM2K_FORCE_MAIN_WINDOW_PROMOTE");
            return (e && e[0] == '1') ? 1 : 0;
        }();
        if (!s_force_promote) {
            char cls[32] = {0};
            GetClassNameA(hwnd, cls, sizeof(cls));
            if (std::strcmp(cls, "KGT2KGAME") == 0 ||
                std::strcmp(cls, "KGT95GAME") == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "LocaleSpoof: main game window hwnd=%p left ANSI so cnc-ddraw "
                    "binds its ddraw state (F4 fullscreen); JP title via "
                    "SetWindowTextA hook", (void*)hwnd);
                return;
            }
        }
    }
    // Capture the original WNDPROC as ANSI — that's how the game
    // registered it via RegisterClassA.
    WNDPROC orig = (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    if (!orig) return;
    if (!SetPropW(hwnd, kOrigWndProcProp, (HANDLE)orig)) return;
    // SetWindowLongPtrW marks the window as Unicode at the OS level so
    // WM_SETTEXT messages stay wide all the way to DefWindowProcW.
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)LocaleSpoofWndProc);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "LocaleSpoof: promoted hwnd=%p to Unicode (orig WNDPROC=%p, IsUnicode=%d)",
        (void*)hwnd, (void*)orig, IsWindowUnicode(hwnd) ? 1 : 0);
}

// --- DialogBoxParamA: translate the dialog template resource ---
//
// Symptom: WW's "specify title" popup renders as `?????`. Cause: the
// dialog template is compiled into the .rc as wide UTF-16, but on a JP-
// system build many wide values are actually SJIS BYTES stuffed into the
// low half of WCHAR slots — i.e. the resource compiler emitted SJIS bytes
// expecting the OS to interpret them via CP_ACP=932, which doesn't
// happen on US Windows. Our user-mode GetACP hook does NOT affect the
// kernel-side resource string conversion, so static labels render as `?`.
//
// Fix: hook DialogBoxParamA. Pull the RT_DIALOG resource ourselves, walk
// the DLGTEMPLATE (or DLGTEMPLATEEX) structure, and for every embedded
// wide-string field check if it looks like packed-SJIS-in-WCHAR. If so,
// flatten back to bytes, decode via CP932, write out a real wide string.
// Then call DialogBoxIndirectParamW with the rewritten template.
//
// Heuristic for "packed-SJIS-in-WCHAR": every WCHAR has its high byte
// zero (high-half ASCII range). True UTF-16 JP would have high bytes
// like 0x30 (Hiragana), 0x4E-0x9F (CJK), etc. Pure-ASCII titles also
// have all-zero high bytes — for those, the round-trip via CP932 is a
// no-op, so always-translate is safe.
namespace {

// Reinterpret a sequence of WCHARs (each holding a single SJIS byte in low
// 8 bits) as a contiguous SJIS byte buffer, then decode via CP932 to real
// UTF-16. Returns the decoded wide string. If any WCHAR has a non-zero
// high byte, the string is treated as already-real-UTF-16 and copied as-is.
std::wstring DialogStringTranslate(const wchar_t* in, size_t in_len) {
    if (!in || in_len == 0) return {};
    bool packed_sjis = true;
    for (size_t i = 0; i < in_len; ++i) {
        if ((unsigned)(in[i] >> 8) != 0) { packed_sjis = false; break; }
    }
    if (!packed_sjis) return std::wstring(in, in_len);
    // Flatten low bytes
    std::vector<char> sjis(in_len);
    for (size_t i = 0; i < in_len; ++i) sjis[i] = (char)(in[i] & 0xFF);
    // Decode via CP932
    int wlen = MultiByteToWideChar(kSpoofedCodePage, 0,
                                   sjis.data(), (int)sjis.size(),
                                   nullptr, 0);
    if (wlen <= 0) return std::wstring(in, in_len);
    std::wstring out(wlen, L'\0');
    MultiByteToWideChar(kSpoofedCodePage, 0,
                        sjis.data(), (int)sjis.size(),
                        out.data(), wlen);
    return out;
}

// Skip a sz_or_ord field at *cursor: either 0xFFFF + ordinal, or wide
// string null-terminated. Advance *cursor past it. If translated_out is
// non-null and the field is a string, write the translated wide string
// (NUL-terminated) into it; otherwise write the raw bytes.
void DlgTemplate_SkipOrTranslateString(const uint8_t** cursor,
                                       std::vector<uint8_t>* out) {
    const uint16_t* w = (const uint16_t*)*cursor;
    if (w[0] == 0x0000) {
        // Empty string (null terminator only). Advance past one WORD.
        if (out) {
            out->push_back(0); out->push_back(0);
        }
        *cursor += 2;
        return;
    }
    if (w[0] == 0xFFFF) {
        // Ordinal: 0xFFFF followed by one WORD. Advance 4 bytes total.
        if (out) {
            const uint8_t* src = *cursor;
            out->insert(out->end(), src, src + 4);
        }
        *cursor += 4;
        return;
    }
    // String: walk to NUL.
    size_t len = 0;
    while (w[len] != 0) ++len;
    if (out) {
        std::wstring translated = DialogStringTranslate((const wchar_t*)w, len);
        const uint8_t* tb = (const uint8_t*)translated.c_str();
        out->insert(out->end(), tb, tb + (translated.size() + 1) * sizeof(wchar_t));
    }
    *cursor += (len + 1) * 2;  // string + NUL, 2 bytes per WCHAR
}

// Align scratch buffer to DWORD boundary (DLGITEMTEMPLATE entries are
// DWORD-aligned in the template).
void DlgTemplate_AlignToDword(std::vector<uint8_t>& out) {
    while (out.size() % 4) out.push_back(0);
}

// Translate a DLGTEMPLATE / DLGTEMPLATEEX into a new buffer with all
// embedded strings re-encoded via CP932. Returns empty on parse failure.
std::vector<uint8_t> TranslateDialogTemplate(const uint8_t* raw, size_t size) {
    if (size < sizeof(DLGTEMPLATE)) return {};
    std::vector<uint8_t> out;
    out.reserve(size + 256);

    const uint16_t* head = (const uint16_t*)raw;
    const bool is_ex = (head[0] == 0x0001 && head[1] == 0xFFFF);
    const uint8_t* cursor = raw;
    uint16_t cdit = 0;
    bool has_font = false;
    DWORD style = 0;

    if (is_ex) {
        // DLGTEMPLATEEX header is 26 bytes: 4 (dlgVer+sig) + 4 (helpID)
        // + 4 (exStyle) + 4 (style) + 2 (cDlgItems) + 2 (x) + 2 (y) + 2 (cx) + 2 (cy)
        if (size < 26) return {};
        out.insert(out.end(), cursor, cursor + 26);
        style = *(const DWORD*)(cursor + 12);
        cdit  = *(const uint16_t*)(cursor + 16);
        cursor += 26;
    } else {
        // DLGTEMPLATE header is 18 bytes
        if (size < 18) return {};
        out.insert(out.end(), cursor, cursor + 18);
        style = *(const DWORD*)cursor;
        cdit  = *(const uint16_t*)(cursor + 8);
        cursor += 18;
    }
    has_font = (style & DS_SETFONT) != 0;

    // menu, windowClass, title
    DlgTemplate_SkipOrTranslateString(&cursor, &out);  // menu
    DlgTemplate_SkipOrTranslateString(&cursor, &out);  // class
    DlgTemplate_SkipOrTranslateString(&cursor, &out);  // title

    if (has_font) {
        if (is_ex) {
            // pointsize(2) + weight(2) + italic(1) + charset(1) + typeface
            out.insert(out.end(), cursor, cursor + 6);
            cursor += 6;
        } else {
            // pointsize(2) + typeface
            out.insert(out.end(), cursor, cursor + 2);
            cursor += 2;
        }
        DlgTemplate_SkipOrTranslateString(&cursor, &out);  // typeface
    }

    // DLGITEMTEMPLATE × cdit, each DWORD-aligned.
    for (uint16_t i = 0; i < cdit; ++i) {
        DlgTemplate_AlignToDword(out);
        // Align cursor in source too — same rule.
        size_t cur_off = (size_t)(cursor - raw);
        while (cur_off % 4) { ++cursor; ++cur_off; }

        if (is_ex) {
            // DLGITEMTEMPLATEEX header: 22 bytes (helpID 4 + exStyle 4 +
            // style 4 + x 2 + y 2 + cx 2 + cy 2 + id 4)
            if (cur_off + 22 > size) return {};
            out.insert(out.end(), cursor, cursor + 22);
            cursor += 22;
        } else {
            // DLGITEMTEMPLATE header: 18 bytes (style 4 + exStyle 4 +
            // x 2 + y 2 + cx 2 + cy 2 + id 2)
            if (cur_off + 18 > size) return {};
            out.insert(out.end(), cursor, cursor + 18);
            cursor += 18;
        }
        DlgTemplate_SkipOrTranslateString(&cursor, &out);  // class
        DlgTemplate_SkipOrTranslateString(&cursor, &out);  // title

        // creation data: WORD count, then (count-2) bytes of payload (count
        // includes the WORD itself per MSDN; we copy as-is).
        size_t off = (size_t)(cursor - raw);
        if (off + 2 > size) return {};
        uint16_t cd = *(const uint16_t*)cursor;
        out.insert(out.end(), cursor, cursor + 2);
        cursor += 2;
        if (cd > 0) {
            // cd is in WORDs, total payload bytes = cd*2 - 2 (cd includes
            // the count WORD itself per some docs; verify per MSDN).
            size_t pay = (size_t)(cd) * 2 - 2;
            if ((size_t)(cursor - raw) + pay > size) return {};
            out.insert(out.end(), cursor, cursor + pay);
            cursor += pay;
        }
    }

    return out;
}

} // namespace

INT_PTR WINAPI Hook_DialogBoxParamA(HINSTANCE inst, LPCSTR template_name,
                                    HWND parent, DLGPROC dlg_proc,
                                    LPARAM init_param) {
    HRSRC res = FindResourceA(inst, template_name, MAKEINTRESOURCEA(5)); // RT_DIALOG = 5
    if (!res) {
        return p_DialogBoxParamA(inst, template_name, parent, dlg_proc, init_param);
    }
    HGLOBAL h = LoadResource(inst, res);
    DWORD sz = SizeofResource(inst, res);
    LPVOID raw = h ? LockResource(h) : nullptr;
    if (!raw || sz == 0) {
        return p_DialogBoxParamA(inst, template_name, parent, dlg_proc, init_param);
    }
    std::vector<uint8_t> translated = TranslateDialogTemplate((const uint8_t*)raw, sz);
    if (translated.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "LocaleSpoof: DialogBoxParamA template translate failed (size=%lu) — passing through",
                    (unsigned long)sz);
        return p_DialogBoxParamA(inst, template_name, parent, dlg_proc, init_param);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "LocaleSpoof: DialogBoxParamA template re-encoded via CP932 (orig=%lu B → new=%zu B)",
                (unsigned long)sz, translated.size());
    return DialogBoxIndirectParamW(inst, (LPCDLGTEMPLATEW)translated.data(),
                                   parent, dlg_proc, init_param);
}

template <typename T>
bool CreateAndEnableHook(LPCSTR module, LPCSTR symbol, void* detour, T*& trampoline) {
    HMODULE mod = GetModuleHandleA(module);
    if (!mod) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "LocaleSpoof: GetModuleHandleA(%s) failed", module);
        return false;
    }
    void* target = reinterpret_cast<void*>(GetProcAddress(mod, symbol));
    if (!target) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "LocaleSpoof: GetProcAddress(%s!%s) not found — skipping", module, symbol);
        return false;
    }
    void* tramp = nullptr;
    MH_STATUS s = MH_CreateHook(target, detour, &tramp);
    if (s != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "LocaleSpoof: MH_CreateHook(%s) failed: %d", symbol, (int)s);
        return false;
    }
    trampoline = reinterpret_cast<T*>(tramp);
    // Queue rather than enable: each MH_EnableHook freezes/thaws every thread
    // in the process via Toolhelp32Snapshot (~80–120ms each). Queue all 24
    // locale hooks and flush once with MH_ApplyQueued at the end of
    // InstallLocaleSpoof — single freeze instead of 24, ~2s saved on cold start.
    s = MH_QueueEnableHook(target);
    if (s != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "LocaleSpoof: MH_QueueEnableHook(%s) failed: %d", symbol, (int)s);
        return false;
    }
    return true;
}

} // namespace

bool InstallLocaleSpoof() {
    if (g_installed) return true;

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "LocaleSpoof: MH_Initialize failed: %d", (int)init);
        return false;
    }

    // Set the LoadLibrary thread's locale immediately so any code we run
    // before the main thread resumes also sees Japanese.
    SetThreadLocale(kSpoofedLCID);

    int ok = 0, total = 0;
    auto try_hook = [&](LPCSTR mod, LPCSTR sym, void* detour, auto& tramp) {
        ++total;
        if (CreateAndEnableHook(mod, sym, detour, tramp)) ++ok;
    };

    // CRT codepage path (load-bearing for sprintf / fopen / etc).
    try_hook("kernel32.dll", "GetACP",              (void*)&Hook_GetACP,              p_GetACP);
    try_hook("kernel32.dll", "GetOEMCP",            (void*)&Hook_GetOEMCP,            p_GetOEMCP);
    try_hook("kernel32.dll", "GetCPInfo",           (void*)&Hook_GetCPInfo,           p_GetCPInfo);
    try_hook("kernel32.dll", "MultiByteToWideChar", (void*)&Hook_MultiByteToWideChar, p_MultiByteToWideChar);
    try_hook("kernel32.dll", "WideCharToMultiByte", (void*)&Hook_WideCharToMultiByte, p_WideCharToMultiByte);

    // LCID / language path (GUI APIs branch on these).
    try_hook("kernel32.dll", "GetUserDefaultLCID",        (void*)&Hook_GetUserDefaultLCID,        p_GetUserDefaultLCID);
    try_hook("kernel32.dll", "GetSystemDefaultLCID",      (void*)&Hook_GetSystemDefaultLCID,      p_GetSystemDefaultLCID);
    try_hook("kernel32.dll", "GetUserDefaultUILanguage",  (void*)&Hook_GetUserDefaultUILanguage,  p_GetUserDefaultUILanguage);
    try_hook("kernel32.dll", "GetSystemDefaultUILanguage",(void*)&Hook_GetSystemDefaultUILanguage,p_GetSystemDefaultUILanguage);
    try_hook("kernel32.dll", "GetThreadLocale",           (void*)&Hook_GetThreadLocale,           p_GetThreadLocale);

    // DBCS + validity probes.
    try_hook("kernel32.dll", "IsDBCSLeadByte",   (void*)&Hook_IsDBCSLeadByte,   p_IsDBCSLeadByte);
    try_hook("kernel32.dll", "IsDBCSLeadByteEx", (void*)&Hook_IsDBCSLeadByteEx, p_IsDBCSLeadByteEx);
    try_hook("kernel32.dll", "IsValidCodePage",  (void*)&Hook_IsValidCodePage,  p_IsValidCodePage);
    try_hook("kernel32.dll", "IsValidLocale",    (void*)&Hook_IsValidLocale,    p_IsValidLocale);

    // kernel32 path APIs — block best-fit narrow-char fallback that mangles
    // fullwidth Latin (Ｃ→C) and breaks CPW's filename derivation.
    try_hook("kernel32.dll", "GetModuleFileNameA",   (void*)&Hook_GetModuleFileNameA,   p_GetModuleFileNameA);
    try_hook("kernel32.dll", "GetCurrentDirectoryA", (void*)&Hook_GetCurrentDirectoryA, p_GetCurrentDirectoryA);
    try_hook("kernel32.dll", "GetFullPathNameA",     (void*)&Hook_GetFullPathNameA,     p_GetFullPathNameA);
    try_hook("kernel32.dll", "GetCommandLineA",      (void*)&Hook_GetCommandLineA,      p_GetCommandLineA);

    // INI/profile APIs — the SJIS-folder GameScreenMode fix. Without these the
    // game reads GameScreenMode=default(0) on kanji paths → GDI mode → no
    // DirectDraw → cnc-ddraw never engages → F4 fullscreen dead. ASCII paths
    // round-trip through CP932 identically, so English games are unaffected.
    try_hook("kernel32.dll", "GetPrivateProfileIntA",      (void*)&Hook_GetPrivateProfileIntA,      p_GetPrivateProfileIntA);
    try_hook("kernel32.dll", "GetPrivateProfileStringA",   (void*)&Hook_GetPrivateProfileStringA,   p_GetPrivateProfileStringA);
    try_hook("kernel32.dll", "WritePrivateProfileStringA", (void*)&Hook_WritePrivateProfileStringA, p_WritePrivateProfileStringA);

    // File-OPEN hooks (CreateFileA / FindFirstFileA / FindNextFileA) for kanji
    // install-path traversal are DISABLED: routing every file open through
    // CreateFileW regressed graphics/sprite asset loading (black screen) on
    // ASCII-path games, and they were never shown to be necessary (kanji-dir
    // games load fine via the already-hooked CRT MultiByteToWideChar path). Left
    // dead until a surgical, ASCII-passthrough + validated version is warranted.
    //   try_hook("kernel32.dll", "CreateFileA",    &Hook_CreateFileA,    p_CreateFileA);
    //   try_hook("kernel32.dll", "FindFirstFileA", &Hook_FindFirstFileA, p_FindFirstFileA);
    //   try_hook("kernel32.dll", "FindNextFileA",  &Hook_FindNextFileA,  p_FindNextFileA);
    (void)&Hook_CreateFileA; (void)&Hook_FindFirstFileA; (void)&Hook_FindNextFileA;

    // USER32 / GDI32 visible-text rendering — covers in-game text, error
    // popups, dialog labels, and window-creation titles. SetWindowTextA was
    // already in place; the rest were added in pass 2 of the FM2K analysis
    // after import-table xrefs surfaced all four ANSI render paths WW uses.
    try_hook("user32.dll",   "SetWindowTextA",   (void*)&Hook_SetWindowTextA,   p_SetWindowTextA);
    try_hook("gdi32.dll",    "TextOutA",         (void*)&Hook_TextOutA,         p_TextOutA);
    try_hook("user32.dll",   "MessageBoxA",      (void*)&Hook_MessageBoxA,      p_MessageBoxA);
    try_hook("user32.dll",   "SetDlgItemTextA",  (void*)&Hook_SetDlgItemTextA,  p_SetDlgItemTextA);
    try_hook("user32.dll",   "CreateWindowExA",  (void*)&Hook_CreateWindowExA,  p_CreateWindowExA);
    try_hook("user32.dll",   "DialogBoxParamA",  (void*)&Hook_DialogBoxParamA,  p_DialogBoxParamA);

    // Single freeze for all queued hooks (vs one freeze per hook).
    MH_STATUS apply = MH_ApplyQueued();
    if (apply != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "LocaleSpoof: MH_ApplyQueued failed: %d", (int)apply);
        return false;
    }

    g_installed = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "LocaleSpoof: Japanese (CP%u, LCID 0x%04X) — %d/%d hooks installed",
        kSpoofedCodePage, kSpoofedLCID, ok, total);
    return ok == total;
}

void UninstallLocaleSpoof() {
    if (!g_installed) return;
    MH_DisableHook(MH_ALL_HOOKS);
    g_installed = false;
}
