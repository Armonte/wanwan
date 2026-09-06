// Should this Sample_Win32 call pump SDL, or has someone already pumped for
// this frame?
//
// Kept dependency-free (no SDL, no windows.h, no game addresses) so the EXACT
// decision the hook makes can be exercised by a host-compiled unit test --
// the same arrangement delay_math.h and alt_colors_math.h use, and for the
// same reason: the behaviour that matters here only appears with two physical
// pads connected, which no test machine we have can produce.
//
// WHY THIS EXISTS. Sample_Win32 pumped on every call, and it is called once
// per player, so local 2P pumped twice a frame across twice the devices --
// with the solo-driver paths in hooks_getinput calling Sample_Win32(0) again
// on top. Inside a 10 ms budget at 100 Hz that is real work whose cost scales
// with the user's HID stack, and this codebase already shipped one fps
// regression from polling in the input path (#63). The pump is also skipped
// entirely when no pad is open, which is exactly why keyboard-only single
// player feels fine while two pads does not.
//
// Pumping once per frame is not only cheaper, it is more correct: every
// caller within a frame reads the SAME device state instead of a slightly
// different one depending on call order.
#pragma once

#include <cstdint>

namespace fm2k::inpump {

// Per-process state. Deliberately a struct rather than a bare "last frame"
// integer: a sentinel value would be indistinguishable from a real counter
// reading (0xFFFFFFFF is a legal frame counter), which would mean never
// pumping at all on the frame that happened to match.
struct PumpState {
    uint32_t last_frame = 0;
    bool     have_last  = false;
};

// `frame_counter_available` is false where the engine exposes no frame
// counter (FM95 builds, or a context whose counter does not advance). There
// the honest answer is to pump every time -- the same behaviour as before
// this throttle existed -- rather than silently stop refreshing input.
inline bool ShouldPump(PumpState& st, uint32_t frame,
                       bool frame_counter_available) {
    if (!frame_counter_available) return true;
    if (st.have_last && st.last_frame == frame) return false;
    st.last_frame = frame;
    st.have_last  = true;
    return true;
}

}  // namespace fm2k::inpump
