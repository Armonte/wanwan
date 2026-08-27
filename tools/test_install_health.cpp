// test_install_health -- standalone Windows test for the Program Files /
// VirtualStore split detector (launcher/game/FM2K_InstallHealth.cpp).
//
// This does NOT live in tests/run.sh: that harness is host-native (Linux g++)
// and this code is Win32 by nature -- the thing under test is UAC filesystem
// virtualization, which has no host-native equivalent to exercise. Built with
// mingw and run through WSL interop instead:
//
//   i686-w64-mingw32-g++ -std=c++17 -static -I launcher/game \
//       tools/test_install_health.cpp launcher/game/FM2K_InstallHealth.cpp \
//       -o build/test_install_health.exe && ./build/test_install_health.exe
//
// It builds a REAL mirror under the real %LOCALAPPDATA%\VirtualStore rather
// than faking the path computation, because the path computation (the
// relative_path() drive-strip) is one of the two things that can actually be
// wrong. It cleans up after itself.
//
// DO NOT PUT "install" (or "setup"/"update"/"patch") IN THE OUTPUT FILENAME.
// Windows' installer-detection heuristic force-elevates an UNMANIFESTED exe
// on the strength of its NAME alone, so building this to
// test_install_health.exe pops a UAC prompt before main() runs and the test
// cannot execute unattended. Measured here, which is why the binary is
// test_ih.exe. Our shipped binaries are unaffected -- both
// FM2K_RollbackLauncher.exe and FM2KUpdater.exe carry app.manifest with an
// explicit requestedExecutionLevel, and an explicit level suppresses the
// heuristic entirely. FM2KUpdater.exe would otherwise be a prime candidate.
//
// PASS argv[1] to probe one directory and print just the verdict. That mode
// demonstrates the manifest dependency the header describes, against the same
// real directory (measured 2026-08-27):
//
//   test_ih.exe            "C:\Program Files"  ->  verdict=Ok          err=0
//   test_ih_manifested.exe "C:\Program Files"  ->  verdict=NotWritable err=5
//
// Same code, same target, opposite answers. That IS the bug this detector
// exists for: our manifested launcher is denied, while the unmanifested game
// is silently redirected and told it succeeded. Build the manifested variant
// with tools/test_ih.rc:
//
//   i686-w64-mingw32-windres tools/test_ih.rc -O coff -o build/test_ih_res.o
#include "FM2K_InstallHealth.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace fm2k::install_health;

static int RunSuite();

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x\n";
}

// argv[1], when given, probes that directory and prints only the verdict.
// Used to demonstrate the MANIFEST DEPENDENCY documented in the header: the
// same probe against the same directory answers differently depending on
// whether the calling process is manifested, because an unmanifested process
// has its denied write silently virtualized and therefore sees success.
int main(int argc, char** argv) {
    if (argc > 1) {
        Report r = Probe(fs::path(argv[1]) / L"anything.exe");
        std::printf("verdict=%s writable_err=%lu\n",
                    r.verdict == Verdict::Ok ? "Ok"
                    : r.verdict == Verdict::NotWritable ? "NotWritable"
                    : "VirtualStoreSplit",
                    r.write_error);
        return 0;
    }
    return RunSuite();
}

static int RunSuite() {
    wchar_t lad[MAX_PATH] = {0};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH)) {
        std::printf("LOCALAPPDATA unset -- cannot test\n");
        return 2;
    }

    wchar_t cwd[MAX_PATH] = {0};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    const fs::path game_dir = fs::path(cwd) / L"build" / L"ih_test" / L"game";
    const fs::path exe = game_dir / L"fake_game.exe";
    const fs::path mirror =
        fs::path(lad) / L"VirtualStore" / game_dir.relative_path();

    std::error_code ec;
    fs::remove_all(game_dir, ec);
    fs::remove_all(mirror, ec);

    touch(exe);
    touch(game_dir / L"game.ini");

    std::printf("game_dir : %ls\nmirror   : %ls\n\n",
                game_dir.c_str(), mirror.c_str());

    std::printf("CASE A -- writable, no mirror (the healthy majority case)\n");
    {
        Report r = Probe(exe);
        check(r.verdict == Verdict::Ok, "verdict == Ok");
        check(r.healthy(), "healthy()");
        check(Describe(r).empty(), "Describe() is silent when healthy");
        check(r.shadowed.empty(), "nothing shadowed");
    }

    std::printf("\nCASE B -- mirror shadows a NON-config file only\n");
    {
        touch(game_dir / L"readme.txt");
        touch(mirror / L"readme.txt");
        Report r = Probe(exe);
        check(r.verdict == Verdict::VirtualStoreSplit, "verdict == VirtualStoreSplit");
        check(!r.config_split(), "config_split() false (game.ini not mirrored)");
        check(r.shadowed.size() == 1, "exactly 1 shadowed file");
        check(!Describe(r).empty(), "Describe() speaks up");
    }

    std::printf("\nCASE C -- mirror shadows game.ini (the silent-divergence case)\n");
    {
        touch(mirror / L"game.ini");
        Report r = Probe(exe);
        check(r.verdict == Verdict::VirtualStoreSplit, "verdict == VirtualStoreSplit");
        check(r.config_split(), "config_split() TRUE");
        check(r.shadowed.size() == 2, "2 shadowed files");
        check(r.shadowed.front() == "game.ini", "game.ini sorted first");
        const std::string d = Describe(r);
        check(d.find("game.ini IS AMONG THEM") != std::string::npos,
              "Describe() names the config split explicitly");
    }

    std::printf("\nCASE D -- a file in the mirror with NO real counterpart is not a split\n");
    {
        fs::remove(mirror / L"game.ini", ec);
        fs::remove(mirror / L"readme.txt", ec);
        touch(mirror / L"orphan.log");
        Report r = Probe(exe);
        check(r.shadowed.empty(), "orphan mirror file is not shadowing anything");
        check(r.verdict == Verdict::Ok, "verdict back to Ok");
    }

    std::printf("\nCASE E -- the probe leaves no litter behind\n");
    {
        fs::remove_all(mirror, ec);
        Probe(exe);
        check(!fs::exists(game_dir / L".fm2k_write_probe", ec),
              "no .fm2k_write_probe left in the game folder");
    }

    fs::remove_all(game_dir, ec);
    fs::remove_all(mirror, ec);

    std::printf("\n%s (%d failure(s))\n", g_fail ? "RED" : "GREEN", g_fail);
    return g_fail ? 1 : 0;
}
