// audio_convert.h -- decode any common audio file and convert it to an
// engine-safe PCM WAV for FM2K sound slots.
//
// Decode: WAV (dr_wav), MP3 (dr_mp3), OGG (stb_vorbis), FLAC (dr_flac)
// -- single-header libs vendored under studio/third_party/.
// Convert: resample (32.32 fixed-point linear, same approach as
// FM2KHook/src/vfs/fpk_reader.cpp opus_to_pcm_payload), channel fold
// (mono<->stereo), bit depth (u8 <-> s16).
//
// Engine constraints (ParseRIFFWaveFormat @0x416043, WW 0946): the
// output WAV must be RIFF/WAVE with a truthful RIFF size, a 16-byte PCM
// fmt chunk, and a data chunk; PCM 8/16-bit, 1-2 channels. EncodeWav
// writes the minimal canonical 44-byte-header WAV satisfying all of it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kgt {

struct PcmAudio {
    uint16_t channels = 0;   // 1 or 2 after Normalize
    uint16_t bits = 0;       // 8 (unsigned) or 16 (signed LE)
    uint32_t rate = 0;
    std::vector<uint8_t> pcm;  // interleaved frames

    size_t FrameCount() const {
        size_t block = size_t(channels) * (bits / 8);
        return block ? pcm.size() / block : 0;
    }
    double Seconds() const { return rate ? double(FrameCount()) / rate : 0.0; }
};

// Sniff + decode a file's bytes (wav/mp3/ogg/flac). False + *err on failure.
// Result is 16-bit; callers convert depth via Normalize if needed.
bool DecodeAudio(const uint8_t* data, size_t len, PcmAudio* out, std::string* err);

// Parse an engine WAV blob (from an existing sound slot) into PcmAudio
// for playback/RMS. Accepts what the engine accepts (proper chunk walk).
bool DecodeEngineWav(const uint8_t* data, size_t len, PcmAudio* out, std::string* err);

// Resample/fold/convert to the target format. Any field 0 = keep source.
PcmAudio Normalize(const PcmAudio& in, uint32_t rate, uint16_t channels, uint16_t bits);

// Minimal canonical RIFF/WAVE (44B header + data) passing the engine walker.
std::vector<uint8_t> EncodeWav(const PcmAudio& a);

// RMS in dBFS (0 = full scale) -- for old-vs-new loudness comparison.
double RmsDbfs(const PcmAudio& a);

}  // namespace kgt
