#include "window_hooks.h"
#include "sdl3_context.h"
#include <MinHook.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

namespace FM2K {
namespace WindowHooks {

// Original CreateWindowExA function pointer
static HWND (WINAPI *original_CreateWindowExA)(
    DWORD dwExStyle,
    LPCSTR lpClassName,
    LPCSTR lpWindowName,
    DWORD dwStyle,
    int X,
    int Y,
    int nWidth,
    int nHeight,
    HWND hWndParent,
    HMENU hMenu,
    HINSTANCE hInstance,
    LPVOID lpParam
) = nullptr;

HWND WINAPI Hook_CreateWindowExA(
    DWORD dwExStyle,
    LPCSTR lpClassName,
    LPCSTR lpWindowName,
    DWORD dwStyle,
    int X,
    int Y,
    int nWidth,
    int nHeight,
    HWND hWndParent,
    HMENU hMenu,
    HINSTANCE hInstance,
    LPVOID lpParam
) {
    printf("WINDOW HOOK: CreateWindowExA called - Class: %s, Name: %s, Size: %dx%d\n",
           lpClassName ? lpClassName : "NULL",
           lpWindowName ? lpWindowName : "NULL",
           nWidth, nHeight);
    
    // Check if this is the main game window
    bool isGameWindow = false;
    if (lpClassName && lpWindowName) {
        // Look for the game's window class name (from IDA analysis)
        if (strstr(lpClassName, "KGT2KGAME") || 
            strstr(lpWindowName, "WonderfulWorld") ||
            strstr(lpWindowName, "Moon Lights")) {
            isGameWindow = true;
            printf("WINDOW HOOK: Detected main game window creation!\n");
        }
    }
    
    if (isGameWindow) {
        // Initialize SDL3 context and create our SDL3 window
        using namespace SDL3Integration;
        
        if (!g_sdlContext.initialized) {
            printf("WINDOW HOOK: Initializing SDL3 context for window hijacking...\n");
            
            // Set window dimensions
            g_sdlContext.windowWidth = nWidth > 0 ? nWidth : 640;
            g_sdlContext.windowHeight = nHeight > 0 ? nHeight : 480;
            
            if (!InitializeSDL3Context(0, nullptr)) {
                printf("WINDOW HOOK: Failed to initialize SDL3 context\n");
                // Fall back to original window creation
                return original_CreateWindowExA(dwExStyle, lpClassName, lpWindowName,
                                              dwStyle, X, Y, nWidth, nHeight,
                                              hWndParent, hMenu, hInstance, lpParam);
            }
            
            // Get the native HWND from the SDL3 window
            HWND sdl_hwnd = (HWND)SDL_GetPointerProperty(
                SDL_GetWindowProperties(g_sdlContext.window),
                SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                NULL
            );
            
            if (sdl_hwnd) {
                printf("WINDOW HOOK: Successfully hijacked window creation with SDL3 HWND: 0x%p\n", sdl_hwnd);
                
                // Update game's global window handle (g_hwnd_parent @ 0x4246f8 from IDA)
                HWND* game_hwnd_ptr = (HWND*)0x4246f8;
                if (game_hwnd_ptr) {
                    *game_hwnd_ptr = sdl_hwnd;
                    printf("WINDOW HOOK: Updated game's global window handle to SDL3 window\n");
                }
                
                // Subclass the SDL3 window to forward messages to game logic
                SubclassSDL3Window(sdl_hwnd);
                
                // Return the SDL3 window handle instead of creating a new window
                return sdl_hwnd;
            } else {
                printf("WINDOW HOOK: Failed to get SDL3 window HWND\n");
            }
        } else {
            // SDL3 context already initialized, return existing window
            HWND sdl_hwnd = (HWND)SDL_GetPointerProperty(
                SDL_GetWindowProperties(g_sdlContext.window),
                SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                NULL
            );
            
            if (sdl_hwnd) {
                printf("WINDOW HOOK: Returning existing SDL3 window HWND: 0x%p\n", sdl_hwnd);
                return sdl_hwnd;
            }
        }
    }
    
    // For non-game windows or if SDL3 setup failed, use original function
    printf("WINDOW HOOK: Using original CreateWindowExA for non-game window\n");
    return original_CreateWindowExA(dwExStyle, lpClassName, lpWindowName,
                                   dwStyle, X, Y, nWidth, nHeight,
                                   hWndParent, hMenu, hInstance, lpParam);
}

bool InitializeWindowHooks() {
    printf("WINDOW HOOK: Initializing window hooks...\n");
    
    // Hook CreateWindowExA to intercept window creation
    MH_STATUS status = MH_CreateHookApi(
        L"user32.dll",
        "CreateWindowExA",
        reinterpret_cast<LPVOID>(Hook_CreateWindowExA),
        reinterpret_cast<LPVOID*>(&original_CreateWindowExA)
    );
    
    if (status != MH_OK) {
        printf("WINDOW HOOK: Failed to create CreateWindowExA hook: %d\n", status);
        return false;
    }
    
    printf("WINDOW HOOK: Window hooks initialized successfully\n");
    return true;
}

void ShutdownWindowHooks() {
    printf("WINDOW HOOK: Shutting down window hooks...\n");
    
    // Hooks will be disabled/removed by main MH_Uninitialize() call
    
    printf("WINDOW HOOK: Window hooks shutdown complete\n");
}

} // namespace WindowHooks
} // namespace FM2K