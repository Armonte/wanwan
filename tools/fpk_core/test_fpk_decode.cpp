// test_fpk_decode.cpp -- CLI: test_fpk_decode IN.fpk OUT.bin
//
// Calls fpk_reconstruct() on IN.fpk and writes the reconstructed original-format
// bytes to OUT.bin. Native test harness for the lossless gate (paired with
// compare_regions.py). No windows.h; links native zstd (and opusfile when built
// with -DFPK_WITH_OPUS).

#include "fpk_reader.h"

#include <cstdio>
#include <string>
#include <vector>

static bool slurp(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(n));
    size_t got = n ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    return got == out.size();
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s IN.fpk OUT.bin\n", argv[0]);
        return 2;
    }

    std::vector<uint8_t> fpk;
    if (!slurp(argv[1], fpk)) {
        std::fprintf(stderr, "error: cannot read %s\n", argv[1]);
        return 1;
    }

    std::string err;
    std::vector<uint8_t> out = fpk_reconstruct(fpk.data(), fpk.size(), &err);
    if (out.empty()) {
        std::fprintf(stderr, "error: fpk_reconstruct failed: %s\n",
                     err.empty() ? "(no detail)" : err.c_str());
        return 1;
    }

    FILE* f = std::fopen(argv[2], "wb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open %s for write\n", argv[2]);
        return 1;
    }
    size_t wrote = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (wrote != out.size()) {
        std::fprintf(stderr, "error: short write to %s\n", argv[2]);
        return 1;
    }

    std::fprintf(stderr, "ok: %s -> %s (%zu bytes)\n", argv[1], argv[2], out.size());
    return 0;
}
