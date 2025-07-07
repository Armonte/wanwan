#pragma once

#include <windows.h>
#include <string>

namespace FM2K {

class DLLInjector {
public:
    // Inject DLL into target process and wait for initialization
    // Returns true if injection and DLL init succeeded
    static bool InjectAndInit(HANDLE process, const std::wstring& dll_path);

    // Free the DLL from target process
    // Returns true if uninjection succeeded
    static bool Uninject(HANDLE process, const std::wstring& dll_path);

private:
    // Get address of LoadLibraryW in target process
    static LPVOID GetLoadLibraryAddr(HANDLE process);
    
    // Create remote thread to load DLL
    static HANDLE CreateLoadLibraryThread(HANDLE process, LPVOID loadlib_addr, const std::wstring& dll_path);
    
    // Wait for DLL to initialize via IPC
    static bool WaitForDLLInit(DWORD timeout_ms = 5000);
}; 