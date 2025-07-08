#pragma once

#include <windows.h>
#include <string>

namespace FM2K {

class DLLInjector {
public:
    // Inject DLL into target process and initialize it
    static bool InjectAndInit(HANDLE process, const std::wstring& dll_path);

    // Uninject DLL from target process
    static bool Uninject(HANDLE process, const std::wstring& dll_path);

private:
    // Helper functions
    static LPVOID GetLoadLibraryAddr(HANDLE process);
    static HANDLE CreateLoadLibraryThread(HANDLE process, LPVOID load_lib_addr,
                                        const std::wstring& dll_path);
    static bool WaitForDLLInit(DWORD timeout_ms = 5000);
};

} // namespace FM2K 