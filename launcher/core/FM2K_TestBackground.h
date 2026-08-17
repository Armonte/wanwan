// FM2K_TEST_BACKGROUND -- harness-only "do not steal my foreground" mode.
// Launcher half. The hook half (and the full rationale + layer map) lives in
// FM2KHook/src/hooks/background_mode.h; the two parsers are deliberate twins,
// because the launcher and the hook are separate binaries with no shared TU.
// KEEP THE ACCEPTED SPELLINGS IN SYNC.
//
// Shipping safety: OFF unless the variable is set to an exact recognised value.
// Unset, empty, "0", or any unrecognised spelling all mean OFF -- a real user
// launching normally can never end up with a window they cannot see.
#pragma once

#include <cstdlib>
#include <cstring>

namespace fm2k {
namespace test_background {

enum class Mode {
    OFF = 0,
    MINIMIZE,     // launcher window created minimized + not-focusable;
                  // game spawned with SW_SHOWMINNOACTIVE
    NOACTIVATE,   // launcher window created not-focusable, normal size;
                  // game spawned with SW_SHOWNOACTIVATE
};

// Read once per process, cached. No logging here (this header is included by
// TUs that run before SDL logging is configured); call sites log.
inline Mode GetMode() {
    static int cached = -1;
    if (cached < 0) {
        cached = (int)Mode::OFF;
        const char* v = std::getenv("FM2K_TEST_BACKGROUND");
        if (v && v[0] != '\0') {
            if (std::strcmp(v, "1") == 0 || std::strcmp(v, "minimize") == 0) {
                cached = (int)Mode::MINIMIZE;
            } else if (std::strcmp(v, "2") == 0 || std::strcmp(v, "noactivate") == 0) {
                cached = (int)Mode::NOACTIVATE;
            }
            // "0" and every unrecognised spelling stay OFF.
        }
    }
    return (Mode)cached;
}

inline bool IsOn() { return GetMode() != Mode::OFF; }

inline const char* ModeName() {
    switch (GetMode()) {
        case Mode::MINIMIZE:   return "minimize";
        case Mode::NOACTIVATE: return "noactivate";
        default:               return "off";
    }
}

}  // namespace test_background
}  // namespace fm2k
