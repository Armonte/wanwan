#pragma once

#include <windows.h>
#include <cstdint>

namespace FM2K {
namespace DirectHooks {

// Hook function types (matching your working patterns)
typedef int (__cdecl *ProcessGameInputsFn)();
typedef int (__cdecl *UpdateGameStateFn)();
typedef int (__cdecl *RNGFn)();

// Hook installation
bool InstallHooks(HANDLE process);
void UninstallHooks();

// Hook functions
int __cdecl Hook_ProcessGameInputs();
int __cdecl Hook_UpdateGameState();
int __cdecl Hook_GameRand();

// Utility functions
bool IsHookSystemActive();
uint32_t GetFrameNumber();

} // namespace DirectHooks
} // namespace FM2K