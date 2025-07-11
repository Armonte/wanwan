#include "dummy_directdraw.h"
#include "sdl3_types.h"
#include "surface_manager.h"
#include <SDL3/SDL.h>

// External references
extern SDL3Context g_sdlContext;

// DirectDraw error codes if not defined
#ifndef DDERR_GENERIC
#define DDERR_GENERIC                   MAKE_HRESULT(1, 0x876, 1)
#define DDERR_INVALIDPARAMS            MAKE_HRESULT(1, 0x876, 2)
#define DDERR_UNSUPPORTED              MAKE_HRESULT(1, 0x876, 3)
#define DDERR_ALREADYINITIALIZED       MAKE_HRESULT(1, 0x876, 4)
#define DDERR_INVALIDOBJECT            MAKE_HRESULT(1, 0x876, 5)
#define DDERR_INVALIDMODE              MAKE_HRESULT(1, 0x876, 6)
#define DDERR_SURFACELOST              MAKE_HRESULT(1, 0x876, 7)
#define DDERR_NOTLOCKED                MAKE_HRESULT(1, 0x876, 8)
#define DDERR_SURFACEBUSY              MAKE_HRESULT(1, 0x876, 9)
#endif

// Dummy DirectDraw method implementations
HRESULT STDMETHODCALLTYPE DummyQueryInterface(void* This, REFIID riid, void** ppvObject) { return S_OK; }
ULONG STDMETHODCALLTYPE DummyAddRef(void* This) { return 1; }
ULONG STDMETHODCALLTYPE DummyRelease(void* This) { return 1; }
HRESULT STDMETHODCALLTYPE DummyMethod(void* This, ...) { return S_OK; }

// Critical Surface method implementations
HRESULT STDMETHODCALLTYPE Surface_GetAttachedSurface(void* This, void* lpDDSCaps, void** lplpDDAttachedSurface) {
    // Return the back buffer when primary surface is asked for attached surface
    if (lplpDDAttachedSurface) {
        // Get the proper SDL3Surface back buffer from surface manager
        *lplpDDAttachedSurface = GetBackSurface();
        return S_OK;
    }
    return DDERR_INVALIDPARAMS;
}

HRESULT STDMETHODCALLTYPE Surface_Lock(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent) {
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface || !surface->surface || !lpDDSurfaceDesc) {
        return DDERR_INVALIDPARAMS;
    }

    if (surface->locked) {
        return DDERR_SURFACEBUSY;
    }

    surface->locked = true;
    surface->lockFlags = dwFlags;

    // Fill in surface description
    DDSURFACEDESC* desc = (DDSURFACEDESC*)lpDDSurfaceDesc;
    desc->dwSize = sizeof(DDSURFACEDESC);
    desc->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_LPSURFACE;
    desc->dwWidth = surface->surface->w;
    desc->dwHeight = surface->surface->h;
    desc->lPitch = surface->surface->pitch;
    desc->lpSurface = surface->surface->pixels;

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

    surface->locked = false;
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Blt(void* This, LPRECT lpDestRect, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx) {
    SDL3Surface* destSurface = (SDL3Surface*)This;
    SDL3Surface* srcSurface = (SDL3Surface*)lpDDSrcSurface;
    
    if (!destSurface || !destSurface->surface) {
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

HRESULT STDMETHODCALLTYPE Surface_GetSurfaceDesc(void* This, void* lpDDSurfaceDesc) {
    SDL3Surface* surface = (SDL3Surface*)This;
    if (!surface || !surface->surface || !lpDDSurfaceDesc) {
        return DDERR_INVALIDPARAMS;
    }

    DDSURFACEDESC* desc = (DDSURFACEDESC*)lpDDSurfaceDesc;
    if (desc->dwSize < sizeof(DDSURFACEDESC)) {
        return DDERR_INVALIDPARAMS;
    }

    // Clear the structure first
    memset(desc, 0, sizeof(DDSURFACEDESC));
    
    // Fill in the surface description
    desc->dwSize = sizeof(DDSURFACEDESC);
    desc->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc->dwHeight = surface->surface->h;
    desc->dwWidth = surface->surface->w;
    desc->lPitch = surface->surface->pitch;
    
    // Fill in pixel format
    desc->ddpfPixelFormat.dwSize = sizeof(desc->ddpfPixelFormat);
    desc->ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc->ddpfPixelFormat.dwRGBBitCount = 32;
    desc->ddpfPixelFormat.dwRBitMask = 0x00FF0000;
    desc->ddpfPixelFormat.dwGBitMask = 0x0000FF00;
    desc->ddpfPixelFormat.dwBBitMask = 0x000000FF;
    desc->ddpfPixelFormat.dwRGBAlphaBitMask = 0xFF000000;
    
    // Fill in surface capabilities
    desc->ddsCaps.dwCaps = 0;
    if (surface->isPrimary) {
        desc->ddsCaps.dwCaps |= DDSCAPS_PRIMARYSURFACE;
    }
    if (surface->isBackBuffer) {
        desc->ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
    }
    desc->ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY;

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

HRESULT STDMETHODCALLTYPE Surface_EnumOverlayZOrders(void* This, DWORD dwFlags, LPVOID lpContext, void* lpfnCallback) {
    return DDERR_UNSUPPORTED;
}

// Minimal DirectDraw vtable
void* dummyDirectDrawVtable[] = {
    (void*)DummyQueryInterface,  // QueryInterface
    (void*)DummyAddRef,          // AddRef  
    (void*)DummyRelease,         // Release
    (void*)DummyMethod,          // All other methods just return S_OK
    (void*)DummyMethod, (void*)DummyMethod, (void*)DummyMethod, (void*)DummyMethod,
    (void*)DummyMethod, (void*)DummyMethod, (void*)DummyMethod, (void*)DummyMethod,
    (void*)DummyMethod, (void*)DummyMethod, (void*)DummyMethod, (void*)DummyMethod
};

// Minimal Surface vtable with critical methods at correct offsets
void* dummySurfaceVtable[] = {
    (void*)DummyQueryInterface,              // 0x00: QueryInterface
    (void*)DummyAddRef,                      // 0x04: AddRef
    (void*)DummyRelease,                     // 0x08: Release  
    (void*)DummyMethod,                      // 0x0C: AddAttachedSurface
    (void*)DummyMethod,                      // 0x10: AddOverlayDirtyRect
    (void*)Surface_Blt,                      // 0x14: Blt
    (void*)DummyMethod,                      // 0x18: BltBatch
    (void*)DummyMethod,                      // 0x1C: BltFast
    (void*)DummyMethod,                      // 0x20: DeleteAttachedSurface
    (void*)DummyMethod,                      // 0x24: EnumAttachedSurfaces
    (void*)DummyMethod,                      // 0x28: EnumOverlayZOrders
    (void*)Surface_Flip,                     // 0x2C: Flip
    (void*)Surface_GetAttachedSurface,       // 0x30: GetAttachedSurface *** CRITICAL ***
    (void*)DummyMethod,                      // 0x34: GetBltStatus
    (void*)DummyMethod,                      // 0x38: GetCaps
    (void*)DummyMethod,                      // 0x3C: GetClipper
    (void*)DummyMethod,                      // 0x40: GetColorKey
    (void*)DummyMethod,                      // 0x44: GetDC
    (void*)DummyMethod,                      // 0x48: GetFlipStatus
    (void*)DummyMethod,                      // 0x4C: GetOverlayPosition
    (void*)DummyMethod,                      // 0x50: GetPalette
    (void*)DummyMethod,                      // 0x54: GetPixelFormat
    (void*)Surface_GetSurfaceDesc,           // 0x58: GetSurfaceDesc *** CRITICAL ***
    (void*)DummyMethod,                      // 0x5C: Initialize
    (void*)DummyMethod,                      // 0x60: IsLost
    (void*)Surface_Lock,                     // 0x64: Lock *** CRITICAL ***
    (void*)DummyMethod,                      // 0x68: ReleaseDC
    (void*)DummyMethod,                      // 0x6C: Restore
    (void*)DummyMethod,                      // 0x70: SetClipper
    (void*)DummyMethod,                      // 0x74: SetColorKey
    (void*)DummyMethod,                      // 0x78: SetOverlayPosition
    (void*)DummyMethod,                      // 0x7C: SetPalette
    (void*)Surface_Unlock,                   // 0x80: Unlock *** CRITICAL ***
    (void*)DummyMethod,                      // 0x84: UpdateOverlay
    (void*)DummyMethod,                      // 0x88: UpdateOverlayDisplay
    (void*)DummyMethod                       // 0x8C: UpdateOverlayZOrder
};

// Dummy objects with vtable pointers
void* dummyDirectDrawObj[] = { dummyDirectDrawVtable };
void* dummyPrimaryObj[] = { dummySurfaceVtable };  
void* dummyBackObj[] = { dummySurfaceVtable }; 