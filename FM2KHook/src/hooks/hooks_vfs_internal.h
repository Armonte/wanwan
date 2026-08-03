// hooks_vfs_internal.h -- the VFile registry shared between hooks_vfs.cpp
// (Win32 open interception + the FPK archive layer) and hooks_vfs_serve.cpp
// (the ReadFile / SetFilePointer / CloseHandle serve path).
//
// Split out because hooks_vfs.cpp had crept over the 1000-line limit. Only
// the registry and the active-gate cross the boundary; the FPK cache and
// prefetch machinery stay private to hooks_vfs.cpp.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// Buffered asset content served in place of real disk reads. `buf` is shared
// so the path-keyed .fpk cache and a live handle reference ONE allocation --
// a cache hit costs a pointer copy, not a ~100MB memcpy, which matters in the
// game's 32-bit address space.
struct VFile {
    std::shared_ptr<std::vector<uint8_t>> buf;
    size_t offset = 0;
};

// Map of OS handle -> buffered content. Real Windows handles are handed back
// to the game (no synthetic-handle plumbing) so any other API it calls on the
// handle still works. Guarded by g_vfile_mtx.
extern std::mutex                                         g_vfile_mtx;
extern std::unordered_map<HANDLE, std::unique_ptr<VFile>> g_vfiles;

// True when either gate (FM2K_FAST_PLAYER_LOAD / FM2K_FPK_VFS) is on. The
// serve hooks fast-path out on this before touching the mutex.
bool VfsActive();

// Per-cluster install, mirroring the hooks.cpp split pattern: each cluster
// does its own MH_CreateHook + MH_QueueEnableHook and InstallVfsHooks calls
// it, so MH_ApplyQueued makes ordering irrelevant.
bool InstallVfsServeHooks(HMODULE kernel32);
