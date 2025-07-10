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
static uint32_t hook_call_count = 0;
static HWND g_game_window = nullptr;

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
    
    LogMessage("Hijacking game window with SDL3...");
    
    // Create SDL3 window using the same size as game window
    RECT gameRect;
    GetWindowRect(gameHwnd, &gameRect);
    int width = gameRect.right - gameRect.left;
    int height = gameRect.bottom - gameRect.top;
    
    g_sdlContext.window = SDL_CreateWindow(
        "FM2K Rollback (SDL3)",
        width,
        height,
        SDL_WINDOW_RESIZABLE
    );
    
    if (!g_sdlContext.window) {
        char buffer[256];
        sprintf(buffer, "SDL_CreateWindow failed: %s", SDL_GetError());
        LogMessage(buffer);
        return false;
    }
    
    // Get the SDL3 window's HWND and replace the game's window handle
    HWND sdlHwnd = (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(g_sdlContext.window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        NULL
    );
    
    if (sdlHwnd) {
        g_game_window = sdlHwnd;
        
        // Hide the original game window and show the SDL3 window  
        ShowWindow(gameHwnd, SW_HIDE);
        SetWindowPos(sdlHwnd, HWND_TOP, gameRect.left, gameRect.top, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        
        char buffer[256];
        sprintf(buffer, "Window hijacking successful - SDL3 HWND: %p, Game HWND: %p", sdlHwnd, gameHwnd);
        LogMessage(buffer);
        return true;
    }
    
    LogMessage("Failed to get SDL3 window HWND");
    return false;
}

bool CreateSDL3Renderer() {
    if (g_sdlContext.renderer) {
        return true;
    }
    
    // Force DirectX 11 renderer for optimal performance
    SDL_PropertiesID rendererProps = SDL_CreateProperties();
    SDL_SetStringProperty(rendererProps, SDL_PROP_RENDERER_CREATE_NAME_STRING, "direct3d11");
    SDL_SetNumberProperty(rendererProps, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, 1);
    g_sdlContext.renderer = SDL_CreateRendererWithProperties(rendererProps);
    SDL_DestroyProperties(rendererProps);
    
    if (!g_sdlContext.renderer) {
        char buffer[256];
        sprintf(buffer, "SDL_CreateRenderer failed: %s", SDL_GetError());
        LogMessage(buffer);
        return false;
    }
    
    LogMessage("SDL3 DirectX 11 renderer created successfully");
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
        SDL_TEXTUREACCESS_TARGET,
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
    if (!g_sdlContext.renderer || !g_sdlContext.gameBuffer) {
        return;
    }
    
    // Set render target to window (nullptr = default framebuffer)
    SDL_SetRenderTarget(g_sdlContext.renderer, nullptr);
    SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_sdlContext.renderer);
    
    // Get actual window dimensions
    int actualWindowWidth, actualWindowHeight;
    SDL_GetWindowSize(g_sdlContext.window, &actualWindowWidth, &actualWindowHeight);
    
    // Calculate scaling to maintain aspect ratio and center the game
    float windowAspect = (float)actualWindowWidth / actualWindowHeight;
    float gameAspect = (float)g_sdlContext.gameWidth / g_sdlContext.gameHeight;  // 256/240 = 1.067
    
    SDL_FRect destRect;
    if (windowAspect > gameAspect) {
        // Window is wider - letterbox on sides
        float scale = (float)actualWindowHeight / g_sdlContext.gameHeight;
        destRect.w = g_sdlContext.gameWidth * scale;
        destRect.h = (float)actualWindowHeight;
        destRect.x = (actualWindowWidth - destRect.w) / 2;
        destRect.y = 0;
    } else {
        // Window is taller - letterbox on top/bottom
        float scale = (float)actualWindowWidth / g_sdlContext.gameWidth;
        destRect.w = (float)actualWindowWidth;
        destRect.h = g_sdlContext.gameHeight * scale;
        destRect.x = 0;
        destRect.y = (actualWindowHeight - destRect.h) / 2;
    }
    
    // Render the scaled game buffer to window using NEAREST_NEIGHBOR filtering
    SDL_RenderTexture(g_sdlContext.renderer, g_sdlContext.gameBuffer, nullptr, &destRect);
    SDL_RenderPresent(g_sdlContext.renderer);
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

// DirectDraw compatibility structures
struct DummyDirectDraw {
    void* vtable;
};

struct DummySurface {
    void* vtable;
    SDL_Texture* texture;
    int width;
    int height;
    int pitch;
    void* pixels;
};

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
        if (InitializeSDL3Context() && CreateSDL3Renderer() && CreateGameTextures()) {
            g_sdl3_initialized = true;
            LogMessage("SDL3 initialized during DirectDraw setup");
        } else {
            LogMessage("Failed to initialize SDL3, falling back to original DirectDraw");
            if (original_initialize_directdraw) {
                return original_initialize_directdraw(isFullScreen, windowHandle);
            }
            return 0;
        }
    }
    
    // Create additional SDL3 textures for DirectDraw surface replacements
    if (!g_sdlContext.backBuffer) {
        g_sdlContext.backBuffer = SDL_CreateTexture(
            g_sdlContext.renderer,
            SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET,
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
        SDL_TEXTUREACCESS_TARGET,
        256, 256
    );
    if (spriteTexture) {
        SDL_SetTextureScaleMode(spriteTexture, SDL_SCALEMODE_NEAREST);
        LogMessage("Created SDL3 sprite texture (256x256)");
    }
    
    // Set up dummy DirectDraw interfaces for game compatibility
    // These pointers need to match the addresses that the game expects
    
    // From research docs - DirectDraw global variables
    // g_direct_draw @ 0x424758
    // g_dd_primary_surface @ 0x424750  
    // g_dd_back_buffer @ 0x424754
    
    // Point game's DirectDraw globals to our dummy structures
    void** pDirectDraw = (void**)0x424758;
    void** pPrimarySurface = (void**)0x424750;
    void** pBackBuffer = (void**)0x424754;
    
    if (pDirectDraw) {
        *pDirectDraw = &g_dummyDirectDraw;
        LogMessage("Set DirectDraw pointer at 0x424758");
    }
    
    if (pPrimarySurface) {
        g_primarySurface.texture = g_sdlContext.gameBuffer;
        g_primarySurface.width = g_sdlContext.gameWidth;
        g_primarySurface.height = g_sdlContext.gameHeight;
        *pPrimarySurface = &g_primarySurface;
        LogMessage("Set primary surface pointer at 0x424750");
    }
    
    if (pBackBuffer) {
        g_backSurface.texture = g_sdlContext.backBuffer;
        g_backSurface.width = 640;
        g_backSurface.height = 480;
        *pBackBuffer = &g_backSurface;
        LogMessage("Set back buffer pointer at 0x424754");
    }
    
    // Set up critical game variables for resolution
    // From research docs - these control the game's rendering resolution
    int* pMaxWidth = (int*)0x6B3060;
    int* pMaxHeight = (int*)0x6B305C;
    int* pBitCount = (int*)0x6B3058;
    
    if (pMaxWidth) {
        *pMaxWidth = 256;  // Native game width
        LogMessage("Set max width to 256");
    }
    if (pMaxHeight) {
        *pMaxHeight = 240; // Native game height  
        LogMessage("Set max height to 240");
    }
    if (pBitCount) {
        *pBitCount = 8;    // 8-bit palettized
        LogMessage("Set bit count to 8");
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
    if (lpWindowName && (strstr(lpWindowName, "Moon Lights") || strstr(lpWindowName, "Fighter Maker") || strstr(lpWindowName, "WonderfulWorld"))) {
        LogMessage("Detected main game window - initiating SDL3 hijack");
        
        // Initialize SDL3 if not already done
        if (!g_sdl3_initialized) {
            if (InitializeSDL3Context()) {
                if (HijackGameWindow(gameWindow) && CreateSDL3Renderer() && CreateGameTextures()) {
                    g_sdl3_initialized = true;
                    LogMessage("SDL3 window hijacking completed successfully");
                } else {
                    LogMessage("SDL3 window hijacking failed");
                }
            }
        }
    }
    
    return gameWindow;
}

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
            
            // Hook 3: initialize_directdraw_mode function at 0x404980 (for DirectDraw replacement)
            void* target3 = (void*)0x404980;
            LogMessage("Creating initialize_directdraw hook...");
            MH_STATUS status3 = MH_CreateHook(target3, (void*)Hook_InitializeDirectDraw, (void**)&original_initialize_directdraw);
            if (status3 != MH_OK) {
                char buffer[256];
                sprintf(buffer, "WARNING: Failed to create initialize_directdraw hook, status %d", status3);
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