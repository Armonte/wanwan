// replay_tag_probe -- print a replay's provenance verdict exactly as the
// launcher computes it (same TUs: replay_provenance + gameplay_tag +
// content_tag), for "why is this replay [different game files]?" support and
// for end-to-end checks of the v2 gameplay tag.
//
//   replay_tag_probe.exe <file.fm2krep|.fm2kset>
//       header tags, participants, the tags recomputed for the replay's game
//       folder, and the verdict.
//   replay_tag_probe.exe --dir <game_dir> [--chars 0,1] [--stages 0] [--all]
//       the v2 tag of a folder for a participant set.
//
// Build (mingw i686, from the repo root):
//   i686-w64-mingw32-g++ -std=c++17 -O2 -municode -static -I. -Ilauncher/core
//     tools/replay_tag_probe.cpp launcher/core/replay_provenance.cpp
//     FM2KHook/src/netplay/gameplay_tag.cpp
//     FM2KHook/src/netplay/gameplay_tag_core.cpp
//     FM2KHook/src/netplay/content_tag.cpp -o replay_tag_probe.exe
//   (one command line)
// Console tool: plain stdio output is the point here (not a hook/launcher
// path, so the quill rule does not apply).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <windows.h>

#include "FM2KHook/src/netplay/content_tag.h"
#include "FM2KHook/src/netplay/gameplay_tag.h"
#include "replay_provenance.h"

namespace gt = fm2k::gameplay_tag;

static std::vector<uint8_t> ParseIds(const wchar_t* s) {
    std::vector<uint8_t> v;
    while (s && *s) {
        v.push_back((uint8_t)wcstoul(s, nullptr, 10));
        const wchar_t* c = wcschr(s, L',');
        s = c ? c + 1 : nullptr;
    }
    return v;
}

static void PrintParts(const gt::Participants& p) {
    std::printf("  participants: %s chars[", p.whole_roster ? "WHOLE ROSTER," : "");
    for (uint8_t c : p.chars) std::printf(" %u", c);
    std::printf(" ] stages[");
    for (uint8_t s : p.stages) std::printf(" %u", s);
    std::printf(" ]\n");
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wcscmp(argv[1], L"--dir") == 0) {
        const std::filesystem::path dir(argv[2]);
        gt::Participants p;
        for (int i = 3; i < argc; ++i) {
            if (!std::wcscmp(argv[i], L"--chars") && i + 1 < argc) p.chars = ParseIds(argv[++i]);
            else if (!std::wcscmp(argv[i], L"--stages") && i + 1 < argc) p.stages = ParseIds(argv[++i]);
            else if (!std::wcscmp(argv[i], L"--all")) p.whole_roster = true;
        }
        const std::wstring exe = fm2k::content_tag::ResolveGameExeName(dir.wstring().c_str());
        const std::wstring kgt = gt::ResolveKgtName(dir.wstring().c_str(), exe.c_str());
        std::printf("exe=%ls kgt=%ls\n", exe.c_str(), kgt.c_str());
        PrintParts(p);
        std::printf("v1 install tag  0x%08X\n",
                    fm2k::content_tag::ComputeForDir(dir.wstring().c_str(), exe.c_str()));
        std::printf("v2 gameplay tag 0x%08X\n",
                    gt::ComputeForDir(dir.wstring().c_str(), exe.c_str(), p));
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: replay_tag_probe <replay> | --dir <game_dir> "
                             "[--chars a,b] [--stages s] [--all]\n");
        return 2;
    }
    const std::filesystem::path rp(argv[1]);
    std::ifstream f(rp, std::ios::binary);
    uint8_t hdr[256] = {};
    f.read((char*)hdr, sizeof(hdr));
    if (f.gcount() != (std::streamsize)sizeof(hdr) || std::memcmp(hdr, "FMSS", 4) != 0) {
        std::fprintf(stderr, "not a v2 FMSS replay\n");
        return 1;
    }
    const auto dir = std::filesystem::absolute(rp).parent_path().parent_path();
    uint32_t tag = 0;
    gt::Participants p;
    const bool v2 = gt::DecodeHeader(hdr, &tag, &p);
    std::printf("replay: %ls\ngame dir: %ls\n", rp.c_str(), dir.c_str());
    std::printf("p1 char %u, p2 char %u\n", hdr[128], hdr[129]);
    if (v2) PrintParts(p);
    const auto v = fm2k::replay_provenance::Evaluate(hdr, dir);
    std::printf("v1 install tag : file 0x%08X  local 0x%08X%s\n", v.v1_file, v.v1_local,
                v.v2_present ? "  (not consulted: v2 present)" : "");
    std::printf("v2 gameplay tag: %s file 0x%08X  local 0x%08X\n",
                v.v2_present ? "present" : "absent ", v.v2_file, v.v2_local);
    std::printf("verdict: %s\n", v.Mismatch() ? "[different game files]" : "playable");
    return v.Mismatch() ? 3 : 0;
}
