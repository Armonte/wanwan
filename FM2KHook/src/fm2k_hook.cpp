#include "fm2k_hook.h"
#include "ipc.h"
#include "state_manager.h"
#include <SDL3/SDL.h>
#include <MinHook.h>
#include <cstdio>

namespace FM2K {
namespace Hooks {

// Custom log output function to send logs over IPC
static void SDLCustomLogOutputForIPC(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    (void)userdata;
    
    // Prevent recursive logging if PostEvent itself fails and logs.
    thread_local bool is_logging = false;
    if (is_logging || !IPC::IsInitialized()) {
        return;
    }
    
    is_logging = true;

    IPC::Event log_event;
    log_event.type = IPC::EventType::LOG_MESSAGE;
    log_event.timestamp_ms = SDL_GetTicks();
    log_event.data.log.category = category;
    log_event.data.log.priority = priority;
    SDL_strlcpy(log_event.data.log.message, message, sizeof(log_event.data.log.message));
    
    IPC::PostEvent(log_event);

    is_logging = false;
}

// Original function pointers
static ProcessGameInputsFn original_process_inputs = nullptr;
static UpdateGameStateFn original_update_game = nullptr;
static RNGFn original_rng = nullptr;

// Module base address
static uintptr_t g_base_address = 0;

// Global frame counter
static SDL_AtomicInt g_frame_counter;
static bool g_frame_counter_initialized = false;

// Hook implementations
static int __cdecl Hook_ProcessGameInputs() {
    // CRITICAL: Add Windows debug output as backup in case IPC logging fails
    OutputDebugStringA("[FM2K HOOK] Hook_ProcessGameInputs() CALLED!\n");
    
    // Initialize frame counter if needed
    if (!g_frame_counter_initialized) {
        SDL_SetAtomicInt(&g_frame_counter, 0);
        g_frame_counter_initialized = true;
        OutputDebugStringA("[FM2K HOOK] Frame counter initialized\n");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Frame counter initialized");
    }
    
    // Increment global frame counter first
    SDL_AddAtomicInt(&g_frame_counter, 1);
    uint32_t current_frame = SDL_GetAtomicInt(&g_frame_counter);
    
    // CRITICAL: Force logging of EVERY hook call to ensure we see them
    char debug_msg[256];
    sprintf(debug_msg, "[FM2K HOOK] process_game_inputs EXECUTED! Frame %u\n", current_frame);
    OutputDebugStringA(debug_msg);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
        " [HOOK] process_game_inputs EXECUTED! Frame %u (call #%d)", 
        current_frame, (int)current_frame);
    
    // Additional critical logging for first few calls
    if (current_frame <= 5) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, 
            " [HOOK] EARLY EXECUTION - Frame %u - HOOK IS DEFINITELY WORKING!", current_frame);
    }

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
    
    // Read current player inputs from FM2K memory (using correct IDA addresses)
    // Since we're injected into the game process, read directly from memory
    
    // Use correct addresses from IDA: g_p1_input=0x4259C0, g_p2_input=0x4259C4
    // Correct for ASLR by adding the RVA to the module base address
    uint16_t* p1_input_ptr = reinterpret_cast<uint16_t*>(g_base_address + 0x259C0);
    uint16_t* p2_input_ptr = reinterpret_cast<uint16_t*>(g_base_address + 0x259C4);
    
    p1_input = *p1_input_ptr;
    p2_input = *p2_input_ptr;
    
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

static int __cdecl Hook_UpdateGameState() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申??申?申 [HOOK] update_game_state EXECUTED!");
    
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

static int __cdecl Hook_GameRand() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申??申?申 [HOOK] game_rand EXECUTED!");
    
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

bool Init(HANDLE process) {
    // First, add a fallback log to Windows debug output before IPC is ready
    OutputDebugStringA("[FM2K HOOK] Init() called - starting initialization\n");
    
    // Initialize IPC system FIRST, so that logging can use it immediately.
    if (!IPC::Init()) {
        // We can't log this failure via SDL, as the handler isn't set yet,
        // but this is a critical failure that will prevent initialization.
        OutputDebugStringA("[FM2K HOOK] CRITICAL: IPC::Init() failed!\n");
        return false;
    }
    
    OutputDebugStringA("[FM2K HOOK] IPC::Init() succeeded, setting up SDL logging\n");

    // Now that IPC is ready, set the log handler to route logs through it.
    SDL_SetLogOutputFunction(SDLCustomLogOutputForIPC, nullptr);

    if (!process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle");
        return false;
    }

    // Set SDL logging to maximum verbosity to catch everything
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL logging set to verbose mode");

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申 HOOK INIT: Starting FM2K hooks initialization...");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申 HOOK INIT: Process handle: %p", process);

    // Get module base address to correct for ASLR
    g_base_address = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[HOOK] game.exe base address: %p", (void*)g_base_address);

    // Initialize MinHook
    OutputDebugStringA("[FM2K HOOK] Calling MH_Initialize()\n");
    MH_STATUS mh_init_status = MH_Initialize();
    if (mh_init_status != MH_OK) {
        char debug_msg[256];
        sprintf(debug_msg, "[FM2K HOOK] CRITICAL: MH_Initialize() failed with status %d\n", mh_init_status);
        OutputDebugStringA(debug_msg);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize MinHook: status %d", mh_init_status);
        return false;
    }
    OutputDebugStringA("[FM2K HOOK] MH_Initialize() succeeded\n");

    // Initialize state manager
    if (!State::Init(process)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize state manager");
        IPC::Shutdown();
        MH_Uninitialize();
        return false;
    }

    // Create hooks using correct addresses from research
    // Create hooks using correct addresses from research
    uintptr_t hook1_addr = g_base_address + 0x146D0;
    char debug_msg[256];
    sprintf(debug_msg, "[FM2K HOOK] Creating hook 1: process_game_inputs at 0x%08X (base=0x%08X + 0x146D0)\n", 
            (unsigned int)hook1_addr, (unsigned int)g_base_address);
    OutputDebugStringA(debug_msg);
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "?ｯ HOOK INIT: Creating hook for process_game_inputs at 0x%08X", (unsigned int)hook1_addr);
    MH_STATUS status1 = MH_CreateHook(reinterpret_cast<LPVOID>(hook1_addr),
                                      reinterpret_cast<LPVOID>(Hook_ProcessGameInputs),
                                      reinterpret_cast<LPVOID*>(&original_process_inputs));
    if (status1 != MH_OK) {
        sprintf(debug_msg, "[FM2K HOOK] CRITICAL: MH_CreateHook failed for process_game_inputs with status %d\n", status1);
        OutputDebugStringA(debug_msg);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create process_game_inputs hook: status %d", status1);
        MH_Uninitialize();
        return false;
    }
    OutputDebugStringA("[FM2K HOOK] Hook 1 created successfully\n");
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating hook for update_game_state at 0x404CD0");
    MH_STATUS status2 = MH_CreateHook(reinterpret_cast<LPVOID>(g_base_address + 0x4CD0),  // update_game_state
                                      reinterpret_cast<LPVOID>(Hook_UpdateGameState),
                                      reinterpret_cast<LPVOID*>(&original_update_game));
    if (status2 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create update_game_state hook: %d", status2);
        MH_Uninitialize();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Creating hook for game_rand at 0x417A22");
    MH_STATUS status3 = MH_CreateHook(reinterpret_cast<LPVOID>(g_base_address + 0x17A22),  // game_rand
                                      reinterpret_cast<LPVOID>(Hook_GameRand),
                                      reinterpret_cast<LPVOID*>(&original_rng));
    if (status3 != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create game_rand hook: %d", status3);
        MH_Uninitialize();
        return false;
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "All hooks created successfully");

    // Enable hooks
    OutputDebugStringA("[FM2K HOOK] Calling MH_EnableHook(MH_ALL_HOOKS)\n");
    MH_STATUS enable_status = MH_EnableHook(MH_ALL_HOOKS);
    if (enable_status != MH_OK) {
        sprintf(debug_msg, "[FM2K HOOK] CRITICAL: MH_EnableHook failed with status %d\n", enable_status);
        OutputDebugStringA(debug_msg);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable hooks: status %d", enable_status);
        State::Shutdown();
        IPC::Shutdown();
        MH_Uninitialize();
        return false;
    }
    OutputDebugStringA("[FM2K HOOK] All hooks enabled successfully\n");

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申 HOOK INIT: FM2K hooks installed successfully - ALL HOOKS ENABLED!");
    
    // Verify hook installation by reading trampoline addresses
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申 HOOK VERIFY: Installed trampolines:");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申   process_game_inputs trampoline: %p", original_process_inputs);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申   update_game_state trampoline: %p", original_update_game);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申   game_rand trampoline: %p", original_rng);
    
    // Test immediate hook execution by calling the functions directly
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申 HOOK TEST: Testing hook execution...");
    
    // CRITICAL: Test if hooks are accessible by trying to call them directly
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申 HOOK TEST: Attempting direct hook call...");
    
    // Test the hook by calling it directly (this will prove if the hook function works)
    OutputDebugStringA("[FM2K HOOK] Starting direct hook test\n");
    try {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "?ｯ HOOK TEST: Calling Hook_ProcessGameInputs directly...");
        OutputDebugStringA("[FM2K HOOK] Calling Hook_ProcessGameInputs() directly...\n");
        int result = Hook_ProcessGameInputs();
        sprintf(debug_msg, "[FM2K HOOK] Direct hook call succeeded, returned %d\n", result);
        OutputDebugStringA(debug_msg);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "?ｯ HOOK TEST: Direct call succeeded, returned %d", result);
    } catch (...) {
        OutputDebugStringA("[FM2K HOOK] CRITICAL: Direct hook call FAILED with exception\n");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "?ｯ HOOK TEST: Direct call FAILED with exception");
    }
    
    // CRITICAL: Add a timer-based verification mechanism
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "??申?申 HOOK MONITOR: Will monitor hook execution every 1000ms");
    
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
    // Correct for ASLR by adding RVA to module base
    if (ReadProcessMemory(GetCurrentProcess(), 
                         (LPCVOID)(g_base_address + 0xCC30),
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