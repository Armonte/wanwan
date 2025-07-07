#include "FM2K_Hooks.h"
#include "SDL3/SDL.h"
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
bool Init(HANDLE proc)
{
    g_proc = proc;

    if (!g_proc) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hooks::Init - invalid process handle");
        return false;
    }

    // Ensure MinHook initialized (idempotent)
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "MinHook initialization failed");
        return false;
    }

    auto create = [&](LPVOID target, LPVOID detour, LPVOID* orig) -> bool {
        return MH_CreateHook(target, detour, orig) == MH_OK;
    };
    auto enable = [&](LPVOID target) -> bool {
        return MH_EnableHook(target) == MH_OK;
    };

    if (!create((LPVOID)FM2K::FRAME_HOOK_ADDR,  &Hook_ProcessGameInputs, (LPVOID*)&original_process_inputs) ||
        !create((LPVOID)FM2K::UPDATE_GAME_STATE_ADDR, &Hook_UpdateGameState, (LPVOID*)&original_update_game) ||
        !create((LPVOID)FM2K::RANDOM_SEED_ADDR /* RNG fn addr */, &Hook_GameRand, (LPVOID*)&original_rand_func)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hook creation failed");
        return false;
    }

    if (!enable((LPVOID)FM2K::FRAME_HOOK_ADDR) ||
        !enable((LPVOID)FM2K::UPDATE_GAME_STATE_ADDR) ||
        !enable((LPVOID)FM2K::RANDOM_SEED_ADDR)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hook enable failed");
        return false;
    }

    SDL_Log("FM2K Hooks installed");
    return true;
}

void Shutdown()
{
    auto disableRemove = [](LPVOID target) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    };

    disableRemove((LPVOID)FM2K::FRAME_HOOK_ADDR);
    disableRemove((LPVOID)FM2K::UPDATE_GAME_STATE_ADDR);
    disableRemove((LPVOID)FM2K::RANDOM_SEED_ADDR);

    original_process_inputs = nullptr;
    original_update_game    = nullptr;
    original_rand_func      = nullptr;

    SDL_Log("FM2K Hooks removed");
}

} // namespace Hooks
} // namespace FM2K 