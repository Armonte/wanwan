// audio_test.cpp -- unit tests for studio/core/audio/audio_convert.cpp.
//
// Coverage:
//   (a) synth sine -> EncodeWav -> DecodeEngineWav round-trip (byte-exact),
//       plus DecodeAudio over the same bytes (dr_wav path agrees).
//   (b) Normalize 44100 -> 22050 halves the frame count (+-1) and keeps
//       RMS within 1 dB.
//   (c) 16 -> 8 -> 16 bit round-trip keeps RMS within 1 dB.
//   (d) optional real-corpus WAV (argv[1], extracted from a .player via
//       tools/kgt/soundtool.py): DecodeEngineWav -> Normalize -> EncodeWav
//       must itself pass DecodeEngineWav. Skipped if no path is given.
//
// Build/run: ./build_native.sh (next to this file).

#include "../audio_convert.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using kgt::PcmAudio;

static int g_failures = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
            std::printf(__VA_ARGS__);                           \
            std::printf("\n");                                  \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

static PcmAudio synth_sine(uint32_t rate, uint16_t channels, double hz,
                           double amplitude, size_t frames) {
    PcmAudio a;
    a.channels = channels;
    a.bits = 16;
    a.rate = rate;
    a.pcm.resize(frames * channels * 2);
    for (size_t f = 0; f < frames; ++f) {
        const double v = amplitude * std::sin(2.0 * M_PI * hz * f / rate);
        const int16_t s = static_cast<int16_t>(std::lround(v * 32767.0));
        for (uint16_t k = 0; k < channels; ++k)
            std::memcpy(a.pcm.data() + (f * channels + k) * 2, &s, 2);
    }
    return a;
}

static void test_encode_decode_roundtrip() {
    const PcmAudio src = synth_sine(44100, 2, 440.0, 0.5, 11025);  // 0.25s
    const std::vector<uint8_t> wav = kgt::EncodeWav(src);
    CHECK(wav.size() == 44 + src.pcm.size(), "wav size %zu != 44 + %zu",
          wav.size(), src.pcm.size());

    PcmAudio back;
    std::string err;
    CHECK(kgt::DecodeEngineWav(wav.data(), wav.size(), &back, &err),
          "DecodeEngineWav failed: %s", err.c_str());
    CHECK(back.channels == src.channels && back.bits == src.bits &&
              back.rate == src.rate,
          "fmt mismatch: %u/%u/%u", back.channels, back.bits, back.rate);
    CHECK(back.pcm == src.pcm, "PCM bytes differ after round-trip");

    // The generic decoder (dr_wav path) must agree on the same bytes.
    PcmAudio gen;
    CHECK(kgt::DecodeAudio(wav.data(), wav.size(), &gen, &err),
          "DecodeAudio(wav) failed: %s", err.c_str());
    CHECK(gen.bits == 16 && gen.channels == src.channels &&
              gen.rate == src.rate && gen.pcm == src.pcm,
          "DecodeAudio(wav) disagrees with the synthesized PCM");
}

static void test_resample_halves_frames_keeps_rms() {
    const PcmAudio src = synth_sine(44100, 2, 440.0, 0.5, 11025);
    const PcmAudio dst = kgt::Normalize(src, 22050, 0, 0);
    CHECK(dst.rate == 22050 && dst.channels == 2 && dst.bits == 16,
          "unexpected normalized fmt: %u/%u/%u", dst.channels, dst.bits,
          dst.rate);
    const double want = src.FrameCount() / 2.0;
    CHECK(std::fabs(double(dst.FrameCount()) - want) <= 1.0,
          "frame count %zu not within 1 of %.1f", dst.FrameCount(), want);
    const double d = std::fabs(kgt::RmsDbfs(src) - kgt::RmsDbfs(dst));
    CHECK(d <= 1.0, "RMS drifted %.3f dB across 44100->22050", d);
}

static void test_bit_depth_roundtrip_rms() {
    const PcmAudio src = synth_sine(44100, 1, 440.0, 0.5, 22050);
    const PcmAudio u8 = kgt::Normalize(src, 0, 0, 8);
    CHECK(u8.bits == 8 && u8.FrameCount() == src.FrameCount(),
          "u8 conversion changed shape: bits=%u frames=%zu", u8.bits,
          u8.FrameCount());
    const PcmAudio s16 = kgt::Normalize(u8, 0, 0, 16);
    CHECK(s16.bits == 16 && s16.FrameCount() == src.FrameCount(),
          "s16 back-conversion changed shape");
    const double d = std::fabs(kgt::RmsDbfs(src) - kgt::RmsDbfs(s16));
    CHECK(d <= 1.0, "RMS drifted %.3f dB across 16->8->16", d);

    // DecodeEngineWav must preserve the SOURCE depth: an 8-bit engine WAV
    // comes back as bits=8 with the raw unsigned bytes, not widened.
    const std::vector<uint8_t> wav8 = kgt::EncodeWav(u8);
    PcmAudio back8;
    std::string err;
    CHECK(kgt::DecodeEngineWav(wav8.data(), wav8.size(), &back8, &err),
          "DecodeEngineWav(8-bit) failed: %s", err.c_str());
    CHECK(back8.bits == 8 && back8.pcm == u8.pcm,
          "8-bit engine wav was not preserved verbatim (bits=%u)", back8.bits);
}

static void test_real_engine_wav(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("SKIP real-file test: cannot open %s\n", path);
        return;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());

    PcmAudio a;
    std::string err;
    CHECK(kgt::DecodeEngineWav(bytes.data(), bytes.size(), &a, &err),
          "DecodeEngineWav(%s) failed: %s", path, err.c_str());
    if (g_failures) return;
    std::printf("real wav: %uHz %u-bit %uch, %zu frames, %.2f dBFS\n",
                a.rate, a.bits, a.channels, a.FrameCount(), kgt::RmsDbfs(a));
    CHECK(a.FrameCount() > 0, "real wav decoded to zero frames");

    // Full pipeline: normalize to a different engine format, re-encode,
    // and the result must pass our own engine walker again.
    const PcmAudio norm = kgt::Normalize(a, 22050, 1, 16);
    CHECK(norm.FrameCount() > 0, "Normalize emptied the real wav");
    const std::vector<uint8_t> wav = kgt::EncodeWav(norm);
    PcmAudio back;
    CHECK(kgt::DecodeEngineWav(wav.data(), wav.size(), &back, &err),
          "re-encoded real wav rejected by DecodeEngineWav: %s", err.c_str());
    CHECK(back.rate == 22050 && back.channels == 1 && back.bits == 16 &&
              back.pcm == norm.pcm,
          "re-encoded real wav did not round-trip");
    const double d = std::fabs(kgt::RmsDbfs(a) - kgt::RmsDbfs(norm));
    CHECK(d <= 1.0, "real wav RMS drifted %.3f dB through Normalize", d);
}

int main(int argc, char** argv) {
    test_encode_decode_roundtrip();
    test_resample_halves_frames_keeps_rms();
    test_bit_depth_roundtrip_rms();
    if (argc > 1)
        test_real_engine_wav(argv[1]);
    else
        std::printf("SKIP real-file test: no wav path given "
                    "(pass one, or let build_native.sh extract it)\n");

    if (g_failures) {
        std::printf("audio_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("audio_test: all tests passed\n");
    return 0;
}
