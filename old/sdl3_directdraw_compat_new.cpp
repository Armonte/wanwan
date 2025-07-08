#include "../../argentum.hpp"
#include "../hooks.hpp"
#include "sdl3_context.hpp"
#include "surface_management.hpp"
#include "palette_system.hpp"
#include "palette_debug.hpp"
#include <SDL3/SDL.h>
#include <windows.h>
#include <MinHook.h>
#include <cstring>

// Forward declarations for dual rendering functions
namespace argentum::hooks {
    void RenderImGuiSDL3();
}

namespace argentum::hooks {
    
    // Hook implementation for initDirectDraw
    int __cdecl initDirectDraw_new(int isFullScreen, void* windowHandle) {
        printf("HOOK DEBUG: initDirectDraw_new called! isFullScreen=%d, windowHandle=%p\n", isFullScreen, windowHandle);
        
        if (g_sdlContext.initialized) {
            printf("HOOK DEBUG: SDL3 context already initialized, returning success\n");
            return 1;
        }
        
        if (!InitializeSDL3Context(isFullScreen, windowHandle)) {
            return 0;
        }
        
        InitializeSurfacePointers();
        
        if (!CreateSDLTextures()) {
            return 0;
        }
        
        if (!CreateSDL3PaletteSystem()) {
            return 0;
        }
        
        // Set up DirectDraw global variables
        void** pThis = (void**)0x439848;
        *pThis = &g_dummyDirectDraw;
        
        void** pGraphicsInterface = (void**)0x43984C;
        *pGraphicsInterface = &g_primarySurface;
        
        void** pspriteSurface = (void**)0x439850;
        *pspriteSurface = &g_spriteSurface;
        
        void** pBackBuffer = (void**)0x439854;
        *pBackBuffer = &g_backSurface;
        
        void** pGraphicsManager = (void**)0x439858;
        *pGraphicsManager = &g_graphicsSurface;
        
        static int dummyClipper = 0x12345678;
        void** pClipper = (void**)0x43985C;
        *pClipper = &dummyClipper;
        
        void** pBitDepth = (void**)0x4C0788;
        void* pScreenBuffer = (void*)0x4C1560;
        *pBitDepth = pScreenBuffer;
        
        int* pMaxWidth = (int*)0x6B3060;
        int* pMaxHeight = (int*)0x6B305C;
        int* pBitCount = (int*)0x6B3058;
        *pMaxWidth = 256;
        *pMaxHeight = 240;
        *pBitCount = 8;
        
        return 1;
    }
    
    // initializeResourceHandlers replacement
    HRESULT __cdecl initializeResourceHandlers_new() {
        printf("HOOK DEBUG: initializeResourceHandlers_new called!\n");
        
        int* pResourceHandlerState = (int*)0x439860;
        if (*pResourceHandlerState != 3) {
            return -1;
        }
        return S_OK;
    }
    
    // Main rendering loop - this is where the color issues need to be debugged
    int __cdecl ProcessScreenUpdatesAndResources_new() {
        static int callCount = 0;
        callCount++;
        // DISABLED for performance: HOOK DEBUG message causes FPS drops
        // if (callCount <= 3 || callCount % 300 == 0) {  // Log first 3 calls and every 300 calls after (every 5 seconds)
        //     printf("HOOK DEBUG: ProcessScreenUpdatesAndResources_new called! (call #%d)\n", callCount);
        // }
        
        // Update SDL events once per frame (essential for input and timing)
        UpdateSDL3Events();
        
        // Frame timing with high precision (uses VSync instead of Sleep)
        // VSync is now enabled on the renderer, so we don't need manual frame limiting
        static LARGE_INTEGER frequency = {0};
        static LARGE_INTEGER lastFrameTime = {0};
        
        if (frequency.QuadPart == 0) {
            QueryPerformanceFrequency(&frequency);
        }
        
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        
        // Calculate frame time in microseconds for precision
        double deltaTimeUs = 0;
        if (lastFrameTime.QuadPart != 0) {
            deltaTimeUs = ((double)(currentTime.QuadPart - lastFrameTime.QuadPart) * 1000000.0) / frequency.QuadPart;
        }
        lastFrameTime = currentTime;
        
        // Debug: Log frame timing issues with high precision
        static int frameCount = 0;
        frameCount++;
        // DISABLED for performance: Frame timing debug causes FPS drops
        // if (frameCount % 300 == 0 && deltaTimeUs > 25000) {  // Log if frame took longer than 25ms, every 5 seconds max
        //     printf("Frame timing: Frame %d took %.2fms (expected 16.67ms)\n", frameCount, deltaTimeUs / 1000.0);
        // }
        
        // Variable mappings
        int* pResourceHandlerState = (int*)0x439860;
        int* pResourceInitCounter = (int*)0x43F1BC;
        int* pGameScreenBuffer = (int*)0x4C1560;
        int* pIsFullscreenMode = (int*)0x4C156C;
        int* pScreenResolutionFlag = (int*)0x4C1570;
        HWND* pHWndParent = (HWND*)0x4C1574;
        void** ppBitDepth = (void**)0x4C0788;
        
        if (!pResourceHandlerState || !pResourceInitCounter || !pGameScreenBuffer || 
            !pIsFullscreenMode || !pScreenResolutionFlag || !ppBitDepth) {
            return 0;
        }
        
        if (!g_sdlContext.initialized || !g_sdlContext.renderer) {
            return 0;
        }
        
        if (!g_spriteTexture || !g_primaryTexture || !g_backTexture || !g_graphicsTexture) {
            if (!CreateSDLTextures()) {
                return 0;
            }
        }
        
        if (!g_indexedSurface || !g_sdlPalette) {
            if (!CreateSDL3PaletteSystem()) {
                return 0;
            }
        }
        
        // Palette hotkeys removed - no longer needed
        
        // Debug: Print SDL3 backend info once
        static bool backendInfoPrinted = false;
        if (!backendInfoPrinted) {
            PrintSDL3BackendInfo();
            CheckAndForceDirectX11Renderer();  // Check renderer backend on first run
            backendInfoPrinted = true;
        }
        
        // Handle palette updates - completely handled by UpdateColorInformation_new hook
        // Don't call any palette functions here to avoid recursion
        
        // Handle Alt+Enter fullscreen toggle
        static bool altEnterPressed = false;
        bool currentAltEnter = IsAltEnterPressed();
        if (currentAltEnter && !altEnterPressed) {
            // Alt+Enter just pressed (edge detection)
            printf("SDL3 FULLSCREEN: Alt+Enter detected - toggling fullscreen mode\n");
            ToggleFullscreen();
        }
        altEnterPressed = currentAltEnter;
        
        // Main rendering logic
        if (*pResourceHandlerState != 2) {
            if (*pResourceInitCounter < 2) {
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
            unsigned char* srcData = (unsigned char*)*ppBitDepth;
            if (!srcData || IsBadReadPtr(srcData, 256 * 240)) {
                srcData = (unsigned char*)pGameScreenBuffer;
            }
            
            // Get palette data for conversion
            typedef void* (__cdecl *GetPaletteEntry_func)();
            GetPaletteEntry_func GetPaletteEntry = (GetPaletteEntry_func)0x0042BBF0;
            void* paletteData = GetPaletteEntry();
            
            // Use SDL3 native palette approach for better color accuracy
            if (SDL_LockSurface(g_indexedSurface)) {
                unsigned char* indexedPixels = (unsigned char*)g_indexedSurface->pixels;
                int indexedPitch = g_indexedSurface->pitch;
                
                // Direct copy of palette indices - SDL3 handles conversion
                for (int y = 0; y < 240; y++) {
                    if (y * indexedPitch + 256 <= indexedPitch * 240) {
                        memcpy(indexedPixels + y * indexedPitch, srcData + y * 256, 256);
                    }
                }
                
                SDL_UnlockSurface(g_indexedSurface);
                
                // Recreate texture from indexed surface with updated palette
                if (g_paletteTexture) {
                    SDL_DestroyTexture(g_paletteTexture);
                }
                g_paletteTexture = SDL_CreateTextureFromSurface(g_sdlContext.renderer, g_indexedSurface);
                
                if (g_paletteTexture) {
                    SDL_SetTextureScaleMode(g_paletteTexture, SDL_SCALEMODE_NEAREST);
                    SDL_UnlockTexture(g_spriteTexture);
                    goto use_palette_texture;
                } 
            }
            
            SDL_UnlockTexture(g_spriteTexture);
            
use_palette_texture:
            // Render to game buffer (not directly to window)
            // This allows the dual rendering system to scale the game properly
            SDL_SetRenderTarget(g_sdlContext.renderer, g_sdlContext.gameBuffer);
            SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 0, 255);
            SDL_RenderClear(g_sdlContext.renderer);
            
            SDL_Texture* renderTexture = g_paletteTexture ? g_paletteTexture : g_spriteTexture;
            
            // Render to the full game buffer (256x240)
            SDL_FRect destRect;
            destRect.x = 0;    
            destRect.y = 0;    
            destRect.w = 256;  // Actual game content width
            destRect.h = 240;  // Actual game content height
            
            SDL_RenderTexture(g_sdlContext.renderer, renderTexture, NULL, &destRect);
            
        /*     // Debug: Only log occasionally to avoid spam
            static int renderCount1 = 0;
            renderCount1++;
            if (renderCount1 % 120 == 0) {  // Every 2 seconds at 60fps
                printf("DirectDraw: Rendered game content to game buffer (256x240) [Frame %d]\n", renderCount1);
            } */
            
        } else {
            // Direct mode rendering
            void* pixels;
            int pitch;
            if (SDL_LockTexture(g_primaryTexture, NULL, &pixels, &pitch) < 0) {
                return 0;
            }
            
            unsigned char* srcData = (unsigned char*)*ppBitDepth;
            unsigned char* dstData = (unsigned char*)pixels;
            
            if (!srcData || IsBadReadPtr(srcData, 256 * 256)) {
                srcData = (unsigned char*)pGameScreenBuffer;
            }
            
            // Get palette for color conversion
            typedef void* (__cdecl *GetPaletteEntry_func)();
            GetPaletteEntry_func GetPaletteEntry = (GetPaletteEntry_func)0x0042BBF0;
            void* paletteData = GetPaletteEntry();
            
            // No manual screen offset - let SDL3 logical presentation handle positioning
            unsigned char* destBuffer = dstData;
            
            // Convert palette indices to RGB using debug functions
            for (int y = 0; y < 256; y++) {
                for (int x = 0; x < 256; x++) {
                    unsigned char paletteIndex = srcData[y * 256 + x];
                    unsigned char r, g, b, a = 255;
                    
                    if (paletteData) {
                        // Use debug conversion with format detection
                        ConvertPaletteEntryToRGB(paletteData, paletteIndex, &r, &g, &b);
                    } else {
                        r = g = b = paletteIndex;
                    }
                    
                    int dstOffset = x * 4;
                    if (dstOffset + 3 < pitch) {
                        destBuffer[dstOffset + 0] = r;
                        destBuffer[dstOffset + 1] = g;
                        destBuffer[dstOffset + 2] = b;
                        destBuffer[dstOffset + 3] = a;
                    }
                }
                destBuffer += pitch;
            }
            
            SDL_UnlockTexture(g_primaryTexture);
            
            // Render to game buffer (not directly to window)
            SDL_SetRenderTarget(g_sdlContext.renderer, g_sdlContext.gameBuffer);
            SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 0, 255);
            SDL_RenderClear(g_sdlContext.renderer);
            
            // Render primary texture to fill game buffer
            SDL_FRect destRect;
            destRect.x = 0;    
            destRect.y = 0;    
            destRect.w = 256;  // Actual game content width
            destRect.h = 240;  // Actual game content height
            
            SDL_RenderTexture(g_sdlContext.renderer, g_primaryTexture, NULL, &destRect);
            
            // Debug: Only log occasionally to avoid spam
            static int renderCount2 = 0;
            renderCount2++;
            if (renderCount2 % 120 == 0) {  // Every 2 seconds at 60fps
                printf("DirectDraw: Rendered direct mode content to game buffer (256x240) [Frame %d]\n", renderCount2);
            }
        }
        
        // IMPORTANT: The DirectDraw compatibility layer IS the main rendering loop
        // We need to call our dual rendering system here to display everything
        
        // Step 1: RenderGameToWindow() - scales our game buffer to fill window
        RenderGameToWindow();
        
        // Step 2: RenderImGuiSDL3() - renders ImGui overlays on top
        RenderImGuiSDL3();
        
        // Step 3: PresentFrame() - presents the final frame
        PresentFrame();
        
        /* // Debug: Only log occasionally to avoid spam
        static int finalCount = 0;
        finalCount++;
        if (finalCount % 300 == 0) {  // Every 5 seconds at 60fps
            printf("DirectDraw: Completed dual rendering cycle [Frame %d]\n", finalCount);
        } */
        
        return 0;
    }
    
    // Missing helper functions from original file
    void graphics_copy_game_data_to_sdl3(int* pScreenData) {
        if (!pScreenData || !g_spriteTexture) {
            return;
        }
        
        void* pixels;
        int pitch;
        if (SDL_LockTexture(g_spriteTexture, NULL, &pixels, &pitch) < 0) {
            return;
        }
        
        unsigned char* srcData = (unsigned char*)pScreenData;
        unsigned char* dstData = (unsigned char*)pixels;
        
        // Convert from 8-bit palettized to RGBA8888
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < 256; x++) {
                unsigned char paletteIndex = srcData[y * 256 + x];
                
                unsigned char r, g, b, a = 255;
                
                if (paletteIndex == 0) {
                    r = g = b = 0;
                } else {
                    r = paletteIndex;
                    g = paletteIndex / 2;
                    b = 255 - paletteIndex;
                }
                
                int dstOffset = y * pitch + x * 4;
                if (dstOffset + 3 < pitch * 240) {
                    dstData[dstOffset + 0] = r;
                    dstData[dstOffset + 1] = g;
                    dstData[dstOffset + 2] = b;
                    dstData[dstOffset + 3] = a;
                }
            }
        }
        
        SDL_UnlockTexture(g_spriteTexture);
    }
    
    // Cleanup function
    void CleanupSDL3DirectDrawCompat() {
        CleanupImGuiSDL3();
        CleanupSDL3PaletteSystem();
        CleanupSDLTextures();
        CleanupSDL3Context();
    }
    
} // namespace argentum::hooks 