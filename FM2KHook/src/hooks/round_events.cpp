// C3.5 -- vs_round_function detour for ROUND_START / ROUND_END emit.
//
// FM2K-only. The function lives at 0x004086A0 in WonderfulWorld_ver_0946 and
// is the central round state machine for vs/story/team modes. Each call
// reads/writes a substate field at obj+0x152 (= 338) on the current
// round-state object (g_object_data_ptr → slot). We snapshot the substate
// pre-call and post-call, detect the two relevant edges, and emit
// SessionEvent ops via the host SpectatorNode_Append* helpers.
//
// IDA hand-off: docs/c3.5_round_events_ida_handoff.md.

#if !defined(ENGINE_FM95)

#include "round_events.h"

#include <SDL3/SDL_log.h>
#include <cstdint>
#include <windows.h>

#include "MinHook.h"

#include "../core/globals.h"           // g_is_rolling_back, g_player_index
#include "../netplay/netplay.h"        // Netplay_MatchSettingsDigest / HostConfigRxCount
#include "../netplay/spectator_node.h" // SpectatorNode_AppendRound{Start,End}
#include "per_game_patches.h"          // PerGamePatches_OnBattleInitComplete
#include "seam_free_probe.h"           // [ENDSEAM-FREE] window latch (telemetry)

// SOCD mode accessor (defined in hooks_input.cpp). Declared here rather than
// pulled from a header for the same reason netplay.cpp declares it locally --
// there is no public hooks header for it yet.
extern "C" int Hook_GetSOCDModePublic();

// vs_round_function dispatcher (FM2K)
constexpr uintptr_t ADDR_VS_ROUND_FUNCTION = 0x004086A0;

// g_object_data_ptr -- pointer to current round-state slot
constexpr uintptr_t ADDR_G_OBJECT_DATA_PTR = 0x004CFA00;
constexpr ptrdiff_t OFF_ROUND_SUBSTATE     = 0x152;  // 338

// Substate values for edge detection.
//
// ROUND_START fires at 112 → 200 (the FIGHT_LATCH → ACTIVE edge).
// Why not the earlier 100→110 (BATTLE_INIT → ANNOUNCE_WAIT)? Case 100's
// body resets every char-slot's hp_max to placeholder=1 (per IDA decompile
// `*(slot_hp_ptr + 0Ch) = 1`); the real per-character hp_max only gets
// re-populated by the fighter type-4 object's init on subsequent frames.
// Empirically (v0.2.25 [HP-VERIFY] probe): hp_max=38/39 at *→900 round end,
// but =1 at 100→110 round start. By 112→200 the fighter init has run +
// the announce intro is done -- hp_max + timer are both correct, and the
// "fight has begun" moment is a more useful seek anchor for replay
// viewing than the round-init init frame.
constexpr int RSS_FIGHT_LATCH      = 112;
constexpr int RSS_ACTIVE           = 200;
constexpr int RSS_ROUND_END_BANNER = 900;

// RSS_MATCH_COMPLETE = the step the engine only reaches when the round limit
// is exhausted. 901's body (0x4096ED) increments the substate to 902 and falls
// into 902's body (0x40970A) in the SAME dispatcher call, where the round-limit
// test splits:
//   next round      @ 0x4097F0: substate = RSS_BATTLE_INIT (100), then
//                               ResetObjectsAndCalculateSpeed() -- the engine
//                               demotes every type>1 object itself.
//   match complete  @ 0x409825: create_game_object(10, 0x7F, 0, 0) and return
//                               -- NO demote; every battle object stays type 4.
// So the OBSERVABLE post substate is 100 for a next round and 902 for a match
// end. `post >= 902` is exactly the match-complete seam and can never fire on
// a normal inter-round transition; 902 is also the highest substate the state
// machine ever holds (vs_round_function's header comment: 900-901 = round end
// -> loop to 100 or spawn game_object(10/13,127)).
constexpr int RSS_MATCH_COMPLETE   = 902;

// ROUND_START payload sources
constexpr uintptr_t ADDR_P1_HP_MAX  = 0x004DFC91;
constexpr uintptr_t ADDR_P2_HP_MAX  = 0x004EDCD0;
constexpr uintptr_t ADDR_SCORE_VAL  = 0x00470050;  // (g_score_value + 1) / 100 → seconds

// ROUND_END payload sources
constexpr uintptr_t ADDR_P1_HP             = 0x004DFC85;
constexpr uintptr_t ADDR_P2_HP             = 0x004EDCC4;
constexpr uintptr_t ADDR_P1_RESULT_KIND    = 0x004DFD87;  // 1=win 2=draw 3=loss
constexpr uintptr_t ADDR_P2_RESULT_KIND    = 0x004EDDC6;

typedef char (__cdecl *VsRoundFunc_t)();
static VsRoundFunc_t orig_vs_round_function = nullptr;

// 1-based intra-match round counter. Reset by RoundEvents_OnMatchStart at
// every Netplay_StartBattle so the first ROUND_START of each match emits
// idx=1. NOT derived from result_kind/round_wins because draws bump both
// win counters and break the formula.
static uint8_t s_round_idx_counter = 0;

// KOF-style retention state. Snapshot is captured at the round-end edge
// (pre != 900 → post == 900) for the winning side, and applied at the
// next round's RSS_FIGHT_LATCH → RSS_ACTIVE edge by writing the
// snapshotted HP and super_meter back into the active char slot.
//
// Toggle: FM2K_TEAM_KOF_RETENTION env var → RoundEvents_SetKofRetention.
// Match-end: cleared in RoundEvents_OnMatchStart so a fresh match starts
// at full HP/meter regardless of the previous match's last round outcome.
#include <atomic>
static std::atomic<bool> g_kof_retention_enabled{false};
static struct KofSnapshot {
    bool     pending = false;
    uint8_t  winner_idx = 0;       // 0=P1, 1=P2 (only valid if pending)
    uint32_t winner_hp = 0;
    uint32_t winner_meter = 0;
} s_kof_snapshot;

// Substate values from the round-state machine (subset of the full set):
//   100 = RSS_BATTLE_INIT -- vs_round_function's round-init runs here, which
//         INCLUDES the HP reset loop at 0x40899B-B0 (zeroes all 8 char slots,
//         then player_data_file_loader repopulates with max_hp from file).
//   110 = RSS_ANNOUNCE_WAIT -- by this state, HP has been reset to max.
//   112 = RSS_FIGHT_LATCH (existing constant)
//   200 = RSS_ACTIVE (existing constant)
//   900 = RSS_ROUND_END_BANNER (existing constant) -- banner shows + result_kind set
//   901 = RSS_ROUND_END_DONE -- banner finishes, transitions back to BATTLE_INIT
//
// KOF apply edge: 100 → 110 (post-reset, before announce). Empirically the
// HP reset happens during substate 100; by 110 the new round's HP_max is
// stamped and our write of winner's snapshotted HP sticks. Trace from
// 2026-05-10 (FM2K_P1_Debug.log):
//   18:45:58.709  901 → 100   ← reset happens here
//   18:45:58.761  100 → 110   ← apply here
//   18:46:00.263  110 → 112
//   18:46:00.563  112 → 200   ← old (broken) apply edge, too late visually
constexpr int RSS_BATTLE_INIT   = 100;
constexpr int RSS_ANNOUNCE_WAIT = 110;

constexpr uintptr_t ADDR_GAME_MODE_FLAG_RND      = 0x00470058;  // 0=story 1=VS 2=team
constexpr uintptr_t ADDR_P1_SUPER_METER          = 0x004DFCDD;  // g_charslot0_super_meter
constexpr uintptr_t ADDR_P2_SUPER_METER          = 0x004EDD1C;  // g_charslot1_super_meter

// Alternation guard. The pre→post substate edge fires multiple times per
// logical round transition because gekkonet's rollback re-simulates
// frames (runahead=4 means 1 forward + 4 replay = 5 fires per logical
// edge). The g_is_rolling_back gate at the top of Hook_vs_round_function
// is supposed to suppress replay fires but apparently isn't being set
// during gekkonet replay -- separate issue to track down.
//
// Belt-and-suspenders dedup: enforce strict START→END→START→END
// alternation. After a START fires, only an END can fire next; vice
// versa. Each round's edge can only emit once regardless of how many
// duplicate fires the rollback machinery produces. Reset on match start.
enum class LastEmit : uint8_t { NONE, ROUND_START, ROUND_END };
static LastEmit s_last_emit = LastEmit::NONE;

static int ReadSubstate() {
    void* slot = *(void**)ADDR_G_OBJECT_DATA_PTR;
    if (!slot) return -1;
    return *(int*)((char*)slot + OFF_ROUND_SUBSTATE);
}

// task #53 exports (decls in hooks.h). LiveSubstate: the round object's
// CURRENT substate (900+ = end/transition sequence, the window where the
// engine frees battle heap and loads CSS assets). ClearAfterimageIndices:
// zero every object's afterimage pool index (BYTE +0x151, read by
// sprite_rendering_engine at 0x40cd15) so no render can chain into heap
// the transition freed.
int RoundEvents_LiveSubstate() { return ReadSubstate(); }

void Fm2k_ClearAfterimageIndices() {
    uint8_t* pool = (uint8_t*)0x4701E0;
    for (int i = 0; i < 1024; ++i) pool[i * 382 + 0x151] = 0;
}

// ---------------------------------------------------------------------------
// Match-end script-VM seam. Direct sibling of the task #53 afterimage fix, one
// dangling field over: there the stale per-object index was the afterimage slot
// (+0x151) into the sprite pool, here it is the script cursor (+0x2C) into the
// character slot's commands_ptr blob, and the consumer that AVs is
// character_state_machine's per-opcode fetch (csm_per_opcode_site, 0x4125FC:
// `mov edx,[ebx+114h]; shl edi,4; add edi,edx; mov al,[edi]`) instead of
// sprite_rendering_engine.
//
// FM2K ObjectSlot (382 B stride @ g_object_pool 0x4701E0), fields used below:
//   +0x000 type              int  -- 4 = script VM (g_object_function_table[4]
//                                    = character_state_machine @ 0x411BF0)
//   +0x02C script_item_idx   int  -- the cursor, * 16 into commands_ptr
//   +0x030 script_id         int  -- * 39 into action_table
//   +0x151 afterimage_slot   byte -- the task #53 field
//   +0x152 script_init_state int  -- VM lifecycle guard, see below
//   +0x156 player_slot_id    int  -- * 0xE03F into g_char_slots_0
//   +0x15A entity_kind       int  -- 0/1/5 select a char slot; 2/3/4 select the
//                                    STATIC g_kgt_file_buffer /
//                                    g_story_char_config / g_char_physics_table
//                                    (never freed, never a hazard)
constexpr uintptr_t ADDR_OBJECT_POOL_RND  = 0x004701E0;
constexpr size_t    OBJ_STRIDE_RND        = 382;
constexpr size_t    OBJ_COUNT_RND         = 1024;
constexpr ptrdiff_t OFF_OBJ_TYPE          = 0x000;
constexpr ptrdiff_t OFF_OBJ_SCRIPT_CURSOR = 0x02C;
constexpr ptrdiff_t OFF_OBJ_SCRIPT_ID     = 0x030;
constexpr ptrdiff_t OFF_OBJ_INIT_STATE    = 0x152;
constexpr ptrdiff_t OFF_OBJ_PLAYER_SLOT   = 0x156;
constexpr ptrdiff_t OFF_OBJ_ENTITY_KIND   = 0x15A;
constexpr int       OBJ_TYPE_SCRIPT_VM    = 4;

// character_state_machine's entry guard @ 0x411C3F:
//     mov eax,[esi+152h] ; xor ebp,ebp ; sub eax,ebp ; mov edx,1
//     jz  loc_411C62     ; 0 -> one-time init, then state = 1
//     dec eax
//     jz  loc_412238     ; 1 -> opcode dispatch (the crash path)
//     pop/pop/pop/pop ; add esp,11Ch ; retn      ; anything else -> RETURN
// The return happens BEFORE any dereference of the character slot: the slot
// ADDRESS is computed at 0x411C12-1E (imul/add on the static base) but nothing
// reads +0x110 / +0x114 until the dispatch path. So parking a live VM object at
// state 2 makes it structurally incapable of touching the freed blob.
constexpr int CSM_STATE_INIT     = 0;
constexpr int CSM_STATE_RUNNING  = 1;
constexpr int CSM_STATE_DONE     = 2;

// g_char_slots_0 -- the ENGINE base. 0x411C1E is unambiguous
// (`add ebx, offset g_char_slots_0` -> 0x4D1D80). This is deliberately NOT
// savestate.h's CHAR_SLOT_BASE (0x4D1D90), which is shifted +16 and whose
// consumers (hooks_getinput.cpp's offsets) are self-consistent with the shift
// -- correcting that is a separate change. The code below replicates the
// engine's own address math, so it must use the engine's own base.
constexpr uintptr_t ADDR_CHAR_SLOTS_0     = 0x004D1D80;
constexpr size_t    CHAR_SLOT_STRIDE_RND  = 0xE03F;  // 57407
constexpr ptrdiff_t OFF_CHAR_COMMANDS_PTR = 0x114;   // GlobalAlloc(16 * item_count)
constexpr size_t    NUM_CHAR_SLOTS_RND    = 8;
constexpr size_t    SCRIPT_ITEM_SIZE      = 16;

// [ENDSEAM-OOB] fencepost. At the match-complete seam, every LIVE script-VM
// object is one `mov al,[edi]` away from reading commands_ptr + cursor*16; the
// crash only manifests when that address happens to land on a decommitted page,
// so the bad read is far more common than the AV. GlobalSize() on a freed
// handle returns 0, so a single probe catches BOTH failure modes: the dangling
// pointer (blob freed by the CSS entry's ClearCharacterSlot) and the shrunk
// blob (CSS reloaded a character with fewer script items).
//
// Always-on, not env-gated: the two call sites are the 902 sim edge (a couple
// of frames per match) and the post-teardown branch of SaveState_Load (only
// rollback loads inside the ~26-frame battle-end window), so this is tens of
// calls per match, each a 1024-slot walk of one dword plus a GlobalSize per
// live script object (< 20 in 1v1). A heisencrash detector that is off by
// default catches nothing.
void Fm2k_CheckEndSeamScriptCursors(const char* where) {
    const uint8_t* pool = (const uint8_t*)ADDR_OBJECT_POOL_RND;
    int reported = 0;
    int violations = 0;
    for (size_t i = 0; i < OBJ_COUNT_RND; ++i) {
        const uint8_t* obj = pool + i * OBJ_STRIDE_RND;
        if (*(const int*)(obj + OFF_OBJ_TYPE) != OBJ_TYPE_SCRIPT_VM) continue;
        // Already inert (state >= 2, or the neutralize below already ran):
        // the entry guard returns without touching commands_ptr, so a stale
        // cursor there is unreachable. Skipping keeps the probe from
        // re-reporting the same objects on every frame of the seam.
        const int init_state = *(const int*)(obj + OFF_OBJ_INIT_STATE);
        if (init_state != CSM_STATE_INIT && init_state != CSM_STATE_RUNNING) continue;
        const int kind = *(const int*)(obj + OFF_OBJ_ENTITY_KIND);
        if (kind != 0 && kind != 1 && kind != 5) continue;  // static-table kinds
        const int slot = *(const int*)(obj + OFF_OBJ_PLAYER_SLOT);
        if (slot < 0 || (size_t)slot >= NUM_CHAR_SLOTS_RND) continue;

        const int cursor = *(const int*)(obj + OFF_OBJ_SCRIPT_CURSOR);
        void* blob = *(void* const*)(ADDR_CHAR_SLOTS_0 +
                                     (size_t)slot * CHAR_SLOT_STRIDE_RND +
                                     OFF_CHAR_COMMANDS_PTR);
        const size_t blob_size = blob ? (size_t)GlobalSize((HGLOBAL)blob) : 0;
        const size_t need = (cursor < 0)
            ? (size_t)-1
            : (size_t)cursor * SCRIPT_ITEM_SIZE + SCRIPT_ITEM_SIZE;
        if (need <= blob_size) continue;

        ++violations;
        if (reported < 8) {  // cap: 1024 slots could otherwise flood the log
            ++reported;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[ENDSEAM-OOB] at=%s obj=%zu slot=%d script=%d cursor=%d "
                "need=%zu blob=%p size=%zu init_state=%d kind=%d",
                where ? where : "?", i, slot,
                *(const int*)(obj + OFF_OBJ_SCRIPT_ID), cursor,
                need, blob, blob_size, init_state, kind);
        }
    }
    if (violations > reported) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[ENDSEAM-OOB] at=%s %d more violation(s) suppressed",
            where ? where : "?", violations - reported);
    }
}

// Park every LIVE script-VM object at script_init_state = 2 so
// character_state_machine returns at its entry guard instead of fetching an
// opcode through the character slot's commands_ptr.
//
// Why this and NOT a replica of ResetObjectsAndCalculateSpeed @ 0x406450 (the
// demote the next-round branch runs): that function walks the pool and writes
// `type = 1` to EVERY slot with type > 1 except g_object_data_ptr, and at the
// match-complete seam the pool contains one object it must not touch -- the
// type-10 object create_game_object(10, 0x7F, ...) just spawned at 0x409825,
// which is game_state_manager, i.e. the object that performs the whole CSS
// transition (its STATE 0 sets g_game_mode = 2000 at 0x407247). Demoting it
// would leave the match in limbo with no driver. The next-round branch never
// has that problem because its demote runs BEFORE any type-10 spawn. Adding an
// ad-hoc "except type 10" carve-out to a replica of an engine function is
// exactly the kind of drift that rots; parking the script VM touches only
// type == 4, which is precisely the set of objects that can reach 0x4125FC.
//
// Secondary benefits over the demote: the objects keep their sprite fields so
// the results transition still draws them (a demote makes them vanish a frame
// early), and nothing else in the engine reads +0x152 on a type-4 slot.
// Phase 1bc instrument (DIAGNOSTIC): how many slots the neutralize below WOULD
// change, computed before it runs. 0 = the call is a pure identity write, > 0 =
// this restore is about to park VMs the snapshot had unparked. Read-only, same
// walk shape as the neutralize itself.
int Fm2k_CountParkableScriptObjects() {
    const uint8_t* pool = (const uint8_t*)ADDR_OBJECT_POOL_RND;
    int n = 0;
    for (size_t i = 0; i < OBJ_COUNT_RND; ++i) {
        const uint8_t* obj = pool + i * OBJ_STRIDE_RND;
        if (*(const int*)(obj + OFF_OBJ_TYPE) != OBJ_TYPE_SCRIPT_VM) continue;
        const int st = *(const int*)(obj + OFF_OBJ_INIT_STATE);
        if (st == CSM_STATE_INIT || st == CSM_STATE_RUNNING) ++n;
    }
    return n;
}

void Fm2k_NeutralizeMatchEndScriptObjects() {
    uint8_t* pool = (uint8_t*)ADDR_OBJECT_POOL_RND;
    for (size_t i = 0; i < OBJ_COUNT_RND; ++i) {
        uint8_t* obj = pool + i * OBJ_STRIDE_RND;
        if (*(const int*)(obj + OFF_OBJ_TYPE) != OBJ_TYPE_SCRIPT_VM) continue;
        int* init_state = (int*)(obj + OFF_OBJ_INIT_STATE);
        // Monotone + idempotent: only 0 (pending init) and 1 (running) can
        // reach the opcode fetch, and anything else already returns. Re-running
        // this on an already-parked pool is a no-op, which is what makes the
        // restore-side call in SaveState_Load a pure identity write whenever the
        // restored frame is at or past the 902 edge.
        if (*init_state == CSM_STATE_INIT || *init_state == CSM_STATE_RUNNING) {
            *init_state = CSM_STATE_DONE;
        }
    }
}

static char __cdecl Hook_vs_round_function() {
    const int pre = ReadSubstate();

    char ret = orig_vs_round_function ? orig_vs_round_function() : 0;

    const int post = ReadSubstate();

    // task #53 SIM-SIDE afterimage kill during the end sequence. Once the
    // round object enters 900+ (results -> match teardown; the 902 step
    // frees .player heap and loads CSS assets), afterimage pool indices
    // must never survive to a render: a rollback batch that restores to a
    // pre-end frame RE-WRITES them during its resim (trail spawns re-run)
    // while the live pass already freed the heap they point at ->
    // sprite_rendering_engine AVs at 0x40cd47. Clearing here runs in the
    // SIM tick -- identical on both peers, in forward passes AND resims --
    // so the poisoned state is structurally unreachable, unlike the
    // teardown/restore-time clears (which a mid-batch resim outruns).
    // Visual cost: afterimage trails vanish on the results screen, on both
    // peers identically.
    if (post >= 900) {
        Fm2k_ClearAfterimageIndices();
    }

    // Match-COMPLETE seam. Same argument as the block above, one field over:
    // a battle object's script cursor (+0x2C) indexes the character slot's
    // commands_ptr blob (+0x114), which the CSS entry GlobalFree's
    // (ClearCharacterSlot -> resource_cleanup_manager, which does NOT null the
    // pointers) and then re-GlobalAlloc's at a different address and a
    // different SIZE for whatever character the CSS loads next. Unlike the
    // next-round branch, the match-complete branch @ 0x409825 never runs
    // ResetObjectsAndCalculateSpeed, so every fighter/projectile/effect stays
    // type 4 and keeps ticking into the transition; a rollback restore that
    // resurrects them then feeds a battle-era cursor to a post-CSS blob and
    // character_state_machine AVs at 0x4125FC (host crash, run_seed83).
    //
    // Sim-tick placement is the load-bearing part, exactly as in v0.2.81: this
    // runs identically on both peers, in forward passes AND in every resim, so
    // a rollback to a frame before 902 replays through this edge and re-applies
    // it, and a rollback to a frame at/after 902 restores an already-parked
    // pool. The poisoned state is structurally unreachable rather than merely
    // cleaned up at teardown time (which a mid-batch resim outruns).
    //
    // Determinism: same write, same logical frame, both peers. `post >= 902` is
    // observable only on the match-complete branch (see RSS_MATCH_COMPLETE) --
    // a normal next-round seam reports post == 100 and is left alone, which
    // matters because objects legitimately survive a next-round transition
    // until the engine's own demote kills them.
    if (post >= RSS_MATCH_COMPLETE) {
        Fm2k_CheckEndSeamScriptCursors("sim902");
        Fm2k_NeutralizeMatchEndScriptObjects();
        // [ENDSEAM-FREE] window latch (telemetry only, see seam_free_probe.h).
        // Deliberately placed here, i.e. ABOVE the g_is_rolling_back early
        // return further down, so the window opens on resim passes too and
        // stays open once any pass has reached the edge. A rollback back past
        // the edge keeping the window OPEN is the correct behaviour: a
        // teardown-crossing load is exactly the event the window covers.
        SeamFreeProbe_OnMatchCompleteTick();
    }

    // AI field writes per frame: drive ai_input_processor's switch via
    // [slot+0xDF65] so the engine runs CPU AI (case 1), Imitate (case 2),
    // or Jump (case 4) for the hijacked slot. Per-frame instead of one-
    // shot because:
    //   (a) health_damage_manager @ 0x40EA63 writes [slot+0xDF65] when the
    //       CPU takes damage and can clobber our mode.
    //   (b) training F2 cycles change the desired mode mid-battle and we
    //       need the next frame to reflect it without a separate hook.
    //   (c) char_state_machine's CSMK_PLAYER init runs the first frame
    //       after spawn -- its writes would clobber any pre-init one-shot.
    // No-op when no hijack submode is active.
    PerGamePatches_OnBattleInitComplete();

    // KOF-style retention runs BEFORE the netplay/host guard below
    // because it's a local state mutation, not a broadcast event.
    // Both peers need to apply the retention to stay in sync.
    //
    // Strategy: snapshot at the round-end edge (any → 900), apply at
    // the post-reset edge (100 → 110). The engine's HP reset loop
    // runs during substate 100 (RSS_BATTLE_INIT @ vs_round_function
    // 0x40899B-B0); by 110 (RSS_ANNOUNCE_WAIT) the reset is done and
    // our write sticks until next round-end.
    //
    // Earlier delayed-frame approach (LilithPort-style 1-sec sleep)
    // didn't work in our impl because the apply landed at substate
    // 901 -- HP was still the snapshotted value (write was a no-op),
    // and the engine reset at 901→100 happened ~2sec after snapshot.
    // Substate-edge apply at 100→110 is unambiguous and post-reset.
    if (g_kof_retention_enabled.load(std::memory_order_relaxed)) {
        const uint32_t mode_flag = *(const uint32_t*)ADDR_GAME_MODE_FLAG_RND;

        // Trace every substate change while KOF is on so we can see
        // the full transition timeline.
        if (pre != post) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[KOF-TRACE] substate %d → %d (mode_flag=%u, "
                "pending=%d snap_winner=%u snap_hp=%u snap_meter=%u)",
                pre, post, (unsigned)mode_flag,
                (int)s_kof_snapshot.pending,
                (unsigned)s_kof_snapshot.winner_idx,
                s_kof_snapshot.winner_hp, s_kof_snapshot.winner_meter);
        }

        // Snapshot: at any → RSS_ROUND_END_BANNER edge, capture the
        // winner's HP/meter so we can restore at next round init.
        const bool snapshot_edge = (pre != RSS_ROUND_END_BANNER &&
                                    post == RSS_ROUND_END_BANNER);
        if (snapshot_edge) {
            if (mode_flag != 2u) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[KOF-RETAIN] snapshot edge hit but mode_flag=%u "
                    "(need 2 = team) -- skip", (unsigned)mode_flag);
            } else {
                const uint32_t r1_kind = *(uint32_t*)ADDR_P1_RESULT_KIND;
                const uint32_t r2_kind = *(uint32_t*)ADDR_P2_RESULT_KIND;
                const uint8_t  widx    = (r1_kind == 1) ? 0
                                       : (r2_kind == 1) ? 1
                                       : 2;
                const uint32_t p1_hp_now    = *(uint32_t*)ADDR_P1_HP;
                const uint32_t p2_hp_now    = *(uint32_t*)ADDR_P2_HP;
                const uint32_t p1_meter_now = *(uint32_t*)ADDR_P1_SUPER_METER;
                const uint32_t p2_meter_now = *(uint32_t*)ADDR_P2_SUPER_METER;
                if (widx >= 2u) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[KOF-RETAIN] snapshot edge: draw "
                        "(r1_kind=%u r2_kind=%u) -- no snapshot taken",
                        r1_kind, r2_kind);
                } else {
                    s_kof_snapshot.pending      = true;
                    s_kof_snapshot.winner_idx   = widx;
                    s_kof_snapshot.winner_hp    = (widx == 0u) ? p1_hp_now : p2_hp_now;
                    s_kof_snapshot.winner_meter = (widx == 0u) ? p1_meter_now : p2_meter_now;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[KOF-RETAIN] SNAPSHOT: winner=P%u hp=%u meter=%u "
                        "(r1_kind=%u r2_kind=%u, p1_hp=%u p2_hp=%u "
                        "p1_meter=%u p2_meter=%u) -- apply at next 100→110",
                        (unsigned)widx + 1u,
                        s_kof_snapshot.winner_hp, s_kof_snapshot.winner_meter,
                        r1_kind, r2_kind, p1_hp_now, p2_hp_now,
                        p1_meter_now, p2_meter_now);
                }
            }
        }

        // (Apply path removed -- Option-A code-cave patch on
        // character_state_machine 0x411CB1 intercepts the HP write
        // directly, so the engine never overwrites with max_hp for
        // the winner's slot. See PerGamePatches_InstallKofHpInitPatch
        // in per_game_patches.cpp. The snapshot here remains for the
        // interceptor to read.)
    }

    // Emit from BOTH player nodes (host index 0 + guest index 1) so each
    // player's own local .fm2krep carries ROUND_START/END (and thus
    // round_offsets for round-level seek). Each player is the SINGLE source
    // for its own spectator subtree -- host and guest subtrees are disjoint,
    // so emitting on both is NOT a double-broadcast. SPECTATOR / relay nodes
    // (index 2) are still skipped: they RECEIVE round events via the relay
    // stream AND run vs_round_function locally, so emitting there would
    // double-source. (Daisy-chain replay is handled in the Hop-1 relay branch
    // of HandleSpecData::EVENT_BATCH.) Previously this was host-only
    // (g_player_index != 0), which is why a guest's replay had no round
    // markers -- and in real netplay the guest wrote no file at all.
    //
    // g_is_rolling_back guards against re-emit during a GekkoNet replay
    // window -- the same logical edge would fire multiple times across
    // forward + replay sims and produce duplicate events.
    if (g_player_index > 1 || g_is_rolling_back) {
        return ret;
    }

    // [RND-EDGE] substate transition trace (host, non-rollback only) --
    // a handful of lines per round, permanently cheap, and the ground
    // truth for edge-condition bugs like the one below.
    if (pre != post) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[RND-EDGE] %d -> %d", pre, post);
    }

    // ROUND_START -- fires on ANY entry into RSS_ACTIVE (the moment the
    // battle becomes interactive), mirroring the END edge's shape. The
    // old exact (RSS_FIGHT_LATCH=112 -> 200) pair silently never matched:
    // per the 0x4086A0 decompile, the announce chain 110->111->112->113
    // ->200 advances on per-state countdowns that all live in the SAME
    // dispatcher call when a counter starts expired, so several states
    // collapse into one call and the observable pre can be 110/111/112
    // depending on sprite timing. Result: zero [ROUND-START]s, round=0
    // in every END, and the alternation guard then ate every second
    // round's END too. Any-entry matches all collapse shapes; the
    // alternation guard + g_is_rolling_back gate dedup re-traversals
    // exactly as they do for the END edge. HP_max is populated by the
    // ACTIVE entry (see constexpr block above for the rationale).
    if (pre != RSS_ACTIVE && post == RSS_ACTIVE &&
        s_last_emit != LastEmit::ROUND_START) {
        ++s_round_idx_counter;
        s_last_emit = LastEmit::ROUND_START;
        const uint16_t p1_hp_max     = (uint16_t)*(uint32_t*)ADDR_P1_HP_MAX;
        const uint16_t p2_hp_max     = (uint16_t)*(uint32_t*)ADDR_P2_HP_MAX;
        const int32_t  score         = *(int32_t*)ADDR_SCORE_VAL;
        const uint16_t timer_seconds = (score >= 0)
            ? (uint16_t)((score + 1) / 100)
            : (uint16_t)0;
        SpectatorNode_AppendRoundStart(
            s_round_idx_counter, p1_hp_max, p2_hp_max, timer_seconds);
        // PER-BATTLE SETTINGS STAMP. `timer=` alone has always been here and
        // is the decisive cross-peer check for the lost-HOST_CONFIG class
        // (host 15s vs guest 60s => g_score_value 1499 vs 5999 => the peers
        // run an entire battle on divergent round state). The rest of the
        // settings that RSS_BATTLE_INIT / process_game_inputs consume are just
        // as capable of diverging silently, so stamp all of them plus the
        // digest the entry barrier agreed on and the count of HOST_CONFIG
        // packets this node ever applied (0 on a guest = smoking gun).
        // Diffing this ONE line between the two peers' logs decides the whole
        // question -- see the triage card in the Phase 1 diagnosis.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[ROUND-START] round=%u p1_hp_max=%u p2_hp_max=%u timer=%us "
            "rounds=%u speed=%u socd=%d stage=%u cfg=0x%08X cfg_rx=%u",
            s_round_idx_counter, p1_hp_max, p2_hp_max, timer_seconds,
            *(uint32_t*)0x430124,                  // g_default_round
            *(uint32_t*)0x430104,                  // uValue (GameSpeed)
            Hook_GetSOCDModePublic(),
            *(uint32_t*)FM2K::ADDR_SELECTED_STAGE,
            Netplay_MatchSettingsDigest(),
            Netplay_HostConfigRxCount());
    }

    // ROUND_END -- substate just transitioned to RSS_ROUND_END_BANNER from
    // any of the win-tail paths. result_kind / HP are already populated.
    if (pre != RSS_ROUND_END_BANNER && post == RSS_ROUND_END_BANNER &&
        s_last_emit != LastEmit::ROUND_END) {
        s_last_emit = LastEmit::ROUND_END;
        const uint32_t r1_kind = *(uint32_t*)ADDR_P1_RESULT_KIND;
        const uint32_t r2_kind = *(uint32_t*)ADDR_P2_RESULT_KIND;
        const uint8_t winner_idx = (r1_kind == 1) ? 0
                                : (r2_kind == 1) ? 1
                                : 2;  // draw / double-KO / unrecognized
        const uint16_t p1_hp = (uint16_t)*(uint32_t*)ADDR_P1_HP;
        const uint16_t p2_hp = (uint16_t)*(uint32_t*)ADDR_P2_HP;
        SpectatorNode_AppendRoundEnd(winner_idx, p1_hp, p2_hp);

        // Host-side diagnostic log so a normal P1+P2 match (no spectator
        // attached) still produces a per-round trail in the .log file.
        // Spectator-side ApplySessionEvent has its own log when a viewer
        // is connected; this duplicates it for the host's own log so
        // grep'ing logs/FM2K_P*_Debug.log shows round outcomes inline.
        const uint32_t p1_wins   = *(uint32_t*)0x4DFC6D;
        const uint32_t p2_wins   = *(uint32_t*)0x4EDCAC;
        const int32_t  score_val = *(int32_t*)ADDR_SCORE_VAL;
        const uint16_t timer_remaining = (score_val >= 0)
            ? (uint16_t)((score_val + 1) / 100)
            : (uint16_t)0;
        const char* who = (winner_idx == 0) ? "P1"
                        : (winner_idx == 1) ? "P2" : "DRAW";
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[ROUND-END] round=%u winner=%s rounds_won=%u-%u "
            "p1_hp=%u p2_hp=%u timer_remaining=%us "
            "(r1_kind=%u r2_kind=%u)",
            s_round_idx_counter, who,
            p1_wins, p2_wins,
            p1_hp, p2_hp, timer_remaining,
            r1_kind, r2_kind);
        // (KOF-style retention snapshot lives BEFORE the netplay guard
        // above so it fires on every peer's local instance, not just P1.)
    }

    return ret;
}

bool RoundEvents_Install() {
    if (MH_CreateHook((void*)ADDR_VS_ROUND_FUNCTION,
                      (void*)Hook_vs_round_function,
                      (void**)&orig_vs_round_function) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "RoundEvents: MH_CreateHook(vs_round_function @ %p) failed",
            (void*)ADDR_VS_ROUND_FUNCTION);
        return false;
    }
    // Queue only -- caller (InitializeHooks) flushes all hooks with one
    // MH_ApplyQueued so we pay ONE thread-freeze cost across the whole boot
    // path instead of one per hook.
    if (MH_QueueEnableHook((void*)ADDR_VS_ROUND_FUNCTION) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "RoundEvents: MH_QueueEnableHook(vs_round_function) failed");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "RoundEvents: queued vs_round_function @ 0x%08X for ROUND_START/END emit",
        (unsigned)ADDR_VS_ROUND_FUNCTION);
    return true;
}

void RoundEvents_OnMatchStart() {
    s_round_idx_counter = 0;
    s_last_emit         = LastEmit::NONE;
    s_kof_snapshot.pending = false;
    SeamFreeProbe_CloseWindow();
}

void RoundEvents_SetKofRetention(bool enabled) {
    g_kof_retention_enabled.store(enabled, std::memory_order_relaxed);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "RoundEvents: KOF-style HP/meter retention %s",
        enabled ? "ENABLED" : "disabled");
}

// Snapshot accessors -- exposed so PerGamePatches's HpInitInterceptor
// can read snapshot state from inside the patched CSMK_PLAYER init.
bool RoundEvents_KofRetentionEnabled() {
    return g_kof_retention_enabled.load(std::memory_order_relaxed);
}
bool RoundEvents_KofSnapshotPending() {
    return s_kof_snapshot.pending;
}
int RoundEvents_KofSnapshotWinnerIdx() {
    return (int)s_kof_snapshot.winner_idx;
}
uint32_t RoundEvents_KofSnapshotWinnerHp() {
    return s_kof_snapshot.winner_hp;
}
uint32_t RoundEvents_KofSnapshotWinnerMeter() {
    return s_kof_snapshot.winner_meter;
}
void RoundEvents_KofSnapshotMarkApplied() {
    s_kof_snapshot.pending = false;
}

#else  // ENGINE_FM95

#include "round_events.h"

bool RoundEvents_Install()           { return true; }  // FM95 emit out of scope (separate hand-off)
void RoundEvents_OnMatchStart()      {}
void RoundEvents_SetKofRetention(bool) {}
bool     RoundEvents_KofRetentionEnabled()    { return false; }
bool     RoundEvents_KofSnapshotPending()     { return false; }
int      RoundEvents_KofSnapshotWinnerIdx()   { return 0; }
uint32_t RoundEvents_KofSnapshotWinnerHp()    { return 0; }
uint32_t RoundEvents_KofSnapshotWinnerMeter() { return 0; }
void     RoundEvents_KofSnapshotMarkApplied() {}

#endif
