#include "imgui_overlay.h"
#include "globals.h"
#include "logging.h"
#include "gekkonet_hooks.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx9.h>
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <ddraw.h>

// DirectDraw function typedefs
typedef HRESULT(WINAPI* DirectDrawCreate_t)(GUID*, LPDIRECTDRAW*, IUnknown*);
typedef HRESULT(WINAPI* DDraw_Flip_t)(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE, DWORD);
typedef HRESULT(WINAPI* DDraw_Blt_t)(LPDIRECTDRAWSURFACE, LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDBLTFX);

// D3D9 function typedefs (fallback)
typedef HRESULT(APIENTRY* EndScene_t)(LPDIRECT3DDEVICE9 pDevice);
typedef HRESULT(APIENTRY* Reset_t)(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);
typedef HRESULT(APIENTRY* Present_t)(LPDIRECT3DDEVICE9 pDevice, CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion);
typedef HRESULT(APIENTRY* DrawIndexedPrimitive_t)(LPDIRECT3DDEVICE9 pDevice, D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount);

// Original function pointers
static EndScene_t EndScene_orig = nullptr;
static Reset_t Reset_orig = nullptr;
static Present_t Present_orig = nullptr;
static DrawIndexedPrimitive_t DrawIndexedPrimitive_orig = nullptr;
static WNDPROC oWndProc = nullptr;

// State variables
static bool g_overlay_visible = false;
static bool g_imgui_initialized = false;
static bool g_hooks_installed = false;
static HWND g_game_window = nullptr;
static bool g_f9_key_pressed = false;

// Hook functions
HRESULT APIENTRY Hook_EndScene(LPDIRECT3DDEVICE9 pDevice);
HRESULT APIENTRY Hook_Reset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);
HRESULT APIENTRY Hook_Present(LPDIRECT3DDEVICE9 pDevice, CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion);
HRESULT APIENTRY Hook_DrawIndexedPrimitive(LPDIRECT3DDEVICE9 pDevice, D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount);
LRESULT WINAPI Hook_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper to display variable grid
void DisplayVariableGrid(const char* prefix, uint32_t base_addr, bool is_16bit = true) {
    ImGui::Columns(4, nullptr, false);
    for (int i = 0; i < 16; i++) {
        char label = 'A' + i;
        if (is_16bit) {
            int16_t* var = (int16_t*)(base_addr + i * 2);
            ImGui::Text("%s%c: %d", prefix, label, *var);
        } else {
            int32_t* var = (int32_t*)(base_addr + i * 4);
            ImGui::Text("%s%c: %d", prefix, label, *var);
        }
        ImGui::NextColumn();
    }
    ImGui::Columns(1);
}

// Render debug information
void RenderDebugOverlay() {
    if (!g_overlay_visible) return;
    
    // Main Debug Window with Tabs
    ImGui::Begin("FM2K Debug Overlay", &g_overlay_visible, ImGuiWindowFlags_MenuBar);
    
    if (ImGui::BeginTabBar("DebugTabs")) {
        // Network & Frame Info Tab
        if (ImGui::BeginTabItem("Network/Frame")) {
            // Frame info
            uint32_t* frame_counter = (uint32_t*)FM2K::State::Memory::FRAME_COUNTER_ADDR;
            uint32_t* game_mode = (uint32_t*)FM2K::State::Memory::GAME_MODE_ADDR;
            uint32_t* fm2k_mode = (uint32_t*)FM2K::State::Memory::FM2K_GAME_MODE_ADDR;
            uint32_t* css_mode = (uint32_t*)FM2K::State::Memory::CHARACTER_SELECT_MODE_ADDR;
            
            ImGui::Text("Frame Counter: %u", *frame_counter);
            ImGui::Text("Game Mode: 0x%08X", *game_mode);
            ImGui::Text("FM2K Mode: 0x%08X", *fm2k_mode);
            ImGui::Text("CSS Mode: 0x%08X", *css_mode);
            
            ImGui::Separator();
            
            // Network info
            ImGui::Text("Network Status:");
            ImGui::Text("  Player Index: %d", ::player_index);
            ImGui::Text("  Is Host: %s", is_host ? "YES" : "NO");
            ImGui::Text("  Online Mode: %s", is_online_mode ? "YES" : "NO");
            ImGui::Text("  GekkoNet Ready: %s", gekko_session_started ? "YES" : "NO");
            ImGui::Text("  Can Advance Frame: %s", can_advance_frame ? "YES" : "NO");
            
            ImGui::EndTabItem();
        }
        
        // System Variables Tab
        if (ImGui::BeginTabItem("System Vars")) {
            ImGui::Text("System Variables A-P:");
            ImGui::Separator();
            DisplayVariableGrid("", 0x4456B0, true); // System vars start at 0x4456B0, 2 bytes each
            ImGui::EndTabItem();
        }
        
        // Task Variables Tab
        if (ImGui::BeginTabItem("Task Vars")) {
            ImGui::Text("P1 Task Variables A-P:");
            ImGui::Separator();
            DisplayVariableGrid("P1.", 0x470311, true); // P1 task vars start at 0x470311
            
            ImGui::Separator();
            ImGui::Text("P2 Task Variables A-P:");
            ImGui::Separator();
            DisplayVariableGrid("P2.", 0x47060D, true); // P2 task vars start at 0x47060D
            
            ImGui::EndTabItem();
        }
        
        // Character Variables Tab
        if (ImGui::BeginTabItem("Character Vars")) {
            ImGui::Text("P1 Character Variables A-P:");
            ImGui::Separator();
            DisplayVariableGrid("P1.", 0x4DFD17, true); // P1 char vars start at 0x4DFD17
            
            ImGui::Separator();
            ImGui::Text("P2 Character Variables A-P:");
            ImGui::Separator();
            DisplayVariableGrid("P2.", 0x4EDD56, true); // P2 char vars start at 0x4EDD56
            
            // Also show position info
            ImGui::Separator();
            ImGui::Text("Position Info:");
            int32_t* p1_x = (int32_t*)0x4DFCC3;
            int16_t* p1_y = (int16_t*)0x4DFCC7;
            int32_t* p2_x = (int32_t*)0x4EDD02;
            int16_t* p2_y = (int16_t*)0x4EDD06;
            ImGui::Text("P1 Position: (%d, %d)", *p1_x, *p1_y);
            ImGui::Text("P2 Position: (%d, %d)", *p2_x, *p2_y);
            
            ImGui::EndTabItem();
        }
        
        // Input Tab
        if (ImGui::BeginTabItem("Input")) {
            // Raw inputs
            uint16_t* p1_raw_input = (uint16_t*)FM2K::State::Memory::P1_RAW_INPUT_ADDR;
            uint16_t* p2_raw_input = (uint16_t*)FM2K::State::Memory::P2_RAW_INPUT_ADDR;
            
            ImGui::Text("P1 Raw Input: 0x%03X", *p1_raw_input);
            ImGui::Text("P2 Raw Input: 0x%03X", *p2_raw_input);
            
            ImGui::Separator();
            
            // Visual input display
            ImGui::Columns(2, nullptr, false);
            
            // P1 Input
            ImGui::Text("P1 Input:");
            ImGui::Text("  LEFT:  %s", (*p1_raw_input & 0x001) ? "[X]" : "[ ]");
            ImGui::Text("  RIGHT: %s", (*p1_raw_input & 0x002) ? "[X]" : "[ ]");
            ImGui::Text("  UP:    %s", (*p1_raw_input & 0x004) ? "[X]" : "[ ]");
            ImGui::Text("  DOWN:  %s", (*p1_raw_input & 0x008) ? "[X]" : "[ ]");
            ImGui::Text("  BTN1:  %s", (*p1_raw_input & 0x010) ? "[X]" : "[ ]");
            ImGui::Text("  BTN2:  %s", (*p1_raw_input & 0x020) ? "[X]" : "[ ]");
            ImGui::Text("  BTN3:  %s", (*p1_raw_input & 0x040) ? "[X]" : "[ ]");
            ImGui::Text("  BTN4:  %s", (*p1_raw_input & 0x080) ? "[X]" : "[ ]");
            ImGui::Text("  BTN5:  %s", (*p1_raw_input & 0x100) ? "[X]" : "[ ]");
            ImGui::Text("  BTN6:  %s", (*p1_raw_input & 0x200) ? "[X]" : "[ ]");
            ImGui::Text("  BTN7:  %s", (*p1_raw_input & 0x400) ? "[X]" : "[ ]");
            
            ImGui::NextColumn();
            
            // P2 Input
            ImGui::Text("P2 Input:");
            ImGui::Text("  LEFT:  %s", (*p2_raw_input & 0x001) ? "[X]" : "[ ]");
            ImGui::Text("  RIGHT: %s", (*p2_raw_input & 0x002) ? "[X]" : "[ ]");
            ImGui::Text("  UP:    %s", (*p2_raw_input & 0x004) ? "[X]" : "[ ]");
            ImGui::Text("  DOWN:  %s", (*p2_raw_input & 0x008) ? "[X]" : "[ ]");
            ImGui::Text("  BTN1:  %s", (*p2_raw_input & 0x010) ? "[X]" : "[ ]");
            ImGui::Text("  BTN2:  %s", (*p2_raw_input & 0x020) ? "[X]" : "[ ]");
            ImGui::Text("  BTN3:  %s", (*p2_raw_input & 0x040) ? "[X]" : "[ ]");
            ImGui::Text("  BTN4:  %s", (*p2_raw_input & 0x080) ? "[X]" : "[ ]");
            ImGui::Text("  BTN5:  %s", (*p2_raw_input & 0x100) ? "[X]" : "[ ]");
            ImGui::Text("  BTN6:  %s", (*p2_raw_input & 0x200) ? "[X]" : "[ ]");
            ImGui::Text("  BTN7:  %s", (*p2_raw_input & 0x400) ? "[X]" : "[ ]");
            
            ImGui::Columns(1);
            ImGui::EndTabItem();
        }
        
        // CSS & Battle Tab
        if (ImGui::BeginTabItem("CSS/Battle")) {
            // Character select info
            ImGui::Text("Character Select:");
            uint32_t* p1_cursor_x = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_X_ADDR;
            uint32_t* p1_cursor_y = (uint32_t*)FM2K::State::Memory::P1_CSS_CURSOR_Y_ADDR;
            uint32_t* p2_cursor_x = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_X_ADDR;
            uint32_t* p2_cursor_y = (uint32_t*)FM2K::State::Memory::P2_CSS_CURSOR_Y_ADDR;
            
            ImGui::Text("P1 CSS Cursor: (%u, %u)", *p1_cursor_x, *p1_cursor_y);
            ImGui::Text("P2 CSS Cursor: (%u, %u)", *p2_cursor_x, *p2_cursor_y);
            
            // Selected characters
            uint32_t* p1_char = (uint32_t*)FM2K::State::Memory::P1_SELECTED_CHAR_ADDR;
            uint32_t* p2_char = (uint32_t*)FM2K::State::Memory::P2_SELECTED_CHAR_ADDR;
            
            ImGui::Text("P1 Selected Character: %u", *p1_char);
            ImGui::Text("P2 Selected Character: %u", *p2_char);
            
            // Confirmed status
            uint32_t* p1_confirmed = (uint32_t*)FM2K::State::Memory::P1_CSS_CONFIRMED_ADDR;
            uint32_t* p2_confirmed = (uint32_t*)FM2K::State::Memory::P2_CSS_CONFIRMED_ADDR;
            
            ImGui::Text("P1 Confirmed: %s", *p1_confirmed ? "YES" : "NO");
            ImGui::Text("P2 Confirmed: %s", *p2_confirmed ? "YES" : "NO");
            
            ImGui::Separator();
            
            // Battle info
            ImGui::Text("Battle Info:");
            
            // HP info (from Cheat Engine table)
            uint32_t* p1_hp = (uint32_t*)0x4DFC85;
            uint32_t* p2_hp = (uint32_t*)0x4EDCC4;
            uint32_t* p1_super = (uint32_t*)0x4DFC9D;
            uint32_t* p2_super = (uint32_t*)0x4EDCDC;
            uint32_t* p1_stock = (uint32_t*)0x4DFC95;
            uint32_t* p2_stock = (uint32_t*)0x4EDCD4;
            
            ImGui::Text("P1 HP: %u | Super: %u | Stock: %u", *p1_hp, *p1_super, *p1_stock);
            ImGui::Text("P2 HP: %u | Super: %u | Stock: %u", *p2_hp, *p2_super, *p2_stock);
            
            uint32_t* round_timer = (uint32_t*)FM2K::State::Memory::ROUND_TIMER_ADDR;
            uint32_t* round_number = (uint32_t*)0x470044;
            ImGui::Text("Round: %u | Timer: %u", *round_number, *round_timer);
            
            ImGui::EndTabItem();
        }
        
        // Additional Debug Tab
        if (ImGui::BeginTabItem("Misc")) {
            // RNG and other debug values
            uint32_t* rng_seed = (uint32_t*)0x41FB1C;
            uint32_t* game_paused = (uint32_t*)0x4701BC;
            uint32_t* replay_mode = (uint32_t*)0x4701C0;
            
            ImGui::Text("RNG Seed: 0x%08X", *rng_seed);
            ImGui::Text("Game Paused: %s", *game_paused ? "YES" : "NO");
            ImGui::Text("Replay Mode: %s", *replay_mode ? "YES" : "NO");
            
            ImGui::Separator();
            
            // Camera info
            uint32_t* cam_x = (uint32_t*)0x447F2C;
            uint32_t* cam_y = (uint32_t*)0x447F30;
            ImGui::Text("Camera Position: (%d, %d)", *cam_x, *cam_y);
            
            ImGui::EndTabItem();
        }
        
        ImGui::EndTabBar();
    }
    
    ImGui::End();
}

// Hook implementations
HRESULT APIENTRY Hook_EndScene(LPDIRECT3DDEVICE9 pDevice) {
    // EndScene hook is working - no debug logging needed
    
    // Render ImGui overlay when visible (exactly like working example)
    if (g_overlay_visible) {
        if (!g_imgui_initialized) {
            g_imgui_initialized = true;
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            
            // Use the same game window that we hooked for WndProc
            if (!g_game_window) {
                g_game_window = FindWindowA(nullptr, "WonderfulWorld");
                if (!g_game_window) {
                    g_game_window = GetForegroundWindow();
                }
            }
            
            char window_title[256] = {0};
            if (g_game_window) {
                GetWindowTextA(g_game_window, window_title, sizeof(window_title));
            }
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ImGui D3D9 initializing - window: 0x%p, title: '%s'", g_game_window, window_title);
            
            // Hook the window procedure RIGHT HERE when we have the correct window
            if (g_game_window && !oWndProc) {
                oWndProc = (WNDPROC)SetWindowLongPtr(g_game_window, GWLP_WNDPROC, (LONG_PTR)Hook_WndProc);
                if (oWndProc) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "WndProc: Successfully hooked CORRECT window 0x%p ('%s')", 
                               g_game_window, window_title);
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "WndProc: Failed to hook window 0x%p ('%s')", 
                                g_game_window, window_title);
                }
            }
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ImGui D3D9 - WndProc hook status: %s", oWndProc ? "HOOKED" : "NOT HOOKED");
            
            ImGui_ImplWin32_Init(g_game_window);
            ImGui_ImplDX9_Init(pDevice);
            
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ImGui D3D9 overlay initialized successfully");
        }
        
        // CURSOR FIX: Unclip cursor for ImGui interaction (visibility handled by NOP patch)
        ClipCursor(nullptr);        // Remove cursor clipping
        
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        // Render our debug overlay instead of demo window for production use
        RenderDebugOverlay();
        
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    } else if (g_imgui_initialized) {
        // CURSOR RESTORE: When overlay is hidden, restore FM2K's cursor clipping behavior
        RECT clip_rect = {100, 100, 101, 101};  // 1x1 pixel area like FM2K does
        ClipCursor(&clip_rect);
        // Cursor visibility handled by NOP patch - always visible now
    }
    
    return EndScene_orig(pDevice);
}

HRESULT APIENTRY Hook_Reset(LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Hook_Reset called");
    // Simplified Reset hook like working example - just call original
    return Reset_orig(pDevice, pPresentationParameters);
}

HRESULT APIENTRY Hook_Present(LPDIRECT3DDEVICE9 pDevice, CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
    // Let EndScene handle ImGui rendering
    return Present_orig(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT APIENTRY Hook_DrawIndexedPrimitive(LPDIRECT3DDEVICE9 pDevice, D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) {
    return DrawIndexedPrimitive_orig(pDevice, Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
}

LRESULT WINAPI Hook_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Let ImGui handle input when overlay is visible
    if (g_overlay_visible && g_imgui_initialized) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
            return true;
        }
    }
    
    return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

// D3D9 rendering handles everything - no fallback needed when hooks work properly

DWORD WINAPI DirectXInit(LPVOID lpParameter) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DirectXInit thread started...");
    
    // DXWRAPPER APPROACH: Wait for d3d9.dll (dxwrapper translates DDraw->D3D9)
    int wait_attempts = 0;
    while (!GetModuleHandleA("d3d9.dll") && wait_attempts < 50) {
        Sleep(100);
        wait_attempts++;
    }
    
    if (!GetModuleHandleA("d3d9.dll")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "d3d9.dll not found - make sure game is running in DDraw->D3D9 mode");
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Found d3d9.dll after %d attempts - dxwrapper/ddraw->D3D9 translation active");
    }
    
    // Create temporary window for D3D9 device creation
    HWND tmpWnd = CreateWindowA("BUTTON", "TempD3D", WS_SYSMENU | WS_MINIMIZEBOX, 
                                CW_USEDEFAULT, CW_USEDEFAULT, 300, 300, 
                                nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!tmpWnd) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create temp window");
        return false;
    }
    
    // Create D3D9
    LPDIRECT3D9 d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        DestroyWindow(tmpWnd);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create D3D9");
        return false;
    }
    
    // Create device with different parameters to avoid conflicts
    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = tmpWnd;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    d3dpp.BackBufferWidth = 1;
    d3dpp.BackBufferHeight = 1;
    d3dpp.EnableAutoDepthStencil = FALSE;
    
    LPDIRECT3DDEVICE9 d3ddev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, tmpWnd, 
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &d3ddev);
    if (hr != D3D_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create D3D9 device, HRESULT: 0x%08X", hr);
        d3d->Release();
        DestroyWindow(tmpWnd);
        return false;
    }
    
    // Get vtable
    DWORD* vtable = *(DWORD**)d3ddev;
    
    // DEBUG: Log vtable addresses we're about to hook
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "D3D9 vtable addresses:");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  EndScene[42] = 0x%08X", vtable[42]);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Reset[16] = 0x%08X", vtable[16]);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Present[17] = 0x%08X", vtable[17]);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  DrawIndexedPrimitive[82] = 0x%08X", vtable[82]);
    
    // DXWRAPPER SUCCESS: These vtable addresses should be called when game uses DDraw->D3D9 mode
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "D3D9 vtable hooks targeting dxwrapper's DirectDraw->D3D9 translation layer");
    
    // Get function pointers from vtable (like working example)
    EndScene_orig = (EndScene_t)vtable[42];
    DrawIndexedPrimitive_orig = (DrawIndexedPrimitive_t)vtable[82];
    Reset_orig = (Reset_t)vtable[16];
    Present_orig = (Present_t)vtable[17];
    
    // MinHook is already initialized by main hook system, so skip MH_Initialize()
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Installing D3D9 vtable hooks...");
    
    // Hook EndScene (exactly like working example)
    if (MH_CreateHook((DWORD_PTR*)vtable[42], (LPVOID)Hook_EndScene, reinterpret_cast<void**>(&EndScene_orig)) != MH_OK) { 
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create EndScene hook");
        return 0;
    }
    if (MH_EnableHook((DWORD_PTR*)vtable[42]) != MH_OK) { 
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable EndScene hook");
        return 0;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "EndScene hook installed at vtable[42]");
    
    // Hook DrawIndexedPrimitive (exactly like working example)
    if (MH_CreateHook((DWORD_PTR*)vtable[82], (LPVOID)Hook_DrawIndexedPrimitive, reinterpret_cast<void**>(&DrawIndexedPrimitive_orig)) != MH_OK) { 
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create DrawIndexedPrimitive hook");
        return 0;
    }
    if (MH_EnableHook((DWORD_PTR*)vtable[82]) != MH_OK) { 
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable DrawIndexedPrimitive hook");
        return 0;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DrawIndexedPrimitive hook installed at vtable[82]");
    
    // Hook Reset (exactly like working example)
    if (MH_CreateHook((DWORD_PTR*)vtable[16], (LPVOID)Hook_Reset, reinterpret_cast<void**>(&Reset_orig)) != MH_OK) { 
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create Reset hook");
        return 0;
    }
    if (MH_EnableHook((DWORD_PTR*)vtable[16]) != MH_OK) { 
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable Reset hook");
        return 0;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Reset hook installed at vtable[16]");
    
    // Hook Present (vtable[17]) - dxwrapper might use this instead of EndScene
    if (MH_CreateHook((DWORD_PTR*)vtable[17], (LPVOID)Hook_Present, reinterpret_cast<void**>(&Present_orig)) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create Present hook");
        return 0;
    }
    if (MH_EnableHook((DWORD_PTR*)vtable[17]) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable Present hook");
        return 0;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Present hook installed at vtable[17]");
    
    // Cleanup
    d3ddev->Release();
    d3d->Release();
    DestroyWindow(tmpWnd);
    
    // Window procedure hook is now done in EndScene when ImGui initializes
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "WndProc: Hook will be installed during ImGui initialization");
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "D3D9 ImGui overlay hooks installed successfully - Press F9 to toggle");
    
    return 1;
}

bool InitializeImGuiOverlay() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating DirectX init thread for D3D9 hooking...");
    
    // Create thread exactly like working example - target D3D9 when game uses DDraw->D3D9 mode
    HANDLE thread = CreateThread(nullptr, 0, DirectXInit, GetModuleHandle(nullptr), 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
        return true;
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create DirectX init thread");
        return false;
    }
}

void ShutdownImGuiOverlay() {
    if (g_imgui_initialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui_initialized = false;
    }
    
    // Restore window proc
    if (g_game_window && oWndProc) {
        SetWindowLongPtr(g_game_window, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    }
}

bool IsOverlayVisible() {
    return g_overlay_visible;
}

void ToggleOverlay() {
    g_overlay_visible = !g_overlay_visible;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ImGui overlay %s", g_overlay_visible ? "shown" : "hidden");
}

void CheckOverlayHotkey() {
    // Check F9 key using GetAsyncKeyState like the other hotkeys
    bool key_f9_current = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (key_f9_current && !g_f9_key_pressed) {
        // Lazy initialization - install hooks on first F9 press
        if (!g_hooks_installed) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "F9 pressed - installing ImGui hooks lazily...");
            if (InitializeImGuiOverlay()) {
                g_hooks_installed = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ImGui hooks installed successfully");
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to install ImGui hooks");
                g_f9_key_pressed = key_f9_current; // Prevent retry spam
                return;
            }
        }
        ToggleOverlay();
    }
    g_f9_key_pressed = key_f9_current;
    
    // D3D9 HOOKS: Let the actual D3D9 hooks handle rendering when called
    // No fallback rendering needed - D3D9 hooks should work when game is in DDraw->D3D9 mode
}