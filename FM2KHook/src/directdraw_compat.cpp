#include "directdraw_compat.h"
#include "sdl3_context.h"
#include "fm2k_hook.h"
#include <MinHook.h>
#include <cstring>
#include <cstdio>

namespace FM2K {
namespace DirectDrawCompat {

// Global dummy objects
DummyDirectDraw g_dummyDirectDraw = {};
DummySurface g_primarySurface = {};
DummySurface g_backSurface = {};
DummySurface g_spriteSurface = {};
DummySurface g_graphicsSurface = {};

// Global textures
SDL_Texture* g_primaryTexture = nullptr;
SDL_Texture* g_backTexture = nullptr;
SDL_Texture* g_spriteTexture = nullptr;
SDL_Texture* g_graphicsTexture = nullptr;

void InitializeSurfacePointers() {
    // Initialize dummy surface structures
    g_primarySurface.width = 640;
    g_primarySurface.height = 480;
    g_primarySurface.texture = g_primaryTexture;
    
    g_backSurface.width = 256;
    g_backSurface.height = 240;
    g_backSurface.texture = g_backTexture;
    
    g_spriteSurface.width = 256;
    g_spriteSurface.height = 256;
    g_spriteSurface.texture = g_spriteTexture;
    
    g_graphicsSurface.width = 256;
    g_graphicsSurface.height = 240;
    g_graphicsSurface.texture = g_graphicsTexture;
}

bool CreateSDLTextures() {
    using namespace SDL3Integration;
    
    if (!g_sdlContext.initialized || !g_sdlContext.renderer) {
        return false;
    }
    
    // Create primary texture (screen buffer)
    g_primaryTexture = SDL_CreateTexture(
        g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET,
        640,
        480
    );
    
    // Create back buffer texture  
    g_backTexture = SDL_CreateTexture(
        g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        256,
        240
    );
    
    // Create sprite texture
    g_spriteTexture = SDL_CreateTexture(
        g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        256,
        256
    );
    
    // Create graphics texture
    g_graphicsTexture = SDL_CreateTexture(
        g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        256,
        240
    );
    
    if (!g_primaryTexture || !g_backTexture || !g_spriteTexture || !g_graphicsTexture) {
        printf("SDL3 TEXTURES: Failed to create textures: %s\n", SDL_GetError());
        return false;
    }
    
    // Set nearest neighbor filtering for pixel art
    SDL_SetTextureScaleMode(g_backTexture, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureScaleMode(g_spriteTexture, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureScaleMode(g_graphicsTexture, SDL_SCALEMODE_NEAREST);
    
    printf("SDL3 TEXTURES: Created all DirectDraw replacement textures\n");
    return true;
}

void UpdateMemoryPointers() {
    // Update FM2K memory addresses to point to our dummy structures
    // These addresses are from the IDA Pro analysis of WonderfulWorld
    
    // DirectDraw interface pointer
    void** pDirectDraw = (void**)0x439848;  // Needs to be verified for this binary
    if (pDirectDraw) {
        *pDirectDraw = &g_dummyDirectDraw;
    }
    
    // Surface pointers - these may need adjustment for WonderfulWorld
    void** pPrimarySurface = (void**)0x43984C;
    if (pPrimarySurface) {
        *pPrimarySurface = &g_primarySurface;
    }
    
    void** pSpriteSurface = (void**)0x439850;
    if (pSpriteSurface) {
        *pSpriteSurface = &g_spriteSurface;
    }
    
    void** pBackBuffer = (void**)0x439854;
    if (pBackBuffer) {
        *pBackBuffer = &g_backSurface;
    }
    
    void** pGraphicsManager = (void**)0x439858;
    if (pGraphicsManager) {
        *pGraphicsManager = &g_graphicsSurface;
    }
    
    // Bit depth and resolution settings
    void** pBitDepth = (void**)0x4C0788;
    void* pScreenBuffer = (void*)0x4C1560;
    if (pBitDepth) {
        *pBitDepth = pScreenBuffer;
    }
    
    // Resolution settings - set to native game resolution
    int* pMaxWidth = (int*)0x6B3060;
    int* pMaxHeight = (int*)0x6B305C;
    int* pBitCount = (int*)0x6B3058;
    
    if (pMaxWidth) *pMaxWidth = 256;
    if (pMaxHeight) *pMaxHeight = 240;
    if (pBitCount) *pBitCount = 8;
    
    printf("SDL3 COMPAT: Updated memory pointers for DirectDraw compatibility\n");
}

// Hook implementation for initDirectDraw
int __cdecl initDirectDraw_new(int isFullScreen, void* windowHandle) {
    printf("HOOK DEBUG: initDirectDraw_new called! isFullScreen=%d, windowHandle=%p\n", isFullScreen, windowHandle);
    
    using namespace SDL3Integration;
    
    if (g_sdlContext.initialized) {
        printf("HOOK DEBUG: SDL3 context already initialized, returning success\n");
        return 1;
    }
    
    if (!InitializeSDL3Context(isFullScreen, windowHandle)) {
        return 0;
    }
    
    if (!CreateSDLTextures()) {
        return 0;
    }
    
    if (!CreateSDL3PaletteSystem()) {
        return 0;
    }
    
    InitializeSurfacePointers();
    UpdateMemoryPointers();
    
    return 1;
}

// initializeResourceHandlers replacement
HRESULT __cdecl initializeResourceHandlers_new() {
    printf("HOOK DEBUG: initializeResourceHandlers_new called!\n");
    
    // For compatibility, just return success
    // The original function sets some resource handler state
    int* pResourceHandlerState = (int*)0x439860;
    if (pResourceHandlerState && *pResourceHandlerState != 3) {
        return -1;
    }
    return S_OK;
}

// Main rendering loop replacement
int __cdecl ProcessScreenUpdatesAndResources_new() {
    static int callCount = 0;
    callCount++;
    
    using namespace SDL3Integration;
    
    // Update SDL events once per frame (essential for input and timing)
    UpdateSDL3Events();
    
    // Variable mappings for WonderfulWorld specific addresses
    int* pResourceHandlerState = (int*)0x439860;
    int* pResourceInitCounter = (int*)0x43F1BC;
    int* pGameScreenBuffer = (int*)0x4C1560;
    int* pIsFullscreenMode = (int*)0x4C156C;
    int* pScreenResolutionFlag = (int*)0x4C1570;
    HWND* pHWndParent = (HWND*)0x4C1574;
    void** ppBitDepth = (void**)0x4C0788;
    
    if (!g_sdlContext.initialized || !g_sdlContext.renderer) {
        return 0;
    }
    
    if (!g_spriteTexture || !g_primaryTexture || !g_backTexture || !g_graphicsTexture) {
        if (!CreateSDLTextures()) {
            return 0;
        }
    }
    
    // Handle Alt+Enter fullscreen toggle
    static bool altEnterPressed = false;
    bool currentAltEnter = IsAltEnterPressed();
    if (currentAltEnter && !altEnterPressed) {
        printf("SDL3 FULLSCREEN: Alt+Enter detected - toggling fullscreen mode\n");
        ToggleFullscreen();
    }
    altEnterPressed = currentAltEnter;
    
    // Main rendering logic
    if (pResourceHandlerState && *pResourceHandlerState != 2) {
        if (pResourceInitCounter && *pResourceInitCounter < 2) {
            HRESULT result = initializeResourceHandlers_new();
            if (result != S_OK) {
                return 0;
            }
            (*pResourceInitCounter)++;
        }
        
        // Lock sprite texture for pixel updates
        void* pixels;
        int pitch;
        if (SDL_LockTexture(g_spriteTexture, NULL, &pixels, &pitch) < 0) {
            return 0;
        }
        
        // Get source data from the actual rendering buffer
        unsigned char* srcData = nullptr;
        if (ppBitDepth && *ppBitDepth) {
            srcData = (unsigned char*)*ppBitDepth;
        } else if (pGameScreenBuffer) {
            srcData = (unsigned char*)pGameScreenBuffer;
        }
        
        if (srcData) {
            // Copy game data to sprite texture
            // This is where palette conversion would happen
            // For now, just clear to a test color
            memset(pixels, 0x40, pitch * 240);
        }
        
        SDL_UnlockTexture(g_spriteTexture);
        
        // Render to game buffer (256x240)
        SetGameRenderTarget();
        
        // Render the sprite texture to game buffer
        SDL_FRect destRect = {0, 0, 256, 240};
        SDL_RenderTexture(g_sdlContext.renderer, g_spriteTexture, NULL, &destRect);
    }
    
    // Scale game buffer to window
    RenderGameToWindow();
    
    // Present the final frame
    PresentFrame();
    
    return 0;
}

} // namespace DirectDrawCompat
} // namespace FM2K