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

// -----------------------------------------------------------------------------
// Hook stubs
// -----------------------------------------------------------------------------
static void __stdcall Hook_ProcessGameInputs()
{
    // TODO: Call NetworkSession::Instance()->ProcessEvents() once integration exists.
    // For now, just log first few hits for sanity.
    static int hit_count = 0;
    if (hit_count < 10) {
        SDL_Log("[Hook] process_game_inputs called (%d)", hit_count);
        ++hit_count;
    }

    // Call the original function to preserve behavior
    if (original_process_inputs)
        original_process_inputs();
}

static void __stdcall Hook_UpdateGameState()
{
    // Placeholder?pure pass-through currently
    if (original_update_game)
        original_update_game();
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
    // Disable and remove hooks
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    // Shutdown state manager
    State::Shutdown();

    // Shutdown IPC
    IPC::Shutdown();

    g_proc = nullptr;
    SDL_Log("FM2K hooks removed");
}

} // namespace Hooks
} // namespace FM2K 