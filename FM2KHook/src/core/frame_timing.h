// frame_timing.h -- PER-FRAME TIMING RING for the "the game feels sluggish"
// class. OBSERVE ONLY: nothing here writes game memory, changes pacing, or
// alters a single sim decision.
//
// THE QUESTION IT EXISTS TO ANSWER. Field logs prove the achieved rate sags to
// 85-96 fps against a 10 ms budget and that BOTH peers sag together. Two
// causes produce that identical signature and want opposite fixes:
//
//   * a SLOW BOX -- the frame's own work (sim + render) does not fit in 10 ms,
//     so the pacer has nothing left to pad and the loop free-runs at whatever
//     the machine manages. SleepToTarget can only pad UP TO the target, never
//     claw back time already spent.
//   * a SLOW CONNECTION -- the work fits easily, but gekko emits no Advance
//     (Netplay_ProcessBattleInputPhase returns false) because it has predicted
//     `input_prediction_window` frames with no real input (CanPredictInput,
//     GekkoLib/src/input.cpp), or the drift brake is lengthening the frame on
//     purpose. Braking ONE peer slows BOTH -- the other can only advance as
//     fast as inputs arrive -- which is exactly why both peers always sag
//     together and why that fact alone has never told us which box was at
//     fault.
//
// The two are trivially separable ONCE THE FRAME IS SPLIT: on a slow box
// sim+render fills the budget and `sleep` goes to zero; on a slow connection
// sim+render is small, `sleep` is large, and the stalls and the brake carry
// the loss. Nothing before this could make that split, which is why every
// downstream question was blocked.
//
// PERCENTILES, NOT AVERAGES. A mean frame time hides precisely the events
// players complain about: 297 good frames and 3 catastrophic ones average to
// "fine". The summary reports p50/p99/p100 plus the count over the 10 ms
// budget, and separately the STALL count and the STALL RUNS, because a stall
// is a DROPPED frame and is worse than a slow one -- five stalls in a row is
// a 50 ms freeze, and it averages away to nothing.
//
// WHAT ONE ROW CARRIES (see Row in frame_timing.cpp for the exact fields):
// the frame's own wall-clock duration, work (sim + render +
// everything else before the pacing sleep), sim, render, sleep, how many sim
// steps ran (the heavy-stage catch-up can run up to 10), whether the frame
// ADVANCED or STALLED, the rollback count and worst rewind depth attributed
// to that frame, frames_ahead, and the brake in microseconds.
//
// TIMES ARE STORED AS RAW QPC TICKS and converted at dump time from the
// frequency written into the CSV header. That keeps the per-frame cost to a
// subtraction and a store -- no 64-bit divide, no floating point -- on an
// instrument whose entire value depends on not perturbing the thing it
// measures.
//
// DARK BY DEFAULT: FM2K_FRAME_TIMING=1 arms it. Unarmed, every entry point is
// one cached bool test and no memory is allocated at all. Armed, a frame costs
// two extra QueryPerformanceCounter reads and one 44-byte store, well under a
// microsecond against a 10000 us budget.
//
// NEVER LOG PER FRAME. Hook logging is SYNCHRONOUS (quill is deliberately off
// in FM2KHook -- its backend thread deadlocks the loader), so a per-frame line
// would BE the stutter this instrument exists to find. Rows accumulate in
// memory; the CSV is written only at match end (the player plane's
// Netplay_EndBattle and the viewer plane's own battle-exit edge -- the viewer
// reaches neither of the other two sites, so without its own it would be
// killed holding a whole session's rows), at WM_QUIT, from the harness
// auto-terminate path, and on the explicit F7 request -- and the F7 dump
// MEASURES AND REPORTS ITS OWN COST, because a diagnostic that hitches the
// frame it is diagnosing has to say so rather than be quietly believed.
#pragma once
#include <cstdint>

// Which loop phase produced the row. Kept distinct rather than folded into one
// "battle" value: an offline battle frame has no gekko and therefore cannot
// stall, so counting its always-true `advanced` alongside a netplay frame's
// would dilute the stall rate that is the whole point.
enum class FtPhase : uint8_t {
    BATTLE_NET = 0,   // netplay battle -- the only phase where STALL is meaningful
    BATTLE_OFFLINE,   // offline battle (frame-skip catch-up path)
    BATTLE_STRESS,    // stress/determinism session
    CSS,
    NATIVE,
    SPECTATOR,
};

// FM2K_FRAME_TIMING=1 arms the ring. Strict parse, default OFF; exactly one
// contract line at the first call, so an armed run says so in its own log and
// a dark run can never be mistaken for an armed one that found nothing.
bool FrameTiming_Enabled();

// Outer-loop bookends (main_loop_trampoline.cpp), i.e. TrampolineMainLoop --
// which both engines reach, FM95 through Fm95OwningLoopEntry. The WIP
// TrampolineFrameTickHostDriven path (CPW's own WinMain driving the tick)
// opens no frame, so every Note* below no-ops there rather than writing rows
// with no bookends. Stated because a silently empty CSV on that path would
// otherwise read as "nothing to see".
// BeginFrame takes the SAME
// QPC anchor the pacer uses, so `frame` here and the pacing deadline cannot
// drift apart. EndWork is called after the phase tick and BEFORE the pacing
// sleep; EndFrame after it, which is what makes `sleep` a measurement rather
// than an inference.
void FrameTiming_BeginFrame(uint64_t tick_start_qpc);
void FrameTiming_EndWork();
void FrameTiming_EndFrame();

// Phase tick contributions. Each Run*Tick names its phase at the top (so an
// early return still produces a correctly-labelled row) and reports the two
// spans it owns. NoteSim accumulates, so the heavy-stage catch-up loop can
// call it once per step or once for the batch.
void FrameTiming_NotePhase(FtPhase phase);
void FrameTiming_NoteSim(uint64_t ticks, int steps);
void FrameTiming_NoteRender(uint64_t ticks);

// A battle tick that asked gekko for an Advance and did not get one. This is
// the single most important event in the file: it is a DROPPED frame, and it
// is what a prediction-window exhaustion looks like from the game's side.
void FrameTiming_NoteStall();

// One REAL (non-runahead) rollback, with how far back it rewound. Called from
// Netplay_HandleLoadEvent, which already distinguishes the runahead rewind
// from a network rollback -- this must not re-derive that.
void FrameTiming_NoteRollback(uint32_t rewind_depth);

// On-demand dump. RequestDump is callable from the WndProc thread (F7); Poll
// runs the actual write on the trampoline thread, exactly like the F8 runahead
// toggle's cross-thread handoff, so nothing touches the ring off-thread.
void FrameTiming_RequestDump();
void FrameTiming_PollDump(int player_index);

// Bump the match counter, so rows carry which match they belong to and the
// analyser can segment a multi-match session without inferring boundaries from
// a battle frame counter that also resets on a rematch. Called at the battle
// teardown, next to the match-end dump.
void FrameTiming_NoteMatchBoundary();

// Write FM2K_P<N>_frametiming.csv plus the percentile summary. The summary is
// unconditional (a dark run says "0 rows", which is the evidence that dark
// means dark); the CSV is written only when rows exist. Rows are KEPT across a
// dump and the file is rewritten whole, so a multi-match session accumulates
// rather than losing everything that happened before the last match end.
void FrameTiming_Dump(int player_index, const char* reason);
