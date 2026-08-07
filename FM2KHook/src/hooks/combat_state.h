#pragma once

// Combat-state introspection -- typed accessors over the live game object /
// char_data fields we use to drive smarter training-mode behaviors. Read-only
// helpers; nothing here writes to game memory. Always safe to call (returns a
// neutral value if the underlying pointers are null / unset, e.g. pre-battle).
//
// Built from analysis of hit_detection_system (0x40f010), health_damage_-
// manager (0x40e7c0), and character_facing_controller (0x40e5c0). See
// docs/analysis/per_game_patches.md (Combat State Map section) for the full
// field layout and provenance.
//
// Used initially by the state-driven Guard mode in per_game_patches.cpp; the
// longer-term goal is to feed an ETM-style reversal/dummy system (see plan
// in conversation history) once we add edge detection on top.

#include <cstdint>

namespace combat_state {

// Stun-state masks within obj+0x15E (a.k.a. `flags_stun`, set by
// hit_detection_system and reset by character_action_controller):
//   bit 2 (=0x04): in scripted action (attack animation, hit reaction, etc.)
//   bit 3 (=0x08): in defensive/stun state (block or hit)
//   bit 4 (=0x10): "already hit this attack" -- attacker-side filter
// Combinations we care about (mask the value with 0xC):
//   0x0  → actionable / idle
//   0x4  → in scripted action (NOT stunned -- e.g., attacking)
//   0x8  → blockstun
//   0xC  → hitstun
constexpr uint32_t kStunStateMask  = 0x0C;
constexpr uint32_t kStunActionable = 0x00;
constexpr uint32_t kStunScripted   = 0x04;
constexpr uint32_t kStunBlock      = 0x08;
constexpr uint32_t kStunHit        = 0x0C;

// Per-player view bundled for one accessor call. Reads are atomic w.r.t. the
// game thread (single-frame snapshot) -- caller should treat as a value type.
struct PlayerView {
    bool valid;            // false if obj_ptr was null (pre-battle / between rounds)
    uint32_t flags_stun;   // raw obj+0x15E value
    uint32_t stun_state;   // flags_stun & 0xC, one of kStun*
    bool in_blockstun;     // stun_state == kStunBlock
    bool in_hitstun;       // stun_state == kStunHit
    bool actionable;       // stun_state == kStunActionable
    bool has_active_hitbox;// any non-null pointer in obj+0x8C..0xD8 (20 slots)
    int32_t pos_x;         // obj+0x08 raw (16.16 fixed point)
    int32_t pos_y;         // obj+0x0C
    int32_t pos_y_ground;  // obj+0x58 -- equals pos_y when grounded
    bool grounded;         // pos_y == pos_y_ground
    int32_t hitstop_frames;// obj+0x40 -- count of frozen frames remaining
};

// Read a snapshot of either side. slot is 0=P1, 1=P2. Returns a PlayerView
// with .valid=false if no opponent ptr has been written yet (e.g., title
// screen, CSS). Safe to call any time.
//
// Implementation reads char_data[1-slot] + 0xDF69 for slot's obj_ptr (P1's
// opponent ptr is P2, P2's is P1). This avoids scanning g_object_pool per
// frame.
PlayerView ReadPlayer(int slot);

// Convenience predicate: should a reactive Guard hold back this frame?
// Returns true when ANY of:
//   - P2 is currently in blockstun (continuing a string)
//   - P1 (the opponent) has an active hitbox (a hit is incoming OR landing)
// Returns false during hitstun (P2 can't act anyway), idle frames, or when
// player snapshots are invalid.
bool ShouldGuardP2();

// Compute the actual input bits P2 should produce this frame for a state-
// driven Guard. Returns 0 (no input), back direction alone (stand-block),
// or DOWN + back direction (crouch-block). FM2K layout: bit 0 = LEFT,
// bit 1 = RIGHT, bit 2 = UP, bit 3 = DOWN.
//
// Block stance is chosen per-frame from the attacker's airborne state:
//   * P1 airborne (jump-in incoming) → stand-block (back only). A crouching
//     P2 misses jumping attacks; standing puts the standing hurtbox in
//     range and the engine's stand-block path handles the block.
//   * P1 grounded → crouch-block (DOWN + back). Crouching sets char_flags
//     bit 3 at char_data+0x7CB6, which routes hit_detection_system into
//     the crouching-block path covering LOW and MID attacks.
//
// Limitation: overheads from a grounded P1 still slip through (need to
// crouch-block lows but stand-block overheads -- disambiguation requires
// hit-flag decode work, deferred). Acceptable default since FM2K games
// rarely use grounded overheads.
//
// Blockstrings: the engine auto-blocks during blockstun. We only need to
// land hit #1 correctly; subsequent hits in a string block themselves.
//
// The back-direction bit is selected based on relative P1/P2 X-positions
// so Guard survives a side switch (cross-up etc). The returned screen-
// coord value goes through Hook_GetPlayerInput's facing-fix, which swaps
// LEFT/RIGHT based on char_flags so the engine ultimately sees the back-
// relative input regardless of which side P2 is on.
//
// Internally latches a `hold_frames` counter for ~12 frames after the last
// trigger so:
//   * P2 doesn't microwalk between active frames of a multi-hit move
//   * P2 keeps blocking through attack recovery so the next gatling/string
//     hit also blocks
//   * the 1-frame state-detection pipeline delay (we see P1's active
//     hitbox AFTER it landed) doesn't cost us blocks on fast moves
//
// Triggers (any one refreshes the latch):
//   1. P2 is currently in blockstun (string continuation)
//   2. P1 has an active hitbox (the strict state signal)
//   3. P1 is in any scripted action (flags_stun & 4 -- startup coverage)
//   4. P1's CURRENT input has any attack button pressed (anticipates the
//      hitbox by 3+ frames since button press leads active frames; this
//      is what makes the Guard actually fast enough on jab-speed attacks)
//
// `p1_input_screen_coord` is the 11-bit screen-coordinate input value
// sampled from P1's bindings before the facing-fix -- same value passed to
// PerGamePatches_BattleInputOverride. Used for trigger #4.
uint16_t GuardP2Input(uint16_t p1_input_screen_coord);

}  // namespace combat_state
