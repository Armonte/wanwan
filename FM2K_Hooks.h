#pragma once

#include <windows.h>
#include "FM2K_Integration.h"  // brings in MinHook + address constants

namespace FM2K {
namespace Hooks {

// Install all MinHook hooks into the suspended FM2K process.
// proc must be a valid process handle with PROCESS_VM_READ/WRITE rights.
bool Init(HANDLE proc);

// Remove and disable all hooks, safe to call multiple times.
void Shutdown();

} // namespace Hooks
} // namespace FM2K 