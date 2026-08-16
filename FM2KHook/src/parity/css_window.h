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
 * PLUS, under its OWN env gate FM2K_CSS_ANIM=1 (dark by default, independent of
 * FM2K_CSS_WIN):
 *   [CSS-ANIM] ... ev=sum      once per window: how many type-4 objects ADVANCED
 *                              their script program counter and how many stayed
 *                              frozen, plus how many were parked
 *   [CSS-ANIM] ... ev=rec      chunked per-object detail (identity + advance
 *                              counts), so object CLASSES separate offline
 * This answers Wave-2 review finding B1e -- the spectator character-select park
 * (spec_css_park.cpp) writes script_init_state = 2 on EVERY type-4 slot for the
 * whole window, and no other term in the tree can see whether that freezes the
 * character previews the spectator is supposed to be watching. Rationale, field
 * list and cost are in-source at the [CSS-ANIM] block in css_window.cpp.
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

/* ---- [BATTLE-OBJ] -- the battle-frame object census ---------------------
 *
 * WHY: the harness's player-vs-player [CHECKSUM] terms can now SAY that the
 * two peers' pools differ, but not WHAT differs. On vanguard-princess the
 * guest enters battle carrying exactly two more live objects than the host,
 * from battle frame 1 onward, in every desyncing session and no clean one --
 * and the pools are byte-identical at character-select close AND at battle
 * frame 0, so the two extra objects are created inside battle initialisation,
 * between frame 0 and frame 1. This dumps the full slot listing
 * (slot:type:owner:script:pslot:kind:x:y, the same fields and the same chunked
 * part=k/n format [CSS-OBJ] uses) on BOTH players at both of those frames, so
 * an offline diff names the objects.
 *
 * COST AND SAFETY: gated on FM2K_BATTLE_F1; unset, OnBattleFrame() returns
 * after one cached env read. Enabled, it is at most TWO dump events per battle
 * per process -- latched, so a rollback re-sim of frames 0/1 does not repeat
 * it. FM2K only (ENGINE_FM95 no-ops, same reason as the CSS window). Deliberately
 * a SEPARATE env var from FM2K_CSS_WIN, which is host-only by Wave-2 review B2;
 * this one must run on both players or it measures nothing.
 *
 * Alongside each census it emits ONE [BATTLE-CFG] line pairing the LATCHED sim
 * values (g_round_limit 0x470048, g_score_value 0x470050, g_active_stage_id
 * 0x470040, g_game_mode_flag 0x470058) with the CONFIG SOURCES they were
 * derived from (0x430124 / 0x430114 / 0x43010C). Rationale in-source: the
 * battle-entry settings barrier hashes the sources, the sim consumes the
 * latches, and the round-limit latch happens one substate BEFORE the
 * game_mode -> 3000 write the barrier is armed on, so "source agrees, latch
 * does not" is a reachable state that no existing line can show.
 *
 * Call site: SaveState_Save(frame), BEFORE its frame<0 clamp -- the save
 * event, i.e. the exact instant and the exact live pool the fingerprint (and
 * the nobj= the harness compares P1-vs-P2) is computed over. It sits there
 * rather than at the [CHECKSUM] emitter in netplay_battle_events.cpp only
 * because that TU is at the 1000-line ceiling; same frame, same pool, same
 * event. `frame` is the save frame (-1 = battle-entry marker, which re-arms). */
bool CensusEnabled();
void OnBattleFrame(int frame);

}  /* namespace CssWindow */
