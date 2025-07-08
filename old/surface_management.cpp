#include "surface_management.hpp"
#include "sdl3_context.hpp"
#include <SDL3/SDL.h>
#include <windows.h>
#include <cstring>
#include <cstdio>

namespace argentum::hooks {
    
    // SDL texture management globals
    SDL_Texture* g_primaryTexture = nullptr;      // For primarySurface (screen buffer)
    SDL_Texture* g_spriteTexture = nullptr;       // For spriteSurface (256x256)
    SDL_Texture* g_backTexture = nullptr;         // For backBufferSurface
    SDL_Texture* g_graphicsTexture = nullptr;     // For screenBuffer
    
    void* g_primaryPixels = nullptr;              // Locked pixel data
    void* g_spritePixels = nullptr;
    void* g_backPixels = nullptr;
    void* g_graphicsPixels = nullptr;
    
    // Forward declarations for palette functions (to avoid circular dependency)
    HRESULT __stdcall CreatePalette_new(void* This, DWORD dwFlags, void* lpDDColorArray, void** lplpDDPalette, void* pUnkOuter);
    
    // Surface implementations
    HRESULT __stdcall SurfaceQueryInterface_new(void* This, void* riid, void** ppvObj) {
        *ppvObj = This;
        return S_OK;
    }
    
    ULONG __stdcall SurfaceAddRef_new(void* This) {
        return 1;
    }
    
    ULONG __stdcall SurfaceRelease_new(void* This) {
        return 0;
    }
    
    HRESULT __stdcall SurfaceSetPalette_new(void* This, void* lpDDPalette) {
        return S_OK;
    }
    
    // Surface locking - provides access to SDL texture pixels
    HRESULT __stdcall SurfaceLock_new(void* This, void* lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, void* hEvent) {
        DummyDirectDrawSurface* surface = (DummyDirectDrawSurface*)This;
        if (surface->isLocked) {
            return DDERR_SURFACEBUSY;
        }
        
        if (!surface->sdlTexture) {
            return DDERR_GENERIC;
        }
        
        int pitch;
        void* pixels;
        if (SDL_LockTexture(surface->sdlTexture, NULL, &pixels, &pitch) < 0) {
            return DDERR_GENERIC;
        }
        
        if (!pixels || pitch <= 0) {
            SDL_UnlockTexture(surface->sdlTexture);
            return DDERR_GENERIC;
        }
        
        surface->lockedPixels = pixels;
        surface->pitch = pitch;
        surface->isLocked = true;
        
        // Fill the DirectDraw surface descriptor if provided
        if (lpDDSurfaceDesc) {
            int* desc = (int*)lpDDSurfaceDesc;
            desc[0] = 108;                    // dwSize
            desc[1] = 0x1 | 0x4 | 0x8 | 0x20; // dwFlags
            desc[2] = surface->height;        // dwHeight
            desc[3] = surface->width;         // dwWidth
            desc[4] = pitch;                  // lPitch
            desc[9] = (int)pixels;            // lpSurface
        }
        
        return S_OK;
    }
    
    HRESULT __stdcall SurfaceUnlock_new(void* This, void* lpSurfaceData) {
        DummyDirectDrawSurface* surface = (DummyDirectDrawSurface*)This;
        if (!surface->isLocked) {
            return DDERR_NOTLOCKED;
        }
        
        SDL_UnlockTexture(surface->sdlTexture);
        surface->lockedPixels = nullptr;
        surface->isLocked = false;
        
        return S_OK;
    }
    
    static DummyDirectDrawSurface* g_backSurfacePtr = nullptr;
    
    HRESULT __stdcall SurfaceGetAttachedSurface_new(void* This, void* lpDDSCaps, void** lplpDDAttachedSurface) {
        *lplpDDAttachedSurface = g_backSurfacePtr;
        return S_OK;
    }
    
    HRESULT __stdcall SurfaceSetClipper_new(void* This, void* lpDDClipper) {
        return S_OK;
    }
    
    HRESULT __stdcall SurfaceBlt_new(void* This, void* lpDestRect, void* lpDDSrcSurface, void* lpSrcRect, DWORD dwFlags, void* lpDDBltFx) {
        return S_OK;
    }
    
    HRESULT __stdcall SurfaceFlip_new(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags) {
        if (g_sdlContext.initialized && g_sdlContext.renderer) {
            SDL_RenderPresent(g_sdlContext.renderer);
        }
        return S_OK;
    }
    
    // DirectDraw object methods
    HRESULT __stdcall DDQueryInterface_new(void* This, void* riid, void** ppvObj) {
        *ppvObj = This;
        return S_OK;
    }
    
    ULONG __stdcall DDAddRef_new(void* This) {
        return 1;
    }
    
    ULONG __stdcall DDRelease_new(void* This) {
        return 0;
    }
    
    HRESULT __stdcall DDSetCooperativeLevel_new(void* This, void* hWnd, DWORD dwFlags) {
        return S_OK;
    }
    
    HRESULT __stdcall DDSetDisplayMode_new(void* This, DWORD dwWidth, DWORD dwHeight, DWORD dwBPP) {
        return S_OK;
    }
    
    HRESULT __stdcall DDCreateSurface_new(void* This, void* lpDDSurfaceDesc, void** lplpDDSurface, void* pUnkOuter);
    HRESULT __stdcall DDCreateClipper_new(void* This, DWORD dwFlags, void** lplpDDClipper, void* pUnkOuter) {
        static int dummyClipper = 0x12345678;
        *lplpDDClipper = &dummyClipper;
        return S_OK;
    }
    
    // VTables
    static DummyDirectDrawSurfaceVTable g_surfaceVTable = {
        SurfaceQueryInterface_new,  // offset 0x00
        SurfaceAddRef_new,          // offset 0x04
        SurfaceRelease_new,         // offset 0x08
        
        // Reserved slots to fill space to offset 0x64 (22 slots = 88 bytes)
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        
        SurfaceLock_new,            // offset 0x64
        
        // Reserved slots to fill space to offset 0x80 (6 slots = 24 bytes)
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        
        SurfaceUnlock_new,          // offset 0x80
        
        // Additional methods
        SurfaceSetPalette_new,
        SurfaceGetAttachedSurface_new,
        SurfaceSetClipper_new,
        SurfaceBlt_new,
        SurfaceFlip_new
    };
    
    static DummyDirectDrawVTable g_ddVTable = {
        DDQueryInterface_new,
        DDAddRef_new,
        DDRelease_new,
        DDSetCooperativeLevel_new,
        DDSetDisplayMode_new,
        DDCreateSurface_new,
        CreatePalette_new,
        DDCreateClipper_new
    };
    
    // Global surfaces definitions
    DummyDirectDrawSurface g_primarySurface = {
        &g_surfaceVTable, 1, nullptr, nullptr, 640, 480, 0, false
    };
    
    DummyDirectDrawSurface g_backSurface = {
        &g_surfaceVTable, 1, nullptr, nullptr, 640, 480, 0, false
    };
    
    DummyDirectDrawSurface g_spriteSurface = {
        &g_surfaceVTable, 1, nullptr, nullptr, 256, 256, 0, false
    };
    
    DummyDirectDrawSurface g_graphicsSurface = {
        &g_surfaceVTable, 1, nullptr, nullptr, 640, 480, 0, false
    };
    
    DummyDirectDraw g_dummyDirectDraw = {
        &g_ddVTable, 1
    };
    
    void InitializeSurfacePointers() {
        g_backSurfacePtr = &g_backSurface;
    }
    
    HRESULT __stdcall DDCreateSurface_new(void* This, void* lpDDSurfaceDesc, void** lplpDDSurface, void* pUnkOuter) {
        if (lpDDSurfaceDesc) {
            int* desc = (int*)lpDDSurfaceDesc;
            int dwFlags = desc[1];
            int dwHeight = desc[2];
            int dwWidth = desc[3];
            int dwCaps = desc[6];
            
            if (dwCaps & 0x200) {  // DDSCAPS_PRIMARYSURFACE
                *lplpDDSurface = &g_primarySurface;
            } else if (dwCaps & 0x4) {  // DDSCAPS_BACKBUFFER  
                *lplpDDSurface = &g_backSurface;
            } else if (dwWidth == 256 && dwHeight == 256) {
                *lplpDDSurface = &g_spriteSurface;
            } else {
                *lplpDDSurface = &g_graphicsSurface;
            }
        } else {
            *lplpDDSurface = &g_primarySurface;
        }
        
        return S_OK;
    }
    
    // Create SDL textures for all surfaces
    bool CreateSDLTextures() {
        if (!g_sdlContext.initialized || !g_sdlContext.renderer) {
            return false;
        }
        
        // Create primary surface texture
        g_primaryTexture = SDL_CreateTexture(g_sdlContext.renderer, SDL_PIXELFORMAT_RGBA8888, 
                                           SDL_TEXTUREACCESS_STREAMING, 640, 480);
        if (!g_primaryTexture) {
            printf("SDL3 ERROR: Failed to create primary texture: %s\n", SDL_GetError());
            return false;
        }
        // Set nearest neighbor for crisp pixel art
        SDL_SetTextureScaleMode(g_primaryTexture, SDL_SCALEMODE_NEAREST);
        g_primarySurface.sdlTexture = g_primaryTexture;
        
        // Create back buffer texture  
        g_backTexture = SDL_CreateTexture(g_sdlContext.renderer, SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_STREAMING, 640, 480);
        if (!g_backTexture) {
            printf("SDL3 ERROR: Failed to create back buffer texture: %s\n", SDL_GetError());
            return false;
        }
        // Set nearest neighbor for crisp pixel art
        SDL_SetTextureScaleMode(g_backTexture, SDL_SCALEMODE_NEAREST);
        g_backSurface.sdlTexture = g_backTexture;
        
        // Create sprite surface texture - use RGB format for palette conversion
        g_spriteTexture = SDL_CreateTexture(g_sdlContext.renderer, SDL_PIXELFORMAT_RGB24,
                                          SDL_TEXTUREACCESS_STREAMING, 256, 256);
        if (!g_spriteTexture) {
            printf("SDL3 ERROR: Failed to create sprite texture: %s\n", SDL_GetError());
            return false;
        }
        // Set nearest neighbor for crisp pixel art (already had this one)
        SDL_SetTextureScaleMode(g_spriteTexture, SDL_SCALEMODE_NEAREST);
        g_spriteSurface.sdlTexture = g_spriteTexture;
        
        // Create graphics surface texture
        g_graphicsTexture = SDL_CreateTexture(g_sdlContext.renderer, SDL_PIXELFORMAT_RGBA8888,
                                            SDL_TEXTUREACCESS_STREAMING, 640, 480);
        if (!g_graphicsTexture) {
            printf("SDL3 ERROR: Failed to create graphics texture: %s\n", SDL_GetError());
            return false;
        }
        // Set nearest neighbor for crisp pixel art
        SDL_SetTextureScaleMode(g_graphicsTexture, SDL_SCALEMODE_NEAREST);
        g_graphicsSurface.sdlTexture = g_graphicsTexture;
        
        printf("SDL3 TEXTURES: All game textures created with NEAREST NEIGHBOR filtering for crisp pixel art\n");
        return true;
    }
    
    // Cleanup SDL textures
    void CleanupSDLTextures() {
        if (g_primaryTexture) {
            SDL_DestroyTexture(g_primaryTexture);
            g_primaryTexture = nullptr;
            g_primarySurface.sdlTexture = nullptr;
        }
        
        if (g_backTexture) {
            SDL_DestroyTexture(g_backTexture);
            g_backTexture = nullptr;
            g_backSurface.sdlTexture = nullptr;
        }
        
        if (g_spriteTexture) {
            SDL_DestroyTexture(g_spriteTexture);
            g_spriteTexture = nullptr;
            g_spriteSurface.sdlTexture = nullptr;
        }
        
        if (g_graphicsTexture) {
            SDL_DestroyTexture(g_graphicsTexture);
            g_graphicsTexture = nullptr;
            g_graphicsSurface.sdlTexture = nullptr;
        }
    }
    
} // namespace argentum::hooks 