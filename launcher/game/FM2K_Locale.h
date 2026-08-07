// FM2K Launcher localization -- flat key→string lookup, three-language support.
//
// Why this exists: every user-visible string in the launcher used to be
// hardcoded English. This module replaces those with T("key") calls that
// resolve through a per-language table loaded from `locales/<code>.ini` at
// startup. Users pick their language from Settings → Language; the choice
// persists to %APPDATA%\FM2K_Rollback\settings.ini and applies on the next
// frame (no restart needed -- ImGui re-renders with the new strings).
//
// Font handling: ImGui's default font is Latin-only. On Init we scan
// Windows-system fonts (msgothic.ttc, meiryo.ttc, msgothic.ttf, YuGothM.ttc)
// and load the first one we find with GetGlyphRangesJapanese(), which also
// covers the Latin and Latin-1 supplement glyphs Spanish uses (ñ á é í ó ú).
// Zero bundled-font cost. Same pattern as bbbeditor/src/main.cpp:111-125.
//
// Translation files live in `locales/` next to the launcher EXE:
//   en.ini -- source of truth, English values for every key.
//   ja.ini -- Japanese.
//   es.ini -- Spanish.
// Format is flat key=value, one per line, # for comments. Missing keys in
// non-English files fall through to en.ini; missing-from-en falls through
// to the key string itself (so a freshly-added T() call in code still
// renders something sensible before en.ini catches up).
//
// Usage:
//   ImGui::Button(T("hub_signout"));
//   ImGui::Text(T("modal_incoming_challenge_body"), nick.c_str());
//
// T() always returns a stable const char* -- strings live in the active
// language map, not stack temporaries -- so it's safe to pass to ImGui's
// printf-style overloads.

#pragma once
#include <string>
#include <vector>

namespace fm2k {

// Three-letter language code in lowercase. New entries: add to Init's
// auto-detect map + ship a corresponding locales/<code>.ini.
enum class Lang { En, Ja, Es };

namespace Locale {

// Call once at launcher startup, AFTER ImGui::CreateContext but BEFORE
// the main loop. Loads en/ja/es INIs from `locales/` next to the EXE,
// reads the persisted language choice from APPDATA, falls back to OS
// locale on first run. Idempotent.
void Init();

// Switch the active language. Persists the choice. Subsequent T() calls
// return strings from the new language.
void Set(Lang lang);

// Currently-active language.
Lang Current();

// Three-letter code ("en"/"ja"/"es") for the given Lang. Inverse of LangFromCode.
const char* CodeForLang(Lang lang);

// Display name shown in the Settings → Language menu ("English",
// "日本語", "Español"). Always rendered in the language's native script
// -- never localized -- so users can find their language regardless of
// what the launcher is currently set to.
const char* DisplayNameForLang(Lang lang);

// Iteration helper for the Settings menu.
const std::vector<Lang>& All();

}  // namespace Locale

// Lookup macro -- used at every call site. Returns const char*. Stable
// pointer (lives in the active language map). If `key` is missing in the
// current language, falls through: current → en → key literal.
const char* T(const char* key);

}  // namespace fm2k

// Pull T() into the global namespace so call sites stay short
// (`T("foo")` not `fm2k::T("foo")`). The Locale namespace stays
// fully-qualified at config sites only.
using fm2k::T;
