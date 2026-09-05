#pragma once
// scene_frame_guard -- the 0x40DD62 access violation: a DESTROYED object is
// rendered one last time against a scene table that was freed and reloaded
// underneath it.
//
// THE ENGINE MECHANISM (IDA, WonderfulWorld_ver_0946; identical .text in
// every FM2K title we ship):
//
//   sprite_rendering_engine @0x40CC30 draws the object at g_current_object
//   @0x4CFA00 when obj[+0x10] == -1. It picks a "scene block" from the
//   object's KIND at obj[+0x15A]:
//       0,1,5 -> 0x4D1D80 + 0xE03F*obj[+0x156]   (per-character data)
//       2     -> 0x433240                        (.kgt game-system block)
//       3     -> 0x425A60                        (.demo scene block)
//       4     -> 0x445740                        (.stage block)
//   then computes the 16-byte frame descriptor
//       ebx = block[+0x114] + 16*obj[+0x2C] - 16
//   and reads it at +3 (sprite id, 0x40CCAE) and +7 (y offset, 0x40DD62).
//   block[+0x114] is a GlobalAlloc'd array of 16-byte frame descriptors,
//   block[+0x110] the 39-byte animation table, block[+0x2228] its entry
//   count. The LAST animation entry is a sentinel whose start frame
//   (u16 at +32) is the frame count, so the valid range of obj[+0x2C] is
//   1 .. frames (it is a 1-based index -- hence the -16).
//
//   NOTHING bounds-checks obj[+0x2C]. An index past the end reads whatever
//   follows the heap block, which is usually harmless garbage -- and faults
//   only when the descriptor happens to straddle the end of a committed
//   page. That is why the crash is intermittent, and why the SAME bug was
//   already being papered over at 0x40CCAE by VectoredRenderGuard in
//   dllmain_logging.cpp long before 0.2.84. 0x40DD62 is the same bad
//   descriptor, faulting eight bytes later.
//
// HOW A STALE INDEX HAPPENS (the cause):
//
//   update_game_state @0x404CD0 rebuilds the 128 update/render buckets from
//   the object pool ONCE, at the top of the frame, and render_game @0x404DD0
//   then walks THOSE SAME buckets -- skipping only objects whose type word
//   obj[+0] is 0. Every scene-change script does, in one call:
//       ResetObjectsAndCalculateSpeed()   // obj[+0] = 1 on every other object
//       demo_scene_loader(id)             // GlobalFree + reload block 0x425A60
//       demo_spawn_scene_objects()        // spawn the new kind-3 objects
//   Type 1 is Obj_ReapMarkedSlot @0x4069A0, a handler that does nothing but
//   `obj[+0] = 0`. So the objects killed in step 1 are DEAD, but they are
//   still on this frame's render list, they still carry their old kind and
//   their old frame index, and the table those index into was freed and
//   replaced in step 2. Any of them that sat EARLIER in the bucket order
//   than the script's own object never gets its reaper call this frame, and
//   render_game draws it against the new, possibly shorter, table.
//
// THE FIX: render_game must not draw destroyed objects. Suppressing the
// draw of a type-1 object touches no simulation state (RenderGame already
// brackets the render with an RNG save/restore, and the only object fields
// sprite_rendering_engine writes are the render colour regs of the object
// being drawn, which is about to be freed and memset anyway), so it cannot
// move the sim -- unlike reaping the pool slot early, which would shift
// create_game_object's first-free scan for the objects spawned in step 3.
//
// THE DETECTOR (FM2K_FRAMEIDX_DETECT=1, dark by default) validates the
// index against the table BEFORE the engine reads it and reports every
// out-of-range draw, faulting or not -- which is the number nobody had.
#include <cstdint>

namespace SceneFrameGuard {

// FM2K_FRAMEIDX_DETECT=1 -- bounds-validate every sprite draw and report.
bool DetectorArmed();

// Called from Hook_SpriteRenderingEngine with g_current_object already set.
// Returns true when the ORIGINAL MUST BE SKIPPED for this object, i.e. the
// object is destroyed (type 1) or its kind selects no scene block at all.
// Runs the detector first when armed.
bool ShouldSuppressDraw();

// One line per counter at teardown; no-op when nothing was suppressed and
// the detector never armed.
void ReportSummary();

}  // namespace SceneFrameGuard
