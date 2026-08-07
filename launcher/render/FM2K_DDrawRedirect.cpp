// FM2K_DDrawRedirect -- see header for rationale.

#include "FM2K_DDrawRedirect.h"

#include <SDL3/SDL_log.h>
#include <winternl.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace FM2K::ddraw_redirect {

namespace {

// Process-wide redirect toggle. Default is ON now that the cnc-ddraw
// installer auto-runs at launcher boot -- every game launch pipes
// through the renamed dll unless the user explicitly opts out via the
// Renderer-tab "Disable" checkbox or the FM2K_TEST_IAT_REWRITE env var.
// Plain bool -- the UI thread flips it; Launch reads it on the same
// thread (launcher main runs both ImGui and the launch path), so no
// atomic needed.
bool g_force_redirect = true;

// Slot length we'll never exceed -- original `.rdata` string is `DDRAW.dll\0`
// (10 bytes). Patching past this would clobber whatever neighbour bytes
// the linker placed next, which on FM2K binaries is the next imported
// name (`KERNEL32.dll`, etc.). Keep the patch strictly local.
constexpr size_t kSlotLen = 10;

bool RPM(HANDLE p, uintptr_t addr, void* buf, size_t n) {
    SIZE_T r = 0;
    return ReadProcessMemory(p, reinterpret_cast<LPCVOID>(addr), buf, n, &r) &&
           r == n;
}

bool WPM(HANDLE p, uintptr_t addr, const void* buf, size_t n) {
    SIZE_T w = 0;
    return WriteProcessMemory(p, reinterpret_cast<LPVOID>(addr), buf, n, &w) &&
           w == n;
}

// Hex+ASCII dump for a small buffer. Used to log "what's actually in
// memory at the patch site." Returns a short string like
// "44 44 52 41 57 2e 64 6c 6c 00 |DDRAW.dll.|".
std::string HexDump(const uint8_t* p, size_t n) {
    std::string out;
    char tmp[8];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(tmp, sizeof(tmp), "%02x ", p[i]);
        out += tmp;
    }
    out += "|";
    for (size_t i = 0; i < n; ++i) {
        out += (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
    }
    out += "|";
    return out;
}

// File-static record of where we patched, so VerifyPatch() can re-read
// it after subsequent operations. RedirectImport sets this on success.
// Single-launch lifetime: gets overwritten by the next Launch call.
struct LastPatch {
    uintptr_t target_va = 0;
    char      expected[16] = {};   // NUL-terminated; must match the
                                   // 10-byte slot's leading bytes
};
LastPatch g_last_patch;

using NtQIP_t = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

// Walks the target's PEB to find the EXE image base. Querying the PEB
// rather than trusting OptionalHeader.ImageBase covers the (vanishingly
// unlikely for VC6-era FM2K binaries, but possible) case where the EXE
// has DYNAMIC_BASE set and the loader relocated it. Returns 0 on any
// failure; caller treats that as "give up, ResumeThread normally."
uintptr_t GetTargetImageBase(HANDLE process) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 0;
    auto NtQIP = reinterpret_cast<NtQIP_t>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!NtQIP) return 0;

    PROCESS_BASIC_INFORMATION pbi = {};
    ULONG ret = 0;
    if (NtQIP(process, /*ProcessBasicInformation*/ 0,
              &pbi, sizeof(pbi), &ret) != 0) {
        return 0;
    }
    if (!pbi.PebBaseAddress) return 0;

    // 32-bit PEB.ImageBaseAddress lives at offset 0x08. We're a 32-bit
    // launcher targeting a 32-bit child, so PEB is the 32-bit layout
    // and a 4-byte read gives the full pointer.
    DWORD base32 = 0;
    if (!RPM(process,
             reinterpret_cast<uintptr_t>(pbi.PebBaseAddress) + 0x08,
             &base32, sizeof(base32))) {
        return 0;
    }
    return static_cast<uintptr_t>(base32);
}

}  // namespace

bool RedirectImport(HANDLE process, const char* new_name) {
    if (!process || !new_name) return false;

    const size_t new_len = std::strlen(new_name);
    if (new_len + 1 > kSlotLen) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect: new name '%s' too long (need <= %zu chars incl. NUL)",
            new_name, kSlotLen - 1);
        return false;
    }

    const uintptr_t image_base = GetTargetImageBase(process);
    if (!image_base) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect: failed to query target PEB / image base");
        return false;
    }

    IMAGE_DOS_HEADER dos = {};
    if (!RPM(process, image_base, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect: bad DOS header at 0x%p", (void*)image_base);
        return false;
    }

    IMAGE_NT_HEADERS32 nt = {};
    if (!RPM(process, image_base + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect: bad NT headers at 0x%p",
            (void*)(image_base + dos.e_lfanew));
        return false;
    }

    const DWORD imp_rva  = nt.OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    const DWORD imp_size = nt.OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if (!imp_rva || imp_size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect: target has no import directory (?)");
        return false;
    }

    std::vector<uint8_t> imp_buf(imp_size);
    if (!RPM(process, image_base + imp_rva, imp_buf.data(), imp_size)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect: RPM import directory failed: %lu",
            GetLastError());
        return false;
    }

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(imp_buf.data());
    const size_t max_descs = imp_size / sizeof(IMAGE_IMPORT_DESCRIPTOR);

    for (size_t i = 0; i < max_descs && desc[i].Name; ++i) {
        // Slurp up to 16 bytes of the import name. Any FM2K-relevant DLL
        // name fits well under that; we only need enough to disambiguate
        // "ddraw.dll" from "kernel32.dll" / "user32.dll" / etc.
        char name_buf[16] = {};
        if (!RPM(process, image_base + desc[i].Name,
                 name_buf, sizeof(name_buf) - 1)) {
            continue;
        }
        if (_stricmp(name_buf, "ddraw.dll") != 0) continue;

        // Found it. Make the .rdata page writable, splat new_name + NUL
        // into the slot, NUL-pad the rest so any later substring scans
        // don't see "RAW.dll" leftovers, then restore protection.
        const uintptr_t target_va = image_base + desc[i].Name;

        // Log the page-level protection state and a pre-write byte dump
        // so we can tell apart "page wasn't writable" from "page was
        // writable, write succeeded, but bytes reverted" failure modes.
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQueryEx(process, (LPCVOID)target_va, &mbi, sizeof(mbi))) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "DDrawRedirect: target page state Base=0x%p Size=0x%zx "
                "AllocProtect=0x%lx CurProtect=0x%lx Type=0x%lx State=0x%lx",
                mbi.BaseAddress, (size_t)mbi.RegionSize,
                (unsigned long)mbi.AllocationProtect,
                (unsigned long)mbi.Protect,
                (unsigned long)mbi.Type,
                (unsigned long)mbi.State);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "DDrawRedirect: VirtualQueryEx failed: %lu", GetLastError());
        }

        uint8_t pre[kSlotLen] = {};
        if (RPM(process, target_va, pre, kSlotLen)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "DDrawRedirect: PRE-write bytes:  %s",
                HexDump(pre, kSlotLen).c_str());
        }

        DWORD old_protect = 0;
        BOOL vp_ok = VirtualProtectEx(process, reinterpret_cast<LPVOID>(target_va),
                                      kSlotLen, PAGE_READWRITE, &old_protect);
        if (!vp_ok) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "DDrawRedirect: VirtualProtectEx(RW) failed: %lu",
                GetLastError());
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect: VirtualProtectEx OK old_protect=0x%lx",
            (unsigned long)old_protect);

        char patched[kSlotLen] = {};  // zero-initialized -> trailing NULs
        std::memcpy(patched, new_name, new_len);

        bool ok = WPM(process, target_va, patched, kSlotLen);
        if (!ok) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "DDrawRedirect: WPM at 0x%p failed: %lu",
                (void*)target_va, GetLastError());
        } else {
            // Force the page to commit the change to the working set
            // before we restore protection, so any AV/AMSI hook running
            // synchronously on the WPM has a chance to fire (and we'd
            // see the diff in the post-write read).
            FlushInstructionCache(process,
                                  reinterpret_cast<LPCVOID>(target_va),
                                  kSlotLen);
        }

        // Read back BEFORE restoring protection -- protection state
        // shouldn't matter for ReadProcessMemory, but doing it here
        // also tells us "we're reading from the same RW window we
        // just wrote to."
        uint8_t post[kSlotLen] = {};
        bool post_ok = RPM(process, target_va, post, kSlotLen);

        DWORD junk = 0;
        VirtualProtectEx(process, reinterpret_cast<LPVOID>(target_va),
                         kSlotLen, old_protect, &junk);

        if (post_ok) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "DDrawRedirect: POST-write bytes: %s",
                HexDump(post, kSlotLen).c_str());
            const bool match = std::memcmp(post, patched, kSlotLen) == 0;
            if (!match) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "DDrawRedirect: BYTES DID NOT MATCH expected after WPM "
                    "(write reported success but page didn't change). "
                    "AV / runtime hook is likely reverting .rdata writes.");
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "DDrawRedirect: post-write bytes match expected");
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "DDrawRedirect: RPM after write failed: %lu",
                GetLastError());
        }

        if (!ok) return false;

        // Stash for VerifyPatch.
        g_last_patch.target_va = target_va;
        std::memcpy(g_last_patch.expected, patched,
                    std::min<size_t>(kSlotLen, sizeof(g_last_patch.expected) - 1));
        g_last_patch.expected[sizeof(g_last_patch.expected) - 1] = '\0';

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect: patched DDRAW.dll -> %s at 0x%p (image base 0x%p)",
            new_name, (void*)target_va, (void*)image_base);
        return true;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "DDrawRedirect: no DDRAW.dll import descriptor found in target");
    return false;
}

std::string VerifyPatch(HANDLE process) {
    if (!process || !g_last_patch.target_va) return {};
    uint8_t buf[kSlotLen] = {};
    if (!RPM(process, g_last_patch.target_va, buf, kSlotLen)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect::VerifyPatch: RPM at 0x%p failed: %lu",
            (void*)g_last_patch.target_va, GetLastError());
        return {};
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "DDrawRedirect::VerifyPatch: bytes at 0x%p: %s",
        (void*)g_last_patch.target_va, HexDump(buf, kSlotLen).c_str());
    return std::string(reinterpret_cast<const char*>(buf),
                       strnlen(reinterpret_cast<const char*>(buf), kSlotLen));
}

void SetForceRedirect(bool enabled) {
    g_force_redirect = enabled;
}

bool GetForceRedirect() {
    return g_force_redirect;
}

bool ShouldRedirect() {
    // Hard override for A/B: FM2K_NO_DDRAW_REDIRECT=1 forces the game to load
    // its OWN ddraw.dll (whatever it ships) instead of our injected cnc-ddraw.
    // Used to isolate whether our cnc-ddraw setup is the render cost on games
    // that bundle their own (e.g. Robot Heroes).
    if (const char* off = std::getenv("FM2K_NO_DDRAW_REDIRECT"); off && off[0] == '1')
        return false;
    if (g_force_redirect) return true;
    const char* env = std::getenv("FM2K_TEST_IAT_REWRITE");
    return env && env[0] == '1';
}

std::wstring ResolveCncDdrawDir() {
    // Env var override -- wide, since paths might be JP. Use the W variant
    // so we don't lose chars through the ANSI codepage.
    wchar_t override_buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"FM2K_DDRAW_DIR", override_buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring s(override_buf, n);
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
        return s;
    }

    // Default: <launcher_exe_dir>\cnc-ddraw
    wchar_t exe_buf[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe_buf, MAX_PATH)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "DDrawRedirect: GetModuleFileNameW failed: %lu", GetLastError());
        return {};
    }
    std::filesystem::path p(exe_buf);
    return (p.parent_path() / L"cnc-ddraw").wstring();
}

}  // namespace FM2K::ddraw_redirect
