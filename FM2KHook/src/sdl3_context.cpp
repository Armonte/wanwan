#include "sdl3_context.h"
#include "fm2k_hook.h"
#include <MinHook.h>
#include <cstdio>
#include <cmath>
#include <cstring>

namespace FM2K {
namespace SDL3Integration {

SDL3Context g_sdlContext = {};

// Window procedure handling
static WNDPROC g_originalSDLWindowProc = nullptr;
static const uintptr_t HANDLE_MENU_CALL_HOTKEYS_ADDR = 0x405f50; // main_window_proc function address

// Game's window procedure function pointer  
typedef LRESULT (__stdcall *HandleMenuCallHotkeysFunc)(HWND, UINT, WPARAM, LPARAM);
static HandleMenuCallHotkeysFunc g_handleMenuCallHotkeys = (HandleMenuCallHotkeysFunc)HANDLE_MENU_CALL_HOTKEYS_ADDR;

// Custom window procedure that forwards messages to the game's window procedure
LRESULT CALLBACK SDL3GameWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Forward ONLY key messages to the game's window procedure
    // Let mouse messages go through SDL3's original window procedure for ImGui
    switch (uMsg) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            // CRITICAL: Let SDL3 process the keyboard event FIRST so SDL_GetKeyboardState() works
            if (g_originalSDLWindowProc) {
                CallWindowProc(g_originalSDLWindowProc, hWnd, uMsg, wParam, lParam);
            }
            
            // THEN forward to the game's window procedure
            return g_handleMenuCallHotkeys(hWnd, uMsg, wParam, lParam);
            
        case WM_CHAR:
        case WM_SYSCHAR:
            // Let SDL3 process character events first
            if (g_originalSDLWindowProc) {
                CallWindowProc(g_originalSDLWindowProc, hWnd, uMsg, wParam, lParam);
            }
            
            // CRITICAL: Pass the SDL3 window handle - the game logic needs the actual visible window
            return g_handleMenuCallHotkeys(hWnd, uMsg, wParam, lParam);
            
        case WM_ACTIVATEAPP:
        case WM_DESTROY:
        case WM_CLOSE:
            // CRITICAL: Pass the SDL3 window handle - the game logic needs the actual visible window  
            return g_handleMenuCallHotkeys(hWnd, uMsg, wParam, lParam);
            
        // Let ALL mouse messages go to SDL3's original window procedure
        // so they can be converted to SDL events for ImGui
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        default:
            // For all other messages, call the original SDL3 window procedure
            if (g_originalSDLWindowProc) {
                return CallWindowProc(g_originalSDLWindowProc, hWnd, uMsg, wParam, lParam);
            }
            return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
}

// SDL3 Event Processing Functions
void UpdateSDL3Events() {
    if (!g_sdlContext.initialized) return;
    
    // Rate limit event pumping to once per frame to avoid jitter
    static DWORD lastPumpTime = 0;
    DWORD currentTime = GetTickCount();
    
    // Only pump events if at least 8ms have passed (120fps max)
    if (currentTime - lastPumpTime < 8) {
        return;
    }
    lastPumpTime = currentTime;
    
    // CRITICAL: Pump SDL events to update input state for keyboard/gamepad
    // This is essential for SDL_GetKeyboardState() and gamepad functions to work
    SDL_PumpEvents();
    
    // Also update gamepad state explicitly
    SDL_UpdateGamepads();
}

bool IsSDL3KeyPressed(int scancode) {
    if (!g_sdlContext.initialized) return false;
    
    // Primary method: Use SDL3 keyboard state
    const bool* keystate = SDL_GetKeyboardState(NULL);
    bool sdl_pressed = keystate && keystate[scancode];
    
    // Fallback method: Use Win32 GetAsyncKeyState for global hotkeys
    bool win32_pressed = false;
    switch (scancode) {
        case SDL_SCANCODE_F1:
            win32_pressed = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
            break;
        case SDL_SCANCODE_F2:
            win32_pressed = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
            break;
        case SDL_SCANCODE_F3:
            win32_pressed = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
            break;
        case SDL_SCANCODE_F4:
            win32_pressed = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
            break;
        case SDL_SCANCODE_RETURN:
            win32_pressed = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
            break;
    }
    
    return sdl_pressed || win32_pressed;
}

// Check if Alt+Enter is pressed for fullscreen toggle
bool IsAltEnterPressed() {
    if (!g_sdlContext.initialized) return false;
    
    const bool* keystate = SDL_GetKeyboardState(NULL);
    bool enter_pressed = keystate && keystate[SDL_SCANCODE_RETURN];
    bool alt_pressed = keystate && (keystate[SDL_SCANCODE_LALT] || keystate[SDL_SCANCODE_RALT]);
    
    // Also check Win32 for global hotkey support
    bool win32_enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    bool win32_alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    
    return (enter_pressed && alt_pressed) || (win32_enter && win32_alt);
}

// Toggle fullscreen mode
bool ToggleFullscreen() {
    if (!g_sdlContext.initialized || !g_sdlContext.window) {
        printf("SDL3 FULLSCREEN: Cannot toggle - context not initialized\n");
        return false;
    }
    
    // Get comprehensive window state
    Uint32 flags = SDL_GetWindowFlags(g_sdlContext.window);
    bool nativeFullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    
    if (nativeFullscreen) {
        // Switch to windowed mode
        SDL_SetWindowFullscreen(g_sdlContext.window, false);
        SDL_SetWindowBordered(g_sdlContext.window, true);
        SDL_SetWindowSize(g_sdlContext.window, 640, 480);
        SDL_SetWindowPosition(g_sdlContext.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_RestoreWindow(g_sdlContext.window);
        SDL_RaiseWindow(g_sdlContext.window);
        
        g_sdlContext.isFullscreen = false;
        g_sdlContext.windowWidth = 640;
        g_sdlContext.windowHeight = 480;
        
        printf("SDL3 FULLSCREEN: Switched to windowed mode (640x480)\n");
    } else {
        // Switch to fullscreen mode
        SDL_DisplayID displayID = SDL_GetDisplayForWindow(g_sdlContext.window);
        const SDL_DisplayMode* displayMode = SDL_GetCurrentDisplayMode(displayID);
        
        if (displayMode) {
            SDL_SetWindowFullscreen(g_sdlContext.window, true);
            g_sdlContext.windowWidth = displayMode->w;
            g_sdlContext.windowHeight = displayMode->h;
            g_sdlContext.isFullscreen = true;
            
            printf("SDL3 FULLSCREEN: Switched to fullscreen mode (%dx%d)\n", 
                   g_sdlContext.windowWidth, g_sdlContext.windowHeight);
        }
    }
    
    return true;
}

bool InitializeSDL3Context(int isFullScreen, void* hwnd) {
    if (g_sdlContext.initialized) {
        return true;
    }
    
    // Initialize SDL3 with video, events, and gamepad support
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD) < 0) {
        printf("SDL3 ERROR: Failed to initialize SDL3: %s\n", SDL_GetError());
        return false;
    }
    
    g_sdlContext.isFullscreen = isFullScreen;
    
    // Set native game resolution for dual rendering
    g_sdlContext.gameWidth = 256;   // Actual game rendering width  
    g_sdlContext.gameHeight = 240;  // Actual game rendering height
    
    // WINDOWED MODE BY DEFAULT: Always start in windowed mode
    if (g_sdlContext.windowWidth == 0 || g_sdlContext.windowHeight == 0) {
        g_sdlContext.windowWidth = 640;   // Default windowed size
        g_sdlContext.windowHeight = 480;
    }
    
    // Always create windowed window initially (Alt+Enter enables fullscreen)
    g_sdlContext.window = SDL_CreateWindow(
        "WonderfulWorld ver 0946",
        g_sdlContext.windowWidth, 
        g_sdlContext.windowHeight,
        SDL_WINDOW_RESIZABLE  // CRITICAL: Must be resizable for fullscreen toggle
    );
    
    if (!g_sdlContext.window) {
        printf("SDL3 ERROR: Failed to create window: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    
    // Create renderer - FORCE DirectX 11 specifically
    SDL_PropertiesID rendererProps = SDL_CreateProperties();
    SDL_SetStringProperty(rendererProps, SDL_PROP_RENDERER_CREATE_NAME_STRING, "direct3d11");
    SDL_SetNumberProperty(rendererProps, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, 1);
    printf("SDL3 RENDERER: Forcing DirectX 11 renderer\n");
    g_sdlContext.renderer = SDL_CreateRendererWithProperties(rendererProps);
    SDL_DestroyProperties(rendererProps);
    
    if (!g_sdlContext.renderer) {
        printf("SDL3 ERROR: Failed to create DirectX 11 renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_sdlContext.window);
        SDL_Quit();
        return false;
    }
    
    // Create game buffer texture for scaled rendering (native game resolution)
    g_sdlContext.gameBuffer = SDL_CreateTexture(
        g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET,
        g_sdlContext.gameWidth,
        g_sdlContext.gameHeight
    );
    
    if (!g_sdlContext.gameBuffer) {
        SDL_DestroyRenderer(g_sdlContext.renderer);
        SDL_DestroyWindow(g_sdlContext.window);
        SDL_Quit();
        return false;
    }
    
    // CRITICAL: Set nearest neighbor filtering for crisp pixel art
    SDL_SetTextureScaleMode(g_sdlContext.gameBuffer, SDL_SCALEMODE_NEAREST);
    
    printf("SDL3 DUAL RENDERING: Game buffer created at %dx%d with NEAREST NEIGHBOR filtering\n",
           g_sdlContext.gameWidth, g_sdlContext.gameHeight);
    
    g_sdlContext.initialized = true;
    
    printf("SDL3 CONTROLS: Press Alt+Enter to toggle between windowed and fullscreen mode\n");
    
    return true;
}

void CleanupSDL3Context() {
    if (!g_sdlContext.initialized) return;
    
    if (g_sdlContext.gameBuffer) {
        SDL_DestroyTexture(g_sdlContext.gameBuffer);
        g_sdlContext.gameBuffer = nullptr;
    }
    
    if (g_sdlContext.backBuffer) {
        SDL_DestroyTexture(g_sdlContext.backBuffer);
        g_sdlContext.backBuffer = nullptr;
    }
    
    if (g_sdlContext.spriteBuffer) {
        SDL_DestroyTexture(g_sdlContext.spriteBuffer);
        g_sdlContext.spriteBuffer = nullptr;
    }
    
    if (g_sdlContext.renderer) {
        SDL_DestroyRenderer(g_sdlContext.renderer);
        g_sdlContext.renderer = nullptr;
    }
    
    if (g_sdlContext.window) {
        SDL_DestroyWindow(g_sdlContext.window);
        g_sdlContext.window = nullptr;
    }
    
    SDL_Quit();
    g_sdlContext.initialized = false;
}

// Subclass the SDL3 window to forward messages to the game's window procedure
void SubclassSDL3Window(HWND hwnd) {
    static HWND already_subclassed_window = nullptr;
    
    if (!hwnd) {
        printf("SDL3 SUBCLASS: Invalid HWND provided\n");
        return;
    }
    
    // Prevent double subclassing of the same window
    if (already_subclassed_window == hwnd) {
        printf("SDL3 SUBCLASS: Window 0x%p already subclassed - skipping\n", hwnd);
        return;
    }
    
    // Store the original SDL3 window procedure
    g_originalSDLWindowProc = (WNDPROC)GetWindowLongPtr(hwnd, GWLP_WNDPROC);
    if (!g_originalSDLWindowProc) {
        printf("SDL3 SUBCLASS: Failed to get original window procedure (error %d)\n", GetLastError());
        return;
    }
    
    // Set our custom window procedure
    WNDPROC prevProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)SDL3GameWindowProc);
    if (!prevProc) {
        printf("SDL3 SUBCLASS: Failed to set custom window procedure (error %d)\n", GetLastError());
        g_originalSDLWindowProc = nullptr;
        return;
    }
    
    already_subclassed_window = hwnd;
    printf("SDL3 SUBCLASS: Successfully subclassed SDL3 window (HWND=0x%p)\n", hwnd);
}

// Restore the original SDL3 window procedure
void UnsubclassSDL3Window(HWND hwnd) {
    if (!hwnd || !g_originalSDLWindowProc) {
        return;
    }
    
    SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)g_originalSDLWindowProc);
    g_originalSDLWindowProc = nullptr;
    printf("SDL3 SUBCLASS: Restored original window procedure\n");
}

// Dual rendering functions for scaled game + unscaled ImGui
void SetGameRenderTarget() {
    if (!g_sdlContext.initialized || !g_sdlContext.gameBuffer) return;
    
    SDL_SetRenderTarget(g_sdlContext.renderer, g_sdlContext.gameBuffer);
    
    // Clear game buffer to black
    SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdlContext.renderer);
}

void SetWindowRenderTarget() {
    if (!g_sdlContext.initialized) return;
    
    // Set render target back to window (null = window)
    SDL_SetRenderTarget(g_sdlContext.renderer, nullptr);
}

void RenderGameToWindow() {
    if (!g_sdlContext.initialized || !g_sdlContext.gameBuffer) return;
    
    // Set render target to window
    SDL_SetRenderTarget(g_sdlContext.renderer, nullptr);
    
    // Clear window to black
    SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdlContext.renderer);
    
    // Get actual window size (for fullscreen, this should be screen resolution)
    int actualWindowWidth, actualWindowHeight;
    SDL_GetWindowSize(g_sdlContext.window, &actualWindowWidth, &actualWindowHeight);
    
    // Calculate scaling to maintain aspect ratio and center the game
    float windowAspect = (float)actualWindowWidth / actualWindowHeight;
    float gameAspect = (float)g_sdlContext.gameWidth / g_sdlContext.gameHeight;  // 256/240 = 1.067
    
    SDL_FRect destRect;
    
    if (windowAspect > gameAspect) {
        // Window is wider than game aspect - letterbox on sides
        float scale = (float)actualWindowHeight / g_sdlContext.gameHeight;
        destRect.w = g_sdlContext.gameWidth * scale;
        destRect.h = (float)actualWindowHeight;
        destRect.x = (actualWindowWidth - destRect.w) / 2;
        destRect.y = 0;
    } else {
        // Window is taller than game aspect - letterbox on top/bottom
        float scale = (float)actualWindowWidth / g_sdlContext.gameWidth;
        destRect.w = (float)actualWindowWidth;
        destRect.h = g_sdlContext.gameHeight * scale;
        destRect.x = 0;
        destRect.y = (actualWindowHeight - destRect.h) / 2;
    }
    
    // Render the scaled game buffer to the window
    SDL_RenderTexture(g_sdlContext.renderer, g_sdlContext.gameBuffer, nullptr, &destRect);
}

void PresentFrame() {
    if (!g_sdlContext.initialized) return;
    
    // Present the final frame to the screen
    SDL_RenderPresent(g_sdlContext.renderer);
}

bool CreateSDLTextures() {
    if (!g_sdlContext.initialized) return false;
    
    // Create sprite buffer texture (this replaces the 256x256 DirectDraw surface)
    g_sdlContext.spriteBuffer = SDL_CreateTexture(
        g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET,
        256,
        256
    );
    
    if (!g_sdlContext.spriteBuffer) {
        return false;
    }
    
    // Set nearest neighbor filtering for sprite buffer
    SDL_SetTextureScaleMode(g_sdlContext.spriteBuffer, SDL_SCALEMODE_NEAREST);
    
    return true;
}

bool CreateSDL3PaletteSystem() {
    // Create 8-bit indexed surface for palette conversion
    g_sdlContext.indexedSurface = SDL_CreateSurface(256, 240, SDL_PIXELFORMAT_INDEX8);
    if (!g_sdlContext.indexedSurface) {
        printf("SDL3 PALETTE: Failed to create indexed surface: %s\n", SDL_GetError());
        return false;
    }
    
    // Create palette with 256 colors
    g_sdlContext.sdlPalette = SDL_CreatePalette(256);
    if (!g_sdlContext.sdlPalette) {
        printf("SDL3 PALETTE: Failed to create palette: %s\n", SDL_GetError());
        SDL_DestroySurface(g_sdlContext.indexedSurface);
        g_sdlContext.indexedSurface = nullptr;
        return false;
    }
    
    // Set default palette (will be updated by game)
    SDL_Color defaultColors[256];
    for (int i = 0; i < 256; i++) {
        defaultColors[i].r = i;
        defaultColors[i].g = i;
        defaultColors[i].b = i;
        defaultColors[i].a = 255;
    }
    
    if (!SDL_SetPaletteColors(g_sdlContext.sdlPalette, defaultColors, 0, 256)) {
        printf("SDL3 PALETTE: Failed to set palette colors: %s\n", SDL_GetError());
        SDL_DestroyPalette(g_sdlContext.sdlPalette);
        SDL_DestroySurface(g_sdlContext.indexedSurface);
        g_sdlContext.sdlPalette = nullptr;
        g_sdlContext.indexedSurface = nullptr;
        return false;
    }
    
    // Assign palette to surface
    if (!SDL_SetSurfacePalette(g_sdlContext.indexedSurface, g_sdlContext.sdlPalette)) {
        printf("SDL3 PALETTE: Failed to assign palette to surface: %s\n", SDL_GetError());
        SDL_DestroyPalette(g_sdlContext.sdlPalette);
        SDL_DestroySurface(g_sdlContext.indexedSurface);
        g_sdlContext.sdlPalette = nullptr;
        g_sdlContext.indexedSurface = nullptr;
        return false;
    }
    
    printf("SDL3 PALETTE: Palette system initialized successfully\n");
    return true;
}

void PrintSDL3BackendInfo() {
    if (!g_sdlContext.initialized) {
        printf("SDL3 BACKEND: Not initialized\n");
        return;
    }
    
    printf("=== SDL3 BACKEND INFO ===\n");
    
    // Video driver info
    const char* videoDriver = SDL_GetCurrentVideoDriver();
    printf("Active Video Driver: %s\n", videoDriver ? videoDriver : "Unknown");
    
    // Renderer info
    if (g_sdlContext.renderer) {
        SDL_PropertiesID rendererProps = SDL_GetRendererProperties(g_sdlContext.renderer);
        if (rendererProps) {
            const char* rendererName = SDL_GetStringProperty(rendererProps, SDL_PROP_RENDERER_NAME_STRING, "Unknown");
            printf("Active Renderer: %s\n", rendererName);
            
            // Backend detection
            if (SDL_HasProperty(rendererProps, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER)) {
                printf("Backend: Direct3D 11\n");
            } else if (SDL_HasProperty(rendererProps, SDL_PROP_RENDERER_D3D12_DEVICE_POINTER)) {
                printf("Backend: Direct3D 12\n");
            } else if (SDL_HasProperty(rendererProps, SDL_PROP_RENDERER_VULKAN_INSTANCE_POINTER)) {
                printf("Backend: Vulkan\n");
            } else {
                printf("Backend: %s (OpenGL/Software/Other)\n", rendererName);
            }
        }
    } else {
        printf("Renderer: Not created\n");
    }
    
    printf("Window: %dx%d %s\n", 
           g_sdlContext.windowWidth, g_sdlContext.windowHeight,
           g_sdlContext.isFullscreen ? "Fullscreen" : "Windowed");
    
    printf("========================\n");
}

bool CheckAndForceDirectX11Renderer() {
    if (!g_sdlContext.initialized || !g_sdlContext.renderer) {
        printf("SDL3 RENDERER CHECK: Context not initialized\n");
        return false;
    }
    
    SDL_PropertiesID rendererProps = SDL_GetRendererProperties(g_sdlContext.renderer);
    if (!rendererProps) {
        printf("SDL3 RENDERER CHECK: Could not get renderer properties\n");
        return false;
    }
    
    const char* rendererName = SDL_GetStringProperty(rendererProps, SDL_PROP_RENDERER_NAME_STRING, "Unknown");
    
    // Check backend type
    bool isDX11 = SDL_HasProperty(rendererProps, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER);
    
    if (isDX11) {
        printf("SDL3 RENDERER CHECK: Already using DirectX 11\n");
        return true;
    } else {
        printf("SDL3 RENDERER CHECK: NOT using DirectX 11! Current: %s\n", rendererName);
        return false;
    }
}

bool ForceDirectX11Renderer() {
    if (!g_sdlContext.window) {
        printf("SDL3 RENDERER FORCE: No window available\n");
        return false;
    }
    
    printf("SDL3 RENDERER FORCE: Attempting to force DirectX 11 renderer...\n");
    
    // Destroy current renderer if it exists
    if (g_sdlContext.renderer) {
        printf("SDL3 RENDERER FORCE: Destroying current renderer\n");
        SDL_DestroyRenderer(g_sdlContext.renderer);
        g_sdlContext.renderer = nullptr;
    }
    
    // Create properties for DirectX 11
    SDL_PropertiesID rendererProps = SDL_CreateProperties();
    if (!rendererProps) {
        printf("SDL3 RENDERER FORCE: Failed to create properties\n");
        return false;
    }
    
    // FORCE DirectX 11 specifically
    SDL_SetStringProperty(rendererProps, SDL_PROP_RENDERER_CREATE_NAME_STRING, "direct3d11");
    SDL_SetNumberProperty(rendererProps, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, 1);
    
    // Create the DirectX 11 renderer
    g_sdlContext.renderer = SDL_CreateRendererWithProperties(rendererProps);
    SDL_DestroyProperties(rendererProps);
    
    if (!g_sdlContext.renderer) {
        printf("SDL3 RENDERER FORCE: Failed to create DirectX 11 renderer: %s\n", SDL_GetError());
        return false;
    }
    
    printf("SDL3 RENDERER FORCE: Successfully forced DirectX 11!\n");
    return true;
}

} // namespace SDL3Integration
} // namespace FM2K