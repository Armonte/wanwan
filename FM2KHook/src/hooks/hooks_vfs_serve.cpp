// hooks_vfs_serve.cpp -- the VFS serve path: ReadFile / SetFilePointer /
// SetFilePointerEx / CloseHandle detours that hand buffered asset bytes to the
// game instead of hitting disk, plus their own MH_CreateHook installs.
//
// Split verbatim out of hooks_vfs.cpp, which had crept over the 1000-line
// limit. Pure move: the four detour bodies are unchanged. The registry they
// read (VFile / g_vfiles / g_vfile_mtx / VfsActive) moved to
// hooks_vfs_internal.h and gained external linkage; everything else about the
// VFS -- the FPK inflate/cache/prefetch machinery and the open-side hooks --
// stayed behind, because that state is private to it.
//
// Install follows the hooks.cpp cluster pattern: this TU queues its own hooks
// and InstallVfsHooks() calls InstallVfsServeHooks(), with MH_ApplyQueued
// making cluster ordering irrelevant.

#include "hooks.h"
#include "hooks_internal.h"
#include "hooks_vfs_internal.h"

#include <MinHook.h>
#include <SDL3/SDL_log.h>
#include <windows.h>

#include <cstring>   // std::memcpy in the serve fast-path
#include <mutex>


// ─── ReadFile / SetFilePointer / CloseHandle hooks ──────────────────────
// All four hot-path handle APIs get a fast lookup. Non-VFile handles fall
// through to the original via a single map.find() -- typically ~30 ns,
// invisible against the work the game is doing on the same call.

using ReadFile_t          = BOOL (WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using SetFilePointer_t    = DWORD(WINAPI*)(HANDLE, LONG, PLONG, DWORD);
using SetFilePointerEx_t  = BOOL (WINAPI*)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
using CloseHandle_t       = BOOL (WINAPI*)(HANDLE);

static ReadFile_t          original_ReadFile         = nullptr;
static SetFilePointer_t    original_SetFilePointer   = nullptr;
static SetFilePointerEx_t  original_SetFilePointerEx = nullptr;
static CloseHandle_t       original_CloseHandle      = nullptr;

static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n,
                                 LPDWORD got, LPOVERLAPPED ov) {
    if (VfsActive()) {
        std::lock_guard<std::mutex> lk(g_vfile_mtx);
        auto it = g_vfiles.find(h);
        if (it != g_vfiles.end()) {
            VFile& vf = *it->second;
            DWORD remaining = (vf.offset >= vf.buf->size())
                              ? 0u
                              : (DWORD)(vf.buf->size() - vf.offset);
            DWORD avail = (n < remaining) ? n : remaining;
            if (avail && buf) {
                std::memcpy(buf, vf.buf->data() + vf.offset, avail);
                vf.offset += avail;
            }
            if (got) *got = avail;
            return TRUE;
        }
    }
    return original_ReadFile(h, buf, n, got, ov);
}

static DWORD WINAPI Hook_SetFilePointer(HANDLE h, LONG dist, PLONG hi,
                                        DWORD method) {
    if (VfsActive()) {
        std::lock_guard<std::mutex> lk(g_vfile_mtx);
        auto it = g_vfiles.find(h);
        if (it != g_vfiles.end()) {
            VFile& vf = *it->second;
            int64_t dist64 = dist;
            if (hi) {
                dist64 = (int64_t)((uint64_t)(uint32_t)dist
                                  | ((uint64_t)(uint32_t)*hi << 32));
            }
            int64_t newpos;
            switch (method) {
                case FILE_BEGIN:   newpos = dist64; break;
                case FILE_CURRENT: newpos = (int64_t)vf.offset + dist64; break;
                case FILE_END:     newpos = (int64_t)vf.buf->size() + dist64; break;
                default:           return INVALID_SET_FILE_POINTER;
            }
            if (newpos < 0) newpos = 0;
            if ((uint64_t)newpos > vf.buf->size()) newpos = vf.buf->size();
            vf.offset = (size_t)newpos;
            if (hi) *hi = (LONG)((uint64_t)newpos >> 32);
            return (DWORD)((uint64_t)newpos & 0xFFFFFFFFu);
        }
    }
    return original_SetFilePointer(h, dist, hi, method);
}

static BOOL WINAPI Hook_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist,
                                         PLARGE_INTEGER newpos_out,
                                         DWORD method) {
    if (VfsActive()) {
        std::lock_guard<std::mutex> lk(g_vfile_mtx);
        auto it = g_vfiles.find(h);
        if (it != g_vfiles.end()) {
            VFile& vf = *it->second;
            int64_t newpos;
            switch (method) {
                case FILE_BEGIN:   newpos = dist.QuadPart; break;
                case FILE_CURRENT: newpos = (int64_t)vf.offset + dist.QuadPart; break;
                case FILE_END:     newpos = (int64_t)vf.buf->size() + dist.QuadPart; break;
                default:           return FALSE;
            }
            if (newpos < 0) newpos = 0;
            if ((uint64_t)newpos > vf.buf->size()) newpos = vf.buf->size();
            vf.offset = (size_t)newpos;
            if (newpos_out) newpos_out->QuadPart = newpos;
            return TRUE;
        }
    }
    return original_SetFilePointerEx(h, dist, newpos_out, method);
}

static BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    if (VfsActive()) {
        std::lock_guard<std::mutex> lk(g_vfile_mtx);
        g_vfiles.erase(h);  // no-op if not a VFile handle
    }
    return original_CloseHandle(h);
}



// Queue the four serve-path detours. Mirrors the original inline block in
// InstallVfsHooks byte-for-byte, including the tolerate-and-warn behavior:
// a missing export or a failed hook degrades to the game's normal disk path
// rather than failing hook init.
bool InstallVfsServeHooks(HMODULE kernel32) {
    if (!kernel32) return false;
        void* real_ReadFile         = (void*)GetProcAddress(kernel32, "ReadFile");
    void* real_SetFilePointer   = (void*)GetProcAddress(kernel32, "SetFilePointer");
    void* real_SetFilePointerEx = (void*)GetProcAddress(kernel32, "SetFilePointerEx");
    void* real_CloseHandle      = (void*)GetProcAddress(kernel32, "CloseHandle");

    if (real_ReadFile) {
        if (MH_CreateHook(real_ReadFile, (void*)Hook_ReadFile,
                          (void**)&original_ReadFile) != MH_OK ||
            MH_QueueEnableHook(real_ReadFile) != MH_OK) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: Failed to hook ReadFile");
        }
    }
    if (real_SetFilePointer) {
        if (MH_CreateHook(real_SetFilePointer, (void*)Hook_SetFilePointer,
                          (void**)&original_SetFilePointer) != MH_OK ||
            MH_QueueEnableHook(real_SetFilePointer) != MH_OK) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: Failed to hook SetFilePointer");
        }
    }
    if (real_SetFilePointerEx) {
        if (MH_CreateHook(real_SetFilePointerEx, (void*)Hook_SetFilePointerEx,
                          (void**)&original_SetFilePointerEx) != MH_OK ||
            MH_QueueEnableHook(real_SetFilePointerEx) != MH_OK) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: Failed to hook SetFilePointerEx");
        }
    }
    if (real_CloseHandle) {
        if (MH_CreateHook(real_CloseHandle, (void*)Hook_CloseHandle,
                          (void**)&original_CloseHandle) != MH_OK ||
            MH_QueueEnableHook(real_CloseHandle) != MH_OK) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Hooks: Failed to hook CloseHandle");
        }
    }
    return true;
}
