// Log-scrubber regression test.
//
// WHY THIS EXISTS. The scrubber masked the pinned cnc-ddraw version as if it
// were an address -- "tag=v7.1.0.0" logged as "tag=v<pub-ip>", because 7.x is
// public address space and a version is four dotted octets. A reader then sees
// a download URL of .../download/v<pub-ip>/cnc-ddraw.zip and starts hunting a
// bug that does not exist. That happened on a real user log (2026-09-08).
//
// The fix narrows the match with a leading boundary, which is exactly the kind
// of change that can silently UNDO the redaction, so most of the cases below
// are the opposite direction: a real public address must still be masked in
// every shape we actually log one, and private addresses must stay readable.
#include "../FM2KHook/src/util/pii_scrub.h"

#include <cstdio>
#include <string>

namespace {
int g_fails = 0;

void Check(const char* in, const char* want) {
    const std::string got = fm2k::pii::Scrub(in);
    const bool ok = (got == want);
    if (!ok) ++g_fails;
    std::printf("%s  in=[%s]\n      got=[%s]\n     want=[%s]\n",
                ok ? "PASS" : "FAIL", in, got.c_str(), want);
}
}  // namespace

int main() {
    // Versions must survive. 203.0.113.x / 198.51.100.x below are the RFC 5737
    // documentation ranges -- public as far as the scrubber is concerned.
    Check("CncDDraw: local=(none) remote=v7.1.0.0 (tag=v7.1.0.0)",
          "CncDDraw: local=(none) remote=v7.1.0.0 (tag=v7.1.0.0)");
    Check("downloading https://github.com/x/releases/download/v7.1.0.0/cnc-ddraw.zip",
          "downloading https://github.com/x/releases/download/v7.1.0.0/cnc-ddraw.zip");

    // Public addresses must STILL be masked, in every shape we log them.
    Check("peer 203.0.113.7 connected",       "peer <pub-ip> connected");
    Check("addr=203.0.113.7:7500",            "addr=<pub-ip>:7500");
    Check("remote='203.0.113.7'",             "remote='<pub-ip>'");
    Check("(203.0.113.7)",                    "(<pub-ip>)");
    Check("203.0.113.7 at line start",        "<pub-ip> at line start");
    Check("range 10.0.0.1-203.0.113.7",       "range 10.0.0.1-<pub-ip>");
    Check("http://203.0.113.7/x",             "http://<pub-ip>/x");
    Check("two 203.0.113.7 and 198.51.100.9", "two <pub-ip> and <pub-ip>");
    Check("trailing 203.0.113.7.",            "trailing <pub-ip>.");

    // Private / loopback stay raw -- LAN diagnostics are not PII.
    Check("punch -> 192.168.1.42",            "punch -> 192.168.1.42");
    Check("127.0.0.1:7100",                   "127.0.0.1:7100");

    if (g_fails) {
        std::printf("\npii_scrub_test: %d FAILED\n", g_fails);
        return 1;
    }
    std::printf("\npii_scrub_test: ALL PASS\n");
    return 0;
}
