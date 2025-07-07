#pragma once

#include <windows.h>

#ifdef FM2KHOOK_EXPORTS
#define FM2KHOOK_API __declspec(dllexport)
#else
#define FM2KHOOK_API __declspec(dllimport)
#endif

// Error codes
enum FM2KHookResult {
    FM2KHOOK_OK = 0,
    FM2KHOOK_ERROR_MINHOOK_INIT = -1,
    FM2KHOOK_ERROR_CREATE_HOOK = -2,
    FM2KHOOK_ERROR_ENABLE_HOOK = -3,
    FM2KHOOK_ERROR_IPC_INIT = -4
};

#ifdef __cplusplus
extern "C" {
#endif

// Initialize hooks and IPC. Returns FM2KHookResult.
FM2KHOOK_API int FM2KHook_Init();

// Shutdown hooks and IPC. Safe to call multiple times.
FM2KHOOK_API void FM2KHook_Shutdown();

// Get last error message if any operation failed
FM2KHOOK_API const char* FM2KHook_GetLastError();

#ifdef __cplusplus
}
#endif 