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
//
// ======================= NARROWED 2026-08-16 (review B1e) ==================
// The first shipped shape of this fix called Fm2k_NeutralizeMatchEndScriptObjects()
// -- a BLANKET park of every type-4 slot -- on every frame of the whole armed
// window. Review finding B1e flagged that as a visibility risk on a screen the
// spectator is looking at, and the owner asked the same question directly
// ("it seems like your current solution is just ignoring character select?").
// It was then MEASURED with the new [CSS-ANIM] instrument (css_window.cpp),
// which reports per type-4 object whether its script program counter (+0x2C)
// advances. wanwan, one between-match window, same run:
//     HOST plane          296 objects, 296 ADVANCED, 0 frozen
//     SPECTATOR park OFF  296 objects, 296 ADVANCED, 0 frozen  (matches host)
//     SPECTATOR park ON   125 objects,   0 ADVANCED, 125 frozen
// and whole object CLASSES were missing from the parked plane entirely (a
// parked VM never spawns its animation children). So the blanket park froze
// EVERYTHING the host animates: the character portraits (create_game_object(4,
// 80, ...) per docs/analysis/css_state_machine.md), the confirm sprite
// (CreateProjectileObject(..., 101, ...)), and the 36-object character-select
// UI panel that lives for the whole window. Confirmed on vanguard-princess as
// well (183-207 objects per window, all advancing with the park off).
//
// The park is therefore narrowed on TWO axes, both engine-derived:
//
//  (A) TEMPORAL -- only after both players have confirmed. g_round_timer_counter
//      @ 0x424F00 is 0 for the entire selection phase and starts incrementing
//      1/frame once both action states latch, triggering battle above 100
//      (docs/analysis/css_state_machine.md; corroborated in every [HOST-CSS]
//      line: timer=0 all through selection, 23/74 in the one sample that lands
//      inside the ramp, back to 0 at the next window). The falling object lives
//      ENTIRELY inside that ~101-frame ramp (laneB_css_window.md 8.4.1; the
//      harness measured the fall over 28-93 of 1054 window frames, always at
//      the end). So ~90% of the window -- all cursor movement, all portrait
//      idle animation, the whole selection -- is now untouched.
//
//  (B) IDENTITY -- only objects that can actually run a FIGHTER script.
//      entity_kind (+0x15A) 0/1/5 select a character slot; 2/3/4 select the
//      static g_kgt_file_buffer / g_story_char_config / g_char_physics_table
//      (round_events.cpp's field map, and the same predicate the [ENDSEAM-OOB]
//      hazard probe uses). The falling object is kind=1 with a bound player
//      slot (laneB 8.3: slot 0, type 4, owner 79/80, script 22/24, pslot 0/1,
//      kind 1); the character-select UI is kind 2/3. So the UI and the confirm
//      animation keep running even inside the ramp.
//
// Net effect on the hazard: unchanged. The object that falls is a kind-1
// character object created after the confirm, i.e. inside the ramp and inside
// the identity set, so it is still parked within one tick of its birth.
//
// LEVER: FM2K_SPEC_CSS_PARK_WIDE=1 restores the old blanket/whole-window shape
// (default OFF). Diagnostic only -- it is the A/B control for the narrowing,
// and it re-freezes the character-select screen.

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
constexpr ptrdiff_t OFF_OBJ_PLAYER   = 0x156;   // player_slot_id
constexpr ptrdiff_t OFF_OBJ_KIND     = 0x15A;   // entity_kind
constexpr int       OBJ_TYPE_SCRIPT_VM  = 4;    // character_state_machine
constexpr int       OBJ_TYPE_CSS_DRIVER = 10;   // game_state_manager @ 0x406FC0
constexpr int       CSM_STATE_INIT      = 0;
constexpr int       CSM_STATE_RUNNING   = 1;
constexpr int       CSM_STATE_DONE      = 2;
constexpr size_t    NUM_CHAR_SLOTS      = 8;

// g_round_timer_counter. 0 for the whole character-select SELECTION phase;
// increments 1/frame once both players' action states latch; battle triggers
// above 100. See the NARROWED block above and docs/analysis/css_state_machine.md.
constexpr uintptr_t ADDR_ROUND_TIMER_COUNTER = 0x00424F00;

bool        s_armed        = false;
const char* s_why          = "";
uint32_t    s_frames       = 0;   // in-window frames the park actually ran on
uint32_t    s_ramp_frames  = 0;   // of those, frames inside the post-confirm ramp
uint32_t    s_park_events  = 0;   // frames where it changed at least one slot
uint32_t    s_park_slots   = 0;   // total slots changed across the window
uint32_t    s_window_ord   = 0;
int         s_wide         = -1;  // FM2K_SPEC_CSS_PARK_WIDE, -1 = env not read

// The pre-narrowing shape, kept reachable as the A/B control for the narrowing.
bool ParkWide() {
    if (s_wide < 0) {
        const char* v = std::getenv("FM2K_SPEC_CSS_PARK_WIDE");
        s_wide = (v && v[0] && v[0] != '0') ? 1 : 0;
        if (s_wide == 1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[CSSPARK] WIDE mode (FM2K_SPEC_CSS_PARK_WIDE=1) -- parking EVERY "
                "type-4 object for the WHOLE character-select window; this is the "
                "pre-2026-08-16 shape and it freezes the select screen the "
                "spectator is watching. Diagnostic control only");
        }
    }
    return s_wide == 1;
}

// The narrowed hazard set: type-4 script VMs whose entity_kind binds a CHARACTER
// SLOT (0/1/5), with a valid player slot. These are the only objects that can
// run a fighter script -- kinds 2/3/4 index the static tables and are the
// character-select UI. Returns how many slots it CHANGED (one walk, unlike the
// wide path's count-then-apply pair, because nothing here needs the dry run).
int ParkHazardScriptObjects() {
    uint8_t* pool = (uint8_t*)ADDR_OBJECT_POOL;
    int n = 0;
    for (size_t i = 0; i < OBJ_COUNT; ++i) {
        uint8_t* obj = pool + i * OBJ_STRIDE;
        if (*(const int*)(obj + OFF_OBJ_TYPE) != OBJ_TYPE_SCRIPT_VM) continue;
        const int kind = *(const int*)(obj + OFF_OBJ_KIND);
        if (kind != 0 && kind != 1 && kind != 5) continue;   // static-table kinds
        const int pslot = *(const int*)(obj + OFF_OBJ_PLAYER);
        if (pslot < 0 || (size_t)pslot >= NUM_CHAR_SLOTS) continue;
        int* init_state = (int*)(obj + OFF_OBJ_INIT_ST);
        if (*init_state != CSM_STATE_INIT && *init_state != CSM_STATE_RUNNING) continue;
        ++n;
        // Monotone + idempotent, exactly as Fm2k_NeutralizeMatchEndScriptObjects:
        // only 0/1 can reach the opcode dispatch, anything else already returns
        // at the 0x411C3F entry guard.
        *init_state = CSM_STATE_DONE;
    }
    return n;
}

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
        "[CSSPARK] win%u close (%s): frames=%u ramp_frames=%u park_frames=%u "
        "slots=%u wide=%d",
        s_window_ord, reason, s_frames, s_ramp_frames, s_park_events,
        s_park_slots, s_wide == 1 ? 1 : 0);
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
    s_ramp_frames = 0;
    s_park_events = 0;
    s_park_slots  = 0;
    ++s_window_ord;
    (void)ParkWide();   // read + announce the lever once, at the first window
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[CSSPARK] win%u engage (%s) mode=%u wide=%d",
        s_window_ord, s_why, *(const uint32_t*)FM2K::ADDR_GAME_MODE,
        s_wide == 1 ? 1 : 0);
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
    ++s_frames;

    if (ParkWide()) {
        // Pre-2026-08-16 shape: every type-4 object, every in-window frame.
        const int parkable = Fm2k_CountParkableScriptObjects();
        if (parkable > 0) {
            Fm2k_NeutralizeMatchEndScriptObjects();
            ++s_park_events;
            s_park_slots += (uint32_t)parkable;
        }
        return;
    }

    // (A) TEMPORAL narrowing: nothing is parked until BOTH players have
    // confirmed. The counter is 0 for the whole selection phase, so the
    // character-select screen animates on the spectator exactly as it does on
    // the host right up to the confirm.
    if (*(const uint32_t*)ADDR_ROUND_TIMER_COUNTER == 0u) return;
    ++s_ramp_frames;

    // (B) IDENTITY narrowing: only the character-bound script VMs. The
    // character-select UI (entity_kind 2/3/4) keeps running through the ramp.
    const int parked = ParkHazardScriptObjects();
    if (parked > 0) {
        ++s_park_events;
        s_park_slots += (uint32_t)parked;
    }
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
