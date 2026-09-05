// host_todo.h -- one way to say "this control is not wired yet".
//
// The rule this exists to enforce: nothing in the UI may look like it
// worked when it did not. Two bugs shipped in this spike from breaking it
// -- a SAVE REPLAY button that set an action code the host never read, and
// a BOOT button that answered by writing "BOOT" into the status bar, which
// reads exactly like success. Both were invisible in review because the
// silent path and the working path look identical.
//
// So an unimplemented control routes here instead of falling through a
// `default: break;`. It says so on stderr with the call site, and it says
// so ON SCREEN, in the status bar, in words the user cannot mistake for a
// confirmation.
#pragma once
#include <cstdio>

extern "C" void slint_ui_set_status(const char* utf8);

namespace hostgui {

inline void NotImplemented(const char* what, const char* file, int line) {
    std::fprintf(stderr, "[NOT IMPLEMENTED] %s  (%s:%d)\n", what, file, line);
    std::fflush(stderr);
    char msg[160];
    std::snprintf(msg, sizeof msg, "NOT IMPLEMENTED: %s", what);
    slint_ui_set_status(msg);
}

}  // namespace hostgui

#define HOST_TODO(what) ::hostgui::NotImplemented((what), __FILE__, __LINE__)
