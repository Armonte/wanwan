// Combat-state introspection -- see header for what each field means and the
// IDA references used to derive the offsets.

#if !defined(ENGINE_FM95)

#include "combat_state.h"

#include <cstring>

namespace combat_state {

namespace {

// Char-data slot bases. Each slot is 57407 (0xE03F) bytes; slot 0 starts at
// g_character_data_base (0x4D1D80). The "opponent obj ptr" sits at +0xDF69
// inside each slot, written every frame by character_facing_controller as it
// finds the nearest valid type-4 fighter to that slot.
constexpr uintptr_t kSlotBases[2] = {
    0x004D1D80,  // slot 0 -- P1 char_data (its 0xDF69 → P2 obj_ptr)
    0x004DFDBF,  // slot 1 -- P2 char_data (its 0xDF69 → P1 obj_ptr)
};
constexpr uintptr_t kOffsetOpponentObjPtr = 0xDF69;

// Object-layout offsets (byte offsets within the 382-byte ObjectSlot struct
// in g_object_pool). Disasm-verified -- Hex-Rays infers dword alignment but
// some fields here are byte-packed and the engine reads unaligned dwords.
constexpr uintptr_t kObjOffPosX            = 0x08;
constexpr uintptr_t kObjOffPosY            = 0x0C;
constexpr uintptr_t kObjOffHitstopFrames   = 0x40;
constexpr uintptr_t kObjOffPosYGround      = 0x58;
// Hitbox slot array: 20 entries × 4 bytes, UNALIGNED starting at byte 0x89.
// Source: hit_detection_system @ 0x40F050 `mov eax, [edi+eax*4+89h]` --
// the engine loads slot pointers at obj+0x89, obj+0x8D, ..., obj+0xD5.
// The Hex-Rays output's `obj[35]` rounds up to byte 0x8C and is wrong; an
// aligned read at 0x8C straddles slot 0 and slot 1 and produces a spurious
// non-zero dword (slot 0's high byte || slot 1's low 3 bytes), which made
// every "has_active_hitbox" check fire even when nothing was active.
constexpr uintptr_t kObjOffHitboxArray     = 0x89;
constexpr int       kObjHitboxSlotCount    = 20;
constexpr uintptr_t kObjOffFlagsStun       = 0x15E;  // obj+350 -- stun state machine
                                                     // (bits 2,3 used; upper bits unused).

void* ReadOpponentObjForSlot(int slot) {
    // slot=0 (P1) reads P1's char_data + 0xDF69 → P2's obj. slot=1 (P2)
    // reads P2's char_data + 0xDF69 → P1's obj. To get *slot's own* obj we
    // index the OTHER slot's char_data because the opponent-ptr is mutual.
    if (slot < 0 || slot > 1) return nullptr;
    const uintptr_t other_slot_base = kSlotBases[1 - slot];
    return *(void* volatile*)(other_slot_base + kOffsetOpponentObjPtr);
}

}  // namespace

PlayerView ReadPlayer(int slot) {
    PlayerView v{};
    v.valid = false;

    void* obj = ReadOpponentObjForSlot(slot);
    if (!obj) return v;

    // Pointer-bounds sanity. g_object_pool lives at 0x4701E0 and runs for
    // 1024 entries × 382 bytes ≈ 0x5F800. Anything outside [0x4701E0,
    // 0x4701E0 + 0x5F800) is bogus and we bail.
    const uintptr_t addr = (uintptr_t)obj;
    if (addr < 0x004701E0u || addr >= 0x004701E0u + 0x5F800u) {
        return v;
    }

    const auto base = (const volatile uint8_t*)obj;
    v.flags_stun     = *(const volatile uint32_t*)(base + kObjOffFlagsStun);
    v.stun_state     = v.flags_stun & kStunStateMask;
    v.in_blockstun   = (v.stun_state == kStunBlock);
    v.in_hitstun     = (v.stun_state == kStunHit);
    v.actionable     = (v.stun_state == kStunActionable);
    v.pos_x          = *(const volatile int32_t*)(base + kObjOffPosX);
    v.pos_y          = *(const volatile int32_t*)(base + kObjOffPosY);
    v.pos_y_ground   = *(const volatile int32_t*)(base + kObjOffPosYGround);
    v.grounded       = (v.pos_y == v.pos_y_ground);
    v.hitstop_frames = *(const volatile int32_t*)(base + kObjOffHitstopFrames);

    // Hitbox check: iterate the 20 unaligned-dword pointer slots starting at
    // obj+0x89 (stride 4). Any non-null entry means the engine has at least
    // one offensive hitbox to test this frame -- i.e., the character is in
    // an active-frame window of an attack. Memcpy for the read so we don't
    // tickle MinGW's strict-aliasing or undefined-behavior sanitizers on
    // unaligned uint32_t loads.
    v.has_active_hitbox = false;
    for (int i = 0; i < kObjHitboxSlotCount; ++i) {
        uint32_t slot_val;
        std::memcpy(&slot_val,
                    (const void*)(base + kObjOffHitboxArray + (uintptr_t)i * 4),
                    sizeof(slot_val));
        if (slot_val != 0u) { v.has_active_hitbox = true; break; }
    }

    v.valid = true;
    return v;
}

bool ShouldGuardP2() {
    // Kept for back-compat callers -- uses zero P1-input (no attack-press
    // signal). New callers should use GuardP2Input() with the real p1_input.
    return GuardP2Input(0) != 0u;
}

namespace {
// Latched hold-counter shared across BattleInputOverride calls. Reset to
// kGuardHoldFrames any time a trigger condition fires; decremented each
// frame until zero. The latch covers:
//   * the 1-frame input pipeline delay between "see P1 hitbox" and "engine
//     reads P2 input on next frame"
//   * multi-hit moves where the hitbox slot blips null between active hits
//   * attack recovery → next-string-hit window
//
// 12 frames @ 100fps ≈ 120ms -- long enough to bridge typical FM2K stings,
// short enough that P2 doesn't stand pinned in guard forever after a whiff.
constexpr int kGuardHoldFrames = 12;
int s_hold_frames = 0;

// FM2K attack-button bitmask. The 6 attack buttons sit in bits 4-9 (0x3F0).
// Used both for trigger #4 in GuardP2Input and matches PerGamePatches_-
// BattleInputOverride's pre-existing convention.
constexpr uint16_t kAttackBits = 0x3F0;

// Direction bits in the screen-coord input convention used by the binder
// before the facing-fix. FM2K layout (see FM2KInputBinder::Bit):
//   0x001 = LEFT, 0x002 = RIGHT, 0x004 = UP, 0x008 = DOWN.
// NOTE -- bit 2 is UP and bit 3 is DOWN (not the other way around). Earlier
// I had these swapped and the "crouch-block" was actually pressing UP+back,
// making P2 jump backwards every guard trigger.
constexpr uint16_t kInputLeft  = 0x001;
constexpr uint16_t kInputRight = 0x002;
constexpr uint16_t kInputUp    = 0x004;
constexpr uint16_t kInputDown  = 0x008;
}  // namespace

uint16_t GuardP2Input(uint16_t p1_input_screen_coord) {
    const PlayerView p2 = ReadPlayer(1);
    if (!p2.valid) {
        s_hold_frames = 0;
        return 0;
    }

    // Hitstun: P2 can't act, input is moot. Don't latch (the engine's hit
    // reaction will move P2 around; we don't want to follow up with a
    // walk-back input the moment hitstun ends).
    if (p2.in_hitstun) {
        s_hold_frames = 0;
        return 0;
    }

    const PlayerView p1 = ReadPlayer(0);
    const bool p1_in_scripted = p1.valid && ((p1.flags_stun & 0x04u) != 0u);
    const bool p1_attack_press = (p1_input_screen_coord & kAttackBits) != 0u;
    const bool trigger =
        p2.in_blockstun ||
        (p1.valid && p1.has_active_hitbox) ||
        p1_in_scripted ||
        p1_attack_press;
    if (trigger) {
        s_hold_frames = kGuardHoldFrames;
    }

    if (s_hold_frames > 0) {
        --s_hold_frames;

        // Choose stand- vs. crouch-block from the attacker's airborne state:
        //
        //   P1 grounded → crouch-block (DOWN + back). Catches LOW and MID
        //                 attacks via the engine's char_flags-bit-3 path.
        //                 Misses overheads from a grounded P1 -- those need
        //                 per-hitbox flag decode (deferred to Phase C).
        //
        //   P1 airborne → stand-block (back only, no DOWN). Jumping
        //                 attacks descend onto P2's standing hurtbox; the
        //                 stand-block path handles them. A crouching P2
        //                 would whiff the block and eat the jump-in.
        //
        // Re-evaluated every frame -- the latch only governs "should we
        // hold ANY direction?", not which one. So transitions (P1 jumps
        // → lands → ground attack) snap to the right block stance with
        // no extra frame delay.
        //
        // Blockstrings handle themselves automatically: while P2 sits in
        // blockstun (flags_stun & 0xC == 0x8), hit_detection_system skips
        // the input check and auto-blocks subsequent hits. So we only
        // need to nail hit #1 -- the engine carries the rest.
        const bool p1_airborne = p1.valid && !p1.grounded;
        const uint16_t crouch_bit = p1_airborne ? 0u : kInputDown;

        const bool p1_left_of_p2 = p1.valid ? (p1.pos_x < p2.pos_x) : true;
        const uint16_t back_bit = p1_left_of_p2 ? kInputRight : kInputLeft;
        return crouch_bit | back_bit;
    }
    return 0u;
}

}  // namespace combat_state

#else   // ENGINE_FM95 -- different engine, different addresses; stub for now.

#include "combat_state.h"

namespace combat_state {
PlayerView ReadPlayer(int) { return PlayerView{}; }
bool       ShouldGuardP2() { return false; }
}  // namespace combat_state

#endif
