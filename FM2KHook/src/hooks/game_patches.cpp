// Game patches shared by hook install paths. NOTE: the boot/multi-instance
// patch set (BypassMultiInstanceCheck, ApplyBootToCharacterSelectPatches,
// ApplyCharacterSelectModePatches, ...) lives as statics in core/dllmain.cpp
// -- the dead duplicates that used to sit here were removed 2026-07-11 so
// there's exactly one copy to edit.
#include "game_patches.h"
#include "globals.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL_log.h>

// Defuse the case-200 t4-walk false positive in vs_round_function.
//
// vs_round_function's case 200 (active battle) walks the object pool for
// active type-4 fighter objects and transitions to state 300 (round end)
// if it finds <2. Vanilla main_game_loop runs `process_game_inputs +
// update_game` N times per outer iteration (frame-skip multi-tick), so
// fighters' KGT scripts complete their tick cycle and any transient
// alive_flag set/clear resolves before the round controller's case-200
// walk runs. Our trampoline does ONE update per outer iter, so case 200
// walks AFTER one fighter tick that may have left alive_flag transiently
// non-zero -- case 200 sees t4 < 2 and false-fires the round-end transition.
//
// Confirmed via [CASE200-TRIP] diagnostic on StudioS Fighters / Strip
// Fighter Zero: pre_t4 oscillates 0/1/2 around update_game; post_t4
// always returns to 2. WW chars don't trigger this; StudioS chars do.
//
// asm at 0x408EC2-0x408ED8:
//   cmp esi, 2
//   jge short loc_408F18         ; 7D 51 -- skip transition if t4 >= 2
//   mov edi, 12Ch
//   mov [ecx+156h], ebx           ; v2[342] = 0
//   mov [ecx+152h], edi           ; v2[338] = 300  ← false round-end
//   jmp short loc_408F1D
//
// Patch: change `jge short` (0x7D) to `jmp short` (0xEB) at 0x408EC5,
// so the transition is always skipped. Round-end still triggers via the
// legitimate paths (round_end_flag, score countdown crossing 0). The
// t4 walk becomes effectively a no-op.
//
// Safety: the t4 path is a redundant safety check -- round_end_flag and
// the score timer are the engine's primary round-end triggers. NOPing
// the t4 transition only removes a false positive; it does not bypass
// any legitimate round-end path. WW behavior unchanged (its t4 walk
// never fires the false positive in the first place).
void PatchVsRoundCase200T4FalsePositive() {
    // Diagnostic: FM2K_SKIP_T4_PATCH=1 leaves vs_round_function's t4 walk
    // untouched. Used to bisect "is the t4 patch causing the StudioS
    // visual glitch?" If glitch persists with patch off → t4 isn't it.
    // If glitch goes away → patch is corrupting something we don't
    // understand yet.
    const char* env_skip = std::getenv("FM2K_SKIP_T4_PATCH");
    if (env_skip && std::strcmp(env_skip, "1") == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: FM2K_SKIP_T4_PATCH=1 -- leaving vs_round_function "
            "case-200 t4 walk untouched. StudioS will likely bail to "
            "CSS shortly after entering battle.");
        return;
    }

    constexpr uintptr_t JGE_ADDR = 0x408EC5;
    uint8_t* jge_addr = (uint8_t*)JGE_ADDR;

    // Verify the EXACT instruction context. WW asm at 0x408EC2..0x408EC6:
    //   83 FE 02         cmp esi, 2      ; -3..-1
    //   7D 51            jge short +0x51 ;  0..+1  ← patch target
    //   BF 2C 01 00 00   mov edi, 12Ch   ; +2..+6
    // If the bytes at -3..+6 don't match this pattern, we're patching the
    // wrong function -- different FM2K builds (StudioS Fighters, Strip
    // Fighter Zero, etc.) may have shifted code addresses even with the
    // same engine. Hitting a random 0x7D byte elsewhere corrupts unrelated
    // code and would explain odd visual / script glitches in StudioS.
    static const uint8_t EXPECTED[] = {
        0x83, 0xFE, 0x02,                 // -3..-1: cmp esi, 2
        0x7D, 0x51,                       //  0..+1: jge short loc_408F18
        0xBF, 0x2C, 0x01, 0x00, 0x00      // +2..+6: mov edi, 12Ch
    };
    const uint8_t* sig_base = jge_addr - 3;
    char hex[3 * sizeof(EXPECTED) + 1];
    for (size_t i = 0; i < sizeof(EXPECTED); ++i) {
        std::snprintf(hex + i * 3, 4, "%02X ", sig_base[i]);
    }

    bool sig_ok = true;
    for (size_t i = 0; i < sizeof(EXPECTED); ++i) {
        if (sig_base[i] != EXPECTED[i]) { sig_ok = false; break; }
    }

    if (!sig_ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: vs_round_function t4 patch -- SIGNATURE MISMATCH at "
            "0x%08X. Bytes at -3..+6: [%s]. Expected: [83 FE 02 7D 51 BF "
            "2C 01 00 00]. This binary is not the WW build the patch was "
            "validated against -- leaving t4 walk untouched to avoid "
            "corrupting random code.",
            (unsigned)JGE_ADDR, hex);
        return;
    }

    DWORD old_protect;
    if (VirtualProtect(jge_addr, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *jge_addr = 0xEB;     // jge short -> jmp short
        VirtualProtect(jge_addr, 1, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: vs_round_function case-200 t4 walk neutered "
            "(jge -> jmp at 0x%08X, sig OK: [%s])",
            (unsigned)JGE_ADDR, hex);
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: Failed to patch case-200 t4 walk");
    }
}

void NeuterFullscreenTogglesForCncDdraw() {
    // Single-byte flips -- same shape as BypassMultiInstanceCheck. Each
    // turns a `jnz short` (0x75) into `jmp short` (0xEB) so the WndProc's
    // "if VK matches, toggle global + InitializeDirectDraw" body is
    // unconditionally skipped. Displacement byte after the opcode stays
    // the same -- both forms are 2 bytes total.
    //
    // FM2K (WonderfulWorld_ver_0946):
    //   0x4060f3  F4         in main_window_proc @ 0x405f50
    //   0x406288  Alt+Enter  same WndProc, WM_SYSKEYDOWN case
    //   Toggle target: g_graphics_mode @ 0x424704
    //
    // FM95 (CPW.exe) has no F4 toggle (binding absent) but still has the
    // Alt+Enter path:
    //   0x40bbf1  Alt+Enter  in main_window_proc @ 0x40b930
    //   Toggle target: g_ddraw_fullscreen_mode @ 0x42557c
    struct Patch { uintptr_t addr; uint8_t expect_first; const char* label; };
    Patch patches_fm2k[] = {
        { 0x4060f3, 0x75, "FM2K F4 (WM_KEYDOWN)"      },
        { 0x406288, 0x75, "FM2K Alt+Enter (WM_SYSKEYDOWN)" },
    };
    Patch patches_fm95[] = {
        { 0x40bbf1, 0x75, "FM95 Alt+Enter (WM_SYSKEYDOWN)" },
    };
    Patch* patches      = nullptr;
    size_t patch_count  = 0;
    if constexpr (FM2K::kIsFM2K) {
        patches = patches_fm2k;
        patch_count = sizeof(patches_fm2k) / sizeof(patches_fm2k[0]);
    } else if constexpr (FM2K::kIsFM95) {
        patches = patches_fm95;
        patch_count = sizeof(patches_fm95) / sizeof(patches_fm95[0]);
    }

    for (size_t i = 0; i < patch_count; ++i) {
        const auto& p = patches[i];
        auto* byte = reinterpret_cast<uint8_t*>(p.addr);
        if (IsBadReadPtr(byte, 1)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PATCH: %s site at 0x%08X unreadable -- wrong build?",
                p.label, (unsigned)p.addr);
            continue;
        }
        if (*byte != p.expect_first) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PATCH: %s site at 0x%08X has 0x%02X (expected 0x%02X) -- "
                "binary doesn't match the build the patch was validated "
                "against; skipping",
                p.label, (unsigned)p.addr, *byte, p.expect_first);
            continue;
        }
        DWORD old_protect = 0;
        if (!VirtualProtect(byte, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "PATCH: VirtualProtect for %s failed (err=%lu)",
                p.label, GetLastError());
            continue;
        }
        *byte = 0xEB;  // jnz -> jmp
        VirtualProtect(byte, 1, old_protect, &old_protect);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PATCH: neutered %s at 0x%08X (jnz -> jmp)",
            p.label, (unsigned)p.addr);
    }
}
