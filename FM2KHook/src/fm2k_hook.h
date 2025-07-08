#pragma once

#include <windows.h>
#include <cstdint>

namespace FM2K {
namespace Hooks {

// Function pointer types for original functions
typedef int (__cdecl *ProcessGameInputsFn)();
typedef int (__cdecl *UpdateGameStateFn)();
typedef int (__cdecl *RNGFn)();

// Hook functions
int __cdecl Hook_ProcessGameInputs();
int __cdecl Hook_UpdateGameState();
int __cdecl Hook_GameRand();

// Initialization/cleanup
bool Init(HANDLE process);
void Shutdown();

// Helper functions
uint32_t GetFrameNumber();
bool ShouldSaveState();
bool VisualStateChanged();

} // namespace Hooks
} // namespace FM2K 