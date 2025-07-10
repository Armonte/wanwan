#include <windows.h>
#include <sddl.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <MinHook.h>

// DirectDraw constants
#define DD_OK                   0x00000000L
#define DDERR_INVALIDPARAMS     0x887000057L
#define DDERR_SURFACEBUSY       0x887000176L
#define DDERR_NOTLOCKED         0x887000094L

// --- Globals ---
static HANDLE g_init_event = nullptr;
static bool g_dll_initialized = false;
static bool g_hooks_initialized = false;
static FILE* g_console_stream = nullptr;

// SDL3 Context Structure
struct SDL3Context {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* gameBuffer;
    SDL_Texture* backBuffer;
    int windowWidth;
    int windowHeight;
    int gameWidth;
    int gameHeight;
    bool initialized;
} g_sdlContext = {nullptr, nullptr, nullptr, nullptr, 640, 480, 256, 240, false};

// SDL3Surface forward declaration (defined below with vtable)

// DirectDraw Surface vtable structure
struct SDL3SurfaceVtbl {
    // IUnknown methods
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* This, REFIID riid, void** ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(void* This);
    ULONG (STDMETHODCALLTYPE *Release)(void* This);
    
    // Essential IDirectDrawSurface methods that FM2K uses
    HRESULT (STDMETHODCALLTYPE *AddAttachedSurface)(void* This, void* lpDDSAttachedSurface);
    HRESULT (STDMETHODCALLTYPE *AddOverlayDirtyRect)(void* This, LPRECT lpRect);
    HRESULT (STDMETHODCALLTYPE *Blt)(void* This, LPRECT lpDestRect, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx);
    HRESULT (STDMETHODCALLTYPE *BltBatch)(void* This, void* lpDDBltBatch, DWORD dwCount, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *BltFast)(void* This, DWORD dwX, DWORD dwY, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans);
    HRESULT (STDMETHODCALLTYPE *DeleteAttachedSurface)(void* This, DWORD dwFlags, void* lpDDSAttachedSurface);
    HRESULT (STDMETHODCALLTYPE *EnumAttachedSurfaces)(void* This, LPVOID lpContext, void* lpEnumSurfacesCallback);
    HRESULT (STDMETHODCALLTYPE *EnumOverlayZOrders)(void* This, DWORD dwFlags, LPVOID lpContext, void* lpfnCallback);
    HRESULT (STDMETHODCALLTYPE *Flip)(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *GetAttachedSurface)(void* This, void* lpDDSCaps, void** lplpDDAttachedSurface);
    HRESULT (STDMETHODCALLTYPE *GetBltStatus)(void* This, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *GetCaps)(void* This, void* lpDDSCaps);
    HRESULT (STDMETHODCALLTYPE *GetClipper)(void* This, void** lplpDDClipper);
    HRESULT (STDMETHODCALLTYPE *GetColorKey)(void* This, DWORD dwFlags, void* lpDDColorKey);
    HRESULT (STDMETHODCALLTYPE *GetDC)(void* This, HDC* lphDC);
    HRESULT (STDMETHODCALLTYPE *GetFlipStatus)(void* This, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *GetOverlayPosition)(void* This, LPLONG lplX, LPLONG lplY);
    HRESULT (STDMETHODCALLTYPE *GetPalette)(void* This, void** lplpDDPalette);
    HRESULT (STDMETHODCALLTYPE *GetPixelFormat)(void* This, void* lpDDPixelFormat);
    HRESULT (STDMETHODCALLTYPE *GetSurfaceDesc)(void* This, void* lpDDSurfaceDesc);
    HRESULT (STDMETHODCALLTYPE *Initialize)(void* This, void* lpDD, void* lpDDSurfaceDesc);
    HRESULT (STDMETHODCALLTYPE *IsLost)(void* This);
    HRESULT (STDMETHODCALLTYPE *Lock)(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent);
    HRESULT (STDMETHODCALLTYPE *ReleaseDC)(void* This, HDC hDC);
    HRESULT (STDMETHODCALLTYPE *Restore)(void* This);
    HRESULT (STDMETHODCALLTYPE *SetClipper)(void* This, void* lpDDClipper);
    HRESULT (STDMETHODCALLTYPE *SetColorKey)(void* This, DWORD dwFlags, void* lpDDColorKey);
    HRESULT (STDMETHODCALLTYPE *SetOverlayPosition)(void* This, LONG lX, LONG lY);
    HRESULT (STDMETHODCALLTYPE *SetPalette)(void* This, void* lpDDPalette);
    HRESULT (STDMETHODCALLTYPE *Unlock)(void* This, LPVOID lpSurfaceData);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlay)(void* This, LPRECT lpSrcRect, void* lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, void* lpDDOverlayFx);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlayDisplay)(void* This, DWORD dwFlags);
    HRESULT (STDMETHODCALLTYPE *UpdateOverlayZOrder)(void* This, DWORD dwFlags, void* lpDDSReference);
};

// Updated SDL3 Surface Structure for DirectDraw compatibility
struct SDL3Surface {
    SDL3SurfaceVtbl* lpVtbl;  // Must be first member (COM object layout)
    SDL_Texture* texture;
    void* pixels;
    int width;
    int height;
    int pitch;
    bool locked;
    DWORD lastLockFlags;
};

// SDL3 DirectDraw Structure
struct SDL3DirectDraw {
    void* lpVtbl;  // DirectDraw vtable (not implemented yet)
    bool initialized;
    SDL3Surface* primarySurface;
    SDL3Surface* backSurface;
    SDL3Surface* spriteSurface;
};

// --- DirectDraw Objects ---
static SDL3DirectDraw g_directDraw;
static SDL3Surface g_primarySurface;
static SDL3Surface g_backSurface;
static SDL3Surface g_spriteSurface;
static HWND g_gameWindow = nullptr;

// --- Function Pointers for Originals ---
static BOOL (WINAPI* original_process_input_history)() = nullptr;
static BOOL (WINAPI* original_initialize_game)(HWND windowHandle) = nullptr;
static BOOL (WINAPI* original_initialize_directdraw)(BOOL isFullScreen, HWND windowHandle) = nullptr;
static LRESULT (WINAPI* original_window_proc)(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) = nullptr;
static HWND (WINAPI* original_create_window_ex_a)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) = nullptr;

// --- Forward Declarations ---
void LogMessage(const char* message);
bool InitializeHooks();
void CleanupHooks();
bool InitializeSDL3();
bool CreateSDL3Window(HWND gameHwnd);
bool CreateSDL3Renderer();
bool CreateSDL3Textures();
void SetupDirectDrawReplacement();
void SetupSurfaceVirtualTables();
void RenderFrame();
DWORD WINAPI InitializeThread(LPVOID hModule);

// DirectDraw Surface Method Implementations
HRESULT STDMETHODCALLTYPE Surface_QueryInterface(void* This, REFIID riid, void** ppvObject);
ULONG STDMETHODCALLTYPE Surface_AddRef(void* This);
ULONG STDMETHODCALLTYPE Surface_Release(void* This);
HRESULT STDMETHODCALLTYPE Surface_Lock(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent);
HRESULT STDMETHODCALLTYPE Surface_Unlock(void* This, LPVOID lpSurfaceData);
HRESULT STDMETHODCALLTYPE Surface_Blt(void* This, LPRECT lpDestRect, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx);
HRESULT STDMETHODCALLTYPE Surface_Flip(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags);
HRESULT STDMETHODCALLTYPE Surface_GetSurfaceDesc(void* This, void* lpDDSurfaceDesc);

// Stub implementation for non-critical methods
HRESULT STDMETHODCALLTYPE Surface_Stub(void* This, ...) {
    return DD_OK;
}

// Global vtable instance
static SDL3SurfaceVtbl g_surfaceVtbl;

// Exception handler for debugging crashes
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* pExceptionInfo) {
    LogMessage("*** GAME CRASHED - Exception handler triggered ***");
    
    char buffer[512];
    sprintf_s(buffer, sizeof(buffer), 
        "CRASH INFO: Exception Code: 0x%08X, Address: 0x%08X", 
        pExceptionInfo->ExceptionRecord->ExceptionCode,
        (DWORD)pExceptionInfo->ExceptionRecord->ExceptionAddress);
    LogMessage(buffer);
    
    // Try to get more context
    CONTEXT* ctx = pExceptionInfo->ContextRecord;
    sprintf_s(buffer, sizeof(buffer),
        "REGISTERS: EAX=0x%08X, EBX=0x%08X, ECX=0x%08X, EDX=0x%08X, ESP=0x%08X, EIP=0x%08X",
        ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esp, ctx->Eip);
    LogMessage(buffer);
    
    LogMessage("*** Exception handler complete - allowing normal crash handling ***");
    return EXCEPTION_CONTINUE_SEARCH; // Let Windows handle the crash normally
}

// --- Implementations ---

void LogMessage(const char* message) {
    if (!g_console_stream) return;
    fprintf(g_console_stream, "FM2K HOOK: %s\n", message);
    fflush(g_console_stream);
    
    OutputDebugStringA("FM2K HOOK: ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

HWND WINAPI Hook_CreateWindowExA(
    DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle,
    int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
    HINSTANCE hInstance, LPVOID lpParam) 
{
    LogMessage("Hook_CreateWindowExA triggered!");
    
    // Call original function to create the game window
    HWND gameWindow = original_create_window_ex_a(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
    
    if (gameWindow && lpClassName) {
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "Window created: class='%s', title='%s', hwnd=%p", 
                lpClassName, lpWindowName ? lpWindowName : "NULL", gameWindow);
        LogMessage(buffer);
        
        // Check if this is the main game window
        if (strcmp(lpClassName, "KGT2KGAME") == 0) {
            LogMessage("*** DETECTED MAIN GAME WINDOW - storing for future use ***");
            g_gameWindow = gameWindow;
            
            // Set the game's window handle global at verified address
            HWND* pGameWindowHandle = (HWND*)0x4246F8; // g_hwnd_parent from IDA
            if (!IsBadWritePtr(pGameWindowHandle, sizeof(HWND))) {
                *pGameWindowHandle = gameWindow;
                LogMessage("Updated g_hwnd_parent global with game window handle");
            } else {
                LogMessage("WARNING: Could not access g_hwnd_parent at 0x4246F8");
            }
            
            // Window detection complete - SDL3 takeover will happen in Hook_InitializeGame
            LogMessage("Main game window detected and stored for SDL3 takeover");
        }
    }
    
    return gameWindow;
}

BOOL WINAPI Hook_InitializeGame(HWND windowHandle) {
    LogMessage("Hook_InitializeGame triggered - setting up SDL3 side-by-side!");
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "Game provided window handle: %p", windowHandle);
    LogMessage(buffer);
    
    // Call original game initialization FIRST
    LogMessage("*** Calling original game initialization function FIRST ***");
    BOOL result = original_initialize_game(windowHandle);
    
    char resultBuffer[256];
    sprintf_s(resultBuffer, sizeof(resultBuffer), "Original game initialization returned: %d", result);
    LogMessage(resultBuffer);
    
    // NOW set up SDL3 after game is fully initialized
    if (result && g_gameWindow) {
        LogMessage("Game initialized successfully - setting up SDL3 side-by-side");
        
        if (InitializeSDL3() && CreateSDL3Window(g_gameWindow) && CreateSDL3Renderer() && CreateSDL3Textures()) {
            LogMessage("SDL3 setup complete - running side-by-side with game!");
            
            // Test render a few frames
            for (int i = 1; i <= 3; i++) {
                char testBuffer[256];
                sprintf_s(testBuffer, sizeof(testBuffer), "Initial SDL3 test render #%d", i);
                LogMessage(testBuffer);
                RenderFrame();
                Sleep(100);
            }
        } else {
            LogMessage("SDL3 setup failed - game will run normally");
        }
    }
    
    return result;
}

BOOL WINAPI Hook_InitializeDirectDraw(BOOL isFullScreen, HWND windowHandle) {
    LogMessage("Hook_InitializeDirectDraw triggered - SDL3 DirectDraw replacement");
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "DirectDraw init: fullscreen=%d, windowHandle=%p", isFullScreen, windowHandle);
    LogMessage(buffer);
    
    // Check if game window was detected by CreateWindowExA hook
    if (!g_gameWindow) {
        LogMessage("ERROR: No game window detected yet - cannot proceed with SDL3 setup");
        // Fall back to original DirectDraw
        if (original_initialize_directdraw) {
            return original_initialize_directdraw(isFullScreen, windowHandle);
        }
        return FALSE;
    }
    
    // Now we can perform full SDL3 setup since game window exists
    LogMessage("Game window available - proceeding with full SDL3 setup");
    
    // Phase 1: Create SDL3 window alongside game window
    if (!g_sdlContext.window) {
        if (!CreateSDL3Window(g_gameWindow)) {
            LogMessage("SDL3 window creation failed");
            if (original_initialize_directdraw) {
                return original_initialize_directdraw(isFullScreen, windowHandle);
            }
            return FALSE;
        }
        
        // Hide the original game window and show SDL3 window
        ShowWindow(g_gameWindow, SW_HIDE);
        SDL_ShowWindow(g_sdlContext.window);
        LogMessage("SDL3 window takeover complete - game window hidden, SDL3 window shown");
    }
    
    // Phase 2: Create SDL3 renderer and textures
    if (!CreateSDL3Renderer() || !CreateSDL3Textures()) {
        LogMessage("SDL3 renderer/texture creation failed");
        if (original_initialize_directdraw) {
            return original_initialize_directdraw(isFullScreen, windowHandle);
        }
        return FALSE;
    }
    
    // Phase 3: Let the original DirectDraw function run first (but it will fail)
    LogMessage("*** Calling original DirectDraw initialization first ***");
    BOOL originalResult = FALSE;
    if (original_initialize_directdraw) {
        originalResult = original_initialize_directdraw(isFullScreen, windowHandle);
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "Original DirectDraw initialization returned: %d", originalResult);
        LogMessage(buffer);
    }
    
    // Phase 4: TEMPORARILY DISABLE DirectDraw replacement to avoid crash
    LogMessage("*** TEMPORARILY DISABLING DirectDraw replacement - letting original DirectDraw work ***");
    // SetupDirectDrawReplacement(); // COMMENTED OUT TO PREVENT CRASH
    
    // Phase 5: Start immediate test rendering to verify SDL3 is working
    LogMessage("Starting immediate test rendering...");
    for (int i = 0; i < 5; i++) {
        RenderFrame();
        Sleep(100); // 100ms delay between test frames
    }
    
    LogMessage("SDL3 DirectDraw replacement setup complete!");
    LogMessage("*** CRITICAL: Now allowing game to continue - watching for DirectDraw calls ***");
    return TRUE;
}

LRESULT WINAPI Hook_WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // This hook is important for intercepting window messages.
    return original_window_proc(hwnd, msg, wParam, lParam);
}

BOOL WINAPI Hook_ProcessInputHistory() {
    // This hook is called 60 times per second - perfect for rendering
    static int renderCallCount = 0;
    renderCallCount++;
    
    BOOL result = original_process_input_history();
    
    // If SDL3 is initialized, perform rendering
    if (g_sdlContext.initialized && g_sdlContext.renderer) {
        if (renderCallCount <= 10) {
            char buffer[256];
            sprintf_s(buffer, sizeof(buffer), "Hook_ProcessInputHistory call #%d - starting render", renderCallCount);
            LogMessage(buffer);
        }
        RenderFrame();
    }
    
    return result;
}

void RenderFrame() {
    static int frameCount = 0;
    frameCount++;
    
    if (frameCount <= 5) {
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "RenderFrame() call #%d", frameCount);
        LogMessage(buffer);
    }
    
    // Clear the renderer
    SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdlContext.renderer);
    
    // Render test content to game buffer to verify SDL3 rendering
    if (g_sdlContext.gameBuffer) {
        // Set render target to game buffer  
        SDL_SetRenderTarget(g_sdlContext.renderer, g_sdlContext.gameBuffer);
        
        // Fill game buffer with test pattern
        SDL_SetRenderDrawColor(g_sdlContext.renderer, 64, 128, 255, 255);  // Blue background
        SDL_RenderClear(g_sdlContext.renderer);
        
        // Draw some test rectangles
        SDL_SetRenderDrawColor(g_sdlContext.renderer, 255, 255, 255, 255);  // White
        SDL_FRect testRect = {50, 50, 156, 140};
        SDL_RenderFillRect(g_sdlContext.renderer, &testRect);
        
        SDL_SetRenderDrawColor(g_sdlContext.renderer, 255, 0, 0, 255);  // Red
        SDL_FRect testRect2 = {75, 75, 106, 90};
        SDL_RenderFillRect(g_sdlContext.renderer, &testRect2);
        
        // Reset render target to window
        SDL_SetRenderTarget(g_sdlContext.renderer, nullptr);
        // Render game buffer to window with proper scaling
        int windowWidth, windowHeight;
        SDL_GetWindowSize(g_sdlContext.window, &windowWidth, &windowHeight);
        
        // Calculate scaling to maintain aspect ratio
        float windowAspect = (float)windowWidth / windowHeight;
        float gameAspect = (float)g_sdlContext.gameWidth / g_sdlContext.gameHeight;
        
        SDL_FRect destRect;
        if (windowAspect > gameAspect) {
            // Window is wider - letterbox on sides
            float scale = (float)windowHeight / g_sdlContext.gameHeight;
            destRect.w = g_sdlContext.gameWidth * scale;
            destRect.h = (float)windowHeight;
            destRect.x = (windowWidth - destRect.w) / 2;
            destRect.y = 0;
        } else {
            // Window is taller - letterbox on top/bottom  
            float scale = (float)windowWidth / g_sdlContext.gameWidth;
            destRect.w = (float)windowWidth;
            destRect.h = g_sdlContext.gameHeight * scale;
            destRect.x = 0;
            destRect.y = (windowHeight - destRect.h) / 2;
        }
        
        // Render game buffer to window
        SDL_RenderTexture(g_sdlContext.renderer, g_sdlContext.gameBuffer, nullptr, &destRect);
    }
    
    // Present the frame
    SDL_RenderPresent(g_sdlContext.renderer);
}

bool InitializeSDL3() {
    if (g_sdlContext.initialized) {
        return true;
    }
    
    LogMessage("Initializing SDL3 context...");
    
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "SDL_Init failed: %s", SDL_GetError());
        LogMessage(buffer);
        return false;
    }
    
    g_sdlContext.initialized = true;
    LogMessage("SDL3 context initialized successfully");
    return true;
}

bool CreateSDL3Window(HWND gameHwnd) {
    if (g_sdlContext.window) {
        return true;
    }
    
    if (!gameHwnd) {
        LogMessage("ERROR: No game window handle provided");
        return false;
    }
    
    char buffer[256];
    sprintf_s(buffer, sizeof(buffer), "Creating SDL3 window for game window handle: %p", gameHwnd);
    LogMessage(buffer);
    
    // Get the game window's position and size for reference
    RECT gameRect;
    if (!GetWindowRect(gameHwnd, &gameRect)) {
        sprintf_s(buffer, sizeof(buffer), "ERROR: GetWindowRect failed for handle %p", gameHwnd);
        LogMessage(buffer);
        return false;
    }
    
    int gameWidth = gameRect.right - gameRect.left;
    int gameHeight = gameRect.bottom - gameRect.top;
    
    sprintf_s(buffer, sizeof(buffer), "Game window dimensions: %dx%d at (%d, %d)", gameWidth, gameHeight, gameRect.left, gameRect.top);
    LogMessage(buffer);
    
    // Create a separate SDL3 window for rendering takeover
    LogMessage("Calling SDL_CreateWindow...");
    g_sdlContext.window = SDL_CreateWindow(
        "Fighter Maker 2K - SDL3 Renderer",
        gameWidth,
        gameHeight,
        SDL_WINDOW_RESIZABLE  // Start visible, ready for takeover
    );
    
    if (!g_sdlContext.window) {
        sprintf_s(buffer, sizeof(buffer), "ERROR: SDL_CreateWindow failed: %s", SDL_GetError());
        LogMessage(buffer);
        return false;
    }
    
    sprintf_s(buffer, sizeof(buffer), "SDL3 window created successfully: %p", g_sdlContext.window);
    LogMessage(buffer);
    
    // Position the SDL3 window at the same location as the game window
    LogMessage("Setting SDL3 window position...");
    SDL_SetWindowPosition(g_sdlContext.window, gameRect.left, gameRect.top);
    
    LogMessage("SDL3 window creation and positioning complete");
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
        sprintf_s(buffer, sizeof(buffer), "DirectX 11 renderer failed: %s", SDL_GetError());
        LogMessage(buffer);
        
        // Fall back to default renderer
        LogMessage("Falling back to default renderer...");
        g_sdlContext.renderer = SDL_CreateRenderer(g_sdlContext.window, nullptr);
        
        if (!g_sdlContext.renderer) {
            sprintf_s(buffer, sizeof(buffer), "Default renderer also failed: %s", SDL_GetError());
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

bool CreateSDL3Textures() {
    if (g_sdlContext.gameBuffer) {
        return true;
    }
    
    if (!g_sdlContext.renderer) {
        LogMessage("ERROR: Cannot create textures - no renderer available");
        return false;
    }
    
    // Create game buffer at native resolution (256x240)
    g_sdlContext.gameBuffer = SDL_CreateTexture(
        g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET,
        g_sdlContext.gameWidth,
        g_sdlContext.gameHeight
    );
    
    if (!g_sdlContext.gameBuffer) {
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "Failed to create game buffer: %s", SDL_GetError());
        LogMessage(buffer);
        return false;
    }
    
    SDL_SetTextureScaleMode(g_sdlContext.gameBuffer, SDL_SCALEMODE_NEAREST);
    LogMessage("Game buffer texture created (256x240)");
    
    // Create back buffer texture (640x480)
    g_sdlContext.backBuffer = SDL_CreateTexture(
        g_sdlContext.renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET,
        640, 480
    );
    
    if (g_sdlContext.backBuffer) {
        SDL_SetTextureScaleMode(g_sdlContext.backBuffer, SDL_SCALEMODE_NEAREST);
        LogMessage("Back buffer texture created (640x480)");
    }
    
    return true;
}

void SetupDirectDrawReplacement() {
    LogMessage("Setting up DirectDraw replacement with verified addresses...");
    
    // Set up DirectDraw global variables using IDA-verified addresses
    void** pDirectDraw = (void**)0x424758;       // g_direct_draw
    void** pPrimarySurface = (void**)0x424750;   // g_dd_primary_surface  
    void** pBackBuffer = (void**)0x424754;       // g_dd_back_buffer
    
    if (!IsBadWritePtr(pDirectDraw, sizeof(void*))) {
        *pDirectDraw = &g_directDraw;
        LogMessage("Set DirectDraw pointer at 0x424758");
    }
    
    if (!IsBadWritePtr(pPrimarySurface, sizeof(void*))) {
        g_primarySurface.texture = g_sdlContext.gameBuffer;
        g_primarySurface.width = g_sdlContext.gameWidth;
        g_primarySurface.height = g_sdlContext.gameHeight;
        *pPrimarySurface = &g_primarySurface;
        LogMessage("Set primary surface pointer at 0x424750");
    }
    
    if (!IsBadWritePtr(pBackBuffer, sizeof(void*))) {
        g_backSurface.texture = g_sdlContext.backBuffer;
        g_backSurface.width = 640;
        g_backSurface.height = 480;
        *pBackBuffer = &g_backSurface;
        LogMessage("Set back buffer pointer at 0x424754");
    }
    
    // Set up resolution globals using IDA-verified addresses
    short* pStageWidth = (short*)0x4452B8;   // g_stage_width_pixels
    short* pStageHeight = (short*)0x4452BA;  // g_stage_height_pixels
    int* pDestWidth = (int*)0x447F20;        // g_dest_width
    int* pDestHeight = (int*)0x447F24;       // g_dest_height
    
    if (!IsBadWritePtr(pStageWidth, sizeof(short))) {
        *pStageWidth = 256;
        LogMessage("Set g_stage_width_pixels to 256");
    }
    
    if (!IsBadWritePtr(pStageHeight, sizeof(short))) {
        *pStageHeight = 240;
        LogMessage("Set g_stage_height_pixels to 240");
    }
    
    if (!IsBadWritePtr(pDestWidth, sizeof(int))) {
        *pDestWidth = 256;
        LogMessage("Set g_dest_width to 256");
    }
    
    if (!IsBadWritePtr(pDestHeight, sizeof(int))) {
        *pDestHeight = 240;
        LogMessage("Set g_dest_height to 240");
    }
    
    // Initialize virtual function tables for DirectDraw surfaces
    SetupSurfaceVirtualTables();
    
    LogMessage("DirectDraw SDL3 replacement setup complete");
}

void SetupSurfaceVirtualTables() {
    LogMessage("Setting up DirectDraw surface virtual function tables...");
    
    // Initialize the virtual function table
    g_surfaceVtbl.QueryInterface = Surface_QueryInterface;
    g_surfaceVtbl.AddRef = Surface_AddRef;
    g_surfaceVtbl.Release = Surface_Release;
    g_surfaceVtbl.AddAttachedSurface = (HRESULT (STDMETHODCALLTYPE *)(void*, void*))Surface_Stub;
    g_surfaceVtbl.AddOverlayDirtyRect = (HRESULT (STDMETHODCALLTYPE *)(void*, LPRECT))Surface_Stub;
    g_surfaceVtbl.Blt = Surface_Blt;
    g_surfaceVtbl.BltBatch = (HRESULT (STDMETHODCALLTYPE *)(void*, void*, DWORD, DWORD))Surface_Stub;
    g_surfaceVtbl.BltFast = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, DWORD, void*, LPRECT, DWORD))Surface_Stub;
    g_surfaceVtbl.DeleteAttachedSurface = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, void*))Surface_Stub;
    g_surfaceVtbl.EnumAttachedSurfaces = (HRESULT (STDMETHODCALLTYPE *)(void*, LPVOID, void*))Surface_Stub;
    g_surfaceVtbl.EnumOverlayZOrders = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, LPVOID, void*))Surface_Stub;
    g_surfaceVtbl.Flip = Surface_Flip;
    g_surfaceVtbl.GetAttachedSurface = (HRESULT (STDMETHODCALLTYPE *)(void*, void*, void**))Surface_Stub;
    g_surfaceVtbl.GetBltStatus = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD))Surface_Stub;
    g_surfaceVtbl.GetCaps = (HRESULT (STDMETHODCALLTYPE *)(void*, void*))Surface_Stub;
    g_surfaceVtbl.GetClipper = (HRESULT (STDMETHODCALLTYPE *)(void*, void**))Surface_Stub;
    g_surfaceVtbl.GetColorKey = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, void*))Surface_Stub;
    g_surfaceVtbl.GetDC = (HRESULT (STDMETHODCALLTYPE *)(void*, HDC*))Surface_Stub;
    g_surfaceVtbl.GetFlipStatus = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD))Surface_Stub;
    g_surfaceVtbl.GetOverlayPosition = (HRESULT (STDMETHODCALLTYPE *)(void*, LPLONG, LPLONG))Surface_Stub;
    g_surfaceVtbl.GetPalette = (HRESULT (STDMETHODCALLTYPE *)(void*, void**))Surface_Stub;
    g_surfaceVtbl.GetPixelFormat = (HRESULT (STDMETHODCALLTYPE *)(void*, void*))Surface_Stub;
    g_surfaceVtbl.GetSurfaceDesc = Surface_GetSurfaceDesc;
    g_surfaceVtbl.Initialize = (HRESULT (STDMETHODCALLTYPE *)(void*, void*, void*))Surface_Stub;
    g_surfaceVtbl.IsLost = (HRESULT (STDMETHODCALLTYPE *)(void*))Surface_Stub;
    g_surfaceVtbl.Lock = Surface_Lock;
    g_surfaceVtbl.ReleaseDC = (HRESULT (STDMETHODCALLTYPE *)(void*, HDC))Surface_Stub;
    g_surfaceVtbl.Restore = (HRESULT (STDMETHODCALLTYPE *)(void*))Surface_Stub;
    g_surfaceVtbl.SetClipper = (HRESULT (STDMETHODCALLTYPE *)(void*, void*))Surface_Stub;
    g_surfaceVtbl.SetColorKey = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, void*))Surface_Stub;
    g_surfaceVtbl.SetOverlayPosition = (HRESULT (STDMETHODCALLTYPE *)(void*, LONG, LONG))Surface_Stub;
    g_surfaceVtbl.SetPalette = (HRESULT (STDMETHODCALLTYPE *)(void*, void*))Surface_Stub;
    g_surfaceVtbl.Unlock = Surface_Unlock;
    g_surfaceVtbl.UpdateOverlay = (HRESULT (STDMETHODCALLTYPE *)(void*, LPRECT, void*, LPRECT, DWORD, void*))Surface_Stub;
    g_surfaceVtbl.UpdateOverlayDisplay = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD))Surface_Stub;
    g_surfaceVtbl.UpdateOverlayZOrder = (HRESULT (STDMETHODCALLTYPE *)(void*, DWORD, void*))Surface_Stub;
    
    // Set up surface structures with virtual function table pointers
    g_primarySurface.lpVtbl = &g_surfaceVtbl;
    g_primarySurface.texture = g_sdlContext.gameBuffer;
    g_primarySurface.width = g_sdlContext.gameWidth;
    g_primarySurface.height = g_sdlContext.gameHeight;
    g_primarySurface.locked = false;
    g_primarySurface.pixels = nullptr;
    
    g_backSurface.lpVtbl = &g_surfaceVtbl;
    g_backSurface.texture = g_sdlContext.backBuffer;
    g_backSurface.width = 640;
    g_backSurface.height = 480;
    g_backSurface.locked = false;
    g_backSurface.pixels = nullptr;
    
    g_spriteSurface.lpVtbl = &g_surfaceVtbl;
    g_spriteSurface.texture = nullptr; // Will create separate sprite texture
    g_spriteSurface.width = 256;
    g_spriteSurface.height = 256;
    g_spriteSurface.locked = false;
    g_spriteSurface.pixels = nullptr;
    
    LogMessage("DirectDraw surface virtual function tables initialized successfully");
}

// DirectDraw Surface Method Implementations
HRESULT STDMETHODCALLTYPE Surface_QueryInterface(void* This, REFIID riid, void** ppvObject) {
    LogMessage("Surface_QueryInterface called");
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Surface_AddRef(void* This) {
    return 1;
}

ULONG STDMETHODCALLTYPE Surface_Release(void* This) {
    return 0;
}

HRESULT STDMETHODCALLTYPE Surface_Lock(void* This, LPRECT lpDestRect, void* lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent) {
    LogMessage("Surface_Lock called - providing fake surface data");
    SDL3Surface* surface = (SDL3Surface*)This;
    
    if (!surface) {
        LogMessage("ERROR: Surface_Lock called with null surface");
        return DDERR_INVALIDPARAMS;
    }
    
    if (surface->locked) {
        LogMessage("WARNING: Surface already locked");
        return DDERR_SURFACEBUSY;
    }
    
    // Set up fake surface description for the game
    struct FakeDDSURFACEDESC {
        DWORD dwSize;
        DWORD dwFlags;
        DWORD dwHeight;
        DWORD dwWidth;
        LONG lPitch;
        LPVOID lpSurface;
        // ... other members the game might check
    };
    
    FakeDDSURFACEDESC* desc = (FakeDDSURFACEDESC*)lpDDSurfaceDesc;
    if (desc) {
        desc->dwSize = sizeof(FakeDDSURFACEDESC);
        desc->dwFlags = 0x00001007; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_LPSURFACE
        desc->dwHeight = surface->height;
        desc->dwWidth = surface->width;
        desc->lPitch = surface->width; // Assume 8-bit pixels for now
        
        // Allocate a temporary pixel buffer for the game to write to
        if (!surface->pixels) {
            surface->pixels = malloc(surface->width * surface->height);
            surface->pitch = surface->width;
        }
        desc->lpSurface = surface->pixels;
        
        char buffer[256];
        sprintf_s(buffer, sizeof(buffer), "Surface_Lock: %dx%d, pitch=%d, buffer=%p", 
                surface->width, surface->height, surface->pitch, surface->pixels);
        LogMessage(buffer);
    }
    
    surface->locked = true;
    surface->lastLockFlags = dwFlags;
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Unlock(void* This, LPVOID lpSurfaceData) {
    LogMessage("Surface_Unlock called - copying data to SDL3 texture");
    SDL3Surface* surface = (SDL3Surface*)This;
    
    if (!surface || !surface->locked) {
        LogMessage("ERROR: Surface_Unlock called on unlocked surface");
        return DDERR_NOTLOCKED;
    }
    
    // Copy pixel data from temporary buffer to SDL3 texture
    if (surface->pixels && surface->texture && g_sdlContext.renderer) {
        void* sdlPixels;
        int sdlPitch;
        
        if (SDL_LockTexture(surface->texture, NULL, &sdlPixels, &sdlPitch) == 0) {
            // For now, just copy the raw data (8-bit to 32-bit conversion needed)
            // This is where palette conversion would happen
            memset(sdlPixels, 0x80, sdlPitch * surface->height); // Gray for now
            SDL_UnlockTexture(surface->texture);
            LogMessage("Game pixel data copied to SDL3 texture");
        }
    }
    
    surface->locked = false;
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Blt(void* This, LPRECT lpDestRect, void* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx) {
    LogMessage("Surface_Blt called - SDL3 texture blitting");
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_Flip(void* This, void* lpDDSurfaceTargetOverride, DWORD dwFlags) {
    LogMessage("Surface_Flip called - triggering SDL3 present");
    // This is where we'd trigger our SDL3 rendering
    if (g_sdlContext.initialized && g_sdlContext.renderer) {
        RenderFrame();
    }
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE Surface_GetSurfaceDesc(void* This, void* lpDDSurfaceDesc) {
    LogMessage("Surface_GetSurfaceDesc called");
    return DD_OK;
}

// Surface_Stub already defined above

bool InitializeHooks() {
    if (g_hooks_initialized) {
        LogMessage("Hooks already initialized.");
        return true;
    }

    LogMessage("Initializing MinHook...");
    if (MH_Initialize() != MH_OK) {
        LogMessage("ERROR: MH_Initialize failed.");
        return false;
    }

    LogMessage("Creating minimal hooks for debugging...");

    // MINIMAL HOOK STRATEGY: Start with only essential hooks to let game initialize
    
    // Hook 1: Input processing (working baseline)
    if (MH_CreateHook((LPVOID)0x4025A0, (LPVOID)Hook_ProcessInputHistory, (LPVOID*)&original_process_input_history) != MH_OK) {
        LogMessage("ERROR: Failed to create Hook_ProcessInputHistory.");
        return false;
    } else {
        LogMessage("SUCCESS: Created Hook_ProcessInputHistory");
    }

    // Hook 2: Window creation (essential for detecting main window)
    if (MH_CreateHookApi(L"user32", "CreateWindowExA", (LPVOID)Hook_CreateWindowExA, (LPVOID*)&original_create_window_ex_a) != MH_OK) {
        LogMessage("ERROR: Failed to create Hook_CreateWindowExA.");
        return false;
    } else {
        LogMessage("SUCCESS: Created Hook_CreateWindowExA");
    }

    // Hook 3: DirectDraw initialization (DISABLED to avoid crash)
    /*
    if (MH_CreateHook((LPVOID)0x404980, (LPVOID)Hook_InitializeDirectDraw, (LPVOID*)&original_initialize_directdraw) != MH_OK) {
        LogMessage("ERROR: Failed to create Hook_InitializeDirectDraw.");
        return false;
    } else {
        LogMessage("SUCCESS: Created Hook_InitializeDirectDraw");
    }
    */
    LogMessage("DISABLED: Hook_InitializeDirectDraw to prevent crash");

    // Hook 4: Game initialization (for full SDL3 setup)
    if (MH_CreateHook((LPVOID)0x4056C0, (LPVOID)Hook_InitializeGame, (LPVOID*)&original_initialize_game) != MH_OK) {
        LogMessage("ERROR: Failed to create Hook_InitializeGame.");
        return false;
    } else {
        LogMessage("SUCCESS: Created Hook_InitializeGame");
    }

    // Hook 5: Window procedure (for message forwarding)
    if (MH_CreateHook((LPVOID)0x405F50, (LPVOID)Hook_WindowProc, (LPVOID*)&original_window_proc) != MH_OK) {
        LogMessage("ERROR: Failed to create Hook_WindowProc.");
        return false;
    } else {
        LogMessage("SUCCESS: Created Hook_WindowProc");
    }

    LogMessage("Enabling hooks...");
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        LogMessage("ERROR: MH_EnableHook failed.");
        return false;
    }

    LogMessage("Hooks initialized and enabled successfully.");
    g_hooks_initialized = true;
    return true;
}

void CleanupHooks() {
    if (!g_hooks_initialized) return;
    LogMessage("Disabling and removing all hooks...");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_hooks_initialized = false;
    LogMessage("Hooks cleaned up.");
}

DWORD WINAPI InitializeThread(LPVOID hModule) {
    // --- Phase 1: Setup Logging ---
    AllocConsole();
    freopen_s(&g_console_stream, "CONOUT$", "w", stdout);
    LogMessage("Initialization thread started.");
    
    // Install crash handler for debugging
    SetUnhandledExceptionFilter(CrashHandler);
    LogMessage("Crash handler installed for debugging");

    // --- Phase 2: Initialize Hooks ---
    if (!InitializeHooks()) {
        LogMessage("Hook initialization failed. Aborting.");
        // We still need to signal the event so the launcher doesn't hang forever
        if (g_init_event) {
            BOOL result = SetEvent(g_init_event);
            char signal_msg[256];
            sprintf_s(signal_msg, sizeof(signal_msg), "SetEvent(failure path) result: %d, handle: %p", result, g_init_event);
            LogMessage(signal_msg);
        }
        if (g_console_stream) fclose(g_console_stream);
        FreeConsole();
        return 1; // Failure
    }

    // --- Phase 3: Signal Success & Wait ---
    g_dll_initialized = true;
    LogMessage("Initialization complete. Signaling launcher...");
    if (g_init_event) {
        BOOL result = SetEvent(g_init_event);
        char signal_msg[256];
        sprintf_s(signal_msg, sizeof(signal_msg), "SetEvent(success path) result: %d, handle: %p, error: %lu", result, g_init_event, GetLastError());
        LogMessage(signal_msg);
    } else {
        LogMessage("ERROR: g_init_event is NULL, cannot signal launcher");
    }
    
    // A small delay to ensure the launcher has time to process the event
    // before this thread potentially exits in future revisions.
    LogMessage("Initialization thread finished. Waiting 5 seconds before exiting.");
    Sleep(5000);

    return 0; // Success
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
        {
            // Create the event that the launcher will wait on.
            // Use CreateEventW with wide string to match launcher exactly.
            g_init_event = CreateEventW(nullptr, TRUE, FALSE, L"FM2KHook_Initialized");

            if (g_init_event == NULL) {
                // Log the error before failing
                DWORD error = GetLastError();
                char error_msg[256];
                sprintf_s(error_msg, sizeof(error_msg), "Failed to create event. Error: %lu", error);
                OutputDebugStringA("FM2K HOOK: ");
                OutputDebugStringA(error_msg);
                OutputDebugStringA("\n");
                return FALSE; // Cannot proceed without event
            } else {
                // Log successful event creation
                char success_msg[256];
                sprintf_s(success_msg, sizeof(success_msg), "Successfully created event handle: %p", g_init_event);
                OutputDebugStringA("FM2K HOOK: ");
                OutputDebugStringA(success_msg);
                OutputDebugStringA("\n");
            }
            
            // To prevent deadlocks, do not do complex work here.
            // Create a separate thread for initialization.
            DisableThreadLibraryCalls(hModule);
            HANDLE hThread = CreateThread(NULL, 0, InitializeThread, hModule, 0, NULL);
            if (hThread == NULL) {
                CloseHandle(g_init_event);
                return FALSE; // Failed to create init thread
            }
            CloseHandle(hThread); // We don't need to manage the thread, so close handle.
            break;
        }

        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;

        case DLL_PROCESS_DETACH:
        {
            CleanupHooks();
            if (g_init_event) {
                CloseHandle(g_init_event);
                g_init_event = nullptr;
            }
            if (g_console_stream) {
                LogMessage("Process detaching. Closing console.");
                fclose(g_console_stream);
                FreeConsole();
            }
            break;
        }
    }
    return TRUE;
}