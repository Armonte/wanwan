// fpk_reader.h -- portable .fpk (FM2K asset pack) -> original-format reconstructor.
//
// Pure C++ runtime model of tools/kgt/fpk.py reconstruct(): inflates a slim .fpk
// back into the exact byte stream FM2K's player_data_file_loader would read for a
// .player/.stage/.demo. The non-audio regions (header, scripts, script_items,
// sprite headers, sprite contents, palettes, tail) are byte-identical to the
// original; audio is lossy (Opus) and only structurally equivalent.
//
// No windows.h -- this is the portable core shared by the i686 DLL and the native
// test harness. Depends only on the standard library + zstd (decompress) and,
// when FPK_WITH_OPUS is defined, opusfile for codec==1 audio.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Reconstruct the original-format byte stream from a packed .fpk image.
// Returns the reconstructed bytes on success. On failure returns an empty
// vector and, if `err` is non-null, sets it to a human-readable reason.
std::vector<uint8_t> fpk_reconstruct(const uint8_t* fpk, size_t len, std::string* err);
