#pragma once

#include <windows.h>
#include <SDL3/SDL.h>
#include "ddraw_types.h"

// Global SDL3 textures (simplified)
extern SDL_Texture* g_primaryTexture;
extern SDL_Texture* g_backTexture;
extern SDL_Texture* g_spriteTexture;

// Surface management functions
void InitializeSurfacePointers();
bool CreateSDLTextures();
void UpdateMemoryPointers();

// Hook implementations
int __cdecl initDirectDraw_new(int isFullScreen, void* windowHandle);
HRESULT __cdecl initializeResourceHandlers_new();
int __cdecl ProcessScreenUpdatesAndResources_new();