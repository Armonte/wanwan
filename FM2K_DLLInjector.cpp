#include "FM2K_DLLInjector.h"
#include <SDL3/SDL.h>

namespace FM2K {

bool DLLInjector::InjectAndInit(HANDLE process, const std::wstring& dll_path) {
    if (!process) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid process handle");
        return false;
    }

    // Get LoadLibraryW address
    LPVOID load_lib_addr = GetLoadLibraryAddr(process);
    if (!load_lib_addr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get LoadLibraryW address");
        return false;
    }

    // Create remote thread to load DLL
    HANDLE thread = CreateLoadLibraryThread(process, load_lib_addr, dll_path);
    if (!thread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create remote thread");
        return false;
    }

    // Wait for DLL to initialize
    if (!WaitForDLLInit(5000)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "DLL initialization timed out");
        CloseHandle(thread);
        return false;
    }

    CloseHandle(thread);
    return true;
}

bool DLLInjector::Uninject(HANDLE process, const std::wstring& dll_path) {
    // TODO: Implement DLL uninjection
    return true;
}

LPVOID DLLInjector::GetLoadLibraryAddr(HANDLE process) {
    return reinterpret_cast<LPVOID>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
}

HANDLE DLLInjector::CreateLoadLibraryThread(HANDLE process, LPVOID load_lib_addr,
                                          const std::wstring& dll_path) {
    // Allocate memory in target process for DLL path
    size_t path_size = (dll_path.length() + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, nullptr, path_size,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to allocate memory in target process");
        return nullptr;
    }

    // Write DLL path to target process
    if (!WriteProcessMemory(process, remote_path, dll_path.c_str(),
                          path_size, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to write DLL path to target process");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return nullptr;
    }

    // Create remote thread to load DLL
    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load_lib_addr),
        remote_path, 0, nullptr);

    if (!thread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to create remote thread");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return nullptr;
    }

    return thread;
}

bool DLLInjector::WaitForDLLInit(DWORD timeout_ms) {
    // TODO: Implement proper DLL initialization wait
    Sleep(timeout_ms);
    return true;
}

} // namespace FM2K 