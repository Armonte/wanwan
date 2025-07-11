#include <windows.h>
#include <sddl.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <MinHook.h>
#include <stdarg.h>  // For va_list
#include <ddraw.h>

// DirectDraw error codes if not defined
#ifndef DDERR_GENERIC
#define DDERR_GENERIC                   MAKE_HRESULT(1, 0x876, 1)
#define DDERR_INVALIDPARAMS            MAKE_HRESULT(1, 0x876, 2)
#define DDERR_UNSUPPORTED              MAKE_HRESULT(1, 0x876, 3)
#define DDERR_ALREADYINITIALIZED       MAKE_HRESULT(1, 0x876, 4)
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
bool CreateSDL3Textures();
void SetupDirectDrawVirtualTable();
void SetupSurfaceVirtualTables();
void SetupDirectDrawReplacement();
void CleanupHooks();

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

// DirectDraw vtable declarations
// IUnknown methods
HRESULT STDMETHODCALLTYPE DirectDraw_QueryInterface(void* This, REFIID riid, void** ppvObject);
ULONG STDMETHODCALLTYPE DirectDraw_AddRef(void* This);
ULONG STDMETHODCALLTYPE DirectDraw_Release(void* This);
// IDirectDraw methods (essential ones based on assembly analysis)
HRESULT STDMETHODCALLTYPE DirectDraw_SetCooperativeLevel(void* This, HWND hWnd, DWORD dwFlags);
HRESULT STDMETHODCALLTYPE DirectDraw_SetDisplayMode(void* This, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP);
HRESULT STDMETHODCALLTYPE DirectDraw_CreateSurface(void* This, void* lpDDSurfaceDesc, void** lplpDDSurface, void* pUnkOuter);
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

// --- Globals ---
static HANDLE g_init_event = nullptr;
static bool g_dll_initialized = false;
static bool g_hooks_initialized = false;
static FILE* g_console_stream = nullptr;
static HWND g_gameWindow = nullptr;

// SDL3 Context
struct SDL3Context {
    SDL_Window* window;              // SDL window
    SDL_Renderer* renderer;          // SDL renderer
    SDL_Texture* gameBuffer;         // Game buffer texture
    SDL_Surface* gameSurface;        // Game buffer surface
    SDL_Texture* backBuffer;         // Back buffer texture
    SDL_Surface* backSurface;        // Back buffer surface
    SDL_Texture* spriteBuffer;       // Sprite buffer texture
    SDL_Surface* spriteSurface;      // Sprite buffer surface
    SDL_Palette* gamePalette;        // Shared palette for all surfaces
    int gameWidth;                   // Game buffer width
    int gameHeight;                  // Game buffer height
    int windowWidth;                 // Window width
    int windowHeight;                // Window height
    bool initialized;                // Context initialization state
};

static SDL3Context g_sdlContext = {nullptr};

// DirectDraw COM Interface - COMPLETE vtable with correct method order
struct IDirectDrawVtbl {
    // IUnknown methods (0-2)
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* This, REFIID riid, void** ppvObject);         // 0
    ULONG (STDMETHODCALLTYPE *AddRef)(void* This);                                                  // 1
    ULONG (STDMETHODCALLTYPE *Release)(void* This);                                                 // 2
    
    // IDirectDraw methods in EXACT order (3-21)
    HRESULT (STDMETHODCALLTYPE *Compact)(void* This);                                               // 3
    HRESULT (STDMETHODCALLTYPE *CreateClipper)(void* This, DWORD, void**, void*);                   // 4
    HRESULT (STDMETHODCALLTYPE *CreatePalette)(void* This, DWORD, void*, void**, void*);            // 5
    HRESULT (STDMETHODCALLTYPE *CreateSurface)(void* This, void*, void**, void*);                   // 6 - offset 0x18h
    HRESULT (STDMETHODCALLTYPE *DuplicateSurface)(void* This, void*, void**);                       // 7
    HRESULT (STDMETHODCALLTYPE *EnumDisplayModes)(void* This, DWORD, void*, void*, void*);          // 8
    HRESULT (STDMETHODCALLTYPE *EnumSurfaces)(void* This, DWORD, void*, void*, void*);              // 9
    HRESULT (STDMETHODCALLTYPE *FlipToGDISurface)(void* This);                                      // 10
    HRESULT (STDMETHODCALLTYPE *GetCaps)(void* This, void*, void*);                                 // 11
    HRESULT (STDMETHODCALLTYPE *GetDisplayMode)(void* This, void*);                                 // 12
    HRESULT (STDMETHODCALLTYPE *GetFourCCCodes)(void* This, LPDWORD, LPDWORD);                     // 13
    HRESULT (STDMETHODCALLTYPE *GetGDISurface)(void* This, void**);                                 // 14
    HRESULT (STDMETHODCALLTYPE *GetMonitorFrequency)(void* This, LPDWORD);                         // 15
    HRESULT (STDMETHODCALLTYPE *GetScanLine)(void* This, LPDWORD);                                  // 16
    HRESULT (STDMETHODCALLTYPE *GetVerticalBlankStatus)(void* This, LPBOOL);                       // 17
    HRESULT (STDMETHODCALLTYPE *Initialize)(void* This, GUID*);                                     // 18
    HRESULT (STDMETHODCALLTYPE *RestoreDisplayMode)(void* This);                                    // 19
    HRESULT (STDMETHODCALLTYPE *SetCooperativeLevel)(void* This, HWND, DWORD);                     // 20 - offset 0x50h
    HRESULT (STDMETHODCALLTYPE *SetDisplayMode)(void* This, DWORD, DWORD, DWORD);                  // 21 - offset 0x54h
    HRESULT (STDMETHODCALLTYPE *WaitForVerticalBlank)(void* This, DWORD, HANDLE);                  // 22
};

// DirectDraw Surface COM Interface
struct IDirectDrawSurfaceVtbl {
    // IUnknown methods
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* This, REFIID riid, void** ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(void* This);
    ULONG (STDMETHODCALLTYPE *Release)(void* This);
    
    // IDirectDrawSurface methods (in vtable order)
    HRESULT (STDMETHODCALLTYPE *AddAttachedSurface)(void* This, void* lpDDSAttachedSurface);
    HRESULT (STDMETHODCALLTYPE *AddOverlayDirtyRect)(void* This, LPRECT lpRect);
    HRESULT (STDMETHODCALLTYPE *Blt)(void* This, LPRECT lpDestRect, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx);
    HRESULT (STDMETHODCALLTYPE *BltBatch)(void* This, void* lpDDBltBatch, DWORD dwCount, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *BltFast)(void* This, DWORD dwX, DWORD dwY, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans);
    HRESULT (STDMETHODCALLTYPE *DeleteAttachedSurface)(void* This, DWORD dwFlags, void* lpDDSAttachedSurface);
    HRESULT (STDMETHODCALLTYPE *EnumAttachedSurfaces)(void* This, LPVOID lpContext, void* lpEnumSurfacesCallback);
    HRESULT (STDMETHODCALLTYPE *EnumOverlayZOrders)(void* This, DWORD dwFlags, LPVOID lpContext, void* lpfnCallback);
    HRESULT (STDMETHODCALLTYPE *Flip)(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *GetAttachedSurface)(void* This, void* lpDDSCaps, void** lplpDDAttachedSurface);
    HRESULT (STDMETHODCALLTYPE *GetBltStatus)(void* This, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *GetCaps)(void* This, void* lpDDSCaps);
    HRESULT (STDMETHODCALLTYPE *GetClipper)(void* This, void** lplpDDClipper);
    HRESULT (STDMETHODCALLTYPE *GetColorKey)(void* This, DWORD dwFlags, void* lpDDColorKey);
    HRESULT (STDMETHODCALLTYPE *GetDC)(void* This, HDC* lphDC);
    HRESULT (STDMETHODCALLTYPE *GetFlipStatus)(void* This, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *GetOverlayPosition)(void* This, LPLONG lplX, LPLONG lplY);
    HRESULT (STDMETHODCALLTYPE *GetPalette)(void* This, void** lplpDDPalette);
    HRESULT (STDMETHODCALLTYPE *GetPixelFormat)(void* This, void* lpDDPixelFormat);
    HRESULT (STDMETHODCALLTYPE *GetSurfaceDesc)(void* This, void* lpDDSurfaceDesc);
    HRESULT (STDMETHODCALLTYPE *Initialize)(void* This, void* lpDD, void* lpDDSurfaceDesc);
    HRESULT (STDMETHODCALLTYPE *IsLost)(void* This);
    HRESULT (STDMETHODCALLTYPE *Lock)(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent);
    HRESULT (STDMETHODCALLTYPE *ReleaseDC)(void* This, HDC hDC);
    HRESULT (STDMETHODCALLTYPE *Restore)(void* This);
    HRESULT (STDMETHODCALLTYPE *SetClipper)(void* This, void* lpDDClipper);
    HRESULT (STDMETHODCALLTYPE *SetColorKey)(void* This, DWORD dwFlags, void* lpDDColorKey);
    HRESULT (STDMETHODCALLTYPE *SetOverlayPosition)(void* This, LONG lX, LONG lY);
    HRESULT (STDMETHODCALLTYPE *SetPalette)(void* This, void* lpDDPalette);
    HRESULT (STDMETHODCALLTYPE *Unlock)(void* This, LPVOID lpSurfaceData);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlay)(void* This, LPRECT lpSrcRect, void* lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, void* lpDDOverlayFx);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlayDisplay)(void* This, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlayZOrder)(void* This, DWORD dwFlags, void* lpDDSReference);
};

// DirectDraw Surface Implementation
struct SDL3Surface {
    IDirectDrawSurfaceVtbl* lpVtbl;  // Must be first (COM layout)
    SDL_Surface* surface;            // SDL surface for pixel access
    SDL_Texture* texture;            // SDL texture for rendering
    bool isPrimary;                  // Is this the primary surface?
    bool isBackBuffer;               // Is this the back buffer?
    bool isSprite;                   // Is this the sprite surface?
    LONG refCount;                   // COM reference count
    bool locked;                     // Surface lock state
    DWORD lockFlags;                 // Last lock flags
};

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

// All surface methods are now properly implemented with correct signatures

// Surface method implementations
// Surface method implementations
HRESULT STDMETHODCALLTYPE Surface_QueryInterface(void* This, REFIID riid, void** ppvObject);
ULONG STDMETHODCALLTYPE Surface_AddRef(void* This);
ULONG STDMETHODCALLTYPE Surface_Release(void* This);

// Surface locking implementation
HRESULT STDMETHODCALLTYPE Surface_Lock(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent) {
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface || !surface->surface || !lpDDSurfaceDesc) {
        return DDERR_INVALIDPARAMS;
    }

    if (surface->locked) {
        return DDERR_SURFACEBUSY;
    }

    if (SDL_LockSurface(surface->surface) != 0) {
        LogMessage("Failed to lock SDL surface");
        return DDERR_GENERIC;
    }

    DDSURFACEDESC* desc = (DDSURFACEDESC*)lpDDSurfaceDesc;
    desc->dwSize = sizeof(DDSURFACEDESC);
    desc->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_LPSURFACE;
    desc->dwWidth = surface->surface->w;
    desc->dwHeight = surface->surface->h;
    desc->lPitch = surface->surface->pitch;
    desc->lpSurface = surface->surface->pixels;
    
    surface->locked = true;
    surface->lockFlags = dwFlags;

    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Unlock(void* This, void* lpRect) {
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface || !surface->surface) {
        return DDERR_INVALIDPARAMS;
    }

    if (!surface->locked) {
        return DDERR_NOTLOCKED;
    }

    SDL_UnlockSurface(surface->surface);
    surface->locked = false;

    // CRITICAL: After the game unlocks the surface, its pixel data has been updated.
    // We must now update our corresponding SDL_Texture with this new data.
    if (surface->texture) {
        if (SDL_UpdateTexture(surface->texture, NULL, surface->surface->pixels, surface->surface->pitch) != 0) {
            char buffer[256];
            sprintf(buffer, "SDL_UpdateTexture failed: %s", SDL_GetError());
            LogMessage(buffer);
        }
    }

    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Flip(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags) {
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface || !surface->isPrimary) {
        return DDERR_INVALIDPARAMS;
    }
    
    if (!g_sdlContext.initialized || !g_sdlContext.renderer) {
        return DD_OK;
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

HRESULT STDMETHODCALLTYPE Surface_GetSurfaceDesc(void* This, void* lpDDSurfaceDesc) {
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface || !lpDDSurfaceDesc) {
        return DDERR_INVALIDPARAMS;
    }
    
    DDSURFACEDESC* desc = (DDSURFACEDESC*)lpDDSurfaceDesc;
    desc->dwSize = sizeof(DDSURFACEDESC);
    desc->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc->dwHeight = surface->surface->h;
    desc->dwWidth = surface->surface->w;
    desc->lPitch = surface->surface->pitch;
    desc->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    desc->ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc->ddpfPixelFormat.dwRGBBitCount = 32;
    
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Blt(void* This, LPRECT lpDestRect, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx) {
    SDL3Surface* destSurface = (SDL3Surface*)This;
    SDL3Surface* srcSurface = (SDL3Surface*)lpDDSrcSurface;
    
    if (!destSurface || !destSurface->surface) {
        LogMessage("Invalid destination surface");
        return DDERR_INVALIDPARAMS;
    }
    
    // Handle color fill operation
    if (!srcSurface && lpDDBltFx && (dwFlags & DDBLT_COLORFILL)) {
        DDBLTFX* bltFx = (DDBLTFX*)lpDDBltFx;
        SDL_Rect dstRect;
        
        if (lpDestRect) {
            dstRect.x = lpDestRect->left;
            dstRect.y = lpDestRect->top;
            dstRect.w = lpDestRect->right - lpDestRect->left;
            dstRect.h = lpDestRect->bottom - lpDestRect->top;
        } else {
            dstRect.x = 0;
            dstRect.y = 0;
            dstRect.w = destSurface->surface->w;
            dstRect.h = destSurface->surface->h;
        }
        
        SDL_FillSurfaceRect(destSurface->surface, &dstRect, bltFx->dwFillColor);
        return DD_OK;
    }
    
    // Handle surface to surface blit
    if (srcSurface && srcSurface->surface) {
        SDL_Rect srcRect, dstRect;
        
        if (lpSrcRect) {
            srcRect.x = lpSrcRect->left;
            srcRect.y = lpSrcRect->top;
            srcRect.w = lpSrcRect->right - lpSrcRect->left;
            srcRect.h = lpSrcRect->bottom - lpSrcRect->top;
        } else {
            srcRect.x = 0;
            srcRect.y = 0;
            srcRect.w = srcSurface->surface->w;
            srcRect.h = srcSurface->surface->h;
        }
        
        if (lpDestRect) {
            dstRect.x = lpDestRect->left;
            dstRect.y = lpDestRect->top;
            dstRect.w = lpDestRect->right - lpDestRect->left;
            dstRect.h = lpDestRect->bottom - lpDestRect->top;
        } else {
            dstRect.x = 0;
            dstRect.y = 0;
            dstRect.w = destSurface->surface->w;
            dstRect.h = destSurface->surface->h;
        }
        
        SDL_BlitSurface(srcSurface->surface, &srcRect, destSurface->surface, &dstRect);
        return DD_OK;
    }
    
    return DDERR_INVALIDPARAMS;
}

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
    
    LogMessage("*** Exception handler complete - allowing normal crash handling ***");
    return EXCEPTION_CONTINUE_SEARCH; // Let Windows handle the crash normally
}

// --- Implementations ---

void LogMessage(const char* message) {
    if (!g_console_stream) return;
    fprintf(g_console_stream, "FM2K HOOK: %s\n", message);
    fflush(g_console_stream);
    
    // Also write to file for detailed analysis
    FILE* logFile = nullptr;
    if (fopen_s(&logFile, "C:\\games\\fm2k_hook_log.txt", "a") == 0 && logFile) {
        fprintf(logFile, "FM2K HOOK: %s\n", message);
        fclose(logFile);
    }
    
    OutputDebugStringA("FM2K HOOK: ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

HWND WINAPI Hook_CreateWindowExA(
    DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle,
    int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
    HINSTANCE hInstance, LPVOID lpParam) 
{
    LogMessage("Hook_CreateWindowExA triggered!");

    // Let the game create its window first.
    HWND gameHwnd = original_create_window_ex_a(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);

    // Check if this is the main game window
    if (gameHwnd && lpClassName && strcmp(lpClassName, "KGT2KGAME") == 0) {
        LogMessage("*** DETECTED MAIN GAME WINDOW - INITIATING DIRECT TAKEOVER ***");
        g_gameWindow = gameHwnd;

        // Initialize our entire SDL3 context right here
        if (!InitializeSDL3()) {
            LogMessage("FATAL: SDL3 base initialization failed. Cannot proceed.");
            return gameHwnd; // Return original handle to be safe
        }
        
        // Create an SDL window from the existing game window handle
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, gameHwnd);
        g_sdlContext.window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);

        if (!g_sdlContext.window) {
            char buffer[256];
            sprintf_s(buffer, sizeof(buffer), "FATAL: SDL_CreateWindowWithProperties failed: %s", SDL_GetError());
            LogMessage(buffer);
            return gameHwnd;
        }

        if (!CreateSDL3Renderer() || !CreateSDL3Textures()) {
            LogMessage("FATAL: SDL3 renderer/texture creation failed. Cannot proceed.");
            return gameHwnd;
        }
        
        LogMessage("Direct Takeover successful. SDL is now docked to the game window.");
    }
    
    // Always return the original handle created by the game.
    return gameHwnd;
}

BOOL WINAPI Hook_InitializeGame(HWND windowHandle) {
    LogMessage("Hook_InitializeGame triggered!");
    
    // We now have a valid windowHandle from our SDL3 window, so we can proceed
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "Game provided window handle: %p (This should be our SDL3 window)", windowHandle);
    LogMessage(buffer);

    // Call original game initialization
    LogMessage("Calling original game initialization function...");
    return original_initialize_game(windowHandle);
}

BOOL WINAPI Hook_InitializeDirectDraw(BOOL isFullScreen, HWND windowHandle) {
    LogMessage("Hook_InitializeDirectDraw triggered - setting up DirectDraw compatibility layer.");
    
    // The SDL3 context should already be initialized by our CreateWindowExA hook.
    if (!g_sdlContext.initialized) {
        LogMessage("ERROR: Hook_InitializeDirectDraw called before SDL3 was initialized. This should not happen.");
        return FALSE;
    }
    
    // Set up our fake DirectDraw surfaces to be used by the game.
    SetupDirectDrawSurfaces();
    
    // CRITICAL: Replace game's global pointers with our fake objects
    SetupDirectDrawReplacement();
    
    LogMessage("DirectDraw compatibility layer is set up.");

    // DO NOT call the original function. We are replacing it entirely.
    // The game will now use our fake surfaces, which are backed by SDL3 textures.
    return TRUE; 
}

LRESULT CALLBACK WindowProc_Hook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // This hook is called from our SDL message filter.
    // We forward the message to the game's real window procedure, but only if it has been set.
    if (original_window_proc) {
        return CallWindowProc(original_window_proc, hwnd, msg, wParam, lParam);
    }
    
    // If the game hasn't set its window proc yet, we must do default processing.
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

BOOL WINAPI Hook_ProcessInputHistory() {
    // This hook is called 60 times per second - perfect for our main loop.
    
    // Process all pending SDL events. This is crucial for window responsiveness and input handling.
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Our EventFilter is called by SDL_PollEvent, so we don't need to do much here.
        // We can add ImGui event handling here in the future.
    }

    // Call the original game logic.
    BOOL result = original_process_input_history();

    // If SDL3 is initialized, perform rendering.
    if (g_sdlContext.initialized && g_sdlContext.renderer) {
        RenderFrame();
    }

    return result;
}

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
    // NOTE: The game seems to draw everything to the BACK buffer first, then flips.
    // So we should render the BACK buffer's texture, not the primary one.
    if (g_backSurface.texture) {
        SDL_RenderTexture(g_sdlContext.renderer, g_backSurface.texture, NULL, &dstRect);
    }
    
    // Present the renderer
    SDL_RenderPresent(g_sdlContext.renderer);
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
    
    // FINAL SAFETY CHECK: Add a small delay and verify our objects are still accessible
    Sleep(100);  // Give the system a moment to settle
    
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
    g_surfaceVtbl.IsLost = Surface_IsLost;
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
        (actualMethodAt0x30 == g_surfaceVtbl.GetAttachedSurface) ? "✓" : "✗");
    LogMessage(offsetVerify);
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
    g_directDrawVtbl.GetCaps = (HRESULT (STDMETHODCALLTYPE *)(void*, void*, void*))DirectDraw_Stub;        // 11
    g_directDrawVtbl.GetDisplayMode = (HRESULT (STDMETHODCALLTYPE *)(void*, void*))DirectDraw_Stub;       // 12
    g_directDrawVtbl.GetFourCCCodes = (HRESULT (STDMETHODCALLTYPE *)(void*, LPDWORD, LPDWORD))DirectDraw_Stub;  // 13
    g_directDrawVtbl.GetGDISurface = (HRESULT (STDMETHODCALLTYPE *)(void*, void**))DirectDraw_Stub;       // 14
    g_directDrawVtbl.GetMonitorFrequency = (HRESULT (STDMETHODCALLTYPE *)(void*, LPDWORD))DirectDraw_Stub;  // 15
    g_directDrawVtbl.GetScanLine = (HRESULT (STDMETHODCALLTYPE *)(void*, LPDWORD))DirectDraw_Stub;        // 16
    g_directDrawVtbl.GetVerticalBlankStatus = (HRESULT (STDMETHODCALLTYPE *)(void*, LPBOOL))DirectDraw_Stub;  // 17
    g_directDrawVtbl.Initialize = (HRESULT (STDMETHODCALLTYPE *)(void*, GUID*))DirectDraw_Stub;           // 18
    g_directDrawVtbl.RestoreDisplayMode = (HRESULT (STDMETHODCALLTYPE *)(void*))DirectDraw_Stub;          // 19
    g_directDrawVtbl.SetCooperativeLevel = DirectDraw_SetCooperativeLevel;  // 20 - CRITICAL at offset 0x50h
    g_directDrawVtbl.SetDisplayMode = DirectDraw_SetDisplayMode;  // 21 - CRITICAL at offset 0x54h
    g_directDrawVtbl.WaitForVerticalBlank = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, HANDLE))DirectDraw_Stub;  // 22
    
    LogMessage("DirectDraw virtual function table initialized successfully");
    
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
    LogMessage("Surface_QueryInterface called");
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

HRESULT STDMETHODCALLTYPE Surface_GetAttachedSurface(void* This, void* lpDDSCaps, void** lplpDDAttachedSurface) {
    LogMessage("Surface_GetAttachedSurface called");
    
    if (!lplpDDAttachedSurface) {
        LogMessage("ERROR: lplpDDAttachedSurface is NULL");
        return DDERR_INVALIDPARAMS;
    }
    
    // Parse the surface caps to determine what type of attached surface is requested
    DWORD requestedCaps = 0;
    if (lpDDSCaps) {
        // Assume lpDDSCaps points to a DWORD containing surface capability flags
        requestedCaps = *(DWORD*)lpDDSCaps;
    }
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "GetAttachedSurface: Requested caps = 0x%X (expecting DDSCAPS_BACKBUFFER=0x%X)", requestedCaps, DDSCAPS_BACKBUFFER);
    LogMessage(buffer);
    
    SDL3Surface* thisSurface = (SDL3Surface*)This;
    
    // Check if this is the primary surface and back buffer is requested
    if (thisSurface && thisSurface->isPrimary) {
        // Check if DDSCAPS_BACKBUFFER is being requested
        if (requestedCaps == DDSCAPS_BACKBUFFER || requestedCaps == 0) {  // Handle both explicit request and default
            *lplpDDAttachedSurface = &g_backSurface;
            g_backSurface.refCount++; // AddRef the returned surface
            LogMessage("SUCCESS: Returned back buffer from primary surface");
            return DD_OK;
        } else {
            sprintf_s(buffer, sizeof(buffer), "Primary surface: Unsupported caps 0x%X requested", requestedCaps);
            LogMessage(buffer);
        }
    } else {
        LogMessage("GetAttachedSurface called on non-primary surface");
    }
    
    // For unsupported requests, no attached surface
    *lplpDDAttachedSurface = nullptr;
    LogMessage("No attached surface found for request");
    return DDERR_NOTFOUND;
}

HRESULT STDMETHODCALLTYPE Surface_EnumOverlayZOrders(void* This, DWORD dwFlags, LPVOID lpContext, void* lpfnCallback) {
    return DDERR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE Surface_GetBltStatus(void* This, DWORD dwFlags) {
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_GetCaps(void* This, void* lpDDSCaps) {
    return DDERR_UNSUPPORTED;
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

HRESULT STDMETHODCALLTYPE Surface_IsLost(void* This) {
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_ReleaseDC(void* This, HDC hDC) {
    LogMessage("Surface_ReleaseDC called (STUB)");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Restore(void* This) {
    LogMessage("Surface_Restore called (STUB)");
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
    LogMessage("DirectDraw_QueryInterface called");
    if (!ppvObject) return E_POINTER;
    *ppvObject = This;
    ((SDL3DirectDraw*)This)->refCount++;
    return S_OK;
}

ULONG STDMETHODCALLTYPE DirectDraw_AddRef(void* This) {
    LogMessage("DirectDraw_AddRef called");
    SDL3DirectDraw* dd = (SDL3DirectDraw*)This;
    return ++dd->refCount;
}

ULONG STDMETHODCALLTYPE DirectDraw_Release(void* This) {
    LogMessage("DirectDraw_Release called");
    SDL3DirectDraw* dd = (SDL3DirectDraw*)This;
    return --dd->refCount;
}

HRESULT STDMETHODCALLTYPE DirectDraw_SetCooperativeLevel(void* This, HWND hWnd, DWORD dwFlags) {
    // CRITICAL: First thing - log that we've entered this function
    LogMessage("*** ENTERED DirectDraw_SetCooperativeLevel ***");
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw_SetCooperativeLevel called: This=%p hWnd=%p dwFlags=0x%X", This, hWnd, dwFlags);
    LogMessage(buffer);
    LogMessage("DirectDraw_SetCooperativeLevel returning DD_OK");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DirectDraw_SetDisplayMode(void* This, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP) {
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw_SetDisplayMode called: This=%p %dx%d %d-bit", This, dwWidth, dwHeight, dwBPP);
    LogMessage(buffer);
    LogMessage("DirectDraw_SetDisplayMode returning DD_OK");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DirectDraw_CreateSurface(void* This, void* lpDDSurfaceDesc, void** lplpDDSurface, void* pUnkOuter) {
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw_CreateSurface called: This=%p lpDDSurfaceDesc=%p lplpDDSurface=%p", This, lpDDSurfaceDesc, lplpDDSurface);
    LogMessage(buffer);
    
    if (!lplpDDSurface) {
        LogMessage("DirectDraw_CreateSurface: Invalid parameters");
        return DDERR_INVALIDPARAMS;
    }
    
    // For now, just return the primary surface
    // In a complete implementation, we'd parse lpDDSurfaceDesc to determine which surface type to create
    *lplpDDSurface = &g_primarySurface;
    g_primarySurface.refCount++; // AddRef the returned surface
    
    LogMessage("DirectDraw_CreateSurface: Returning primary surface, DD_OK");
    return DD_OK;
}


HRESULT __stdcall DirectDraw_TestFunction() {
    LogMessage("*** DirectDraw_TestFunction called - minimal test ***");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DirectDraw_Stub(void* This, ...) {
    LogMessage("DirectDraw_Stub called");
    return DD_OK;
}

// Hook for DirectDrawCreate - intercept the real DirectDraw creation
HRESULT WINAPI Hook_DirectDrawCreate(void* lpGUID, void** lplpDD, void* pUnkOuter) {
    LogMessage("*** Hook_DirectDrawCreate called - intercepting DirectDraw creation ***");
    
    char hookDebug[256];
    sprintf_s(hookDebug, sizeof(hookDebug), "DirectDrawCreate hook: lpGUID=%p, lplpDD=%p, pUnkOuter=%p", 
             lpGUID, lplpDD, pUnkOuter);
    LogMessage(hookDebug);
    
    // Setup our fake DirectDraw system if not already done
    if (!g_directDraw.initialized) {
        LogMessage("Setting up DirectDraw surfaces from DirectDrawCreate hook...");
        SetupDirectDrawVirtualTable();
        SetupSurfaceVirtualTables();
        InitializeSurfaces();

        // CRITICAL FIX: The vtable for the main DirectDraw object was not being set here.
        LogMessage("CRITICAL FIX: Explicitly setting g_directDraw vtable and state.");
        g_directDraw.lpVtbl = &g_directDrawVtbl;
        g_directDraw.initialized = true;
        g_directDraw.refCount = 1; // Start with a reference count of 1
        g_directDraw.primarySurface = &g_primarySurface;
        g_directDraw.backSurface = &g_backSurface;
        g_directDraw.spriteSurface = &g_spriteSurface;
    }
    
    // Return our fake DirectDraw object instead of creating a real one
    if (lplpDD) {
        *lplpDD = &g_directDraw;
        g_directDraw.refCount++;
        LogMessage("DirectDrawCreate: Returning our fake DirectDraw object");
        
        // This log should now show a valid vtable address
        sprintf_s(hookDebug, sizeof(hookDebug), "DirectDrawCreate: Set *lplpDD=%p, vtbl=%p", 
                 &g_directDraw, g_directDraw.lpVtbl);
        LogMessage(hookDebug);
    }
    
    LogMessage("DirectDrawCreate hook completed successfully");
    return DD_OK;  // S_OK - success
}

bool InitializeHooks() {
    LogMessage("Initializing hooks with verified addresses...");

    if (MH_Initialize() != MH_OK) {
        LogMessage("ERROR: MinHook failed to initialize.");
        return false;
    }

    // Hook CreateWindowExA to capture the game window handle
    if (MH_CreateHook((LPVOID)&CreateWindowExA, (LPVOID)&Hook_CreateWindowExA, (void**)&original_create_window_ex_a) != MH_OK) {
        LogMessage("ERROR: Failed to create hook for CreateWindowExA.");
        return false;
    }
    LogMessage("Hook for CreateWindowExA created.");

    // Hook key game functions using direct addresses from IDA
    uintptr_t baseAddress = (uintptr_t)GetModuleHandle(NULL);
    if (!baseAddress) {
        LogMessage("ERROR: Failed to get game module handle.");
        return false;
    }
    LogMessage("Game module base address obtained.");

    // Hook DirectDrawCreate via jump stub (0x41b544 from IDA analysis)  
    void* pDirectDrawCreateStub = (void*)(baseAddress + 0x1b544); // 0x41b544 - 0x400000
    if (MH_CreateHook(pDirectDrawCreateStub, (LPVOID)&Hook_DirectDrawCreate, (void**)&original_directdraw_create) != MH_OK) {
        LogMessage("ERROR: Failed to create hook for DirectDrawCreate stub at 0x41b544.");
        return false;
    }
    LogMessage("Hook for DirectDrawCreate jump stub created at 0x41b544.");

    void* pInitGame = (void*)(baseAddress + 0x56C0); // 0x4056C0 - 0x400000
    if (MH_CreateHook(pInitGame, (LPVOID)&InitGame_Hook, (void**)&original_initialize_game) != MH_OK) {
        LogMessage("ERROR: Failed to create hook for initialize_game at 0x4056C0.");
        return false;
    }
    LogMessage("Hook for initialize_game created.");

    void* pInitDirectDraw = (void*)(baseAddress + 0x4980); // 0x404980 - 0x400000
    if (MH_CreateHook(pInitDirectDraw, (LPVOID)&InitDirectDraw_Hook, (void**)&original_initialize_directdraw) != MH_OK) {
        LogMessage("ERROR: Failed to create hook for initialize_directdraw_mode at 0x404980.");
        return false;
    }
    LogMessage("Hook for initialize_directdraw_mode created.");

    void* pProcessInput = (void*)(baseAddress + 0x25A0); // 0x4025A0 - 0x400000
    if (MH_CreateHook(pProcessInput, (LPVOID)&Hook_ProcessInputHistory, (void**)&original_process_input_history) != MH_OK) {
        LogMessage("ERROR: Failed to create hook for process_input_history at 0x4025A0.");
        return false;
    }
    LogMessage("Hook for process_input_history created.");

    void* pWindowProc = (void*)(baseAddress + 0x5F50); // 0x405F50 - 0x400000
    if (MH_CreateHook(pWindowProc, (LPVOID)&WindowProc_Hook, (void**)&original_window_proc) != MH_OK) {
        LogMessage("ERROR: Failed to create hook for main_window_proc at 0x405F50.");
        return false;
    }
    LogMessage("Hook for main_window_proc created.");

    // Hook SetWindowLongA to intercept window procedure changes
    if (MH_CreateHook((LPVOID)&SetWindowLongA, (LPVOID)&Hook_SetWindowLongA, (void**)&original_set_window_long_a) != MH_OK) {
        LogMessage("ERROR: Failed to create hook for SetWindowLongA.");
        return false;
    }
    LogMessage("Hook for SetWindowLongA created.");

    // Enable all created hooks
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        LogMessage("ERROR: Failed to enable hooks.");
        return false;
    }

    LogMessage("All hooks enabled successfully.");
    g_hooks_initialized = true;
    return true;
}

void CleanupHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    LogMessage("All hooks cleaned up.");
}


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
    LogMessage("InitDirectDraw_Hook called");
    
    // Initialize SDL3 first
    if (!g_sdlContext.initialized) {
        if (!InitializeSDL3()) {
            LogMessage("SDL3 initialization failed");
            return FALSE;
        }
        
        if (!CreateSDL3Window(g_gameWindow) || !CreateSDL3Renderer() || !CreateSDL3Textures()) {
            LogMessage("SDL3 setup failed");
            return FALSE;
        }
        
        // Hide the game window - we'll render in our window
        ShowWindow(g_gameWindow, SW_HIDE);
        SDL_ShowWindow(g_sdlContext.window);
        LogMessage("Game window hidden, SDL3 window shown");
    }
    
    // Since DirectDrawCreate hook isn't working, set up our fake objects directly
    LogMessage("Setting up DirectDraw surfaces directly in InitDirectDraw_Hook...");
    SetupDirectDrawVirtualTable();
    SetupSurfaceVirtualTables();
    InitializeSurfaces();
    
    // CRITICAL: Force DirectDraw mode by setting g_graphics_mode = 1
    int* pGraphicsMode = (int*)0x424704; // g_graphics_mode from IDA
    int oldGraphicsMode = *pGraphicsMode;
    *pGraphicsMode = 1; // Force fullscreen DirectDraw mode
    
    char modeDebug[256];
    sprintf_s(modeDebug, sizeof(modeDebug), "Forced g_graphics_mode from %d to %d to enable DirectDraw path", 
             oldGraphicsMode, *pGraphicsMode);
    LogMessage(modeDebug);
    
    // Call original function to set up game state, then immediately replace the globals
    LogMessage("Calling original initialize_directdraw to set up game state...");
    BOOL result = original_initialize_directdraw(isFullScreen, windowHandle);
    
    char resultMsg[256];
    sprintf_s(resultMsg, sizeof(resultMsg), "Original initialize_directdraw returned: %s (%d)", 
             result ? "TRUE" : "FALSE", result);
    LogMessage(resultMsg);
    
    // Immediately overwrite whatever the original function created with our fake objects
    LogMessage("Overwriting DirectDraw globals with our fake objects...");
    SetupDirectDrawReplacement();
    
    // Always return TRUE regardless of original result - we want our fake objects to be used
    LogMessage("InitDirectDraw_Hook completed successfully - forcing success");
    return TRUE;
}

LONG WINAPI Hook_SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong) {
    if (nIndex == GWLP_WNDPROC) {
        LogMessage("Hook_SetWindowLongA: Intercepted attempt to set a new window procedure.");
        
        // Store the game's intended window procedure if we haven't already
        if (!original_window_proc) {
            original_window_proc = (WNDPROC)dwNewLong;
            LogMessage("Stored game's main window procedure.");
        }
        
        // Return the existing procedure, as we are managing it now.
        // We are replacing their call with our own subclassing.
        return (LONG)SetWindowLongPtr(hWnd, nIndex, (LONG_PTR)WindowProc_Hook);
    }
    return original_set_window_long_a(hWnd, nIndex, dwNewLong);
}

DWORD WINAPI InitializeThread(LPVOID hModule) {
    // --- Phase 1: Setup Logging ---
    AllocConsole();
    freopen_s(&g_console_stream, "CONOUT$", "w", stdout);
    LogMessage("Initialization thread started.");
    
    // Install crash handler for debugging
    SetUnhandledExceptionFilter(CrashHandler);
    LogMessage("Crash handler installed for debugging");

    // --- Phase 2: Initialize Hooks ---
    if (!InitializeHooks()) {
        LogMessage("Hook initialization failed. Aborting.");
        // We still need to signal the event so the launcher doesn't hang forever
        if (g_init_event) {
            BOOL result = SetEvent(g_init_event);
            char signal_msg[256];
            sprintf_s(signal_msg, sizeof(signal_msg), "SetEvent(failure path) result: %d, handle: %p", result, g_init_event);
            LogMessage(signal_msg);
        }
        if (g_console_stream) fclose(g_console_stream);
        FreeConsole();
        return 1; // Failure
    }

    // --- Phase 3: Signal Success & Wait ---
    g_dll_initialized = true;
    LogMessage("Initialization complete. Signaling launcher...");
            if (g_init_event) {
        BOOL result = SetEvent(g_init_event);
        char signal_msg[256];
        sprintf_s(signal_msg, sizeof(signal_msg), "SetEvent(success path) result: %d, handle: %p, error: %lu", result, g_init_event, GetLastError());
        LogMessage(signal_msg);
    } else {
        LogMessage("ERROR: g_init_event is NULL, cannot signal launcher");
    }
    
    // A small delay to ensure the launcher has time to process the event
    // before this thread potentially exits in future revisions.
    LogMessage("Initialization thread finished. Waiting 5 seconds before exiting.");
    Sleep(5000);

    return 0; // Success
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
        {
            // Create the event that the launcher will wait on.
            // Use CreateEventW with wide string to match launcher exactly.
            g_init_event = CreateEventW(nullptr, TRUE, FALSE, L"FM2KHook_Initialized");

            if (g_init_event == NULL) {
                // Log the error before failing
                DWORD error = GetLastError();
                char error_msg[256];
                sprintf_s(error_msg, sizeof(error_msg), "Failed to create event. Error: %lu", error);
                OutputDebugStringA("FM2K HOOK: ");
                OutputDebugStringA(error_msg);
                OutputDebugStringA("\n");
                return FALSE; // Cannot proceed without event
            } else {
                // Log successful event creation
                char success_msg[256];
                sprintf_s(success_msg, sizeof(success_msg), "Successfully created event handle: %p", g_init_event);
                OutputDebugStringA("FM2K HOOK: ");
                OutputDebugStringA(success_msg);
                OutputDebugStringA("\n");
            }
            
            // To prevent deadlocks, do not do complex work here.
            // Create a separate thread for initialization.
            DisableThreadLibraryCalls(hModule);
            HANDLE hThread = CreateThread(NULL, 0, InitializeThread, hModule, 0, NULL);
            if (hThread == NULL) {
                    CloseHandle(g_init_event);
                return FALSE; // Failed to create init thread
            }
            CloseHandle(hThread); // We don't need to manage the thread, so close handle.
            break;
        }

        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;

        case DLL_PROCESS_DETACH:
        {
            CleanupHooks();
            if (g_init_event) {
                CloseHandle(g_init_event);
                g_init_event = nullptr;
            }
            if (g_console_stream) {
                LogMessage("Process detaching. Closing console.");
                fclose(g_console_stream);
                FreeConsole();
            }
            break;
        }
    }
    return TRUE;
}

void CleanupSDL3() {
    LogMessage("Cleaning up SDL3 resources...");
    
    // Destroy textures
    if (g_sdlContext.gameBuffer) {
        SDL_DestroyTexture(g_sdlContext.gameBuffer);
        g_sdlContext.gameBuffer = nullptr;
    }
    
    if (g_sdlContext.backBuffer) {
        SDL_DestroyTexture(g_sdlContext.backBuffer);
        g_sdlContext.backBuffer = nullptr;
    }
    
    if (g_sdlContext.spriteBuffer) {
        SDL_DestroyTexture(g_sdlContext.spriteBuffer);
        g_sdlContext.spriteBuffer = nullptr;
    }
    
    // Destroy surfaces
    if (g_sdlContext.gameSurface) {
        SDL_DestroySurface(g_sdlContext.gameSurface);
        g_sdlContext.gameSurface = nullptr;
    }
    
    if (g_sdlContext.backSurface) {
        SDL_DestroySurface(g_sdlContext.backSurface);
        g_sdlContext.backSurface = nullptr;
    }
    
    if (g_sdlContext.spriteSurface) {
        SDL_DestroySurface(g_sdlContext.spriteSurface);
        g_sdlContext.spriteSurface = nullptr;
    }
    
    // Destroy palette
    if (g_sdlContext.gamePalette) {
        SDL_DestroyPalette(g_sdlContext.gamePalette);
        g_sdlContext.gamePalette = nullptr;
    }
    
    // Destroy renderer
    if (g_sdlContext.renderer) {
        SDL_DestroyRenderer(g_sdlContext.renderer);
        g_sdlContext.renderer = nullptr;
    }
    
    // Destroy window
    if (g_sdlContext.window) {
        SDL_DestroyWindow(g_sdlContext.window);
        g_sdlContext.window = nullptr;
    }
    
    g_sdlContext.initialized = false;
    LogMessage("SDL3 cleanup complete");
}

void CleanupSurfaces() {
    LogMessage("Cleaning up DirectDraw surfaces...");
    
    // Clean up primary surface
    if (g_primarySurface.texture) {
        SDL_DestroyTexture(g_primarySurface.texture);
        g_primarySurface.texture = nullptr;
    }
    if (g_primarySurface.surface) {
        SDL_DestroySurface(g_primarySurface.surface);
        g_primarySurface.surface = nullptr;
    }
    
    // Clean up back buffer surface
    if (g_backSurface.texture) {
        SDL_DestroyTexture(g_backSurface.texture);
        g_backSurface.texture = nullptr;
    }
    if (g_backSurface.surface) {
        SDL_DestroySurface(g_backSurface.surface);
        g_backSurface.surface = nullptr;
    }
    
    // Clean up sprite surface
    if (g_spriteSurface.texture) {
        SDL_DestroyTexture(g_spriteSurface.texture);
        g_spriteSurface.texture = nullptr;
    }
    if (g_spriteSurface.surface) {
        SDL_DestroySurface(g_spriteSurface.surface);
        g_spriteSurface.surface = nullptr;
    }
    
    LogMessage("DirectDraw surfaces cleaned up successfully");
}