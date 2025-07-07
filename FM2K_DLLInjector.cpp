#include "FM2K_DLLInjector.h"
#include "SDL3/SDL.h"
#include <TlHelp32.h>

namespace FM2K {

bool DLLInjector::InjectAndInit(HANDLE process, const std::wstring& dll_path) {
    // Get LoadLibraryW address in target process
    LPVOID loadlib_addr = GetLoadLibraryAddr(process);
    if (!loadlib_addr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get LoadLibraryW address");
        return false;
    }

    // Allocate space for DLL path in target process
    size_t path_bytes = (dll_path.length() + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, nullptr, path_bytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to allocate memory for DLL path");
        return false;
    }

    // Write DLL path to target process
    if (!WriteProcessMemory(process, remote_path, dll_path.c_str(), path_bytes, nullptr)) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write DLL path");
        return false;
    }

    // Create remote thread to load DLL
    HANDLE thread = CreateLoadLibraryThread(process, loadlib_addr, dll_path);
    if (!thread) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create remote thread");
        return false;
    }

    // Wait for thread completion
    WaitForSingleObject(thread, INFINITE);
    
    // Get thread exit code
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);

    if (!exit_code) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LoadLibrary failed in target process");
        return false;
    }

    // Wait for DLL to initialize via IPC
    return WaitForDLLInit();
}

bool DLLInjector::Uninject(HANDLE process, const std::wstring& dll_path) {
    // TODO: Use GetModuleHandle + FreeLibrary to properly unload
    return true;
}

LPVOID DLLInjector::GetLoadLibraryAddr(HANDLE process) {
    return GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
}

HANDLE DLLInjector::CreateLoadLibraryThread(HANDLE process, LPVOID loadlib_addr,
                                          const std::wstring& dll_path) {
    size_t path_bytes = (dll_path.length() + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, nullptr, path_bytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path || !WriteProcessMemory(process, remote_path, dll_path.c_str(),
                                          path_bytes, nullptr)) {
        return nullptr;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
                                     (LPTHREAD_START_ROUTINE)loadlib_addr,
                                     remote_path, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    }
    return thread;
}

bool DLLInjector::WaitForDLLInit(DWORD timeout_ms) {
    // TODO: Wait for IPC initialization event
    return true;
}
} 