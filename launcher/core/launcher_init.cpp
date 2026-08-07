// launcher_init.cpp -- FM2KLauncher construction/teardown + SDL/ImGui bring-up,
// split out of FM2K_RollbackClient.cpp. Pure move of member functions (the
// class decl stays in FM2K_Integration.h, so no internal header is needed):
//   - FM2KLauncher ctor/dtor
//   - Initialize()      : subsystem bring-up + UI-callback wiring + first scan
//   - InitializeSDL()   : window/renderer/ImGui/app-icon setup
//   - Shutdown()        : teardown (clients, UI, SDL, MinHook)
#include "SDL3/SDL.h"
#include <SDL3_image/SDL_image.h>
#include "app_icon_data.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "MinHook.h"
#include "FM2K_GameInstance.h"
#include "FM2K_Integration.h"
#include "game_discovery.h"  // game scan/cache/sniff + async discovery (moved out of this file)
#include "FM2K_GameIni.h"
#include "FM2K_Utf8Path.h"
#include "FM2KHook/src/ui/shared_mem.h"
#include "FM2KHook/src/ui/input_binder.h"  // RefreshGamepads() on SDL hot-plug events
#include "FM2KHook/src/netplay/spec_relay_queue.h"  // hub-relay outbound drain (Phase 2c)
#define XXH_INLINE_ALL
#include "vendored/xxhash/xxhash.h"
#include "LocalSession.h"
#include "OnlineSession.h"
#include "FM2K_PortMapper.h"  // --upnp-test self-contained router validation

#include <chrono>
#include <string>
#include <cstring>
#include <cstdlib>  // std::getenv for FM2K_FULL_CRCS perf-run override
#include <optional>
#include <vector>
#include <iostream>
#include <thread>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <system_error>

// FM2KLauncher Implementation
FM2KLauncher::FM2KLauncher() 
    : window_(nullptr)
    , renderer_(nullptr)
    , current_state_(LauncherState::GameSelection)
    , running_(true) {
    // Register the custom event type exactly once per process.
    if (g_event_discovery_complete == 0) {
        g_event_discovery_complete = SDL_RegisterEvents(1);
        if (g_event_discovery_complete == (Uint32)-1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to register discovery completion event: %s", SDL_GetError());
        }
    }

    discovery_thread_ = nullptr;
    discovery_in_progress_ = false;
    // Initialize multi-client testing
    client1_process_id_ = 0;
    client2_process_id_ = 0;
    
    // Initialize GekkoNet session management
    
    // Load saved games directories (if any) so they can be used before
    // Initialize() completes. Old single-string configs migrate naturally
    // because the persistence format is one path per line.
    games_root_paths_ = Utils::LoadGamesRootPaths();
}

FM2KLauncher::~FM2KLauncher() {
    Shutdown();
}

bool FM2KLauncher::Initialize() {
    // Set log priorities using SDL_SetLogPriority instead of SDL_LogSetPriority
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_DEBUG);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_ERROR, SDL_LOG_PRIORITY_DEBUG);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_INFO);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_INFO);
    
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initializing FM2K Launcher...");

    if (!InitializeSDL()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize SDL3: %s", SDL_GetError());
        return false;
    }
    
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize MinHook");
        return false;
    }
    
    // Create subsystems
    ui_ = std::make_unique<LauncherUI>();
    if (!ui_->Initialize(window_, renderer_)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize UI");
        return false;
    }
    
    // Connect UI callbacks to launcher logic (wired in launcher_callbacks.cpp)
    WireUICallbacks();

    // If no games directories stored, default to the launcher's own dir.
    if (games_root_paths_.empty()) {
        std::string base_path;
        if (const char *sdl_base = SDL_GetBasePath()) {
            base_path = sdl_base;
            SDL_free(const_cast<char *>(sdl_base));
        } else {
            const char* cwd = SDL_GetCurrentDirectory();
            base_path = cwd ? cwd : "";
            if (cwd) SDL_free(const_cast<char*>(cwd));
        }
        // Remove trailing slash if present (we want the directory itself, not a subdirectory)
        if (!base_path.empty() && (base_path.back() == '/' || base_path.back() == '\\')) {
            base_path.pop_back();
        }
        if (!base_path.empty()) {
            games_root_paths_.push_back(base_path);
        }
    }

    // Cache-first display: load the full cached game list (xxh64, engine,
    // kgt summary, …) so the UI is fully populated immediately. Async
    // discovery still runs to catch newly installed/removed games, but
    // it's invisible to the user when nothing has changed.
    bool seeded_from_cache = false;
    {
        auto cached_games = Utils::LoadGameCache();
        if (!cached_games.empty()) {
            ui_->SetGames(cached_games);
            // Mirror into our internal state so FindKgtByGameId / launch
            // paths see the cached games before async discovery completes.
            discovered_games_ = std::move(cached_games);
            seeded_from_cache = true;
        }
        ui_->SetGamesRootPaths(games_root_paths_);
    }
    // Suppress the "Scanning…" spinner when the cache already filled the
    // list -- the background walk just verifies nothing changed.
    StartAsyncDiscovery(/*show_spinner=*/!seeded_from_cache);
    
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Launcher initialized successfully");
    //SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Found %d FM2K games", (int)discovered_games_.size());
    
    return true;
}

bool FM2KLauncher::InitializeSDL() {
    // Hints MUST be set before the gamepad subsystem starts. SDL3
    // reads HIDAPI/RawInput hints when it stands up the joystick
    // backend, NOT lazily -- setting them later (e.g. inside the
    // input binder's Init()) is a no-op. Without HIDAPI_PS3 the
    // PS3 controller (and Qanba sticks in PS3 mode) fall through to
    // a generic HID joystick path that has no SDL gamepad mapping,
    // so SDL_GetGamepadButton sees nothing and the binder ignores
    // every press. Mirrors revolve_input_sdl3's setup order.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI,         "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH,  "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT,       "1");

    // Initialize SDL with all necessary subsystems
    SDL_InitFlags init_flags = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;

    if (!SDL_Init(init_flags)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    // Gamepad events flow into SDL's event queue and update polled
    // state; the binder reads the polled state via SDL_GetGamepadButton.
    // Without this enabled, polled state never refreshes on Windows.
    SDL_SetGamepadEventsEnabled(true);
    
    // Create window with SDL_Renderer graphics context
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    
    window_ = SDL_CreateWindow("FM2K Rollback Launcher", 
        (int)(1280 * main_scale), (int)(720 * main_scale), 
        window_flags);
        
    if (!window_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window_);
        SDL_Quit();
        return false;
    }
    // Vsync is the framerate cap. If it silently fails (driver fallback,
    // headless / RDP session, software renderer), the launcher would spin
    // at hundreds of fps and burn CPU/GPU -- exactly the symptom users
    // reported on Xeon E3 / 3060 (~20% CPU + ~20% GPU at idle). Log the
    // result, and stash a flag so SDL_AppIterate can soft-cap to ~60fps
    // via SDL_DelayNS when vsync is unavailable.
    if (!SDL_SetRenderVSync(renderer_, 1)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
            "SDL_SetRenderVSync(1) failed: %s -- falling back to software cap",
            SDL_GetError());
        vsync_available_ = false;
    } else {
        int v = 0;
        if (SDL_GetRenderVSync(renderer_, &v) && v == 1) {
            vsync_available_ = true;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                "SDL_GetRenderVSync reports vsync=%d -- assuming off, software-capping",
                v);
            vsync_available_ = false;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_RENDER,
        "Renderer: '%s', vsync=%s",
        SDL_GetRendererName(renderer_) ? SDL_GetRendererName(renderer_) : "?",
        vsync_available_ ? "on" : "off (software cap)");

    // App icon. We try paths first (so a future assets/icon.bmp drop-in
    // overrides without a rebuild), then fall back to an embedded
    // base64-decoded PNG (placeholder smiley). If both fail, draw a
    // 32×32 blue square. SDL3_image is statically linked so IMG_Load_IO
    // handles the PNG without an external SDL_image.dll.
    SDL_Surface* icon = nullptr;
    const char* icon_paths[] = {
        "assets/icon.bmp",
        "icon.bmp",
        "../icon.bmp"
    };

    for (const char* path : icon_paths) {
        icon = SDL_LoadBMP(path);
        if (icon) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Loaded icon from: %s", path);
            break;
        }
    }

    if (!icon) {
        // Decode the embedded base64 PNG. Decoder is short and self-
        // contained -- pulling SDL_base64 would mean wiring another
        // header path, not worth it for a one-shot at startup.
        const char* b64 = fm2k::kAppIconBase64;
        const size_t b64_len = std::strlen(b64);
        std::vector<uint8_t> png_bytes;
        png_bytes.reserve((b64_len * 3) / 4 + 4);
        auto val = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        uint32_t buf = 0;
        int      bits = 0;
        for (size_t i = 0; i < b64_len; ++i) {
            const char c = b64[i];
            if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
                if (c == '=') break;
                continue;
            }
            const int v = val(c);
            if (v < 0) continue;
            buf = (buf << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                png_bytes.push_back((uint8_t)((buf >> bits) & 0xFFu));
            }
        }
        SDL_IOStream* io = SDL_IOFromConstMem(png_bytes.data(), png_bytes.size());
        if (io) {
            icon = IMG_Load_IO(io, /*closeio=*/true);
            if (icon) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Loaded embedded smiley icon (%zu bytes PNG)",
                            png_bytes.size());
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "IMG_Load_IO failed for embedded icon: %s",
                            SDL_GetError());
            }
        }
    }

    // If still no icon, draw a 32×32 blue square as final fallback.
    if (!icon) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No icon file found, creating default icon");
        icon = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_RGBA32);
        if (icon) {
            // Create a solid blue color (R=0, G=120, B=215, A=255)
            Uint8* pixels = (Uint8*)icon->pixels;
            int pitch = icon->pitch;
            SDL_LockSurface(icon);
            for (int y = 0; y < 32; y++) {
                for (int x = 0; x < 32; x++) {
                    Uint32* pixel = (Uint32*)(pixels + y * pitch + x * 4);
                    *pixel = 0x0078D7FF; // RGBA packed value for Windows blue
                }
            }
            SDL_UnlockSurface(icon);
        }
    }

    // Set window icon if we have one
    if (icon) {
        SDL_SetWindowIcon(window_, icon);

        // Console window (conhost) inherits a generic icon when we attach
        // via AllocConsole / parent inheritance. Convert the SDL surface
        // to an HICON and SendMessage(WM_SETICON) to the console window
        // so the smiley shows up in the title bar + Alt-Tab. Skipped if
        // there's no console (launcher started without one -- then
        // GetConsoleWindow returns NULL).
        if (HWND console_hwnd = GetConsoleWindow()) {
            // Normalize to RGBA32 -- DIB section we hand to CreateIconIndirect
            // expects 32-bit BGRA top-down. ConvertSurface returns NULL on
            // mismatch but a fresh copy on success; we own it.
            SDL_Surface* rgba = SDL_ConvertSurface(icon, SDL_PIXELFORMAT_BGRA32);
            if (rgba) {
                SDL_LockSurface(rgba);
                BITMAPV5HEADER bi = {};
                bi.bV5Size        = sizeof(bi);
                bi.bV5Width       = rgba->w;
                bi.bV5Height      = -rgba->h;          // top-down
                bi.bV5Planes      = 1;
                bi.bV5BitCount    = 32;
                bi.bV5Compression = BI_BITFIELDS;
                bi.bV5RedMask     = 0x00FF0000;
                bi.bV5GreenMask   = 0x0000FF00;
                bi.bV5BlueMask    = 0x000000FF;
                bi.bV5AlphaMask   = 0xFF000000;
                HDC screen_dc = GetDC(nullptr);
                void* dib_pixels = nullptr;
                HBITMAP color_bmp = CreateDIBSection(
                    screen_dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                    &dib_pixels, nullptr, 0);
                ReleaseDC(nullptr, screen_dc);
                if (color_bmp && dib_pixels) {
                    std::memcpy(dib_pixels, rgba->pixels,
                                (size_t)rgba->w * rgba->h * 4u);
                    HBITMAP mask_bmp = CreateBitmap(rgba->w, rgba->h, 1, 1, nullptr);
                    ICONINFO info = {};
                    info.fIcon    = TRUE;
                    info.hbmMask  = mask_bmp;
                    info.hbmColor = color_bmp;
                    HICON hicon = CreateIconIndirect(&info);
                    if (hicon) {
                        SendMessageW(console_hwnd, WM_SETICON,
                                     ICON_SMALL, (LPARAM)hicon);
                        SendMessageW(console_hwnd, WM_SETICON,
                                     ICON_BIG,   (LPARAM)hicon);
                        // Don't DestroyIcon -- the console keeps a reference
                        // for the lifetime of the window. Leaks one icon
                        // handle on launcher exit, which is fine.
                    }
                    if (mask_bmp) DeleteObject(mask_bmp);
                    DeleteObject(color_bmp);
                }
                SDL_UnlockSurface(rgba);
                SDL_DestroySurface(rgba);
            }
        }
    }

    // No tray icon - just a normal window application

    // Now we can destroy the surface
    if (icon) {
        SDL_DestroySurface(icon);
    }

    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);
    
    return true;
}

void FM2KLauncher::Shutdown() {
    // Terminate any running test clients
    TerminateAllClients();


    // Stop network and game first
    // DLL handles GekkoNet directly - no launcher-side session needed

    // Close spec hub-relay shared-mem mappings (Phase 4). Close handles
    // nullptr safely; hook side closes its end on DLL unload.
    if (spec_relay_out_ring_) {
        fm2k::spec_relay::Close(
            static_cast<fm2k::spec_relay::Ring*>(spec_relay_out_ring_));
        spec_relay_out_ring_ = nullptr;
    }
    if (spec_relay_in_ring_) {
        fm2k::spec_relay::Close(
            static_cast<fm2k::spec_relay::Ring*>(spec_relay_in_ring_));
        spec_relay_in_ring_ = nullptr;
    }

    if (game_instance_) {
        game_instance_->Terminate();
        game_instance_.reset();
    }
    
    // No tray icon to destroy
    
    // Ensure UI cleanup happens before ImGui shutdown
    if (ui_) {
        ui_->Shutdown();
        ui_.reset();
    }
    
    // SDL cleanup
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    
    // Make sure discovery thread is finished before quitting SDL
    if (discovery_thread_) {
        SDL_WaitThread(discovery_thread_, nullptr);
        discovery_thread_ = nullptr;
    }

    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    
    SDL_Quit();
    MH_Uninitialize();
}
