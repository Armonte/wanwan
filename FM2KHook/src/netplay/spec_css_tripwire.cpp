// spec_css_tripwire.cpp -- battle-frame-0 parked-VM detector. Contract and the
// history of what it replaced are in spec_css_tripwire.h.

#if !defined(ENGINE_FM95)

#include "spec_css_tripwire.h"

#include <SDL3/SDL_log.h>
#include <cstdint>
#include <cstddef>

namespace {

// Object pool literals, same as round_events.cpp / css_autoconfirm.cpp. Repeated
// rather than shared because each of those TUs deliberately owns its own copy
// (they are WonderfulWorld absolute addresses, FM2K-only, and this file is
// compiled out entirely under ENGINE_FM95).
constexpr uintptr_t ADDR_OBJECT_POOL = 0x004701E0;
constexpr size_t    OBJ_STRIDE       = 382;
constexpr size_t    OBJ_COUNT        = 1024;
constexpr ptrdiff_t OFF_OBJ_TYPE     = 0x00;
constexpr ptrdiff_t OFF_OBJ_INIT_ST  = 0x152;
constexpr int       OBJ_TYPE_SCRIPT_VM = 4;
constexpr int       CSM_STATE_DONE     = 2;

}  // namespace

namespace specnode {

void SpecCssTripwire_OnBattleFrameZero() {
    const uint8_t* pool = (const uint8_t*)ADDR_OBJECT_POOL;
    int total4 = 0, parked4 = 0;
    for (size_t i = 0; i < OBJ_COUNT; ++i) {
        const uint8_t* obj = pool + i * OBJ_STRIDE;
        if (*(const int*)(obj + OFF_OBJ_TYPE) != OBJ_TYPE_SCRIPT_VM) continue;
        ++total4;
        if (*(const int*)(obj + OFF_OBJ_INIT_ST) == CSM_STATE_DONE) ++parked4;
    }
    if (parked4 > 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[CSSPARK-TRIP] battle frame 0 with %d of %d type-4 objects PARKED "
            "(script_init_state=2) -- some mechanism left script VMs parked "
            "into a battle; those fighters cannot execute their scripts. The "
            "character-select park was deleted 2026-08-17, so this is NOT it: "
            "look at the match-end seam park (round_events.cpp sim-902) and at "
            "any newly added +0x152 writer",
            parked4, total4);
    }
}

}  // namespace specnode

#else  // ENGINE_FM95 -- FM2K object-pool literals do not apply

#include "spec_css_tripwire.h"

namespace specnode {
void SpecCssTripwire_OnBattleFrameZero() {}
}  // namespace specnode

#endif
