#pragma once

#include <SDL3/SDL.h>
#include <windows.h>

namespace FM2K {
namespace SDL3Integration {

struct SDL3Context {
    bool initialized = false;
    
    // SDL3 resources
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    
    // Textures for DirectDraw surface replacement
    SDL_Texture* gameBuffer = nullptr;      // 256x240 native game resolution
    SDL_Texture* backBuffer = nullptr;      // Window-sized back buffer
    SDL_Texture* spriteBuffer = nullptr;    // 256x256 sprite surface
    SDL_Texture* primaryTexture = nullptr;  // Primary surface replacement
    
    // Palette system
    SDL_Surface* indexedSurface = nullptr;
    SDL_Palette* sdlPalette = nullptr;
    SDL_Texture* paletteTexture = nullptr;
    
    // Window dimensions
    int windowWidth = 640;
    int windowHeight = 480;
    int gameWidth = 256;    // Native game resolution
    int gameHeight = 240;
    
    bool isFullscreen = false;
};

// Global SDL3 context
extern SDL3Context g_sdlContext;

// Core functions
bool InitializeSDL3Context(int isFullScreen, void* hwnd);
void CleanupSDL3Context();

// Window management
HWND CreateMainWindow_new(int displayMode, HINSTANCE hInstance, int nCmdShow);
void SubclassSDL3Window(HWND hwnd);
void UnsubclassSDL3Window(HWND hwnd);

// DirectDraw compatibility
int initDirectDraw_new(int isFullScreen, void* windowHandle);
int ProcessScreenUpdatesAndResources_new();

// Rendering
void SetGameRenderTarget();
void SetWindowRenderTarget();  
void RenderGameToWindow();
void PresentFrame();

// Input and events
void UpdateSDL3Events();
bool IsSDL3KeyPressed(int scancode);
bool IsAltEnterPressed();
bool ToggleFullscreen();

// Surface management
bool CreateSDLTextures();
bool CreateSDL3PaletteSystem();

// Utility functions
void PrintSDL3BackendInfo();
bool CheckAndForceDirectX11Renderer();
bool ForceDirectX11Renderer();

} // namespace SDL3Integration
} // namespace FM2K