#include "sdl3_context.hpp"
#include "../../argentum.hpp"
#include "../../input/core/input_manager.hpp"
#include "../../simple_input_hooks.h"  // Include for UpdateInputFromWindowMessage
#include <SDL3/SDL.h>
#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cmath>  // For fabs and other math functions

namespace argentum::hooks {
    
    SDL3Context g_sdlContext = {};
    
    // SDL3 event integration variables
    static HHOOK g_messageHook = nullptr;
    static HMODULE g_user32Module = nullptr;
    static int (WINAPI *o_GetMessageA)(LPMSG, HWND, UINT, UINT) = nullptr;
    static BOOL (WINAPI *o_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT) = nullptr;
    
    // Window procedure handling
    static WNDPROC g_originalSDLWindowProc = nullptr;
    static const uintptr_t HANDLE_MENU_CALL_HOTKEYS_ADDR = 0x406390; // handleMenuCallHotkeys function address
    
    // Game's window procedure function pointer
    typedef LRESULT (__stdcall *HandleMenuCallHotkeysFunc)(HWND, UINT, WPARAM, LPARAM);
    static HandleMenuCallHotkeysFunc g_handleMenuCallHotkeys = (HandleMenuCallHotkeysFunc)HANDLE_MENU_CALL_HOTKEYS_ADDR;
    
    // Original game window handle - this is what the game logic expects to work with
    static HWND g_originalGameWindow = nullptr;
    
    // Custom window procedure that forwards messages to the game's window procedure
    LRESULT CALLBACK SDL3GameWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        // Forward ONLY key messages to the game's window procedure
        // Let mouse messages go through SDL3's original window procedure for ImGui
        switch (uMsg) {
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                // Reduced spam: Only log first few messages or special keys
                static int keyMsgCount = 0;
                keyMsgCount++;
                // DISABLED for performance: SDL3 window forwarding debug causes FPS drops
                // if (keyMsgCount <= 5 || wParam == VK_F9 || wParam == VK_ESCAPE) {
                //     printf("SDL3 WINDOW: Forwarding key message %u (wParam=%u) to game window procedure\n", uMsg, (unsigned int)wParam);
                // }
                
                // CRITICAL: Let SDL3 process the keyboard event FIRST so SDL_GetKeyboardState() works
                if (g_originalSDLWindowProc) {
                    CallWindowProc(g_originalSDLWindowProc, hWnd, uMsg, wParam, lParam);
                }
                
                // CRITICAL: Update input state for our new input system
                UpdateInputFromWindowMessage(uMsg, wParam);
                
                // Debug: Show virtual key code for common keys (heavily reduced spam)
                static int keyDebugCount = 0;
                if (uMsg == WM_KEYDOWN && keyDebugCount < 3) {
                    keyDebugCount++;
                    switch (wParam) {
                        case VK_ESCAPE: printf("  -> ESC key (VK_ESCAPE = %d)\n", VK_ESCAPE); break;
                        case VK_F3: printf("  -> F3 key (VK_F3 = %d)\n", VK_F3); break;
                        case VK_F4: printf("  -> F4 key (VK_F4 = %d)\n", VK_F4); break;
                        case VK_F8: printf("  -> F8 key (VK_F8 = %d)\n", VK_F8); break;
                        case VK_F9: printf("  -> F9 key (VK_F9 = %d)\n", VK_F9); break;
                        case 'W': case 'A': case 'S': case 'D':
                        case 'Z': case 'X': case 'C':
                        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
                        case 'U': case 'I': case 'O':
                            printf("  -> Game input key: %d (debug suppressed after 3 times)\n", (int)wParam); 
                            break;
                        default: 
                            if (keyDebugCount == 1) printf("  -> Other key: %d (debug suppressed after 3 times)\n", (int)wParam); 
                            break;
                    }
                }
                
                // THEN forward to the game's window procedure
                return g_handleMenuCallHotkeys(hWnd, uMsg, wParam, lParam);
                
            case WM_CHAR:
            case WM_SYSCHAR:
                // Reduced spam: Only log first few messages
                static int charMsgCount = 0;
                charMsgCount++;
                // DISABLED for performance: SDL3 window forwarding debug causes FPS drops
                // if (charMsgCount <= 5) {
                //     printf("SDL3 WINDOW: Forwarding key message %u (wParam=%u) to game window procedure\n", uMsg, (unsigned int)wParam);
                // }
                
                // Let SDL3 process character events first
                if (g_originalSDLWindowProc) {
                    CallWindowProc(g_originalSDLWindowProc, hWnd, uMsg, wParam, lParam);
                }
                
                // CRITICAL: Pass the SDL3 window handle - the game logic needs the actual visible window
                return g_handleMenuCallHotkeys(hWnd, uMsg, wParam, lParam);
                
            case WM_ACTIVATEAPP:
            case WM_DESTROY:
            case WM_CLOSE:
                // DISABLED for performance: SDL3 window forwarding debug causes FPS drops
                // printf("SDL3 WINDOW: Forwarding window message %u to game window procedure\n", uMsg);
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
        
        // Only peek at critical events for window management, don't consume them
        SDL_Event event;
        if (SDL_PeepEvents(&event, 1, SDL_PEEKEVENT, SDL_EVENT_QUIT, SDL_EVENT_QUIT) > 0) {
            if (event.type == SDL_EVENT_QUIT) {
                printf("SDL3 EVENT: Quit event detected\n");
                // Don't consume it, let the game handle it
            }
        }
    }
    
    bool IsSDL3KeyPressed(int scancode) {
        if (!g_sdlContext.initialized) return false;
        
        // Don't pump events here - let UpdateSDL3Events handle it centrally
        // This prevents excessive event pumping which can cause jitter
        
        // Primary method: Use SDL3 keyboard state
        const bool* keystate = SDL_GetKeyboardState(NULL);
        bool sdl_pressed = keystate && keystate[scancode];
        
        // Fallback method: Use Win32 GetAsyncKeyState for global hotkeys (works even when window doesn't have focus)
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
            case SDL_SCANCODE_1:
                win32_pressed = (GetAsyncKeyState('1') & 0x8000) != 0;
                break;
            case SDL_SCANCODE_2:
                win32_pressed = (GetAsyncKeyState('2') & 0x8000) != 0;
                break;
            case SDL_SCANCODE_3:
                win32_pressed = (GetAsyncKeyState('3') & 0x8000) != 0;
                break;
            case SDL_SCANCODE_4:
                win32_pressed = (GetAsyncKeyState('4') & 0x8000) != 0;
                break;
            case SDL_SCANCODE_5:
                win32_pressed = (GetAsyncKeyState('5') & 0x8000) != 0;
                break;
            case SDL_SCANCODE_6:
                win32_pressed = (GetAsyncKeyState('6') & 0x8000) != 0;
                break;
            case SDL_SCANCODE_7:
                win32_pressed = (GetAsyncKeyState('7') & 0x8000) != 0;
                break;
            case SDL_SCANCODE_RETURN:
                win32_pressed = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
                break;
        }
        
        bool pressed = sdl_pressed || win32_pressed;
        
        // Debug output for hotkeys (only log state changes to avoid spam)
        static int last_pressed_key = -1;
        static bool last_was_pressed = false;
        
        if (pressed && (!last_was_pressed || scancode != last_pressed_key)) {
            last_pressed_key = scancode;
            last_was_pressed = true;
        } else if (!pressed && last_was_pressed && scancode == last_pressed_key) {
            last_was_pressed = false;
        }
        
        return pressed;
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
        
        // Get current window dimensions and position
        int windowW, windowH, windowX, windowY;
        SDL_GetWindowSize(g_sdlContext.window, &windowW, &windowH);
        SDL_GetWindowPosition(g_sdlContext.window, &windowX, &windowY);
        
        // Get display info for comparison
        SDL_DisplayID displayID = SDL_GetDisplayForWindow(g_sdlContext.window);
        const SDL_DisplayMode* displayMode = SDL_GetCurrentDisplayMode(displayID);
        
        // Determine if we're in borderless fullscreen
        bool borderlessFullscreen = false;
        if (displayMode && !nativeFullscreen) {
            borderlessFullscreen = (windowW == displayMode->w && windowH == displayMode->h && 
                                   windowX == 0 && windowY == 0);
        }
        
        bool anyFullscreen = nativeFullscreen || borderlessFullscreen;
        
        // Debug: Show comprehensive current window state
        printf("SDL3 FULLSCREEN: === CURRENT STATE ANALYSIS ===\n");
        printf("SDL3 FULLSCREEN: Window flags = 0x%08X\n", flags);
        printf("SDL3 FULLSCREEN: Window size = %dx%d at (%d,%d)\n", windowW, windowH, windowX, windowY);
        if (displayMode) {
            printf("SDL3 FULLSCREEN: Display size = %dx%d\n", displayMode->w, displayMode->h);
        }
        printf("SDL3 FULLSCREEN: Native fullscreen = %s\n", nativeFullscreen ? "YES" : "NO");
        printf("SDL3 FULLSCREEN: Borderless fullscreen = %s\n", borderlessFullscreen ? "YES" : "NO");
        printf("SDL3 FULLSCREEN: Any fullscreen = %s\n", anyFullscreen ? "YES" : "NO");
        printf("SDL3 FULLSCREEN: Toggling to %s\n", anyFullscreen ? "WINDOWED" : "FULLSCREEN");
        
        if (anyFullscreen) {
            // Switch from any fullscreen mode to windowed mode
            printf("SDL3 FULLSCREEN: === SWITCHING TO WINDOWED ===\n");
            
            // First, disable native fullscreen if active
            if (nativeFullscreen) {
                printf("SDL3 FULLSCREEN: Disabling native fullscreen...\n");
                if (!SDL_SetWindowFullscreen(g_sdlContext.window, false)) {
                    printf("SDL3 FULLSCREEN: ? Failed to disable native fullscreen: %s\n", SDL_GetError());
                    return false;
                }
            }
            
            // Force window to proper windowed state
            printf("SDL3 FULLSCREEN: Restoring windowed properties...\n");
            SDL_SetWindowBordered(g_sdlContext.window, true);  // Ensure borders are shown
            SDL_SetWindowSize(g_sdlContext.window, 640, 480);
            SDL_SetWindowPosition(g_sdlContext.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            SDL_RestoreWindow(g_sdlContext.window);  // Ensure not minimized/maximized
            SDL_RaiseWindow(g_sdlContext.window);    // Bring to front
            
            // Wait for changes to take effect
            SDL_SyncWindow(g_sdlContext.window);
            
            // Update context
            g_sdlContext.isFullscreen = false;
            g_sdlContext.windowWidth = 640;
            g_sdlContext.windowHeight = 480;
            
            printf("SDL3 FULLSCREEN: ? Successfully switched to windowed mode (640x480)\n");
        } else {
            // Switch from windowed to fullscreen mode
            printf("SDL3 FULLSCREEN: === SWITCHING TO FULLSCREEN ===\n");
            
            if (displayMode) {
                printf("SDL3 FULLSCREEN: Target display mode = %dx%d @ %dHz\n", 
                       displayMode->w, displayMode->h, (int)displayMode->refresh_rate);
            } else {
                printf("SDL3 FULLSCREEN: ?? Could not get display mode info\n");
                return false;
            }
            
            // Try native fullscreen first
            printf("SDL3 FULLSCREEN: Attempting native fullscreen...\n");
            bool fullscreenSuccess = SDL_SetWindowFullscreen(g_sdlContext.window, true);
            
            if (fullscreenSuccess) {
                printf("SDL3 FULLSCREEN: ? Native fullscreen successful\n");
            } else {
                printf("SDL3 FULLSCREEN: ?? Native fullscreen failed: %s\n", SDL_GetError());
                printf("SDL3 FULLSCREEN: Falling back to borderless fullscreen...\n");
                
                // Fallback: borderless fullscreen 
                SDL_SetWindowBordered(g_sdlContext.window, false);
                SDL_SetWindowSize(g_sdlContext.window, displayMode->w, displayMode->h);
                SDL_SetWindowPosition(g_sdlContext.window, 0, 0);
                SDL_MaximizeWindow(g_sdlContext.window);  // Try to ensure it covers everything
                
                printf("SDL3 FULLSCREEN: ? Borderless fullscreen configured\n");
            }
            
            // Wait for changes to take effect
            SDL_SyncWindow(g_sdlContext.window);
            
            // Update context
            g_sdlContext.windowWidth = displayMode->w;
            g_sdlContext.windowHeight = displayMode->h;
            g_sdlContext.isFullscreen = true;
            
            printf("SDL3 FULLSCREEN: ? Successfully switched to fullscreen mode (%dx%d)\n", 
                   g_sdlContext.windowWidth, g_sdlContext.windowHeight);
        }
        
        // Final verification
        Uint32 finalFlags = SDL_GetWindowFlags(g_sdlContext.window);
        bool finalFullscreen = (finalFlags & SDL_WINDOW_FULLSCREEN) != 0;
        int finalW, finalH;
        SDL_GetWindowSize(g_sdlContext.window, &finalW, &finalH);
        
        printf("SDL3 FULLSCREEN: === FINAL STATE ===\n");
        printf("SDL3 FULLSCREEN: Window size = %dx%d\n", finalW, finalH);
        printf("SDL3 FULLSCREEN: Native fullscreen = %s\n", finalFullscreen ? "YES" : "NO");
        printf("SDL3 FULLSCREEN: Toggle operation completed\n");
        
        return true;
    }
    
    // Debug function to test all hotkeys
    void TestAllHotkeys() {
        static DWORD lastTest = 0;
        DWORD currentTime = GetTickCount();
        
        // Test every 1000ms to avoid spam
        if (currentTime - lastTest < 1000) return;
        lastTest = currentTime;
        
        printf("=== SDL3 HOTKEY TEST ===\n");
        printf("F1 pressed: %s\n", IsSDL3KeyPressed(SDL_SCANCODE_F1) ? "YES" : "NO");
        printf("F2 pressed: %s\n", IsSDL3KeyPressed(SDL_SCANCODE_F2) ? "YES" : "NO");
        printf("F3 pressed: %s\n", IsSDL3KeyPressed(SDL_SCANCODE_F3) ? "YES" : "NO");
        printf("F4 pressed: %s\n", IsSDL3KeyPressed(SDL_SCANCODE_F4) ? "YES" : "NO");
        printf("Alt+Enter pressed: %s\n", IsAltEnterPressed() ? "YES" : "NO");
        
        // Also test Win32 directly
        printf("Win32 F1: %s\n", (GetAsyncKeyState(VK_F1) & 0x8000) ? "YES" : "NO");
        printf("Win32 F2: %s\n", (GetAsyncKeyState(VK_F2) & 0x8000) ? "YES" : "NO");
        printf("Win32 F3: %s\n", (GetAsyncKeyState(VK_F3) & 0x8000) ? "YES" : "NO");
        printf("Win32 F4: %s\n", (GetAsyncKeyState(VK_F4) & 0x8000) ? "YES" : "NO");
        printf("Win32 Alt: %s, Enter: %s\n", 
               (GetAsyncKeyState(VK_MENU) & 0x8000) ? "YES" : "NO",
               (GetAsyncKeyState(VK_RETURN) & 0x8000) ? "YES" : "NO");
        
        // Show current fullscreen state
        if (g_sdlContext.window) {
            Uint32 flags = SDL_GetWindowFlags(g_sdlContext.window);
            bool isFullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
            printf("Current mode: %s (%dx%d)\n", 
                   isFullscreen ? "FULLSCREEN" : "WINDOWED",
                   g_sdlContext.windowWidth, g_sdlContext.windowHeight);
        }
        printf("========================\n");
        
        // Check for F5 to trigger renderer backend check
        static bool lastF5State = false;
        bool currentF5State = IsSDL3KeyPressed(SDL_SCANCODE_F5);
        if (currentF5State && !lastF5State) {
            printf("F5 HOTKEY: Triggering renderer backend check...\n");
            CheckRendererBackendAndSwitchToDX11();
        }
        lastF5State = currentF5State;
        
        // Check for F6 to force DirectX 11
        static bool lastF6State = false;
        bool currentF6State = IsSDL3KeyPressed(SDL_SCANCODE_F6);
        if (currentF6State && !lastF6State) {
            printf("F6 HOTKEY: Force switching to DirectX 11...\n");
            ForceDirectX11Renderer();
        }
        lastF6State = currentF6State;
    }
    
    // Debug function to print current SDL3 backend information
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
    
    // Check current renderer backend and optionally force DirectX 11
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
        printf("SDL3 RENDERER CHECK: Current renderer = %s\n", rendererName);
        
        // Check backend type
        bool isDX11 = SDL_HasProperty(rendererProps, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER);
        bool isDX12 = SDL_HasProperty(rendererProps, SDL_PROP_RENDERER_D3D12_DEVICE_POINTER);
        bool isVulkan = SDL_HasProperty(rendererProps, SDL_PROP_RENDERER_VULKAN_INSTANCE_POINTER);
        bool isGPU = (strcmp(rendererName, "gpu") == 0);
        
        printf("SDL3 RENDERER CHECK: Backend analysis:\n");
        printf("  - DirectX 11: %s\n", isDX11 ? "YES" : "NO");
        printf("  - DirectX 12: %s\n", isDX12 ? "YES" : "NO");
        printf("  - Vulkan: %s\n", isVulkan ? "YES" : "NO");
        printf("  - SDL_GPU: %s\n", isGPU ? "YES" : "NO");
        printf("  - Other/OpenGL/Software: %s\n", (!isDX11 && !isDX12 && !isVulkan && !isGPU) ? "YES" : "NO");
        
        if (isDX11) {
            printf("SDL3 RENDERER CHECK: ? Already using DirectX 11 - no changes needed\n");
            return true;
        } else {
            printf("SDL3 RENDERER CHECK: ? NOT using DirectX 11! Current: %s\n", rendererName);
            printf("SDL3 RENDERER CHECK: We can recreate the renderer to force DirectX 11\n");
            return false;
        }
    }
    
    // Force DirectX 11 renderer creation
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
        
        printf("SDL3 RENDERER FORCE: Creating DirectX 11 renderer with properties:\n");
        printf("  - Name: direct3d11\n");
        printf("  - VSync: enabled\n");
        
        // Create the DirectX 11 renderer
        g_sdlContext.renderer = SDL_CreateRendererWithProperties(rendererProps);
        SDL_DestroyProperties(rendererProps);
        
        if (!g_sdlContext.renderer) {
            printf("SDL3 RENDERER FORCE: Failed to create DirectX 11 renderer: %s\n", SDL_GetError());
            printf("SDL3 RENDERER FORCE: Available render drivers:\n");
            
            int numRenderDrivers = SDL_GetNumRenderDrivers();
            for (int i = 0; i < numRenderDrivers; i++) {
                const char* driverName = SDL_GetRenderDriver(i);
                printf("  [%d]: %s\n", i, driverName ? driverName : "Unknown");
            }
            return false;
        }
        
        // Verify we got DirectX 11
        SDL_PropertiesID newRendererProps = SDL_GetRendererProperties(g_sdlContext.renderer);
        if (newRendererProps) {
            const char* newRendererName = SDL_GetStringProperty(newRendererProps, SDL_PROP_RENDERER_NAME_STRING, "Unknown");
            bool isNewDX11 = SDL_HasProperty(newRendererProps, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER);
            
            printf("SDL3 RENDERER FORCE: New renderer created:\n");
            printf("  - Name: %s\n", newRendererName);
            printf("  - DirectX 11: %s\n", isNewDX11 ? "YES" : "NO");
            
            if (isNewDX11) {
                printf("SDL3 RENDERER FORCE: ? Successfully forced DirectX 11!\n");
                return true;
            } else {
                printf("SDL3 RENDERER FORCE: ? Failed to force DirectX 11, got: %s\n", newRendererName);
                return false;
            }
        }
        
        return true;
    }
    
    // Windows Message Hook Integration
    LRESULT CALLBACK SDL3MessageHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0) {
            // DISABLED: Event pumping now handled once per frame in ProcessScreenUpdatesAndResources_new
            // UpdateSDL3Events();
        }
        
        // Call next hook in chain
        return CallNextHookEx(g_messageHook, nCode, wParam, lParam);
    }
    
    // Hooked GetMessageA - inject SDL event processing
    int WINAPI GetMessageA_Hook(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
        // DISABLED: Excessive event pumping was causing jitter
        // UpdateSDL3Events();
        
        // Call original GetMessageA
        int result = o_GetMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
        
        // DISABLED: Excessive event pumping was causing jitter
        // if (result > 0) {
        //     UpdateSDL3Events();
        // }
        
        return result;
    }
    
    // Hooked PeekMessageA - inject SDL event processing  
    BOOL WINAPI PeekMessageA_Hook(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) {
        // DISABLED: Excessive event pumping was causing jitter
        // UpdateSDL3Events();
        
        // Call original PeekMessageA
        BOOL result = o_PeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
        
        return result;
    }
    
    // Simplified event integration - just ensure SDL3 events are pumped regularly
    void InstallMessageHook() {
        // No complex hooking needed - we'll pump events when needed
        printf("SDL3 simplified event integration ready\n");
    }
    
    // Simplified cleanup
    void UninstallMessageHook() {
        printf("SDL3 simplified event integration cleanup\n");
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
        
        already_subclassed_window = hwnd;  // Mark this window as subclassed
        printf("SDL3 SUBCLASS: Successfully subclassed SDL3 window (HWND=0x%p)\n", hwnd);
        printf("SDL3 SUBCLASS: Original WndProc=0x%p, New WndProc=0x%p\n", g_originalSDLWindowProc, SDL3GameWindowProc);
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
    
    // Create a hidden original game window that the game logic can work with
    HWND CreateOriginalGameWindow(int displayMode, HINSTANCE hInstance) {
        // Use the known game values (from decompiled CreateMainWindow)
        const char* className = "Moon Lights 2 Ver.1.07";
        const char* caption = "Moon Lights 2 Ver.1.07";
        
        // Register window class with the game's window procedure
        WNDCLASSA windowClass = {};
        windowClass.cbClsExtra = 0;
        windowClass.cbWndExtra = 0;
        windowClass.hInstance = hInstance;
        windowClass.style = 3;
        windowClass.lpfnWndProc = g_handleMenuCallHotkeys;
        windowClass.hIcon = LoadIconA(NULL, IDI_APPLICATION); // Use system default app icon
        windowClass.hCursor = LoadCursorA(0, IDC_ARROW);
        windowClass.hbrBackground = 0;
        windowClass.lpszMenuName = caption;
        windowClass.lpszClassName = className;
        
        // Register the class (ignore error if already registered)
        RegisterClassA(&windowClass);
        
        // Create a hidden window that the game logic can work with
        HWND gameWindow = CreateWindowExA(
            0,                          // Extended style
            className,                  // Class name
            caption,                    // Window title
            WS_OVERLAPPEDWINDOW,        // Style
            CW_USEDEFAULT,              // X position
            CW_USEDEFAULT,              // Y position  
            640,                        // Width
            480,                        // Height
            nullptr,                    // Parent window
            nullptr,                    // Menu
            hInstance,                  // Instance
            nullptr                     // Additional application data
        );
        
        if (gameWindow) {
            // Don't show this window - it's hidden and only used for game logic
            // ShowWindow(gameWindow, SW_HIDE);
            printf("SDL3 GAME WINDOW: Created hidden original game window 0x%p for game logic\n", gameWindow);
        } else {
            printf("SDL3 GAME WINDOW: Failed to create original game window (error %d)\n", GetLastError());
        }
        
        return gameWindow;
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
        
        // Detect and report SDL3 backend information
        const char* videoDriver = SDL_GetCurrentVideoDriver();
        printf("SDL3 BACKEND: Video driver = %s\n", videoDriver ? videoDriver : "Unknown");
        
        int numVideoDrivers = SDL_GetNumVideoDrivers();
        printf("SDL3 BACKEND: Available video drivers (%d):\n", numVideoDrivers);
        for (int i = 0; i < numVideoDrivers; i++) {
            const char* driverName = SDL_GetVideoDriver(i);
            printf("  [%d]: %s%s\n", i, driverName ? driverName : "Unknown",
                   (videoDriver && driverName && strcmp(videoDriver, driverName) == 0) ? " (ACTIVE)" : "");
        }
        
        int numRenderDrivers = SDL_GetNumRenderDrivers();
        printf("SDL3 BACKEND: Available render drivers (%d):\n", numRenderDrivers);
        for (int i = 0; i < numRenderDrivers; i++) {
            const char* renderDriverName = SDL_GetRenderDriver(i);
            printf("  [%d]: %s\n", i, renderDriverName ? renderDriverName : "Unknown");
        }
        
        g_sdlContext.isFullscreen = isFullScreen;
        
        // Set native game resolution for dual rendering
        g_sdlContext.gameWidth = 256;   // Actual game rendering width  
        g_sdlContext.gameHeight = 240;  // Actual game rendering height
        
        // WINDOWED MODE BY DEFAULT: Always start in windowed mode (use Alt+Enter to toggle fullscreen)
        // Windowed mode - use 640x480 default size
        // Don't override if already set by CreateMainWindow_new
        if (g_sdlContext.windowWidth == 0 || g_sdlContext.windowHeight == 0) {
            g_sdlContext.windowWidth = 640;   // Default windowed size
            g_sdlContext.windowHeight = 480;
        }
        
        // Always create windowed window initially (Alt+Enter enables fullscreen)
        g_sdlContext.window = SDL_CreateWindow(
            "Moon Lights 2 Ver.1.07",
            g_sdlContext.windowWidth, 
            g_sdlContext.windowHeight,
            SDL_WINDOW_RESIZABLE  // CRITICAL: Must be resizable for fullscreen toggle
        );
        
        if (!g_sdlContext.window) {
            printf("SDL3 ERROR: Failed to create window: %s\n", SDL_GetError());
            SDL_Quit();
            return false;
        }
        
        // Create renderer - FORCE DirectX 11 specifically (no more SDL_GPU!)
        SDL_PropertiesID rendererProps = SDL_CreateProperties();
        SDL_SetStringProperty(rendererProps, SDL_PROP_RENDERER_CREATE_NAME_STRING, "direct3d11"); // FORCE DirectX 11
        SDL_SetNumberProperty(rendererProps, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, 1);
        printf("SDL3 RENDERER: Forcing DirectX 11 renderer (no more auto-selection)\n");
        g_sdlContext.renderer = SDL_CreateRendererWithProperties(rendererProps);
        SDL_DestroyProperties(rendererProps);
        
        if (!g_sdlContext.renderer) {
            printf("SDL3 ERROR: Failed to create DirectX 11 renderer: %s\n", SDL_GetError());
            printf("SDL3 ERROR: Available render drivers:\n");
            int numRenderDrivers = SDL_GetNumRenderDrivers();
            for (int i = 0; i < numRenderDrivers; i++) {
                const char* driverName = SDL_GetRenderDriver(i);
                printf("  [%d]: %s\n", i, driverName ? driverName : "Unknown");
            }
            SDL_DestroyWindow(g_sdlContext.window);
            SDL_Quit();
            return false;
        }
        
        // Check if we actually got DirectX 11
        CheckAndForceDirectX11Renderer();
        
        // Detect and report renderer backend information
        SDL_PropertiesID rendererInfoProps = SDL_GetRendererProperties(g_sdlContext.renderer);
        if (rendererInfoProps) {
            const char* rendererName = SDL_GetStringProperty(rendererInfoProps, SDL_PROP_RENDERER_NAME_STRING, "Unknown");
            printf("SDL3 BACKEND: Active renderer = %s\n", rendererName);
            
            // Check for specific backend properties
            if (SDL_HasProperty(rendererInfoProps, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER)) {
                printf("SDL3 BACKEND: Using Direct3D 11 backend\n");
            } else if (SDL_HasProperty(rendererInfoProps, SDL_PROP_RENDERER_D3D12_DEVICE_POINTER)) {
                printf("SDL3 BACKEND: Using Direct3D 12 backend\n");
            } else if (SDL_HasProperty(rendererInfoProps, SDL_PROP_RENDERER_VULKAN_INSTANCE_POINTER)) {
                printf("SDL3 BACKEND: Using Vulkan backend\n");
            } else {
                printf("SDL3 BACKEND: Using %s backend (other/software/OpenGL)\n", rendererName);
            }
        } else {
            printf("SDL3 BACKEND: Could not get renderer properties\n");
        }
        
        // TEMPORARILY DISABLED: Logical presentation for mouse coordinate testing
        // This scaling was causing mouse coordinate translation issues with ImGui
        /*
        printf("SDL3 LOGICAL: Setting logical presentation to actual window size: %dx%d\n", 
               g_sdlContext.windowWidth, g_sdlContext.windowHeight);
        
        SDL_SetRenderLogicalPresentation(g_sdlContext.renderer, 
            g_sdlContext.windowWidth, g_sdlContext.windowHeight,  // Use actual window size
            SDL_LOGICAL_PRESENTATION_LETTERBOX);
        */
        
        printf("SDL3 LOGICAL: Logical presentation DISABLED for mouse coordinate testing\n");
        printf("SDL3 LOGICAL: Using direct 1:1 coordinate mapping - window size: %dx%d\n", 
               g_sdlContext.windowWidth, g_sdlContext.windowHeight);
        
        // Create back buffer texture (this replaces DirectDraw's back buffer)
        g_sdlContext.backBuffer = SDL_CreateTexture(
            g_sdlContext.renderer,
            SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET,
            g_sdlContext.windowWidth,
            g_sdlContext.windowHeight
        );
        
        if (!g_sdlContext.backBuffer) {
            SDL_DestroyRenderer(g_sdlContext.renderer);
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
            SDL_DestroyTexture(g_sdlContext.backBuffer);
            SDL_DestroyRenderer(g_sdlContext.renderer);
            SDL_DestroyWindow(g_sdlContext.window);
            SDL_Quit();
            return false;
        }
        
        // CRITICAL: Set nearest neighbor filtering for crisp pixel art
        SDL_SetTextureScaleMode(g_sdlContext.gameBuffer, SDL_SCALEMODE_NEAREST);
        
        printf("SDL3 DUAL RENDERING: Game buffer created at %dx%d with NEAREST NEIGHBOR filtering, window buffer at %dx%d\n",
               g_sdlContext.gameWidth, g_sdlContext.gameHeight,
               g_sdlContext.windowWidth, g_sdlContext.windowHeight);
        
        // Create sprite buffer texture (this replaces the 256x256 DirectDraw surface)
        g_sdlContext.spriteBuffer = SDL_CreateTexture(
            g_sdlContext.renderer,
            SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET,
            256,
            256
        );
        
        if (!g_sdlContext.spriteBuffer) {
            SDL_DestroyTexture(g_sdlContext.gameBuffer);
            SDL_DestroyTexture(g_sdlContext.backBuffer);
            SDL_DestroyRenderer(g_sdlContext.renderer);
            SDL_DestroyWindow(g_sdlContext.window);
            SDL_Quit();
            return false;
        }
        
        // Set nearest neighbor filtering for sprite buffer
        SDL_SetTextureScaleMode(g_sdlContext.spriteBuffer, SDL_SCALEMODE_NEAREST);
        
        // Set initial render target to game buffer (so the game renders at 320x240)
        SDL_SetRenderTarget(g_sdlContext.renderer, g_sdlContext.gameBuffer);
        
        // Clear game buffer to black
        SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_sdlContext.renderer);
        
        printf("SDL3 RENDER TARGET: Set to game buffer (%dx%d) for game rendering\n",
               g_sdlContext.gameWidth, g_sdlContext.gameHeight);
        
        g_sdlContext.initialized = true;
        
        // User-friendly information about fullscreen toggle
        printf("SDL3 CONTROLS: Press Alt+Enter to toggle between windowed and fullscreen mode\n");
        
        // CRITICAL: Force gamepad system refresh now that SDL3 is fully initialized
        printf("SDL3 GAMEPAD: Triggering immediate gamepad detection after SDL3 context ready...\n");
        auto& inputManager = argentum::input::InputManager::getInstance();
        inputManager.initialize();  // Ensure input manager is initialized
        inputManager.refreshGamepads();  // Force gamepad detection
        printf("SDL3 GAMEPAD: Gamepad refresh completed, connected controllers: %d\n", inputManager.getConnectedGamepadCount());
        
        // Ensure proper focus for input handling using SDL3 APIs
        if (!SDL_SetWindowFocusable(g_sdlContext.window, true)) {
            printf("Warning: Could not set window focusable: %s\n", SDL_GetError());
        }
        
        // Request keyboard focus for our window
        if (!SDL_RaiseWindow(g_sdlContext.window)) {
            printf("Warning: Could not raise window: %s\n", SDL_GetError());
        }
        
        // Debug: Check which window has focus
        SDL_Window* focused_window = SDL_GetKeyboardFocus();
        if (focused_window == g_sdlContext.window) {
            printf("SDL3 FOCUS: Our window has keyboard focus!\n");
        } else {
            printf("SDL3 FOCUS: Our window does NOT have keyboard focus (focused_window=%p, our_window=%p)\n", 
                   focused_window, g_sdlContext.window);
        }
        
        // Also get the HWND for compatibility but use SDL3 focus APIs
        HWND sdl_hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_sdlContext.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (sdl_hwnd) {
            printf("SDL3 FOCUS: Window created with HWND 0x%p\n", sdl_hwnd);
        }
        
        // Install SDL3 event integration hooks for proper keyboard input
        InstallMessageHook();
        
        // CRITICAL: Subclass the SDL3 window to forward messages to game's window procedure
        if (sdl_hwnd) {
            SubclassSDL3Window(sdl_hwnd);
        }
        
        return true;
    }
    
    void CleanupSDL3Context() {
        if (!g_sdlContext.initialized) return;
        
        // No hidden window to cleanup
        
        // Unsubclass the SDL3 window first
        if (g_sdlContext.window) {
            HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_sdlContext.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
            if (hwnd) {
                UnsubclassSDL3Window(hwnd);
            }
        }
        
        // Uninstall SDL3 event integration hooks
        UninstallMessageHook();
        
        if (g_sdlContext.backBuffer) {
            SDL_DestroyTexture(g_sdlContext.backBuffer);
            g_sdlContext.backBuffer = nullptr;
        }
        
        if (g_sdlContext.gameBuffer) {
            SDL_DestroyTexture(g_sdlContext.gameBuffer);
            g_sdlContext.gameBuffer = nullptr;
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
    
    SDL_Texture* CreateCompatibleTexture(int width, int height) {
        if (!g_sdlContext.initialized) return nullptr;
        
        return SDL_CreateTexture(
            g_sdlContext.renderer,
            SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET,
            width,
            height
        );
    }
    
    // CreateMainWindow replacement - creates SDL window directly 
    HWND __cdecl CreateMainWindow_new(int displayMode, HINSTANCE hInstance, int nCmdShow) {
        printf("HOOK DEBUG: CreateMainWindow_new called! displayMode=%d, hInstance=%p, nCmdShow=%d\n", displayMode, hInstance, nCmdShow);
        
        // Skip color depth check since SDL3 handles color conversion
        // The original game required 8-bit color mode, but we can emulate this with SDL3
        bool isFullScreen = false;
        int windowWidth = 640;
        int windowHeight = 480;
        
        // WINDOWED MODE BY DEFAULT: All modes start windowed, use Alt+Enter to toggle fullscreen
        switch (displayMode) {
            case 0: // 640x480 windowed (upgraded from 384x320)
                windowWidth = 640;
                windowHeight = 480;
                isFullScreen = false;
                printf("SDL3 WINDOW MODE: Selected 640x480 windowed (upgraded from mode 0)\n");
                break;
            case 1: // 640x480 windowed (changed from fullscreen)
                windowWidth = 640;
                windowHeight = 480;
                isFullScreen = false;
                printf("SDL3 WINDOW MODE: Selected 640x480 windowed (changed from fullscreen mode 1)\n");
                break;
            case 2: // 640x480 windowed (upgraded from 262x280)
                windowWidth = 640;
                windowHeight = 480;
                isFullScreen = false;
                printf("SDL3 WINDOW MODE: Selected 640x480 windowed (upgraded from mode 2)\n");
                break;
            case 3: // 640x480 windowed (changed from fullscreen)
                windowWidth = 640;
                windowHeight = 480;
                isFullScreen = false;
                printf("SDL3 WINDOW MODE: Selected 640x480 windowed (changed from fullscreen mode 3)\n");
                break;
            default:
                windowWidth = 640;
                windowHeight = 480;
                isFullScreen = false;
                printf("SDL3 WINDOW MODE: Selected default 640x480 windowed (mode %d)\n", displayMode);
                break;
        }
        
        // No need for hidden window - we'll pass the SDL3 window handle directly to game logic
        
        // Initialize SDL if not already initialized
        if (!g_sdlContext.initialized) {
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD) < 0) {
                printf("SDL3 ERROR: Failed to initialize SDL3 in CreateMainWindow: %s\n", SDL_GetError());
                return NULL;
            }
            
            g_sdlContext.isFullscreen = isFullScreen;
            g_sdlContext.windowWidth = windowWidth;
            g_sdlContext.windowHeight = windowHeight;
            
            // Set native game resolution for dual rendering
            g_sdlContext.gameWidth = 256;   // Actual game rendering width  
            g_sdlContext.gameHeight = 240;  // Actual game rendering height
            
            // Create SDL window - WINDOWED MODE BY DEFAULT (use Alt+Enter for fullscreen)
            Uint32 flags = SDL_WINDOW_RESIZABLE;  // CRITICAL: Must be resizable for fullscreen toggle
            
            g_sdlContext.window = SDL_CreateWindow(
                "Moon Lights 2 Ver.1.07",
                windowWidth,
                windowHeight,
                flags
            );
            
            // For windowed mode, ensure the window is properly positioned and sized
            if (!isFullScreen) {
                SDL_SetWindowPosition(g_sdlContext.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                
                // Verify the actual size we got vs what we requested
                int actualWidth, actualHeight;
                SDL_GetWindowSize(g_sdlContext.window, &actualWidth, &actualHeight);
                printf("SDL3 WINDOW SIZE: Requested %dx%d, Got %dx%d\n", 
                       windowWidth, windowHeight, actualWidth, actualHeight);
                
                // Check if SDL3 gave us something different and why
                if (actualWidth != windowWidth || actualHeight != windowHeight) {
                    printf("SDL3 WINDOW WARNING: Size mismatch! This could be due to:\n");
                    printf("  - Window decorations (title bar, borders)\n");
                    printf("  - Display scaling/DPI settings\n");
                    printf("  - Platform-specific limitations\n");
                    
                    // For small windows like 262x280, the decorations can significantly affect the client area
                    // SDL3 should handle this, but let's see what we actually got
                    printf("SDL3 WINDOW INFO: Using actual size %dx%d for rendering\n", actualWidth, actualHeight);
                }
                
                // Update context with actual size (this is what we'll use for rendering)
                g_sdlContext.windowWidth = actualWidth;
                g_sdlContext.windowHeight = actualHeight;
                
                // Get the Win32 window handle to check client vs window area
                HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_sdlContext.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
                if (hwnd) {
                    RECT clientRect, windowRect;
                    GetClientRect(hwnd, &clientRect);
                    GetWindowRect(hwnd, &windowRect);
                    int clientWidth = clientRect.right - clientRect.left;
                    int clientHeight = clientRect.bottom - clientRect.top;
                    int windowWidth_win32 = windowRect.right - windowRect.left;
                    int windowHeight_win32 = windowRect.bottom - windowRect.top;
                    
                    printf("SDL3 WIN32 DEBUG: Window area=%dx%d, Client area=%dx%d\n",
                           windowWidth_win32, windowHeight_win32, clientWidth, clientHeight);
                    printf("SDL3 WIN32 DEBUG: Decorations add %dx%d pixels\n",
                           windowWidth_win32 - clientWidth, windowHeight_win32 - clientHeight);
                }
            }
            
            if (!g_sdlContext.window) {
                printf("SDL3 ERROR: Failed to create window in CreateMainWindow: %s\n", SDL_GetError());
                SDL_Quit();
                return NULL;
            }
            
            // Create renderer - FORCE DirectX 11 specifically (no more SDL_GPU!)
            SDL_PropertiesID rendererProps = SDL_CreateProperties();
            SDL_SetPointerProperty(rendererProps, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, g_sdlContext.window);
            SDL_SetStringProperty(rendererProps, SDL_PROP_RENDERER_CREATE_NAME_STRING, "direct3d11"); // FORCE DirectX 11
            SDL_SetNumberProperty(rendererProps, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, 1);
            printf("SDL3 RENDERER: CreateMainWindow - Forcing DirectX 11 renderer (no more auto-selection)\n");
            g_sdlContext.renderer = SDL_CreateRendererWithProperties(rendererProps);
            SDL_DestroyProperties(rendererProps);
            
            if (!g_sdlContext.renderer) {
                printf("SDL3 ERROR: Failed to create DirectX 11 renderer in CreateMainWindow: %s\n", SDL_GetError());
                printf("SDL3 ERROR: Available render drivers:\n");
                int numRenderDrivers = SDL_GetNumRenderDrivers();
                for (int i = 0; i < numRenderDrivers; i++) {
                    const char* driverName = SDL_GetRenderDriver(i);
                    printf("  [%d]: %s\n", i, driverName ? driverName : "Unknown");
                }
                SDL_DestroyWindow(g_sdlContext.window);
                SDL_Quit();
                return NULL;
            }
            
            // Check if we actually got DirectX 11
            CheckAndForceDirectX11Renderer();
            
            // Detect and report renderer backend information
            SDL_PropertiesID rendererInfoProps = SDL_GetRendererProperties(g_sdlContext.renderer);
            if (rendererInfoProps) {
                const char* rendererName = SDL_GetStringProperty(rendererInfoProps, SDL_PROP_RENDERER_NAME_STRING, "Unknown");
                printf("SDL3 BACKEND: CreateMainWindow - Active renderer = %s\n", rendererName);
                
                // Check for specific backend properties
                if (SDL_HasProperty(rendererInfoProps, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER)) {
                    printf("SDL3 BACKEND: CreateMainWindow - Using Direct3D 11 backend\n");
                } else if (SDL_HasProperty(rendererInfoProps, SDL_PROP_RENDERER_D3D12_DEVICE_POINTER)) {
                    printf("SDL3 BACKEND: CreateMainWindow - Using Direct3D 12 backend\n");
                } else if (SDL_HasProperty(rendererInfoProps, SDL_PROP_RENDERER_VULKAN_INSTANCE_POINTER)) {
                    printf("SDL3 BACKEND: CreateMainWindow - Using Vulkan backend\n");
                } else {
                    printf("SDL3 BACKEND: CreateMainWindow - Using %s backend (other/software/OpenGL)\n", rendererName);
                }
            }
            
            // TEMPORARILY DISABLED: Logical presentation for mouse coordinate testing
            // This scaling was causing mouse coordinate translation issues with ImGui
            /*
            printf("SDL3 LOGICAL: Setting logical presentation to actual window size: %dx%d\n", 
                   windowWidth, windowHeight);
            
            SDL_SetRenderLogicalPresentation(g_sdlContext.renderer, 
                windowWidth, windowHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
            */
            
            printf("SDL3 LOGICAL: Logical presentation DISABLED for mouse coordinate testing\n");
            printf("SDL3 LOGICAL: Using direct 1:1 coordinate mapping - window size: %dx%d\n", 
                   windowWidth, windowHeight);
            
            // Create textures
            g_sdlContext.backBuffer = SDL_CreateTexture(
                g_sdlContext.renderer,
                SDL_PIXELFORMAT_RGBA8888,
                SDL_TEXTUREACCESS_TARGET,
                windowWidth,
                windowHeight
            );
            
            g_sdlContext.gameBuffer = SDL_CreateTexture(
                g_sdlContext.renderer,
                SDL_PIXELFORMAT_RGBA8888,
                SDL_TEXTUREACCESS_TARGET,
                g_sdlContext.gameWidth,
                g_sdlContext.gameHeight
            );
            
            // CRITICAL: Set nearest neighbor filtering for crisp pixel art
            if (g_sdlContext.gameBuffer) {
                SDL_SetTextureScaleMode(g_sdlContext.gameBuffer, SDL_SCALEMODE_NEAREST);
            }
            
            g_sdlContext.spriteBuffer = SDL_CreateTexture(
                g_sdlContext.renderer,
                SDL_PIXELFORMAT_RGBA8888,
                SDL_TEXTUREACCESS_TARGET,
                256,
                256
            );
            
            // Set nearest neighbor filtering for sprite buffer
            if (g_sdlContext.spriteBuffer) {
                SDL_SetTextureScaleMode(g_sdlContext.spriteBuffer, SDL_SCALEMODE_NEAREST);
            }
            
            printf("SDL3 DUAL RENDERING: CreateMainWindow - Game buffer created at %dx%d with NEAREST NEIGHBOR filtering, window buffer at %dx%d\n",
                   g_sdlContext.gameWidth, g_sdlContext.gameHeight, windowWidth, windowHeight);
            
            g_sdlContext.initialized = true;
            
            // User-friendly information about fullscreen toggle
            printf("SDL3 CONTROLS: Press Alt+Enter to toggle between windowed and fullscreen mode\n");
            
            // Install SDL3 event integration hooks for proper keyboard input
            InstallMessageHook();
        }
        
        // Get the native Windows handle from the SDL window
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_sdlContext.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        
        if (!hwnd) {
            return NULL;
        }
        
        // Show the window and ensure it can receive input using SDL3 APIs
        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
        
        // CRITICAL: Ensure this window can receive input for the game's input system using SDL3 APIs
        if (!SDL_SetWindowFocusable(g_sdlContext.window, true)) {
            printf("Warning: Could not set window focusable in CreateMainWindow: %s\n", SDL_GetError());
        }
        
        if (!SDL_RaiseWindow(g_sdlContext.window)) {
            printf("Warning: Could not raise window in CreateMainWindow: %s\n", SDL_GetError());
        }
        
        // Check focus status
        SDL_Window* focused_window = SDL_GetKeyboardFocus();
        if (focused_window == g_sdlContext.window) {
            printf("SDL3 FOCUS: CreateMainWindow - Our window has keyboard focus!\n");
        } else {
            printf("SDL3 FOCUS: CreateMainWindow - Our window does NOT have keyboard focus (focused_window=%p, our_window=%p)\n", 
                   focused_window, g_sdlContext.window);
        }
        
        printf("SDL3 FOCUS: Window created, HWND 0x%p for game input and logic\n", hwnd);
        
        // CRITICAL: Subclass the SDL3 window to forward messages to game's window procedure
        SubclassSDL3Window(hwnd);
        
        return hwnd;
    }
    
    // SDL3-compatible InitializeWindow replacement  
    int __cdecl InitializeWindow_new(void* hdc) {
        if (!g_sdlContext.initialized) {
            return -1;
        }
        
        // In the original, this function used GDI operations like CreateCompatibleDC, 
        // CreateBitmap, BitBlt, etc. For SDL3, we just need to ensure our context is ready.
        
        return 0;
    }

    // SDL3-compatible isGraphicsSystemInitialized replacement
    int __cdecl isGraphicsSystemInitialized_new() {
        if (g_sdlContext.initialized && g_sdlContext.window && g_sdlContext.renderer) {
            return 0;  // Success (graphics system is initialized)
        } else {
            return -1; // Failure (graphics system not initialized)
        }
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
        
        // Debug: Log scaling info very rarely to avoid performance impact
        static int debugFrameCount = 0;
        debugFrameCount++;
        // DISABLED for performance: SDL3 windowed debug causes FPS drops
        // if (debugFrameCount % 1800 == 0) {  // Every 30 seconds at 60fps
        //     printf("SDL3 WINDOWED DEBUG: Window=%dx%d, Game=%dx%d, Dest=(%.0f,%.0f,%.0fx%.0f), Aspect=%.3f/%.3f\n",
        //            actualWindowWidth, actualWindowHeight, 
        //            g_sdlContext.gameWidth, g_sdlContext.gameHeight,
        //            destRect.x, destRect.y, destRect.w, destRect.h,
        //            windowAspect, gameAspect);
        // }
        
        // Render the scaled game buffer to the window
        SDL_RenderTexture(g_sdlContext.renderer, g_sdlContext.gameBuffer, nullptr, &destRect);
        
        // Debug: Log scaling info only occasionally to avoid spam
       /*  static int frameCount = 0;
        frameCount++;
        if (frameCount % 120 == 0) {  // Every 2 seconds at 60fps
            printf("SDL3 SCALING: Game %dx%d scaled to %.0fx%.0f at offset (%.0f,%.0f) on %dx%d screen [Frame %d]\n",
                   g_sdlContext.gameWidth, g_sdlContext.gameHeight,
                   destRect.w, destRect.h, destRect.x, destRect.y, 
                   actualWindowWidth, actualWindowHeight, frameCount);
        } */
    }
    
    void PresentFrame() {
        if (!g_sdlContext.initialized) return;
        
        // Present the final frame to the screen
        SDL_RenderPresent(g_sdlContext.renderer);
    }
    
    void TestGameBuffer() {
        if (!g_sdlContext.initialized || !g_sdlContext.gameBuffer) return;
        
        // Set render target to game buffer
        SDL_SetRenderTarget(g_sdlContext.renderer, g_sdlContext.gameBuffer);
        
        // Clear to dark blue
        SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 0, 128, 255);
        SDL_RenderClear(g_sdlContext.renderer);
        
        // Draw some test rectangles to verify scaling (adjusted for 256x240)
        SDL_SetRenderDrawColor(g_sdlContext.renderer, 255, 0, 0, 255); // Red
        SDL_FRect redRect = {10, 10, 50, 50};
        SDL_RenderFillRect(g_sdlContext.renderer, &redRect);
        
        SDL_SetRenderDrawColor(g_sdlContext.renderer, 0, 255, 0, 255); // Green  
        SDL_FRect greenRect = {196, 10, 50, 50};  // 256-60 = 196 for right side
        SDL_RenderFillRect(g_sdlContext.renderer, &greenRect);
        
        SDL_SetRenderDrawColor(g_sdlContext.renderer, 255, 255, 0, 255); // Yellow
        SDL_FRect yellowRect = {10, 180, 50, 50};
        SDL_RenderFillRect(g_sdlContext.renderer, &yellowRect);
        
        SDL_SetRenderDrawColor(g_sdlContext.renderer, 255, 255, 255, 255); // White
        SDL_FRect whiteRect = {196, 180, 50, 50};  // 256-60 = 196 for right side
        SDL_RenderFillRect(g_sdlContext.renderer, &whiteRect);
        
        // Draw a center cross to verify centering (adjusted for 256x240)
        SDL_SetRenderDrawColor(g_sdlContext.renderer, 255, 255, 255, 255); // White
        SDL_FRect hLine = {0, 120, 256, 2};  // Horizontal line across width
        SDL_FRect vLine = {128, 0, 2, 240};  // Vertical line at center (256/2 = 128)
        SDL_RenderFillRect(g_sdlContext.renderer, &hLine);
        SDL_RenderFillRect(g_sdlContext.renderer, &vLine);
        
        printf("SDL3 TEST: Drew test content to game buffer (256x240)\n");
    }
    
    // Public function to check renderer backend and optionally switch to DirectX 11
    void CheckRendererBackendAndSwitchToDX11() {
        printf("\n=== SDL3 RENDERER BACKEND CHECK ===\n");
        
        if (!CheckAndForceDirectX11Renderer()) {
            printf("Current renderer is NOT DirectX 11. Would you like to switch? (Y/N)\n");
            printf("Note: This will recreate textures and may cause a brief flicker.\n");
            
            // For automatic switching without user input, uncomment the next lines:
            // printf("AUTO-SWITCHING to DirectX 11...\n");
            // if (ForceDirectX11Renderer()) {
            //     printf("? Successfully switched to DirectX 11!\n");
            //     // Recreate any textures that were destroyed
            //     // You may need to recreate game textures here
            // } else {
            //     printf("? Failed to switch to DirectX 11\n");
            // }
            
            printf("To auto-switch, you can call ForceDirectX11Renderer() function\n");
        }
        
        printf("====================================\n\n");
    }
    
} // namespace argentum::hooks 