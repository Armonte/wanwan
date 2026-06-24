// CSS auto-lock-and-confirm hook — see header for design rationale and
// docs/analysis/css_state_machine.md for the IDA-pass that maps the state
// machine and globals.

#if !defined(ENGINE_FM95)

#include "css_autoconfirm.h"

#include <SDL3/SDL_log.h>
#include <atomic>
#include <cstdint>
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
constexpr uintptr_t ADDR_P1_TEAM_SLOT_COUNT = 0x004700EC;  // g_p1_round_count — # P1 chars locked so far
constexpr uintptr_t ADDR_P2_TEAM_SLOT_COUNT = 0x004700F0;  // g_p1_round_state — # P2 chars locked so far (P2 counter, despite the name)

// AssignPlayerColor @ 0x406F20 reads `input_changes & 0x3E0` (bits 5..9) to
// pick color slot 1..5; if none of those bits set, the function falls
// through to color 0. The caller in game_state_manager triggers the call
// only when `input_changes & 0x3F0` (bits 4..9) is non-zero — so bit 4
// (0x10) on its own confirms with color 0.
//
// Earlier versions of this file used a fixed `INJECTED_CONFIRM_BIT = 0x10`
// for both players, which always landed on color 0 regardless of what the
// recorded match used. That made offline replays look like every fight
// confirmed with palette 0 even when the original players picked colors
// 1..5 — visible as wrong palette in the replay's CSS portrait + battle
// sprites.
//
// Replacement: per-player target bit derived from the MATCH_START header's
// p1_color / p2_color, mapped through ColorBitForSlot:
//
//   color 0 → 0x10  (bit 4 — confirm-only, falls through to default 0)
//   color 1 → 0x20  (bit 5)
//   color 2 → 0x40  (bit 6)
//   color 3 → 0x80  (bit 7)
//   color 4 → 0x100 (bit 8)
//   color 5 → 0x200 (bit 9)
//
// We still OVERWRITE the 0x3F0 bits rather than OR — pb_queue's leaked
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

// Tick counter for diag — log first few frames of pinning then stay quiet so
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
    // path — they're orthogonal features that both happen to live in the
    // same hook because MinHook only allows one detour per function.
    ApplyTeamDupeLock();

    // Team size override — apply BEFORE original so the natural copy
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

                // Mask out d-pad bits in g_processed_input — pb_queue's
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
                //  here — mode-1's CSS code is if/else: confirm path AND
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

                if (p1_act == 0u) {
                    if (selected[0] != static_cast<int>(p1c)) {
                        selected[0] = -1;
                        in_changes[0] &= ~0x3F0u;
                    } else {
                        in_changes[0] = (in_changes[0] & ~0x3F0u) | p1_bit;
                    }
                }
                if (p2_act == 0u) {
                    if (selected[1] != static_cast<int>(p2c)) {
                        selected[1] = -1;
                        in_changes[1] &= ~0x3F0u;
                    } else {
                        in_changes[1] = (in_changes[1] & ~0x3F0u) | p2_bit;
                    }
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
            // Battle entered — disengage automatically.
            CssAutoConfirm_Disengage();
        }
    }

    char ret = g_orig ? g_orig() : 0;

    // Team size override — second pass, AFTER the engine ran. The engine
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
    // Queue only — InitializeHooks flushes all queued hooks with a single
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
    g_active.store(true, std::memory_order_release);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "CssAutoConfirm: armed for replay — p1=%u/c%u p2=%u/c%u stage=%u",
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

#else  // ENGINE_FM95 — separate state machine, separate hand-off

#include "css_autoconfirm.h"

bool CssAutoConfirm_Install()                              { return true; }
void CssAutoConfirm_OnReplayMatchStart(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) {}
void CssAutoConfirm_Disengage()                            {}
void CssAutoConfirm_SetTeamDupeLock(bool)                  {}
void CssAutoConfirm_SetSeamHold(bool, uint8_t, uint8_t)    {}

#endif
