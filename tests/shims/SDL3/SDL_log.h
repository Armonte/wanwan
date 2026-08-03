// Minimal SDL3 logging shim for host-native unit tests.
//
// reliable_channel.cpp is real hook source and logs through SDL, but the
// standalone RC test builds on the host toolchain with no SDL present. Rather
// than #ifdef the production file for a test's benefit, this shim satisfies
// the four SDL_Log* calls it makes and goes to stderr.
//
// It is ONLY on the include path of the reliable_channel_test target (see
// tests/CMakeLists.txt) -- it can never shadow the real SDL in a game build.
#pragma once

#include <cstdarg>
#include <cstdio>

enum {
    SDL_LOG_CATEGORY_APPLICATION = 0,
};

// Args are forwarded rather than discarded so that variables used only in a
// log call don't trip -Wunused under -Wall -Wextra.
static inline void sdl_shim_log(const char* level, const char* fmt, va_list ap) {
    std::fprintf(stderr, "[%s] ", level);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

#define SDL_SHIM_DEFINE_LOG(fn, level)                                   \
    static inline void fn(int /*category*/, const char* fmt, ...) {      \
        va_list ap;                                                      \
        va_start(ap, fmt);                                               \
        sdl_shim_log(level, fmt, ap);                                    \
        va_end(ap);                                                      \
    }

SDL_SHIM_DEFINE_LOG(SDL_LogInfo,  "info")
SDL_SHIM_DEFINE_LOG(SDL_LogWarn,  "warn")
SDL_SHIM_DEFINE_LOG(SDL_LogError, "error")

#undef SDL_SHIM_DEFINE_LOG
