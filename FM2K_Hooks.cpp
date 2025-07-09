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
    // Since we're running inside the game process, read memory directly
    uint32_t* frame_ptr = reinterpret_cast<uint32_t*>(State::Memory::FRAME_NUMBER_ADDR);
    uint32_t frame_value = *frame_ptr;
    
    // Debug logging every 100 reads to avoid spam
    static int debug_counter = 0;
    if (++debug_counter % 100 == 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
            "GetFrameNumber: Reading from 0x%08X, value=%u (debug #%d)", 
            State::Memory::FRAME_NUMBER_ADDR, frame_value, debug_counter);
    }
    
    return frame_value;
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
    
    // Since we're running inside the game process, read memory directly
    uint32_t* effect_ptr = reinterpret_cast<uint32_t*>(State::Memory::EFFECT_ACTIVE_FLAGS);
    uint32_t current_effect_flags = *effect_ptr;
    
    if (current_effect_flags != last_effect_flags) {
        last_effect_flags = current_effect_flags;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Hook stubs
// -----------------------------------------------------------------------------
static void __stdcall Hook_ProcessGameInputs()
{
    // Debug: Log that hook is being called
    static int hook_call_count = 0;
    hook_call_count++;
    
    // Capture inputs BEFORE original function runs, as it clears them
    // Since we're running inside the game process, read memory directly
    uint32_t* p1_input_ptr = reinterpret_cast<uint32_t*>(State::Memory::P1_INPUT_ADDR);
    uint32_t* p2_input_ptr = reinterpret_cast<uint32_t*>(State::Memory::P2_INPUT_ADDR);
    uint32_t p1_input_raw = *p1_input_ptr;
    uint32_t p2_input_raw = *p2_input_ptr;
    
    // Debug: Log frame value before calling original function
    uint32_t frame_before = GetFrameNumber();
    if (hook_call_count <= 10 || hook_call_count % 50 == 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
            "Hook_ProcessGameInputs #%d: Frame before original=%u, P1=0x%04x, P2=0x%04x", 
            hook_call_count, frame_before, p1_input_raw, p2_input_raw);
    }

    // Call the original function to preserve behavior and advance frame
    if (original_process_inputs) {
        original_process_inputs();
    }

    // Now, get the frame number *after* it has been incremented
    uint32_t current_frame = GetFrameNumber();
    
    // Debug: Log frame value after calling original function
    if (hook_call_count <= 10 || hook_call_count % 50 == 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
            "Hook_ProcessGameInputs #%d: Frame after original=%u (changed from %u)", 
            hook_call_count, current_frame, frame_before);
    }

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