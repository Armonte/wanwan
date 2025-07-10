#include "fm2k_hook.h"
#include "ipc.h"
#include "state_manager.h"
#include "sdl3_context.h"
#include "directdraw_compat.h"
#include "window_hooks.h"
#include <SDL3/SDL.h>
#include <MinHook.h>

namespace FM2K {
namespace Hooks {

// Original function pointers
static ProcessGameInputsFn original_process_inputs = nullptr;
static UpdateGameStateFn original_update_game = nullptr;
static RNGFn original_rng = nullptr;

// SDL3 integration function pointers
typedef int (__cdecl *InitializeGameFn)();
typedef int (__cdecl *InitializeDirectDrawFn)(int isFullScreen, void* windowHandle);
typedef LRESULT (__stdcall *WindowProcFn)(HWND, UINT, WPARAM, LPARAM);

static InitializeGameFn original_initialize_game = nullptr;
static InitializeDirectDrawFn original_initialize_directdraw = nullptr;
static WindowProcFn original_window_proc = nullptr;

// Global frame counter
static SDL_AtomicInt g_frame_counter;
static bool g_frame_counter_initialized = false;

// Hook implementations
int __cdecl Hook_ProcessGameInputs() {
    // Initialize frame counter if needed
    if (!g_frame_counter_initialized) {
        SDL_SetAtomicInt(&g_frame_counter, 0);
        g_frame_counter_initialized = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Frame counter initialized");
    }
    
    // Increment global frame counter first
    SDL_AddAtomicInt(&g_frame_counter, 1);
    uint32_t current_frame = SDL_GetAtomicInt(&g_frame_counter);
    
    // Log ALL calls for now to track execution
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
        "[Hook] process_game_inputs ENTRY - frame %u", current_frame);

    // Call original function to ensure game state is updated
    SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, 
        "[Hook] Calling original process_game_inputs at %p", original_process_inputs);
    
    if (original_process_inputs) {
        original_process_inputs();
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] Original process_game_inputs returned successfully");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] original_process_inputs is NULL!");
    }

    // Capture player inputs and send via IPC
    uint16_t p1_input = 0;
    uint16_t p2_input = 0;
    
    // Read current player inputs from FM2K memory
    HANDLE current_process = GetCurrentProcess();
    SIZE_T bytes_read;
    
    if (ReadProcessMemory(current_process, reinterpret_cast<LPCVOID>(0x470100), 
                         &p1_input, sizeof(uint16_t), &bytes_read) && bytes_read == sizeof(uint16_t)) {
        // P1 input read successfully
    } else {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Failed to read P1 input");
    }
    
    if (ReadProcessMemory(current_process, reinterpret_cast<LPCVOID>(0x470300), 
                         &p2_input, sizeof(uint16_t), &bytes_read) && bytes_read == sizeof(uint16_t)) {
        // P2 input read successfully  
    } else {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Failed to read P2 input");
    }
    
    // Send input event via IPC
    IPC::Event input_event;
    input_event.type = IPC::EventType::INPUT_CAPTURED;
    input_event.frame_number = current_frame;
    input_event.timestamp_ms = SDL_GetTicks();
    input_event.data.input.p1_input = p1_input;
    input_event.data.input.p2_input = p2_input;
    input_event.data.input.frame_number = current_frame;
    
    if (!IPC::PostEvent(input_event)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to post input event for frame %u", current_frame);
    }
    
    // Notify launcher that frame has advanced using SDL3 events  
    SDL_Event sdl_event;
    SDL_zero(sdl_event);
    sdl_event.type = SDL_EVENT_USER;
    sdl_event.user.code = 1; // FRAME_ADVANCED
    sdl_event.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(current_frame));
    sdl_event.user.data2 = reinterpret_cast<void*>(static_cast<uintptr_t>(SDL_GetTicks()));
    
    if (!SDL_PushEvent(&sdl_event)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to push frame advanced event for frame %u: %s", 
            current_frame, SDL_GetError());
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
        "[Hook] process_game_inputs EXIT - frame %u", current_frame);
    
    // Return the same value as original function
    return 0; // Original returns result * 4, but we'll return 0 for safety
}

int __cdecl Hook_UpdateGameState() {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "[Hook] update_game_state ENTRY");
    
    // Call original first
    SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, 
        "[Hook] Calling original update_game_state at %p", original_update_game);
        
    if (original_update_game) {
        original_update_game();
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] Original update_game_state returned successfully");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] original_update_game is NULL!");
    }

    // Save state if needed
    if (ShouldSaveState()) {
        // TODO: Implement actual state saving when state manager is ready
        uint32_t current_frame = GetFrameNumber();
        uint32_t checksum = 0x12345678; // Placeholder checksum
        
        // Notify launcher of state save using SDL3 events
        SDL_Event sdl_event;
        SDL_zero(sdl_event);
        sdl_event.type = SDL_EVENT_USER;
        sdl_event.user.code = 2; // STATE_SAVED
        sdl_event.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(current_frame));
        sdl_event.user.data2 = reinterpret_cast<void*>(static_cast<uintptr_t>(checksum));
        
        if (!SDL_PushEvent(&sdl_event)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Failed to push state saved event: %s", SDL_GetError());
        }
    }

    // Check for visual state changes
    if (VisualStateChanged()) {
        uint32_t current_frame = GetFrameNumber();
        
        // Notify launcher of visual state change using SDL3 events
        SDL_Event sdl_event;
        SDL_zero(sdl_event);
        sdl_event.type = SDL_EVENT_USER;
        sdl_event.user.code = 3; // VISUAL_STATE_CHANGED
        sdl_event.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(current_frame));
        sdl_event.user.data2 = nullptr;
        
        if (!SDL_PushEvent(&sdl_event)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Failed to push visual state changed event: %s", SDL_GetError());
        }
    }
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "[Hook] update_game_state EXIT");
    
    // Return appropriate value (original returns an int)
    return 0;
}

int __cdecl Hook_GameRand() {
    SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "[Hook] game_rand ENTRY");
    
    // Call original RNG
    int result = 0;
    if (original_rng) {
        result = original_rng();
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] Original game_rand returned: %d", result);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "[Hook] original_rng is NULL!");
    }

    // Log RNG calls for debugging
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "RNG called at frame %u, result: %d",
        GetFrameNumber(), result);

    SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "[Hook] game_rand EXIT");
    return result;
}

// SDL3 Integration Hooks

static int __cdecl Hook_InitializeGame() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[Hook] initialize_game ENTRY");
    
    // Call original function first to set up window class and basic resources
    int result = 0;
    if (original_initialize_game) {
        result = original_initialize_game();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[Hook] Original initialize_game returned: %d", result);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[Hook] original_initialize_game is NULL!");
        return -1;
    }
    
    // Initialize SDL3 context after the game has set up its window
    using namespace SDL3Integration;
    if (!g_sdlContext.initialized) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[Hook] Initializing SDL3 context...");
        if (!InitializeSDL3Context(0, nullptr)) {  // Start in windowed mode
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[Hook] Failed to initialize SDL3 context");
            return -1;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[Hook] SDL3 context initialized successfully");
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[Hook] initialize_game EXIT");
    return result;
}

static int __cdecl Hook_InitializeDirectDraw(int isFullScreen, void* windowHandle) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "[Hook] initialize_directdraw_mode ENTRY - isFullScreen=%d, windowHandle=%p", 
        isFullScreen, windowHandle);
    
    // Instead of calling the original DirectDraw function, use our SDL3 replacement
    using namespace DirectDrawCompat;
    int result = initDirectDraw_new(isFullScreen, windowHandle);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        "[Hook] SDL3 DirectDraw replacement returned: %d", result);
    
    return result;
}

static LRESULT __stdcall Hook_WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Forward to SDL3 window procedure for proper message handling
    using namespace SDL3Integration;
    
    // Let SDL3 handle events first
    UpdateSDL3Events();
    
    // Check for Alt+Enter fullscreen toggle
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        if (GetAsyncKeyState(VK_MENU) & 0x8000) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[Hook] Alt+Enter detected in window proc");
            ToggleFullscreen();
            return 0;
        }
    }
    
    // Call original window procedure
    if (original_window_proc) {
        return original_window_proc(hWnd, uMsg, wParam, lParam);
    }
    
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

bool Init(HANDLE process) {
    if (!process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle");
        return false;
    }

    // Set SDL logging to maximum verbosity to catch everything
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL logging set to verbose mode");

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initializing FM2K hooks...");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Process handle: %p", process);

    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize MinHook");
        return false;
    }

    // Initialize IPC system
    if (!IPC::Init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize IPC");
        MH_Uninitialize();
        return false;
    }

    // Initialize state manager
    if (!State::Init(process)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize state manager");
        IPC::Shutdown();
        MH_Uninitialize();
        return false;
    }
    
    // Initialize window hooks for window hijacking
    if (!WindowHooks::InitializeWindowHooks()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize window hooks");
        State::Shutdown();
        IPC::Shutdown();
        MH_Uninitialize();
        return false;
    }

    // Create hooks using correct addresses from IDA Pro analysis of WonderfulWorld
    
    // Hook initialize_game function for window creation and SDL3 setup
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating hook for initialize_game at 0x4056C0");
    MH_STATUS status1 = MH_CreateHook(reinterpret_cast<LPVOID>(0x4056C0),  // initialize_game
                                      reinterpret_cast<LPVOID>(Hook_InitializeGame),
                                      reinterpret_cast<LPVOID*>(&original_initialize_game));
    if (status1 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create initialize_game hook: %d", status1);
        MH_Uninitialize();
        return false;
    }
    
    // Hook initialize_directdraw_mode for DirectDraw replacement
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating hook for initialize_directdraw_mode at 0x404980");
    MH_STATUS status2 = MH_CreateHook(reinterpret_cast<LPVOID>(0x404980),  // initialize_directdraw_mode
                                      reinterpret_cast<LPVOID>(Hook_InitializeDirectDraw),
                                      reinterpret_cast<LPVOID*>(&original_initialize_directdraw));
    if (status2 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create initialize_directdraw_mode hook: %d", status2);
        MH_Uninitialize();
        return false;
    }
    
    // Hook main window procedure for message forwarding
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating hook for main_window_proc at 0x405F50");
    MH_STATUS status3 = MH_CreateHook(reinterpret_cast<LPVOID>(0x405F50),  // main_window_proc
                                      reinterpret_cast<LPVOID>(Hook_WindowProc),
                                      reinterpret_cast<LPVOID*>(&original_window_proc));
    if (status3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create main_window_proc hook: %d", status3);
        MH_Uninitialize();
        return false;
    }
    
    // Hook update_game_state for rollback integration
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating hook for update_game_state at 0x404CD0");
    MH_STATUS status4 = MH_CreateHook(reinterpret_cast<LPVOID>(0x404CD0),  // update_game_state
                                      reinterpret_cast<LPVOID>(Hook_UpdateGameState),
                                      reinterpret_cast<LPVOID*>(&original_update_game));
    if (status4 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create update_game_state hook: %d", status4);
        MH_Uninitialize();
        return false;
    }
    
    // Hook process_input_history for rollback input handling
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating hook for process_input_history at 0x4025A0");
    MH_STATUS status5 = MH_CreateHook(reinterpret_cast<LPVOID>(0x4025A0),  // process_input_history
                                      reinterpret_cast<LPVOID>(Hook_ProcessGameInputs),
                                      reinterpret_cast<LPVOID*>(&original_process_inputs));
    if (status5 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create process_input_history hook: %d", status5);
        MH_Uninitialize();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "All hooks created successfully");

    // Enable hooks
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable hooks");
        State::Shutdown();
        IPC::Shutdown();
        MH_Uninitialize();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K hooks installed successfully");
    
    // Send initialization confirmation event to launcher
    SDL_Event sdl_event;
    SDL_zero(sdl_event);
    sdl_event.type = SDL_EVENT_USER;
    sdl_event.user.code = 0; // HOOKS_INITIALIZED
    sdl_event.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(1)); // Success
    sdl_event.user.data2 = nullptr;
    
    if (!SDL_PushEvent(&sdl_event)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to push hooks initialized event: %s", SDL_GetError());
    }
    
    return true;
}

void Shutdown() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Shutting down FM2K hooks...");
    
    // Cleanup SDL3 context first
    using namespace SDL3Integration;
    CleanupSDL3Context();
    
    // Shutdown window hooks
    WindowHooks::ShutdownWindowHooks();
    
    // Disable and remove hooks
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    
    // Shutdown subsystems
    State::Shutdown();
    IPC::Shutdown();
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FM2K hooks shutdown complete");
}

uint32_t GetFrameNumber() {
    if (!g_frame_counter_initialized) {
        return 0;
    }
    return SDL_GetAtomicInt(&g_frame_counter);
}

bool ShouldSaveState() {
    // For now, save state every frame for testing
    // Later, optimize based on:
    // - Input changes
    // - Critical game state changes
    // - Network prediction window
    return true;
}

bool VisualStateChanged() {
    static uint32_t last_effect_flags = 0;
    uint32_t current_effect_flags;

    // Read effect flags from 0x40CC30 (from research doc)
    if (ReadProcessMemory(GetCurrentProcess(), 
                         (LPCVOID)0x40CC30,
                         &current_effect_flags,
                         sizeof(current_effect_flags),
                         nullptr)) {
        if (current_effect_flags != last_effect_flags) {
            last_effect_flags = current_effect_flags;
            return true;
        }
    }
    return false;
}

} // namespace Hooks
} // namespace FM2K 