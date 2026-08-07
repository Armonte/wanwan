#pragma once

// UTF-8 ↔ std::filesystem::path adapters. Use these everywhere we cross
// the std::string ↔ std::filesystem::path boundary, since on MinGW with
// libstdc++, `path::string()` and `path(std::string)` go through the
// system ANSI codepage (CP1252 on most non-Japanese Windows installs).
// That silently rewrites unrepresentable codepoints -- full-width forms
// like ＣＰＷ (U+FF23/FF30/FF37) become '_' or '?' in the round-trip,
// breaking:
//   - Cross-peer #57 game-hash agreement (different bytes on different
//     stdlib builds for identical files on disk).
//   - CreateProcess working dir for JP-named game folders.
//   - Per-game profile / override / room-id derivations from exe stems.
//
// Win32 wide APIs handle the full Unicode BMP unconditionally; round-
// tripping through UTF-16 keeps the bytes intact across all locales.

#include <filesystem>
#include <string>
#include <windows.h>

namespace fm2k::utf8path {

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                        w.data(), n);
    return w;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// Construct a filesystem::path from a UTF-8 string without going through
// the narrow-codepage path. Always prefer this over `std::filesystem::
// path(std::string&)` when the input may contain non-ASCII bytes.
inline std::filesystem::path FromUtf8(const std::string& s) {
    return std::filesystem::path(Utf8ToWide(s));
}

// Render a filesystem::path back to UTF-8 without the narrow-codepage
// detour. Always prefer this over `path.string()` when the result is
// rendered to UI, hashed across peers, or persisted to a file the
// launcher reads back.
inline std::string ToUtf8(const std::filesystem::path& p) {
    return WideToUtf8(p.wstring());
}

// Same, for the path components most commonly passed through the UI:
inline std::string StemUtf8(const std::filesystem::path& p) {
    return WideToUtf8(p.stem().wstring());
}
inline std::string FilenameUtf8(const std::filesystem::path& p) {
    return WideToUtf8(p.filename().wstring());
}
inline std::string ParentPathUtf8(const std::filesystem::path& p) {
    return WideToUtf8(p.parent_path().wstring());
}
inline std::string ExtensionUtf8(const std::filesystem::path& p) {
    return WideToUtf8(p.extension().wstring());
}

}  // namespace fm2k::utf8path
