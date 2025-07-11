#pragma once

#include <windows.h>
#include <ddraw.h>
#include <SDL3/SDL.h>

// --- Fake COM Interface Structures ---

// A forward declaration of our fake DirectDraw surface structure.
struct SDL3Surface;

// The vtable for our fake IDirectDraw interface.
// The order of these function pointers MUST EXACTLY MATCH the real IDirectDrawVtbl.
struct IDirectDrawVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* This, REFIID riid, void** ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(void* This);
    ULONG (STDMETHODCALLTYPE *Release)(void* This);
    HRESULT (STDMETHODCALLTYPE *Compact)(void* This);
    HRESULT (STDMETHODCALLTYPE *CreateClipper)(void* This, DWORD, void**, void*);
    HRESULT (STDMETHODCALLTYPE *CreatePalette)(void* This, DWORD, void*, void**, void*);
    HRESULT (STDMETHODCALLTYPE *CreateSurface)(void* This, void*, void**, void*);
    HRESULT (STDMETHODCALLTYPE *DuplicateSurface)(void* This, void*, void**);
    HRESULT (STDMETHODCALLTYPE *EnumDisplayModes)(void* This, DWORD, void*, void*, void*);
    HRESULT (STDMETHODCALLTYPE *EnumSurfaces)(void* This, DWORD, void*, void*, void*);
    HRESULT (STDMETHODCALLTYPE *FlipToGDISurface)(void* This);
    HRESULT (STDMETHODCALLTYPE *GetCaps)(void* This, void*, void*);
    HRESULT (STDMETHODCALLTYPE *GetDisplayMode)(void* This, void*);
    HRESULT (STDMETHODCALLTYPE *GetFourCCCodes)(void* This, LPDWORD, LPDWORD);
    HRESULT (STDMETHODCALLTYPE *GetGDISurface)(void* This, void**);
    HRESULT (STDMETHODCALLTYPE *GetMonitorFrequency)(void* This, LPDWORD);
    HRESULT (STDMETHODCALLTYPE *GetScanLine)(void* This, LPDWORD);
    HRESULT (STDMETHODCALLTYPE *GetVerticalBlankStatus)(void* This, LPBOOL);
    HRESULT (STDMETHODCALLTYPE *Initialize)(void* This, GUID*);
    HRESULT (STDMETHODCALLTYPE *RestoreDisplayMode)(void* This);
    HRESULT (STDMETHODCALLTYPE *SetCooperativeLevel)(void* This, HWND, DWORD);
    HRESULT (STDMETHODCALLTYPE *SetDisplayMode)(void* This, DWORD, DWORD, DWORD);
    HRESULT (STDMETHODCALLTYPE *WaitForVerticalBlank)(void* This, DWORD, HANDLE);
};

// The vtable for our fake IDirectDrawSurface interface.
// The order must also be exact.
struct IDirectDrawSurfaceVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* This, REFIID riid, void** ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(void* This);
    ULONG (STDMETHODCALLTYPE *Release)(void* This);
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
    HRESULT (STDMETHODCALLTYPE *Unlock)(void* This, void* lpRect);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlay)(void* This, LPRECT lpSrcRect, void* lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, void* lpDDOverlayFx);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlayDisplay)(void* This, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlayZOrder)(void* This, DWORD dwFlags, void* lpDDSReference);
};

// Our fake surface object. The vtable pointer must be the first member
// to maintain COM compatibility, as the game will treat a pointer to this
// struct as a pointer to the interface.
struct SDL3Surface {
    IDirectDrawSurfaceVtbl* lpVtbl;
    SDL_Surface* backingSurface; // The real SDL surface for pixel data.
    SDL_Texture* backingTexture; // The SDL texture for rendering.
    LONG refCount;
    bool isPrimary;
    bool isBackBuffer;
};

// Our fake DirectDraw object.
struct SDL3DirectDraw {
    IDirectDrawVtbl* lpVtbl;
    SDL3Surface* primarySurface;
    SDL3Surface* backSurface;
    LONG refCount;
    bool initialized;
};

// --- DirectDraw Hook Management ---

// Creates our fake DirectDraw object and all its associated surfaces.
void InitializeDirectDrawHooks();

// Returns the singleton instance of our fake DirectDraw object.
// The DirectDrawCreate hook will return a pointer to this.
SDL3DirectDraw* GetFakeDirectDraw();

// Cleans up all DirectDraw-related resources.
void CleanupDirectDrawHooks(); 