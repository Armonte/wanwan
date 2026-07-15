// audio_convert.cpp -- see audio_convert.h.
//
// Decode: magic-sniffed dispatch to the vendored single-header decoders
// (studio/third_party/, versions pinned in its README.md). All decoders
// emit 16-bit interleaved PCM; DecodeAudio never yields 8-bit.
//
// Engine WAV parsing mirrors tools/kgt/soundtool.py walk_wav (itself a
// mirror of ParseRIFFWaveFormat @0x416043): the RIFF size field is the
// walk bound, chunks are 2-byte aligned, first fmt/first data win. We
// REJECT what walk_wav flags fatal (overstated RIFF size, data past the
// bound) because the engine would read out of bounds on those in-game.
//
// Resampling is the 32.32 fixed-point linear interpolator from
// FM2KHook/src/vfs/fpk_reader.cpp opus_to_pcm_payload: one 64-bit
// mul+shift per output sample, driven by OUTPUT frame index, so cost
// scales with the target rate. Channel folding matches it too
// (fold-to-mono = average, to-stereo = take/duplicate first two).

#include "audio_convert.h"

#include <cmath>
#include <cstring>

// ── vendored decoders (implementations live in this TU only) ────────────
// Third-party code is not warning-clean under -Wall -Wextra; scope the
// noise to these includes so our own code still builds clean.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "../../third_party/dr_wav.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../../third_party/dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "../../third_party/dr_flac.h"

#define STB_VORBIS_NO_STDIO
#include "../../third_party/stb_vorbis.c"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace kgt {

namespace {

bool fail(std::string* err, const char* msg) {
    if (err) *err = msg;
    return false;
}

uint16_t rd_u16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

uint32_t rd_u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// Move decoded s16 frames into a PcmAudio (bits is always 16 here; the
// 8-bit path only exists for engine WAVs, which never go through the
// decoders).
void take_s16(PcmAudio* out, const int16_t* pcm, size_t frames,
              uint16_t channels, uint32_t rate) {
    out->channels = channels;
    out->bits = 16;
    out->rate = rate;
    out->pcm.assign(reinterpret_cast<const uint8_t*>(pcm),
                    reinterpret_cast<const uint8_t*>(pcm) +
                        frames * channels * sizeof(int16_t));
}

bool decode_wav(const uint8_t* data, size_t len, PcmAudio* out,
                std::string* err) {
    drwav wav;
    if (!drwav_init_memory(&wav, data, len, nullptr))
        return fail(err, "wav: unreadable or unsupported RIFF/WAVE");
    if (wav.channels < 1 || wav.channels > 8 || wav.sampleRate == 0) {
        drwav_uninit(&wav);
        return fail(err, "wav: implausible channel count / sample rate");
    }
    const drwav_uint64 total = wav.totalPCMFrameCount;
    if (total == 0 || total > (1ull << 31)) {
        drwav_uninit(&wav);
        return fail(err, "wav: empty or absurdly long stream");
    }
    std::vector<int16_t> pcm(static_cast<size_t>(total) * wav.channels);
    const drwav_uint64 got = drwav_read_pcm_frames_s16(&wav, total, pcm.data());
    const uint16_t ch = wav.channels;
    const uint32_t rate = wav.sampleRate;
    drwav_uninit(&wav);
    if (got == 0)
        return fail(err, "wav: decoded zero frames");
    take_s16(out, pcm.data(), static_cast<size_t>(got), ch, rate);
    return true;
}

bool decode_mp3(const uint8_t* data, size_t len, PcmAudio* out,
                std::string* err) {
    drmp3_config cfg;
    drmp3_uint64 frames = 0;
    drmp3_int16* pcm = drmp3_open_memory_and_read_pcm_frames_s16(
        data, len, &cfg, &frames, nullptr);
    if (!pcm || frames == 0) {
        if (pcm) drmp3_free(pcm, nullptr);
        return fail(err, "mp3: no decodable audio stream");
    }
    if (cfg.channels < 1 || cfg.channels > 8 || cfg.sampleRate == 0) {
        drmp3_free(pcm, nullptr);
        return fail(err, "mp3: implausible channel count / sample rate");
    }
    take_s16(out, pcm, static_cast<size_t>(frames),
             static_cast<uint16_t>(cfg.channels), cfg.sampleRate);
    drmp3_free(pcm, nullptr);
    return true;
}

bool decode_flac(const uint8_t* data, size_t len, PcmAudio* out,
                 std::string* err) {
    unsigned int ch = 0, rate = 0;
    drflac_uint64 frames = 0;
    drflac_int16* pcm = drflac_open_memory_and_read_pcm_frames_s16(
        data, len, &ch, &rate, &frames, nullptr);
    if (!pcm || frames == 0) {
        if (pcm) drflac_free(pcm, nullptr);
        return fail(err, "flac: no decodable audio stream");
    }
    if (ch < 1 || ch > 8 || rate == 0) {
        drflac_free(pcm, nullptr);
        return fail(err, "flac: implausible channel count / sample rate");
    }
    take_s16(out, pcm, static_cast<size_t>(frames),
             static_cast<uint16_t>(ch), rate);
    drflac_free(pcm, nullptr);
    return true;
}

bool decode_ogg(const uint8_t* data, size_t len, PcmAudio* out,
                std::string* err) {
    if (len > 0x7FFFFFFFu)
        return fail(err, "ogg: file too large");
    int ch = 0, rate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(
        data, static_cast<int>(len), &ch, &rate, &pcm);
    if (frames <= 0 || !pcm) {
        if (pcm) free(pcm);
        return fail(err, "ogg: not a decodable Ogg Vorbis stream");
    }
    if (ch < 1 || ch > 8 || rate <= 0) {
        free(pcm);
        return fail(err, "ogg: implausible channel count / sample rate");
    }
    take_s16(out, pcm, static_cast<size_t>(frames),
             static_cast<uint16_t>(ch), static_cast<uint32_t>(rate));
    free(pcm);
    return true;
}

}  // namespace

bool DecodeAudio(const uint8_t* data, size_t len, PcmAudio* out,
                 std::string* err) {
    if (!data || len < 4 || !out)
        return fail(err, "audio: file too short to identify");
    if (std::memcmp(data, "RIFF", 4) == 0)
        return decode_wav(data, len, out, err);
    if (std::memcmp(data, "OggS", 4) == 0)
        return decode_ogg(data, len, out, err);
    if (std::memcmp(data, "fLaC", 4) == 0)
        return decode_flac(data, len, out, err);
    // MP3 last: its magics are weak (ID3 tag, or a bare 0xFFEx frame sync).
    if (std::memcmp(data, "ID3", 3) == 0 ||
        (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0))
        return decode_mp3(data, len, out, err);
    return fail(err, "audio: unrecognized format (want wav/mp3/ogg/flac)");
}

bool DecodeEngineWav(const uint8_t* data, size_t len, PcmAudio* out,
                     std::string* err) {
    if (!data || !out || len < 12 || std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "WAVE", 4) != 0)
        return fail(err, "engine wav: not a RIFF/WAVE file");
    const uint32_t riff_size = rd_u32(data + 4);
    // walk_wav marks an overstated RIFF size fatal: the engine trusts this
    // field as its walk bound and would read out of bounds in-game.
    if (static_cast<uint64_t>(riff_size) + 8 > len)
        return fail(err, "engine wav: RIFF size field overstates the file");
    const size_t end = 8 + riff_size;

    bool have_fmt = false;
    uint16_t tag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* pcm_beg = nullptr;
    size_t pcm_len = 0;
    bool have_data = false;

    // First fmt / first data win, exactly like the engine's walker.
    size_t pos = 12;
    while (pos + 8 <= end) {
        const uint8_t* cid = data + pos;
        const uint32_t csize = rd_u32(data + pos + 4);
        const size_t body = pos + 8;
        if (!have_fmt && std::memcmp(cid, "fmt ", 4) == 0) {
            if (csize < 14)
                return fail(err, "engine wav: fmt chunk under 14 bytes");
            if (csize < 16)  // no bits field: unusable as PCM
                return fail(err, "engine wav: fmt chunk lacks a bits field");
            if (body + 16 > end)
                return fail(err, "engine wav: fmt chunk runs past the RIFF bound");
            tag = rd_u16(data + body);
            ch = rd_u16(data + body + 2);
            rate = rd_u32(data + body + 4);
            bits = rd_u16(data + body + 14);
            have_fmt = true;
        } else if (!have_data && std::memcmp(cid, "data", 4) == 0) {
            // walk_wav marks data-past-bound fatal: the engine would copy
            // garbage/OOB.
            if (static_cast<uint64_t>(body) + csize > end)
                return fail(err, "engine wav: data chunk runs past the RIFF bound");
            pcm_beg = data + body;
            pcm_len = csize;
            have_data = true;
        }
        pos = body + ((static_cast<size_t>(csize) + 1) & ~size_t(1));
    }
    if (!have_fmt)
        return fail(err, "engine wav: no fmt chunk within the RIFF bound");
    if (!have_data)
        return fail(err, "engine wav: no data chunk within the RIFF bound");
    if (tag != 1)
        return fail(err, "engine wav: not PCM (fmt tag != 1)");
    if (ch != 1 && ch != 2)
        return fail(err, "engine wav: channels not 1 or 2");
    if (bits != 8 && bits != 16)
        return fail(err, "engine wav: bits not 8 or 16");
    if (rate == 0)
        return fail(err, "engine wav: zero sample rate");

    out->channels = ch;
    out->bits = bits;  // source depth preserved (8-bit stays unsigned u8)
    out->rate = rate;
    const size_t block = static_cast<size_t>(ch) * (bits / 8);
    out->pcm.assign(pcm_beg, pcm_beg + (pcm_len / block) * block);
    return true;
}

PcmAudio Normalize(const PcmAudio& in, uint32_t rate, uint16_t channels,
                   uint16_t bits) {
    if (in.channels == 0 || in.channels > 8 || in.rate == 0 ||
        (in.bits != 8 && in.bits != 16))
        return {};
    const uint32_t dst_rate = rate ? rate : in.rate;
    const uint16_t dst_ch = channels ? channels : in.channels;
    const uint16_t dst_bits = bits ? bits : in.bits;
    if (dst_bits != 8 && dst_bits != 16)
        return {};
    if (dst_ch != in.channels && dst_ch != 1 && dst_ch != 2)
        return {};  // folding targets mono/stereo only (engine formats)
    if (dst_rate == in.rate && dst_ch == in.channels && dst_bits == in.bits)
        return in;

    // Work in signed 16-bit throughout; u8 is unsigned around 0x80.
    const size_t src_frames = in.FrameCount();
    std::vector<int16_t> work;
    if (in.bits == 8) {
        work.reserve(in.pcm.size());
        for (uint8_t b : in.pcm)
            work.push_back(static_cast<int16_t>((int(b) - 128) << 8));
    } else {
        work.resize(in.pcm.size() / 2);
        std::memcpy(work.data(), in.pcm.data(), work.size() * 2);
    }

    // Channel fold (same policy as opus_to_pcm_payload): to mono = average
    // all source channels, to stereo = first two (mono duplicated).
    uint16_t ch = in.channels;
    if (dst_ch != ch) {
        std::vector<int16_t> conv;
        conv.reserve(src_frames * dst_ch);
        for (size_t f = 0; f < src_frames; ++f) {
            const int16_t* s = work.data() + f * ch;
            if (dst_ch == 1) {
                int acc = 0;
                for (int k = 0; k < ch; ++k) acc += s[k];
                conv.push_back(static_cast<int16_t>(acc / ch));
            } else {  // dst_ch == 2
                conv.push_back(s[0]);
                conv.push_back(ch >= 2 ? s[1] : s[0]);
            }
        }
        work.swap(conv);
        ch = dst_ch;
    }

    // Resample: 32.32 fixed-point linear interpolation driven by OUTPUT
    // frame index -- one 64-bit mul + shift per sample (fpk_reader.cpp).
    if (dst_rate != in.rate) {
        const size_t frames = work.size() / ch;
        const size_t out_frames = static_cast<size_t>(
            static_cast<uint64_t>(frames) * dst_rate / in.rate);
        std::vector<int16_t> res(out_frames * ch);
        if (frames != 0) {
            const uint64_t step =
                (static_cast<uint64_t>(in.rate) << 32) / dst_rate;
            uint64_t pos = 0;
            for (size_t i = 0; i < out_frames; ++i, pos += step) {
                size_t f0 = static_cast<size_t>(pos >> 32);
                if (f0 >= frames) f0 = frames - 1;  // guard the last step
                const size_t f1 = (f0 + 1 < frames) ? f0 + 1 : f0;
                const uint32_t frac = static_cast<uint32_t>(pos & 0xFFFFFFFFu);
                for (int k = 0; k < ch; ++k) {
                    const int32_t s0 = work[f0 * ch + k];
                    const int32_t s1 = work[f1 * ch + k];
                    res[i * ch + k] = static_cast<int16_t>(
                        s0 + static_cast<int32_t>(
                                 (static_cast<int64_t>(s1 - s0) * frac) >> 32));
                }
            }
        }
        work.swap(res);
    }

    PcmAudio out;
    out.channels = ch;
    out.bits = dst_bits;
    out.rate = dst_rate;
    if (dst_bits == 8) {
        out.pcm.reserve(work.size());
        for (int16_t s : work)  // same narrowing as opus_to_pcm_payload
            out.pcm.push_back(static_cast<uint8_t>((s >> 8) + 128));
    } else {
        out.pcm.resize(work.size() * 2);
        std::memcpy(out.pcm.data(), work.data(), out.pcm.size());
    }
    return out;
}

std::vector<uint8_t> EncodeWav(const PcmAudio& a) {
    if (a.channels == 0 || a.rate == 0 || (a.bits != 8 && a.bits != 16))
        return {};
    const uint16_t block_align =
        static_cast<uint16_t>(a.channels * (a.bits / 8));
    // Whole frames only: a trailing partial frame would make the engine's
    // data-vs-fmt accounting lie.
    const uint32_t data_bytes = static_cast<uint32_t>(
        (a.pcm.size() / block_align) * block_align);
    const uint32_t byte_rate = a.rate * block_align;
    const uint32_t riff_size = 36 + data_bytes;

    std::vector<uint8_t> wav;
    wav.reserve(44 + data_bytes);
    auto put = [&](const void* s, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(s);
        wav.insert(wav.end(), b, b + n);
    };
    auto put_u32 = [&](uint32_t v) { put(&v, 4); };
    auto put_u16 = [&](uint16_t v) { put(&v, 2); };

    put("RIFF", 4);
    put_u32(riff_size);
    put("WAVE", 4);
    put("fmt ", 4);
    put_u32(16);  // PCM fmt chunk size
    put_u16(1);   // PCM
    put_u16(a.channels);
    put_u32(a.rate);
    put_u32(byte_rate);
    put_u16(block_align);
    put_u16(a.bits);
    put("data", 4);
    put_u32(data_bytes);
    if (data_bytes)
        put(a.pcm.data(), data_bytes);
    return wav;
}

double RmsDbfs(const PcmAudio& a) {
    // -144 dBFS floor (below 24-bit noise) keeps silence finite/comparable.
    constexpr double kFloor = -144.0;
    if (a.channels == 0 || (a.bits != 8 && a.bits != 16) || a.pcm.empty())
        return kFloor;
    double acc = 0.0;
    size_t n = 0;
    if (a.bits == 8) {
        for (uint8_t b : a.pcm) {
            const double x = (int(b) - 128) / 128.0;
            acc += x * x;
        }
        n = a.pcm.size();
    } else {
        n = a.pcm.size() / 2;
        for (size_t i = 0; i < n; ++i) {
            int16_t s;
            std::memcpy(&s, a.pcm.data() + i * 2, 2);
            const double x = s / 32768.0;
            acc += x * x;
        }
    }
    if (n == 0 || acc <= 0.0)
        return kFloor;
    const double db = 10.0 * std::log10(acc / n);  // 20*log10(sqrt(...))
    return db < kFloor ? kFloor : db;
}

}  // namespace kgt
