// spec_css_park.cpp -- SPECTATOR CHARACTER-SELECT PARK (Wave 2).
//
// THE BUG (Lane B, laneB_css_window.md sections 8.3-8.4, measured live):
// during a BETWEEN-MATCH character-select window the spectator's object pool
// holds the fighters as type 4 (script VMs) while the host's holds them as
// type 1 (marked-for-reap). Type 4 dispatches to character_state_machine
// (g_object_function_table[4] @ 0x41ED58+16 -> 0x411BF0), so on the spectator
// the fighters EXECUTE their battle-entry script under gravity while the CSS is
// still on screen: slot 0, type 4, script 24, vel_y accumulating by a constant
// 17685/frame, descending 535 -> 921 px over the last ~29 frames of the window.
// The host's own resolved object is frozen at y=535 the whole time. Four such
// falls in one vanpri run, zero in the next -- per-window probabilistic.
// Every pre-existing gate is battle-only, so all of that ran green.
//
// THE FIX: on the SPECTATOR PLANE ONLY, park type-4 objects for the duration of
// that window by calling Fm2k_NeutralizeMatchEndScriptObjects() (round_events.
// cpp), which sets script_init_state (+0x152) = 2 so character_state_machine
// returns at its entry guard (0x411C3F). That primitive is monotone,
// idempotent, touches ONLY type-4 slots (so it can never demote the type-10
// driver that performs the whole transition) and keeps sprite fields intact.
// It is the same call the match-end seam campaign shipped through gates G2/G3.
//
// WHY PER-FRAME AND NOT TWO ONE-SHOTS. Lane B's recommendation was to call it
// at the two window edges. That is not sufficient and the same report says why:
// the object count JUMPS (34 -> 41) after the pin engages, i.e. the objects
// that fall are created part-way through the window. An edge-only park runs
// before they exist. So the edges ARM a window and the park is re-applied every
// frame while the window is open; a freshly created object executes for at most
// the tick it was born on. The call is idempotent by construction, so a
// re-application on an already-parked pool is a pure identity write.
//
// ================= WHY THE PARK CANNOT SURVIVE INTO BATTLE =================
// This is the failure mode a review must attack: a fighter that enters battle
// at script_init_state == 2 is frozen for the round. It cannot happen, for
// three independent reasons -- one engine-structural, one by-construction in
// this file, one plane-specific -- plus an always-on tripwire that measures it.
//
// (1) ENGINE: every battle object is FRESHLY CREATED with +0x152 zeroed.
//     Verified in the IDB (WonderfulWorld_ver_0946, 2026-08-15):
//       - game_state_manager (the type-10 CSS driver, 0x406FC0) STATE 4 calls
//         ResetObjectsAndCalculateSpeed @ 0x406450 (call at 0x406FF3), which
//         walks all 1024 slots and writes type = 1 to EVERY slot with type > 1
//         except itself; then it self-demotes (`mov [eax], ebp`, ebp = 1, at
//         0x407003) and spawns the battle-init driver. So every parked type-4
//         object is marked type 1 at that edge.
//       - type 1 dispatches to Obj_ReapMarkedSlot (g_object_function_table[1]
//         @ 0x41ED58+4 -> 0x4069A0), whose entire body is `*g_object_data_ptr
//         = 0` -- the slot's type field goes to 0, i.e. the slot is FREE, on the
//         very next update_game_state pass (0x404CD0).
//       - create_game_object (0x406570) only ever hands out a slot whose type
//         field is 0 (`while (*v5)` first-free scan) and its FIRST action on
//         that slot is `memset(slot, 0, 0x17C)` (0x4065E3, and 0x406599 on the
//         pool-full path). 0x152 < 0x17C, so script_init_state is ZEROED
//         (= CSM_STATE_INIT) at creation, unconditionally, for every object.
//     Therefore no battle object can inherit a character-select-window park:
//     the objects this file parks are reaped before any battle object exists,
//     and the allocator zeroes the field anyway.
//
// (2) BY CONSTRUCTION HERE: the park is gated on a live type-10 object.
//     The type-10 object IS game_state_manager, and it self-demotes to type 1
//     at 0x407003 in the same instruction stream that runs the demote above --
//     strictly BEFORE the battle-init driver it spawns creates anything. So
//     "a type-10 object exists in the pool" is an engine-derived predicate that
//     goes false at the exact edge past which battle objects start appearing,
//     and it is false for the whole of battle. Lane B's type census corroborates
//     it from the other side: during the ramp the spectator pool is "all type 4
//     + 1 type 10" while the host's is "all type 1 + 1 type 14" (type 14 =
//     vs_round_function, the battle driver -- the host has already crossed the
//     edge, the spectator has not). game_mode == 2000 is required as well; the
//     two together are redundant on purpose.
//
// (3) SPECTATOR PLANE: Phase 4c's pool resync re-applies the HOST's
//     authoritative battle-entry pool bytes at every MATCH_START
//     (spec_pool_sync.cpp -> SaveState_LoadFromBytes over the whole pool
//     region, +0x152 included). Even if (1) and (2) both failed, the bytes this
//     file wrote would be overwritten by the host's before the battle runs.
//     Listed third because it is kill-switchable (FM2K_SPEC_POOL_SYNC) and the
//     other two are not.
//
// (4) MEASURED: SpecCssPark_OnBattleFrameZero() is an ALWAYS-ON tripwire that
//     counts type-4 slots sitting at script_init_state == 2 at the spectator's
//     first battle frame and logs loudly if any are found. One line per battle
//     entry, only when non-zero. If the three arguments above are ever wrong,
//     this says so in the log before the battle terms do.
// ===========================================================================
//
// SCOPE. Spectator plane only, by construction: the two Engage call sites are
// inside SpectatorNode's live-playback branches (spec_playback.cpp) and the
// Tick/tripwire sites are inside the spectator trampoline. Nothing in a player
// or offline-replay process can arm the window, and Tick() is a single
// predicate load when disarmed. The player-plane match-end seam park
// (round_events.cpp sim-902) is untouched and unrelated.
//
// COST. One pool scan for the type-10 predicate plus one for the park, per
// character-select frame of an armed window only: ~2048 dword loads over a
// 391KB region, no allocation, no formatting, no file IO (hook logging is
// SYNCHRONOUS -- quill is hardcoded off in the hook). Two log lines per window.
//
// KILL SWITCH: FM2K_SPEC_CSS_PARK=0 disables (default ON), for the causality
// control run and for the field if this ever needs to come out without a build.

#if !defined(ENGINE_FM95)

#include "spec_css_park.h"

#include <SDL3/SDL_log.h>
#include <cstdint>
#include <cstddef>
#include <cstdlib>

#include "../core/globals.h"   // FM2K::ADDR_GAME_MODE
#include "../hooks/hooks.h"    // Fm2k_{Count,Neutralize}...ScriptObjects

namespace {

// Object pool literals, same as round_events.cpp / seam_trace.cpp. Repeated
// rather than shared because each of those TUs deliberately owns its own copy
// (they are WonderfulWorld absolute addresses, FM2K-only, and the file is
// compiled out entirely under ENGINE_FM95).
constexpr uintptr_t ADDR_OBJECT_POOL = 0x004701E0;
constexpr size_t    OBJ_STRIDE       = 382;
constexpr size_t    OBJ_COUNT        = 1024;
constexpr ptrdiff_t OFF_OBJ_TYPE     = 0x00;
constexpr ptrdiff_t OFF_OBJ_INIT_ST  = 0x152;
constexpr int       OBJ_TYPE_SCRIPT_VM  = 4;    // character_state_machine
constexpr int       OBJ_TYPE_CSS_DRIVER = 10;   // game_state_manager @ 0x406FC0
constexpr int       CSM_STATE_DONE      = 2;

bool        s_armed        = false;
const char* s_why          = "";
uint32_t    s_frames       = 0;   // in-window frames the park actually ran on
uint32_t    s_park_events  = 0;   // frames where it changed at least one slot
uint32_t    s_park_slots   = 0;   // total slots changed across the window
uint32_t    s_window_ord   = 0;

// The type-10 character-select driver. Its presence is the engine-derived
// "the CSS transition has not handed off yet" predicate -- see (2) above.
bool CssDriverAlive() {
    const uint8_t* pool = (const uint8_t*)ADDR_OBJECT_POOL;
    for (size_t i = 0; i < OBJ_COUNT; ++i) {
        const uint8_t* obj = pool + i * OBJ_STRIDE;
        if (*(const int*)(obj + OFF_OBJ_TYPE) == OBJ_TYPE_CSS_DRIVER) return true;
    }
    return false;
}

void CloseWindow(const char* reason) {
    if (!s_armed) return;
    s_armed = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[CSSPARK] win%u close (%s): frames=%u park_frames=%u slots=%u",
        s_window_ord, reason, s_frames, s_park_events, s_park_slots);
}

}  // namespace

namespace specnode {

bool SpecCssPark_Enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("FM2K_SPEC_CSS_PARK");
        // DEFAULT ON. Only an explicit "0" turns it off.
        cached = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
        if (cached == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[CSSPARK] DISABLED by FM2K_SPEC_CSS_PARK=0 -- spectator CSS "
                "windows will run their fighters' entry scripts (falling "
                "objects); diagnostic control only");
        }
    }
    return cached == 1;
}

void SpecCssPark_Engage(const char* why) {
    if (!SpecCssPark_Enabled()) return;
    if (s_armed) return;   // idempotent: both edges of one window may fire
    s_armed       = true;
    s_why         = why ? why : "?";
    s_frames      = 0;
    s_park_events = 0;
    s_park_slots  = 0;
    ++s_window_ord;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[CSSPARK] win%u engage (%s) mode=%u",
        s_window_ord, s_why, *(const uint32_t*)FM2K::ADDR_GAME_MODE);
}

void SpecCssPark_Tick() {
    if (!s_armed) return;
    const uint32_t mode = *(const uint32_t*)FM2K::ADDR_GAME_MODE;
    if (mode >= 3000u) {
        // Battle reached without the tripwire seeing it first (catch-up pop
        // ordering); close here so a window can never span a match.
        CloseWindow("battle entered");
        return;
    }
    if (mode != 2000u) return;          // results screen / title: not our window
    if (!CssDriverAlive()) return;      // handed off to battle init: never park
    const int parkable = Fm2k_CountParkableScriptObjects();
    if (parkable > 0) {
        Fm2k_NeutralizeMatchEndScriptObjects();
        ++s_park_events;
        s_park_slots += (uint32_t)parkable;
    }
    ++s_frames;
}

void SpecCssPark_OnBattleFrameZero() {
    // ALWAYS ON, regardless of the kill switch: this is the measurement that
    // says whether a park (ours or anyone's) leaked into a battle.
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
            "(script_init_state=2) -- a character-select park reached battle; "
            "those fighters cannot execute their scripts. Last window: win%u "
            "frames=%u park_frames=%u slots=%u",
            parked4, total4, s_window_ord, s_frames, s_park_events);
    }
    CloseWindow("battle frame 0");
}

void SpecCssPark_Reset() {
    CloseWindow("reset");
}

}  // namespace specnode

#else  // ENGINE_FM95 -- FM2K object-pool literals do not apply

#include "spec_css_park.h"

namespace specnode {
bool SpecCssPark_Enabled()            { return false; }
void SpecCssPark_Engage(const char*)  {}
void SpecCssPark_Tick()               {}
void SpecCssPark_OnBattleFrameZero()  {}
void SpecCssPark_Reset()              {}
}  // namespace specnode

#endif
