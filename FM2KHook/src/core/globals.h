#ifndef GLOBALS_H
#define GLOBALS_H

#include <windows.h>
#include <cstdint>

// ============================================================================
// FUNCTION TYPEDEFS - for hooking original game functions
// ============================================================================
typedef int(__cdecl* GetPlayerInputFunc)(int, int);
typedef int(__cdecl* GetPlayerInputFM95Func)(int);  // FM95 split: p1/p2 are separate single-arg fns
typedef int(__cdecl* UpdateGameStateFunc)();
typedef BOOL(__cdecl* RunGameLoopFunc)();
typedef void(__cdecl* RenderGameFunc)();
typedef uint32_t(__cdecl* GameRandFunc)();
typedef int(__cdecl* ProcessGameInputsFunc)();
// Render sub-profiler leaf (BlitSpriteWithBlendMode @ 0x40C140) — the per-sprite
// software pixel blit. Hooked only when FM2K_RENDER_PROFILE=1, to rank heavy-
// stage render cost. Display-only; a6=width, a7=height, *(obj+336)=blend mode.
typedef int(__cdecl* BlitSpriteFunc)(int, int, int, int, int, int, int, short);
// sprite_rendering_engine @ 0x40CC30 -- hooked under FM2K_BLIT_SIMD to
// intercept the case -10 full-screen blur (render_mode obj[+0x10] == -10).
typedef void(__cdecl* SpriteRenderEngineFunc)(int);

// Original function pointers (set by MinHook)
extern GetPlayerInputFunc original_get_player_input;
extern GetPlayerInputFM95Func original_get_player_input_p1;  // FM95 only — 0x408AE0
extern GetPlayerInputFM95Func original_get_player_input_p2;  // FM95 only — 0x408D60
extern UpdateGameStateFunc original_update_game;
extern RunGameLoopFunc original_run_game_loop;
extern RenderGameFunc original_render_game;
extern GameRandFunc original_game_rand;
extern ProcessGameInputsFunc original_process_game_inputs;
extern BlitSpriteFunc original_blit_sprite;  // set only under FM2K_RENDER_PROFILE / FM2K_BLIT_SIMD
extern SpriteRenderEngineFunc original_sprite_render_engine;  // set only under FM2K_BLIT_SIMD

// Render RNG stream isolation (rollback/cross-peer determinism).
// game_rand() is a single shared LCG used by BOTH sim and render. Render runs
// at per-peer-variable cadence (frame-skip/runahead/rollback), so its
// game_rand consumption can't be made cross-peer-deterministic on the shared
// seed. Fix: while g_in_render_rng is set (around render_game), Hook_GameRand
// draws from g_render_rng_seed instead of the gameplay seed, leaving the
// gameplay rng (g_rand_seed @ 0x41FB1C) untouched by render. g_render_rng_seed
// is re-seeded from the gameplay seed at the start of each render, so render
// colors are deterministic per confirmed frame AND identical across peers.
extern uint32_t g_render_rng_seed;
extern bool     g_in_render_rng;
extern uint32_t g_render_rand_calls;  // #63 diag: render-side game_rand calls/frame
extern uint32_t g_gameplay_rand_calls;  // spectate-desync hunt: gameplay-seed game_rand calls/frame
extern uint32_t g_gp_rand_by_fn[8];     // spectate-desync hunt: gameplay-rand by caller fn

// Render sub-profiler accumulators (FM2K_RENDER_PROFILE=1). Cumulative census
// of the per-sprite blit, filled by Hook_BlitSpriteWithBlendMode during
// render_game; RenderFrameWithSnapshot snapshots deltas every 300 frames and
// logs render_game-total vs blit-time vs residual(blur+rle+lut+tail), plus the
// blend-mode mix and blit-calls-per-object (afterimage multiplier). Display-
// only — these never feed sim/rollback state, so the renderer is free to
// optimize without desync risk.
extern volatile uint32_t g_rp_blit_calls;    // total blit calls
extern volatile uint64_t g_rp_blit_ns;       // total time inside the blit leaf
extern volatile uint64_t g_rp_blit_area;     // total nominal width*height blitted
extern volatile uint32_t g_rp_blit_mode[5];  // calls per blend mode (0=copy..4=alpha)

// Sim throughput counter: incremented once per game logic tick (update_game),
// across every path (offline frame-skip loop, netplay battle advance, CSS,
// native). Sampled in Hook_RenderDiagnostics_Tick to derive sim-fps separately
// from render-fps, so the title bar can show "sim N / rdr M" -- sim must hold
// 100 even when render drops on heavy stages. Monotonic; wraps harmlessly.
extern volatile uint32_t g_sim_step_count;

// ============================================================================
// ENGINE TARGET — the same source compiles into:
//   FM2KHook.dll : default build, addresses target Fighter Maker 2nd binaries
//                  (e.g. WonderfulWorld_ver_0946.exe — base 0x400000)
//   FM95Hook.dll : -DENGINE_FM95=1 build, addresses target Fighter Maker 95
//                  prototype binaries (e.g. CPW.exe — base 0x400000)
//
// Both DLLs export the same hook surface; only the address values change.
// The launcher picks which to inject based on FM2K::FM2KGameInfo::engine.
//
// FM95 addresses are sourced from /mnt/c/dev/wanwan/FM95_Integration.h (verified
// against the live CPW IDB at C:\dev\fm95\CPW\ＣＰＷ.exe.i64). Address values
// without a direct FM95 equivalent are set to 0 and must be guarded with
// `#if !defined(ENGINE_FM95)` at consumer sites — the build will surface them.
// ============================================================================
#if defined(ENGINE_FM95)
namespace FM2K {
    // Function addresses (FM95 / CPW.exe — verified 2026-05-05)
    // FM95 has SEPARATE per-player input fns; the trampoline assumes a single
    // GET_PLAYER_INPUT — leave the P1 entry as the canonical hook target and
    // expose P2 separately for hooks that need it.
    constexpr uintptr_t ADDR_GET_PLAYER_INPUT = 0x408AE0;   // FM95: get_player_input_p1
    constexpr uintptr_t ADDR_GET_PLAYER_INPUT_P2 = 0x408D60; // FM95-only second target
    constexpr uintptr_t ADDR_UPDATE_GAME = 0x40A060;
    constexpr uintptr_t ADDR_RUN_GAME_LOOP = 0x40AB60;       // FM95: _WinMain@16 (frame loop is inlined)
    constexpr uintptr_t ADDR_RENDER_GAME = 0x40A910;
    constexpr uintptr_t ADDR_BLIT_SPRITE = 0;                // render profiler is FM2K-only; unused on FM95 (guarded by kIsFM2K)
    constexpr uintptr_t ADDR_SPRITE_RENDER_ENGINE = 0;       // blur intercept is FM2K-only; unused on FM95
    constexpr uintptr_t ADDR_GAME_RAND = 0x41A864;           // FM95: CRT _rand
    constexpr uintptr_t ADDR_PROCESS_INPUTS = 0x408FF0;
    constexpr uintptr_t ADDR_DISPATCH_SCRIPT_SOUND = 0x401FF0;

    // Sound-system entry points (rollback sound layer). IDA-verified on CPW:
    // PlaySoundFromBufferArray @ 0x401590, StopAllSoundsInBufferArray @
    // 0x4016A0, ControlSoundSystem @ 0x4020A0 (0=stop all, 2=stop MIDI,
    // 3=stop CD -- same cmd contract as FM2K's ControlSound @ 0x4034D0).
    constexpr uintptr_t ADDR_PLAY_SOUND_FROM_BUFFER_ARRAY    = 0x401590;
    constexpr uintptr_t ADDR_STOP_ALL_SOUNDS_IN_BUFFER_ARRAY = 0x4016A0;
    constexpr uintptr_t ADDR_CONTROL_SOUND                   = 0x4020A0;
    // Sound channel registry -- the FM95 equivalent of FM2K's
    // g_sound_channel_table is NOT yet RE'd (RE-1 in
    // docs/FM95_Support_Status.md). 0 sentinels keep the SFX desired/actual
    // layer inert-but-safe (Init/scan paths no-op); the BGM desired path has
    // no table dependency and stays live.
    constexpr uintptr_t ADDR_SOUND_CHANNEL_TABLE     = 0;
    constexpr uintptr_t ADDR_SOUND_CHANNEL_TABLE_END = 0;

    // Game state addresses
    constexpr uintptr_t ADDR_FRAME_COUNTER = 0x4DD7A8;       // FM95: g_game_tick_counter
    constexpr uintptr_t ADDR_GAME_MODE = 0x425558;           // values 0/1/10/0x10 — see FM95_Integration.h delta #2
    constexpr uintptr_t ADDR_RANDOM_SEED = 0x4243FC;

    // Player current-frame inputs (post-poll cache)
    constexpr uintptr_t ADDR_P1_INPUT = 0x437750;
    constexpr uintptr_t ADDR_P2_INPUT = 0x437754;

    // Per-player HP — FM95 has no global HP scalar; the value lives at
    // offset +72 of each player's main-object slot in the pool. The
    // pointers to those slots are kept in the array below, indexed by
    // player_idx (0/1). HP read pattern (FM95-specific):
    //   void* main = *(void**)(ADDR_P_MAIN_OBJECT_PTR + player_idx*4);
    //   uint32_t hp = *(uint32_t*)((uint8_t*)main + 72);
    // Static globals stay 0 to force consumer sites to branch on
    // kIsFM95 and use the indirection.
    constexpr uintptr_t ADDR_P1_HP = 0;
    constexpr uintptr_t ADDR_P2_HP = 0;
    // g_p_main_object_ptr — array of pointers to each player's main
    // object in g_object_pool. IDA-verified in CPW @ 0x430e84
    // (referenced by process_combo_input, character_state_machine).
    // Index 0 = P1, 1 = P2. Per-player HP read pattern:
    //   void* main = *(void**)(ADDR_P_MAIN_OBJECT_PTR + pid*4);
    //   uint32_t hp = *(uint32_t*)((uint8_t*)main + 72);
    constexpr uintptr_t ADDR_P_MAIN_OBJECT_PTR = 0x430e84;

    // Round-win counters — IDA-verified canonical fields used by
    // obj_match_result_state @ 0x410db0 case 4 to decide the match
    // winner. Per-player struct stride is 100 bytes (25 dwords);
    // g_p1_win_counter[25*1] == g_p2_win_counter. These are the
    // safest fields to compare in Netplay_EndBattle (no pointer
    // indirection, monotonic across rounds, set to 0 only at the
    // start of a new match in vs_round_function case 1).
    constexpr uintptr_t ADDR_P1_WIN_COUNTER = 0x5e9914;
    constexpr uintptr_t ADDR_P2_WIN_COUNTER = 0x5e9978;

    // FM2K-only fields (stubbed at 0 on FM95). Mirror of the symmetric
    // stubs in the FM2K branch — keeps `if constexpr (kIsFM2K) { ... }`
    // bodies parseable in the FM95 build.
    constexpr uintptr_t ADDR_P1_ROUNDS_WON  = 0;
    constexpr uintptr_t ADDR_P2_ROUNDS_WON  = 0;

    // Damage-taken / HP-loss scalars — IDA shows
    // vs_round_function case 30 spawning the KO panel when
    // g_p1_damage_taken[0] == g_char_max_hp_table[0]. Same 100-byte
    // stride as win_counter; useful for in-match HP queries (e.g.
    // titlebar live HP%) without going through the main_object_ptr
    // indirection. (IDA's auto-name "g_p1_total_wins" is misleading —
    // these aren't win counts; they're cumulative damage taken.)
    constexpr uintptr_t ADDR_P1_DAMAGE_TAKEN = 0x5e9910;
    constexpr uintptr_t ADDR_P2_DAMAGE_TAKEN = 0x5e9974;

    // CSS state — FM95 layout differs (cursor is single int, not x/y pair).
    // Treat ADDR_P*_CURSOR_POS as the cursor index slot. ACTION_STATE maps to
    // the per-player 'confirmed' flag at +0x0C of the per-player CSS struct.
    constexpr uintptr_t ADDR_P1_CURSOR_POS = 0x432720;       // FM95: g_p1_css_char_cursor (int32)
    constexpr uintptr_t ADDR_P2_CURSOR_POS = 0x432730;       // FM95: g_p2_css_char_cursor
    constexpr uintptr_t ADDR_P1_ACTION_STATE = 0x43272C;     // FM95: g_p1_css_confirmed
    constexpr uintptr_t ADDR_P2_ACTION_STATE = 0x43273C;     // FM95: g_p2_css_confirmed

    // No direct FM95 analog — FM2K's "frames since both locked" is replaced by
    // walking the type=19 object's sub_state in [0x69, 0x6F] range.
    constexpr uintptr_t ADDR_ROUND_TIMER_COUNTER = 0;

    // Internal game timer — same address as frame counter on FM95 (single counter).
    constexpr uintptr_t ADDR_GAME_TIMER = 0x4DD7A8;

    // No global CSS_ACTIVE_PLAYER on FM95; phase classification walks the pool.
    constexpr uintptr_t ADDR_CSS_ACTIVE_PLAYER = 0;
    constexpr uintptr_t ADDR_PLAYER_STAGE_POSITIONS = 0;

    // g_frame_time_ms -- ms per sim step, read by the engine's frame-skip
    // math. FM95_Integration.h ADDR_FRAME_TIME_MS.
    constexpr uintptr_t ADDR_FRAME_TIME_MS = 0x422F7C;

    // Character data — FM95 has 5 slots × 229844 bytes at 0x509100.
    // Static (don't save during rollback).
    constexpr uintptr_t ADDR_CHAR_SLOTS       = 0x509100;
    constexpr size_t    CHAR_SLOT_TOTAL_SIZE  = 229844;
    constexpr size_t    CHAR_SLOT_COUNT       = 5;

    // Object pool — 256 × 0xA4 layout, base 0x426A40.
    constexpr uintptr_t ADDR_OBJECT_POOL      = 0x426A40;
    constexpr size_t    OBJECT_POOL_STRIDE    = 0xA4;        // 164 bytes/slot
    constexpr size_t    OBJECT_POOL_COUNT     = 256;
    constexpr size_t    SIZE_OBJECT_POOL      = OBJECT_POOL_COUNT * OBJECT_POOL_STRIDE;  // 0xA400

    // Input ring — 256 entries × 4 bytes (8-bit modulo idx).
    constexpr uintptr_t ADDR_INPUT_BUFFER_INDEX = 0x437700;
    constexpr uintptr_t ADDR_P1_INPUT_HISTORY   = 0x431720;
    constexpr uintptr_t ADDR_P2_INPUT_HISTORY   = 0x431B20;
    constexpr size_t    INPUT_HISTORY_LEN       = 256;
    constexpr uint32_t  INPUT_HISTORY_MASK      = INPUT_HISTORY_LEN - 1;  // 0xFF

    // CSS — selected character IDs (cursor index at confirm time on FM95).
    constexpr uintptr_t ADDR_P1_SELECTED_CHAR     = 0x432720;
    constexpr uintptr_t ADDR_P2_SELECTED_CHAR     = 0x432730;
    // Character filename table — 50 slots × 256-byte CP932 strings.
    constexpr uintptr_t ADDR_CHAR_FILENAME_TABLE  = 0x463CF0;
    constexpr size_t    CHAR_FILENAME_STRIDE      = 256;
    constexpr size_t    CHAR_FILENAME_COUNT       = 50;

    // Stage selection on FM95 splits two ways:
    //   - Practice mode: vs_round_function reads g_practice_stage_id
    //     (uint8) at 0x43274c — this IS the random-stage write target
    //     that mirrors FM2K's 0x43010c.
    //   - VS / Story mode: stage is character-driven, read from
    //     g_char_stage_per_round[char][round] at 0x540337. There's no
    //     single "selected stage" scalar; to override in vs-mode the
    //     hook would need a function-prologue trampoline on
    //     LoadStageFile_alt @ 0x4054b0 to rewrite its first arg.
    //     Deferred — random-stage on FM95 currently only takes effect
    //     in practice mode.
    constexpr uintptr_t ADDR_SELECTED_STAGE       = 0x43274c;
    constexpr uintptr_t ADDR_LOAD_STAGE_FILE_ALT  = 0x4054b0;
    // FM2K-only seam (LoadStageFile + the two globals the dispatcher reads
    // around it). FM95 overrides via LoadStageFile_alt's arg0 instead.
    constexpr uintptr_t ADDR_LOAD_STAGE_FILE      = 0;
    constexpr uintptr_t ADDR_GAME_MODE_FLAG       = 0;
    constexpr uintptr_t ADDR_ACTIVE_STAGE_ID      = 0;
    constexpr uintptr_t ADDR_CHAR_STAGE_PER_ROUND = 0x540337;
    // Stage filename table (256-byte name entries; LoadStageFile does
    // sprintf("%s.stage", table[256*idx])). 0 = not mapped on FM95 yet;
    // the random-stage clamp skips when 0.
    constexpr uintptr_t ADDR_STAGE_FILE_TABLE     = 0;

    // FM2K-only fields stubbed at 0 on FM95 — the AssignPlayerColor /
    // per-slot color-pick mechanism is FM2K-specific. FM95 has its own
    // palette select that we don't currently surface to MATCH_START.
    constexpr uintptr_t ADDR_CHARSLOT0_COLOR_PICK = 0;
    constexpr size_t    CHARSLOT_STRIDE           = 0;

    // Engine tag — runtime check for code that needs to branch on engine.
    constexpr bool kIsFM95 = true;
    constexpr bool kIsFM2K = false;
}
#else  // ENGINE_FM2K (default)
namespace FM2K {
    // Function addresses
    constexpr uintptr_t ADDR_GET_PLAYER_INPUT = 0x414340;
    // FM2K has a single dispatch fn; this stub exists only so the FM95-
    // specific install path in InitializeHooks compiles on FM2K builds.
    constexpr uintptr_t ADDR_GET_PLAYER_INPUT_P2 = 0;
    constexpr uintptr_t ADDR_UPDATE_GAME = 0x404CD0;
    constexpr uintptr_t ADDR_RUN_GAME_LOOP = 0x405AD0;
    constexpr uintptr_t ADDR_RENDER_GAME = 0x404DD0;
    constexpr uintptr_t ADDR_BLIT_SPRITE = 0x40C140;  // BlitSpriteWithBlendMode (render profiler leaf / SIMD reimpl)
    constexpr uintptr_t ADDR_SPRITE_RENDER_ENGINE = 0x40CC30;  // sprite_rendering_engine (case -10 blur intercept)
    constexpr uintptr_t ADDR_GAME_RAND = 0x417A22;
    constexpr uintptr_t ADDR_PROCESS_INPUTS = 0x4146D0;
    constexpr uintptr_t ADDR_DISPATCH_SCRIPT_SOUND = 0x403430;  // SFX dispatcher — rollback sound hook

    // Sound-system entry points (rollback sound layer, IDA-verified 2026-04-23).
    constexpr uintptr_t ADDR_PLAY_SOUND_FROM_BUFFER_ARRAY    = 0x415DF0;  // core DSound play + round-robin
    constexpr uintptr_t ADDR_STOP_ALL_SOUNDS_IN_BUFFER_ARRAY = 0x415F00;  // stop every buffer in one array
    constexpr uintptr_t ADDR_CONTROL_SOUND                   = 0x4034D0;  // 0=stop all, 2=stop MIDI, 3=stop CD
    // g_sound_channel_table -- array of SoundBufferArray* (end marker used by
    // ReleaseAllSoundBuffers). Channel identity for the Mike Z SFX layer.
    constexpr uintptr_t ADDR_SOUND_CHANNEL_TABLE     = 0x430640;
    constexpr uintptr_t ADDR_SOUND_CHANNEL_TABLE_END = 0x433240;

    // Game state addresses
    constexpr uintptr_t ADDR_FRAME_COUNTER = 0x4456FC;
    constexpr uintptr_t ADDR_GAME_MODE = 0x470054;
    constexpr uintptr_t ADDR_RANDOM_SEED = 0x41FB1C;

    // Player data
    constexpr uintptr_t ADDR_P1_INPUT = 0x4259C0;
    constexpr uintptr_t ADDR_P2_INPUT = 0x4259C4;
    constexpr uintptr_t ADDR_P1_HP = 0x47010C;
    constexpr uintptr_t ADDR_P2_HP = 0x47030C;

    // CSS (Character Select Screen) state - from IDA analysis
    // Cursor positions: each is a struct with {int x, int y}
    constexpr uintptr_t ADDR_P1_CURSOR_POS = 0x424E50;  // g_p1_cursor_pos
    constexpr uintptr_t ADDR_P2_CURSOR_POS = 0x424E58;  // g_p2_cursor_pos

    // Action state: 0 = selecting, 1 = locked/confirmed
    constexpr uintptr_t ADDR_P1_ACTION_STATE = 0x47019C;  // g_p1_action_state
    constexpr uintptr_t ADDR_P2_ACTION_STATE = 0x4701A0;  // g_p2_action_state

    // Round timer counter: frames since both players locked (>100 triggers battle).
    // Verified 2026-05-08 via IDA xref of game_state_manager @ 0x406FC0 — there are
    // 7 reads/writes from there. The legacy 0x47008E address had zero xrefs (dead);
    // it predates the IDA pass and never actually drove the CSS->battle transition.
    constexpr uintptr_t ADDR_ROUND_TIMER_COUNTER = 0x424F00;  // g_round_timer_counter

    // Internal game timer - equivalent to CCCaster's CC_WORLD_TIMER_ADDR
    constexpr uintptr_t ADDR_GAME_TIMER = 0x470044;  // g_game_timer (increments every game frame)

    // CSS active player (for stage select mode)
    constexpr uintptr_t ADDR_CSS_ACTIVE_PLAYER = 0x424F24;  // g_css_active_player

    // CSS-selected character index pair (uint32 [2]). The IDA name on
    // this address used to be g_player_stage_positions, which was a
    // misnomer — neither field carries stage data; both are just the
    // currently-selected character grid index for each player. Renamed
    // 2026-05-05 to g_p1_selected_char_idx / g_p2_selected_char_idx
    // after verifying via ProcessCharacterSelectHandler (0x407D70).
    // Kept as a legacy alias here so any older code that still names
    // it the old way keeps building until they migrate to
    // ADDR_P1_SELECTED_CHAR / ADDR_P2_SELECTED_CHAR below.
    constexpr uintptr_t ADDR_PLAYER_STAGE_POSITIONS = 0x470020;

    // Character slots - OPTIMIZED: only save dynamic portion for rollback
    // Static data (sprites, animations, hitboxes) loaded from .player files doesn't change
    // See savestate.h for CHAR_SLOT_* constants
    constexpr uintptr_t ADDR_CHAR_SLOTS       = 0x4D1D90;  // g_character_data_base (Wave C audit corrected from 0x4D1D80)
    constexpr size_t    CHAR_SLOT_TOTAL_SIZE  = 57407;     // Per-slot size from IDA
    constexpr size_t    CHAR_SLOT_COUNT       = 8;

    // Object pool — 1024 × 382 layout, base 0x4701E0.
    constexpr uintptr_t ADDR_OBJECT_POOL      = 0x4701E0;
    constexpr size_t    OBJECT_POOL_STRIDE    = 382;       // bytes/slot
    constexpr size_t    OBJECT_POOL_COUNT     = 1024;
    constexpr size_t    SIZE_OBJECT_POOL      = 0x5F800;   // OBJECT_POOL_COUNT * OBJECT_POOL_STRIDE

    // Input ring — 1024 entries × 4 bytes (10-bit modulo idx).
    // g_input_buffer_index @ 0x447EE0 -- matches savestate_internal.h and every
    // live buf_idx read in the hooks (rings at 0x4280E0/0x4290E0). The previous
    // value here (0x470000) had zero consumers in the FM2K build and did not
    // match the ring index the game actually advances. Corrected 2026-07-11
    // (fm95 parity workplan 0d) so FM95's 0x437700 twin pairs with the real
    // FM2K global.
    constexpr uintptr_t ADDR_INPUT_BUFFER_INDEX = 0x447EE0;
    constexpr uintptr_t ADDR_P1_INPUT_HISTORY   = 0x470200;
    constexpr uintptr_t ADDR_P2_INPUT_HISTORY   = 0x470400;
    constexpr size_t    INPUT_HISTORY_LEN       = 1024;
    constexpr uint32_t  INPUT_HISTORY_MASK      = INPUT_HISTORY_LEN - 1;  // 0x3FF

    // CSS — selected character indexes. Point to g_player_stage_positions,
    // a uint32 pair at 0x470020/0x470024. ProcessCharacterSelectHandler
    // (0x407D70 in WonderfulWorld_ver_0946) writes each player's pick
    // into [0]/[1] then immediately calls
    // `player_data_file_loader(player, g_player_stage_positions[player])`,
    // which loads `<g_char_slot_data[256 * idx]>.player`. So [0]/[1] are
    // the canonical "this is the char I picked" values that drive the
    // .player roster lookup. Verified via IDA xrefs 2026-05-05.
    //
    // The previous values 0x470180/0x470184 were unwritten zero memory
    // (no xrefs from any code) — every match shipped char_id=0 to the
    // hub, so both peers always got logged as the slot-0 character.
    constexpr uintptr_t ADDR_P1_SELECTED_CHAR     = 0x470020;
    constexpr uintptr_t ADDR_P2_SELECTED_CHAR     = 0x470024;

    // FM95-only fields (stubbed at 0 on FM2K). Consumers must guard
    // on `if constexpr (kIsFM95)`; without these stubs the FM2K-side
    // parse of an `if constexpr (kIsFM95) { FM2K::ADDR_P1_WIN_COUNTER }`
    // body fails name lookup before the branch is discarded.
    constexpr uintptr_t ADDR_P_MAIN_OBJECT_PTR  = 0;
    constexpr uintptr_t ADDR_P1_WIN_COUNTER     = 0;
    constexpr uintptr_t ADDR_P2_WIN_COUNTER     = 0;

    // FM2K round-win counters — IDA-suggested + empirically verified
    // (v0.2.21 [ROUND-WIN-PROBE] log: P1=0, P2=1 after P2 wins round 1;
    // P2=2 after round 2). Per-char-slot field at offset -0x18 from HP
    // (HP at 0x4DFC85, win at 0x4DFC6D). Stride 0xE03F = HP_STRIDE.
    // Hooks.cpp:680 misnames these `g_match_phase` / `g_round_sub_state`
    // — they're actually per-slot rounds-won counters. Reset to 0 at
    // start of new match, increment on each round won by that player.
    constexpr uintptr_t ADDR_P1_ROUNDS_WON      = 0x4DFC6D;
    constexpr uintptr_t ADDR_P2_ROUNDS_WON      = 0x4EDCAC;
    constexpr uintptr_t ADDR_P1_DAMAGE_TAKEN    = 0;
    constexpr uintptr_t ADDR_P2_DAMAGE_TAKEN    = 0;
    constexpr uintptr_t ADDR_LOAD_STAGE_FILE_ALT  = 0;
    constexpr uintptr_t ADDR_CHAR_STAGE_PER_ROUND = 0;
    // Character filename table — g_char_slot_data, 256-byte CP932 strings.
    constexpr uintptr_t ADDR_CHAR_FILENAME_TABLE  = 0x435474;
    constexpr size_t    CHAR_FILENAME_STRIDE      = 256;
    constexpr size_t    CHAR_FILENAME_COUNT       = 258;       // 0x1023C / 256

    // Stage selection — global int32 the round-init code reads as the
    // VS-mode stage_id. IDA-verified in WonderfulWorld_ver_0946:
    //   * vs_round_function @ 0x4087e0 calls LoadStageFile(wParam),
    //     where wParam is IDA's name for this address.
    //   * settings_dialog_proc @ 0x416387 stores the stage dropdown's
    //     CB_GETCURSEL into this address.
    // The previous value 0x470188 had ZERO xrefs in WW — every write
    // from the random-stage feature went to a dead address while
    // the game read the cursor's own pick from 0x43010c.
    constexpr uintptr_t ADDR_SELECTED_STAGE       = 0x43010c;
    // g_stage_file_buffer @ 0x43A29C: 256-byte stage filename entries;
    // LoadStageFile(idx) does sprintf("%s.stage", table[256*idx]). An
    // out-of-range idx gives an empty/garbage name -> modal "GameStage
    // Open error" box mid-match. First NUL first-byte = end of list.
    constexpr uintptr_t ADDR_STAGE_FILE_TABLE     = 0x43A29C;
    // LoadStageFile @ 0x4041E0 -- sprintf("%s.stage", stage_table[256*idx]).
    // IDA-verified in WonderfulWorld_ver_0946: exactly THREE callers, all in
    // vs_round_function @ 0x4086A0 -- 0x408785 and 0x4087E0 pass
    // g_selected_stage (the VS and Team match-init paths, mutually exclusive,
    // once per match), 0x40882C passes a per-character story-table value.
    // This is the ordering-correct seam for random stage: the dispatcher reads
    // 0x43010c HERE, so writing 0x43010c after this point only affects the
    // NEXT match (the old one-match-late bug).
    constexpr uintptr_t ADDR_LOAD_STAGE_FILE      = 0x4041E0;
    // g_game_mode_flag @ 0x470058 -- 0 = 1P/story, 1 = VS 1v1, 2 = VS Team.
    // Selects which branch of vs_round_function case 0/1 runs, i.e. whether
    // the LoadStageFile call reads g_selected_stage or the story table.
    constexpr uintptr_t ADDR_GAME_MODE_FLAG       = 0x470058;
    // g_active_stage_id @ 0x470040 -- the dispatcher copies g_selected_stage
    // into this ONE LINE BEFORE calling LoadStageFile, so a stage override
    // applied at the load seam must write both or they disagree.
    constexpr uintptr_t ADDR_ACTIVE_STAGE_ID      = 0x470040;

    // Per-character-slot record table. Each slot is 0xE03F bytes;
    // 8 slots span 0x4D1D80..0x54FF83. Field at slot+0xE00B is the
    // resolved palette/color slot (0..5) — written by AssignPlayerColor
    // @ 0x406F20 once a player presses an attack button on confirm. In
    // 1v1 VS mode slot[0]=P1, slot[1]=P2; team mode uses 0..3 / 4..7.
    //
    // ADDR_CHARSLOT0_COLOR_PICK is `&slot[0].color` (0x4D1D80 + 0xE00B).
    // For slot N's color: ADDR_CHARSLOT0_COLOR_PICK + CHARSLOT_STRIDE * N.
    // IDA: g_charslot0_color_pick @ 0x4DFD8B (renamed from
    // g_charslot0_demo_link_id 2026-05-09 — the field at +0xE00B is the
    // color pick, the stale name was a misread of the same struct slot
    // when used in demo-replay context).
    constexpr uintptr_t ADDR_CHARSLOT0_COLOR_PICK = 0x4DFD8B;
    constexpr size_t    CHARSLOT_STRIDE           = 0xE03F;

    // g_frame_time_ms -- ms per sim step; main_game_loop @ 0x405AD3 sets it
    // to 10 and the CSS frame-skip math / BATTLE STATUS debug log read it.
    constexpr uintptr_t ADDR_FRAME_TIME_MS = 0x41E2F0;

    // Engine tag — runtime check for code that needs to branch on engine.
    constexpr bool kIsFM95 = false;
    constexpr bool kIsFM2K = true;
}
#endif // ENGINE_FM95

// ============================================================================
// MINIMAL GLOBAL STATE
// ============================================================================

// Player identity (set at startup from environment)
extern int g_player_index;  // 0 = P1/Host, 1 = P2/Client

// Frame counter (for logging)
extern uint32_t g_frame_counter;

// Rollback state flag - true during GekkoNet rollback replay frames
// Used by input hooks to avoid corrupting edge detection state during replay
extern bool g_is_rolling_back;

// #66: enable ROLLBACK for the character-select (CSS) gekko session (default
// OFF = lockstep, the shipping behavior). Set once at CSS session start from
// FM2K_CSS_ROLLBACK=1. When true, CSS runs a prediction window + save/load
// re-sim (sim-per-AdvanceEvent), suppresses .player loads on re-sim, and
// records replay/spectator inputs confirmed-only. Read cross-module by the
// CSS trampoline, the get-input hook, and the .player loader detour.
extern bool g_css_rollback;

// Network config (parsed at startup, used when entering battle)
extern bool g_offline_mode;
extern uint16_t g_local_port;
extern char g_remote_addr[64];

// Stress-test mode: single-instance determinism check via GekkoStressSession.
// When enabled, FM2KHook creates a GekkoStressSession with both players local,
// no network, and GekkoNet artificially rolls back every `check_distance` frames.
// If the sim is deterministic, desync_detection stays silent. If it isn't,
// we get a local-only repro of the nondeterminism bug without needing a peer.
// Set via FM2K_STRESS_MODE=1 env var. Implies !g_offline_mode (we still want
// GekkoNet active to drive save/load/advance events) but skips socket setup.
extern bool g_stress_mode;

// Spectator mode: this instance is a passive viewer, not a player. Skips
// the HELLO/HELLO_ACK netplay handshake and instead sends SPEC_JOIN_REQ to
// the configured remote (the host) at startup. The trampoline pins the
// phase to SPECTATOR_PLAYBACK regardless of game_mode so the local FM2K's
// CSS doesn't run native lockstep code while waiting for upstream.
// Set via FM2K_SPECTATOR_MODE=1 env var.
extern bool g_spectator_mode;

// Spectator fast catch-up: true while RunSpectatorTick's inner loop is
// burning through queued events to close the gap between pb_queue depth
// and live edge (LIVE_LAG_FRAMES target). Read by Hook_RenderGame to
// skip render and Hook_DispatchScriptSoundCommand to mute audio so a
// 30k-event drain doesn't blast 5 minutes of compressed sound in a few
// seconds. Cleared once pb_queue depth drops below the live-lag target.
extern bool g_spectator_catchup;

// User-toggled fast-forward (F12 in spectator window). When true, the
// spectator's catchup loop engages regardless of the one-shot latch and
// the queue-depth target — lets the user manually race to live edge
// after a lag spike, then toggle off to resume normal-speed playback.
// Window title displays "[FF]" while active. Default false.
extern bool g_spectator_ff_user;

// FM95 host-driven trampoline: Hook_UpdateGameState sets this to tell
// Hook_RenderGame to skip the host's natural render call this frame
// (the trampoline tick's RenderFrameWithSnapshot already rendered).
// Cleared by Hook_RenderGame on consumption.
extern bool g_fm95_skip_next_render;

// FM95: true only while a trampoline sim tick is executing (set around the
// TrampolineFrameTickHostDriven dispatch). Hook_timeGetTime uses it to
// virtualize the clock ONLY during the sim — CPW's own WinMain pacing loop
// also consumes timeGetTime, and virtualizing THAT warps the loop
// (catchup-spin/stall). Unused on FM2K (its TrampolineMainLoop owns pacing).
extern bool g_fm95_in_sim_tick;
extern bool g_fm95_loop_owned;   // FM95 owns its loop via the 0x40AD67 inline patch

// ============================================================================
// LOG-FILE PATH HELPER
// ============================================================================
// All hook-side diagnostic / debug log files (FM2K_P*_Debug.log, EB diag,
// rng trace, parity recorder, replay diff log, etc.) go through here so they
// land in `<game_dir>/logs/<filename>` instead of cluttering the game folder.
// `out` must be at least MAX_PATH bytes. Creates the logs directory lazily
// on first call. Returns true on success; on failure leaves `out` unchanged
// (callers can fall back to writing in cwd).
bool Fm2k_BuildLogPath(char* out, size_t out_size, const char* filename);

#endif // GLOBALS_H
