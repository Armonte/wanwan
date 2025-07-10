#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <MinHook.h>
#include <SDL3/SDL.h>

// Enhanced DLL with SDL3 rendering integration
static HANDLE g_logFile = nullptr;
static bool g_sdl3_initialized = false;
static SDL_AtomicInt g_frame_counter;
static bool g_frame_counter_initialized = false;

// SDL3 Context for rendering
struct SDL3Context {
    bool initialized = false;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* gameBuffer = nullptr;
    SDL_Texture* backBuffer = nullptr;
    int windowWidth = 640;
    int windowHeight = 480;
    int gameWidth = 256;
    int gameHeight = 240;
    bool isFullscreen = false;
};

static SDL3Context g_sdlContext;

// Simple logging function that writes to both file and SDL
void LogMessage(const char* message) {
    // Always try file logging first
    FILE* log = fopen("C:\\Games\\fm2k_hook_log.txt", "a");
    if (log) {
        fprintf(log, "%s\n", message);
        fflush(log);
        fclose(log);
    }
    
    // Also try SDL logging if available
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K HOOK: %s", message);
}

// Hook function pointers
typedef int (__cdecl *ProcessInputsFn)();
typedef int (__cdecl *InitializeGameFn)();
typedef int (__cdecl *InitializeDirectDrawFn)(int isFullScreen, void* windowHandle);
typedef LRESULT (__stdcall *WindowProcFn)(HWND, UINT, WPARAM, LPARAM);
typedef HWND (WINAPI *CreateWindowExAFn)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);

static ProcessInputsFn original_process_inputs = nullptr;
static InitializeGameFn original_initialize_game = nullptr;
static InitializeDirectDrawFn original_initialize_directdraw = nullptr;
static WindowProcFn original_window_proc = nullptr;
static CreateWindowExAFn original_create_window_ex_a = nullptr;

typedef int (__cdecl *RenderGameFn)();
static RenderGameFn original_render_game = nullptr;

typedef void (__thiscall *InitializeDirectDrawModeFn)(void* this_ptr);
static InitializeDirectDrawModeFn original_initialize_directdraw_mode = nullptr;
static uint32_t hook_call_count = 0;
static HWND g_game_window = nullptr;

// Forward declarations
void CopySDLContentToGameWindow();

// DirectDraw Surface Virtual Function Table - IDirectDrawSurface interface
// Based on the actual DirectDraw COM interface layout
struct DirectDrawSurfaceVTable {
    void* QueryInterface;        // offset 0
    void* AddRef;               // offset 4
    void* Release;              // offset 8
    void* AddAttachedSurface;   // offset 12
    void* AddOverlayDirtyRect;  // offset 16
    void* Blt;                  // offset 20
    void* BltBatch;             // offset 24
    void* BltFast;              // offset 28
    void* DeleteAttachedSurface; // offset 32
    void* EnumAttachedSurfaces; // offset 36
    void* EnumOverlayZOrders;   // offset 40
    void* Flip;                 // offset 44  - Called by game at offset 44
    void* GetAttachedSurface;   // offset 48
    void* GetBltStatus;         // offset 52
    void* GetCaps;              // offset 56
    void* GetClipper;           // offset 60
    void* GetColorKey;          // offset 64
    void* GetDC;                // offset 68
    void* GetFlipStatus;        // offset 72
    void* GetOverlayPosition;   // offset 76
    void* GetPalette;           // offset 80
    void* GetPixelFormat;       // offset 84
    void* GetSurfaceDesc;       // offset 88
    void* Initialize;           // offset 92
    void* IsLost;               // offset 96  - Called by game at offset 96
    void* Lock;                 // offset 100 - Called by game at offset 100
    void* ReleaseDC;            // offset 104
    void* Restore;              // offset 108 - Called by game at offset 108
    void* SetClipper;           // offset 112
    void* SetColorKey;          // offset 116
    void* SetOverlayPosition;   // offset 120
    void* SetPalette;           // offset 124
    void* Unlock;               // offset 128 - Called by game at offset 128
    void* UpdateOverlay;        // offset 132
    void* UpdateOverlayDisplay; // offset 136
    void* UpdateOverlayZOrder;  // offset 140
};

// DirectDraw Surface structure
struct DummySurface {
    DirectDrawSurfaceVTable* vtable;
    SDL_Texture* texture;
    int width;
    int height;
    void* pixels;
    int pitch;
};

// SDL3 helper functions
bool InitializeSDL3Context() {
    if (g_sdlContext.initialized) {
        return true;
    }
    
    LogMessage("Initializing SDL3 context...");
    
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        char buffer[256];
        sprintf(buffer, "SDL_Init failed: %s", SDL_GetError());
        LogMessage(buffer);
        return false;
    }
    
    g_sdlContext.initialized = true;
    LogMessage("SDL3 context initialized successfully");
    return true;
}

bool HijackGameWindow(HWND gameHwnd) {
    if (g_sdlContext.window) {
        return true;
    }
    
    LogMessage("Creating separate SDL3 window for rendering...");
    
    // Get the game window's position and size for reference
    RECT gameRect;
    GetWindowRect(gameHwnd, &gameRect);
    int gameWidth = gameRect.right - gameRect.left;
    int gameHeight = gameRect.bottom - gameRect.top;
    
    char buffer[256];
    sprintf(buffer, "Game window dimensions: %dx%d at (%d, %d)", gameWidth, gameHeight, gameRect.left, gameRect.top);
    LogMessage(buffer);
    
            // Create a separate SDL3 window instead of wrapping the existing one
        g_sdlContext.window = SDL_CreateWindow(
            "FM2K SDL3 Renderer",
            gameWidth,
            gameHeight,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALWAYS_ON_TOP  // Start hidden, stay on top when shown
        );
    
    if (!g_sdlContext.window) {
        sprintf(buffer, "SDL_CreateWindow failed: %s", SDL_GetError());
        LogMessage(buffer);
        return false;
    }
    
    // Get the SDL3 window's native HWND
    HWND sdlHwnd = (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(g_sdlContext.window), 
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, 
        NULL
    );
    
    // Store both window handles
    g_game_window = gameHwnd;  // Original game window
    
    sprintf(buffer, "Created separate SDL3 window: game_hwnd=%p, sdl_hwnd=%p", gameHwnd, sdlHwnd);
    LogMessage(buffer);
    
    // Position the SDL3 window at the same location as the game window
    SDL_SetWindowPosition(g_sdlContext.window, gameRect.left, gameRect.top);
    
    LogMessage("Separate SDL3 window created successfully");
    return true;
}

bool CreateSDL3Renderer() {
    if (g_sdlContext.renderer) {
        return true;
    }
    
    if (!g_sdlContext.window) {
        LogMessage("ERROR: Cannot create renderer - no SDL3 window available");
        return false;
    }
    
    // Try to create renderer with DirectX 11 backend first
    g_sdlContext.renderer = SDL_CreateRenderer(g_sdlContext.window, "direct3d11");
    
    if (!g_sdlContext.renderer) {
        char buffer[256];
        sprintf(buffer, "DirectX 11 renderer failed: %s", SDL_GetError());
        LogMessage(buffer);
        
        // Fall back to default renderer (let SDL3 choose)
        LogMessage("Falling back to default renderer...");
        g_sdlContext.renderer = SDL_CreateRenderer(g_sdlContext.window, nullptr);
        
        if (!g_sdlContext.renderer) {
            sprintf(buffer, "Default renderer also failed: %s", SDL_GetError());
            LogMessage(buffer);
            return false;
        }
        
        LogMessage("SDL3 default renderer created successfully");
    } else {
        LogMessage("SDL3 DirectX 11 renderer created successfully");
    }
    
    // Enable VSync
    SDL_SetRenderVSync(g_sdlContext.renderer, 1);
    
    return true;
}

bool CreateGameTextures() {
    if (g_sdlContext.gameBuffer) {
        return true;
    }
    
    // Create game buffer at native resolution (256x240)
    g_sdlContext.gameBuffer = SDL_CreateTexture(
        g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,  // Use STREAMING for Lock/Unlock access
        g_sdlContext.gameWidth,
        g_sdlContext.gameHeight
    );
    
    if (!g_sdlContext.gameBuffer) {
        char buffer[256];
        sprintf(buffer, "Failed to create game buffer: %s", SDL_GetError());
        LogMessage(buffer);
        return false;
    }
    
    SDL_SetTextureScaleMode(g_sdlContext.gameBuffer, SDL_SCALEMODE_NEAREST);
    LogMessage("Game buffer texture created (256x240)");
    return true;
}

void ProcessGameFrame() {
    if (!g_sdlContext.renderer || !g_sdlContext.gameBuffer) {
        return;
    }
    
    // Set render target to game buffer (256x240)
    SDL_SetRenderTarget(g_sdlContext.renderer, g_sdlContext.gameBuffer);
    SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdlContext.renderer);
    
    // Here the game would normally render to DirectDraw surfaces
    // Instead, we'll let the game's drawing code render to our SDL3 texture
    // This happens automatically via the DirectDraw compatibility layer
}

void RenderGameToWindow() {
    if (!g_sdlContext.renderer || !g_sdlContext.gameBuffer || !g_game_window) {
        return;
    }
    
    // Step 1: Render to SDL3 window first
    SDL_SetRenderTarget(g_sdlContext.renderer, nullptr);
    SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdlContext.renderer);
    
    // Get SDL3 window dimensions
    int sdlWindowWidth, sdlWindowHeight;
    SDL_GetWindowSize(g_sdlContext.window, &sdlWindowWidth, &sdlWindowHeight);
    
    // Calculate scaling to maintain aspect ratio and center the game
    float windowAspect = (float)sdlWindowWidth / sdlWindowHeight;
    float gameAspect = (float)g_sdlContext.gameWidth / g_sdlContext.gameHeight;  // 256/240 = 1.067
    
    SDL_FRect destRect;
    if (windowAspect > gameAspect) {
        // Window is wider - letterbox on sides
        float scale = (float)sdlWindowHeight / g_sdlContext.gameHeight;
        destRect.w = g_sdlContext.gameWidth * scale;
        destRect.h = (float)sdlWindowHeight;
        destRect.x = (sdlWindowWidth - destRect.w) / 2;
        destRect.y = 0;
    } else {
        // Window is taller - letterbox on top/bottom
        float scale = (float)sdlWindowWidth / g_sdlContext.gameWidth;
        destRect.w = (float)sdlWindowWidth;
        destRect.h = g_sdlContext.gameHeight * scale;
        destRect.x = 0;
        destRect.y = (sdlWindowHeight - destRect.h) / 2;
    }
    
    // Render the scaled game buffer to SDL3 window using NEAREST_NEIGHBOR filtering
    SDL_RenderTexture(g_sdlContext.renderer, g_sdlContext.gameBuffer, nullptr, &destRect);
    SDL_RenderPresent(g_sdlContext.renderer);
    
    // Step 2: Copy SDL3 window content to the original game window
    CopySDLContentToGameWindow();
}

void CopySDLContentToGameWindow() {
    if (!g_sdlContext.window || !g_game_window) {
        return;
    }
    
    HWND sdlHwnd = (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(g_sdlContext.window), 
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, 
        NULL
    );
    
    if (!sdlHwnd) {
        LogMessage("ERROR: Failed to get SDL3 window HWND for content copying");
        return;
    }
    
    // Get device contexts for both windows
    HDC sdlDC = GetDC(sdlHwnd);
    HDC gameDC = GetDC(g_game_window);
    
    if (!sdlDC || !gameDC) {
        char buffer[256];
        sprintf(buffer, "Failed to get device contexts: sdlDC=%p, gameDC=%p", sdlDC, gameDC);
        LogMessage(buffer);
        if (sdlDC) ReleaseDC(sdlHwnd, sdlDC);
        if (gameDC) ReleaseDC(g_game_window, gameDC);
        return;
    }
    
    // Get window dimensions with error checking
    RECT sdlRect, gameRect;
    if (!GetClientRect(sdlHwnd, &sdlRect) || !GetClientRect(g_game_window, &gameRect)) {
        LogMessage("ERROR: Failed to get window client rectangles");
        ReleaseDC(sdlHwnd, sdlDC);
        ReleaseDC(g_game_window, gameDC);
        return;
    }
    
    int sdlWidth = sdlRect.right - sdlRect.left;
    int sdlHeight = sdlRect.bottom - sdlRect.top;
    int gameWidth = gameRect.right - gameRect.left;
    int gameHeight = gameRect.bottom - gameRect.top;
    
    // Validate dimensions
    if (sdlWidth <= 0 || sdlHeight <= 0 || gameWidth <= 0 || gameHeight <= 0) {
        char buffer[256];
        sprintf(buffer, "Invalid window dimensions: SDL=%dx%d, Game=%dx%d", sdlWidth, sdlHeight, gameWidth, gameHeight);
        LogMessage(buffer);
        ReleaseDC(sdlHwnd, sdlDC);
        ReleaseDC(g_game_window, gameDC);
        return;
    }
    
    // Copy SDL3 window content to game window using StretchBlt for scaling
    BOOL copyResult = StretchBlt(
        gameDC, 0, 0, gameWidth, gameHeight,     // Destination
        sdlDC, 0, 0, sdlWidth, sdlHeight,        // Source
        SRCCOPY                                   // Copy operation
    );
    
    if (!copyResult) {
        LogMessage("WARNING: StretchBlt operation failed during content copying");
    }
    
    // Release device contexts
    ReleaseDC(sdlHwnd, sdlDC);
    ReleaseDC(g_game_window, gameDC);
}

void UpdateSDL3Events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            LogMessage("SDL3 quit event received");
        } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F11) {
            // Toggle fullscreen on F11
            Uint32 flags = SDL_GetWindowFlags(g_sdlContext.window);
            bool isFullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
            
            if (isFullscreen) {
                SDL_SetWindowFullscreen(g_sdlContext.window, false);
                SDL_SetWindowSize(g_sdlContext.window, 640, 480);
            } else {
                SDL_SetWindowFullscreen(g_sdlContext.window, true);
            }
            
            LogMessage(isFullscreen ? "Switched to windowed mode" : "Switched to fullscreen mode");
        }
    }
}

// Enhanced hook functions
int __cdecl Hook_ProcessInputs() {
    // Initialize frame counter if needed
    if (!g_frame_counter_initialized) {
        SDL_SetAtomicInt(&g_frame_counter, 0);
        g_frame_counter_initialized = true;
    }
    
    // Increment frame counter
    SDL_AddAtomicInt(&g_frame_counter, 1);
    uint32_t current_frame = SDL_GetAtomicInt(&g_frame_counter);
    hook_call_count++;
    
    // Update SDL3 events and render frame
    if (g_sdl3_initialized) {
        UpdateSDL3Events();
        
        // Process game frame and render to window every frame
        ProcessGameFrame();
        RenderGameToWindow();
    }
    
    // Log every 60 calls (about once per second at 60fps)
    if (hook_call_count % 60 == 0) {
        char buffer[256];
        sprintf(buffer, "Hook called %u times, frame %u", hook_call_count, current_frame);
        LogMessage(buffer);
    }
    
    // Call original function
    if (original_process_inputs) {
        return original_process_inputs();
    }
    return 0;
}

int __cdecl Hook_InitializeGame() {
    LogMessage("Hook_InitializeGame called");
    
    // Call original function first
    int result = 0;
    if (original_initialize_game) {
        result = original_initialize_game();
        LogMessage("Original initialize_game completed");
    }
    
    // Initialize SDL3 after game window is created
    if (!g_sdl3_initialized) {
        if (InitializeSDL3Context() && CreateSDL3Renderer() && CreateGameTextures()) {
            g_sdl3_initialized = true;
            LogMessage("SDL3 rendering system initialized successfully");
        } else {
            LogMessage("Failed to initialize SDL3 rendering system");
        }
    }
    
    return result;
}

// DirectDraw Surface Method Implementations
// These will be called directly by the game instead of going through vtables

HRESULT __stdcall SDL3_SurfaceLock(void* This, void* lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, void* hEvent) {
    LogMessage("DirectDraw Surface Lock called - redirecting to SDL3 texture");
    
    DummySurface* surface = (DummySurface*)This;
    if (!surface || !surface->texture) {
        LogMessage("ERROR: Invalid surface in Lock call");
        return 0x80004005; // DDERR_GENERIC
    }
    
    // Lock the SDL3 texture for pixel access
    void* pixels;
    int pitch;
    if (SDL_LockTexture(surface->texture, NULL, &pixels, &pitch) < 0) {
        char buffer[256];
        sprintf(buffer, "SDL_LockTexture failed: %s", SDL_GetError());
        LogMessage(buffer);
        return 0x80004005; // DDERR_GENERIC
    }
    
    // Store the pixel data for Unlock
    surface->pixels = pixels;
    surface->pitch = pitch;
    
    // Fill DirectDraw surface descriptor if provided
    // Based on the decompiled code, this is a DDSURFACEDESC structure
    if (lpDDSurfaceDesc) {
        // Clear the structure first
        memset(lpDDSurfaceDesc, 0, 108);
        
        int* desc = (int*)lpDDSurfaceDesc;
        desc[0] = 108;                    // dwSize (DDSURFACEDESC size)
        desc[1] = 0x1 | 0x4 | 0x8 | 0x20; // dwFlags (DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_LPSURFACE)
        desc[2] = surface->height;        // dwHeight
        desc[3] = surface->width;         // dwWidth
        desc[4] = pitch;                  // lPitch
        // Skip some fields...
        desc[9] = (int)pixels;            // lpSurface (pixel data pointer)
        
        char buffer[256];
        sprintf(buffer, "Surface Lock: %dx%d, pitch=%d, pixels=%p", surface->width, surface->height, pitch, pixels);
        LogMessage(buffer);
    }
    
    return 0; // S_OK
}

HRESULT __stdcall SDL3_SurfaceUnlock(void* This, void* lpSurfaceData) {
    LogMessage("DirectDraw Surface Unlock called - updating SDL3 texture");
    
    DummySurface* surface = (DummySurface*)This;
    if (!surface || !surface->texture) {
        LogMessage("ERROR: Invalid surface in Unlock call");
        return 0x80004005; // DDERR_GENERIC
    }
    
    // Unlock the SDL3 texture
    SDL_UnlockTexture(surface->texture);
    surface->pixels = nullptr;
    surface->pitch = 0;
    
    LogMessage("Surface Unlock completed successfully");
    return 0; // S_OK
}

HRESULT __stdcall SDL3_SurfaceBlt(void* This, void* lpDestRect, void* lpDDSrcSurface, void* lpSrcRect, DWORD dwFlags, void* lpDDBltFx) {
    LogMessage("DirectDraw Surface Blt called - performing SDL3 texture copy");
    
    // For now, just return success - the game's main rendering happens through Lock/Unlock
    // More complex Blt operations can be implemented later if needed
    return 0; // S_OK
}

HRESULT __stdcall SDL3_SurfaceIsLost(void* This) {
    LogMessage("DirectDraw Surface IsLost called - SDL3 surfaces are never lost");
    
    // SDL3 surfaces are never lost, so always return S_OK
    return 0; // S_OK
}

HRESULT __stdcall SDL3_SurfaceRestore(void* This) {
    LogMessage("DirectDraw Surface Restore called - SDL3 surfaces don't need restoration");
    
    // SDL3 surfaces don't need restoration, so always return S_OK
    return 0; // S_OK
}

HRESULT __stdcall SDL3_SurfaceFlip(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags) {
    LogMessage("DirectDraw Surface Flip called - triggering SDL3 frame presentation");
    
    // This is called when the game wants to present the frame
    // Trigger our SDL3 rendering pipeline
    if (g_sdl3_initialized && g_sdlContext.renderer) {
        RenderGameToWindow();
    }
    
    return 0; // S_OK
}

// Simplified vtable with just the essential methods
// DirectDrawSurfaceVTable already defined at top of file

// Create the vtable with our implementations
static DirectDrawSurfaceVTable g_surfaceVTable = {
    nullptr, // QueryInterface
    nullptr, // AddRef
    nullptr, // Release
    nullptr, // AddAttachedSurface
    nullptr, // AddOverlayDirtyRect
    (void*)SDL3_SurfaceBlt, // Blt
    nullptr, // BltBatch
    nullptr, // BltFast
    nullptr, // DeleteAttachedSurface
    nullptr, // EnumAttachedSurfaces
    nullptr, // EnumOverlayZOrders
    (void*)SDL3_SurfaceFlip, // Flip - offset 44
    nullptr, // GetAttachedSurface
    nullptr, // GetBltStatus
    nullptr, // GetCaps
    nullptr, // GetClipper
    nullptr, // GetColorKey
    nullptr, // GetDC
    nullptr, // GetFlipStatus
    nullptr, // GetOverlayPosition
    nullptr, // GetPalette
    nullptr, // GetPixelFormat
    nullptr, // GetSurfaceDesc
    nullptr, // Initialize
    (void*)SDL3_SurfaceIsLost, // IsLost - offset 96
    (void*)SDL3_SurfaceLock, // Lock - offset 100
    nullptr, // ReleaseDC
    (void*)SDL3_SurfaceRestore, // Restore - offset 108
    nullptr, // SetClipper
    nullptr, // SetColorKey
    nullptr, // SetOverlayPosition
    nullptr, // SetPalette
    (void*)SDL3_SurfaceUnlock, // Unlock - offset 128
    nullptr, // UpdateOverlay
    nullptr, // UpdateOverlayDisplay
    nullptr, // UpdateOverlayZOrder
};

struct DummyDirectDraw {
    void* vtable;
};

// DirectDraw Surface structure already defined at top of file

static DummyDirectDraw g_dummyDirectDraw;
static DummySurface g_primarySurface;
static DummySurface g_backSurface;
static DummySurface g_spriteSurface;

int __cdecl Hook_InitializeDirectDraw(int isFullScreen, void* windowHandle) {
    LogMessage("Hook_InitializeDirectDraw called - SDL3 DirectDraw replacement");
    
    char buffer[256];
    sprintf(buffer, "DirectDraw init: fullscreen=%d, windowHandle=%p", isFullScreen, windowHandle);
    LogMessage(buffer);
    
    // Ensure SDL3 is initialized
    if (!g_sdl3_initialized) {
        LogMessage("SDL3 not initialized, attempting initialization...");
        
        // Try to initialize SDL3 with comprehensive error handling
        bool sdl3_success = false;
        const char* failure_reason = "";
        
        if (!InitializeSDL3Context()) {
            failure_reason = "SDL3 context initialization failed";
        } else if (!CreateSDL3Renderer()) {
            failure_reason = "SDL3 renderer creation failed";
        } else if (!CreateGameTextures()) {
            failure_reason = "SDL3 texture creation failed";
        } else {
            sdl3_success = true;
        }
        
        if (sdl3_success) {
            g_sdl3_initialized = true;
            LogMessage("SDL3 initialized successfully during DirectDraw setup");
        } else {
            char buffer[256];
            sprintf(buffer, "SDL3 initialization failed: %s - falling back to original DirectDraw", failure_reason);
            LogMessage(buffer);
            
            // Clean up any partial SDL3 state
            if (g_sdlContext.gameBuffer) {
                SDL_DestroyTexture(g_sdlContext.gameBuffer);
                g_sdlContext.gameBuffer = nullptr;
            }
            if (g_sdlContext.backBuffer) {
                SDL_DestroyTexture(g_sdlContext.backBuffer);
                g_sdlContext.backBuffer = nullptr;
            }
            if (g_sdlContext.renderer) {
                SDL_DestroyRenderer(g_sdlContext.renderer);
                g_sdlContext.renderer = nullptr;
            }
            if (g_sdlContext.window) {
                SDL_DestroyWindow(g_sdlContext.window);
                g_sdlContext.window = nullptr;
            }
            if (g_sdlContext.initialized) {
                SDL_Quit();
                g_sdlContext.initialized = false;
            }
            
            // Fall back to original DirectDraw implementation
            if (original_initialize_directdraw) {
                LogMessage("Calling original DirectDraw initialization as fallback");
                return original_initialize_directdraw(isFullScreen, windowHandle);
            } else {
                LogMessage("ERROR: No original DirectDraw function available for fallback");
                return 0;
            }
        }
    }
    
    // Create additional SDL3 textures for DirectDraw surface replacements
    if (!g_sdlContext.backBuffer) {
        g_sdlContext.backBuffer = SDL_CreateTexture(
            g_sdlContext.renderer,
            SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_STREAMING,  // Use STREAMING for Lock/Unlock access
            640, 480  // Standard back buffer size
        );
        if (g_sdlContext.backBuffer) {
            SDL_SetTextureScaleMode(g_sdlContext.backBuffer, SDL_SCALEMODE_NEAREST);
            LogMessage("Created SDL3 back buffer texture (640x480)");
        }
    }
    
    // Create sprite surface texture (256x256 as per research docs)
    SDL_Texture* spriteTexture = SDL_CreateTexture(
        g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,  // Use STREAMING for Lock/Unlock access
        256, 256
    );
    if (spriteTexture) {
        SDL_SetTextureScaleMode(spriteTexture, SDL_SCALEMODE_NEAREST);
        LogMessage("Created SDL3 sprite texture (256x256)");
        
        // Set up sprite surface structure
        g_spriteSurface.vtable = &g_surfaceVTable;
        g_spriteSurface.texture = spriteTexture;
        g_spriteSurface.width = 256;
        g_spriteSurface.height = 256;
        g_spriteSurface.pixels = nullptr;
        g_spriteSurface.pitch = 0;
    }
    
    // Set up dummy DirectDraw interfaces for game compatibility
    // These pointers need to match the addresses that the game expects
    
    // From research docs - DirectDraw global variables
    // g_direct_draw @ 0x424758
    // g_dd_primary_surface @ 0x424750  
    // g_dd_back_buffer @ 0x424754
    
    // Set up DirectDraw global variables using IDA verified addresses
    void** pDirectDraw = (void**)0x424758;        // g_direct_draw
    void** pPrimarySurface = (void**)0x424750;    // g_dd_primary_surface  
    void** pBackBuffer = (void**)0x424754;        // g_dd_back_buffer
    
    if (pDirectDraw) {
        *pDirectDraw = &g_dummyDirectDraw;
        LogMessage("Set DirectDraw pointer at 0x424758 (g_direct_draw)");
    }
    
    if (pPrimarySurface) {
        g_primarySurface.vtable = &g_surfaceVTable;
        g_primarySurface.texture = g_sdlContext.gameBuffer;
        g_primarySurface.width = g_sdlContext.gameWidth;
        g_primarySurface.height = g_sdlContext.gameHeight;
        g_primarySurface.pixels = nullptr;
        g_primarySurface.pitch = 0;
        *pPrimarySurface = &g_primarySurface;
        LogMessage("Set primary surface pointer at 0x424750 (g_dd_primary_surface) with vtable");
    }
    
    if (pBackBuffer) {
        g_backSurface.vtable = &g_surfaceVTable;
        g_backSurface.texture = g_sdlContext.backBuffer;
        g_backSurface.width = 640;
        g_backSurface.height = 480;
        g_backSurface.pixels = nullptr;
        g_backSurface.pitch = 0;
        *pBackBuffer = &g_backSurface;
        LogMessage("Set back buffer pointer at 0x424754 (g_dd_back_buffer) with vtable");
    }
    
    // Set up sprite surface with vtable (this was missing!)
    g_spriteSurface.vtable = &g_surfaceVTable;
    g_spriteSurface.texture = spriteTexture;
    g_spriteSurface.width = 256;
    g_spriteSurface.height = 256;
    g_spriteSurface.pixels = nullptr;
    g_spriteSurface.pitch = 0;
    
    // Set up critical game variables for resolution using IDA-verified addresses
    short* pStageWidth = (short*)0x4452B8;   // g_stage_width_pixels
    short* pStageHeight = (short*)0x4452BA;  // g_stage_height_pixels  
    int* pDestWidth = (int*)0x447F20;        // g_dest_width
    int* pDestHeight = (int*)0x447F24;       // g_dest_height
    
    // Validate address access before writing
    if (!IsBadWritePtr(pStageWidth, sizeof(short))) {
        *pStageWidth = 256;  // Native game width
        LogMessage("Set g_stage_width_pixels to 256");
    } else {
        LogMessage("ERROR: Cannot access g_stage_width_pixels at 0x4452B8");
    }
    
    if (!IsBadWritePtr(pStageHeight, sizeof(short))) {
        *pStageHeight = 240; // Native game height  
        LogMessage("Set g_stage_height_pixels to 240");
    } else {
        LogMessage("ERROR: Cannot access g_stage_height_pixels at 0x4452BA");
    }
    
    if (!IsBadWritePtr(pDestWidth, sizeof(int))) {
        *pDestWidth = 256;   // Destination width
        LogMessage("Set g_dest_width to 256");
    } else {
        LogMessage("ERROR: Cannot access g_dest_width at 0x447F20");
    }
    
    if (!IsBadWritePtr(pDestHeight, sizeof(int))) {
        *pDestHeight = 240;  // Destination height
        LogMessage("Set g_dest_height to 240");
    } else {
        LogMessage("ERROR: Cannot access g_dest_height at 0x447F24");
    }
    
    // Set the game's window handle global at the correct IDA-verified address
    HWND* pGameWindowHandle = (HWND*)0x4246F8; // g_hwnd_parent from IDA
    if (!IsBadWritePtr(pGameWindowHandle, sizeof(HWND))) {
        *pGameWindowHandle = g_game_window;
        LogMessage("Set g_hwnd_parent global with game window handle");
        
        // Verify the setting worked
        char buffer[256];
        sprintf(buffer, "g_hwnd_parent verification: stored=%p, game=%p", *pGameWindowHandle, g_game_window);
        LogMessage(buffer);
    } else {
        LogMessage("ERROR: Failed to access g_hwnd_parent at 0x4246F8");
    }
    
    LogMessage("DirectDraw SDL3 replacement initialization complete");
    return 1; // Success
}

LRESULT __stdcall Hook_WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Handle SDL3 events and forward to original window procedure
    if (g_sdl3_initialized) {
        UpdateSDL3Events();
        
        // Render frame if this is a paint message
        if (uMsg == WM_PAINT) {
            RenderGameToWindow();
        }
    }
    
    // Call original window procedure
    if (original_window_proc) {
        return original_window_proc(hWnd, uMsg, wParam, lParam);
    }
    
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

HWND WINAPI Hook_CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName,
                                 DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
                                 HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam) {
    
    LogMessage("CreateWindowExA hook called");
    
    // Call original function to create the game window
    HWND gameWindow = nullptr;
    if (original_create_window_ex_a) {
        gameWindow = original_create_window_ex_a(dwExStyle, lpClassName, lpWindowName,
                                                 dwStyle, X, Y, nWidth, nHeight,
                                                 hWndParent, hMenu, hInstance, lpParam);
    }
    
    if (!gameWindow) {
        LogMessage("Original CreateWindowExA failed");
        return nullptr;
    }
    
    // Check if this is the main game window (look for "KGT2KGAME" class or similar)
    char className[256];
    GetClassNameA(gameWindow, className, sizeof(className));
    
    char buffer[512];
    sprintf(buffer, "Window created: class='%s', title='%s', hwnd=%p", 
            className, lpWindowName ? lpWindowName : "NULL", gameWindow);
    LogMessage(buffer);
    
    // If this looks like the main game window, hijack it with SDL3
    if (strcmp(className, "KGT2KGAME") == 0) {
        LogMessage("Detected main game window (KGT2KGAME class) - initiating SDL3 hijack");
        
        // Set the game's window handle global at the correct address from IDA verified addresses
        HWND* pGameWindowHandle = (HWND*)0x4246F8; // g_hwnd_parent from IDA
        if (pGameWindowHandle) {
            *pGameWindowHandle = gameWindow;
            LogMessage("Updated g_hwnd_parent global with game window handle");
            
            // Verify the setting worked
            sprintf(buffer, "g_hwnd_parent verification: stored=%p, game=%p", *pGameWindowHandle, gameWindow);
            LogMessage(buffer);
        } else {
            LogMessage("ERROR: Failed to access g_hwnd_parent at 0x4246F8");
        }
        
        // Initialize SDL3 if not already done
        if (!g_sdl3_initialized) {
            if (InitializeSDL3Context()) {
                if (HijackGameWindow(gameWindow) && CreateSDL3Renderer() && CreateGameTextures()) {
                    g_sdl3_initialized = true;
                    LogMessage("SDL3 window hijacking completed successfully");
                    
                                            // Keep the SDL3 window hidden - we only use it for rendering
                        // SDL_ShowWindow(g_sdlContext.window);
                        LogMessage("SDL3 window created but kept hidden for rendering");
                } else {
                    LogMessage("SDL3 window hijacking failed");
                }
            }
        }
    }
    
    return gameWindow;
}

// Hook for initialize_directdraw_mode to override DirectDraw surface creation
void __thiscall Hook_InitializeDirectDrawMode(void* this_ptr) {
    LogMessage("Hook_InitializeDirectDrawMode called - preventing real DirectDraw creation");
    
    // Call the original function first to let it do its setup
    if (original_initialize_directdraw_mode) {
        original_initialize_directdraw_mode(this_ptr);
    }
    
    // Now override the DirectDraw pointers with our SDL3 surfaces
    LogMessage("Overriding DirectDraw surfaces with SDL3 surfaces after game initialization");
    
    // Set up DirectDraw global variables using IDA verified addresses
    void** pDirectDraw = (void**)0x424758;        // g_direct_draw
    void** pPrimarySurface = (void**)0x424750;    // g_dd_primary_surface  
    void** pBackBuffer = (void**)0x424754;        // g_dd_back_buffer
    
    if (pDirectDraw) {
        *pDirectDraw = &g_dummyDirectDraw;
        LogMessage("Re-set DirectDraw pointer at 0x424758 (g_direct_draw) after game init");
    }
    
    if (pPrimarySurface) {
        g_primarySurface.vtable = &g_surfaceVTable;
        g_primarySurface.texture = g_sdlContext.gameBuffer;
        g_primarySurface.width = g_sdlContext.gameWidth;
        g_primarySurface.height = g_sdlContext.gameHeight;
        g_primarySurface.pixels = nullptr;
        g_primarySurface.pitch = 0;
        *pPrimarySurface = &g_primarySurface;
        LogMessage("Re-set primary surface pointer at 0x424750 after game init");
    }
    
    if (pBackBuffer) {
        g_backSurface.vtable = &g_surfaceVTable;
        g_backSurface.texture = g_sdlContext.backBuffer;
        g_backSurface.width = 640;
        g_backSurface.height = 480;
        g_backSurface.pixels = nullptr;
        g_backSurface.pitch = 0;
        *pBackBuffer = &g_backSurface;
        LogMessage("Re-set back buffer pointer at 0x424754 after game init");
    }
    
    LogMessage("DirectDraw surface override completed");
}

// Main rendering loop hook - intercepts render_game function
int __cdecl Hook_ProcessScreenUpdates() {
    static int callCount = 0;
    callCount++;
    
    // First, let the original game do its rendering to DirectDraw surfaces
    int originalResult = 0;
    if (original_render_game) {
        originalResult = original_render_game();
    }
    
    // Update SDL events once per frame (essential for input and timing)
    UpdateSDL3Events();
    
    // If SDL3 is not initialized, just return the original result
    if (!g_sdl3_initialized || !g_sdlContext.renderer) {
        return originalResult;
    }
    
    // Now intercept the DirectDraw surface data and transfer it to SDL3
    // The game has already rendered to the back buffer, so we can copy from there
    
    // Get the back buffer pixels that the game just rendered to
    void** ppBackBufferPixels = (void**)0x46FF64;  // From the decompiled render_game function
    if (ppBackBufferPixels && *ppBackBufferPixels) {
        unsigned char* gamePixels = (unsigned char*)*ppBackBufferPixels;
        
        // Lock our SDL3 game buffer texture
        void* sdlPixels;
        int sdlPitch;
        if (SDL_LockTexture(g_sdlContext.gameBuffer, NULL, &sdlPixels, &sdlPitch) == 0) {
            unsigned char* dstData = (unsigned char*)sdlPixels;
            
            // Copy the game's rendered pixels to our SDL3 texture
            // The game uses 640x480 back buffer, but the actual game area is 256x240
            for (int y = 0; y < 240; y++) {
                for (int x = 0; x < 256; x++) {
                    // The game buffer might be palettized, so we need to handle that
                    unsigned char pixelValue = gamePixels[y * 1280 + x];  // 1280 = pitch from BlitPitched call
                    
                    // Convert to RGBA (simple grayscale for now)
                    int dstOffset = y * sdlPitch + x * 4;
                    if (dstOffset + 3 < sdlPitch * 240) {
                        dstData[dstOffset + 0] = pixelValue;  // R
                        dstData[dstOffset + 1] = pixelValue;  // G
                        dstData[dstOffset + 2] = pixelValue;  // B
                        dstData[dstOffset + 3] = 255;        // A
                    }
                }
            }
            
            SDL_UnlockTexture(g_sdlContext.gameBuffer);
            
            // Now render our SDL3 content to the window
            RenderGameToWindow();
            
            // Present the final frame
            SDL_RenderPresent(g_sdlContext.renderer);
            
            // Log success occasionally
            static int renderCount = 0;
            renderCount++;
            if (renderCount % 300 == 0) {  // Every 5 seconds at 60fps
                LogMessage("Successfully intercepted and rendered game data via SDL3");
            }
        }
    }
    
    return originalResult;
}

// Main rendering loop hook function

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        {
            DisableThreadLibraryCalls(hModule);
            
            LogMessage("DLL attached to process!");
            
            // Wait a bit for the process to initialize
            Sleep(100);
            
            LogMessage("Initializing MinHook...");
            
            // Initialize MinHook
            MH_STATUS mh_init = MH_Initialize();
            if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
                char buffer[256];
                sprintf(buffer, "ERROR: MH_Initialize failed with status %d", mh_init);
                LogMessage(buffer);
                return FALSE;
            }
            
            LogMessage("MinHook initialized successfully");
            
            // Hook 1: process_input_history function at 0x4025A0 (working baseline)
            void* target1 = (void*)0x4025A0;
            if (IsBadCodePtr((FARPROC)target1)) {
                LogMessage("ERROR: Target address 0x4025A0 is not valid code");
                MH_Uninitialize();
                return FALSE;
            }
            
            LogMessage("Creating process_input_history hook...");
            MH_STATUS status1 = MH_CreateHook(target1, (void*)Hook_ProcessInputs, (void**)&original_process_inputs);
            if (status1 != MH_OK) {
                char buffer[256];
                sprintf(buffer, "ERROR: Failed to create process_inputs hook, status %d", status1);
                LogMessage(buffer);
                MH_Uninitialize();
                return FALSE;
            }
            
            // Hook 2: initialize_game function at 0x4056C0 (for SDL3 setup)
            void* target2 = (void*)0x4056C0;
            LogMessage("Creating initialize_game hook...");
            MH_STATUS status2 = MH_CreateHook(target2, (void*)Hook_InitializeGame, (void**)&original_initialize_game);
            if (status2 != MH_OK) {
                char buffer[256];
                sprintf(buffer, "WARNING: Failed to create initialize_game hook, status %d", status2);
                LogMessage(buffer);
                // Continue without this hook
            }
            
            // Hook 3: initialize_directdraw_mode function at 0x404980 (to override surface creation)
            void* target3 = (void*)0x404980;
            LogMessage("Creating initialize_directdraw_mode hook...");
            MH_STATUS status3 = MH_CreateHook(target3, (void*)Hook_InitializeDirectDrawMode, (void**)&original_initialize_directdraw_mode);
            if (status3 != MH_OK) {
                char buffer[256];
                sprintf(buffer, "WARNING: Failed to create initialize_directdraw_mode hook, status %d", status3);
                LogMessage(buffer);
                // Continue without this hook
            }
            
            // Hook 4: main_window_proc function at 0x405F50 (for message forwarding)
            void* target4 = (void*)0x405F50;
            LogMessage("Creating window_proc hook...");
            MH_STATUS status4 = MH_CreateHook(target4, (void*)Hook_WindowProc, (void**)&original_window_proc);
            if (status4 != MH_OK) {
                char buffer[256];
                sprintf(buffer, "WARNING: Failed to create window_proc hook, status %d", status4);
                LogMessage(buffer);
                // Continue without this hook
            }
            
            // Hook 5: CreateWindowExA for window hijacking
            LogMessage("Creating CreateWindowExA hook...");
            MH_STATUS status5 = MH_CreateHook((LPVOID)CreateWindowExA, (void*)Hook_CreateWindowExA, (void**)&original_create_window_ex_a);
            if (status5 != MH_OK) {
                char buffer[256];
                sprintf(buffer, "WARNING: Failed to create CreateWindowExA hook, status %d", status5);
                LogMessage(buffer);
                // Continue without this hook
            }
            
                         // Hook 6: render_game function at 0x404dd0 (main game rendering function)
             void* target6 = (void*)0x404dd0;
             LogMessage("Creating render_game hook...");
             MH_STATUS status6 = MH_CreateHook(target6, (void*)Hook_ProcessScreenUpdates, (void**)&original_render_game);
             if (status6 != MH_OK) {
                 char buffer[256];
                 sprintf(buffer, "WARNING: Failed to create render_game hook, status %d", status6);
                 LogMessage(buffer);
                 // Continue without this hook
             }
            
            LogMessage("Enabling all hooks...");
            
            // Enable all hooks
            if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
                LogMessage("ERROR: Failed to enable hooks");
                MH_Uninitialize();
                return FALSE;
            }
            
            LogMessage("SUCCESS: FM2K hook initialized!");
            
            // Signal the launcher that DLL initialization is complete
            HANDLE init_event = CreateEventW(nullptr, TRUE, FALSE, L"FM2KHook_Initialized");
            if (init_event) {
                SetEvent(init_event);
                CloseHandle(init_event);
                LogMessage("Signaled launcher that initialization is complete");
            } else {
                LogMessage("ERROR: Failed to create initialization event");
            }
            
            break;
        }
        
    case DLL_PROCESS_DETACH:
        {
            LogMessage("DLL detaching from process");
            
            // Cleanup hooks
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            break;
        }
    }
    return TRUE;
}