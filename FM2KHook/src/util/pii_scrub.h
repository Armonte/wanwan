#pragma once

// fm2k::pii -- log redaction pipeline. Runs every log line through a
// chain of pattern-based replacements before it hits disk OR the
// in-memory launcher buffer, so users can share their logs with us
// without leaking their Windows username, real name (often embedded
// in OneDrive paths), public IP, Discord ID, or email.
//
// Used by both the hook DLL's LogOutputFunction (in dllmain.cpp) and
// the launcher's SDLCustomLogOutput (in FM2K_LauncherUI.cpp), so the
// .cpp is added to BOTH targets in CMake. Idempotent -- Init() can be
// called from each side without coordination.
//
// What gets redacted:
//   - The OS USERNAME everywhere it appears (caught once at Init())
//     -> "<USER>"
//   - Public IPv4 addresses                       -> "<pub-ip>" (fully masked)
//     Loopback, RFC1918, link-local kept intact (useful for diagnostics)
//   - Email addresses                             -> "<email>"
//   - Discord snowflake IDs (17-19 digit runs in user_id=/id="" contexts)
//     -> first 4 + ...XXXX last 4 (enough to correlate without doxxing)
//   - "OneDrive - <Org>" segments                 -> "OneDrive"
//
// What is NOT redacted (intentionally):
//   - Game install paths beyond username (D:\Games\fm2k\... is fine)
//   - Hub IDs / match tokens (we need them to cross-correlate)
//   - File names inside game dirs
//   - LAN/private IPs (192.168.*, 10.*, 127.*)

#include <cstddef>
#include <string>
#include <string_view>

namespace fm2k::pii {

// One-shot init. Reads the current OS USERNAME via the environment so
// the scrubber can find it embedded in arbitrary path strings and
// fopen() arguments later. Cheap; safe to call repeatedly.
void Init();

// Returns a redacted copy of the input. Allocates.
std::string Scrub(std::string_view in);

// Buffer-based variant for the hot logging path. Writes up to dst_cap-1
// bytes plus null terminator into `dst`. Returns the number of bytes
// written (excluding null). Truncates if redacted output would exceed
// dst_cap; in practice the redacted form is shorter than the raw line.
size_t ScrubInto(const char* src, char* dst, size_t dst_cap);

}  // namespace fm2k::pii
