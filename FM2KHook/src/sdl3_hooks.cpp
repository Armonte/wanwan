#include "sdl3_hooks.h"
#include <cstdio>

// Define the global SDL context instance.
SDL3Context g_sdlContext = {};

// Pointers for the window procedure chain.
static WNDPROC g_originalSDLWindowProc = nullptr;
static WNDPROC g_originalGameWindowProc = nullptr;

void LogMessage(const char* message); // Forward declaration

bool InitializeSDL3() {
    if (g_sdlContext.initialized) {
        return true;
    }

    LogMessage("Initializing SDL3...");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "SDL_Init failed: %s", SDL_GetError());
        LogMessage(buffer);
        return false;
    }

    g_sdlContext.gameWidth = 256;
    g_sdlContext.gameHeight = 240;
    g_sdlContext.windowWidth = 640;
    g_sdlContext.windowHeight = 480;

    LogMessage("SDL3 initialized successfully.");
    return true;
}

bool CreateSDL3Context(HWND hwnd) {
    LogMessage("Creating SDL3 Context...");
    
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, hwnd);
    g_sdlContext.window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);

    if (!g_sdlContext.window) {
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "FATAL: SDL_CreateWindowWithProperties failed: %s", SDL_GetError());
        LogMessage(buffer);
        return false;
    }
    LogMessage(" -> SDL3 window docked to game HWND.");
    
    g_sdlContext.renderer = SDL_CreateRenderer(g_sdlContext.window, "direct3d11");
    if (!g_sdlContext.renderer) {
        LogMessage(" -> Failed to create DirectX 11 renderer, falling back to default.");
        g_sdlContext.renderer = SDL_CreateRenderer(g_sdlContext.window, nullptr);
    }

    if (!g_sdlContext.renderer) {
        LogMessage("FATAL: Failed to create any renderer.");
        return false;
    }
    LogMessage(" -> SDL3 renderer created.");
    
    SDL_SetRenderVSync(g_sdlContext.renderer, 1);

    g_sdlContext.gameTexture = SDL_CreateTexture(g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888, 
        SDL_TEXTUREACCESS_STREAMING, 
        g_sdlContext.gameWidth, 
        g_sdlContext.gameHeight);

    if (!g_sdlContext.gameTexture) {
        LogMessage("FATAL: Failed to create game texture.");
        return false;
    }
    LogMessage(" -> Game texture created.");

    g_originalSDLWindowProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)InterceptedWindowProc);
    LogMessage(" -> Window subclassed successfully.");

    g_sdlContext.initialized = true;
    LogMessage("SDL3 Context creation complete.");
    return true;
}

void CleanupSDL3() {
    if (!g_sdlContext.initialized) return;
    LogMessage("Cleaning up SDL3 context...");

    if (g_sdlContext.gameTexture) SDL_DestroyTexture(g_sdlContext.gameTexture);
    if (g_sdlContext.renderer) SDL_DestroyRenderer(g_sdlContext.renderer);
    if (g_sdlContext.window) SDL_DestroyWindow(g_sdlContext.window);
    SDL_Quit();
    
    memset(&g_sdlContext, 0, sizeof(SDL3Context));
    LogMessage("SDL3 context cleaned up successfully.");
}

void RenderGame() {
    if (!g_sdlContext.initialized) return;

    SDL_Renderer* renderer = g_sdlContext.renderer;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_GetWindowSize(g_sdlContext.window, &g_sdlContext.windowWidth, &g_sdlContext.windowHeight);
    float gameAspect = (float)g_sdlContext.gameWidth / g_sdlContext.gameHeight;
    float windowAspect = (float)g_sdlContext.windowWidth / g_sdlContext.windowHeight;

    SDL_FRect destRect = {0};
    if (windowAspect > gameAspect) {
        destRect.h = (float)g_sdlContext.windowHeight;
        destRect.w = destRect.h * gameAspect;
        destRect.x = (g_sdlContext.windowWidth - destRect.w) / 2.0f;
    } else {
        destRect.w = (float)g_sdlContext.windowWidth;
        destRect.h = destRect.w / gameAspect;
        destRect.y = (g_sdlContext.windowHeight - destRect.h) / 2.0f;
    }

    SDL_RenderTexture(renderer, g_sdlContext.gameTexture, NULL, &destRect);
    SDL_RenderPresent(renderer);
}

void PollSDLEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_KEY_DOWN) {
            // SDL3 no longer uses the 'keysym' nested struct.
            if (event.key.scancode == SDL_SCANCODE_F11) {
                ToggleFullscreen();
            }
        }
    }
}

LRESULT CALLBACK InterceptedWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (g_originalSDLWindowProc) {
        CallWindowProc(g_originalSDLWindowProc, hWnd, uMsg, wParam, lParam);
    }

    if (g_originalGameWindowProc) {
        return CallWindowProc(g_originalGameWindowProc, hWnd, uMsg, wParam, lParam);
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void ToggleFullscreen() {
    if (!g_sdlContext.initialized) return;

    g_sdlContext.isFullscreen = !g_sdlContext.isFullscreen;
    SDL_SetWindowFullscreen(g_sdlContext.window, g_sdlContext.isFullscreen);
    char buffer[128];
    sprintf_s(buffer, sizeof(buffer), "Toggled fullscreen: %s", g_sdlContext.isFullscreen ? "ON" : "OFF");
    LogMessage(buffer);
}

void SetOriginalWindowProc(WNDPROC proc) {
    LogMessage("Original game window procedure stored.");
    g_originalGameWindowProc = proc;
} 