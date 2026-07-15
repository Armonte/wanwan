// kgt_gate.cpp -- corpus round-trip gate for kgtcore (kgt_file.h contract:
// Serialize(Parse(bytes)) == bytes for every FM2K-family file).
//
//   kgt_gate FILE_LIST     text file of absolute paths, one per line
//   kgt_gate --info FILE   print section counts for one file (oracle
//                          cross-check vs tools/kgt/fm2nd.py parse)

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "kgt_file.h"

namespace {

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n < 0) return false;
    out->resize(static_cast<size_t>(n));
    f.seekg(0);
    if (n && !f.read(reinterpret_cast<char*>(out->data()), n)) return false;
    return true;
}

// First differing offset, or SIZE_MAX if equal-prefix (sizes differ).
size_t FirstDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return i;
    return SIZE_MAX;
}

int InfoMode(const std::string& path) {
    kgt::FileType type;
    if (!kgt::FileTypeFromPath(path, &type)) {
        std::fprintf(stderr, "unknown extension: %s\n", path.c_str());
        return 2;
    }
    std::vector<uint8_t> data;
    if (!ReadFileBytes(path, &data)) {
        std::fprintf(stderr, "read failed: %s\n", path.c_str());
        return 2;
    }
    if (kgt::IsFm95(data.data(), data.size())) {
        std::fprintf(stderr, "fm95 file, refused: %s\n", path.c_str());
        return 2;
    }
    kgt::KgtFile f;
    std::string err;
    if (!kgt::Parse(data.data(), data.size(), type, &f, &err)) {
        std::fprintf(stderr, "parse failed: %s: %s\n", path.c_str(), err.c_str());
        return 2;
    }
    std::printf("file: %s\n", path.c_str());
    std::printf("project: %s\n",
                kgt::DecodeName(f.project_name.data(), f.project_name.size()).c_str());
    std::printf("scripts: %zu\n", f.scripts.size());
    std::printf("script_items: %zu\n", f.items.size());
    std::printf("sprite_frames: %zu\n", f.sprites.size());
    std::printf("sounds: %zu\n", f.sounds.size());
    std::printf("tail: %zu\n", f.tail.size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--info") == 0)
        return InfoMode(argv[2]);
    if (argc != 2) {
        std::fprintf(stderr, "usage: kgt_gate FILE_LIST | kgt_gate --info FILE\n");
        return 2;
    }

    std::ifstream list(argv[1]);
    if (!list) {
        std::fprintf(stderr, "cannot open file list: %s\n", argv[1]);
        return 2;
    }

    size_t ok = 0, fail = 0, skip = 0;
    std::string line;
    while (std::getline(list, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty()) continue;
        const std::string& path = line;

        kgt::FileType type;
        if (!kgt::FileTypeFromPath(path, &type)) {
            std::printf("SKIP ext %s\n", path.c_str());
            ++skip;
            continue;
        }
        std::vector<uint8_t> data;
        if (!ReadFileBytes(path, &data)) {
            std::printf("FAIL %s read error\n", path.c_str());
            ++fail;
            continue;
        }
        if (kgt::IsFm95(data.data(), data.size())) {
            std::printf("SKIP fm95 %s\n", path.c_str());
            ++skip;
            continue;
        }
        kgt::KgtFile f;
        std::string err;
        if (!kgt::Parse(data.data(), data.size(), type, &f, &err)) {
            std::printf("FAIL %s %s\n", path.c_str(), err.c_str());
            ++fail;
            continue;
        }
        std::vector<uint8_t> repacked = kgt::Serialize(f);
        if (repacked != data) {
            size_t d = FirstDiff(data, repacked);
            if (d == SIZE_MAX)
                std::printf("FAIL %s size mismatch orig=%zu repack=%zu\n",
                            path.c_str(), data.size(), repacked.size());
            else
                std::printf("FAIL %s first diff at offset 0x%zx "
                            "(orig=%zu repack=%zu)\n",
                            path.c_str(), d, data.size(), repacked.size());
            ++fail;
            continue;
        }
        ++ok;
    }

    std::printf("DONE ok=%zu fail=%zu skip=%zu\n", ok, fail, skip);
    return fail > 0 ? 1 : 0;
}
