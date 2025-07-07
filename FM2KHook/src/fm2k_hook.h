#pragma once

#include <windows.h>
#include <cstdint>

namespace FM2K {
namespace Hooks {

// Function pointer types for original functions
typedef void (__stdcall *ProcessGameInputsFn)();
typedef void (__stdcall *UpdateGameStateFn)();
typedef int (__stdcall *RNGFn)();

// Hook functions
void __stdcall Hook_ProcessGameInputs();
void __stdcall Hook_UpdateGameState();
int __stdcall Hook_RNG();

// Initialization/cleanup
bool Init(HANDLE process);
void Shutdown();

// Helper functions
uint32_t GetFrameNumber();
bool ShouldSaveState();
bool VisualStateChanged();

} // namespace Hooks
} // namespace FM2K 