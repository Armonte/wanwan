// [EXIT] trace -- see exit_trace.h.
#include "exit_trace.h"
#include "../core/globals.h"
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include "MinHook.h"

namespace {

using ExitProcessFn = void(WINAPI*)(UINT);
ExitProcessFn p_ExitProcess = nullptr;
DWORD g_main_tid = 0;

// "module+0xOFFSET" for a code address, or the raw address if no module owns
// it (JIT / shellcode / freed module).
void Describe(void* addr, char* out, size_t cap) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, sizeof(path));
        const char* base = path;
        for (const char* p = path; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        std::snprintf(out, cap, "%s+0x%X", base,
                      (unsigned)((uintptr_t)addr - (uintptr_t)mod));
    } else {
        std::snprintf(out, cap, "0x%08X", (unsigned)(uintptr_t)addr);
    }
}

void WINAPI Hook_ExitProcess(UINT code) {
    void* frames[12] = {};
    // Skip this detour's own frame; frame 0 is then the caller of ExitProcess.
    const USHORT n = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
    char stack[12 * 48] = {};
    size_t off = 0;
    for (USHORT i = 0; i < n && off < sizeof(stack) - 48; ++i) {
        char one[48];
        Describe(frames[i], one, sizeof(one));
        off += std::snprintf(stack + off, sizeof(stack) - off, "%s%s",
                             i ? " <- " : "", one);
    }
    const DWORD tid = GetCurrentThreadId();
    const uint32_t mode = *(volatile uint32_t*)FM2K::ADDR_GAME_MODE;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "[EXIT] ExitProcess(%u) on %s thread %lu, game_mode=%u, caller: %s",
        code, (g_main_tid && tid == g_main_tid) ? "MAIN" : "other", (unsigned long)tid,
        (unsigned)mode, n ? stack : "(no stack)");
    p_ExitProcess(code);
}

}  // namespace

void ExitTrace_SetMainThread(unsigned long tid) { g_main_tid = tid; }

void ExitTrace_Install() {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void* target = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "ExitProcess"))
                       : nullptr;
    if (!target) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[EXIT] kernel32!ExitProcess not found -- exit trace off");
        return;
    }
    void* tramp = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook_ExitProcess), &tramp) != MH_OK ||
        MH_QueueEnableHook(target) != MH_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[EXIT] MH hook on ExitProcess failed -- exit trace off");
        return;
    }
    p_ExitProcess = reinterpret_cast<ExitProcessFn>(tramp);
}
