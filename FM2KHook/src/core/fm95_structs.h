// fm95_structs.h -- typed mirrors of the FM95/CPW in-memory layouts.
//
// These structs mirror the types defined in the CPW IDB (verified against
// create_game_object @0x40E2C0, render_game @0x40A910, and the title/round
// state machines -- see docs/dev/fm95_re_findings.md RE-2b + the RE-hygiene
// pass). Mirroring them here lets the FM95 hook read `slot->sub_state` /
// `round->players[1].win_counter` instead of scattering magic offsets, and the
// static_asserts turn any stride/offset drift into a compile error instead of a
// silent bad read. FM95-only by construction: the accessors below reference
// fixed CPW addresses and are never ODR-used in an FM2K build, so they are not
// emitted there (FM2K stays byte-identical). Fields we could not confirm from
// the disassembly are left as `pad`/`unk` at their known offsets rather than
// guessed.

#pragma once
#include <cstdint>
#include <cstddef>

namespace fm95 {

// Fixed CPW addresses (self-contained so this header does not depend on the
// engine-branched globals; values are the FM95 branch of FM2K::ADDR_*).
inline constexpr uintptr_t kObjectPoolAddr = 0x426A40;   // g_object_pool
inline constexpr size_t    kObjectPoolCount = 256;
inline constexpr uintptr_t kRoundStateAddr = 0x5E98A0;   // g_round_state_block
inline constexpr uintptr_t kCssPlayersAddr = 0x432720;   // g_css_players

// Per-character data blob strides (RE-hygiene pass, Fm95CharSlotConst).
inline constexpr size_t kCharSlotStride     = 229844;    // 0x381D4
inline constexpr size_t kCharFrameTableOfs  = 65840;     // 0x10130
inline constexpr size_t kCharFrameRecSize   = 39;

// ---------------------------------------------------------------------------
// Object-pool slot (0xA4 = 164 B). Type-punned in CPW: the same offsets carry
// a "character view" (type-6 objects) and a "state-machine view" (title/round
// dispatch). The state-machine names are given here since that is the view the
// FM95 hook cares about. Unconfirmed sub-regions are padding at known offsets.
// ---------------------------------------------------------------------------
struct Fm95ObjectSlot {
    uint32_t type;              // +0x00  object type; ==1 means deactivated
    uint8_t  _pad04[0x0C];      // +0x04
    uint32_t anim_frame_idx;    // +0x10  result[4]; indexes the frame table
    uint8_t  _pad14[0x04];      // +0x14
    int32_t  char_data_ptr;     // +0x18  result[6]; &g_char_slot_data + stride*id
    uint32_t frame_duration;    // +0x1C  result[7]; u16 hold-count from frame table
    uint32_t subframe_accum;    // +0x20  result[8]
    int32_t  pos_x;             // +0x24  result[9]; 14.18 fixed
    int32_t  pos_y;             // +0x28  result[10]
    uint32_t script_id;         // +0x2C  result[11]; create arg2
    uint8_t  _pad30[0x18];      // +0x30  vel_x/y/extra/decay (char view, unverified)
    uint32_t hp;                // +0x48  character view
    uint8_t  _pad4C[0x10];      // +0x4C
    uint32_t facing;            // +0x5C  result[23]; 1=left
    uint32_t char_id;           // +0x60  create arg5
    uint8_t  _pad64[0x08];      // +0x64
    int32_t  sub_state;         // +0x6C  result[27]; switch-dispatch field
    uint32_t sm_player_idx;     // +0x70  result[28]; player/char idx (reused as ctr)
    uint32_t sm_param_b;        // +0x74  result[29]; aux / scan-dir sign
    uint32_t sm_timer;          // +0x78  result[30]; *(iter+120) frame counter
    uint32_t sm_timer2;         // +0x7C  result[31]
    uint32_t sm_saved;          // +0x80  result[32]; saved pos for interpolation
    int32_t  sm_dir_delta;      // +0x84  result[33]
    uint8_t  _pad88[0x08];      // +0x88
    uint32_t sm_saved_misc;     // +0x90  result[36]; saved color
    uint8_t  _pad94[0x0C];      // +0x94
    int32_t  parent_obj_ptr;    // +0xA0  result[40]; caller slot at spawn
};
static_assert(sizeof(Fm95ObjectSlot) == 0xA4, "Fm95ObjectSlot must be 164 B");
static_assert(offsetof(Fm95ObjectSlot, type)      == 0x00, "type");
static_assert(offsetof(Fm95ObjectSlot, pos_x)     == 0x24, "pos_x");
static_assert(offsetof(Fm95ObjectSlot, facing)    == 0x5C, "facing");
static_assert(offsetof(Fm95ObjectSlot, sub_state) == 0x6C, "sub_state");
static_assert(offsetof(Fm95ObjectSlot, sm_timer)  == 0x78, "sm_timer");
static_assert(offsetof(Fm95ObjectSlot, parent_obj_ptr) == 0xA0, "parent");

// ---------------------------------------------------------------------------
// Per-player round state (0x64 = 100 B). The 0x5E98A0 region is an array of 4
// of these + a 16-byte round tail (4*100+16 = 0x1A0). players[1]=P1, [2]=P2.
// Confirmed fields from vs_round_function; unverified spans left as pad/unk.
// ---------------------------------------------------------------------------
struct Fm95PlayerRoundState {
    int32_t  pos_x_snap;        // +0x00
    int32_t  pos_y_snap;        // +0x04
    uint32_t facing_snap;       // +0x08
    uint32_t hp;                // +0x0C  == g_char_max_hp_table => KO
    uint32_t win_counter;       // +0x10  >= round_count_max => match end
    uint8_t  _pad14[0x04];      // +0x14  unverified
    uint32_t meter_cur;         // +0x18
    uint32_t meter_extra;       // +0x1C
    uint8_t  _pad20[0x28];      // +0x20  combo_* region (unverified)
    uint32_t round_intensity;   // +0x48
    uint8_t  _pad4C[0x18];      // +0x4C  incl. +0x60 partner_object (unverified)
};
static_assert(sizeof(Fm95PlayerRoundState) == 0x64, "player block must be 100 B");
static_assert(offsetof(Fm95PlayerRoundState, hp)          == 0x0C, "hp");
static_assert(offsetof(Fm95PlayerRoundState, win_counter) == 0x10, "win_counter");

struct Fm95RoundStateBlock {
    Fm95PlayerRoundState players[4];  // +0x000  [1]=P1 [2]=P2
    uint32_t round_state_var0;        // +0x190
    uint32_t round_time_limit;        // +0x194
    uint32_t round_state_var1;        // +0x198
    uint32_t round_count_max;         // +0x19C  rounds-to-win cap
};
static_assert(sizeof(Fm95RoundStateBlock) == 0x1A0, "round block must be 416 B");
static_assert(offsetof(Fm95RoundStateBlock, round_count_max) == 0x19C, "cap");
// Anchor the mirror to the real addresses the old raw reads used.
static_assert(kRoundStateAddr + offsetof(Fm95RoundStateBlock, players) + 1 * sizeof(Fm95PlayerRoundState)
              + offsetof(Fm95PlayerRoundState, win_counter) == 0x5E9914, "P1 win_counter addr");
static_assert(kRoundStateAddr + offsetof(Fm95RoundStateBlock, players) + 2 * sizeof(Fm95PlayerRoundState)
              + offsetof(Fm95PlayerRoundState, win_counter) == 0x5E9978, "P2 win_counter addr");
static_assert(kRoundStateAddr + offsetof(Fm95RoundStateBlock, round_count_max) == 0x5E9A3C, "cap addr");

// ---------------------------------------------------------------------------
// CSS player slot (16 B @ g_css_players). Verified via disasm (RE-hygiene).
// ---------------------------------------------------------------------------
struct Fm95CssPlayer {
    int32_t  char_cursor;       // +0x00
    uint32_t control_mode;      // +0x04
    uint32_t color_variant;     // +0x08  cycles 0..3
    uint32_t confirmed;         // +0x0C
};
static_assert(sizeof(Fm95CssPlayer) == 0x10, "Fm95CssPlayer must be 16 B");

// --- Accessors (FM95 builds only; unused-and-not-emitted in FM2K) -----------
inline Fm95ObjectSlot* Pool()      { return reinterpret_cast<Fm95ObjectSlot*>(kObjectPoolAddr); }
inline Fm95RoundStateBlock* Round(){ return reinterpret_cast<Fm95RoundStateBlock*>(kRoundStateAddr); }
inline Fm95CssPlayer* CssPlayers() { return reinterpret_cast<Fm95CssPlayer*>(kCssPlayersAddr); }

}  // namespace fm95
