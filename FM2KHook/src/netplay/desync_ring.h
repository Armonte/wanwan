#pragma once

#include <cstdint>

// The cross-peer state of the last few thousand frames, kept in memory and
// written out ONLY when GekkoNet reports a desync.
//
// WHY A RING AND NOT A LOG LINE. Hook logging is SYNCHRONOUS -- quill is
// hardcoded off in FM2KHook, so every line is a blocking write on the sim
// thread. The hub-path desync reproduced on the FIRST run, twice, with
// byte-identical checksums; after the cross-peer fields were added to the
// per-frame [CHECKSUM] line it stopped reproducing in 12 consecutive runs.
// Nothing about game logic changed -- only the size of a per-frame blocking
// write. The divergence is timing-sensitive and watching it that way masks
// it, which is very likely why this class outlived every earlier attempt to
// look at it more closely.
//
// So: a few stores per frame into a fixed buffer, no formatting, no I/O.
// The dump happens once, at the moment of the desync, and still carries the
// frames leading into it -- which is all the attribution needs. Same pattern
// frame_timing.cpp and the seam ring already use.
//
// WHAT IT RECORDS is the CROSS-PEER term and only that: the four fields
// wire_fingerprint hashes (rng, p1_hp, p2_hp, round_number). Those are what
// two machines are required to agree on. The [CHECKSUM] crc and its region
// checksums are a host-vs-SPECTATOR instrument and differ between players in
// runs that are perfectly healthy -- comparing them peer-to-peer measures
// nothing (measured: a clean run shows crc differing at f=1).
namespace fm2k::desync_ring {

// One store per gekko Save. Cheap by construction: no formatting, no locks,
// no allocation.
void Note(int32_t frame, int32_t confirmed_frame);

// Write the ring to <game dir>/logs/FM2K_P<n>_desyncring.csv and log the
// path. Called from the desync handler; safe to call more than once (each
// call writes a fresh file with a sequence suffix so a second desync in the
// same session cannot overwrite the first one's evidence).
void Dump(const char* reason, int32_t desync_frame);

}  // namespace fm2k::desync_ring
