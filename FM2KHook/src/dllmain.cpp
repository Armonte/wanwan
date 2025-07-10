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
#define DDSCAPS_PRIMARYSURFACE  0x00000200L
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
bool EventFilter(void* userdata, SDL_Event* event);
bool WindowsMessageHook(void* userdata, MSG* msg);
bool CreateSDL3Textures();
void SetupSurfaceVirtualTables();
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
    static HWND (WINAPI* original_create_window_ex_a)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) = nullptr;
}

// DirectDraw Surface vtable declarations
HRESULT STDMETHODCALLTYPE Surface_QueryInterface(void* This, REFIID riid, void** ppvObject);
ULONG STDMETHODCALLTYPE Surface_AddRef(void* This);
ULONG STDMETHODCALLTYPE Surface_Release(void* This);
HRESULT STDMETHODCALLTYPE Surface_Lock(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent);
HRESULT STDMETHODCALLTYPE Surface_Unlock(void* This, void* lpRect);
HRESULT STDMETHODCALLTYPE Surface_Blt(void* This, LPRECT lpDestRect, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx);
HRESULT STDMETHODCALLTYPE Surface_Flip(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags);
HRESULT STDMETHODCALLTYPE Surface_SetPalette(void* This, void* lpDDPalette);
HRESULT STDMETHODCALLTYPE Surface_GetDC(void* This, HDC* lphDC);
HRESULT STDMETHODCALLTYPE Surface_ReleaseDC(void* This, HDC hDC);
HRESULT STDMETHODCALLTYPE Surface_GetAttachedSurface(void* This, void* lpDDSCaps, void** lplpDDAttachedSurface);
HRESULT STDMETHODCALLTYPE Surface_GetCaps(void* This, void* lpDDSCaps);
HRESULT STDMETHODCALLTYPE Surface_GetPixelFormat(void* This, void* lpDDPixelFormat);
HRESULT STDMETHODCALLTYPE Surface_GetSurfaceDesc(void* This, void* lpDDSurfaceDesc);

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
    void* lpVtbl;  // DirectDraw vtable (not implemented yet)
    bool initialized;
    SDL3Surface* primarySurface;
    SDL3Surface* backSurface;
    SDL3Surface* spriteSurface;
};

// Global instances
static SDL3DirectDraw g_directDraw = {nullptr};
static SDL3Surface g_primarySurface = {nullptr};
static SDL3Surface g_backSurface = {nullptr};
static SDL3Surface g_spriteSurface = {nullptr};
static IDirectDrawSurfaceVtbl g_surfaceVtbl = {nullptr};

// Stub implementation for non-critical methods
HRESULT STDMETHODCALLTYPE Surface_Stub(void* This, ...) {
    // Just return success for any unimplemented method
    return DD_OK;
}

// Surface method implementations
// Surface method implementations
HRESULT STDMETHODCALLTYPE Surface_QueryInterface(void* This, REFIID riid, void** ppvObject);
ULONG STDMETHODCALLTYPE Surface_AddRef(void* This);
ULONG STDMETHODCALLTYPE Surface_Release(void* This);

// Surface locking implementation
HRESULT STDMETHODCALLTYPE Surface_Lock(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent) {
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface || !lpDDSurfaceDesc) {
        return DDERR_INVALIDPARAMS;
    }
    
    // Already locked
    if (surface->locked) {
        return DDERR_SURFACEBUSY;
    }
    
    // Lock the surface
    if (!SDL_LockSurface(surface->surface)) {
        LogMessage("Failed to lock surface");
        return DDERR_GENERIC;
    }
    
    // Fill out the surface description
    DDSURFACEDESC* desc = (DDSURFACEDESC*)lpDDSurfaceDesc;
    desc->dwSize = sizeof(DDSURFACEDESC);
    desc->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc->dwHeight = surface->surface->h;
    desc->dwWidth = surface->surface->w;
    desc->lPitch = surface->surface->pitch;
    desc->lpSurface = surface->surface->pixels;
    
    // Set lock state
    surface->locked = true;
    surface->lockFlags = dwFlags;
    
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Unlock(void* This, void* lpRect) {
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface) {
        return DDERR_INVALIDPARAMS;
    }
    
    // Not locked
    if (!surface->locked) {
        return DDERR_NOTLOCKED;
    }
    
    // Unlock the surface
    SDL_UnlockSurface(surface->surface);
    
    // Update texture from surface
    if (surface->texture) {
        SDL_UpdateTexture(surface->texture, NULL, surface->surface->pixels, surface->surface->pitch);
    }
    
    // Clear lock state
    surface->locked = false;
    surface->lockFlags = 0;
    
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Flip(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags) {
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface || !surface->isPrimary) {
        return DDERR_INVALIDPARAMS;
    }
    
    // Clear the renderer
    SDL_RenderClear(g_sdlContext.renderer);
    
    // Set up source and destination rectangles for proper scaling
    SDL_FRect srcRect = {0, 0, (float)surface->surface->w, (float)surface->surface->h};
    SDL_FRect dstRect = {0, 0, (float)g_sdlContext.windowWidth, (float)g_sdlContext.windowHeight};
    
    // Render the back buffer to the screen with proper scaling
    if (g_sdlContext.backBuffer) {
        SDL_RenderTexture(g_sdlContext.renderer, g_sdlContext.backBuffer, &srcRect, &dstRect);
    }
    
    // Present the renderer
    SDL_RenderPresent(g_sdlContext.renderer);
    
    return DD_OK;
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
    desc->ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    desc->ddpfPixelFormat.dwRGBBitCount = 8;
    
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
    
    // Initialize primary surface
    g_primarySurface.lpVtbl = &g_surfaceVtbl;
    g_primarySurface.surface = SDL_CreateSurface(g_sdlContext.gameWidth, g_sdlContext.gameHeight, SDL_PIXELFORMAT_INDEX8);
    g_primarySurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_primarySurface.surface);
    g_primarySurface.isPrimary = true;
    g_primarySurface.isBackBuffer = false;
    g_primarySurface.isSprite = false;
    g_primarySurface.refCount = 1;
    g_primarySurface.locked = false;
    g_primarySurface.lockFlags = 0;
    
    // Initialize back buffer surface
    g_backSurface.lpVtbl = &g_surfaceVtbl;
    g_backSurface.surface = SDL_CreateSurface(640, 480, SDL_PIXELFORMAT_INDEX8);
    g_backSurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_backSurface.surface);
    g_backSurface.isPrimary = false;
    g_backSurface.isBackBuffer = true;
    g_backSurface.isSprite = false;
    g_backSurface.refCount = 1;
    g_backSurface.locked = false;
    g_backSurface.lockFlags = 0;
    
    // Initialize sprite surface
    g_spriteSurface.lpVtbl = &g_surfaceVtbl;
    g_spriteSurface.surface = SDL_CreateSurface(256, 256, SDL_PIXELFORMAT_INDEX8);
    g_spriteSurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_spriteSurface.surface);
    g_spriteSurface.isPrimary = false;
    g_spriteSurface.isBackBuffer = false;
    g_spriteSurface.isSprite = true;
    g_spriteSurface.refCount = 1;
    g_spriteSurface.locked = false;
    g_spriteSurface.lockFlags = 0;
    
    // Create shared palette for all surfaces
    SDL_Palette* palette = SDL_CreatePalette(256);
    if (palette) {
        SDL_SetSurfacePalette(g_primarySurface.surface, palette);
        SDL_SetSurfacePalette(g_backSurface.surface, palette);
        SDL_SetSurfacePalette(g_spriteSurface.surface, palette);
        SDL_DestroyPalette(palette);  // Surfaces will keep a reference
    }
    
    LogMessage("DirectDraw surfaces initialized successfully");
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
    
    // Call original function to create the game window
    HWND gameWindow = original_create_window_ex_a(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
    
    if (gameWindow && lpClassName) {
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "Window created: class='%s', title='%s', hwnd=%p", 
                lpClassName, lpWindowName ? lpWindowName : "NULL", gameWindow);
        LogMessage(buffer);
        
        // Check if this is the main game window
        if (strcmp(lpClassName, "KGT2KGAME") == 0) {
            LogMessage("*** DETECTED MAIN GAME WINDOW - storing for future use ***");
            g_gameWindow = gameWindow;
            
            // Set the game's window handle global at verified address
            HWND* pGameWindowHandle = (HWND*)0x4246F8; // g_hwnd_parent from IDA
            if (!IsBadWritePtr(pGameWindowHandle, sizeof(HWND))) {
                *pGameWindowHandle = gameWindow;
                LogMessage("Updated g_hwnd_parent global with game window handle");
            } else {
                LogMessage("WARNING: Could not access g_hwnd_parent at 0x4246F8");
            }
            
            // Window detection complete - SDL3 takeover will happen in Hook_InitializeGame
            LogMessage("Main game window detected and stored for SDL3 takeover");
        }
    }
    
    return gameWindow;
}

BOOL WINAPI Hook_InitializeGame(HWND windowHandle) {
    LogMessage("Hook_InitializeGame triggered - setting up SDL3 side-by-side!");
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "Game provided window handle: %p", windowHandle);
    LogMessage(buffer);

    // Call original game initialization FIRST
    LogMessage("*** Calling original game initialization function FIRST ***");
    BOOL result = original_initialize_game(windowHandle);
    
    char resultBuffer[256];
    sprintf_s(resultBuffer, sizeof(resultBuffer), "Original game initialization returned: %d", result);
    LogMessage(resultBuffer);
    
    // NOW set up SDL3 after game is fully initialized
    if (result && g_gameWindow) {
        LogMessage("Game initialized successfully - setting up SDL3 side-by-side");
        
        if (InitializeSDL3() && CreateSDL3Window(g_gameWindow) && CreateSDL3Renderer() && CreateSDL3Textures()) {
            LogMessage("SDL3 setup complete - running side-by-side with game!");
            
            // Test render a few frames
            for (int i = 1; i <= 3; i++) {
                char testBuffer[256];
                sprintf_s(testBuffer, sizeof(testBuffer), "Initial SDL3 test render #%d", i);
                LogMessage(testBuffer);
                RenderFrame();
                Sleep(100);
            }
        } else {
            LogMessage("SDL3 setup failed - game will run normally");
        }
    }

    return result;
}

BOOL WINAPI Hook_InitializeDirectDraw(BOOL isFullScreen, HWND windowHandle) {
    LogMessage("Hook_InitializeDirectDraw triggered - intercepting surfaces");
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw init: fullscreen=%d, windowHandle=%p", isFullScreen, windowHandle);
    LogMessage(buffer);

    // Initialize SDL3 if not already done
    if (!g_sdlContext.initialized) {
        if (!InitializeSDL3()) {
            LogMessage("ERROR: SDL3 initialization failed");
            return FALSE;
        }
        
        if (!CreateSDL3Window(g_gameWindow) || !CreateSDL3Renderer() || !CreateSDL3Textures()) {
            LogMessage("ERROR: SDL3 setup failed");
            return FALSE;
        }

        // Hide the game window - we'll render in our window
        ShowWindow(g_gameWindow, SW_HIDE);
        SDL_ShowWindow(g_sdlContext.window);
        LogMessage("Game window hidden, SDL3 window shown");
    }
    
    // Set up DirectDraw surface interception
    SetupDirectDrawSurfaces();
    
    // Let the original function run to set up any necessary state
    BOOL result = FALSE;
    if (original_initialize_directdraw) {
        result = original_initialize_directdraw(isFullScreen, windowHandle);
        LogMessage("Original DirectDraw initialization complete");
    }
    
    return TRUE; // Always return success since we're handling rendering
}

LRESULT WINAPI Hook_WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // This hook is important for intercepting window messages.
    return original_window_proc(hwnd, msg, wParam, lParam);
}

BOOL WINAPI Hook_ProcessInputHistory() {
    // This hook is called 60 times per second - perfect for rendering
    static int renderCallCount = 0;
    renderCallCount++;

    BOOL result = original_process_input_history();

    // If SDL3 is initialized, perform rendering
    if (g_sdlContext.initialized && g_sdlContext.renderer) {
        if (renderCallCount <= 10) {
        char buffer[256];
            sprintf_s(buffer, sizeof(buffer), "Hook_ProcessInputHistory call #%d - starting render", renderCallCount);
            LogMessage(buffer);
        }
        RenderFrame();
    }

    return result;
}

void RenderFrame() {
    if (!g_sdlContext.initialized || !g_sdlContext.renderer) {
        return;
    }
    
    // Clear the renderer
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
    
    // Render the game texture
    if (g_sdlContext.gameBuffer) {
        SDL_RenderTexture(g_sdlContext.renderer, g_sdlContext.gameBuffer, NULL, &dstRect);
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
    g_primarySurface.surface = SDL_CreateSurface(g_sdlContext.gameWidth, g_sdlContext.gameHeight, SDL_PIXELFORMAT_INDEX8);
    g_primarySurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_primarySurface.surface);
    g_primarySurface.isPrimary = true;
    g_primarySurface.isBackBuffer = false;
    g_primarySurface.isSprite = false;
    g_primarySurface.refCount = 1;
    g_primarySurface.locked = false;
    g_primarySurface.lockFlags = 0;
    
    // Initialize back buffer surface
    g_backSurface.lpVtbl = &g_surfaceVtbl;
    g_backSurface.surface = SDL_CreateSurface(g_sdlContext.gameWidth, g_sdlContext.gameHeight, SDL_PIXELFORMAT_INDEX8);
    g_backSurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_backSurface.surface);
    g_backSurface.isPrimary = false;
    g_backSurface.isBackBuffer = true;
    g_backSurface.isSprite = false;
    g_backSurface.refCount = 1;
    g_backSurface.locked = false;
    g_backSurface.lockFlags = 0;
    
    // Initialize sprite surface
    g_spriteSurface.lpVtbl = &g_surfaceVtbl;
    g_spriteSurface.surface = SDL_CreateSurface(g_sdlContext.gameWidth, g_sdlContext.gameHeight, SDL_PIXELFORMAT_INDEX8);
    g_spriteSurface.texture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_spriteSurface.surface);
    g_spriteSurface.isPrimary = false;
    g_spriteSurface.isBackBuffer = false;
    g_spriteSurface.isSprite = true;
    g_spriteSurface.refCount = 1;
    g_spriteSurface.locked = false;
    g_spriteSurface.lockFlags = 0;
    
    // Create shared palette for all surfaces
    SDL_Palette* palette = SDL_CreatePalette(256);
    if (palette) {
        SDL_SetSurfacePalette(g_primarySurface.surface, palette);
        SDL_SetSurfacePalette(g_backSurface.surface, palette);
        SDL_SetSurfacePalette(g_spriteSurface.surface, palette);
        SDL_DestroyPalette(palette);  // Surfaces will keep a reference
    }
}

bool CreateSDL3Window(HWND gameHwnd) {
    LogMessage("Creating SDL3 window...");
    
    // Store the game window handle
    g_gameWindow = gameHwnd;
    
    // Hide the game window
    ShowWindow(g_gameWindow, SW_HIDE);
    
    // Get the game window's position and size
    RECT gameRect;
    GetWindowRect(g_gameWindow, &gameRect);
    
    // Create our SDL window
    g_sdlContext.window = SDL_CreateWindow(
        "Fighter Maker 2nd - SDL3",
        gameRect.right - gameRect.left,
        gameRect.bottom - gameRect.top,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
    );

    if (!g_sdlContext.window) {
        LogMessage("Failed to create SDL window");
        return false;
    }
    
    // Set window position
    SDL_SetWindowPosition(g_sdlContext.window, gameRect.left, gameRect.top);
    
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
        SDL_PIXELFORMAT_INDEX8
    );
    
    if (!gameSurface) {
        LogMessage("Failed to create game surface");
        return false;
    }
    
    // Create palette for 8-bit indexed color
    SDL_Palette* palette = SDL_CreatePalette(256);
    if (!palette) {
        LogMessage("Failed to create palette");
        SDL_DestroySurface(gameSurface);
        return false;
    }
    
    // Set the palette for the surface
    if (!SDL_SetSurfacePalette(gameSurface, palette)) {
        LogMessage("Failed to set surface palette");
        SDL_DestroyPalette(palette);
        SDL_DestroySurface(gameSurface);
        return false;
    }
    
    // Create texture from surface
    g_sdlContext.gameBuffer = SDL_CreateTextureFromSurface(g_sdlContext.renderer, gameSurface);
    if (!g_sdlContext.gameBuffer) {
        LogMessage("Failed to create game buffer texture");
        SDL_DestroyPalette(palette);
        SDL_DestroySurface(gameSurface);
        return false;
    }
    
    // Store the palette and surface for later use
    g_sdlContext.gamePalette = palette;
    g_sdlContext.gameSurface = gameSurface;
    
    // Create back buffer surface and texture (640x480)
    SDL_Surface* backSurface = SDL_CreateSurface(
        640, 480,
        SDL_PIXELFORMAT_INDEX8
    );
    
    if (!backSurface) {
        LogMessage("Failed to create back buffer surface");
        return false;
    }
    
    // Set the palette for the back buffer
    if (!SDL_SetSurfacePalette(backSurface, palette)) {
        LogMessage("Failed to set back buffer palette");
        SDL_DestroySurface(backSurface);
        return false;
    }
    
    // Create texture from surface
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
        SDL_PIXELFORMAT_INDEX8
    );
    
    if (!spriteSurface) {
        LogMessage("Failed to create sprite buffer surface");
        return false;
    }
    
    // Set the palette for the sprite buffer
    if (!SDL_SetSurfacePalette(spriteSurface, palette)) {
        LogMessage("Failed to set sprite buffer palette");
        SDL_DestroySurface(spriteSurface);
        return false;
    }
    
    // Create texture from surface
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
    
    // Set up DirectDraw global variables using IDA-verified addresses
    void** pDirectDraw = (void**)0x424758;       // g_direct_draw
    void** pPrimarySurface = (void**)0x424750;   // g_dd_primary_surface  
    void** pBackBuffer = (void**)0x424754;       // g_dd_back_buffer
    
    if (!IsBadWritePtr(pDirectDraw, sizeof(void*))) {
        *pDirectDraw = &g_directDraw;
        LogMessage("Set DirectDraw pointer at 0x424758");
    }
    
    if (!IsBadWritePtr(pPrimarySurface, sizeof(void*))) {
        g_primarySurface.texture = g_sdlContext.gameBuffer;
        *pPrimarySurface = &g_primarySurface;
        LogMessage("Set primary surface pointer at 0x424750");
    }
    
    if (!IsBadWritePtr(pBackBuffer, sizeof(void*))) {
        g_backSurface.texture = g_sdlContext.backBuffer;
        *pBackBuffer = &g_backSurface;
        LogMessage("Set back buffer pointer at 0x424754");
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
    
    // Initialize virtual function tables for DirectDraw surfaces
    SetupSurfaceVirtualTables();
    
    LogMessage("DirectDraw SDL3 replacement setup complete");
}

void SetupSurfaceVirtualTables() {
    LogMessage("Setting up surface virtual tables...");
    
    // Initialize the virtual function table
    g_surfaceVtbl.QueryInterface = Surface_QueryInterface;
    g_surfaceVtbl.AddRef = Surface_AddRef;
    g_surfaceVtbl.Release = Surface_Release;
    g_surfaceVtbl.Lock = Surface_Lock;
    g_surfaceVtbl.Unlock = Surface_Unlock;
    g_surfaceVtbl.Blt = Surface_Blt;
    g_surfaceVtbl.Flip = Surface_Flip;
    g_surfaceVtbl.SetPalette = Surface_SetPalette;
    g_surfaceVtbl.GetDC = Surface_GetDC;
    g_surfaceVtbl.ReleaseDC = Surface_ReleaseDC;
    g_surfaceVtbl.GetAttachedSurface = Surface_GetAttachedSurface;
    g_surfaceVtbl.GetCaps = Surface_GetCaps;
    g_surfaceVtbl.GetPixelFormat = Surface_GetPixelFormat;
    g_surfaceVtbl.GetSurfaceDesc = Surface_GetSurfaceDesc;

    // Set up surface structures with virtual function table pointers
    g_primarySurface.lpVtbl = &g_surfaceVtbl;
    g_primarySurface.texture = g_sdlContext.gameBuffer;
    g_primarySurface.locked = false;
    g_primarySurface.refCount = 1;
    g_primarySurface.isPrimary = true;
    g_primarySurface.isBackBuffer = false;
    g_primarySurface.isSprite = false;
    
    g_backSurface.lpVtbl = &g_surfaceVtbl;
    g_backSurface.texture = g_sdlContext.backBuffer;
    g_backSurface.locked = false;
    g_backSurface.refCount = 1;
    g_backSurface.isPrimary = false;
    g_backSurface.isBackBuffer = true;
    g_backSurface.isSprite = false;
    
    g_spriteSurface.lpVtbl = &g_surfaceVtbl;
    g_spriteSurface.texture = g_sdlContext.spriteBuffer;
    g_spriteSurface.locked = false;
    g_spriteSurface.refCount = 1;
    g_spriteSurface.isPrimary = false;
    g_spriteSurface.isBackBuffer = false;
    g_spriteSurface.isSprite = true;
    
    LogMessage("DirectDraw surface virtual function tables initialized successfully");
}

// DirectDraw Surface Method Implementations
HRESULT STDMETHODCALLTYPE Surface_QueryInterface(void* This, REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = This;
    ((SDL3Surface*)This)->refCount++;
    return S_OK;
}

ULONG STDMETHODCALLTYPE Surface_AddRef(void* This) {
    SDL3Surface* surface = (SDL3Surface*)This;
    return ++surface->refCount;
}

ULONG STDMETHODCALLTYPE Surface_Release(void* This) {
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
    LogMessage("Surface_GetAttachedSurface called (STUB)");
    if (lplpDDAttachedSurface) *lplpDDAttachedSurface = nullptr;
    return DD_OK;
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
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface || !lpDDPalette) return DDERR_INVALIDPARAMS;
    
    // Get the palette entries from the DirectDraw palette
    PALETTEENTRY entries[256];
    IDirectDrawPalette* ddPalette = (IDirectDrawPalette*)lpDDPalette;
    if (ddPalette) {
        ddPalette->GetEntries(0, 0, 256, entries);
        
        // Convert DirectDraw palette entries to SDL colors
        SDL_Color colors[256];
        for (int i = 0; i < 256; i++) {
            colors[i].r = entries[i].peRed;
            colors[i].g = entries[i].peGreen;
            colors[i].b = entries[i].peBlue;
            colors[i].a = 255;  // Full opacity
        }
        
        // Update the palette colors
        if (!SDL_SetPaletteColors(g_sdlContext.gamePalette, colors, 0, 256)) {
            LogMessage("Failed to set palette colors");
            return DDERR_GENERIC;
        }
        
        // Update the surface palette
        if (!SDL_SetSurfacePalette(surface->surface, g_sdlContext.gamePalette)) {
            LogMessage("Failed to set surface palette");
            return DDERR_GENERIC;
        }
        
        // If this is a primary surface, update all other surfaces that share the palette
        if (surface->isPrimary) {
            // Update back buffer palette
            if (g_sdlContext.backSurface) {
                SDL_SetSurfacePalette(g_sdlContext.backSurface, g_sdlContext.gamePalette);
            }
            
            // Update sprite buffer palette
            if (g_sdlContext.spriteSurface) {
                SDL_SetSurfacePalette(g_sdlContext.spriteSurface, g_sdlContext.gamePalette);
            }
        }
    } else {
        // Remove palette
        if (!SDL_SetSurfacePalette(surface->surface, NULL)) {
            LogMessage("Failed to remove surface palette");
            return DDERR_GENERIC;
        }
    }
    
    LogMessage("Surface_SetPalette called");
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

bool InitializeHooks() {
    LogMessage("Initializing hooks...");
    
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        LogMessage("ERROR: Failed to initialize MinHook");
        return false;
    }
    
    // Create hooks for the game's functions
    HMODULE hModule = GetModuleHandle(NULL);
    if (!hModule) {
        LogMessage("ERROR: Failed to get module handle");
        return false;
    }
    
    // Hook the game initialization function
    FARPROC pInitGame = GetProcAddress(hModule, "initialize_game");
    if (pInitGame) {
        if (MH_CreateHook((void*)pInitGame, (void*)&InitGame_Hook, (void**)&original_initialize_game) != MH_OK) {
            LogMessage("ERROR: Failed to create initialize_game hook");
            return false;
        }
    }
    
    // Hook the DirectDraw initialization function
    FARPROC pInitDirectDraw = GetProcAddress(hModule, "initialize_directdraw_mode");
    if (pInitDirectDraw) {
        if (MH_CreateHook((void*)pInitDirectDraw, (void*)&InitDirectDraw_Hook, (void**)&original_initialize_directdraw) != MH_OK) {
            LogMessage("ERROR: Failed to create initialize_directdraw hook");
            return false;
        }
    }
    
    // Hook the window procedure
    FARPROC pWndProc = GetProcAddress(hModule, "main_window_proc");
    if (pWndProc) {
        if (MH_CreateHook((void*)pWndProc, (void*)&WindowProc_Hook, (void**)&original_window_proc) != MH_OK) {
            LogMessage("ERROR: Failed to create window_proc hook");
            return false;
        }
    }

    // Hook the ProcessInputHistory function
    FARPROC pProcessInputHistory = GetProcAddress(hModule, "process_input_history");
    if (pProcessInputHistory) {
        if (MH_CreateHook((LPVOID)pProcessInputHistory, (void*)&Hook_ProcessInputHistory, (void**)&original_process_input_history) != MH_OK) {
            LogMessage("Failed to create hook for ProcessInputHistory");
            return false;
        }
    }
    
    // Enable all hooks
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        LogMessage("ERROR: Failed to enable hooks");
        return false;
    }
    
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
    
    // Set up DirectDraw surface interception
    SetupDirectDrawSurfaces();
    
    // Call original function
    BOOL result = original_initialize_directdraw(isFullScreen, windowHandle);
    if (!result) {
        LogMessage("Original initialize_directdraw failed");
        return result;
    }
    
    return TRUE;
}

LRESULT CALLBACK WindowProc_Hook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Forward to original window proc
    return original_window_proc(hwnd, msg, wParam, lParam);
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