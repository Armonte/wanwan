#include "FM2K_Hooks.h"
#include "FM2KHook/src/ipc.h"
#include "FM2KHook/src/state_manager.h"
#include <SDL3/SDL.h>
#include "MinHook.h"

namespace FM2K {
namespace Hooks {

// -----------------------------------------------------------------------------
// Forward declarations of original functions (trampolines)
// -----------------------------------------------------------------------------
using VoidFn = void (__stdcall*)();
using RandFn = int  (__stdcall*)();

static VoidFn original_process_inputs  = nullptr;
static VoidFn original_update_game     = nullptr;
static RandFn  original_rand_func      = nullptr;

// We may need the process handle later (e.g., to read RNG state).
static HANDLE g_proc = nullptr;

// Frame counter implementation - reads directly from game memory
uint32_t GetFrameNumber() {
    uint32_t frame_number = 0;
    SIZE_T bytes_read = 0;
    if (ReadProcessMemory(g_proc, (LPCVOID)State::Memory::FRAME_NUMBER_ADDR, &frame_number, sizeof(frame_number), &bytes_read) && bytes_read == sizeof(frame_number)) {
        return frame_number;
    }
    // Return 0 or log an error if read fails
    return 0;
}

// State save condition (from implementation guide)
bool ShouldSaveState() {
    // For now, save state every frame for testing
    // Later, optimize based on:
    // - Input changes
    // - Critical game state changes
    // - Network prediction window
    return true;
}

// Visual state change detection (from implementation guide)
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

// -----------------------------------------------------------------------------
// Hook stubs
// -----------------------------------------------------------------------------
static void __stdcall Hook_ProcessGameInputs()
{
    // Capture inputs BEFORE original function runs, as it clears them
    uint32_t p1_input_raw = 0;
    uint32_t p2_input_raw = 0;
    SIZE_T bytes_read = 0;

    ReadProcessMemory(g_proc, (LPCVOID)State::Memory::P1_INPUT_ADDR, &p1_input_raw, sizeof(p1_input_raw), &bytes_read);
    ReadProcessMemory(g_proc, (LPCVOID)State::Memory::P2_INPUT_ADDR, &p2_input_raw, sizeof(p2_input_raw), &bytes_read);

    // Call the original function to preserve behavior and advance frame
    if (original_process_inputs) {
        original_process_inputs();
    }

    // Now, get the frame number *after* it has been incremented
    uint32_t current_frame = GetFrameNumber();

    // Send INPUT_CAPTURED event
    FM2K::IPC::Event input_event;
    input_event.type = FM2K::IPC::EventType::INPUT_CAPTURED;
    input_event.frame_number = current_frame;
    input_event.timestamp_ms = SDL_GetTicks();
    input_event.data.input.p1_input = static_cast<uint16_t>(p1_input_raw);
    input_event.data.input.p2_input = static_cast<uint16_t>(p2_input_raw);
    input_event.data.input.frame_number = current_frame;
    if (!FM2K::IPC::PostEvent(input_event)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to post input captured event for frame %u", current_frame);
    }

    // Check if we should save state before processing
    if (ShouldSaveState()) {
        // TODO: Implement state saving when state manager is ready
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
            "State save needed at frame %u", current_frame);
    }

    // Send IPC event that frame has advanced
    FM2K::IPC::Event event;
    event.type = FM2K::IPC::EventType::FRAME_ADVANCED;
    event.frame_number = current_frame;
    event.timestamp_ms = SDL_GetTicks();
    
    if (!FM2K::IPC::PostEvent(event)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to post frame advanced event for frame %u", current_frame);
    }
}

static void __stdcall Hook_UpdateGameState()
{
    // Call original function first to update game state
    if (original_update_game) {
        original_update_game();
    }

    // Check if we should save state after game state update
    uint32_t current_frame = GetFrameNumber();
    if (current_frame > 0 && ShouldSaveState()) {
        // Calculate real state checksum using state manager
        uint32_t checksum = State::CalculateStateChecksum();
        
        // Send IPC event that state should be saved
        FM2K::IPC::Event event;
        event.type = FM2K::IPC::EventType::STATE_SAVED;
        event.frame_number = current_frame;
        event.timestamp_ms = SDL_GetTicks();
        event.data.state.checksum = checksum;
        event.data.state.frame_number = current_frame; // Redundant but matches structure
        
        if (!FM2K::IPC::PostEvent(event)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                "Failed to post state save event for frame %u", current_frame);
        }
    }

    // Check for visual state changes
    if (VisualStateChanged()) {
        FM2K::IPC::Event event;
        event.type = FM2K::IPC::EventType::VISUAL_STATE_CHANGED;
        event.frame_number = current_frame;
        event.timestamp_ms = SDL_GetTicks();
        
        if (!FM2K::IPC::PostEvent(event)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
                "Failed to post visual state change event for frame %u", current_frame);
        }
    }
}

static int __stdcall Hook_GameRand()
{
    int result = 0;
    if (original_rand_func)
        result = original_rand_func();

    return result; // could log or checksum later
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
bool Init(HANDLE process)
{
    if (!process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle");
        return false;
    }

    g_proc = process;

    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize MinHook");
        return false;
    }

    // Initialize state manager first
    if (!State::Init(process)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize state manager");
        return false;
    }

    if (!IPC::Init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize IPC");
        State::Shutdown();
        return false;
    }

    // Install hooks
    MH_STATUS status;

    // Process inputs hook (primary rollback entry)
    status = MH_CreateHook(
        reinterpret_cast<LPVOID>(0x4146D0),
        reinterpret_cast<LPVOID>(&Hook_ProcessGameInputs),
        reinterpret_cast<LPVOID*>(&original_process_inputs)
    );
    if (status != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create process inputs hook");
        return false;
    }

    // Update game state hook
    status = MH_CreateHook(
        reinterpret_cast<LPVOID>(0x404CD0),
        reinterpret_cast<LPVOID>(&Hook_UpdateGameState),
        reinterpret_cast<LPVOID*>(&original_update_game)
    );
    if (status != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create update game hook");
        return false;
    }

    // RNG function hook
    status = MH_CreateHook(
        reinterpret_cast<LPVOID>(0x417A22),
        reinterpret_cast<LPVOID>(&Hook_GameRand),
        reinterpret_cast<LPVOID*>(&original_rand_func)
    );
    if (status != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create RNG hook");
        return false;
    }

    // Enable all hooks
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to enable hooks");
        return false;
    }

    SDL_Log("FM2K hooks installed successfully");
    return true;
}

void Shutdown()
{
    // Disable all hooks
    MH_DisableHook(MH_ALL_HOOKS);
    
    // Uninitialize MinHook
    MH_Uninitialize();

    // Shutdown subsystems
    IPC::Shutdown();
    State::Shutdown();

    g_proc = nullptr;
    SDL_Log("FM2K hooks removed");
}

} // namespace Hooks
} // namespace FM2K 