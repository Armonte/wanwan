#pragma once

#include <windows.h>
#include <SDL3/SDL.h>

namespace FM2K {
namespace DirectDrawCompat {

// Dummy DirectDraw structures for compatibility
struct DummyDirectDraw {
    void* vtable;
    // Add other fields as needed
};

struct DummySurface {
    void* vtable;
    SDL_Texture* texture;
    int width;
    int height;
    int pitch;
    void* pixels;
};

// Global dummy objects
extern DummyDirectDraw g_dummyDirectDraw;
extern DummySurface g_primarySurface;
extern DummySurface g_backSurface;
extern DummySurface g_spriteSurface;
extern DummySurface g_graphicsSurface;

// Global textures
extern SDL_Texture* g_primaryTexture;
extern SDL_Texture* g_backTexture;
extern SDL_Texture* g_spriteTexture;
extern SDL_Texture* g_graphicsTexture;

// DirectDraw function hooks
int __cdecl initDirectDraw_new(int isFullScreen, void* windowHandle);
int __cdecl ProcessScreenUpdatesAndResources_new();
HRESULT __cdecl initializeResourceHandlers_new();

// Surface management
void InitializeSurfacePointers();
bool CreateSDLTextures();
void UpdateMemoryPointers();

} // namespace DirectDrawCompat
} // namespace FM2K