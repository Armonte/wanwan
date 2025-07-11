#include <windows.h>
#include <cstdio>
#include <MinHook.h>
<<<<<<< Updated upstream
#include "sdl3_hooks.h"
#include "ddraw_hooks.h"
=======
#include <stdarg.h>  // For va_list
#include <ddraw.h>
#include "address.h"  // Verified FM2K addresses
#include "dummy_directdraw.h"  // External dummy DirectDraw objects
#include "sdl3_types.h"  // SDL3 shared types
#include "surface_manager.h"  // SDL3 surface management

// DirectDraw error codes if not defined
#ifndef DDERR_GENERIC
#define DDERR_GENERIC                   MAKE_HRESULT(1, 0x876, 1)
#define DDERR_INVALIDPARAMS            MAKE_HRESULT(1, 0x876, 2)
#define DDERR_UNSUPPORTED              MAKE_HRESULT(1, 0x876, 3)
#define DDERR_ALREADYINITIALIZED       MAKE_HRESULT(1, 0x876, 4)
#define DDERR_INVALIDOBJECT            MAKE_HRESULT(1, 0x876, 5)
#define DDERR_INVALIDMODE              MAKE_HRESULT(1, 0x876, 6)
#define DDERR_SURFACELOST              MAKE_HRESULT(1, 0x876, 7)
#endif

// Ensure proper calling conventions for COM methods
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE __stdcall
#endif

// DirectDraw flags if not defined
#ifndef DDBLT_COLORFILL
#define DDBLT_COLORFILL               0x00000400l
#endif

// SDL3 Window and Renderer flags
#define SDL_WINDOW_SHOWN              0x00000004
#define SDL_WINDOW_RESIZABLE         0x00000020
#define SDL_WINDOW_FULLSCREEN        0x00000001
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001
#define SDL_RENDERER_ACCELERATED      0x00000002
#define SDL_RENDERER_PRESENTVSYNC     0x00000004

// DirectDraw constants
#define DD_OK                   0x00000000L
#define DDERR_INVALIDPARAMS     0x887000057L
#define DDERR_SURFACEBUSY       0x887000176L
#define DDERR_NOTLOCKED         0x887000094L
#define DDERR_NOTFOUND          0x887000076L
#define DDSCAPS_PRIMARYSURFACE  0x00000200L
#define DDSCAPS_BACKBUFFER      0x00000004L
#define DDSCAPS_FLIP           0x00000001L
#define DDSCAPS_COMPLEX        0x00000008L
#define DDSCAPS_VIDEOMEMORY    0x00004000L

// Forward declarations
void LogMessage(const char* message);
void InitializeSurfaces();
void CleanupSurfaces();
bool InitializeSDL3();
bool CreateSDL3Window(HWND gameHwnd);
bool CreateSDL3Renderer();
void RenderFrame();
BOOL WINAPI Hook_InitializeGame(HWND windowHandle);
BOOL WINAPI Hook_InitializeDirectDraw(BOOL isFullScreen, HWND windowHandle);
LRESULT CALLBACK WindowProc_Hook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LONG WINAPI Hook_SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong);
bool EventFilter(void* userdata, SDL_Event* event);
bool WindowsMessageHook(void* userdata, MSG* msg);
bool CreateSDL3Textures();
void SetupDirectDrawVirtualTable();
void SetupSurfaceVirtualTables();
void SetupDirectDrawReplacement();
void CleanupHooks();
// SDL3 Window Subclassing (Moon Lights 2 approach)
LRESULT CALLBACK SDL3GameWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void SetupWindowSubclassing(HWND sdlHwnd);
void UpdateInputFromWindowMessage(UINT uMsg, WPARAM wParam);

// --- Forward declarations for hooks ---
BOOL WINAPI InitGame_Hook(HWND windowHandle);
BOOL WINAPI InitDirectDraw_Hook(BOOL isFullScreen, HWND windowHandle);

// Function pointers for original functions
extern "C" {
    static BOOL (WINAPI* original_process_input_history)() = nullptr;
    static BOOL (WINAPI* original_initialize_game)(HWND windowHandle) = nullptr;
    static BOOL (WINAPI* original_initialize_directdraw)(BOOL isFullScreen, HWND windowHandle) = nullptr;
    static LRESULT (WINAPI* original_window_proc)(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) = nullptr;
    static LONG (WINAPI* original_set_window_long_a)(HWND hWnd, int nIndex, LONG dwNewLong) = nullptr;
    static HWND (WINAPI* original_create_window_ex_a)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) = nullptr;
    static HRESULT (WINAPI* original_directdraw_create)(void* lpGUID, void** lplpDD, void* pUnkOuter) = nullptr;
}

// SDL3 Window Subclassing Variables (Moon Lights 2 approach)
static WNDPROC g_originalSDLWindowProc = nullptr;
static const uintptr_t HANDLE_MENU_CALL_HOTKEYS_ADDR = 0x405F50; // main_window_proc function address from IDA
typedef LRESULT (__stdcall *HandleMenuCallHotkeysFunc)(HWND, UINT, WPARAM, LPARAM);
static HandleMenuCallHotkeysFunc g_handleMenuCallHotkeys = (HandleMenuCallHotkeysFunc)HANDLE_MENU_CALL_HOTKEYS_ADDR;

// DirectDraw vtable declarations
// IUnknown methods
HRESULT STDMETHODCALLTYPE DirectDraw_QueryInterface(void* This, REFIID riid, void** ppvObject);
ULONG STDMETHODCALLTYPE DirectDraw_AddRef(void* This);
ULONG STDMETHODCALLTYPE DirectDraw_Release(void* This);
// IDirectDraw methods (essential ones based on assembly analysis)
HRESULT STDMETHODCALLTYPE DirectDraw_SetCooperativeLevel(void* This, HWND hWnd, DWORD dwFlags);
HRESULT STDMETHODCALLTYPE DirectDraw_SetDisplayMode(void* This, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP);
HRESULT STDMETHODCALLTYPE DirectDraw_CreateSurface(void* This, void* lpDDSurfaceDesc, void** lplpDDSurface, void* pUnkOuter);
// Critical DirectDraw methods implemented to prevent crashes
HRESULT STDMETHODCALLTYPE DirectDraw_GetDisplayMode(void* This, void* lpDDSurfaceDesc);
HRESULT STDMETHODCALLTYPE DirectDraw_GetCaps(void* This, void* lpDDDriverCaps, void* lpDDHELCaps);
HRESULT STDMETHODCALLTYPE DirectDraw_GetMonitorFrequency(void* This, LPDWORD lpdwFrequency);
HRESULT STDMETHODCALLTYPE DirectDraw_GetVerticalBlankStatus(void* This, LPBOOL lpbIsInVB);
// Stub implementations for other DirectDraw methods
HRESULT STDMETHODCALLTYPE DirectDraw_Stub(void* This, ...);
// Test function with minimal implementation
HRESULT __stdcall DirectDraw_TestFunction();

// DirectDrawCreate hook
HRESULT WINAPI Hook_DirectDrawCreate(void* lpGUID, void** lplpDD, void* pUnkOuter);

// DirectDraw Surface vtable declarations - ALL 27 methods
// IUnknown methods
HRESULT STDMETHODCALLTYPE Surface_QueryInterface(void* This, REFIID riid, void** ppvObject);
ULONG STDMETHODCALLTYPE Surface_AddRef(void* This);
ULONG STDMETHODCALLTYPE Surface_Release(void* This);
// IDirectDrawSurface methods  
HRESULT STDMETHODCALLTYPE Surface_AddAttachedSurface(void* This, void* lpDDSAttachedSurface);
HRESULT STDMETHODCALLTYPE Surface_AddOverlayDirtyRect(void* This, LPRECT lpRect);
HRESULT STDMETHODCALLTYPE Surface_Blt(void* This, LPRECT lpDestRect, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx);
HRESULT STDMETHODCALLTYPE Surface_BltBatch(void* This, void* lpDDBltBatch, DWORD dwCount, DWORD dwFlags);
HRESULT STDMETHODCALLTYPE Surface_BltFast(void* This, DWORD dwX, DWORD dwY, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans);
HRESULT STDMETHODCALLTYPE Surface_DeleteAttachedSurface(void* This, DWORD dwFlags, void* lpDDSAttachedSurface);
HRESULT STDMETHODCALLTYPE Surface_EnumAttachedSurfaces(void* This, LPVOID lpContext, void* lpEnumSurfacesCallback);
HRESULT STDMETHODCALLTYPE Surface_EnumOverlayZOrders(void* This, DWORD dwFlags, LPVOID lpContext, void* lpfnCallback);
HRESULT STDMETHODCALLTYPE Surface_Flip(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags);
HRESULT STDMETHODCALLTYPE Surface_GetAttachedSurface(void* This, void* lpDDSCaps, void** lplpDDAttachedSurface);
HRESULT STDMETHODCALLTYPE Surface_GetBltStatus(void* This, DWORD dwFlags);
HRESULT STDMETHODCALLTYPE Surface_GetCaps(void* This, void* lpDDSCaps);
HRESULT STDMETHODCALLTYPE Surface_GetClipper(void* This, void** lplpDDClipper);
HRESULT STDMETHODCALLTYPE Surface_GetColorKey(void* This, DWORD dwFlags, void* lpDDColorKey);
HRESULT STDMETHODCALLTYPE Surface_GetDC(void* This, HDC* lphDC);
HRESULT STDMETHODCALLTYPE Surface_GetFlipStatus(void* This, DWORD dwFlags);
HRESULT STDMETHODCALLTYPE Surface_GetOverlayPosition(void* This, LPLONG lplX, LPLONG lplY);
HRESULT STDMETHODCALLTYPE Surface_GetPalette(void* This, void** lplpDDPalette);
HRESULT STDMETHODCALLTYPE Surface_GetPixelFormat(void* This, void* lpDDPixelFormat);
HRESULT STDMETHODCALLTYPE Surface_GetSurfaceDesc(void* This, void* lpDDSurfaceDesc);
HRESULT STDMETHODCALLTYPE Surface_Initialize(void* This, void* lpDD, void* lpDDSurfaceDesc);
HRESULT STDMETHODCALLTYPE Surface_IsLost(void* This);
int Surface_IsLost_Raw();
HRESULT STDMETHODCALLTYPE Surface_Lock(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent);
HRESULT STDMETHODCALLTYPE Surface_ReleaseDC(void* This, HDC hDC);
HRESULT STDMETHODCALLTYPE Surface_Restore(void* This);
HRESULT STDMETHODCALLTYPE Surface_SetClipper(void* This, void* lpDDClipper);
HRESULT STDMETHODCALLTYPE Surface_SetColorKey(void* This, DWORD dwFlags, void* lpDDColorKey);
HRESULT STDMETHODCALLTYPE Surface_SetOverlayPosition(void* This, LONG lX, LONG lY);
HRESULT STDMETHODCALLTYPE Surface_SetPalette(void* This, void* lpDDPalette);
HRESULT STDMETHODCALLTYPE Surface_Unlock(void* This, void* lpRect);
HRESULT STDMETHODCALLTYPE Surface_UpdateOverlay(void* This, LPRECT lpSrcRect, void* lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, void* lpDDOverlayFx);
HRESULT STDMETHODCALLTYPE Surface_UpdateOverlayDisplay(void* This, DWORD dwFlags);
HRESULT STDMETHODCALLTYPE Surface_UpdateOverlayZOrder(void* This, DWORD dwFlags, void* lpDDSReference);
>>>>>>> Stashed changes

// --- Globals ---
static FILE* g_console_stream = nullptr;
void LogMessage(const char* message);

// --- Function Pointers for Original Game Functions ---
    static HRESULT (WINAPI* original_directdraw_create)(void* lpGUID, void** lplpDD, void* pUnkOuter) = nullptr;
static HWND (WINAPI* original_create_window_ex_a)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) = nullptr;
static LONG (WINAPI* original_set_window_long_a)(HWND, int, LONG) = nullptr;
static BOOL (WINAPI* original_process_input_history)() = nullptr;

// --- Hook Implementations ---

HWND WINAPI Hook_CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam) {
    // Let the game create its window first.
    HWND gameHwnd = original_create_window_ex_a(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);

<<<<<<< Updated upstream
    if (gameHwnd && lpClassName && strcmp(lpClassName, "KGT2KGAME") == 0) {
        LogMessage("*** DETECTED MAIN GAME WINDOW - INITIATING DIRECT TAKEOVER ***");
        if (InitializeSDL3()) {
            if (CreateSDL3Context(gameHwnd)) {
                // After docking SDL, we must explicitly show the window,
                // as the game's own ShowWindow call might be missed or ignored.
                LogMessage("Forcing game window to show.");
                ShowWindow(gameHwnd, SW_SHOW);
                UpdateWindow(gameHwnd);
=======
// DirectDraw Structure
struct SDL3DirectDraw {
    IDirectDrawVtbl* lpVtbl;  // DirectDraw vtable pointer
    bool initialized;
    SDL3Surface* primarySurface;
    SDL3Surface* backSurface;
    SDL3Surface* spriteSurface;
    LONG refCount;
};

// Global instances
static SDL3DirectDraw g_directDraw = {nullptr};
static SDL3Surface g_primarySurface = {nullptr};
static SDL3Surface g_backSurface = {nullptr};
static SDL3Surface g_spriteSurface = {nullptr};
static IDirectDrawVtbl g_directDrawVtbl;
static IDirectDrawSurfaceVtbl g_surfaceVtbl;

// Global SDL3 context is defined above as static

// All surface methods are now properly implemented with correct signatures

// Surface method implementations
// Surface method implementations
HRESULT STDMETHODCALLTYPE Surface_QueryInterface(void* This, REFIID riid, void** ppvObject);
ULONG STDMETHODCALLTYPE Surface_AddRef(void* This);
ULONG STDMETHODCALLTYPE Surface_Release(void* This);

// Surface methods are now defined in dummy_directdraw.cpp

// Function to set up DirectDraw surface interception
void SetupDirectDrawSurfaces() {
    LogMessage("Setting up DirectDraw surfaces...");
    
    // CRITICAL: Initialize vtables FIRST before any surface assignments
    SetupDirectDrawVirtualTable();
    SetupSurfaceVirtualTables();
    
    // Initialize primary surface
    g_primarySurface.lpVtbl = &g_surfaceVtbl;
    g_primarySurface.surface = SDL_CreateSurface(g_sdlContext.gameWidth, g_sdlContext.gameHeight, SDL_PIXELFORMAT_RGBA8888);
    g_primarySurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_primarySurface.surface);
    g_primarySurface.isPrimary = true;
    g_primarySurface.isBackBuffer = false;
    g_primarySurface.isSprite = false;
    g_primarySurface.refCount = 1;
    g_primarySurface.locked = false;
    g_primarySurface.lockFlags = 0;
    
    // Initialize back buffer surface
    g_backSurface.lpVtbl = &g_surfaceVtbl;
    g_backSurface.surface = SDL_CreateSurface(640, 480, SDL_PIXELFORMAT_RGBA8888);
    g_backSurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_backSurface.surface);
    g_backSurface.isPrimary = false;
    g_backSurface.isBackBuffer = true;
    g_backSurface.isSprite = false;
    g_backSurface.refCount = 1;
    g_backSurface.locked = false;
    g_backSurface.lockFlags = 0;
    
    // Initialize sprite surface
    g_spriteSurface.lpVtbl = &g_surfaceVtbl;
    g_spriteSurface.surface = SDL_CreateSurface(256, 256, SDL_PIXELFORMAT_RGBA8888);
    g_spriteSurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_spriteSurface.surface);
    g_spriteSurface.isPrimary = false;
    g_spriteSurface.isBackBuffer = false;
    g_spriteSurface.isSprite = true;
    g_spriteSurface.refCount = 1;
    g_spriteSurface.locked = false;
    g_spriteSurface.lockFlags = 0;
    
    // Initialize DirectDraw object
    LogMessage("About to set DirectDraw vtable...");
    char vtableDebug[256];
    sprintf_s(vtableDebug, sizeof(vtableDebug), "g_directDrawVtbl address: %p, first method: %p", 
             &g_directDrawVtbl, g_directDrawVtbl.QueryInterface);
    LogMessage(vtableDebug);
    
    g_directDraw.lpVtbl = &g_directDrawVtbl;
    g_directDraw.initialized = true;
    g_directDraw.primarySurface = &g_primarySurface;
    g_directDraw.backSurface = &g_backSurface;
    g_directDraw.spriteSurface = &g_spriteSurface;
    g_directDraw.refCount = 1;
    
    sprintf_s(vtableDebug, sizeof(vtableDebug), "DirectDraw object vtable set to: %p", g_directDraw.lpVtbl);
    LogMessage(vtableDebug);
    
    LogMessage("DirectDraw surfaces initialized successfully");
    
    // Debug: Verify DirectDraw object is properly initialized
    char ddObjectDebug[256];
    sprintf_s(ddObjectDebug, sizeof(ddObjectDebug),
        "DIRECTDRAW OBJECT DEBUG: lpVtbl=%p initialized=%d refCount=%d",
        g_directDraw.lpVtbl, g_directDraw.initialized, g_directDraw.refCount);
    LogMessage(ddObjectDebug);
    
    // Debug: Verify surface objects are properly initialized
    char surfaceDebug[512];
    sprintf_s(surfaceDebug, sizeof(surfaceDebug),
        "SURFACE DEBUG: Primary lpVtbl=%p surface=%p texture=%p | Back lpVtbl=%p surface=%p texture=%p",
        g_primarySurface.lpVtbl, g_primarySurface.surface, g_primarySurface.texture,
        g_backSurface.lpVtbl, g_backSurface.surface, g_backSurface.texture);
    LogMessage(surfaceDebug);
}

// Exception handler for debugging crashes
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* pExceptionInfo) {
    LogMessage("*** GAME CRASHED - Exception handler triggered ***");
    
    char buffer[512];
    sprintf_s(buffer, sizeof(buffer), 
        "CRASH INFO: Exception Code: 0x%08X, Address: 0x%08X", 
        pExceptionInfo->ExceptionRecord->ExceptionCode,
        (DWORD)pExceptionInfo->ExceptionRecord->ExceptionAddress);
    LogMessage(buffer);

    // Try to get more context
    CONTEXT* ctx = pExceptionInfo->ContextRecord;
    sprintf_s(buffer, sizeof(buffer),
        "REGISTERS: EAX=0x%08X, EBX=0x%08X, ECX=0x%08X, EDX=0x%08X, ESP=0x%08X, EIP=0x%08X",
        ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esp, ctx->Eip);
    LogMessage(buffer);
    
    // CRITICAL: Check if our vtable got corrupted after crash
    char postCrashVtable[256];
    sprintf_s(postCrashVtable, sizeof(postCrashVtable), 
             "POST-CRASH VTABLE: g_directDrawVtbl.QueryInterface=%p", g_directDrawVtbl.QueryInterface);
    LogMessage(postCrashVtable);
    
    // Check DirectDraw global pointer
    void** pDirectDraw = (void**)0x424758;
    if (!IsBadReadPtr(pDirectDraw, sizeof(void*))) {
        SDL3DirectDraw* crashDD = (SDL3DirectDraw*)*pDirectDraw;
        char crashDD_info[256];
        sprintf_s(crashDD_info, sizeof(crashDD_info), 
                 "POST-CRASH DirectDraw global: %p, vtbl=%p", 
                 crashDD, crashDD ? crashDD->lpVtbl : nullptr);
        LogMessage(crashDD_info);
    }
    
    // CRITICAL: Check surface object vtable pointers at crash time
    void** pPrimarySurface = (void**)0x424750;
    if (!IsBadReadPtr(pPrimarySurface, sizeof(void*))) {
        SDL3Surface* crashSurface = (SDL3Surface*)*pPrimarySurface;
        char crashSurface_info[256];
        sprintf_s(crashSurface_info, sizeof(crashSurface_info), 
                 "POST-CRASH Primary Surface: %p, vtbl=%p (expected=%p)", 
                 crashSurface, crashSurface ? crashSurface->lpVtbl : nullptr, &g_surfaceVtbl);
        LogMessage(crashSurface_info);
        
        // Check what's at the exact crash offset
        if (crashSurface && crashSurface->lpVtbl) {
            void** surfaceVtbl = (void**)crashSurface->lpVtbl;
            void* methodAt96 = *(surfaceVtbl + 24);  // IsLost at offset 96
            char crashMethod_info[256];
            sprintf_s(crashMethod_info, sizeof(crashMethod_info), 
                     "POST-CRASH Surface vtable method@96: %p (expected=%p)", 
                     methodAt96, Surface_IsLost_Raw);
            LogMessage(crashMethod_info);
            
            // CRITICAL: Check if surface vtable is our global vtable
            if (crashSurface->lpVtbl == &g_surfaceVtbl) {
                LogMessage("POST-CRASH: Surface is using OUR global vtable");
            } else {
                LogMessage("*** SMOKING GUN: Surface is using DIFFERENT vtable! ***");
                char vtableAddr[256];
                sprintf_s(vtableAddr, sizeof(vtableAddr), 
                         "Surface vtable: %p, Our vtable: %p", 
                         crashSurface->lpVtbl, &g_surfaceVtbl);
                LogMessage(vtableAddr);
>>>>>>> Stashed changes
            }
        }
    }
    return gameHwnd;
}

HRESULT WINAPI Hook_DirectDrawCreate(void* lpGUID, void** lplpDD, void* pUnkOuter) {
    LogMessage("*** Hook_DirectDrawCreate called - intercepting DirectDraw creation ***");
    *lplpDD = GetFakeDirectDraw();
    return DD_OK;
}

LONG WINAPI Hook_SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong) {
    if (nIndex == GWLP_WNDPROC) {
        LogMessage("Hook_SetWindowLongA: Intercepted game's attempt to set a new window procedure.");
        
        // Store the game's intended window procedure so our hook can call it.
        SetOriginalWindowProc((WNDPROC)dwNewLong);
        
        // IMPORTANT: Do NOT call the original SetWindowLongA for GWLP_WNDPROC.
        // Doing so would overwrite our hook. We've already subclassed the window
        // in CreateSDL3Context, and our goal here is just to capture the game's
        // intended procedure. We can return the handle to our own hook,
        // which mimics the behavior of SetWindowLongA returning the previous WNDPROC.
        return (LONG)(LONG_PTR)InterceptedWindowProc;
    }
    
    // For any other nIndex value, pass the call through to the original function.
    return original_set_window_long_a(hWnd, nIndex, dwNewLong);
}

BOOL WINAPI Hook_ProcessInputHistory() {
    PollSDLEvents();
    BOOL result = original_process_input_history();
    RenderGame();
    return result;
}

<<<<<<< Updated upstream
// --- Initialization and Cleanup ---
=======
void RenderFrame() {
    if (!g_sdlContext.initialized || !g_sdlContext.renderer) {
        return;
    }
    
    // Clear the renderer to prevent visual artifacts
    SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdlContext.renderer);
    
    // Get the window size for scaling
    int windowWidth, windowHeight;
    SDL_GetWindowSize(g_sdlContext.window, &windowWidth, &windowHeight);
    
    // Calculate aspect ratio preserving scaling
    float gameAspect = (float)g_sdlContext.gameWidth / g_sdlContext.gameHeight;
    float windowAspect = (float)windowWidth / windowHeight;
    
    SDL_FRect dstRect = {0};
    if (windowAspect > gameAspect) {
        // Window is wider than game aspect
        dstRect.h = (float)windowHeight;
        dstRect.w = dstRect.h * gameAspect;
        dstRect.x = (windowWidth - dstRect.w) / 2;
        dstRect.y = 0;
    } else {
        // Window is taller than game aspect
        dstRect.w = (float)windowWidth;
        dstRect.h = dstRect.w / gameAspect;
        dstRect.x = 0;
        dstRect.y = (windowHeight - dstRect.h) / 2;
    }
    
    // Render the game's primary surface texture, which should contain the latest frame
    if (g_primarySurface.texture) {
        SDL_RenderTexture(g_sdlContext.renderer, g_primarySurface.texture, NULL, &dstRect);
    }
    
    // Present the renderer
    SDL_RenderPresent(g_sdlContext.renderer);
}

// SDL3 Window Subclassing Implementation (Moon Lights 2 approach)
LRESULT CALLBACK SDL3GameWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Forward ONLY key messages to the game's window procedure
    // Let mouse messages go through SDL3's original window procedure
    switch (uMsg) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            // Reduced spam: Only log first few messages or special keys
            static int keyMsgCount = 0;
            keyMsgCount++;
            if (keyMsgCount <= 5 || wParam == VK_F9 || wParam == VK_ESCAPE) {
                char buffer[256];
                sprintf_s(buffer, sizeof(buffer), "SDL3 WINDOW: Forwarding key message %u (wParam=%u) to game window procedure", uMsg, (unsigned int)wParam);
                LogMessage(buffer);
            }
            
            // CRITICAL: Let SDL3 process the keyboard event FIRST so SDL_GetKeyboardState() works
            if (g_originalSDLWindowProc) {
                CallWindowProc(g_originalSDLWindowProc, hWnd, uMsg, wParam, lParam);
            }
            
            // CRITICAL: Update input state for our input system
            UpdateInputFromWindowMessage(uMsg, wParam);
            
            // THEN forward to the game's window procedure
            return g_handleMenuCallHotkeys(hWnd, uMsg, wParam, lParam);
            
        case WM_CHAR:
        case WM_SYSCHAR:
            // Let SDL3 process character events first
            if (g_originalSDLWindowProc) {
                CallWindowProc(g_originalSDLWindowProc, hWnd, uMsg, wParam, lParam);
            }
            
            // Pass to game logic
            return g_handleMenuCallHotkeys(hWnd, uMsg, wParam, lParam);
            
        case WM_ACTIVATEAPP:
        case WM_ACTIVATE:
            // Forward activation messages to both SDL3 and game
            if (g_originalSDLWindowProc) {
                CallWindowProc(g_originalSDLWindowProc, hWnd, uMsg, wParam, lParam);
            }
            return g_handleMenuCallHotkeys(hWnd, uMsg, wParam, lParam);
            
        default:
            // For all other messages, just use SDL3's original procedure
            if (g_originalSDLWindowProc) {
                return CallWindowProc(g_originalSDLWindowProc, hWnd, uMsg, wParam, lParam);
            }
            return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
}

void SetupWindowSubclassing(HWND sdlHwnd) {
    LogMessage("Setting up SDL3 window subclassing for message forwarding...");
    
    // Store the original SDL window procedure
    g_originalSDLWindowProc = (WNDPROC)GetWindowLongPtrW(sdlHwnd, GWLP_WNDPROC);
    
    if (!g_originalSDLWindowProc) {
        LogMessage("ERROR: Failed to get original SDL window procedure");
        return;
    }
    
    // Replace with our custom window procedure
    SetWindowLongPtrW(sdlHwnd, GWLP_WNDPROC, (LONG_PTR)SDL3GameWindowProc);
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "Window subclassing complete: Original proc=%p, New proc=%p", 
             g_originalSDLWindowProc, SDL3GameWindowProc);
    LogMessage(buffer);
}

void UpdateInputFromWindowMessage(UINT uMsg, WPARAM wParam) {
    // Simple input state tracking - can be expanded later
    static bool keyStates[256] = {false};
    
    switch (uMsg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam < 256) {
                keyStates[wParam] = true;
            }
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wParam < 256) {
                keyStates[wParam] = false;
            }
            break;
    }
}

bool InitializeSDL3() {
    LogMessage("Initializing SDL3 context...");
    
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        LogMessage("SDL_Init failed");
        return false;
    }
    
    // Set up event handling
    SDL_SetEventFilter(EventFilter, NULL);
    
    // Initialize context values
    g_sdlContext.gameWidth = 256;
    g_sdlContext.gameHeight = 240;
    g_sdlContext.windowWidth = 640;
    g_sdlContext.windowHeight = 480;
    g_sdlContext.initialized = true;
    
    return true;
}

void InitializeSurfaces() {
    LogMessage("Initializing DirectDraw surfaces...");
    
    // Initialize primary surface
    g_primarySurface.lpVtbl = &g_surfaceVtbl;
    g_primarySurface.surface = SDL_CreateSurface(g_sdlContext.gameWidth, g_sdlContext.gameHeight, SDL_PIXELFORMAT_RGBA8888);
    g_primarySurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_primarySurface.surface);
    g_primarySurface.isPrimary = true;
    g_primarySurface.isBackBuffer = false;
    g_primarySurface.isSprite = false;
    g_primarySurface.refCount = 1;
    g_primarySurface.locked = false;
    g_primarySurface.lockFlags = 0;
    
    // Initialize back buffer surface
    g_backSurface.lpVtbl = &g_surfaceVtbl;
    g_backSurface.surface = SDL_CreateSurface(g_sdlContext.gameWidth, g_sdlContext.gameHeight, SDL_PIXELFORMAT_RGBA8888);
    g_backSurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_backSurface.surface);
    g_backSurface.isPrimary = false;
    g_backSurface.isBackBuffer = true;
    g_backSurface.isSprite = false;
    g_backSurface.refCount = 1;
    g_backSurface.locked = false;
    g_backSurface.lockFlags = 0;
    
    // Initialize sprite surface
    g_spriteSurface.lpVtbl = &g_surfaceVtbl;
    g_spriteSurface.surface = SDL_CreateSurface(g_sdlContext.gameWidth, g_sdlContext.gameHeight, SDL_PIXELFORMAT_RGBA8888);
    g_spriteSurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_spriteSurface.surface);
    g_spriteSurface.isPrimary = false;
    g_spriteSurface.isBackBuffer = false;
    g_spriteSurface.isSprite = true;
    g_spriteSurface.refCount = 1;
    g_spriteSurface.locked = false;
    g_spriteSurface.lockFlags = 0;
    
    // No palette needed for 32-bit RGBA surfaces
}

bool CreateSDL3Window(HWND gameHwnd) {
    LogMessage("Creating SDL3 window...");
    
    // This function no longer needs the gameHwnd, but we keep the signature for now
    // to avoid breaking other parts of the code during refactoring.
    (void)gameHwnd;

    // Create our SDL window
    g_sdlContext.window = SDL_CreateWindow(
        "Fighter Maker 2nd - SDL3",
        g_sdlContext.windowWidth,
        g_sdlContext.windowHeight,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
    );

    if (!g_sdlContext.window) {
        LogMessage("Failed to create SDL window");
        return false;
    }
    
    // Set up Windows message hook
    SDL_SetWindowsMessageHook(WindowsMessageHook, NULL);
    
    return true;
}

// Get the native window handle if needed
HWND GetNativeWindowHandle(SDL_Window* window) {
    if (!window) return NULL;
    
    #if defined(SDL_PLATFORM_WIN32)
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    #else
    return NULL;
    #endif
}

// Window event callback
int WINAPI WindowEventWatch(void* userdata, SDL_Event* event) {
    if (!event) return 0;
    
    switch (event->type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (g_gameWindow) {
                PostMessage(g_gameWindow, WM_CLOSE, 0, 0);
            }
            return 0;
            
        case SDL_EVENT_WINDOW_RESIZED:
            RenderFrame();
            return 0;
    }
    
    return 1;
}

// Event filter callback
bool EventFilter(void* userdata, SDL_Event* event) {
    if (!event) return false;
    
    switch (event->type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (g_gameWindow) {
                PostMessage(g_gameWindow, WM_CLOSE, 0, 0);
            }
            return false;
            
        case SDL_EVENT_WINDOW_RESIZED:
            RenderFrame();
            return false;
    }
    
    return true;
}

// Windows message hook callback
bool WindowsMessageHook(void* userdata, MSG* msg) {
    // Forward relevant messages to our window proc
    if (msg && msg->hwnd == g_gameWindow) {
        WindowProc_Hook(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    }
    return true;
}

bool CreateSDL3Renderer() {
    if (g_sdlContext.renderer) {
        return true;
    }

    if (!g_sdlContext.window) {
        LogMessage("ERROR: Cannot create renderer - no SDL3 window available");
        return false;
    }
    
    // Try to create renderer with DirectX 11 backend first
    g_sdlContext.renderer = SDL_CreateRenderer(g_sdlContext.window, "direct3d11");
    
    if (!g_sdlContext.renderer) {
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "DirectX 11 renderer failed: %s", SDL_GetError());
        LogMessage(buffer);

        // Fall back to default renderer
        LogMessage("Falling back to default renderer...");
        g_sdlContext.renderer = SDL_CreateRenderer(g_sdlContext.window, nullptr);
        
        if (!g_sdlContext.renderer) {
            sprintf_s(buffer, sizeof(buffer), "Default renderer also failed: %s", SDL_GetError());
            LogMessage(buffer);
            return false;
        }
        
        LogMessage("SDL3 default renderer created successfully");
    } else {
        LogMessage("SDL3 DirectX 11 renderer created successfully");
    }

    // Enable VSync
    SDL_SetRenderVSync(g_sdlContext.renderer, 1);
    
    return true;
}

bool CreateSDL3Textures() {
    LogMessage("Creating SDL3 textures...");
    
    // Create game buffer surface and texture (256x240)
    SDL_Surface* gameSurface = SDL_CreateSurface(
        g_sdlContext.gameWidth,
        g_sdlContext.gameHeight,
        SDL_PIXELFORMAT_RGBA8888
    );
    
    if (!gameSurface) {
        LogMessage("Failed to create game surface");
        return false;
    }
    
    // Create texture from surface (32-bit RGBA, no palette needed)
    g_sdlContext.gameBuffer = SDL_CreateTextureFromSurface(g_sdlContext.renderer, gameSurface);
    if (!g_sdlContext.gameBuffer) {
        LogMessage("Failed to create game buffer texture");
        SDL_DestroySurface(gameSurface);
        return false;
    }
    
    // Store the surface for later use  
    g_sdlContext.gameSurface = gameSurface;
    
    // Create back buffer surface and texture (640x480)
    SDL_Surface* backSurface = SDL_CreateSurface(
        640, 480,
        SDL_PIXELFORMAT_RGBA8888
    );
    
    if (!backSurface) {
        LogMessage("Failed to create back buffer surface");
        return false;
    }
    
    // Create texture from surface (32-bit RGBA, no palette needed)
    g_sdlContext.backBuffer = SDL_CreateTextureFromSurface(g_sdlContext.renderer, backSurface);
    if (!g_sdlContext.backBuffer) {
        LogMessage("Failed to create back buffer texture");
        SDL_DestroySurface(backSurface);
        return false;
    }
    
    // Store the back buffer surface
    g_sdlContext.backSurface = backSurface;
    
    // Create sprite buffer surface and texture (256x256)
    SDL_Surface* spriteSurface = SDL_CreateSurface(
        256, 256,
        SDL_PIXELFORMAT_RGBA8888
    );
    
    if (!spriteSurface) {
        LogMessage("Failed to create sprite buffer surface");
        return false;
    }
    
    // Create texture from surface (32-bit RGBA, no palette needed)
    g_sdlContext.spriteBuffer = SDL_CreateTextureFromSurface(g_sdlContext.renderer, spriteSurface);
    if (!g_sdlContext.spriteBuffer) {
        LogMessage("Failed to create sprite buffer texture");
        SDL_DestroySurface(spriteSurface);
        return false;
    }
    
    // Store the sprite buffer surface
    g_sdlContext.spriteSurface = spriteSurface;
    
    return true;
}

void SetupDirectDrawReplacement() {
    LogMessage("Setting up DirectDraw replacement with verified addresses...");
    
    // Debug: Check our DirectDraw object before assignment
    char debugBuffer[512];
    sprintf_s(debugBuffer, sizeof(debugBuffer), 
             "BEFORE ASSIGNMENT: g_directDraw at %p, vtbl=%p, initialized=%d", 
             &g_directDraw, g_directDraw.lpVtbl, g_directDraw.initialized);
    LogMessage(debugBuffer);
    
    // Set up DirectDraw global variables using IDA-verified addresses
    void** pDirectDraw = (void**)0x424758;       // g_direct_draw
    void** pPrimarySurface = (void**)0x424750;   // g_dd_primary_surface  
    void** pBackBuffer = (void**)0x424754;       // g_dd_back_buffer
    
    if (!IsBadWritePtr(pDirectDraw, sizeof(void*))) {
        void* oldValue = *pDirectDraw;
        
        // VTABLE PRE-ASSIGNMENT CHECK
        char preAssignCheck[256];
        sprintf_s(preAssignCheck, sizeof(preAssignCheck), 
                 "PRE-ASSIGNMENT: g_directDrawVtbl.QueryInterface=%p", g_directDrawVtbl.QueryInterface);
        LogMessage(preAssignCheck);
        
        // CRITICAL FIX: Fully re-initialize DirectDraw object here
        g_directDraw.lpVtbl = &g_directDrawVtbl;
        g_directDraw.initialized = true;
        g_directDraw.primarySurface = &g_primarySurface;
        g_directDraw.backSurface = &g_backSurface;
        g_directDraw.spriteSurface = &g_spriteSurface;
        g_directDraw.refCount = 1;
        
        sprintf_s(debugBuffer, sizeof(debugBuffer), 
                 "FIXED: g_directDraw at %p, vtbl=%p, initialized=%d", 
                 &g_directDraw, g_directDraw.lpVtbl, g_directDraw.initialized);
        LogMessage(debugBuffer);
        
        *pDirectDraw = &g_directDraw;
        
        // VTABLE POST-ASSIGNMENT CHECK
        char postAssignCheck[256];
        sprintf_s(postAssignCheck, sizeof(postAssignCheck), 
                 "POST-ASSIGNMENT: g_directDrawVtbl.QueryInterface=%p", g_directDrawVtbl.QueryInterface);
        LogMessage(postAssignCheck);
        
        if (g_directDrawVtbl.QueryInterface == (void*)0xFFFFFFFF) {
            LogMessage("*** CRITICAL: VTABLE CORRUPTED DURING ASSIGNMENT! ***");
        }
        
        // Verify the assignment worked and read back the vtable
        SDL3DirectDraw* assignedDD = (SDL3DirectDraw*)*pDirectDraw;
        sprintf_s(debugBuffer, sizeof(debugBuffer), 
                 "ASSIGNMENT: 0x424758: %p -> %p, readback vtbl=%p", 
                 oldValue, &g_directDraw, assignedDD ? assignedDD->lpVtbl : nullptr);
        LogMessage(debugBuffer);
    }
    
    if (!IsBadWritePtr(pPrimarySurface, sizeof(void*))) {
        void* oldValue = *pPrimarySurface;
        g_primarySurface.texture = g_sdlContext.gameBuffer;
        *pPrimarySurface = &g_primarySurface;
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "Set primary surface pointer at 0x424750: %p -> %p (vtbl=%p)", 
                 oldValue, &g_primarySurface, g_primarySurface.lpVtbl);
        LogMessage(buffer);
    }
    
    if (!IsBadWritePtr(pBackBuffer, sizeof(void*))) {
        void* oldValue = *pBackBuffer;
        g_backSurface.texture = g_sdlContext.backBuffer;
        *pBackBuffer = &g_backSurface;
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "Set back buffer pointer at 0x424754: %p -> %p (vtbl=%p)", 
                 oldValue, &g_backSurface, g_backSurface.lpVtbl);
        LogMessage(buffer);
    }
    
    // Set up resolution globals using IDA-verified addresses
    short* pStageWidth = (short*)0x4452B8;   // g_stage_width_pixels
    short* pStageHeight = (short*)0x4452BA;  // g_stage_height_pixels
    int* pDestWidth = (int*)0x447F20;        // g_dest_width
    int* pDestHeight = (int*)0x447F24;       // g_dest_height
    
    if (!IsBadWritePtr(pStageWidth, sizeof(short))) {
        *pStageWidth = 256;
        LogMessage("Set g_stage_width_pixels to 256");
    }
    
    if (!IsBadWritePtr(pStageHeight, sizeof(short))) {
        *pStageHeight = 240;
        LogMessage("Set g_stage_height_pixels to 240");
    }
    
    if (!IsBadWritePtr(pDestWidth, sizeof(int))) {
        *pDestWidth = 256;
        LogMessage("Set g_dest_width to 256");
    }
    
    if (!IsBadWritePtr(pDestHeight, sizeof(int))) {
        *pDestHeight = 240;
        LogMessage("Set g_dest_height to 240");
    }
    
    LogMessage("DirectDraw SDL3 replacement setup complete");
    
    // IMMEDIATE VTABLE CHECK: Verify vtable is still valid right after setup
    char immediateCheck[256];
    sprintf_s(immediateCheck, sizeof(immediateCheck), 
             "IMMEDIATE CHECK: g_directDrawVtbl.QueryInterface=%p", g_directDrawVtbl.QueryInterface);
    LogMessage(immediateCheck);
    
    // CRITICAL TIMING DEBUG: Check vtable before sleep
    char beforeSleepCheck[256];
    sprintf_s(beforeSleepCheck, sizeof(beforeSleepCheck), 
             "BEFORE SLEEP: g_directDrawVtbl.QueryInterface=%p", g_directDrawVtbl.QueryInterface);
    LogMessage(beforeSleepCheck);
    
    // FINAL SAFETY CHECK: Add a small delay and verify our objects are still accessible
    Sleep(100);  // Give the system a moment to settle
    
    // CRITICAL TIMING DEBUG: Check vtable after sleep
    char afterSleepCheck[256];
    sprintf_s(afterSleepCheck, sizeof(afterSleepCheck), 
             "AFTER SLEEP: g_directDrawVtbl.QueryInterface=%p", g_directDrawVtbl.QueryInterface);
    LogMessage(afterSleepCheck);
    
    if (g_directDrawVtbl.QueryInterface == (void*)0xFFFFFFFF) {
        LogMessage("*** CRITICAL: VTABLE CORRUPTED DURING SLEEP! ***");
    }
    
    // Try to access our DirectDraw object like the game would
    SDL3DirectDraw* finalTestDD = (SDL3DirectDraw*)(*((void**)0x424758));
    if (finalTestDD && finalTestDD->lpVtbl) {
        LogMessage("FINAL VERIFICATION: DirectDraw object still accessible after setup");
    } else {
        LogMessage("CRITICAL ERROR: DirectDraw object became inaccessible!");
    }
    
    LogMessage("Setup complete - waiting for game to call our methods...");
    
    // CRITICAL: Verify that the game can access our objects through the global pointers
    void** testDD = (void**)0x424758;
    void** testPrimary = (void**)0x424750;
    void** testBack = (void**)0x424754;
    
    if (!IsBadReadPtr(testDD, sizeof(void*)) && *testDD) {
        SDL3DirectDraw* dd = (SDL3DirectDraw*)*testDD;
        if (!IsBadReadPtr(dd, sizeof(SDL3DirectDraw)) && dd->lpVtbl) {
            LogMessage("VERIFICATION: DirectDraw object accessible and has valid vtable");
        } else {
            LogMessage("ERROR: DirectDraw object or vtable corrupted!");
        }
    }
    
    if (!IsBadReadPtr(testPrimary, sizeof(void*)) && *testPrimary) {
        SDL3Surface* surf = (SDL3Surface*)*testPrimary;
        if (!IsBadReadPtr(surf, sizeof(SDL3Surface)) && surf->lpVtbl) {
            LogMessage("VERIFICATION: Primary surface accessible and has valid vtable");
        } else {
            LogMessage("ERROR: Primary surface or vtable corrupted!");
        }
    }
}

void SetupSurfaceVirtualTables() {
    LogMessage("Setting up surface virtual tables...");
    
    // Initialize the virtual function table - ALL 27 methods must be set
    // IUnknown methods
    g_surfaceVtbl.QueryInterface = Surface_QueryInterface;
    g_surfaceVtbl.AddRef = Surface_AddRef;
    g_surfaceVtbl.Release = Surface_Release;
    
    // IDirectDrawSurface methods (in exact vtable order)
    // IDirectDrawSurface methods (in exact vtable order)
    // Use actual function implementations with correct signatures
    g_surfaceVtbl.AddAttachedSurface = Surface_AddAttachedSurface;
    g_surfaceVtbl.AddOverlayDirtyRect = Surface_AddOverlayDirtyRect;
    g_surfaceVtbl.Blt = Surface_Blt;  // CRITICAL - implemented
    g_surfaceVtbl.BltBatch = Surface_BltBatch;
    g_surfaceVtbl.BltFast = Surface_BltFast;
    g_surfaceVtbl.DeleteAttachedSurface = Surface_DeleteAttachedSurface;
    g_surfaceVtbl.EnumAttachedSurfaces = Surface_EnumAttachedSurfaces;
    g_surfaceVtbl.EnumOverlayZOrders = Surface_EnumOverlayZOrders;
    g_surfaceVtbl.Flip = Surface_Flip;  // CRITICAL - implemented
    g_surfaceVtbl.GetAttachedSurface = Surface_GetAttachedSurface;
    g_surfaceVtbl.GetBltStatus = Surface_GetBltStatus;
    g_surfaceVtbl.GetCaps = Surface_GetCaps;
    g_surfaceVtbl.GetClipper = Surface_GetClipper;
    g_surfaceVtbl.GetColorKey = Surface_GetColorKey;
    g_surfaceVtbl.GetDC = Surface_GetDC;
    g_surfaceVtbl.GetFlipStatus = Surface_GetFlipStatus;
    g_surfaceVtbl.GetOverlayPosition = Surface_GetOverlayPosition;
    g_surfaceVtbl.GetPalette = Surface_GetPalette;
    g_surfaceVtbl.GetPixelFormat = Surface_GetPixelFormat;
    g_surfaceVtbl.GetSurfaceDesc = Surface_GetSurfaceDesc;  // CRITICAL - implemented  
    g_surfaceVtbl.Initialize = Surface_Initialize;
    g_surfaceVtbl.IsLost = (HRESULT (STDMETHODCALLTYPE *)(void*))Surface_IsLost_Raw;
    g_surfaceVtbl.Lock = Surface_Lock;  // CRITICAL - implemented
    g_surfaceVtbl.ReleaseDC = Surface_ReleaseDC;
    g_surfaceVtbl.Restore = Surface_Restore;
    g_surfaceVtbl.SetClipper = Surface_SetClipper;
    g_surfaceVtbl.SetColorKey = Surface_SetColorKey;
    g_surfaceVtbl.SetOverlayPosition = Surface_SetOverlayPosition;
    g_surfaceVtbl.SetPalette = Surface_SetPalette;  // CRITICAL - implemented
    g_surfaceVtbl.Unlock = Surface_Unlock;  // CRITICAL - implemented  
    g_surfaceVtbl.UpdateOverlay = Surface_UpdateOverlay;
    g_surfaceVtbl.UpdateOverlayDisplay = Surface_UpdateOverlayDisplay;
    g_surfaceVtbl.UpdateOverlayZOrder = Surface_UpdateOverlayZOrder;
    
    LogMessage("DirectDraw surface virtual function tables initialized successfully");
    
    // Debug: Verify surface vtable function pointers are valid
    char surfaceVtableDebug[512];
    sprintf_s(surfaceVtableDebug, sizeof(surfaceVtableDebug), 
        "SURFACE VTABLE DEBUG: QueryInterface=%p AddRef=%p Release=%p Blt=%p Lock=%p Unlock=%p Flip=%p GetAttachedSurface=%p",
        g_surfaceVtbl.QueryInterface, g_surfaceVtbl.AddRef, g_surfaceVtbl.Release, 
        g_surfaceVtbl.Blt, g_surfaceVtbl.Lock, g_surfaceVtbl.Unlock, g_surfaceVtbl.Flip, g_surfaceVtbl.GetAttachedSurface);
    LogMessage(surfaceVtableDebug);
    
    // CRITICAL: Verify GetAttachedSurface is at correct offset 0x30 (method index 12)
    void** surfaceVtableBase = (void**)&g_surfaceVtbl;
    void* actualMethodAt0x30 = *(surfaceVtableBase + 12);  // What's actually stored at offset 0x30
    char offsetVerify[256];
    sprintf_s(offsetVerify, sizeof(offsetVerify),
        "SURFACE OFFSET VERIFY: Base=%p, stored@0x30=%p, expected=%p %s",
        surfaceVtableBase, actualMethodAt0x30, g_surfaceVtbl.GetAttachedSurface, 
        (actualMethodAt0x30 == g_surfaceVtbl.GetAttachedSurface) ? "?��?" : "?��?");
    LogMessage(offsetVerify);
    
    // CRITICAL: Check what's at offset 96 (IsLost) and 108 (Restore) - THE CRASH POINTS!
    void* actualIsLostAt96 = *(surfaceVtableBase + 24);   // Offset 96 = method 24 (IsLost)
    void* actualRestoreAt108 = *(surfaceVtableBase + 27); // Offset 108 = method 27 (Restore)
    char crashPointCheck[512];
    sprintf_s(crashPointCheck, sizeof(crashPointCheck),
        "CRASH POINT CHECK: IsLost@96=%p (expected=%p) Restore@108=%p (expected=%p)",
        actualIsLostAt96, Surface_IsLost_Raw, actualRestoreAt108, g_surfaceVtbl.Restore);
    LogMessage(crashPointCheck);
    
    if (actualIsLostAt96 == (void*)0xFFFFFFFF || actualRestoreAt108 == (void*)0xFFFFFFFF) {
        LogMessage("*** FOUND THE BUG: SURFACE VTABLE HAS 0xFFFFFFFF AT CRASH OFFSETS! ***");
    }
}

void SetupDirectDrawVirtualTable() {
    LogMessage("Setting up DirectDraw virtual table...");
    
    // IUnknown methods
    g_directDrawVtbl.QueryInterface = DirectDraw_QueryInterface;
    g_directDrawVtbl.AddRef = DirectDraw_AddRef;
    g_directDrawVtbl.Release = DirectDraw_Release;
    
    // IDirectDraw methods - implement essential ones, stub the rest with proper casts
    g_directDrawVtbl.Compact = (HRESULT (STDMETHODCALLTYPE *)(void*))DirectDraw_Stub;                     // 3
    g_directDrawVtbl.CreateClipper = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, void**, void*))DirectDraw_Stub;  // 4
    g_directDrawVtbl.CreatePalette = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, void*, void**, void*))DirectDraw_Stub;  // 5
    g_directDrawVtbl.CreateSurface = DirectDraw_CreateSurface;  // 6 - CRITICAL at offset 0x18h
    g_directDrawVtbl.DuplicateSurface = (HRESULT (STDMETHODCALLTYPE *)(void*, void*, void**))DirectDraw_Stub;  // 7
    g_directDrawVtbl.EnumDisplayModes = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, void*, void*, void*))DirectDraw_Stub;  // 8
    g_directDrawVtbl.EnumSurfaces = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, void*, void*, void*))DirectDraw_Stub;  // 9
    g_directDrawVtbl.FlipToGDISurface = (HRESULT (STDMETHODCALLTYPE *)(void*))DirectDraw_Stub;             // 10
    g_directDrawVtbl.GetCaps = DirectDraw_GetCaps;        // 11 - IMPLEMENTED
    g_directDrawVtbl.GetDisplayMode = DirectDraw_GetDisplayMode;       // 12 - IMPLEMENTED
    g_directDrawVtbl.GetFourCCCodes = (HRESULT (STDMETHODCALLTYPE *)(void*, LPDWORD, LPDWORD))DirectDraw_Stub;  // 13
    g_directDrawVtbl.GetGDISurface = (HRESULT (STDMETHODCALLTYPE *)(void*, void**))DirectDraw_Stub;       // 14
    g_directDrawVtbl.GetMonitorFrequency = DirectDraw_GetMonitorFrequency;  // 15 - IMPLEMENTED
    g_directDrawVtbl.GetScanLine = (HRESULT (STDMETHODCALLTYPE *)(void*, LPDWORD))DirectDraw_Stub;        // 16
    g_directDrawVtbl.GetVerticalBlankStatus = DirectDraw_GetVerticalBlankStatus;  // 17 - IMPLEMENTED
    g_directDrawVtbl.Initialize = (HRESULT (STDMETHODCALLTYPE *)(void*, GUID*))DirectDraw_Stub;           // 18
    g_directDrawVtbl.RestoreDisplayMode = (HRESULT (STDMETHODCALLTYPE *)(void*))DirectDraw_Stub;          // 19
    g_directDrawVtbl.SetCooperativeLevel = DirectDraw_SetCooperativeLevel;  // 20 - CRITICAL at offset 0x50h
    g_directDrawVtbl.SetDisplayMode = DirectDraw_SetDisplayMode;  // 21 - CRITICAL at offset 0x54h
    g_directDrawVtbl.WaitForVerticalBlank = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, HANDLE))DirectDraw_Stub;  // 22
    
    LogMessage("DirectDraw virtual function table initialized successfully");
    
    // CRITICAL: Validate ALL vtable entries are valid function pointers
    char validationBuffer[1024];
    sprintf_s(validationBuffer, sizeof(validationBuffer),
        "VTABLE VALIDATION: QI=%p AddRef=%p Release=%p GetCaps=%p GetDisplayMode=%p GetMonitorFreq=%p",
        g_directDrawVtbl.QueryInterface, g_directDrawVtbl.AddRef, g_directDrawVtbl.Release,
        g_directDrawVtbl.GetCaps, g_directDrawVtbl.GetDisplayMode, g_directDrawVtbl.GetMonitorFrequency);
    LogMessage(validationBuffer);
    
    // Check for any NULL pointers in critical methods
    if (!g_directDrawVtbl.QueryInterface || !g_directDrawVtbl.AddRef || !g_directDrawVtbl.Release ||
        !g_directDrawVtbl.CreateSurface || !g_directDrawVtbl.SetCooperativeLevel || !g_directDrawVtbl.SetDisplayMode) {
        LogMessage("*** CRITICAL ERROR: NULL pointers found in DirectDraw vtable! ***");
    }
    
    // Debug: Verify DirectDraw vtable function pointers are valid at critical offsets
    char ddVtableDebug[512];
    sprintf_s(ddVtableDebug, sizeof(ddVtableDebug), 
        "DIRECTDRAW VTABLE: CreateSurface[6]=%p SetCooperativeLevel[20]=%p SetDisplayMode[21]=%p",
        g_directDrawVtbl.CreateSurface, g_directDrawVtbl.SetCooperativeLevel, g_directDrawVtbl.SetDisplayMode);
    LogMessage(ddVtableDebug);
    
    // Verify offset calculations
    void** vtableBase = (void**)&g_directDrawVtbl;
    char offsetDebug[256];
    sprintf_s(offsetDebug, sizeof(offsetDebug),
        "OFFSET VERIFICATION: Base=%p CreateSurface@0x18=%p SetCoop@0x50=%p SetDisplay@0x54=%p",
        vtableBase, vtableBase + 6, vtableBase + 20, vtableBase + 21);
    LogMessage(offsetDebug);
}

// DirectDraw Surface Method Implementations
HRESULT STDMETHODCALLTYPE Surface_QueryInterface(void* This, REFIID riid, void** ppvObject) {
    LogMessage("*** Surface_QueryInterface called ***");
    if (!ppvObject) return E_POINTER;
    *ppvObject = This;
    ((SDL3Surface*)This)->refCount++;
    return S_OK;
}

ULONG STDMETHODCALLTYPE Surface_AddRef(void* This) {
    LogMessage("Surface_AddRef called");
    SDL3Surface* surface = (SDL3Surface*)This;
    return ++surface->refCount;
}

ULONG STDMETHODCALLTYPE Surface_Release(void* This) {
    LogMessage("Surface_Release called");
    SDL3Surface* surface = (SDL3Surface*)This;
    ULONG ref = --surface->refCount;
    if (ref == 0) {
        if (surface->surface) {
            SDL_DestroySurface(surface->surface);
            surface->surface = nullptr;
        }
        if (surface->texture) {
            SDL_DestroyTexture(surface->texture);
            surface->texture = nullptr;
        }
    }
    return ref;
}

// Stub implementations for unused methods
HRESULT STDMETHODCALLTYPE Surface_AddAttachedSurface(void* This, void* lpDDSAttachedSurface) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_AddOverlayDirtyRect(void* This, LPRECT lpRect) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_BltBatch(void* This, void* lpDDBltBatch, DWORD dwCount, DWORD dwFlags) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_BltFast(void* This, DWORD dwX, DWORD dwY, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_DeleteAttachedSurface(void* This, DWORD dwFlags, void* lpDDSAttachedSurface) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_EnumAttachedSurfaces(void* This, LPVOID lpContext, void* lpEnumSurfacesCallback) {
    return DDERR_UNSUPPORTED;
}

// Surface methods are now defined in dummy_directdraw.cpp

HRESULT STDMETHODCALLTYPE Surface_GetBltStatus(void* This, DWORD dwFlags) {
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_GetCaps(void* This, void* lpDDSCaps) {
    LogMessage("Surface_GetCaps called");
    
    if (!This) {
        LogMessage("Surface_GetCaps: Invalid This pointer");
        return DDERR_INVALIDPARAMS;
    }
    
    if (!lpDDSCaps) {
        LogMessage("Surface_GetCaps: Invalid caps pointer");
        return DDERR_INVALIDPARAMS;
    }
    
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface->lpVtbl) {
        LogMessage("Surface_GetCaps: Corrupted surface object");
        return DDERR_INVALIDOBJECT;
    }
    
    // Fill in surface capabilities
    struct DDSCAPS {
        DWORD dwCaps;
    };
    
    DDSCAPS* caps = (DDSCAPS*)lpDDSCaps;
    caps->dwCaps = 0;
    
    if (surface->isPrimary) {
        caps->dwCaps |= 0x200; // DDSCAPS_PRIMARYSURFACE
        LogMessage("Surface_GetCaps: Primary surface caps");
    }
    
    if (surface->isBackBuffer) {
        caps->dwCaps |= 0x800; // DDSCAPS_BACKBUFFER  
        LogMessage("Surface_GetCaps: Back buffer caps");
    }
    
    caps->dwCaps |= 0x1000; // DDSCAPS_VIDEOMEMORY (pretend it's in video memory)
    
    char buffer[128];
    sprintf_s(buffer, sizeof(buffer), "Surface_GetCaps: Returning caps=0x%X", caps->dwCaps);
    LogMessage(buffer);
    
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_GetClipper(void* This, void** lplpDDClipper) {
    if (lplpDDClipper) *lplpDDClipper = nullptr;
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_GetColorKey(void* This, DWORD dwFlags, void* lpDDColorKey) {
    LogMessage("Surface_GetColorKey called (STUB)");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_GetDC(void* This, HDC* lphDC) {
    LogMessage("Surface_GetDC called (STUB)");
    // This should ideally return a DC compatible with the SDL surface
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_GetFlipStatus(void* This, DWORD dwFlags) {
    LogMessage("Surface_GetFlipStatus called (STUB)");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_GetOverlayPosition(void* This, LPLONG lplX, LPLONG lplY) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_GetPalette(void* This, void** lplpDDPalette) {
    if (lplpDDPalette) *lplpDDPalette = nullptr;
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_GetPixelFormat(void* This, void* lpDDPixelFormat) {
    LogMessage("Surface_GetPixelFormat called (STUB)");
    // This should fill out the pixel format structure
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Initialize(void* This, void* lpDD, void* lpDDSurfaceDesc) {
    return DDERR_ALREADYINITIALIZED;
}

// Raw function that just returns 0 - let's see if THIS gets called
int Surface_IsLost_Raw() {
    return 0;
}

HRESULT STDMETHODCALLTYPE Surface_IsLost(void* This) {
    LogMessage("*** BREAKTHROUGH: Surface_IsLost called - THE GAME IS USING SURFACE METHODS! ***");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_ReleaseDC(void* This, HDC hDC) {
    LogMessage("Surface_ReleaseDC called (STUB)");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Restore(void* This) {
    LogMessage("*** BREAKTHROUGH: Surface_Restore called - THE GAME IS USING SURFACE METHODS! ***");
    char debugInfo[256];
    sprintf_s(debugInfo, sizeof(debugInfo), "Surface_Restore: This=%p", This);
    LogMessage(debugInfo);
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_SetClipper(void* This, void* lpDDClipper) {
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_SetColorKey(void* This, DWORD dwFlags, void* lpDDColorKey) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_SetOverlayPosition(void* This, LONG lX, LONG lY) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_SetPalette(void* This, void* lpDDPalette) {
    // For 32-bit RGBA surfaces, palette operations are not needed
    LogMessage("Surface_SetPalette called (32-bit mode - no palette needed)");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_UpdateOverlay(void* This, LPRECT lpSrcRect, void* lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, void* lpDDOverlayFx) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_UpdateOverlayDisplay(void* This, DWORD dwFlags) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_UpdateOverlayZOrder(void* This, DWORD dwFlags, void* lpDDSReference) {
    return DDERR_UNSUPPORTED;
}

// DirectDraw Method Implementations
HRESULT STDMETHODCALLTYPE DirectDraw_QueryInterface(void* This, REFIID riid, void** ppvObject) {
    LogMessage("*** BREAKTHROUGH: DirectDraw_QueryInterface called - methods ARE being called! ***");
    char debugInfo[256];
    sprintf_s(debugInfo, sizeof(debugInfo), "DirectDraw_QueryInterface: This=%p, riid=%p, ppvObject=%p", This, riid, ppvObject);
    LogMessage(debugInfo);
    
    // Comprehensive parameter validation
    if (!This) {
        LogMessage("DirectDraw_QueryInterface: Invalid This pointer");
        return E_INVALIDARG;
    }
    
    if (!ppvObject) {
        LogMessage("DirectDraw_QueryInterface: Invalid ppvObject pointer");
        return E_POINTER;
    }
    
    // Validate our DirectDraw object structure
    SDL3DirectDraw* dd = (SDL3DirectDraw*)This;
    if (!dd->lpVtbl) {
        LogMessage("DirectDraw_QueryInterface: Corrupted DirectDraw object - no vtable");
        return E_FAIL;
    }
    
    *ppvObject = This;
    dd->refCount++;
    
    char buffer[128];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw_QueryInterface: Success, refCount=%d", dd->refCount);
    LogMessage(buffer);
    return S_OK;
}

ULONG STDMETHODCALLTYPE DirectDraw_AddRef(void* This) {
    LogMessage("DirectDraw_AddRef called");
    
    if (!This) {
        LogMessage("DirectDraw_AddRef: Invalid This pointer");
        return 0;
    }
    
    SDL3DirectDraw* dd = (SDL3DirectDraw*)This;
    if (!dd->lpVtbl) {
        LogMessage("DirectDraw_AddRef: Corrupted DirectDraw object");
        return 0;
    }
    
    ULONG newRef = ++dd->refCount;
    char buffer[128];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw_AddRef: New refCount=%d", newRef);
    LogMessage(buffer);
    return newRef;
}

ULONG STDMETHODCALLTYPE DirectDraw_Release(void* This) {
    LogMessage("DirectDraw_Release called");
    
    if (!This) {
        LogMessage("DirectDraw_Release: Invalid This pointer");
        return 0;
    }
    
    SDL3DirectDraw* dd = (SDL3DirectDraw*)This;
    if (!dd->lpVtbl) {
        LogMessage("DirectDraw_Release: Corrupted DirectDraw object");
        return 0;
    }
    
    if (dd->refCount == 0) {
        LogMessage("DirectDraw_Release: WARNING - refCount already 0");
        return 0;
    }
    
    ULONG newRef = --dd->refCount;
    char buffer[128];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw_Release: New refCount=%d", newRef);
    LogMessage(buffer);
    
    if (newRef == 0) {
        LogMessage("DirectDraw_Release: Object reference count reached 0");
        // Note: We don't actually free the global object
    }
    
    return newRef;
}

HRESULT STDMETHODCALLTYPE DirectDraw_SetCooperativeLevel(void* This, HWND hWnd, DWORD dwFlags) {
    LogMessage("*** BREAKTHROUGH: DirectDraw_SetCooperativeLevel called - methods ARE being called! ***");
    char debugInfo[256];
    sprintf_s(debugInfo, sizeof(debugInfo), "DirectDraw_SetCooperativeLevel: This=%p, hWnd=%p, dwFlags=0x%X", This, hWnd, dwFlags);
    LogMessage(debugInfo);
    
    // Parameter validation
    if (!This) {
        LogMessage("DirectDraw_SetCooperativeLevel: Invalid This pointer");
        return DDERR_INVALIDPARAMS;
    }
    
    SDL3DirectDraw* dd = (SDL3DirectDraw*)This;
    if (!dd->lpVtbl) {
        LogMessage("DirectDraw_SetCooperativeLevel: Corrupted DirectDraw object");
        return DDERR_INVALIDOBJECT;
    }
    
    // Validate window handle if not NULL
    if (hWnd && !IsWindow(hWnd)) {
        LogMessage("DirectDraw_SetCooperativeLevel: Invalid window handle");
        return DDERR_INVALIDPARAMS;
    }
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw_SetCooperativeLevel: This=%p hWnd=%p dwFlags=0x%X", This, hWnd, dwFlags);
    LogMessage(buffer);
    
    // Log cooperative level flags for debugging
    if (dwFlags & 0x8) LogMessage("  - DDSCL_EXCLUSIVE");
    if (dwFlags & 0x10) LogMessage("  - DDSCL_FULLSCREEN");
    if (dwFlags & 0x20) LogMessage("  - DDSCL_ALLOWMODEX");
    if (dwFlags & 0x1) LogMessage("  - DDSCL_NORMAL");
    
    LogMessage("DirectDraw_SetCooperativeLevel: Accepting all cooperative levels, returning DD_OK");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DirectDraw_SetDisplayMode(void* This, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP) {
    LogMessage("DirectDraw_SetDisplayMode called");
    
    // Parameter validation
    if (!This) {
        LogMessage("DirectDraw_SetDisplayMode: Invalid This pointer");
        return DDERR_INVALIDPARAMS;
    }
    
    SDL3DirectDraw* dd = (SDL3DirectDraw*)This;
    if (!dd->lpVtbl) {
        LogMessage("DirectDraw_SetDisplayMode: Corrupted DirectDraw object");
        return DDERR_INVALIDOBJECT;
    }
    
    // Validate display mode parameters
    if (dwWidth == 0 || dwHeight == 0) {
        LogMessage("DirectDraw_SetDisplayMode: Invalid resolution (width or height is 0)");
        return DDERR_INVALIDPARAMS;
    }
    
    if (dwBPP != 8 && dwBPP != 16 && dwBPP != 24 && dwBPP != 32) {
        char errorBuffer[128];
        sprintf_s(errorBuffer, sizeof(errorBuffer), "DirectDraw_SetDisplayMode: Unsupported bit depth: %d", dwBPP);
        LogMessage(errorBuffer);
        return DDERR_INVALIDMODE;
    }
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw_SetDisplayMode: This=%p %dx%d %d-bit", This, dwWidth, dwHeight, dwBPP);
    LogMessage(buffer);
    
    // Always accept the mode but don't actually change display - SDL3 handles this
    LogMessage("DirectDraw_SetDisplayMode: Mode accepted (SDL3 manages actual display), returning DD_OK");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DirectDraw_CreateSurface(void* This, void* lpDDSurfaceDesc, void** lplpDDSurface, void* pUnkOuter) {
    LogMessage("*** DirectDraw_CreateSurface called ***");
    
    // Parameter validation
    if (!This) {
        LogMessage("DirectDraw_CreateSurface: Invalid This pointer");
        return DDERR_INVALIDPARAMS;
    }
    
    SDL3DirectDraw* dd = (SDL3DirectDraw*)This;
    if (!dd->lpVtbl) {
        LogMessage("DirectDraw_CreateSurface: Corrupted DirectDraw object");
        return DDERR_INVALIDOBJECT;
    }
    
    if (!lplpDDSurface) {
        LogMessage("DirectDraw_CreateSurface: Invalid surface pointer parameter");
        return DDERR_INVALIDPARAMS;
    }
    
    if (pUnkOuter) {
        LogMessage("DirectDraw_CreateSurface: Aggregation not supported");
        return CLASS_E_NOAGGREGATION;
    }
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw_CreateSurface: This=%p lpDDSurfaceDesc=%p lplpDDSurface=%p", This, lpDDSurfaceDesc, lplpDDSurface);
    LogMessage(buffer);
    
    // Initialize return value to NULL for safety
    *lplpDDSurface = nullptr;
    
    // Validate surface description if provided
    if (lpDDSurfaceDesc) {
        // Basic surface desc structure (simplified)
        struct BasicSurfaceDesc {
            DWORD dwSize;
            DWORD dwFlags;
            DWORD dwHeight;
            DWORD dwWidth;
        };
        
        BasicSurfaceDesc* desc = (BasicSurfaceDesc*)lpDDSurfaceDesc;
        if (desc->dwSize < sizeof(BasicSurfaceDesc)) {
            LogMessage("DirectDraw_CreateSurface: Invalid surface description size");
            return DDERR_INVALIDPARAMS;
        }
        
        // Log surface type request
        char descBuffer[128];
        sprintf_s(descBuffer, sizeof(descBuffer), "  Surface request: %dx%d, flags=0x%X", desc->dwWidth, desc->dwHeight, desc->dwFlags);
        LogMessage(descBuffer);
    }
    
    // For now, always return the primary surface (simplified implementation)
    // In a complete implementation, we'd parse lpDDSurfaceDesc to determine which surface type to create
    *lplpDDSurface = &g_primarySurface;
    g_primarySurface.refCount++; // AddRef the returned surface
    
    char refBuffer[64];
    sprintf_s(refBuffer, sizeof(refBuffer), "  Primary surface refCount now: %d", g_primarySurface.refCount);
    LogMessage(refBuffer);
    
    LogMessage("DirectDraw_CreateSurface: Returning primary surface, DD_OK");
    return DD_OK;
}


HRESULT __stdcall DirectDraw_TestFunction() {
    LogMessage("*** DirectDraw_TestFunction called - minimal test ***");
    return DD_OK;
}

// Critical DirectDraw method implementations
HRESULT STDMETHODCALLTYPE DirectDraw_GetDisplayMode(void* This, void* lpDDSurfaceDesc) {
    LogMessage("*** DirectDraw_GetDisplayMode called ***");
    
    if (!lpDDSurfaceDesc) {
        LogMessage("DirectDraw_GetDisplayMode: Invalid surface desc pointer");
        return DDERR_INVALIDPARAMS;
    }
    
    // Fill in a basic DDSURFACEDESC structure with current display mode
    // This is a minimal implementation for 256x240 game resolution
    struct DDSURFACEDESC {
        DWORD dwSize;
        DWORD dwFlags;
        DWORD dwHeight;
        DWORD dwWidth;
        DWORD dwLinearSize;
        DWORD dwBackBufferCount;
        DWORD dwRefreshRate;
        DWORD dwBitCount;
        // ... other fields would follow
    };
    
    DDSURFACEDESC* desc = (DDSURFACEDESC*)lpDDSurfaceDesc;
    memset(desc, 0, sizeof(DDSURFACEDESC));
    desc->dwSize = sizeof(DDSURFACEDESC);
    desc->dwFlags = 0x1007; // DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | others
    desc->dwHeight = 240;
    desc->dwWidth = 256;
    desc->dwBitCount = 32; // 32-bit color
    desc->dwRefreshRate = 60;
    
    LogMessage("DirectDraw_GetDisplayMode: Returning 256x240x32 @ 60Hz");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DirectDraw_GetCaps(void* This, void* lpDDDriverCaps, void* lpDDHELCaps) {
    LogMessage("*** DirectDraw_GetCaps called ***");
    
    // Both parameters are optional but must be valid if not NULL
    if (lpDDDriverCaps) {
        // Fill in basic DirectDraw capabilities
        struct DDCAPS {
            DWORD dwSize;
            DWORD dwCaps;
            DWORD dwCaps2;
            DWORD dwCKeyCaps;
            DWORD dwFXCaps;
            DWORD dwFXAlphaCaps;
            DWORD dwPalCaps;
            DWORD dwSVCaps;
            DWORD dwAlphaBltConstBitDepths;
            DWORD dwAlphaBltPixelBitDepths;
            DWORD dwAlphaBltSurfaceBitDepths;
            DWORD dwAlphaOverlayConstBitDepths;
            DWORD dwAlphaOverlayPixelBitDepths;
            DWORD dwAlphaOverlaySurfaceBitDepths;
            DWORD dwZBufferBitDepths;
            DWORD dwVidMemTotal;
            DWORD dwVidMemFree;
            DWORD dwMaxVisibleOverlays;
            DWORD dwCurrVisibleOverlays;
            DWORD dwNumFourCCCodes;
            DWORD dwAlignBoundarySrc;
            DWORD dwAlignSizeSrc;
            DWORD dwAlignBoundaryDest;
            DWORD dwAlignSizeDest;
            DWORD dwAlignStrideAlign;
            DWORD dwRops[8];
            DWORD dwReserved1;
            DWORD dwReserved2;
            DWORD dwReserved3;
        };
        
        DDCAPS* caps = (DDCAPS*)lpDDDriverCaps;
        memset(caps, 0, sizeof(DDCAPS));
        caps->dwSize = sizeof(DDCAPS);
        caps->dwCaps = 0x11041; // Basic surface capabilities
        caps->dwVidMemTotal = 16777216; // 16MB video memory
        caps->dwVidMemFree = 16777216;
        caps->dwMaxVisibleOverlays = 1;
        
        LogMessage("DirectDraw_GetCaps: Filled driver caps");
    }
    
    if (lpDDHELCaps) {
        // HEL (Hardware Emulation Layer) caps - usually same as driver caps
        memcpy(lpDDHELCaps, lpDDDriverCaps, sizeof(DDCAPS));
        LogMessage("DirectDraw_GetCaps: Filled HEL caps");
    }
    
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DirectDraw_GetMonitorFrequency(void* This, LPDWORD lpdwFrequency) {
    LogMessage("DirectDraw_GetMonitorFrequency called");
    
    if (!lpdwFrequency) {
        LogMessage("DirectDraw_GetMonitorFrequency: Invalid frequency pointer");
        return DDERR_INVALIDPARAMS;
    }
    
    *lpdwFrequency = 60; // Standard 60Hz refresh rate
    LogMessage("DirectDraw_GetMonitorFrequency: Returning 60Hz");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DirectDraw_GetVerticalBlankStatus(void* This, LPBOOL lpbIsInVB) {
    LogMessage("DirectDraw_GetVerticalBlankStatus called");
    
    if (!lpbIsInVB) {
        LogMessage("DirectDraw_GetVerticalBlankStatus: Invalid pointer");
        return DDERR_INVALIDPARAMS;
    }
    
    *lpbIsInVB = FALSE; // Always say we're not in vertical blank
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DirectDraw_Stub(void* This, ...) {
    LogMessage("*** DirectDraw_Stub called - WHICH METHOD??? ***");
    
    // Enhanced debugging - try to identify which method was called
    if (!This) {
        LogMessage("DirectDraw_Stub: WARNING - This pointer is NULL");
        return DDERR_INVALIDPARAMS;
    }
    
    SDL3DirectDraw* dd = (SDL3DirectDraw*)This;
    if (!dd->lpVtbl) {
        LogMessage("DirectDraw_Stub: WARNING - DirectDraw object has no vtable");
        return DDERR_INVALIDOBJECT;
    }
    
    char buffer[128];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw_Stub: This=%p, vtable=%p", This, dd->lpVtbl);
    LogMessage(buffer);
    
    // Try to determine which vtable method was called by examining the call stack
    // This is a critical debugging point - we need to know which method is being called
    LogMessage("DirectDraw_Stub: *** THIS IS WHERE WE NEED TO IDENTIFY THE MISSING METHOD ***");
    LogMessage("DirectDraw_Stub: Returning DD_OK (unimplemented method)");
    return DD_OK;
}

// Hook for DirectDrawCreate - intercept the real DirectDraw creation
HRESULT WINAPI Hook_DirectDrawCreate(void* lpGUID, void** lplpDD, void* pUnkOuter) {
    LogMessage("*** Hook_DirectDrawCreate called - intercepting DirectDraw creation ***");
    
    char hookDebug[256];
    sprintf_s(hookDebug, sizeof(hookDebug), "DirectDrawCreate hook: lpGUID=%p, lplpDD=%p, pUnkOuter=%p", 
             lpGUID, lplpDD, pUnkOuter);
    LogMessage(hookDebug);
    
    // Moon Lights 2 approach: Simple dummy objects are set in InitDirectDraw_Hook
    // No complex vtable setup needed here
    LogMessage("DirectDrawCreate hook: Using Moon Lights 2 simple approach");
    
    // Return simple dummy object (Moon Lights 2 approach)
    if (lplpDD) {
        static int dummyDirectDrawForCreate = 0x12345678;
        *lplpDD = &dummyDirectDrawForCreate;
        LogMessage("DirectDrawCreate: Returning simple dummy object (Moon Lights 2 approach)");
        
        sprintf_s(hookDebug, sizeof(hookDebug), "DirectDrawCreate: Set *lplpDD=%p (simple dummy)", 
                 &dummyDirectDrawForCreate);
        LogMessage(hookDebug);
    }
    
    LogMessage("DirectDrawCreate hook completed successfully");
    return DD_OK;  // S_OK - success
}

bool InitializeHooks() {
    LogMessage("Initializing hooks with verified addresses...");
>>>>>>> Stashed changes

void InitializeHooks() {
    if (MH_Initialize() != MH_OK) {
        LogMessage("MinHook failed to initialize.");
        return;
    }
  //it's not time yet
  //  MH_CreateHook((LPVOID)&CreateWindowExA, (LPVOID)&Hook_CreateWindowExA, (void**)&original_create_window_ex_a);
  //  MH_CreateHook((LPVOID)&DirectDrawCreate, (LPVOID)&Hook_DirectDrawCreate, (void**)&original_directdraw_create);
    MH_CreateHook((LPVOID)&SetWindowLongA, (LPVOID)&Hook_SetWindowLongA, (void**)&original_set_window_long_a);
    
    // We hook the game's main loop (process_input_history at 0x4025A0) to drive our rendering and event polling.
    uintptr_t baseAddress = (uintptr_t)GetModuleHandle(NULL);
    void* processInputTarget = (void*)(baseAddress + 0x25A0); // 0x4025A0
    MH_CreateHook(processInputTarget, (LPVOID)&Hook_ProcessInputHistory, (void**)&original_process_input_history);

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        LogMessage("Failed to enable hooks.");
        return;
    }
    LogMessage("All hooks initialized successfully.");
}

void CleanupHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    CleanupDirectDrawHooks();
    CleanupSDL3();
    LogMessage("All hooks cleaned up.");
}

<<<<<<< Updated upstream
DWORD WINAPI MainThread(LPVOID hModule) {
    // Setup console for logging
=======

// Hook function implementations
BOOL WINAPI InitGame_Hook(HWND windowHandle) {
    LogMessage("InitGame_Hook called");
    
    // Call original function first
    BOOL result = original_initialize_game(windowHandle);
    if (!result) {
        LogMessage("Original initialize_game failed");
        return result;
    }
    
    // Initialize our SDL3 context
    if (InitializeSDL3() && CreateSDL3Window(g_gameWindow) && CreateSDL3Renderer()) {
        LogMessage("SDL3 initialization successful");
        return TRUE;
    }
    
    LogMessage("SDL3 initialization failed");
    return FALSE;
}

BOOL WINAPI InitDirectDraw_Hook(BOOL isFullScreen, HWND windowHandle) {
    LogMessage("*** MOON LIGHTS 2 APPROACH: InitDirectDraw_Hook called - implementing simple pattern ***");
    
    static bool setupComplete = false;
    
    if (setupComplete) {
        LogMessage("DirectDraw already initialized, returning success immediately");
        return TRUE;
    }
    
    // SDL3 should already be initialized by window theft, but verify
    if (!g_sdlContext.initialized) {
        char errorDebug[512];
        sprintf_s(errorDebug, sizeof(errorDebug),
            "ERROR: SDL3 context not initialized - window theft failed. window=%p, renderer=%p, initialized=%d",
            g_sdlContext.window, g_sdlContext.renderer, g_sdlContext.initialized);
        LogMessage(errorDebug);
        return FALSE;
    }
    
    // Following Moon Lights 2 pattern: Set up SDL3-backed surface objects
    LogMessage("Setting up SDL3-backed DirectDraw objects following Moon Lights 2 pattern...");
    
    // Create real SDL3 surfaces and textures
    if (!CreateSDL3Surfaces()) {
        LogMessage("ERROR: Failed to create SDL3 surfaces");
        return FALSE;
    }
    
    // Set up DirectDraw global variables with proper SDL3Surface objects
    *GAME::g_direct_draw = dummyDirectDrawObj;
    *GAME::g_dd_primary_surface = GetPrimarySurface();
    *GAME::g_dd_back_buffer = GetBackSurface();
    
    // Debug: Verify the assignments worked
    char debugAssignment[512];
    sprintf_s(debugAssignment, sizeof(debugAssignment),
        "DirectDraw assignments: g_direct_draw=%p->%p, g_dd_primary_surface=%p->%p, g_dd_back_buffer=%p->%p",
        GAME::g_direct_draw, *GAME::g_direct_draw,
        GAME::g_dd_primary_surface, *GAME::g_dd_primary_surface,
        GAME::g_dd_back_buffer, *GAME::g_dd_back_buffer);
    LogMessage(debugAssignment);
    
    // Set DirectDraw initialization flags using address.h definitions
    *GAME::g_dd_init_success = TRUE;
    *GAME::g_dd_init_success_count = 1;
    
    // Force graphics mode to DirectDraw using address.h definition
    *GAME::g_graphics_mode = 1; // DirectDraw mode
    
    setupComplete = true;
    
    LogMessage("Moon Lights 2 pattern setup complete - simple dummy objects installed");
    LogMessage("*** CRITICAL: DO NOT call original function - return success immediately ***");
    
    return TRUE; // Never call original function
}

LONG WINAPI Hook_SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong) {
    if (nIndex == GWLP_WNDPROC) {
        LogMessage("Hook_SetWindowLongA: Intercepted attempt to set a new window procedure.");
        // Store the game's intended window procedure
        original_window_proc = (WNDPROC)dwNewLong;
        // Return the existing procedure, as we are managing it now.
        return (LONG)WindowProc_Hook;
    }
    return original_set_window_long_a(hWnd, nIndex, dwNewLong);
}

LRESULT CALLBACK WindowProc_Hook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // This hook is important for intercepting window messages.
    // For now, we'll just log some key messages and pass everything to the original.
    switch (msg) {
        case WM_ACTIVATEAPP:
            LogMessage("WindowProc_Hook: WM_ACTIVATEAPP received.");
            break;
        case WM_DESTROY:
            LogMessage("WindowProc_Hook: WM_DESTROY received.");
            break;
        case WM_CLOSE:
            LogMessage("WindowProc_Hook: WM_CLOSE received.");
            break;
    }
    
    if (original_window_proc) {
        return original_window_proc(hwnd, msg, wParam, lParam);
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

DWORD WINAPI InitializeThread(LPVOID hModule) {
    // --- Phase 1: Setup Logging ---
>>>>>>> Stashed changes
    AllocConsole();
    freopen_s(&g_console_stream, "CONOUT$", "w", stdout);
    LogMessage("Hook DLL Attached. Initializing...");
    
    InitializeHooks();
    
    // Signal launcher that we are ready
    HANDLE init_event = CreateEventW(nullptr, TRUE, FALSE, L"FM2KHook_Initialized");
    if (init_event) {
        SetEvent(init_event);
        CloseHandle(init_event);
    }
    
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
            break;
        case DLL_PROCESS_DETACH:
            CleanupHooks();
            if (g_console_stream) {
                fclose(g_console_stream);
            }
            FreeConsole();
            break;
    }
    return TRUE;
}

void LogMessage(const char* message) {
    // Log to the console if it's available.
    if (g_console_stream) {
        fprintf(g_console_stream, "FM2K HOOK: %s\n", message);
        fflush(g_console_stream);
    }

    // Also write to a persistent log file.
    FILE* logFile = nullptr;
    if (fopen_s(&logFile, "C:\\games\\fm2k_hook_log.txt", "a") == 0 && logFile) {
        fprintf(logFile, "FM2K HOOK: %s\n", message);
        fclose(logFile);
    }
}