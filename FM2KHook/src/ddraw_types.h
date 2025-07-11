#pragma once

#include <windows.h>
#include <unknwn.h>  // For IUnknown
#include <ddraw.h>   // For DirectDraw types

// DirectDraw COM interface GUIDs
extern const GUID IID_IDirectDraw;
extern const GUID IID_IDirectDrawSurface;

// Forward declarations
struct SDL3Surface;

// SDL3DirectDraw - Our implementation of IDirectDraw
struct SDL3DirectDraw {
    IDirectDraw* lpVtbl;  // Must be first member (COM object layout)
    bool initialized;
    SDL3Surface* primarySurface;
    SDL3Surface* backSurface;
    SDL3Surface* spriteSurface;
};

// SDL3Surface - Our implementation of IDirectDrawSurface
struct SDL3Surface {
    IDirectDrawSurface* lpVtbl;  // Must be first member (COM object layout)
    SDL_Texture* texture;
    void* pixels;
    int width;
    int height;
    int pitch;
    bool locked;
    DWORD lastLockFlags;
};

// DirectDraw vtable structure
struct DirectDrawVtbl {
    // IUnknown methods
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDirectDraw* This, REFIID riid, LPVOID* ppvObj);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDirectDraw* This);
    ULONG (STDMETHODCALLTYPE *Release)(IDirectDraw* This);

    // IDirectDraw methods
    HRESULT (STDMETHODCALLTYPE *Compact)(IDirectDraw* This);
    HRESULT (STDMETHODCALLTYPE *CreateClipper)(IDirectDraw* This, DWORD dwFlags, LPDIRECTDRAWCLIPPER* lplpDDClipper, IUnknown* pUnkOuter);
    HRESULT (STDMETHODCALLTYPE *CreatePalette)(IDirectDraw* This, DWORD dwFlags, LPPALETTEENTRY lpDDColorArray, LPDIRECTDRAWPALETTE* lplpDDPalette, IUnknown* pUnkOuter);
    HRESULT (STDMETHODCALLTYPE *CreateSurface)(IDirectDraw* This, LPDDSURFACEDESC lpDDSurfaceDesc, LPDIRECTDRAWSURFACE* lplpDDSurface, IUnknown* pUnkOuter);
    HRESULT (STDMETHODCALLTYPE *DuplicateSurface)(IDirectDraw* This, LPDIRECTDRAWSURFACE lpDDSurface, LPDIRECTDRAWSURFACE* lplpDupDDSurface);
    HRESULT (STDMETHODCALLTYPE *EnumDisplayModes)(IDirectDraw* This, DWORD dwFlags, LPDDSURFACEDESC lpDDSurfaceDesc, LPVOID lpContext, LPDDENUMMODESCALLBACK lpEnumModesCallback);
    HRESULT (STDMETHODCALLTYPE *EnumSurfaces)(IDirectDraw* This, DWORD dwFlags, LPDDSURFACEDESC lpDDSD, LPVOID lpContext, LPDDENUMSURFACESCALLBACK lpEnumSurfacesCallback);
    HRESULT (STDMETHODCALLTYPE *FlipToGDISurface)(IDirectDraw* This);
    HRESULT (STDMETHODCALLTYPE *GetCaps)(IDirectDraw* This, LPDDCAPS lpDDDriverCaps, LPDDCAPS lpDDHELCaps);
    HRESULT (STDMETHODCALLTYPE *GetDisplayMode)(IDirectDraw* This, LPDDSURFACEDESC lpDDSurfaceDesc);
    HRESULT (STDMETHODCALLTYPE *GetFourCCCodes)(IDirectDraw* This, LPDWORD lpNumCodes, LPDWORD lpCodes);
    HRESULT (STDMETHODCALLTYPE *GetGDISurface)(IDirectDraw* This, LPDIRECTDRAWSURFACE* lplpGDIDDSSurface);
    HRESULT (STDMETHODCALLTYPE *GetMonitorFrequency)(IDirectDraw* This, LPDWORD lpdwFrequency);
    HRESULT (STDMETHODCALLTYPE *GetScanLine)(IDirectDraw* This, LPDWORD lpdwScanLine);
    HRESULT (STDMETHODCALLTYPE *GetVerticalBlankStatus)(IDirectDraw* This, LPBOOL lpbIsInVB);
    HRESULT (STDMETHODCALLTYPE *Initialize)(IDirectDraw* This, GUID* lpGUID);
    HRESULT (STDMETHODCALLTYPE *RestoreDisplayMode)(IDirectDraw* This);
    HRESULT (STDMETHODCALLTYPE *SetCooperativeLevel)(IDirectDraw* This, HWND hWnd, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *SetDisplayMode)(IDirectDraw* This, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP);
    HRESULT (STDMETHODCALLTYPE *WaitForVerticalBlank)(IDirectDraw* This, DWORD dwFlags, HANDLE hEvent);
};

// DirectDrawSurface vtable structure
struct DirectDrawSurfaceVtbl {
    // IUnknown methods
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDirectDrawSurface* This, REFIID riid, LPVOID* ppvObj);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDirectDrawSurface* This);
    ULONG (STDMETHODCALLTYPE *Release)(IDirectDrawSurface* This);

    // IDirectDrawSurface methods
    HRESULT (STDMETHODCALLTYPE *AddAttachedSurface)(IDirectDrawSurface* This, LPDIRECTDRAWSURFACE lpDDSAttachedSurface);
    HRESULT (STDMETHODCALLTYPE *AddOverlayDirtyRect)(IDirectDrawSurface* This, LPRECT lpRect);
    HRESULT (STDMETHODCALLTYPE *Blt)(IDirectDrawSurface* This, LPRECT lpDestRect, LPDIRECTDRAWSURFACE lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx);
    HRESULT (STDMETHODCALLTYPE *BltBatch)(IDirectDrawSurface* This, LPDDBLTBATCH lpDDBltBatch, DWORD dwCount, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *BltFast)(IDirectDrawSurface* This, DWORD dwX, DWORD dwY, LPDIRECTDRAWSURFACE lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans);
    HRESULT (STDMETHODCALLTYPE *DeleteAttachedSurface)(IDirectDrawSurface* This, DWORD dwFlags, LPDIRECTDRAWSURFACE lpDDSAttachedSurface);
    HRESULT (STDMETHODCALLTYPE *EnumAttachedSurfaces)(IDirectDrawSurface* This, LPVOID lpContext, LPDDENUMSURFACESCALLBACK lpEnumSurfacesCallback);
    HRESULT (STDMETHODCALLTYPE *EnumOverlayZOrders)(IDirectDrawSurface* This, DWORD dwFlags, LPVOID lpContext, LPDDENUMSURFACESCALLBACK lpfnCallback);
    HRESULT (STDMETHODCALLTYPE *Flip)(IDirectDrawSurface* This, LPDIRECTDRAWSURFACE lpDDSurfaceTargetOverride, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *GetAttachedSurface)(IDirectDrawSurface* This, LPDDSCAPS lpDDSCaps, LPDIRECTDRAWSURFACE* lplpDDAttachedSurface);
    HRESULT (STDMETHODCALLTYPE *GetBltStatus)(IDirectDrawSurface* This, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *GetCaps)(IDirectDrawSurface* This, LPDDSCAPS lpDDSCaps);
    HRESULT (STDMETHODCALLTYPE *GetClipper)(IDirectDrawSurface* This, LPDIRECTDRAWCLIPPER* lplpDDClipper);
    HRESULT (STDMETHODCALLTYPE *GetColorKey)(IDirectDrawSurface* This, DWORD dwFlags, LPDDCOLORKEY lpDDColorKey);
    HRESULT (STDMETHODCALLTYPE *GetDC)(IDirectDrawSurface* This, HDC* lphDC);
    HRESULT (STDMETHODCALLTYPE *GetFlipStatus)(IDirectDrawSurface* This, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *GetOverlayPosition)(IDirectDrawSurface* This, LPLONG lplX, LPLONG lplY);
    HRESULT (STDMETHODCALLTYPE *GetPalette)(IDirectDrawSurface* This, LPDIRECTDRAWPALETTE* lplpDDPalette);
    HRESULT (STDMETHODCALLTYPE *GetPixelFormat)(IDirectDrawSurface* This, LPDDPIXELFORMAT lpDDPixelFormat);
    HRESULT (STDMETHODCALLTYPE *GetSurfaceDesc)(IDirectDrawSurface* This, LPDDSURFACEDESC lpDDSurfaceDesc);
    HRESULT (STDMETHODCALLTYPE *Initialize)(IDirectDrawSurface* This, LPDIRECTDRAW lpDD, LPDDSURFACEDESC lpDDSurfaceDesc);
    HRESULT (STDMETHODCALLTYPE *IsLost)(IDirectDrawSurface* This);
    HRESULT (STDMETHODCALLTYPE *Lock)(IDirectDrawSurface* This, LPRECT lpDestRect, LPDDSURFACEDESC lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent);
    HRESULT (STDMETHODCALLTYPE *ReleaseDC)(IDirectDrawSurface* This, HDC hDC);
    HRESULT (STDMETHODCALLTYPE *Restore)(IDirectDrawSurface* This);
    HRESULT (STDMETHODCALLTYPE *SetClipper)(IDirectDrawSurface* This, LPDIRECTDRAWCLIPPER lpDDClipper);
    HRESULT (STDMETHODCALLTYPE *SetColorKey)(IDirectDrawSurface* This, DWORD dwFlags, LPDDCOLORKEY lpDDColorKey);
    HRESULT (STDMETHODCALLTYPE *SetOverlayPosition)(IDirectDrawSurface* This, LONG lX, LONG lY);
    HRESULT (STDMETHODCALLTYPE *SetPalette)(IDirectDrawSurface* This, LPDIRECTDRAWPALETTE lpDDPalette);
    HRESULT (STDMETHODCALLTYPE *Unlock)(IDirectDrawSurface* This, LPVOID lpSurfaceData);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlay)(IDirectDrawSurface* This, LPRECT lpSrcRect, LPDIRECTDRAWSURFACE lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlayDisplay)(IDirectDrawSurface* This, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlayZOrder)(IDirectDrawSurface* This, DWORD dwFlags, LPDIRECTDRAWSURFACE lpDDSReference);
};

// Global vtable instances
    extern DirectDrawVtbl g_directDrawVtbl;
    extern DirectDrawSurfaceVtbl g_surfaceVtbl; 