#include "surface_manager.h"
#include "sdl3_types.h"
#include "dummy_directdraw.h"

// Global SDL3Surface instances
static SDL3Surface g_primarySurface = {0};
static SDL3Surface g_backSurface = {0};
static SDL3Surface g_spriteSurface = {0};

extern SDL3Context g_sdlContext;
extern void* dummySurfaceVtable[];

bool CreateSDL3Surfaces() {
    if (!g_sdlContext.initialized || !g_sdlContext.renderer) {
        return false;
    }
    
    // Create primary surface (256x240 for FM2K)
    SDL_Surface* primarySdlSurface = SDL_CreateSurface(256, 240, SDL_PIXELFORMAT_RGBA8888);
    if (!primarySdlSurface) return false;
    
    SDL_Texture* primaryTexture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, primarySdlSurface);
    if (!primaryTexture) {
        SDL_DestroySurface(primarySdlSurface);
        return false;
    }
    
    // Create back buffer surface
    SDL_Surface* backSdlSurface = SDL_CreateSurface(256, 240, SDL_PIXELFORMAT_RGBA8888);
    if (!backSdlSurface) return false;
    
    SDL_Texture* backTexture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, backSdlSurface);
    if (!backTexture) {
        SDL_DestroySurface(backSdlSurface);
        return false;
    }
    
    // Create sprite surface
    SDL_Surface* spriteSdlSurface = SDL_CreateSurface(256, 256, SDL_PIXELFORMAT_RGBA8888);
    if (!spriteSdlSurface) return false;
    
    SDL_Texture* spriteTexture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, spriteSdlSurface);
    if (!spriteTexture) {
        SDL_DestroySurface(spriteSdlSurface);
        return false;
    }
    
    // Initialize primary surface structure
    g_primarySurface.lpVtbl = dummySurfaceVtable;
    g_primarySurface.surface = primarySdlSurface;
    g_primarySurface.texture = primaryTexture;
    g_primarySurface.isPrimary = true;
    g_primarySurface.isBackBuffer = false;
    g_primarySurface.isSprite = false;
    g_primarySurface.refCount = 1;
    g_primarySurface.locked = false;
    g_primarySurface.lockFlags = 0;
    
    // Initialize back buffer structure
    g_backSurface.lpVtbl = dummySurfaceVtable;
    g_backSurface.surface = backSdlSurface;
    g_backSurface.texture = backTexture;
    g_backSurface.isPrimary = false;
    g_backSurface.isBackBuffer = true;
    g_backSurface.isSprite = false;
    g_backSurface.refCount = 1;
    g_backSurface.locked = false;
    g_backSurface.lockFlags = 0;
    
    // Initialize sprite surface structure
    g_spriteSurface.lpVtbl = dummySurfaceVtable;
    g_spriteSurface.surface = spriteSdlSurface;
    g_spriteSurface.texture = spriteTexture;
    g_spriteSurface.isPrimary = false;
    g_spriteSurface.isBackBuffer = false;
    g_spriteSurface.isSprite = true;
    g_spriteSurface.refCount = 1;
    g_spriteSurface.locked = false;
    g_spriteSurface.lockFlags = 0;
    
    return true;
}

SDL3Surface* GetPrimarySurface() {
    return &g_primarySurface;
}

SDL3Surface* GetBackSurface() {
    return &g_backSurface;
}

SDL3Surface* GetSpriteSurface() {
    return &g_spriteSurface;
}