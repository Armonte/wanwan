#include "FM2K_Integration.h"
#include "SDL3/SDL.h"
#include <windows.h>

// All definitions inside FM2K namespace declared in the header
namespace FM2K {

bool ReadMemoryRaw(HANDLE proc, uintptr_t remote_addr, void* out, size_t bytes)
{
    if (!proc || !out || bytes == 0) return false;
    SIZE_T read = 0;
    if (!::ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(remote_addr), out, bytes, &read) || read != bytes) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ReadProcessMemory failed (addr=0x%p, bytes=%zu, err=%lu)",
                     reinterpret_cast<void*>(remote_addr), bytes, ::GetLastError());
        return false;
    }
    return true;
}

bool WriteMemoryRaw(HANDLE proc, uintptr_t remote_addr, const void* in, size_t bytes)
{
    if (!proc || !in || bytes == 0) return false;
    SIZE_T written = 0;
    if (!::WriteProcessMemory(proc, reinterpret_cast<LPVOID>(remote_addr), in, bytes, &written) || written != bytes) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "WriteProcessMemory failed (addr=0x%p, bytes=%zu, err=%lu)",
                     reinterpret_cast<void*>(remote_addr), bytes, ::GetLastError());
        return false;
    }
    return true;
}

bool BulkCopyOut(HANDLE proc, void* local_dst, uintptr_t remote_src, size_t bytes)
{
    return ReadMemoryRaw(proc, remote_src, local_dst, bytes);
}

bool BulkCopyIn(HANDLE proc, uintptr_t remote_dst, const void* local_src, size_t bytes)
{
    return WriteMemoryRaw(proc, remote_dst, local_src, bytes);
}

} // namespace FM2K 