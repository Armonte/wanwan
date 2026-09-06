#include "alt_colors.h"

#include <MinHook.h>
#include <SDL3/SDL.h>
#include <cstdint>

#include "../core/globals.h"
#include "alt_colors_math.h"

namespace {

// void __cdecl AssignPlayerColor(int slot, int input)
//   arg1 = char-slot index, arg2 = engine input word. Confirmed by
//   disassembly: `mov eax,[esp+8]` reads the input BEFORE the three pushes,
//   and `mov edi,[esp+0x10]` reads the slot AFTER them.
using AssignPlayerColorFn = void(__cdecl*)(int, int);
AssignPlayerColorFn original_AssignPlayerColor = nullptr;

using fm2k::altcolor::kColours;
using fm2k::altcolor::kFn1;
using fm2k::altcolor::kFn2;

inline uint32_t* SlotColour(int slot) {
    return (uint32_t*)(FM2K::ADDR_CHARSLOT0_COLOR_PICK +
                       FM2K::CHARSLOT_STRIDE * (size_t)slot);
}

// Which character a slot picked, or -1 when we cannot say. 1v1 VS pins
// slot[0]=P1 and slot[1]=P2; team modes spread 0..3 / 4..7 across slots the
// 1v1 character grid does not describe, so we answer -1 and the rule falls
// back to the engine's stricter behaviour.
// Which character each slot is on, remembered as the slot confirms.
//
// Measured at confirm time on 2026-09-05: the team history arrays
// (0x47006C / 0x47007C) and the lock counts (0x4700EC / 0x4700F0) are all
// still ZERO, so they cannot answer this yet -- but the 1v1 character grid
// is live and correct, and slots confirm one at a time. So read the grid for
// the slot being confirmed and keep it. That works for 1v1, team modes and
// any extended team count, without needing the slot-to-player layout for
// anything except which half of the grid to read.
int g_slot_char[kColours] = {-1, -1, -1, -1, -1, -1, -1, -1};

inline bool SlotBelongsToP1(int slot) {
    // 1v1 pins slot0=P1, slot1=P2. Team modes use 0..3 for P1 and 4..7 for
    // P2 (globals.h). g_game_mode_flag: 1 = VS, 2 = team.
    const uint32_t mode_flag = *(const uint32_t*)FM2K::ADDR_GAME_MODE_FLAG;
    return (mode_flag == 2u) ? (slot < 4) : (slot == 0);
}

inline void RememberSlotChar(int slot) {
    if (slot < 0 || slot >= kColours) return;
    g_slot_char[slot] = (int)*(const uint32_t*)(
        SlotBelongsToP1(slot) ? FM2K::ADDR_P1_SELECTED_CHAR
                              : FM2K::ADDR_P2_SELECTED_CHAR);
}

inline int SlotChar(int slot) {
    return (slot >= 0 && slot < kColours) ? g_slot_char[slot] : -1;
}

void AssignWithCollisionRule(int slot, int desired) {
    RememberSlotChar(slot);
    int colours[kColours], chars[kColours];
    for (int i = 0; i < kColours; ++i) {
        colours[i] = (int)*SlotColour(i);
        chars[i]   = SlotChar(i);
    }
    const int got = fm2k::altcolor::Resolve(slot, desired, colours, chars,
                                            kColours);
    if (got < 0) return;                       // no free colour -- leave it
    if (*SlotColour(slot) == (uint32_t)got) return;
    // Log only on a CHANGE: this can be called on several frames while a
    // confirm is held, and hook logging is synchronous.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[ALTCOLOR] slot=%d char=%d asked=%d got=%d%s",
        slot, chars[slot], desired, got,
        (got != desired) ? " (bumped -- same char had it)" : "");
    *SlotColour(slot) = (uint32_t)got;
}

void __cdecl Hook_AssignPlayerColor(int slot, int input) {
    if (slot < 0 || slot >= kColours || !original_AssignPlayerColor) {
        if (original_AssignPlayerColor) original_AssignPlayerColor(slot, input);
        return;
    }
    // We take over the whole assignment rather than post-correcting the
    // engine's: it is a short, fully understood function (ladder, collision
    // scan, one store, nothing else), and letting it write first then
    // overwriting would make two different colours briefly visible to
    // anything reading the slot in between.
    AssignWithCollisionRule(slot,
        fm2k::altcolor::LadderColour((uint16_t)input));
}

}  // namespace

namespace AltColors {

bool Install() {
    if constexpr (FM2K::kIsFM95 || FM2K::ADDR_CHARSLOT0_COLOR_PICK == 0) {
        return true;  // FM95 has its own palette select -- not wired here.
    } else {
        void* target = (void*)0x406F20;
        if (MH_CreateHook(target, (void*)&Hook_AssignPlayerColor,
                          (void**)&original_AssignPlayerColor) != MH_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "AltColors: MH_CreateHook(AssignPlayerColor @ 0x406F20) failed");
            return false;
        }
        if (MH_EnableHook(target) != MH_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "AltColors: MH_EnableHook failed");
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "AltColors: FN1/FN2 now select palettes 6 and 7");
        return true;
    }
}

void Uninstall() {
    if constexpr (!FM2K::kIsFM95 && FM2K::ADDR_CHARSLOT0_COLOR_PICK != 0) {
        MH_DisableHook((void*)0x406F20);
        MH_RemoveHook((void*)0x406F20);
        original_AssignPlayerColor = nullptr;
    }
}

}  // namespace AltColors
