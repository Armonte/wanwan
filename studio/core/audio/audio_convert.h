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

// Tiered engine-truth verdict for a WAV blob -- the chunk walk of
// tools/kgt/soundtool.py walk_wav (itself a mirror of the engine's
// ParseRIFFWaveFormat @0x416043), with the tiers kept exactly:
//   FATAL -- the walk itself breaks: not RIFF/WAVE, RIFF size field
//   overstating the buffer, fmt chunk under 14 bytes, missing fmt/data,
//   data chunk past the RIFF bound. The loader mis-reads these in-game.
//   WARN  -- walk passes but the format is outside the classic envelope
//   (fmt tag != 1, bits not 8/16, channels not 1-2). The official editor
//   imports WAVs with zero validation (AddSoundEntry @0x4336E0 is a raw
//   file slurp) and the engine hands fmt RAW to CreateSoundBuffer
//   @0x415C20; a legacy DirectSound stack that rejects the format leaves
//   a NULL buffer stored unchecked and the first script play crashes
//   (StopAllSoundsInBufferArray @0x415F19). Modern Windows (WASAPI-
//   wrapped dsound) plays these fine -- hence WARN, not FATAL.
// DecodeEngineWav stays strict (refuses WARN formats); ProbeWav is the
// classifier the UI tiers ('!' column) and preview fallback key off.
struct WavValidity {
    std::vector<std::string> fatal;     // red tier
    std::vector<std::string> warnings;  // yellow tier
    // fmt fields as walked (meaningful when have_fmt).
    bool have_fmt = false;
    bool have_data = false;
    uint16_t format_tag = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    int bits = -1;                      // -1 = fmt chunk too short to carry bits
    uint32_t data_size = 0;             // first data chunk's size field

    bool Fatal() const { return !fatal.empty(); }
    bool Warn() const { return fatal.empty() && !warnings.empty(); }
    bool Ok() const { return fatal.empty() && warnings.empty(); }
};
WavValidity ProbeWav(const uint8_t* data, size_t len);

// "PCM 22050Hz 16-bit mono" / "tag=3 44100Hz 32-bit stereo" from a probe
// (the CLI's fmt_desc) -- format cell text when DecodeEngineWav refused.
std::string ProbeDesc(const WavValidity& v);

// Resample/fold/convert to the target format. Any field 0 = keep source.
PcmAudio Normalize(const PcmAudio& in, uint32_t rate, uint16_t channels, uint16_t bits);

// Minimal canonical RIFF/WAVE (44B header + data) passing the engine walker.
std::vector<uint8_t> EncodeWav(const PcmAudio& a);

// RMS in dBFS (0 = full scale) -- for old-vs-new loudness comparison.
double RmsDbfs(const PcmAudio& a);

}  // namespace kgt
