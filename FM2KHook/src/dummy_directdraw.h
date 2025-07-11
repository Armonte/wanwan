#pragma once
#include <windows.h>
#include <ddraw.h>
#include "sdl3_types.h"

// DirectDraw Surface Implementation (COM layout compatible)
struct SDL3Surface {
    void* lpVtbl;                   // Must be first (COM layout)
    SDL_Surface* surface;           // SDL surface for pixel access
    SDL_Texture* texture;           // SDL texture for rendering
    bool isPrimary;                 // Is this the primary surface?
    bool isBackBuffer;              // Is this the back buffer?
    bool isSprite;                  // Is this the sprite surface?
    LONG refCount;                  // COM reference count
    bool locked;                    // Surface lock state
    DWORD lockFlags;                // Last lock flags
};

// Dummy DirectDraw method implementations
extern "C" {
    // Dummy methods
    HRESULT STDMETHODCALLTYPE DummyQueryInterface(void* This, REFIID riid, void** ppvObject);
    ULONG STDMETHODCALLTYPE DummyAddRef(void* This);
    ULONG STDMETHODCALLTYPE DummyRelease(void* This);
    HRESULT STDMETHODCALLTYPE DummyMethod(void* This, ...);

    // Critical Surface methods
    HRESULT STDMETHODCALLTYPE Surface_GetAttachedSurface(void* This, void* lpDDSCaps, void** lplpDDAttachedSurface);
    HRESULT STDMETHODCALLTYPE Surface_Lock(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent);
    HRESULT STDMETHODCALLTYPE Surface_Unlock(void* This, void* lpRect);
    HRESULT STDMETHODCALLTYPE Surface_Blt(void* This, LPRECT lpDestRect, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx);
    HRESULT STDMETHODCALLTYPE Surface_Flip(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags);
    HRESULT STDMETHODCALLTYPE Surface_GetSurfaceDesc(void* This, void* lpDDSurfaceDesc);
    HRESULT STDMETHODCALLTYPE Surface_EnumOverlayZOrders(void* This, DWORD dwFlags, LPVOID lpContext, void* lpfnCallback);

    // Declare the dummy objects as extern
    extern void* dummyDirectDrawVtable[];
    extern void* dummySurfaceVtable[];
    extern void* dummyDirectDrawObj[];
    extern void* dummyPrimaryObj[];
    extern void* dummyBackObj[];
} 