#pragma once

// ============================================================================
// STATUS: REFERENCE SPEC, NOT A BUILD INPUT.
//
// Nothing #includes this file. That is deliberate, not rot -- the addresses
// below were transcribed by hand into FM2KHook/src/core/globals.h, which is
// what the hook actually compiles against, and seven comments across the tree
// (globals.h:81, savestate.h:17, FM2KHook/CMakeLists.txt:22, FM2K_KgtParser
// .cpp, FM2K_Integration.h:274, ...) cite this file as the source of truth for
// FM95/CPW addresses.
//
// The hazard that creates: this file and globals.h can drift apart silently,
// since no compiler checks the transcription. If you change an address here,
// grep globals.h for the FM95 block and change it there too -- and vice versa.
// See docs/FM95_Support_Status.md, which tracks address drift as a known gap.
// (Flagged by the 2026-08-03 dead-code audit; kept on purpose.)
// ============================================================================

// ============================================================================
// FM95 Integration Header -- Comic Party Wars (CPW.exe), the FM95 prototype
// of FM2K. Address-only translation of FM2K_Integration.h; the launcher /
// UI / networking layer is binary-agnostic and reused as-is.
//
// Verified via IDA: structurally near-identical to FM2K (same per-tick/render
// split, same 256×0xA4 object pool, same LCG `state = 214013*state + 2531011`
// rand, same input ring buffer with 8-bit wrapping idx, same odd/even frame
// parity). All addresses below were confirmed by decompilation against the
// FM2K originals at /mnt/c/dev/wanwan/FM2K_Integration.h.
//
// Binary metadata
//   md5     : c110888b0e20be1f5758e5d7f0922903
//   sha256  : 8ab3f89951fc72070c9b5091995b8f19055cc4796108bb53add6f6485a4776a0
//   base    : 0x00400000
//   size    : 0x00226000
// ============================================================================

#include <windows.h>
#include <cstdint>

namespace FM95 {

// ----------------------------------------------------------------------------
// Function addresses (hook targets)
// ----------------------------------------------------------------------------

// Frame loop driver -- message pump + timeGetTime catchup loop (max 5 frames),
// calls update_game_state then render_game. FM2K analog: RUN_GAME_LOOP @ 0x405AD0.
// `_WinMain@16` IS the frame loop in FM95 -- the prototype has no separate driver.
constexpr uintptr_t ADDR_WINMAIN              = 0x40AB60;

// Per-tick game logic. Iterates 256-entry object pool, runs collision pass,
// physics, etc. FM2K analog: UPDATE_GAME @ 0x404CD0.
constexpr uintptr_t ADDR_UPDATE_GAME          = 0x40A060;

// DDraw vtable blit + sprite rendering. FM2K analog: RENDER_GAME @ 0x404DD0.
constexpr uintptr_t ADDR_RENDER_GAME          = 0x40A910;

// Per-frame input poll. Calls GetKeyboardState, increments g_input_buffer_index,
// writes ring buffers at g_p1_input_history[idx] and g_p2_input_history[idx].
// **Primary hook target for input injection.** FM2K analog: PROCESS_INPUTS @ 0x4146D0.
constexpr uintptr_t ADDR_PROCESS_INPUTS       = 0x408FF0;

// Per-player input read (called from process_game_inputs). FM2K analog: GET_PLAYER_INPUT @ 0x414340.
constexpr uintptr_t ADDR_GET_PLAYER_INPUT_P1  = 0x408AE0;
constexpr uintptr_t ADDR_GET_PLAYER_INPUT_P2  = 0x408D60;

// CRT `_rand` -- same LCG constants as FM2K (214013/2531011, return (state>>16)&0x7FFF).
// FM2K analog: GAME_RAND @ 0x417A22.
constexpr uintptr_t ADDR_GAME_RAND            = 0x41A864;

// Sound dispatch -- **mute target during rollback resimulation**.
// Switches on cmd byte: 0=stop, 1=play_wave, 2=play_midi, 3=play_cd.
// FM2K analog: DISPATCH_SCRIPT_SOUND @ 0x403430.
constexpr uintptr_t ADDR_DISPATCH_SCRIPT_SOUND = 0x401FF0;

// Per-object dispatch (jumps through g_object_function_table indexed by type).
constexpr uintptr_t ADDR_UPDATE_GAME_OBJECT   = 0x419A30;

// Object spawn helpers (256-slot allocator, 0xA4-byte stride, scans for type==0).
constexpr uintptr_t ADDR_CREATE_GAME_OBJECT   = 0x40E2C0;  // hard cap 255, MessageBox on full
constexpr uintptr_t ADDR_CREATE_EFFECT_OBJECT = 0x40E360;  // soft cap 246, silent fail

// Collision passes (run from update_game_state)
constexpr uintptr_t ADDR_PROCESS_HITBOX_COLLISIONS = 0x409350;
constexpr uintptr_t ADDR_HIT_DETECTION_SYSTEM      = 0x409750;

// Per-object script interpreter (the major game logic loop)
constexpr uintptr_t ADDR_CHARACTER_STATE_MACHINE   = 0x414020;
constexpr uintptr_t ADDR_PROCESS_OBJECT_SCRIPT_FRAME = 0x40DE70;

// Asset loaders (one per asset type + cmdline-mode alts)
constexpr uintptr_t ADDR_PLAYER_LOADER            = 0x406D70;
constexpr uintptr_t ADDR_PLAYER_LOADER_ALT        = 0x4062E0;
constexpr uintptr_t ADDR_LOAD_STAGE_FILE          = 0x4050A0;
constexpr uintptr_t ADDR_LOAD_STAGE_FILE_ALT      = 0x4054B0;
constexpr uintptr_t ADDR_LOAD_DEMO_FILE           = 0x4059B0;
constexpr uintptr_t ADDR_LOAD_DEMO_FILE_ALT       = 0x405DC0;
constexpr uintptr_t ADDR_LOAD_GAME_SYSTEM_FILE    = 0x404D00;
constexpr uintptr_t ADDR_LOAD_KGT_FILE            = 0x4072D0;
constexpr uintptr_t ADDR_LOAD_KGT_FROM_CMDLINE    = 0x406750;

// Init / lifecycle
constexpr uintptr_t ADDR_INIT_GAME_STATE          = 0x4075A0;
constexpr uintptr_t ADDR_INIT_WINDOW_AND_SUBSYS   = 0x40B420;
constexpr uintptr_t ADDR_INIT_DIRECTDRAW          = 0x401B70;
constexpr uintptr_t ADDR_INIT_DIRECTSOUND         = 0x401F20;
constexpr uintptr_t ADDR_INIT_JOYSTICKS           = 0x404050;
constexpr uintptr_t ADDR_RELEASE_JOYSTICKS        = 0x404170;
constexpr uintptr_t ADDR_DIRECTDRAW_CLEANUP       = 0x41A380;
constexpr uintptr_t ADDR_INIT_OBJECT_POOL         = 0x40E3E0;
constexpr uintptr_t ADDR_INIT_ALL_CHAR_SLOTS      = 0x4062A0;

// Window / cursor (used during pause/resume)
constexpr uintptr_t ADDR_WINDOWED_BLIT_PRESENT    = 0x404730;
constexpr uintptr_t ADDR_SHOW_CURSOR_CLIP         = 0x404570;
constexpr uintptr_t ADDR_RESTORE_CURSOR_CLIP      = 0x404650;
constexpr uintptr_t ADDR_MAIN_WINDOW_PROC         = 0x40B930;

// Joystick read (used by process_game_inputs)
constexpr uintptr_t ADDR_READ_JOYSTICK_INPUT      = 0x4041A0;

// ----------------------------------------------------------------------------
// Frame / RNG / mode state
// ----------------------------------------------------------------------------

// CRT _rand seed (single u32). FM2K analog: 0x41FB1C.
constexpr uintptr_t ADDR_RANDOM_SEED          = 0x4243FC;

// Game tick counter -- incremented by update_game_state every frame.
// LSB is ping-pong parity for object iteration direction. FM2K analog: 0x4456FC.
constexpr uintptr_t ADDR_GAME_TICK_COUNTER    = 0x4DD7A8;

// Render frame counter -- incremented by render_game.
constexpr uintptr_t ADDR_RENDER_FRAME_COUNTER = 0x425520;

// Game mode / round phase. 0=normal, 1=in-round-active, 2/3=KO/draw, 4=time-up.
// FM2K analog: 0x470054.
constexpr uintptr_t ADDR_GAME_MODE            = 0x425558;

// Pause / quit / loop-pause flags
constexpr uintptr_t ADDR_GAME_PAUSED          = 0x425568;
constexpr uintptr_t ADDR_QUIT_FLAG            = 0x425564;
constexpr uintptr_t ADDR_PAUSE_LOOP_FLAG      = 0x425590;

// Frame timing (timeGetTime-based)
constexpr uintptr_t ADDR_LAST_FRAME_TIME      = 0x4255E4;
constexpr uintptr_t ADDR_FRAME_TIME_MS        = 0x422F7C;
constexpr uintptr_t ADDR_FIRST_FRAME_DONE     = 0x4255E8;
constexpr uintptr_t ADDR_FRAME_TIME_DELTA     = 0x426960;
constexpr uintptr_t ADDR_TOTAL_TIME_MS        = 0x437760;
constexpr uintptr_t ADDR_FRAME_SKIP_SPEEDUP   = 0x509060;

// Cmdline-derived mode flag. 0=editor, 1=normal, 2=practice, 3=vs-debug, 4=demo.
constexpr uintptr_t ADDR_CMDLINE_INIT_FLAG    = 0x42555C;

// ----------------------------------------------------------------------------
// Input ring buffer (the rollback target surface)
// ----------------------------------------------------------------------------

// Circular index, advances modulo 256 each frame. FM2K analog: 0x470000.
constexpr uintptr_t ADDR_INPUT_BUFFER_INDEX   = 0x437700;

// Per-player 256-entry × 4-byte input history rings.
// FM2K analog: 0x470200 (P1) / 0x470400 (P2).
constexpr uintptr_t ADDR_P1_INPUT_HISTORY     = 0x431720;
constexpr uintptr_t ADDR_P2_INPUT_HISTORY     = 0x431B20;
constexpr size_t    INPUT_HISTORY_LEN         = 256;

// Extra input history (used by character_state_machine motion lookups).
constexpr uintptr_t ADDR_INPUT_HISTORY_EXTRA  = 0x431320;

// Current-frame combined inputs
constexpr uintptr_t ADDR_P1_INPUT             = 0x437750;
constexpr uintptr_t ADDR_P2_INPUT             = 0x437754;

// Per-player control mode: 0=normal, 1=combo-replay, 2=copy-other, 3=replay-D, 4=replay-H, 5=nullsub.
// Used by process_game_inputs to redirect inputs during practice/demo replay.
// NOTE: dispatch only fires when g_game_mode == 1 (round-end / rematch screen). During active
// battle (g_game_mode == 0) inputs are written raw regardless of control_mode.
constexpr uintptr_t ADDR_P1_CONTROL_MODE      = 0x432724;
constexpr uintptr_t ADDR_P2_CONTROL_MODE      = 0x432734;

// Raw GetKeyboardState buffer (256 bytes) -- populated by process_game_inputs each frame.
constexpr uintptr_t ADDR_KEY_STATE            = 0x4253C8;
constexpr uintptr_t ADDR_KEYPRESS_STATE       = 0x4252C4;

// Per-player keyboard binding tables (12 bytes each, parallel layout).
// Layout: +0 up VK, +1 left VK, +2 down VK, +3 right VK,
//         +4..+7 packed buttons 1-4 VKs, +8..+11 packed buttons 5-6 + pad.
constexpr uintptr_t ADDR_P1_KEY_BINDING       = 0x426980;
constexpr uintptr_t ADDR_P2_KEY_BINDING       = 0x426990;

// Joystick button bindings (16 bytes total, 4-byte packed entries).
// Layout overlaps players: g_p1_joy_buttons (0x4376F0) holds P1 buttons 1-4,
// g_p2_joy_buttons (0x4376F4) holds P1 buttons 5-6 + P2 buttons 1-2,
// g_p3_joy_buttons (0x4376F8) holds P2 buttons 3-6.
constexpr uintptr_t ADDR_P1_JOY_BUTTONS       = 0x4376F0;
constexpr uintptr_t ADDR_P2_JOY_BUTTONS       = 0x4376F4;
constexpr uintptr_t ADDR_P3_JOY_BUTTONS       = 0x4376F8;

// ----------------------------------------------------------------------------
// Object pool (256 entries × 0xA4 stride = 0xA400 bytes)
// ----------------------------------------------------------------------------

constexpr uintptr_t ADDR_OBJECT_POOL          = 0x426A40;
constexpr size_t    OBJECT_POOL_COUNT         = 256;
constexpr size_t    OBJECT_POOL_STRIDE        = 0xA4;
constexpr size_t    SIZE_OBJECT_POOL          = OBJECT_POOL_COUNT * OBJECT_POOL_STRIDE;
constexpr uintptr_t ADDR_OBJECT_POOL_LAST     = 0x430D9C;  // last entry, used by reverse-direction iter

// Object dispatch type IDs -- index into g_object_function_table[31] at 0x424078.
// Created via create_game_object(type, script_id, pos_x, pos_y, player_idx, aux).
namespace ObjectType {
    constexpr uint32_t INIT_MAIN          = 3;   // obj_init_main_state -- boot-time game init
    constexpr uint32_t CHARACTER          = 6;   // character_state_machine -- playable character
    constexpr uint32_t TITLE_DEMO_LOOP    = 15;  // obj_title_demo_loop -- title attract loop
    constexpr uint32_t BATTLE             = 16;  // vs_round_function -- round / battle state machine
    constexpr uint32_t TITLE_CSS          = 19;  // title_screen_state_machine -- title + character select
    constexpr uint32_t POST_CSS_INTRO     = 21;  // obj_post_css_round_intro -- pre-battle setup
    constexpr uint32_t MATCH_RESULT       = 22;  // obj_match_result_state -- between-rounds reload
    constexpr uint32_t DECREMENT_TIMER    = 25;  // obj_decrement_timer
    constexpr uint32_t SCORE_DIGIT        = 26;  // obj_score_digit_display
    constexpr uint32_t DEMO_INTRO         = 27;  // obj_demo_intro_state -- story cutscene player
    constexpr uint32_t STORY_PROGRESS     = 28;  // obj_story_progress -- story-mode dispatcher
    constexpr uint32_t ROUND_DEMO         = 29;  // obj_round_demo_state -- game-over demo
    constexpr uint32_t LOGO_INTRO         = 30;  // obj_logo_intro_state -- boot logo (default startup)
}

// Object dispatch table base -- 31 function pointers, indexed by ObjectSlot.type.
constexpr uintptr_t ADDR_OBJECT_FUNCTION_TABLE_TYPED = 0x424078;
constexpr size_t    OBJECT_FUNCTION_TABLE_COUNT      = 31;

// State-machine objects (types 15/16/19/21/22/27/28/29/30) repurpose ObjectSlot fields:
//   +108  sub_state                (header struct says 'script_partner_ptr' -- that's only for type 6)
//   +112  sub_state_data_a         (often player_idx)
//   +116  sub_state_data_b
//   +120  sub_state_timer          (counts up while waiting in a sub-state)
//   +124  sub_state_data_c
// The character object (type 6) keeps the field meanings documented in ObjectSlot below.

constexpr size_t    OBJ_OFF_SUB_STATE         = 108;
constexpr size_t    OBJ_OFF_SUB_STATE_DATA_A  = 112;
constexpr size_t    OBJ_OFF_SUB_STATE_DATA_B  = 116;
constexpr size_t    OBJ_OFF_SUB_STATE_TIMER   = 120;
constexpr size_t    OBJ_OFF_SUB_STATE_DATA_C  = 124;
constexpr size_t    OBJ_OFF_TYPE              = 0;
constexpr size_t    OBJ_OFF_SCRIPT_ID         = 44;

// Iteration cursor + index, set by update_game_state before each per-object call
constexpr uintptr_t ADDR_OBJECT_ITER_PTR      = 0x4DD260;
constexpr uintptr_t ADDR_OBJECT_ITER_INDEX    = 0x4376E4;
constexpr uintptr_t ADDR_RENDER_OBJECT_ITER_PTR = 0x4269A0;
constexpr uintptr_t ADDR_CURRENT_OBJECT_ID    = 0x423224;

// Object dispatch table (function pointers indexed by object type at +0)
constexpr uintptr_t ADDR_OBJECT_FUNCTION_TABLE = 0x424078;

// Per-player main-object pointer cache (indexed by player slot, 0/1)
constexpr uintptr_t ADDR_P_MAIN_OBJECT_PTR    = 0x430E84;

// Render linked list (per-priority object chains, populated by render_game)
constexpr uintptr_t ADDR_OBJECT_NODE_POOL     = 0x4DCA60;
constexpr uintptr_t ADDR_OBJECT_LIST_HEADS    = 0x4351E0;
constexpr uintptr_t ADDR_OBJECT_LIST_TAILS    = 0x4351E4;

// Per-object struct layout (offsets in bytes, derived from update_game_state +
// character_state_machine decompilation). NOT exhaustive -- covers fields the
// rollback layer reads.
struct ObjectSlot {
    uint32_t type;                // +0   : 0=empty, 1=disabled, ≥2=active
    uint32_t _pad04[3];           // +4..15
    uint32_t state_class;         // +16 (offset 4 dwords)
    uint32_t script_frame_remaining; // +20 : 9-clamp gate for script execution
    uint32_t flags_a;             // +24
    uint32_t script_cmd_idx;      // +28 : current command index in word_519230 table
    uint32_t script_subframe_timer; // +32 : ticks until next cmd advance
    uint32_t pos_x;               // +36 : fixed 14.18
    uint32_t pos_y;               // +40 : fixed 14.18
    uint32_t script_id;           // +44 : current script
    uint32_t vel_x;               // +48
    uint32_t vel_y;               // +52
    uint32_t vel_x_extra;         // +56
    uint32_t vel_y_extra;         // +60
    uint32_t vel_decay_x;         // +64
    uint32_t vel_decay_y;         // +68
    uint32_t hp;                  // +72 : pending damage application
    uint32_t throw_target_anim;   // +76
    uint32_t pre_anim_change_id;  // +80
    uint32_t _pad84_88;           // +84..91
    uint32_t facing;              // +92 : 0=right, 1=left
    uint32_t char_id;             // +96 : 229844-byte stride into g_char_slot_data
    uint32_t char_id_saved;       // +100 : restored at script reset
    uint32_t floor_y;             // +104
    uint32_t y_max_clamp;         // +108
    uint32_t flags_b;             // +112 : extensive flag set (player_idx in low 3 bits)
    uint32_t round_lockout_timer; // +116
    uint32_t anim_subframe;       // +120
    uint32_t state_id;            // +124 : current state in word_519230 table
    uint32_t pending_state_idx;   // +128
    uint32_t script_partner_ptr;  // +132
    uint32_t pending_anim;        // +136
    uint32_t pending_state_delay; // +140
    uint32_t pending_misc;        // +144
    uint32_t loop_count_remaining; // +148 (byte) : `9` cmd loop counter
    uint32_t loop_target_idx;     // +149..152 (word + dword)
    uint32_t pending_loop_state;  // +153
    uint32_t afterimage_active;   // +157 (byte) : `15` cmd state
    uint32_t afterimage_color;    // +158 (byte)
    uint32_t afterimage_period;   // +159 (byte)
    uint32_t parent_obj_ptr;      // +160 : object that spawned this one
};
static_assert(sizeof(ObjectSlot) <= OBJECT_POOL_STRIDE, "ObjectSlot too big");

// ----------------------------------------------------------------------------
// Character / stage / demo / system data slots
// ----------------------------------------------------------------------------

// 5 character slots × 229844 bytes (0x381D4). Loaded from .player files.
// Contains: palette (32 entries × 4 bytes), images (1024 bitmap descriptors),
// sounds (256 buffers), 39936 bytes of script data, etc. Static once loaded.
constexpr uintptr_t ADDR_CHAR_SLOT_DATA       = 0x509100;
constexpr size_t    CHAR_SLOT_STRIDE          = 229844;
constexpr size_t    CHAR_SLOT_COUNT           = 5;

constexpr uintptr_t ADDR_STAGE_DATA           = 0x4DD7C0;
constexpr size_t    STAGE_DATA_SIZE           = 0x2B79C;

constexpr uintptr_t ADDR_KGT_DATA             = 0x463BE0;  // master config (palette + system images)
constexpr size_t    KGT_DATA_SIZE             = 0x78D48;
constexpr uintptr_t ADDR_KGT_LOADED_FLAG      = 0x463BE8;

constexpr uintptr_t ADDR_SYSTEM_DATA          = 0x471190;  // populated by LoadGameSystemFile

// Slot identity / flags
constexpr uintptr_t ADDR_CHAR_SLOT_ID_MAP     = 0x534890;  // [char_id] -> player_idx+1
constexpr uintptr_t ADDR_PLAYER_DATA_VALID    = 0x426900;  // bool[] per .player file
constexpr uintptr_t ADDR_PLAYER_LOADED_COUNT  = 0x426930;
constexpr uintptr_t ADDR_PLAYER_COUNT_LOADED  = 0x426708;
constexpr uintptr_t ADDR_STAGE_LOADED_ID      = 0x508F50;
constexpr uintptr_t ADDR_DEMO_LOADED_ID       = 0x463610;

// File-name arrays (256 bytes per slot, NUL-terminated)
constexpr uintptr_t ADDR_PLAYER_FILE_NAMES    = 0x463CF0;
constexpr uintptr_t ADDR_STAGE_FILE_NAMES     = 0x467B88;
constexpr uintptr_t ADDR_DEMO_FILE_NAMES      = 0x46AD88;

// ----------------------------------------------------------------------------
// Character Select state -- per-player struct at 0x432720, stride 16 bytes.
// ----------------------------------------------------------------------------
//
// Layout (per player, indices 0=P1, 1=P2):
//   +0x00  char_grid_cursor    int32   character roster index (mod 50)
//   +0x04  control_mode        int32   see ADDR_P*_CONTROL_MODE comment above
//   +0x08  color_variant       int32   palette / alt-color (0..3, mod 4)
//   +0x0C  confirmed           int32   0=picking, 1=locked-in
//
// Read/written by title_screen_state_machine cases 0x64..0x67 (CSS active loop)
// and reread by obj_match_result_state when reloading player files between matches.
//
// FM95 has NO global g_game_mode value that signals 'CSS active' (FM2K uses 2000).
// Detection requires walking the object pool -- see CharSelect::IsCSSActive() below.

namespace CharSelect {
    constexpr uintptr_t ADDR_BASE              = 0x432720;
    constexpr size_t    PER_PLAYER_STRIDE      = 16;

    constexpr uintptr_t ADDR_P1_CHAR_CURSOR    = 0x432720;
    constexpr uintptr_t ADDR_P1_COLOR_VARIANT  = 0x432728;
    constexpr uintptr_t ADDR_P1_CONFIRMED      = 0x43272C;
    constexpr uintptr_t ADDR_P2_CHAR_CURSOR    = 0x432730;
    constexpr uintptr_t ADDR_P2_COLOR_VARIANT  = 0x432738;
    constexpr uintptr_t ADDR_P2_CONFIRMED      = 0x43273C;

    // Roster size -- game iterates char_cursor mod 50 in CSS navigation.
    constexpr size_t    ROSTER_SIZE            = 50;

    // CSS-active sub_state range on a type=19 object (title_screen_state_machine).
    constexpr uint32_t  SUBSTATE_CSS_ENTRY     = 0x28;  // 40
    constexpr uint32_t  SUBSTATE_CSS_END       = 0xC9;  // 201 (assist-select tail)
    constexpr uint32_t  SUBSTATE_CSS_ACTIVE_INPUT = 0x67;  // 103 -- main char-grid input loop

    // Battle-active sub_state range on a type=16 object (vs_round_function).
    constexpr uint32_t  SUBSTATE_BATTLE_HUD_SPAWN = 10;
    constexpr uint32_t  SUBSTATE_BATTLE_END       = 31;

    // Phase classifier -- walk the 256-slot pool looking for a type=19/16 object
    // and read its sub_state. Inline here so both the launcher (read via RPM) and
    // the hook DLL (direct ptr) can share the formula.
    enum class Phase { Boot, Title, CSS, PostCSS, Battle, MatchEnd, Other };

    inline Phase ClassifyPhase(const uint8_t* pool_base) noexcept {
        for (size_t i = 0; i < OBJECT_POOL_COUNT; ++i) {
            const uint8_t* slot = pool_base + i * OBJECT_POOL_STRIDE;
            uint32_t type = *reinterpret_cast<const uint32_t*>(slot + OBJ_OFF_TYPE);
            if (type < 2) continue;  // 0=empty, 1=disabled
            uint32_t sub = *reinterpret_cast<const uint32_t*>(slot + OBJ_OFF_SUB_STATE);
            if (type == ObjectType::TITLE_CSS) {
                if (sub >= SUBSTATE_CSS_ENTRY && sub <= SUBSTATE_CSS_END) return Phase::CSS;
                return Phase::Title;
            }
            if (type == ObjectType::BATTLE) {
                if (sub >= SUBSTATE_BATTLE_HUD_SPAWN && sub <= SUBSTATE_BATTLE_END) return Phase::Battle;
                return Phase::MatchEnd;
            }
            if (type == ObjectType::POST_CSS_INTRO) return Phase::PostCSS;
            if (type == ObjectType::LOGO_INTRO || type == ObjectType::TITLE_DEMO_LOOP) return Phase::Boot;
        }
        return Phase::Other;
    }
}

// ----------------------------------------------------------------------------
// Round / match state
// ----------------------------------------------------------------------------

constexpr uintptr_t ADDR_ROUND_COUNT_SETTING  = 0x467B80;
constexpr uintptr_t ADDR_ROUND_COUNT_MAX      = 0x5E9A3C;
constexpr uintptr_t ADDR_ROUND_TIME_LIMIT     = 0x5E9A34;
constexpr uintptr_t ADDR_ROUND_STARTING_PLAYER = 0x426934;
constexpr uintptr_t ADDR_ROUND_ACTIVE_FLAG    = 0x426940;
constexpr uintptr_t ADDR_PRACTICE_MODE_FLAG   = 0x426704;
constexpr uintptr_t ADDR_PLAYER_ROUND_CHOICES = 0x426938;
constexpr uintptr_t ADDR_SCORE_ROUND_COUNT    = 0x4DD268;

// Per-player round/score arrays (25-int stride, indexed by player slot 0..N).
// "g_p_*" prefix in IDA matches FM2K's per-player effect/score table layout.
constexpr uintptr_t ADDR_P_SCORE_VALUE        = 0x5E98AC;
constexpr uintptr_t ADDR_P_ROUND_LOSE_COUNT   = 0x5E98B0;
constexpr uintptr_t ADDR_P_ROUND_WIN_COUNT    = 0x5E98B4;
constexpr uintptr_t ADDR_P_METER_LEVEL        = 0x5E98B8;
constexpr uintptr_t ADDR_P_METER_PROGRESS     = 0x5E98BC;
constexpr uintptr_t ADDR_P_COMBO_HITS         = 0x5E98C8;
constexpr uintptr_t ADDR_P_COMBO_DAMAGE       = 0x5E98CC;
constexpr uintptr_t ADDR_P_COMBO_MAX          = 0x5E98D0;
constexpr uintptr_t ADDR_P_PARTNER_OBJECT     = 0x5E9900;
constexpr uintptr_t ADDR_P_POS_X_SNAP         = 0x5E98A0;  // captured before round end for replay
constexpr uintptr_t ADDR_P_POS_Y_SNAP         = 0x5E98A4;
constexpr uintptr_t ADDR_P_FACING_SNAP        = 0x5E98A8;

// Per-side aggregates (P1 = idx 0 of arrays above + these scalars)
constexpr uintptr_t ADDR_P1_TOTAL_WINS        = 0x5E9910;  // current-round HP value
constexpr uintptr_t ADDR_P2_TOTAL_WINS        = 0x5E9974;
constexpr uintptr_t ADDR_P1_WIN_COUNTER       = 0x5E9914;  // rounds won
constexpr uintptr_t ADDR_P2_WIN_COUNTER       = 0x5E9978;
constexpr uintptr_t ADDR_ROUND_P1_SCORE       = 0x5E994C;
constexpr uintptr_t ADDR_ROUND_P2_SCORE       = 0x5E99B0;
constexpr uintptr_t ADDR_P1_METER_DATA        = 0x5E991C;
constexpr uintptr_t ADDR_P2_METER_DATA        = 0x5E9980;

// ----------------------------------------------------------------------------
// DirectDraw / rendering state
// ----------------------------------------------------------------------------

constexpr uintptr_t ADDR_DDRAW_BACK_SURFACE   = 0x425570;
constexpr uintptr_t ADDR_DDRAW_CLIPPER        = 0x42556C;
constexpr uintptr_t ADDR_DDRAW_PALETTE        = 0x425578;
constexpr uintptr_t ADDR_DDRAW_FULLSCREEN     = 0x42557C;
constexpr uintptr_t ADDR_DDRAW_INIT_STATE     = 0x425584;
constexpr uintptr_t ADDR_DDRAW_SURFACE_DESC   = 0x463B60;  // DDSURFACEDESC2-ish, dwSize set to 108
constexpr uintptr_t ADDR_DDRAW_SURFACE_PITCH  = 0x463B70;
constexpr uintptr_t ADDR_DDRAW_SURFACE_PTR    = 0x4269D0;  // current locked surface pixels
constexpr uintptr_t ADDR_RENDER_BUFFER        = 0x42550C;  // intermediate 640×480 framebuffer
constexpr uintptr_t ADDR_USE_ROTZOOM_BLIT     = 0x425594;  // 640×480 vs 320×240
constexpr uintptr_t ADDR_WINDOW_RESIZE_PENDING = 0x425598;
constexpr uintptr_t ADDR_SOUND_ENABLED        = 0x4255A0;

// Palette LUT (game maintains a 256-entry × 7-byte palette mapping table)
constexpr uintptr_t ADDR_PALETTE_LUT_ENTRIES  = 0x437780;
constexpr uintptr_t ADDR_PALETTE_LUT_END      = 0x437E80;
constexpr uintptr_t ADDR_PALETTE_USED_COUNT   = 0x466EF0;
constexpr uintptr_t ADDR_PALETTE_LUT_ACTIVE_IDX = 0x4DCA40;
constexpr uintptr_t ADDR_PALETTE_ACTIVE_IDX   = 0x508F57;
constexpr uintptr_t ADDR_PLAYER_WHITE_PAL_IDX = 0x426A00;
constexpr uintptr_t ADDR_WHITE_PALETTE_IDX    = 0x425519;
constexpr uintptr_t ADDR_BLACK_PALETTE_IDX    = 0x42551A;

// ----------------------------------------------------------------------------
// Heap / window / misc
// ----------------------------------------------------------------------------

constexpr uintptr_t ADDR_MAIN_HEAP_HANDLE     = 0x437764;  // GlobalAlloc result
constexpr uintptr_t ADDR_MAIN_HEAP_PTR        = 0x4376FC;  // GlobalLock result
constexpr uintptr_t ADDR_OBJECT_DISPATCH_COUNTER = 0x42551C;
constexpr uintptr_t ADDR_GAME_TIMER_LCG       = 0x43285C;  // secondary LCG state used in advance_frame_lcg
constexpr uintptr_t ADDR_ACTIVE_DEMO_ID       = 0x471188;
constexpr uintptr_t ADDR_SECONDARY_DEMO_ID    = 0x47118C;

// ----------------------------------------------------------------------------
// Static character property tables (read-only, no need to save in rollback)
// All indexed by char_id with stride 114922 (16-bit table) or 229844 (8/32-bit).
// Listed for completeness so hooks can interpret object-pool fields.
// ----------------------------------------------------------------------------

constexpr uintptr_t ADDR_CHAR_MAX_HP_TABLE        = 0x540317;
constexpr uintptr_t ADDR_CHAR_MAX_METER           = 0x54031F;
constexpr uintptr_t ADDR_CHAR_THROW_RANGE         = 0x54032E;
constexpr uintptr_t ADDR_CHAR_DEFAULT_STATE       = 0x53FFF0;
constexpr uintptr_t ADDR_CHAR_WALK_FORWARD_STATE  = 0x540002;
constexpr uintptr_t ADDR_CHAR_WALK_BACK_STATE     = 0x540000;
constexpr uintptr_t ADDR_CHAR_LANDING_STATE       = 0x540006;
constexpr uintptr_t ADDR_CHAR_PRE_ROUND_STATE     = 0x54000A;
constexpr uintptr_t ADDR_CHAR_ROUND_WINNER_STATE  = 0x54000C;
constexpr uintptr_t ADDR_CHAR_POST_MATCH_STATE    = 0x540028;
constexpr uintptr_t ADDR_CHAR_COLLISION_X_OFFSET  = 0x540327;
constexpr uintptr_t ADDR_CHAR_COLLISION_Y_FLOOR   = 0x540329;

// Reaction tables (hit/block lookups, indexed by char_id and reaction class)
constexpr uintptr_t ADDR_REACTION_NORMAL_HIT  = 0x53F0E8;
constexpr uintptr_t ADDR_REACTION_AIR_HIT     = 0x53F0EA;
constexpr uintptr_t ADDR_REACTION_HIGH_HIT    = 0x53F0EC;
constexpr uintptr_t ADDR_REACTION_LOW_HIT     = 0x53F0EE;
constexpr uintptr_t ADDR_REACTION_BLOCK       = 0x53F0F0;
constexpr uintptr_t ADDR_REACTION_BLOCK_HIGH  = 0x53F0F2;
constexpr uintptr_t ADDR_REACTION_BLOCK_LOW   = 0x53F0F4;
constexpr uintptr_t ADDR_REACTION_AIR_BLOCK   = 0x53F0F6;
constexpr uintptr_t ADDR_REACTION_THROW       = 0x53F0F8;

// Motion input tables (qcf/qcb/etc. lookup, indexed by char_id then motion_id)
constexpr uintptr_t ADDR_MOTION_INPUT_TABLE   = 0x53CC94;
constexpr uintptr_t ADDR_MOTION_INPUT_COUNT   = 0x53CCB4;

// Hitbox shape tables (per script frame, 56-byte stride)
constexpr uintptr_t ADDR_HITBOX_TYPE_TABLE    = 0x522E18;
constexpr uintptr_t ADDR_HITBOX_X_TABLE       = 0x522E10;
constexpr uintptr_t ADDR_HITBOX_Y_TABLE       = 0x522E12;
constexpr uintptr_t ADDR_HITBOX_W_TABLE       = 0x522E14;
constexpr uintptr_t ADDR_HITBOX_H_TABLE       = 0x522E16;
constexpr uintptr_t ADDR_HITBOX_FLAG_TABLE    = 0x522E19;

// State→frame lookup (resolves state_id → starting animation frame)
constexpr uintptr_t ADDR_STATE_FRAME_TABLE    = 0x519230;

// Script command stream (per char_id, 16-byte command stride)
constexpr uintptr_t ADDR_SCRIPT_CMD_BYTES     = 0x509210;
constexpr uintptr_t ADDR_SCRIPT_CMD_WORDS     = 0x509211;
constexpr uintptr_t ADDR_SCRIPT_CMD_PARAM1    = 0x509213;
constexpr uintptr_t ADDR_SCRIPT_CMD_PARAM2    = 0x509215;
constexpr uintptr_t ADDR_SCRIPT_CMD_PARAM3    = 0x509217;
constexpr uintptr_t ADDR_SCRIPT_CMD_PARAM4    = 0x50921A;
constexpr uintptr_t ADDR_SCRIPT_CMD_PARAM5    = 0x50921C;

// ----------------------------------------------------------------------------
// Differences from FM2K (informational -- affects rollback hook strategy)
// ----------------------------------------------------------------------------
//
// 1. **CSS exists, just looks different.** FM95 has a full character-select
//    screen (50-slot roster, color variants, locked-in flag) -- see CharSelect::
//    namespace above. The earlier note claiming 'no CSS' was wrong; CPW IDA
//    decompilation of title_screen_state_machine (0x40FB40) verifies cases
//    0x63..0xC9 are the CSS sub-state machine.
//
// 2. **g_game_mode encoding is different from FM2K.** FM95 keeps g_game_mode
//    near 0/1/10/0x10 (the original FM2K-era model); it does NOT use FM2K's
//    2000=CSS / 3000+=Battle magic numbers. During CSS and active battle,
//    g_game_mode == 0. During round-end / rematch screens, g_game_mode == 1
//    (and the |= 0x10 bit indicates round-result phase). To classify the
//    rollback phase, walk the object pool: see CharSelect::ClassifyPhase().
//
// 3. **No DirectPlay / Winsock imports.** The .exe has no native networking --
//    same as FM2K. Rollback is launcher-injected (GekkoNet over UDP).
//
// 4. **Frame loop lives in `_WinMain@16`.** No separate `RUN_GAME_LOOP` function.
//    Hook `update_game_state` and `render_game` directly; the WinMain loop
//    will keep calling them at the right cadence.
//
// 5. **Resolution toggle.** `g_use_rotzoom_blit` switches between 320×240 and
//    640×480. Render hooks must read this to choose the correct blit width.
//
// 6. **CRT _rand vs FM2K's inline LCG.** FM95 uses the actual MSVC CRT _rand
//    function. The seed at `g_rand_seed` is a single u32 with the same LCG
//    constants (214013, 2531011) -- capture/restore is identical.
//
// 7. **Object pool layout matches FM2K** (256 × 0xA4 stride). Field offsets in
//    `ObjectSlot` below describe the type=6 (character) view; state-machine
//    objects (types 15/16/19/21/22/27/28/29/30) reuse +108..+124 as
//    sub_state/data/timer -- see OBJ_OFF_SUB_STATE_* constants above.
//
// 8. **Sound dispatch.** FM95's `DispatchScriptSoundCommand` (0x401FF0) has a
//    cleaner switch (0=stop, 1=wave, 2=midi, 3=cd) than FM2K's. Same hook
//    strategy: skip during resimulation by reading a "rolling back" flag.
//
// 9. **Per-character data is at 0x509100** with 229844-byte stride (5 slots in
//    FM95 vs FM2K's larger character roster). Static script + image + sound
//    payloads -- *do not* save during rollback, only the per-instance state in
//    the object pool changes per-frame.
//
// 10. **Input bit layout is identical to FM2K** (left=0x1, right=0x2, up=0x4,
//     down=0x8, btn1-6=0x010..0x200, btn7=0x400). Same facing-aware L/R flip
//     in get_player_input_p1/p2 driven by g_p_facing_snap[25*player]. Same
//     rollback hazard -- must snapshot facing post-render and patch back into
//     the save-state pre-resimulation.
//
// 11. **Rollback hook target is process_game_inputs (0x408FF0)** -- hook
//     post-call and overwrite g_p1_input_history[g_input_buffer_index] and
//     g_p2_input_history[g_input_buffer_index]. Identical to FM2K strategy.

// ----------------------------------------------------------------------------
// Function typedefs (for MinHook trampolining -- same shapes as FM2K)
// ----------------------------------------------------------------------------

typedef int  (__cdecl* UpdateGameStateFunc)(void);
typedef void (__cdecl* RenderGameFunc)(void);
typedef int  (__cdecl* ProcessGameInputsFunc)(void);
typedef int  (__cdecl* GetPlayerInputFunc)(int player_idx);  // 1 or 2
typedef int  (__cdecl* GameRandFunc)(void);
typedef int  (__cdecl* DispatchScriptSoundCommandFunc)(int sound_obj_addr);

// ----------------------------------------------------------------------------
// MinimalGameState (rollback determinism check, mirrors FM2K's 48-byte struct)
// ----------------------------------------------------------------------------

struct MinimalGameState {
    // Round/match state
    uint32_t p1_total_wins;       // 0x5E9910 -- current-round HP value
    uint32_t p2_total_wins;       // 0x5E9974
    uint32_t p1_win_counter;      // 0x5E9914
    uint32_t p2_win_counter;      // 0x5E9978

    // Object 0/1 (player main objects) positions -- read from g_p_main_object_ptr
    uint32_t p1_pos_x, p1_pos_y;
    uint32_t p2_pos_x, p2_pos_y;

    // Timers + RNG
    uint32_t round_time_limit;    // 0x5E9A34
    uint32_t game_tick_counter;   // 0x4DD7A8
    uint32_t random_seed;         // 0x4243FC
    uint32_t input_checksum;      // XOR of recent g_p1_input_history / g_p2_input_history

    // Returns Fletcher32 of the struct
    uint32_t CalculateChecksum() const;
};
static_assert(sizeof(MinimalGameState) == 48, "MinimalGameState must stay 48 bytes for parity with FM2K");

// ----------------------------------------------------------------------------
// Input bitmask (same 11-bit layout as FM2K -- value lives in input history rings)
// ----------------------------------------------------------------------------

struct Input {
    union {
        struct {
            uint16_t left     : 1;
            uint16_t right    : 1;
            uint16_t up       : 1;
            uint16_t down     : 1;
            uint16_t button1  : 1;
            uint16_t button2  : 1;
            uint16_t button3  : 1;
            uint16_t button4  : 1;
            uint16_t button5  : 1;
            uint16_t button6  : 1;
            uint16_t button7  : 1;
            uint16_t reserved : 5;
        } bits;
        uint16_t value;
    };
};

// ----------------------------------------------------------------------------
// Cross-process memory access (same templates as FM2K_Integration.h)
// ----------------------------------------------------------------------------

template<typename T>
inline bool ReadMemory(HANDLE process, uintptr_t address, T& value) {
    SIZE_T bytes_read = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address),
        &value, sizeof(T), &bytes_read) && bytes_read == sizeof(T);
}

template<typename T>
inline bool WriteMemory(HANDLE process, uintptr_t address, const T& value) {
    SIZE_T bytes_written = 0;
    return WriteProcessMemory(process, reinterpret_cast<LPVOID>(address),
        const_cast<T*>(&value), sizeof(T), &bytes_written) && bytes_written == sizeof(T);
}

} // namespace FM95
