#pragma once

#include <windows.h>
#include <SDL3/SDL.h>
#include "ddraw_types.h"

namespace FM2K {
namespace DirectDrawCompat {

// Global DirectDraw objects
extern IDirectDraw* g_directDraw;
extern IDirectDrawSurface* g_primarySurface;
extern IDirectDrawSurface* g_backSurface;
extern IDirectDrawSurface* g_spriteSurface;

// Global SDL3 textures
extern SDL_Texture* g_primaryTexture;
extern SDL_Texture* g_backTexture;
extern SDL_Texture* g_spriteTexture;

// DirectDraw function hooks
int __cdecl Hook_InitializeDirectDraw(int isFullScreen, void* windowHandle);
int __cdecl Hook_DirectDrawCleanup();

// Surface management
void InitializeSurfacePointers();
bool CreateSDLTextures();
void UpdateMemoryPointers();

// Surface method implementations
HRESULT STDMETHODCALLTYPE Surface_QueryInterface(IDirectDrawSurface* This, REFIID riid, void** ppvObject);
ULONG STDMETHODCALLTYPE Surface_AddRef(IDirectDrawSurface* This);
ULONG STDMETHODCALLTYPE Surface_Release(IDirectDrawSurface* This);
HRESULT STDMETHODCALLTYPE Surface_Lock(IDirectDrawSurface* This, LPRECT lpDestRect, LPDDSURFACEDESC lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent);
HRESULT STDMETHODCALLTYPE Surface_Unlock(IDirectDrawSurface* This, LPVOID lpSurfaceData);
HRESULT STDMETHODCALLTYPE Surface_Blt(IDirectDrawSurface* This, LPRECT lpDestRect, LPDIRECTDRAWSURFACE lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx);

// DirectDraw method implementations
HRESULT STDMETHODCALLTYPE DirectDraw_QueryInterface(IDirectDraw* This, REFIID riid, void** ppvObject);
ULONG STDMETHODCALLTYPE DirectDraw_AddRef(IDirectDraw* This);
ULONG STDMETHODCALLTYPE DirectDraw_Release(IDirectDraw* This);
HRESULT STDMETHODCALLTYPE DirectDraw_CreateSurface(IDirectDraw* This, LPDDSURFACEDESC lpDDSurfaceDesc, LPDIRECTDRAWSURFACE* lplpDDSurface, IUnknown* pUnkOuter);
HRESULT STDMETHODCALLTYPE DirectDraw_SetCooperativeLevel(IDirectDraw* This, HWND hwnd, DWORD dwFlags);

} // namespace DirectDrawCompat
} // namespace FM2K