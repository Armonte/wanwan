#include "ddraw_hooks.h"
#include "sdl3_hooks.h"
#include <cstdio>

// --- Global Instances of our Fake Objects ---
static IDirectDrawVtbl g_ddraw_vtable;
static IDirectDrawSurfaceVtbl g_surface_vtable;
static SDL3DirectDraw g_fake_ddraw;
static SDL3Surface g_fake_primary_surface;
static SDL3Surface g_fake_back_surface;

void LogMessage(const char* message); // Forward declaration

// --- Helper Functions ---
HRESULT STDMETHODCALLTYPE DD_Stub() {
    return DD_OK;
}

static void FillVtableStubs(void* vtable, size_t size) {
    void** vtable_ptr = (void**)vtable;
    size_t count = size / sizeof(void*);
    for (size_t i = 0; i < count; ++i) {
        vtable_ptr[i] = (void*)DD_Stub;
    }
}

// --- IDirectDraw Method Implementations ---

HRESULT STDMETHODCALLTYPE DD_QueryInterface(void* This, REFIID riid, void** ppvObject) {
    LogMessage("IDirectDraw::QueryInterface");
    *ppvObject = This;
    return S_OK;
}

ULONG STDMETHODCALLTYPE DD_AddRef(void* This) {
    ULONG refCount = ++((SDL3DirectDraw*)This)->refCount;
    LogMessage("IDirectDraw::AddRef");
    return refCount;
}

ULONG STDMETHODCALLTYPE DD_Release(void* This) {
    ULONG refCount = --((SDL3DirectDraw*)This)->refCount;
    LogMessage("IDirectDraw::Release");
    return refCount;
}

HRESULT STDMETHODCALLTYPE DD_CreateSurface(void* This, void* lpDDSurfaceDesc, void** lplpDDSurface, void* pUnkOuter) {
    LogMessage("IDirectDraw::CreateSurface");
    DDSURFACEDESC* desc = (DDSURFACEDESC*)lpDDSurfaceDesc;

    if (desc->dwFlags & DDSD_CAPS && desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
        LogMessage(" -> Requested Primary Surface");
        *lplpDDSurface = &g_fake_primary_surface;
        return DD_OK;
    }
    
    // For now, any other surface request will also get the primary surface.
    LogMessage(" -> Requested Other Surface (returning primary for now)");
    *lplpDDSurface = &g_fake_primary_surface;
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DD_SetCooperativeLevel(void* This, HWND hWnd, DWORD dwFlags) {
    LogMessage("IDirectDraw::SetCooperativeLevel");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DD_SetDisplayMode(void* This, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP) {
    char buffer[128];
    sprintf_s(buffer, sizeof(buffer), "IDirectDraw::SetDisplayMode - %dx%d %d bpp", dwWidth, dwHeight, dwBPP);
    LogMessage(buffer);
    SDL_SetWindowSize(g_sdlContext.window, dwWidth, dwHeight);
    return DD_OK;
}

// --- IDirectDrawSurface Method Implementations ---

HRESULT STDMETHODCALLTYPE DDS_QueryInterface(void* This, REFIID riid, void** ppvObject) {
    LogMessage("IDirectDrawSurface::QueryInterface");
    *ppvObject = This;
    return S_OK;
}

ULONG STDMETHODCALLTYPE DDS_AddRef(void* This) {
    ULONG refCount = ++((SDL3Surface*)This)->refCount;
    LogMessage("IDirectDrawSurface::AddRef");
    return refCount;
}

ULONG STDMETHODCALLTYPE DDS_Release(void* This) {
    ULONG refCount = --((SDL3Surface*)This)->refCount;
    LogMessage("IDirectDrawSurface::Release");
    return refCount;
}

HRESULT STDMETHODCALLTYPE DDS_Lock(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent) {
    LogMessage("IDirectDrawSurface::Lock");
    SDL3Surface* surface = (SDL3Surface*)This;
    DDSURFACEDESC* desc = (DDSURFACEDESC*)lpDDSurfaceDesc;
    
    void* pixels;
    int pitch;
    SDL_LockTexture(surface->backingTexture, NULL, &pixels, &pitch);

    desc->dwSize = sizeof(DDSURFACEDESC);
    desc->dwFlags = DDSD_PITCH | DDSD_LPSURFACE;
    desc->lPitch = pitch;
    desc->lpSurface = pixels;

    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DDS_Unlock(void* This, void* lpRect) {
    LogMessage("IDirectDrawSurface::Unlock");
    SDL3Surface* surface = (SDL3Surface*)This;
    SDL_UnlockTexture(surface->backingTexture);
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DDS_Blt(void* This, LPRECT lpDestRect, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx) {
    LogMessage("IDirectDrawSurface::Blt");
    SDL3Surface* src = (SDL3Surface*)lpDDSrcSurface;
    SDL3Surface* dst = (SDL3Surface*)This;

    if (!src || !dst || !src->backingTexture || !dst->backingTexture) {
        return DDERR_INVALIDPARAMS;
    }

    SDL_FRect src_rect = { (float)lpSrcRect->left, (float)lpSrcRect->top, (float)(lpSrcRect->right - lpSrcRect->left), (float)(lpSrcRect->bottom - lpSrcRect->top) };
    SDL_FRect dst_rect = { (float)lpDestRect->left, (float)lpDestRect->top, (float)(lpDestRect->right - lpDestRect->left), (float)(lpDestRect->bottom - lpDestRect->top) };

    SDL_SetRenderTarget(g_sdlContext.renderer, dst->backingTexture);
    SDL_RenderTexture(g_sdlContext.renderer, src->backingTexture, &src_rect, &dst_rect);
    SDL_SetRenderTarget(g_sdlContext.renderer, NULL); 

    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DDS_Flip(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags) {
    LogMessage("IDirectDrawSurface::Flip");
    RenderGame();
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DDS_GetAttachedSurface(void* This, void* lpDDSCaps, void** lplpDDAttachedSurface) {
    LogMessage("IDirectDrawSurface::GetAttachedSurface");
    LogMessage(" -> Returning back buffer");
    *lplpDDAttachedSurface = &g_fake_back_surface;
    return DD_OK;
}

// --- Initialization ---

void InitializeDirectDrawHooks() {
    LogMessage("Initializing DirectDraw hooks...");

    FillVtableStubs(&g_ddraw_vtable, sizeof(IDirectDrawVtbl));
    FillVtableStubs(&g_surface_vtable, sizeof(IDirectDrawSurfaceVtbl));
    
    g_ddraw_vtable.QueryInterface = DD_QueryInterface;
    g_ddraw_vtable.AddRef = DD_AddRef;
    g_ddraw_vtable.Release = DD_Release;
    g_ddraw_vtable.CreateSurface = DD_CreateSurface;
    g_ddraw_vtable.SetCooperativeLevel = DD_SetCooperativeLevel;
    g_ddraw_vtable.SetDisplayMode = DD_SetDisplayMode;
    LogMessage(" -> DirectDraw vtable populated.");

    g_surface_vtable.QueryInterface = DDS_QueryInterface;
    g_surface_vtable.AddRef = DDS_AddRef;
    g_surface_vtable.Release = DDS_Release;
    g_surface_vtable.Lock = DDS_Lock;
    g_surface_vtable.Unlock = DDS_Unlock;
    g_surface_vtable.Blt = DDS_Blt;
    g_surface_vtable.Flip = DDS_Flip;
    g_surface_vtable.GetAttachedSurface = DDS_GetAttachedSurface;
    LogMessage(" -> Surface vtable populated.");

    g_fake_ddraw.lpVtbl = &g_ddraw_vtable;
    g_fake_ddraw.refCount = 1;
    g_fake_ddraw.initialized = true;
    g_fake_ddraw.primarySurface = &g_fake_primary_surface;
    g_fake_ddraw.backSurface = &g_fake_back_surface;

    g_fake_primary_surface.lpVtbl = &g_surface_vtable;
    g_fake_primary_surface.refCount = 1;
    g_fake_primary_surface.isPrimary = true;
    g_fake_primary_surface.backingTexture = g_sdlContext.gameTexture;

    g_fake_back_surface.lpVtbl = &g_surface_vtable;
    g_fake_back_surface.refCount = 1;
    g_fake_back_surface.isBackBuffer = true;
    g_fake_back_surface.backingTexture = g_sdlContext.gameTexture;

    LogMessage("DirectDraw hooks initialization complete.");
}

SDL3DirectDraw* GetFakeDirectDraw() {
    if (!g_fake_ddraw.initialized) {
        InitializeDirectDrawHooks();
    }
    return &g_fake_ddraw;
}

void CleanupDirectDrawHooks() {
    LogMessage("DirectDraw hooks cleaned up.");
} 