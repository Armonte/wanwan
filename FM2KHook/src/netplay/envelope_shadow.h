// envelope_shadow.h -- ENVELOPE INVERSION PHASE 1: SHADOW MODE.
//
// The instrument that maps every remaining hole in the save-state envelope
// automatically, with ZERO behaviour change. It observes; it never restores.
//
// Method (docs: envelope_inversion_study.md section 6, phase 1): on every
// SaveState_Save, hash the whole writable image (.data, 0x41E000..0x544000) in
// fixed 256-byte blocks. A FORWARD save stores its block-hash vector in a
// per-frame ring; the REPLAY save of the same frame (the discriminator already
// exists -- `is_replay_save` at savestate_fm2k_save.cpp:112) recomputes and
// diffs. A block that differs forward-vs-replay is, by definition, state the
// rollback envelope did not put back. Classifying each such block as inside or
// outside the current save regions splits the result into two piles:
//
//   * OUTSIDE the envelope -> a HOLE (the thing this phase exists to find).
//   * INSIDE the envelope  -> either a deliberate carve-out (the E1..E5 render
//     timers, which MUST show up or the instrument is broken) or a RESTORE BUG,
//     which is loud.
//
// Dark by default: FM2K_ENVELOPE_SHADOW=1 arms it. Armed, it costs one extra
// ~1.2 MB hash (~80 us) plus one ~1.2 MB witness memcpy (~280 us) per forward
// save, so it is a DIAGNOSTIC build posture, the same one FM2K_FULL_CRCS has,
// and never a shipping default.
//
// Sync-logging rules apply (quill is hardcoded off in FM2KHook; every SDL_Log*
// is fputs+fflush): nothing here logs per frame. Counters accumulate in memory
// and the CSV is written only from paths that are already tearing down or are
// between matches -- exactly the seam_trace.{h,cpp} precedent this TU is
// modelled on.
#pragma once
#include <cstdint>

// FM2K_ENVELOPE_SHADOW=1 arms the whole instrument. Strict parse, default OFF,
// unrecognised values fail to OFF with an Error line. One contract line at the
// first call. FM2K_ENVELOPE_SHADOW_WITNESS=0 additionally drops the byte-level
// witness ring (block granularity only, ~20 MB and ~280 us/save cheaper) for
// runs where the per-frame budget matters more than dword resolution.
bool EnvelopeShadow_Enabled();

// One call per SaveState_Save, placed immediately after `is_replay_save` is
// computed and BEFORE the save mutates anything -- forward and replay must
// sample the live image at the identical point of the frame or every block
// differs for a trivial reason.
void EnvelopeShadow_OnSave(int frame, bool is_replay_save);

// Flush the block table to FM2K_P<N>_envshadow.csv plus a one-line summary.
// The summary is unconditional (a dark run still says "0 events", which is the
// evidence that dark means dark); the CSV is written only when armed and
// something was actually compared. Legal from HandleDesyncDetected, the harness
// auto-terminate path, and the between-matches battle-end path -- never mid-match.
void EnvelopeShadow_Dump(int player_index, const char* reason);

// Per-battle-session reset. Invalidates the per-frame hash ring and the witness
// ring (frame numbers restart at every battle, so a stale slot would otherwise
// diff match N's frame 30 against match N+1's) and bumps the match index. The
// accumulated per-block findings deliberately SURVIVE -- the hole list is a
// property of the session, not of one match.
void EnvelopeShadow_Reset();
