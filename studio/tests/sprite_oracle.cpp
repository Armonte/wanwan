// sprite_oracle.cpp -- differential test for core/sprite_decode.cpp against
// tools/dump_player_pics.py, which is the byte-exact reference port of the
// engine's own decompressor.
//
// It prints one line per sprite: index, dimensions, whether the payload was
// compressed, and an FNV-1a hash of the DECODED INDEX PLANE (palette applied
// separately, so a palette bug cannot mask a decompression bug). The Python
// side prints the same lines for the same file and the two are diffed.
//
// Hashing the index plane rather than the RGBA output is deliberate: RGBA
// would fold three independent decisions (decompress, palette select, BGRA
// swizzle) into one number, and a mismatch would not say which broke.
//
// Build: g++ -O2 -std=c++17 -I../core ../core/kgt_file.cpp
//            ../core/sprite_decode.cpp sprite_oracle.cpp -o sprite_oracle
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>

#include "kgt_file.h"
#include "sprite_decode.h"

static uint64_t Fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: sprite_oracle FILE [--rgba INDEX OUT.raw]\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "open failed: %s\n", argv[1]); return 2; }
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());

    kgt::FileType type;
    if (!kgt::FileTypeFromPath(argv[1], &type)) {
        std::fprintf(stderr, "unknown file type: %s\n", argv[1]);
        return 2;
    }
    kgt::KgtFile f;
    std::string err;
    if (!kgt::Parse(blob.data(), blob.size(), type, &f, &err)) {
        std::fprintf(stderr, "parse failed: %s\n", err.c_str());
        return 2;
    }

    // --rgba dumps ONE decoded sprite as raw RGBA8888 so the palette and
    // alpha keying can be eyeballed. The hash mode above proves only the
    // index plane; it would happily pass with the BGRA swizzle inverted.
    if (argc >= 5 && std::string(argv[2]) == "--rgba") {
        const int want = std::atoi(argv[3]);
        kgt::DecodedSprite d;
        if (!kgt::DecodeSprite(f, want, 0, kgt::kDefaultTransparentIndex, d)) {
            std::fprintf(stderr, "decode failed: %s\n", d.error.c_str());
            return 1;
        }
        std::ofstream o(argv[4], std::ios::binary);
        o.write((const char*)d.rgba.data(), (std::streamsize)d.rgba.size());
        std::printf("%d %d\n", d.width, d.height);
        return 0;
    }

    for (size_t i = 0; i < f.sprites.size(); ++i) {
        const kgt::SpriteFrame& s = f.sprites[i];
        if (s.width <= 0 || s.height <= 0) {
            std::printf("%zu EMPTY\n", i);
            continue;
        }
        const size_t pal = s.has_private_palette ? 1024u : 0u;
        const size_t want = (size_t)s.width * (size_t)s.height + pal;
        std::vector<uint8_t> plain;
        bool ok;
        if (s.size != 0) {
            ok = kgt::DecompressSprite(s.content.data(), s.content.size(), want, plain);
        } else {
            ok = s.content.size() >= want;
            if (ok) plain.assign(s.content.begin(), s.content.begin() + (ptrdiff_t)want);
        }
        if (!ok) { std::printf("%zu FAIL\n", i); continue; }
        const uint64_t h = Fnv1a(plain.data() + pal, (size_t)s.width * (size_t)s.height);
        std::printf("%zu %d %d %d %d %016llx\n", i, s.width, s.height,
                    s.has_private_palette ? 1 : 0, s.size != 0 ? 1 : 0,
                    (unsigned long long)h);
    }
    return 0;
}
