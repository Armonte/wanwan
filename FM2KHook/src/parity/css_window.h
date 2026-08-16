/* SPDX-License-Identifier: Apache-2.0 */
/* [CSS-WIN] / [CSS-OBJ] -- the character-select window's object-pool gate.
 *
 * WHY THIS EXISTS
 * ---------------
 * Every correctness gate in the tree is BATTLE-ONLY. CINPUT, the [CHECKSUM]
 * full-state fencepost (and its 4c nobj=/top=/bind= pool terms), parity_diff
 * and tools/test_css_gate.py all filter to `match_phase == 3000` segments;
 * [CSS-FP] covers the cursor and the selection, not the object pool. So the
 * whole character-select window between two matches has never been measured
 * on either plane. Phase 4c measured it by hand and found it materially
 * divergent host-vs-spectator (25-29% of frames carry a different active-slot
 * map, the spectator holding an extra type-1 object the host does not), with a
 * visible consequence: the owner-reported "player objects fall down on the
 * character-select screen" bug. From the harness .pty captures of run V1r6
 * (vanguard-princess, spectator S1, window 11220-12270, `match_phase == 2000`
 * throughout):
 *
 *     idx      p2.pos_y   note
 *     11220     535.0     resting, identical to the host
 *     12240     545.5     fall starts, 29 frames before battle entry
 *     12260     805.0
 *     12267     920.9     peak -- 385 px below the host's resting position
 *     12268       --      object gone; battle entry follows
 *
 * The host caps at 535.0 px in EVERY window of every run; a clean spectator
 * window holds exactly two distinct values (480.0 / 535.0). The broken window
 * holds 98. The acceleration profile is gravity.
 *
 * WHAT THIS EMITS (both planes, same call site, same cadence)
 * ----------------------------------------------------------
 *   [CSS-WIN] ... ev=open      once, when match_phase enters 2000
 *   [CSS-WIN] ... i=<k> ...    every kEmitPeriod frames inside the window:
 *                              population (nobj=), the process-independent
 *                              slot->type map digest (map=), the binding
 *                              digest (bind=), and each player object's
 *                              resolved slot + pos_y (the falling signature)
 *   [CSS-WIN] ... ev=close     once, when the window ends: frame count and the
 *                              per-player MAXIMUM pos_y observed in the window
 *   [CSS-OBJ] ... why=fall     at most once per window, the first frame a
 *                              player object crosses the fall threshold
 *   [CSS-OBJ] ... why=close    at most once per window, the last in-window
 *                              sample, so a window that did NOT trip still
 *                              leaves a full slot listing to diff against
 *
 * [CSS-OBJ] is the DIAGNOSIS half: it names the falling object's identity
 * (slot, type, owner, script id, player-slot binding, entity kind, position)
 * so "present on one plane and not the other" is answerable offline. It is
 * chunked with an explicit part=k/n rather than silently truncated (the
 * DumpTopoDetail truncation defect, seam_p4c_review A4f).
 *
 * COST AND SAFETY
 * ---------------
 * - Entirely gated on FM2K_CSS_WIN=1; unset, OnCapture() returns after one
 *   cached env read and touches no memory.
 * - FM2K only. Under ENGINE_FM95 every entry point is a no-op: the pool
 *   literals below are WonderfulWorld_ver_0946.exe addresses (shared with
 *   parity_pool.h) and FM95's unrelated 0x426A40 pool must never be scanned
 *   through them.
 * - No per-frame file IO. Hook logging is SYNCHRONOUS (quill is hardcoded off
 *   in FM2KHook), so the periodic emit is 1 line / 30 frames and the detail
 *   dump is <= 2 events / window. The per-frame work is a fixed-size scan into
 *   a static buffer -- no allocation, no formatting, no syscall.
 * - The character-select screen is not a hot path; the battle loop never
 *   reaches any of this (the phase gate is exact).
 *
 * Call site: ParityRecorder::Capture(), after the .pty snapshot's player
 * resolution, so both planes sample at the same point in the frame and the
 * emitted values are the same quantities the .pty already records. That also
 * means the emitter only runs when the parity recorder is open, which in the
 * harness is always (FM2K_PARITY_RECORD_PATH is set for every process). The
 * harness treats "no [CSS-WIN] lines" as NOT COMPUTED, never as agreement.
 */
#pragma once

#include <cstdint>

namespace CssWindow {

/* True when FM2K_CSS_WIN is set to anything other than 0 AND this is an FM2K
 * build. Exposed so a caller can say "off" instead of printing zeros. */
bool Enabled();

/* One post-update sample. `match_phase` is g_game_mode (0x470054): 2000 is the
 * character-select / results window, 3000 is battle. Slots are the values
 * ParityPool::FindPlayerObjectSlot returned (-1 = no character object), pos_y
 * is the raw 16.16 fixed-point +0x0C field. Everything else is derived here. */
void OnCapture(uint32_t seq, int32_t match_phase,
               int p1_slot, int32_t p1_pos_y, int32_t p1_script,
               int p2_slot, int32_t p2_pos_y, int32_t p2_script);

}  /* namespace CssWindow */
