#include "FM2K_DLLInjector.h"
#include <SDL3/SDL.h>

namespace FM2K {

bool DLLInjector::InjectAndInit(HANDLE process, const std::wstring& dll_path) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Starting DLL injection process...");
    
    if (!process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle for injection");
        return false;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Getting LoadLibraryW address...");
    // Get LoadLibraryW address
    LPVOID load_lib_addr = GetLoadLibraryAddr(process);
    if (!load_lib_addr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get LoadLibraryW address");
        return false;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "LoadLibraryW found at %p", load_lib_addr);

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Creating remote thread to load DLL: %ls", dll_path.c_str());
    // Create remote thread to load DLL
    HANDLE thread = CreateLoadLibraryThread(process, load_lib_addr, dll_path);
    if (!thread) {
        DWORD error = GetLastError();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to create remote thread for DLL injection. Error: %lu", error);
        return false;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Remote thread created successfully");

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Waiting for DLL initialization...");
    // Wait for DLL to initialize
    if (!WaitForDLLInit(5000)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DLL initialization timed out after 5 seconds");
        CloseHandle(thread);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DLL injected and initialized successfully");
    CloseHandle(thread);
    return true;
}

bool DLLInjector::Uninject(HANDLE process, const std::wstring& dll_path) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Uninjecting DLL: %ls", dll_path.c_str());
    // TODO: Implement DLL uninjection
    return true;
}

LPVOID DLLInjector::GetLoadLibraryAddr(HANDLE process) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to get kernel32.dll handle. Error: %lu", GetLastError());
        return nullptr;
    }
    
    LPVOID addr = reinterpret_cast<LPVOID>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (!addr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to get LoadLibraryW address. Error: %lu", GetLastError());
    }
    return addr;
}

HANDLE DLLInjector::CreateLoadLibraryThread(HANDLE process, LPVOID load_lib_addr, const std::wstring& dll_path) {
    // Allocate memory in target process for DLL path
    SIZE_T path_size = (dll_path.length() + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, nullptr, path_size, 
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to allocate memory in target process. Error: %lu", GetLastError());
        return nullptr;
    }

    // Write DLL path to target process memory
    if (!WriteProcessMemory(process, remote_path, dll_path.c_str(), path_size, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to write DLL path to target process. Error: %lu", GetLastError());
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return nullptr;
    }

    // Create remote thread to call LoadLibraryW
    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load_lib_addr),
        remote_path, 0, nullptr);
    
    if (!thread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to create remote thread. Error: %lu", GetLastError());
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return nullptr;
    }

    return thread;
}

bool DLLInjector::WaitForDLLInit(DWORD timeout_ms) {
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
        "Waiting up to %lu ms for DLL initialization...", timeout_ms);
    
    // Create/open a named event that the DLL will signal when initialized
    HANDLE init_event = CreateEventW(nullptr, TRUE, FALSE, L"FM2KHook_Initialized");
    if (!init_event) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "Failed to create initialization event. Error: %lu", GetLastError());
        return false;
    }

    // Wait for the DLL to signal initialization
    DWORD wait_result = WaitForSingleObject(init_event, timeout_ms);
    CloseHandle(init_event);

    if (wait_result == WAIT_OBJECT_0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "DLL initialization completed successfully");
        return true;
    } else if (wait_result == WAIT_TIMEOUT) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, 
            "DLL initialization timed out after %lu ms", timeout_ms);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Wait for DLL initialization failed. Error: %lu", GetLastError());
    }
    return false;
}

} // namespace FM2K 