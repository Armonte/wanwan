// fpk_reader.cpp -- see fpk_reader.h. Direct port of tools/kgt/fpk.py reconstruct().
//
// .fpk layout (little-endian throughout):
//   "FPK2" (current) or "FPK1" (legacy) + u32 level
//   zblob(header, 272B)
//   zblob(scripts_region)
//   zblob(script_items_region)
//   u32 sprite_n
//   zblob(sprite_headers = sprite_n * 20B)
//   zblob(sprite_contents = solid concat of every frame body)
//   zblob(palettes_region = 8 * 1056B)
//   u32 sound_n
//   per sound: u8 codec (0=raw, 1=opus)
//              42B sound header
//              FPK2 codec==1: u32 payload_off, u32 payload_len,
//                             u32 orig_sr, u16 orig_ch, u16 orig_bits,
//                             zblob(container = original WAV minus the
//                                   data-chunk payload, split at payload_off),
//                             rawblob(ogg-opus of the PCM payload)
//              FPK1 codec==1: u32 orig_sr, u16 orig_ch, rawblob(ogg-opus)
//              codec==0:      rawblob(original data verbatim)
//   zblob(tail)
//
// FPK2 sounds reconstruct BYTE-LENGTH-IDENTICAL to the original: the WAV
// container bytes are emitted verbatim and only the PCM payload (decoded,
// resampled to orig_sr, converted to orig_bits, padded/trimmed to exactly
// payload_len) is lossy. FPK1's whole-WAV rewrite drifted every sound's size
// (Nogaku.player -22840 B), which broke the engine's loader -- kept read-only
// for old files.
//
// A zblob is [u32 complen][complen bytes of a zstd frame]. The python packer
// compresses bytes objects, so each frame carries its content size and we can
// pre-size the output via ZSTD_getFrameContentSize.
//
// A rawblob is [u32 len][len bytes].

#include "fpk_reader.h"

#include <cstring>

#include <zstd.h>

#ifdef FPK_WITH_OPUS
#include <opusfile.h>
#endif

namespace {

// ── little-endian readers over a bounds-checked cursor ──────────────────
struct Cursor {
    const uint8_t* p;
    size_t len;
    size_t off;
    bool ok;
    std::string err;

    Cursor(const uint8_t* d, size_t n) : p(d), len(n), off(0), ok(true) {}

    void fail(const char* msg) {
        if (ok) { err = msg; ok = false; }
    }

    bool need(size_t n) {
        if (!ok) return false;
        if (off + n > len) { fail("unexpected end of .fpk stream"); return false; }
        return true;
    }

    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v;
        std::memcpy(&v, p + off, 4);
        off += 4;
        return v;
    }

    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v;
        std::memcpy(&v, p + off, 2);
        off += 2;
        return v;
    }

    uint8_t u8() {
        if (!need(1)) return 0;
        return p[off++];
    }

    // Returns a pointer to `n` bytes and advances; nullptr on under-run.
    const uint8_t* bytes(size_t n) {
        if (!need(n)) return nullptr;
        const uint8_t* r = p + off;
        off += n;
        return r;
    }
};

// Append n bytes to `out`.
void emit(std::vector<uint8_t>& out, const uint8_t* src, size_t n) {
    if (n) out.insert(out.end(), src, src + n);
}

void emit_i32(std::vector<uint8_t>& out, int32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    out.insert(out.end(), b, b + 4);
}

// Read one zstd-compressed blob and return its inflated bytes.
bool read_zblob(Cursor& c, std::vector<uint8_t>& dst) {
    uint32_t complen = c.u32();
    if (!c.ok) return false;
    const uint8_t* comp = c.bytes(complen);
    if (!comp) return false;

    unsigned long long content = ZSTD_getFrameContentSize(comp, complen);
    if (content == ZSTD_CONTENTSIZE_ERROR) {
        c.fail("zstd: bad frame (content-size error)");
        return false;
    }
    if (content == ZSTD_CONTENTSIZE_UNKNOWN) {
        c.fail("zstd: frame content size unknown (unsupported packer)");
        return false;
    }
    dst.resize(static_cast<size_t>(content));
    size_t got = ZSTD_decompress(dst.empty() ? nullptr : dst.data(), dst.size(),
                                 comp, complen);
    if (ZSTD_isError(got)) {
        c.fail("zstd: decompress failed");
        return false;
    }
    if (got != dst.size()) {
        c.fail("zstd: decompressed size mismatch");
        return false;
    }
    return true;
}

// Read one raw [u32 len][len bytes] blob; returns pointer (into the source) + len.
const uint8_t* read_rawblob(Cursor& c, size_t& out_len) {
    uint32_t n = c.u32();
    if (!c.ok) { out_len = 0; return nullptr; }
    const uint8_t* b = c.bytes(n);
    out_len = b ? n : 0;
    return b;
}

#ifdef FPK_WITH_OPUS
// opusfile always decodes at this fixed rate, regardless of the input stream.
constexpr uint32_t kOpusDecodeRate = 48000;

// Decode an OGG-OPUS payload to a 16-bit interleaved PCM RIFF/WAVE blob.
//
// opusfile always decodes at 48000 Hz. We emit the WAV at 48000 Hz directly --
// NO resample back to orig_sr. The engine builds its DirectSound buffer from
// the WAV's declared fmt rate and DirectSound mixes any rate to the primary, so
// 48k plays at correct pitch/duration. Skipping the old double-precision linear
// resampler removes seconds of per-load CPU on audio-heavy stages (9+ min of
// BGM is ~26M frames). orig_sr/orig_ch are recorded by the packer but only the
// decoded content matters for playback; the WAV header channel count must match
// the actual decoded channels, so we use the stream's channel count.
bool opus_to_wav(const uint8_t* opus, size_t opus_len, uint32_t orig_sr,
                 uint16_t orig_ch, std::vector<uint8_t>& wav, std::string* err) {
    int oe = 0;
    OggOpusFile* of = op_open_memory(opus, opus_len, &oe);
    if (!of) {
        if (err) *err = "opus: op_open_memory failed";
        return false;
    }
    int ch = op_channel_count(of, -1);
    if (ch < 1 || ch > 8) {
        op_free(of);
        if (err) *err = "opus: unexpected channel count";
        return false;
    }

    std::vector<int16_t> pcm48;
    int16_t tmp[11520 * 8];
    for (;;) {
        int got = op_read(of, tmp, static_cast<int>(sizeof(tmp) / sizeof(int16_t)), nullptr);
        if (got < 0) {
            op_free(of);
            if (err) *err = "opus: op_read error";
            return false;
        }
        if (got == 0) break;
        pcm48.insert(pcm48.end(), tmp, tmp + static_cast<size_t>(got) * ch);
    }
    op_free(of);

    // Emit at opus's native 48kHz with the ACTUAL decoded channel count -- no
    // resample (see function header). The WAV header rate/channels must match
    // the PCM we hand the engine; orig_sr/orig_ch are not needed for playback.
    (void)orig_sr;
    (void)orig_ch;
    const uint16_t channels = static_cast<uint16_t>(ch);
    const uint32_t sr = kOpusDecodeRate;
    const std::vector<int16_t>& pcm = pcm48;

    const uint16_t bits = 16;
    const uint16_t block_align = static_cast<uint16_t>(channels * (bits / 8));
    const uint32_t byte_rate = sr * block_align;
    const uint32_t data_bytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    const uint32_t riff_size = 36 + data_bytes;

    wav.clear();
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
    put_u32(16);              // PCM fmt chunk size
    put_u16(1);              // PCM
    put_u16(channels);
    put_u32(sr);
    put_u32(byte_rate);
    put_u16(block_align);
    put_u16(bits);
    put("data", 4);
    put_u32(data_bytes);
    if (data_bytes)
        put(pcm.data(), data_bytes);
    return true;
}
// FPK2: decode an OGG-OPUS payload to raw PCM matching the ORIGINAL WAV fmt
// (orig_sr, orig_ch, orig_bits), fitted to exactly payload_len bytes. The
// caller splices this into the original container bytes, so the fmt chunk the
// engine reads is the original one -- the PCM must actually be at that rate.
//
// Resampling 48k -> orig_sr uses 32.32 fixed-point linear interpolation driven
// by OUTPUT frame index (one 64-bit mul + shift per sample), so the cost scales
// with the original stream, not the 48k decode. This is integer math, not the
// old double-precision path that was cut from the FPK1 reader for being
// seconds-per-load on long BGM.
bool opus_to_pcm_payload(const uint8_t* opus, size_t opus_len,
                         uint32_t orig_sr, uint16_t orig_ch, uint16_t orig_bits,
                         size_t payload_len, std::vector<uint8_t>& out,
                         std::string* err) {
    if (orig_sr == 0 || (orig_bits != 8 && orig_bits != 16) ||
        (orig_ch != 1 && orig_ch != 2)) {
        if (err) *err = "fpk2: unsupported original PCM format";
        return false;
    }

    int oe = 0;
    OggOpusFile* of = op_open_memory(opus, opus_len, &oe);
    if (!of) {
        if (err) *err = "opus: op_open_memory failed";
        return false;
    }
    int src_ch = op_channel_count(of, -1);
    if (src_ch < 1 || src_ch > 8) {
        op_free(of);
        if (err) *err = "opus: unexpected channel count";
        return false;
    }

    std::vector<int16_t> pcm48;
    int16_t tmp[11520 * 8];
    for (;;) {
        int got = op_read(of, tmp, static_cast<int>(sizeof(tmp) / sizeof(int16_t)), nullptr);
        if (got < 0) {
            op_free(of);
            if (err) *err = "opus: op_read error";
            return false;
        }
        if (got == 0) break;
        pcm48.insert(pcm48.end(), tmp, tmp + static_cast<size_t>(got) * src_ch);
    }
    op_free(of);

    // Fold the decoded stream to orig_ch (the packer encodes from the original
    // WAV, so mismatches are rare: mono<->stereo only).
    if (src_ch != orig_ch) {
        std::vector<int16_t> conv;
        const size_t frames = pcm48.size() / static_cast<size_t>(src_ch);
        conv.reserve(frames * orig_ch);
        for (size_t f = 0; f < frames; ++f) {
            const int16_t* s = pcm48.data() + f * src_ch;
            if (orig_ch == 1) {
                int acc = 0;
                for (int k = 0; k < src_ch; ++k) acc += s[k];
                conv.push_back(static_cast<int16_t>(acc / src_ch));
            } else {  // orig_ch == 2
                conv.push_back(s[0]);
                conv.push_back(src_ch >= 2 ? s[1] : s[0]);
            }
        }
        pcm48.swap(conv);
    }

    const size_t src_frames = pcm48.size() / orig_ch;
    const size_t bytes_per_samp = orig_bits / 8;
    const size_t block = static_cast<size_t>(orig_ch) * bytes_per_samp;
    const size_t out_frames = payload_len / block;
    const uint8_t silence = (orig_bits == 8) ? 0x80 : 0x00;

    out.clear();
    out.resize(payload_len, silence);
    if (src_frames != 0) {
        // src position (48k domain) per output frame, 32.32 fixed point
        const uint64_t step =
            (static_cast<uint64_t>(kOpusDecodeRate) << 32) / orig_sr;
        uint8_t* dst = out.data();
        uint64_t pos = 0;
        for (size_t i = 0; i < out_frames; ++i, pos += step) {
            size_t f0 = static_cast<size_t>(pos >> 32);
            if (f0 >= src_frames) break;  // drift past the decode: stay silent
            const size_t f1 = (f0 + 1 < src_frames) ? f0 + 1 : f0;
            const uint32_t frac = static_cast<uint32_t>(pos & 0xFFFFFFFFu);
            for (int k = 0; k < orig_ch; ++k) {
                const int32_t s0 = pcm48[f0 * orig_ch + k];
                const int32_t s1 = pcm48[f1 * orig_ch + k];
                const int32_t s = s0 + static_cast<int32_t>(
                    (static_cast<int64_t>(s1 - s0) * frac) >> 32);
                uint8_t* d = dst + i * block + k * bytes_per_samp;
                if (orig_bits == 8) {
                    *d = static_cast<uint8_t>((s >> 8) + 128);
                } else {
                    const int16_t s16 = static_cast<int16_t>(s);
                    std::memcpy(d, &s16, 2);
                }
            }
        }
    }
    return true;
}
#endif  // FPK_WITH_OPUS

}  // namespace

std::vector<uint8_t> fpk_reconstruct(const uint8_t* fpk, size_t len, std::string* err) {
    auto bail = [&](const char* msg) -> std::vector<uint8_t> {
        if (err) *err = msg;
        return {};
    };

    Cursor c(fpk, len);
    const uint8_t* magic = c.bytes(4);
    if (!magic)
        return bail("not an .fpk (bad magic)");
    bool v1;
    if (std::memcmp(magic, "FPK2", 4) == 0)
        v1 = false;
    else if (std::memcmp(magic, "FPK1", 4) == 0)
        v1 = true;
    else
        return bail("not an .fpk (bad magic)");
    c.u32();  // level (unused on reconstruct)
    if (!c.ok) return bail(c.err.c_str());

    std::vector<uint8_t> out;
    // Pre-size to avoid repeated reallocations while appending ~100MB of
    // reconstructed regions across 8192 sprite-frame emits + sounds. ~10x is
    // the typical pack ratio; any shortfall just grows once more. Capped so a
    // tiny .fpk does not over-commit address space in the 32-bit process.
    try {
        out.reserve(len < 64u * 1024 * 1024 ? len * 10u : len * 6u);
    } catch (...) { /* reserve is an optimization; proceed without it */ }

    // header / scripts / script_items -- emit verbatim.
    std::vector<uint8_t> header, scripts, script_items;
    if (!read_zblob(c, header)) return bail(c.err.c_str());
    if (!read_zblob(c, scripts)) return bail(c.err.c_str());
    if (!read_zblob(c, script_items)) return bail(c.err.c_str());
    emit(out, header.data(), header.size());
    emit(out, scripts.data(), scripts.size());
    emit(out, script_items.data(), script_items.size());

    // sprites: walk the 20B headers, slice the matching body out of the
    // solid contents blob, emit header20 + body for each frame.
    uint32_t sprite_n = c.u32();
    if (!c.ok) return bail(c.err.c_str());
    std::vector<uint8_t> sprite_headers, sprite_contents;
    if (!read_zblob(c, sprite_headers)) return bail(c.err.c_str());
    if (!read_zblob(c, sprite_contents)) return bail(c.err.c_str());

    if (sprite_headers.size() < static_cast<size_t>(sprite_n) * 20)
        return bail("sprite header blob too small for sprite_n");

    emit_i32(out, static_cast<int32_t>(sprite_n));
    size_t cpos = 0;
    for (uint32_t i = 0; i < sprite_n; ++i) {
        const uint8_t* hdr = sprite_headers.data() + static_cast<size_t>(i) * 20;
        int32_t w, h, hpp, size;
        std::memcpy(&w, hdr + 4, 4);
        std::memcpy(&h, hdr + 8, 4);
        std::memcpy(&hpp, hdr + 12, 4);
        std::memcpy(&size, hdr + 16, 4);

        size_t n;
        if (size == 0) {
            long long t = static_cast<long long>(w) * static_cast<long long>(h);
            n = (t > 0) ? static_cast<size_t>(t + (hpp ? 1024 : 0)) : 0;
        } else {
            n = static_cast<size_t>(static_cast<uint32_t>(size));
        }

        if (cpos + n > sprite_contents.size())
            return bail("sprite contents blob underrun");

        emit(out, hdr, 20);
        emit(out, sprite_contents.data() + cpos, n);
        cpos += n;
    }

    // palettes -- emit verbatim.
    std::vector<uint8_t> palettes;
    if (!read_zblob(c, palettes)) return bail(c.err.c_str());
    emit(out, palettes.data(), palettes.size());

    // sounds.
    uint32_t sound_n = c.u32();
    if (!c.ok) return bail(c.err.c_str());
    emit_i32(out, static_cast<int32_t>(sound_n));

    for (uint32_t s = 0; s < sound_n; ++s) {
        uint8_t codec = c.u8();
        const uint8_t* hdr42 = c.bytes(42);
        if (!hdr42) return bail(c.err.c_str());

        if (codec == 1 && !v1) {
            // FPK2: splice decoded PCM into the verbatim original container.
            uint32_t p_off = c.u32();
            uint32_t p_len = c.u32();
            uint32_t orig_sr = c.u32();
            uint16_t orig_ch = c.u16();
            uint16_t orig_bits = c.u16();
            if (!c.ok) return bail(c.err.c_str());
            std::vector<uint8_t> container;
            if (!read_zblob(c, container)) return bail(c.err.c_str());
            size_t opus_len = 0;
            const uint8_t* opus = read_rawblob(c, opus_len);
            if (!c.ok) return bail(c.err.c_str());
            if (p_off > container.size())
                return bail("fpk2: sound payload offset past container");

            std::vector<uint8_t> pcm;
#ifdef FPK_WITH_OPUS
            std::string aerr;
            if (!opus_to_pcm_payload(opus, opus_len, orig_sr, orig_ch,
                                     orig_bits, p_len, pcm, &aerr))
                return bail(aerr.empty() ? "opus decode failed" : aerr.c_str());
#else
            // No decoder in this build: emit digital silence of the exact
            // original length. The file stays byte-length-identical and every
            // non-audio byte exact; the sound just plays silent.
            (void)opus;
            (void)opus_len;
            (void)orig_sr;
            (void)orig_ch;
            pcm.assign(p_len, orig_bits == 8 ? 0x80 : 0x00);
#endif
            emit(out, hdr42, 42);  // verbatim: size field is the original size
            emit(out, container.data(), p_off);
            emit(out, pcm.data(), pcm.size());
            emit(out, container.data() + p_off, container.size() - p_off);
        } else if (codec == 1) {
            // FPK1 legacy: whole-WAV rewrite; sizes drift (known-broken for
            // the engine's loader, kept so old .fpk files remain readable).
            uint32_t orig_sr = c.u32();
            uint16_t orig_ch = c.u16();
            if (!c.ok) return bail(c.err.c_str());
            size_t payload_len = 0;
            const uint8_t* payload = read_rawblob(c, payload_len);
            if (!c.ok) return bail(c.err.c_str());

#ifdef FPK_WITH_OPUS
            std::vector<uint8_t> wav;
            std::string aerr;
            if (!opus_to_wav(payload, payload_len, orig_sr, orig_ch, wav, &aerr))
                return bail(aerr.empty() ? "opus decode failed" : aerr.c_str());
            // Patch the 42B header's size field (+36) to the new WAV length.
            uint8_t hdr[42];
            std::memcpy(hdr, hdr42, 42);
            int32_t wsize = static_cast<int32_t>(wav.size());
            std::memcpy(hdr + 36, &wsize, 4);
            emit(out, hdr, 42);
            emit(out, wav.data(), wav.size());
#else
            (void)orig_sr;
            (void)orig_ch;
            // Lossless-first stub: keep the stream parseable by emitting the raw
            // opus payload as the sound body with the 42B header's size field
            // (+36) patched to the payload length. The lossless gate ignores
            // audio bytes (it checks sound COUNT + the non-audio regions), so
            // this keeps every other region byte-exact while audio is deferred.
            uint8_t hdr[42];
            std::memcpy(hdr, hdr42, 42);
            int32_t psize = static_cast<int32_t>(payload_len);
            std::memcpy(hdr + 36, &psize, 4);
            emit(out, hdr, 42);
            emit(out, payload, payload_len);
#endif
        } else {
            // codec 0 = raw passthrough: header + original data verbatim.
            size_t data_len = 0;
            const uint8_t* data = read_rawblob(c, data_len);
            if (!c.ok) return bail(c.err.c_str());
            emit(out, hdr42, 42);
            emit(out, data, data_len);
        }
    }

    // tail -- emit verbatim.
    std::vector<uint8_t> tail;
    if (!read_zblob(c, tail)) return bail(c.err.c_str());
    emit(out, tail.data(), tail.size());

    if (!c.ok) return bail(c.err.c_str());
    return out;
}
