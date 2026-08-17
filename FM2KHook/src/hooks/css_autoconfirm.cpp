// CSS auto-lock-and-confirm hook -- see header for design rationale and
// docs/analysis/css_state_machine.md for the IDA-pass that maps the state
// machine and globals.

#if !defined(ENGINE_FM95)

#include "css_autoconfirm.h"

#include <SDL3/SDL_log.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <windows.h>

#include "MinHook.h"

#include "../core/globals.h"  // FM2K::ADDR_GAME_MODE, ADDR_*
#include "per_game_patches.h" // PerGamePatches_GetTeamSizeOverride

namespace {

// FM2K addresses (verified 2026-05-08 via IDA decompile of game_state_manager
// @ 0x406FC0; full xref + flow doc in docs/analysis/css_state_machine.md).
constexpr uintptr_t ADDR_GAME_STATE_MANAGER = 0x00406FC0;
constexpr uintptr_t ADDR_P1_CURSOR_POS      = 0x00424E50;  // i32×2 {x, y}
constexpr uintptr_t ADDR_P2_CURSOR_POS      = 0x00424E58;
constexpr uintptr_t ADDR_SELECTED_CHAR      = 0x00470020;  // i32×2 [p1, p2]
constexpr uintptr_t ADDR_INPUT_CHANGES      = 0x00447F60;  // u32 per-player
constexpr uintptr_t ADDR_PROCESSED_INPUT    = 0x00447F40;  // u32 per-player
constexpr uintptr_t ADDR_P1_ACTION_STATE    = 0x0047019C;
constexpr uintptr_t ADDR_P2_ACTION_STATE    = 0x004701A0;
constexpr uintptr_t ADDR_STAGE_WIDTH        = 0x004452B8;  // u16
constexpr uintptr_t ADDR_STAGE_HEIGHT       = 0x004452BA;  // u16
constexpr uintptr_t ADDR_SELECTED_STAGE     = 0x0043010C;
constexpr uintptr_t ADDR_GAME_MODE          = 0x00470054;

// Team-mode dupe-lock supplementary addresses (verified via IDA disasm
// of game_state_manager @ 0x40752F and vs_round_function @ 0x408A07).
constexpr uintptr_t ADDR_GAME_MODE_FLAG     = 0x00470058;  // 0=story, 1=VS, 2=team
constexpr uintptr_t ADDR_P1_TEAM_HISTORY    = 0x0047006C;  // g_player_move_history (u32×4: P1 team chars)
constexpr uintptr_t ADDR_P2_TEAM_HISTORY    = 0x0047007C;  // g_p2_round_history_chars (u32×4: P2 team chars)
constexpr uintptr_t ADDR_P1_TEAM_SLOT_COUNT = 0x004700EC;  // g_p1_round_count -- # P1 chars locked so far
constexpr uintptr_t ADDR_P2_TEAM_SLOT_COUNT = 0x004700F0;  // g_p1_round_state -- # P2 chars locked so far (P2 counter, despite the name)

// AssignPlayerColor @ 0x406F20 reads `input_changes & 0x3E0` (bits 5..9) to
// pick color slot 1..5; if none of those bits set, the function falls
// through to color 0. The caller in game_state_manager triggers the call
// only when `input_changes & 0x3F0` (bits 4..9) is non-zero -- so bit 4
// (0x10) on its own confirms with color 0.
//
// Earlier versions of this file used a fixed `INJECTED_CONFIRM_BIT = 0x10`
// for both players, which always landed on color 0 regardless of what the
// recorded match used. That made offline replays look like every fight
// confirmed with palette 0 even when the original players picked colors
// 1..5 -- visible as wrong palette in the replay's CSS portrait + battle
// sprites.
//
// Replacement: per-player target bit derived from the MATCH_START header's
// p1_color / p2_color, mapped through ColorBitForSlot:
//
//   color 0 → 0x10  (bit 4 -- confirm-only, falls through to default 0)
//   color 1 → 0x20  (bit 5)
//   color 2 → 0x40  (bit 6)
//   color 3 → 0x80  (bit 7)
//   color 4 → 0x100 (bit 8)
//   color 5 → 0x200 (bit 9)
//
// We still OVERWRITE the 0x3F0 bits rather than OR -- pb_queue's leaked
// battle-input bits could otherwise set a different attack button and shift
// AssignPlayerColor's bit ladder onto the wrong color.
inline uint32_t ColorBitForSlot(uint8_t color) {
    if (color == 0u || color > 5u) return 0x10u;
    return 0x10u << color;  // 0x20, 0x40, 0x80, 0x100, 0x200
}

typedef char (__cdecl *GameStateManagerFn)();
GameStateManagerFn g_orig = nullptr;

// Cached MATCH_START targets. Set by CssAutoConfirm_OnReplayMatchStart, read
// every frame by the detour while active.
std::atomic<bool>    g_active{false};
std::atomic<uint8_t> g_target_p1_char{0};
std::atomic<uint8_t> g_target_p2_char{0};
std::atomic<uint8_t> g_target_p1_color{0};
std::atomic<uint8_t> g_target_p2_color{0};
std::atomic<uint8_t> g_target_stage_id{0};

// Tick counter for diag -- log first few frames of pinning then stay quiet so
// CSS doesn't blast the log file with 100+ "pinning chars" lines.
std::atomic<uint32_t> g_pin_tick{0};

// Team-mode dupe-lock toggle. Set from FM2K_TEAM_CSS_DUPE_LOCK env var at
// hook init; flipped per-game via the launcher's host config panel.
std::atomic<bool> g_team_dupe_lock{false};

// Spectator seam hold (Phase F) -- see header. Holds CSS unadvanceable
// while the spectator waits for the next MATCH_START's pin targets.
std::atomic<bool> g_seam_hold{false};
// Colors carried from the just-finished match for the held portraits.
// AssignPlayerColor only writes the per-slot color at confirm time, and
// the hold suppresses confirms -- without these writes the held CSS
// renders palette 0 for both players ("wrong colors" vs the battle they
// just watched). Per-slot color lives at slot+0xE00B (g_charslot0_color
// _pick @ 0x4DFD8B, byte stride 0xE03F; see AssignPlayerColor @ 0x406F20).
// 1v1 mapping: P1 = slot 0, P2 = slot 1 (team CSS out of scope here).
std::atomic<uint8_t> g_seam_hold_p1_color{0};
std::atomic<uint8_t> g_seam_hold_p2_color{0};
constexpr uintptr_t ADDR_CHARSLOT0_COLOR_PICK = 0x4DFD8B;
constexpr size_t    CHARSLOT_STRIDE_BYTES     = 0xE03F;

// ===================== ORPHANED-PREVIEW FIX (2026-08-17) ==================
//
// THE DEFECT. Phase A of the pin (below) forces `selected[i] = -1` with a bare
// write. The ENGINE never produces that state transition with a bare write. All
// three character-select flavours run the same shape, and the unload is ALWAYS
// paired with the -1:
//
//   if (selected[i] != grid) {
//       if (selected[i] == -1) { selected[i] = grid; player_data_file_loader();
//                                create_game_object(4, 0x50, ...); }
//       else                   { Css_UnloadPlayerPreview(slot); selected[i] = -1; }
//   }
//
// verified by disassembly of game_state_manager @ 0x406FC0 on 2026-08-17:
// 0x407425 (team), 0x407967 (VS 1v1), 0x407C2A (1P/story) -- and those three
// are Css_UnloadPlayerPreview's ONLY xrefs in the binary.
//
// Skipping the unload leaves the outgoing preview object ALIVE. The engine then
// creates a SECOND preview for the same player slot on the next engine pass,
// and player_data_file_loader (0x4039F0) replaces that char slot's script blob
// underneath the survivor whenever the pin target differs from what the viewer
// had selected (its no-reload guard is g_slot_loaded_char_idx[slot] == target;
// a real reload calls ClearCharacterSlot, which GlobalFrees the old blob). The
// orphan keeps executing with a script_idx (+0x30) and item_idx (+0x2C) that
// now index a DIFFERENT character's script buffer -- which is both the falling
// character-select object the campaign has been suppressing with a park, and an
// unmeasured out-of-bounds script fetch.
//
// THE FIX: restore the engine's own pairing, under exactly the engine's own
// precondition (selected[i] >= 0). No ramp window, no entity-kind heuristic, no
// per-game constant; it generalizes to every game and reaches the mid-CSS-join
// and offline-replay windows, because it lives in the pin and those paths run
// the pin.
//
// WHICH HALF IS THE FIX (review F1, and the code and the story must agree). On
// the PRIMARY path -- the engine's character-select block runs in this same
// g_orig() call -- the load-bearing half is NOT WRITING THE -1: the deferral is
// what makes the engine take its own `else { Css_UnloadPlayerPreview(); selected
// = -1; }` branch. The set our census marks at detour entry is the set the
// engine would mark inside g_orig() (nothing between the two points creates a
// type-4 object or iterates the pool), so on that path the explicit call below
// is redundant with the engine's own.
//
// The explicit call is kept because it is NOT redundant on the paths where the
// engine's block does not run on the frame the pin re-selects -- the mid-CSS
// join and the re-join/seam arm, where the arm can land while character-select
// is in state 0/2/3/4 or act != 0. There the pin's anti-hang fallback writes the
// -1 itself on the NEXT tick, and without this call that write is the bare
// pre-fix orphaning write. One call, those paths, that reason.
//
// PLANE CONTAINMENT. The pin arms from four sites only: spec_playback.cpp:284
// (offline replay + live spectator at a match-end seam), spec_playback.cpp:301
// (the battle-align path -- the mid-CSS joiner), spec_recv.cpp:430 (spectator
// SNAPSHOT_BEGIN) and the test-only netplay_css.cpp:407 (FM2K_TEST_CSS_CHAR,
// which appears in NO spec_selftest env list). No production player process
// reaches it, and no plane that reaches it ever rolls back.
constexpr uintptr_t ADDR_CSS_UNLOAD_PREVIEW = 0x00406520;
typedef int (__cdecl *CssUnloadPlayerPreviewFn)(int char_slot);

// Object pool literals -- same values as round_events.cpp / parity_pool.h /
// spec_css_tripwire.cpp, repeated because each of those TUs deliberately owns
// its own copy (WonderfulWorld absolute addresses, FM2K-only, and this whole
// file is FM2K-gated).
constexpr uintptr_t ADDR_OBJECT_POOL   = 0x004701E0;
constexpr size_t    OBJ_STRIDE         = 382;
constexpr size_t    OBJ_COUNT          = 1024;
constexpr ptrdiff_t OFF_OBJ_TYPE       = 0x000;
constexpr ptrdiff_t OFF_OBJ_POSY       = 0x00C;
constexpr ptrdiff_t OFF_OBJ_ITEM_IDX   = 0x02C;
constexpr ptrdiff_t OFF_OBJ_SCRIPT_IDX = 0x030;
constexpr ptrdiff_t OFF_OBJ_PLAYER     = 0x156;
constexpr ptrdiff_t OFF_OBJ_KIND       = 0x15A;
constexpr int       OBJ_TYPE_SCRIPT_VM = 4;

// g_slot_loaded_char_idx (int[8]) -- player_data_file_loader's no-reload guard
// at 0x4452CC + 4*slot + 567060. Reading it tells us, BEFORE the engine acts,
// whether this pin will swap the char slot's blob (the stale-blob precondition).
constexpr uintptr_t ADDR_SLOT_LOADED_CHAR = 0x004CF9E0;

// FM2K_SPEC_CSS_UNLOAD=0 restores the pre-fix behaviour byte-for-byte. This is
// the causality control (the RED arm), not a fallback: default ON.
bool CssUnloadEnabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("FM2K_SPEC_CSS_UNLOAD");
        cached = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
        if (cached == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[CSSPIN] DISABLED by FM2K_SPEC_CSS_UNLOAD=0 -- the pin will "
                "write selected=-1 without the engine's paired preview unload, "
                "orphaning the outgoing preview; causality control only");
        }
    }
    return cached == 1;
}

// The CHAR SLOT the engine binds previews to (+0x156), per flavour. Derived
// from the disassembly, not guessed: the VS 1v1 site pushes esi (the 0/1 player
// loop index), the 1P site pushes ebx (= 0), and the team site pushes
// lea edi,[esi+3] where esi is the BYTE offset 4*i, i.e. 4*i+3.
// Returns -1 = REFUSE TO ACT.
int PinCharSlotForPlayer(int i) {
    const uint32_t flag = *(const uint32_t*)ADDR_GAME_MODE_FLAG;
    if (flag == 1u) return i;                    // VS 1v1 -- the netplay/spectate pin
    if (flag == 0u) return (i == 0) ? 0 : -1;    // 1P/story -- one cursor, slot 0
    // Team (flag 2) uses 4*i+3, but team-mode netplay/spectate is an explicit
    // non-goal and the pin itself is not team-correct today: refuse rather than
    // unload a slot whose binding we do not drive.
    static bool s_warned = false;
    if (!s_warned) {
        s_warned = true;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[CSSPIN] g_game_mode_flag=%u (not 1v1 or 1P) -- declining the "
            "preview unload; team-mode char-select is not driven by this pin",
            flag);
    }
    return -1;
}

// The ENGINE's own "these preview objects belong to this char slot" predicate,
// verbatim from Css_UnloadPlayerPreview: type == 4 && entity_kind < 2 &&
// player_slot_id == char_slot. Returns the count and fills `detail` with up to
// `cap` "slot/script/item/y" records -- the slot identity the fall correlation
// needs (cross-referenced offline against [CSS-ANIM]'s per-slot born/adv/y).
int CensusPreviewObjects(int char_slot, char* detail, size_t detail_cap) {
    const uint8_t* pool = (const uint8_t*)ADDR_OBJECT_POOL;
    int n = 0;
    size_t at = 0;
    if (detail_cap) detail[0] = '\0';
    for (size_t i = 0; i < OBJ_COUNT; ++i) {
        const uint8_t* obj = pool + i * OBJ_STRIDE;
        if (*(const int*)(obj + OFF_OBJ_TYPE) != OBJ_TYPE_SCRIPT_VM) continue;
        if (*(const int*)(obj + OFF_OBJ_KIND) >= 2) continue;
        if (*(const int*)(obj + OFF_OBJ_PLAYER) != char_slot) continue;
        ++n;
        if (at + 40 < detail_cap) {
            const int len = std::snprintf(detail + at, detail_cap - at,
                "%u/%d/%d/%d,", (unsigned)i,
                *(const int*)(obj + OFF_OBJ_SCRIPT_IDX),
                *(const int*)(obj + OFF_OBJ_ITEM_IDX),
                *(const int*)(obj + OFF_OBJ_POSY) >> 16);
            if (len > 0) at += (size_t)len;
        }
    }
    return n;
}

// Per-arm [CSSPIN] accumulator. One line per pin ARM (not per frame): two
// 1024-slot walks per player per arm, 2-4 arms per session, no per-frame work.
bool g_pin_line_pending = false;
bool g_pin_unloaded[2]  = { false, false };
char g_pin_note[2][192];

// Returns true if the teardown was performed and the caller must therefore let
// the ENGINE write selected[i] = -1 (see the TIMING note at the call site).
//
// `*burn_one_shot` is set only when an ACTUAL re-select teardown was REACHED --
// i.e. the flavour resolved a char slot AND the engine's own else-branch
// condition (prev_sel >= 0) held, so the census ran. It is deliberately NOT set
// on the two declined paths (unknown flavour; nothing bound to tear down),
// because the caller's one-shot is what pairs the unload with the -1: burning it
// on a path that unloaded nothing would spend the pairing before the first real
// re-select of the arm and leave that one writing the bare pre-fix -1 (review
// F1(c)). The one-shot still exists, and is still per arm: it stops the second
// and later frames of the SAME re-select from walking the pool again, and it is
// the anti-hang fallback's arming condition.
bool PinUnloadOutgoingPreview(int i, int target_char, int prev_sel,
                              bool* burn_one_shot) {
    char* note = g_pin_note[i];
    const int slot = PinCharSlotForPlayer(i);
    if (slot < 0) {
        std::snprintf(note, sizeof(g_pin_note[0]),
                      "p%d sel=%d->%d DECLINED(flag)", i, prev_sel, target_char);
        return false;
    }
    const int loaded = (slot < 8)
        ? *(const int*)(ADDR_SLOT_LOADED_CHAR + 4 * (uintptr_t)slot) : -999;
    const bool reload = (loaded != target_char);

    // The engine's OWN else-branch condition. selected[i] == -1 is the CREATE
    // branch's state (boot character-select, or a pin that already ran) and
    // must NOT unload -- there is nothing bound to tear down.
    if (prev_sel < 0) {
        std::snprintf(note, sizeof(g_pin_note[0]),
                      "p%d sel=%d->%d slot=%d orphans=0 unloaded=0 reload=%s",
                      i, prev_sel, target_char, slot, reload ? "YES" : "no");
        return false;
    }

    // From here on this IS the arm's first actual re-select: the census runs and
    // (on the fix arm) the engine's own teardown is applied, so the one-shot is
    // spent on work that happened.
    *burn_one_shot = true;

    // The census runs on BOTH arms -- it is the measurement that names the
    // orphans, and the causality control needs it exactly as much as the fix.
    char before[112];
    const int n_before = CensusPreviewObjects(slot, before, sizeof(before));
    int n_after = n_before;
    const bool act = CssUnloadEnabled();
    if (act) {
        ((CssUnloadPlayerPreviewFn)ADDR_CSS_UNLOAD_PREVIEW)(slot);
        char after[16];
        n_after = CensusPreviewObjects(slot, after, sizeof(after));
    }
    std::snprintf(note, sizeof(g_pin_note[0]),
                  "p%d sel=%d->%d slot=%d orphans=%d unloaded=%d reload=%s [%s]",
                  i, prev_sel, target_char, slot, n_before, n_before - n_after,
                  reload ? "YES" : "no", before);
    return act;
}

// Apply team-mode dupe-lock: in team mode CSS, mask each player's confirm
// bits if their cursor would land on a character already locked into one
// of their earlier team slots. Engine writes the cursor's selected char
// to g_player_move_history[round_count*4] (P1) / g_p2_round_history_chars
// [round_state*4] (P2) at confirm time without dedup; this masks the
// confirm bits BEFORE the engine sees them so the slot doesn't advance.
void ApplyTeamDupeLock() {
    if (!g_team_dupe_lock.load(std::memory_order_relaxed)) return;
    if (*(const uint32_t*)ADDR_GAME_MODE_FLAG != 2u) return;       // team mode only
    if (*(const uint32_t*)ADDR_GAME_MODE      != 2000u) return;    // CSS phase only

    const int*       selected   = (const int*)ADDR_SELECTED_CHAR;
    uint32_t*        in_changes = (uint32_t*)ADDR_INPUT_CHANGES;
    const uint32_t*  p1_hist    = (const uint32_t*)ADDR_P1_TEAM_HISTORY;
    const uint32_t*  p2_hist    = (const uint32_t*)ADDR_P2_TEAM_HISTORY;
    const uint32_t   p1_locked  = *(const uint32_t*)ADDR_P1_TEAM_SLOT_COUNT;
    const uint32_t   p2_locked  = *(const uint32_t*)ADDR_P2_TEAM_SLOT_COUNT;

    // Cap iteration at 4 (storage size of each history array).
    const uint32_t p1_n = p1_locked > 4u ? 4u : p1_locked;
    const uint32_t p2_n = p2_locked > 4u ? 4u : p2_locked;

    for (uint32_t i = 0; i < p1_n; ++i) {
        if (p1_hist[i] == (uint32_t)selected[0]) {
            in_changes[0] &= ~0x3F0u;  // suppress confirm
            break;
        }
    }
    for (uint32_t i = 0; i < p2_n; ++i) {
        if (p2_hist[i] == (uint32_t)selected[1]) {
            in_changes[1] &= ~0x3F0u;
            break;
        }
    }
}

// Address of g_team_round_setting (live runtime team count, 0x470064).
// game_state_manager copies g_team_round → here at CSS init; we re-write
// every frame after the original to ensure our override sticks.
constexpr uintptr_t ADDR_G_TEAM_ROUND_SETTING = 0x00470064;

char __cdecl Hook_GameStateManager() {
    // OPTION-cycle title→CSS apply runs FIRST. Must fire BEFORE the
    // original game_state_manager STATE 0 body so CSS init sees the
    // final g_game_mode_flag (e.g. 1P→2P hijack writes flag=1 before
    // STATE 0 reads it to dispatch 1-cursor vs 2-cursor CSS init).
    PerGamePatches_OnGameStateManagerEntry();

    // Team-mode dupe-lock runs independently of the replay auto-confirm
    // path -- they're orthogonal features that both happen to live in the
    // same hook because MinHook only allows one detour per function.
    ApplyTeamDupeLock();

    // Team size override -- apply BEFORE original so the natural copy
    // from g_team_round at game_state_manager STATE 0 (init CSS) sees
    // our value via g_team_round (we also re-write g_team_round_setting
    // post-call below for paths that don't re-read from g_team_round).
    {
        const int override = PerGamePatches_GetTeamSizeOverride();
        if (override >= 2 && override <= 8) {
            // Write to g_team_round (the INI source the engine copies from).
            DWORD old_protect;
            if (VirtualProtect((void*)0x00430128, 4, PAGE_READWRITE, &old_protect)) {
                *(uint32_t*)0x00430128 = (uint32_t)override;
                VirtualProtect((void*)0x00430128, 4, old_protect, &old_protect);
            }
        }
    }

    // Seam hold: while waiting for the next MATCH_START, keep the CSS
    // unadvanceable -- zero both action_states (the rematch flow carries
    // the previous match's locks, which auto-advance to battle on neutral
    // inputs) and mask all confirm bits. The pin (g_active) takes
    // precedence: once armed it drives the natural confirm flow itself.
    if (g_seam_hold.load(std::memory_order_relaxed) &&
        !g_active.load(std::memory_order_relaxed)) {
        const uint32_t game_mode = *(uint32_t*)ADDR_GAME_MODE;
        if (game_mode == 2000u) {
            *(uint32_t*)ADDR_P1_ACTION_STATE = 0;
            *(uint32_t*)ADDR_P2_ACTION_STATE = 0;
            uint32_t* in_changes = (uint32_t*)ADDR_INPUT_CHANGES;
            in_changes[0] &= ~0x3F0u;
            in_changes[1] &= ~0x3F0u;
            // OPTIONAL carry-color stamping (0xFF = don't touch). Only
            // the legacy long-hold display path uses it. The lean
            // 10-frame masks must NOT write these: AssignPlayerColor's
            // collision scan reads the slot color fields, so stamped
            // values diverge the bump logic from the host's and the
            // mirrored confirms land on different palettes (rematch
            // wrong-colors-on-both, 2026-06-11).
            const uint8_t hc1 = g_seam_hold_p1_color.load(std::memory_order_relaxed);
            const uint8_t hc2 = g_seam_hold_p2_color.load(std::memory_order_relaxed);
            if (hc1 < 8) {
                *(int32_t*)(ADDR_CHARSLOT0_COLOR_PICK + 0 * CHARSLOT_STRIDE_BYTES) = hc1;
            }
            if (hc2 < 8) {
                *(int32_t*)(ADDR_CHARSLOT0_COLOR_PICK + 1 * CHARSLOT_STRIDE_BYTES) = hc2;
            }

            // [SEAM-CSS] 1Hz: is the mirror actually moving this CSS?
            // Pairs with [HOST-CSS] below for a side-by-side diff.
            static uint64_t s_seam_css_log_ms = 0;
            const uint64_t now_ms = GetTickCount64();
            if (now_ms - s_seam_css_log_ms >= 1000) {
                s_seam_css_log_ms = now_ms;
                const int* p1c = (const int*)ADDR_P1_CURSOR_POS;
                const int* p2c = (const int*)ADDR_P2_CURSOR_POS;
                const int* sel = (const int*)ADDR_SELECTED_CHAR;
                const uint32_t* proc = (const uint32_t*)ADDR_PROCESSED_INPUT;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SEAM-CSS] p1@(%d,%d) p2@(%d,%d) sel=%d/%d "
                    "in=0x%03X/0x%03X timer=%u demo=%u",
                    p1c[0], p1c[1], p2c[0], p2c[1], sel[0], sel[1],
                    proc[0], proc[1],
                    *(uint32_t*)0x424F00u, *(uint32_t*)0x47010Cu);
            }
        }
    }

    // [HOST-CSS] 1Hz mirror reference: any non-spectator instance at CSS
    // logs the same fields so a seam-mirror diff is one grep away.
    {
        static int s_is_spec2 = -1;
        if (s_is_spec2 < 0) {
            const char* v = std::getenv("FM2K_SPECTATOR_MODE");
            s_is_spec2 = (v && v[0] == '1') ? 1 : 0;
        }
        if (s_is_spec2 == 0 &&
            *(uint32_t*)ADDR_GAME_MODE == 2000u) {
            static uint64_t s_host_css_log_ms = 0;
            const uint64_t now_ms = GetTickCount64();
            if (now_ms - s_host_css_log_ms >= 1000) {
                s_host_css_log_ms = now_ms;
                const int* p1c = (const int*)ADDR_P1_CURSOR_POS;
                const int* p2c = (const int*)ADDR_P2_CURSOR_POS;
                const int* sel = (const int*)ADDR_SELECTED_CHAR;
                const uint32_t* proc = (const uint32_t*)ADDR_PROCESSED_INPUT;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[HOST-CSS] p1@(%d,%d) p2@(%d,%d) sel=%d/%d "
                    "in=0x%03X/0x%03X act=%u/%u timer=%u demo=%u",
                    p1c[0], p1c[1], p2c[0], p2c[1], sel[0], sel[1],
                    proc[0], proc[1],
                    *(uint32_t*)ADDR_P1_ACTION_STATE,
                    *(uint32_t*)ADDR_P2_ACTION_STATE,
                    *(uint32_t*)0x424F00u, *(uint32_t*)0x47010Cu);
            }
        }
    }

    // FM2K_CSS_TRACE: log the CSS grid (selected chars) on change, on EVERY peer
    // (host + spectator), to compare rematch-CSS char alignment. Diagnostic only.
    {
        static int s_css_trace = []{
            const char* v = std::getenv("FM2K_CSS_TRACE");
            return (v && v[0]) ? 1 : 0;
        }();
        if (s_css_trace) {
            const uint32_t gm = *(uint32_t*)ADDR_GAME_MODE;
            const int s0 = *(int*)ADDR_SELECTED_CHAR;
            const int s1 = *((int*)ADDR_SELECTED_CHAR + 1);
            static uint32_t lgm = 0xFFFFFFFFu; static int ls0 = -99, ls1 = -99;
            if (gm != lgm || s0 != ls0 || s1 != ls1) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "CSS-TRACE: mode=%u sel0=%d sel1=%d active=%d",
                    gm, s0, s1, (int)g_active.load(std::memory_order_relaxed));
                lgm = gm; ls0 = s0; ls1 = s1;
            }
        }
    }

    if (g_active.load(std::memory_order_relaxed)) {
        const uint32_t game_mode = *(uint32_t*)ADDR_GAME_MODE;
        if (game_mode == 2000u) {
            const uint8_t p1c = g_target_p1_char.load(std::memory_order_relaxed);
            const uint8_t p2c = g_target_p2_char.load(std::memory_order_relaxed);
            const uint8_t stg = g_target_stage_id.load(std::memory_order_relaxed);
            const uint16_t sw = *(uint16_t*)ADDR_STAGE_WIDTH;

            if (sw > 0) {
                int*      p1_cur     = (int*)ADDR_P1_CURSOR_POS;
                int*      p2_cur     = (int*)ADDR_P2_CURSOR_POS;
                int*      selected   = (int*)ADDR_SELECTED_CHAR;
                uint32_t* in_changes = (uint32_t*)ADDR_INPUT_CHANGES;
                uint32_t* processed  = (uint32_t*)ADDR_PROCESSED_INPUT;
                const uint32_t p1_act = *(uint32_t*)ADDR_P1_ACTION_STATE;
                const uint32_t p2_act = *(uint32_t*)ADDR_P2_ACTION_STATE;

                // Pin stage. The natural battle-init read site is 0x43010C;
                // writing here ensures recorded stage matches when type-14
                // battle-init spawns post-CSS-timer.
                *(uint32_t*)ADDR_SELECTED_STAGE = stg;

                // Mask out d-pad bits in g_processed_input -- pb_queue's
                // leaked LRUD bits would otherwise drift the cursor away
                // from our pin via WrapPositionToStageBounds before
                // game_state_manager computes grid_pos.
                processed[0] &= ~0xFu;
                processed[1] &= ~0xFu;

                // Pin cursor positions.
                p1_cur[0] = static_cast<int>(p1c % sw);
                p1_cur[1] = static_cast<int>(p1c / sw);
                p2_cur[0] = static_cast<int>(p2c % sw);
                p2_cur[1] = static_cast<int>(p2c / sw);

                // Two-phase per-player drive:
                //
                //  Phase A (selected != target): force selected = -1 so the
                //  natural state-1 d-pad branch sees prev_pos==-1 + new grid
                //  pos == target_idx → triggers player_data_file_loader to
                //  load the right .player file. We MUST NOT inject confirm
                //  here -- mode-1's CSS code is if/else: confirm path AND
                //  d-pad/file-load path are mutually exclusive per frame,
                //  and the confirm path is checked first. Without this,
                //  state-0 init's stale selected[player]=0 (cursor was at
                //  (0,0)) would lock player to char 0 before we ever loaded
                //  the right file.
                //
                //  Phase B (selected == target): file already loaded by
                //  Phase A; inject confirm bit, game's confirm branch fires,
                //  action_state locks. Stop pinning on next frame
                //  (action_state==1 means d-pad/confirm code is skipped).
                //
                // pb_queue's natural attack-button bits are ALSO masked out
                // during Phase A so they can't accidentally trigger an early
                // confirm with the wrong selected value.
                const uint8_t p1col = g_target_p1_color.load(std::memory_order_relaxed);
                const uint8_t p2col = g_target_p2_color.load(std::memory_order_relaxed);
                const uint32_t p1_bit = ColorBitForSlot(p1col);
                const uint32_t p2_bit = ColorBitForSlot(p2col);

                // ORPHANED-PREVIEW FIX, with the engine's own TIMING.
                //
                // The teardown runs in the detour, strictly before g_orig().
                // The `selected[i] = -1` write is then DEFERRED TO THE ENGINE
                // for that one frame, and this is the load-bearing half.
                //
                // Css_UnloadPlayerPreview only MARKS the outgoing previews
                // type 1; Obj_ReapMarkedSlot frees them (type 0) on the next
                // update_game_state pass. The engine's own transition is
                // therefore two-pass: unload + `selected = -1` on frame N,
                // create on frame N+1, with the reap in between -- so the new
                // preview set is allocated into the SAME low slots the old one
                // occupied. Writing the -1 ourselves compresses that into one
                // frame: g_orig() then sees selected == -1 immediately, and
                // create_game_object's first-free scan has to skip the
                // still-marked slots, so the new preview set lands HIGHER than
                // the host's and in a different intra-set order. Measured on
                // kensei2023: the character portrait moved from slot 0 to slot
                // 10/11 while its y=980 children took slots 3-5, which flipped
                // which member FindPlayerObjectSlot resolves and reddened the
                // CSS-WIN FALL term with no object having moved.
                //
                // The engine writes the -1 itself in the same g_orig() call:
                // the cursor is pinned to the target above, so its grid ==
                // target != selected[i] and != -1, which is exactly the
                // `else { Css_UnloadPlayerPreview(); selected = -1; }` branch
                // (0x407425 / 0x407967 / 0x407C2A). Its unload call is a no-op
                // second application over ours (type == 4 no longer matches).
                //
                // FALLBACK, so a flavour whose cursor we do not drive can never
                // hang the pin: if the engine did NOT clear it (we already
                // unloaded once for this arm and selected[i] is still neither
                // the target nor -1), write the -1 directly next tick, which is
                // the pre-fix behaviour.
                if (p1_act == 0u) {
                    if (selected[0] != static_cast<int>(p1c)) {
                        bool engine_writes = false;
                        if (!g_pin_unloaded[0]) {
                            bool burn = false;
                            engine_writes = PinUnloadOutgoingPreview(
                                0, static_cast<int>(p1c), selected[0], &burn);
                            if (burn) {
                                g_pin_unloaded[0] = true;
                                g_pin_line_pending = true;
                            }
                        }
                        if (!engine_writes) selected[0] = -1;
                        in_changes[0] &= ~0x3F0u;
                    } else {
                        in_changes[0] = (in_changes[0] & ~0x3F0u) | p1_bit;
                    }
                }
                if (p2_act == 0u) {
                    if (selected[1] != static_cast<int>(p2c)) {
                        bool engine_writes = false;
                        if (!g_pin_unloaded[1]) {
                            bool burn = false;
                            engine_writes = PinUnloadOutgoingPreview(
                                1, static_cast<int>(p2c), selected[1], &burn);
                            if (burn) {
                                g_pin_unloaded[1] = true;
                                g_pin_line_pending = true;
                            }
                        }
                        if (!engine_writes) selected[1] = -1;
                        in_changes[1] &= ~0x3F0u;
                    } else {
                        in_changes[1] = (in_changes[1] & ~0x3F0u) | p2_bit;
                    }
                }

                // [CSSPIN] -- ALWAYS ON, one line per pin ARM. This is the
                // measurement that says whether the orphan class is back:
                // `orphans` counted with the ENGINE's own preview predicate
                // BEFORE the unload, `unloaded` how many the call marked,
                // `reload` whether the char slot's blob is about to be swapped
                // (the stale-blob precondition). orphans>0 with unloaded=0 is
                // the regression alarm. The bracketed list is slot/script/item/y
                // per orphan -- the slot identity that correlates against
                // [CSS-ANIM]'s per-slot born/adv/y records offline.
                //
                // The line is armed by the ARM and RE-armed by any actual
                // re-select teardown (the `burn` sites above), so a teardown
                // that happens LATER than the arm's first in-block frame still
                // gets a line. Without the re-arm the one-shot could print
                // "no-reselect" and then go silent over the teardown that
                // followed, which would make the harness's fatal
                // `orphans>0 unloaded=0` term blind to exactly the frames it
                // exists to judge.
                if (g_pin_line_pending) {
                    g_pin_line_pending = false;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[CSSPIN] arm flag=%u unload=%d | %s | %s",
                        *(const uint32_t*)ADDR_GAME_MODE_FLAG,
                        CssUnloadEnabled() ? 1 : 0,
                        g_pin_note[0], g_pin_note[1]);
                }

                const uint32_t tick = g_pin_tick.fetch_add(1, std::memory_order_relaxed);
                if (tick < 8u || (tick % 30u) == 0u) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "CssAutoConfirm: tick=%u p1=%u/c%u@(%d,%d)[sel=%d,bit=0x%X] "
                        "p2=%u/c%u@(%d,%d)[sel=%d,bit=0x%X] act=%u/%u stage=%u sw=%u",
                        tick,
                        p1c, p1col, p1_cur[0], p1_cur[1], selected[0], p1_bit,
                        p2c, p2col, p2_cur[0], p2_cur[1], selected[1], p2_bit,
                        p1_act, p2_act, stg, sw);
                }
            }
        } else if (game_mode >= 3000u) {
            // Battle entered -- disengage automatically.
            CssAutoConfirm_Disengage();
        }
    }

    char ret = g_orig ? g_orig() : 0;

    // Team size override -- second pass, AFTER the engine ran. The engine
    // copies g_team_round → g_team_round_setting at CSS init (state 0);
    // we re-stamp g_team_round_setting directly so any subsequent game
    // logic reading it during this frame sees our override even on
    // frames that don't run state 0 (most frames).
    {
        const int override = PerGamePatches_GetTeamSizeOverride();
        if (override >= 2 && override <= 8) {
            *(uint32_t*)ADDR_G_TEAM_ROUND_SETTING = (uint32_t)override;
        }
    }

    return ret;
}

}  // namespace

bool CssAutoConfirm_Install() {
    if (MH_CreateHook((void*)ADDR_GAME_STATE_MANAGER,
                      (void*)Hook_GameStateManager,
                      (void**)&g_orig) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CssAutoConfirm: MH_CreateHook(game_state_manager @ 0x%08X) failed",
            (unsigned)ADDR_GAME_STATE_MANAGER);
        return false;
    }
    // Queue only -- InitializeHooks flushes all queued hooks with a single
    // MH_ApplyQueued (one thread-freeze for the whole boot path).
    if (MH_QueueEnableHook((void*)ADDR_GAME_STATE_MANAGER) != MH_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CssAutoConfirm: MH_QueueEnableHook(game_state_manager) failed");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CssAutoConfirm: queued game_state_manager @ 0x%08X (idle until "
        "OnReplayMatchStart fires)",
        (unsigned)ADDR_GAME_STATE_MANAGER);
    return true;
}

void CssAutoConfirm_OnReplayMatchStart(uint8_t p1_char, uint8_t p1_color,
                                       uint8_t p2_char, uint8_t p2_color,
                                       uint8_t stage_id) {
    g_target_p1_char.store(p1_char, std::memory_order_relaxed);
    g_target_p2_char.store(p2_char, std::memory_order_relaxed);
    g_target_p1_color.store(p1_color, std::memory_order_relaxed);
    g_target_p2_color.store(p2_color, std::memory_order_relaxed);
    g_target_stage_id.store(stage_id, std::memory_order_relaxed);
    g_pin_tick.store(0, std::memory_order_relaxed);
    // Arm the one-shot [CSSPIN] census + the paired preview unload for this arm.
    // Defaults describe the "pin had nothing to re-select" case, which is what a
    // player whose selection already equals the target legitimately produces.
    std::snprintf(g_pin_note[0], sizeof(g_pin_note[0]), "p0 no-reselect");
    std::snprintf(g_pin_note[1], sizeof(g_pin_note[1]), "p1 no-reselect");
    g_pin_unloaded[0] = g_pin_unloaded[1] = false;
    g_pin_line_pending = true;
    g_active.store(true, std::memory_order_release);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CssAutoConfirm: armed for replay -- p1=%u/c%u p2=%u/c%u stage=%u",
        p1_char, p1_color, p2_char, p2_color, stage_id);
}

void CssAutoConfirm_Disengage() {
    if (g_active.exchange(false, std::memory_order_acq_rel)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CssAutoConfirm: disengaged (battle entered or match ended)");
    }
}

void CssAutoConfirm_SetTeamDupeLock(bool enabled) {
    g_team_dupe_lock.store(enabled, std::memory_order_relaxed);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CssAutoConfirm: team-mode dupe-lock %s",
        enabled ? "ENABLED" : "disabled");
}

void CssAutoConfirm_SetSeamHold(bool enabled,
                                uint8_t p1_color, uint8_t p2_color) {
    g_seam_hold_p1_color.store(p1_color, std::memory_order_relaxed);
    g_seam_hold_p2_color.store(p2_color, std::memory_order_relaxed);
    const bool was = g_seam_hold.exchange(enabled, std::memory_order_acq_rel);
    if (was != enabled) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "CssAutoConfirm: seam hold %s (carry colors p1=c%u p2=c%u)",
            enabled ? "ENGAGED" : "released", p1_color, p2_color);
    }
}

#else  // ENGINE_FM95 -- separate state machine, separate hand-off

#include "css_autoconfirm.h"

bool CssAutoConfirm_Install()                              { return true; }
void CssAutoConfirm_OnReplayMatchStart(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) {}
void CssAutoConfirm_Disengage()                            {}
void CssAutoConfirm_SetTeamDupeLock(bool)                  {}
void CssAutoConfirm_SetSeamHold(bool, uint8_t, uint8_t)    {}

#endif
