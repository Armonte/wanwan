// FM2K Launcher localization — see FM2K_Locale.h for the design rationale.

#include "FM2K_Locale.h"

#include <SDL3/SDL_log.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <string>

namespace fm2k {

namespace {

using StringMap = std::unordered_map<std::string, std::string>;

// Per-language string maps. Populated by Init(). Lookups in T() chase
// `current` first then `en` for fallback (so a missing-from-ja key still
// renders sensible English). Beyond that, T() returns the key literal.
StringMap g_strings_en;
StringMap g_strings_ja;
StringMap g_strings_es;
Lang      g_current_lang = Lang::En;
StringMap* g_active = &g_strings_en;

bool LoadIni(const char* path, StringMap& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char line[1024];
    int n = 0;
    while (std::fgets(line, sizeof(line), f)) {
        // Strip trailing CR / LF / whitespace.
        size_t len = std::strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '  || line[len-1] == '\t')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        // Strip leading whitespace.
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\0') continue;
        // Split on first '=' only.
        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = p;
        char* val = eq + 1;
        // Strip trailing space from key, leading space from val.
        size_t klen = std::strlen(key);
        while (klen > 0 && (key[klen-1] == ' ' || key[klen-1] == '\t')) {
            key[--klen] = '\0';
        }
        while (*val == ' ' || *val == '\t') ++val;
        if (klen == 0) continue;
        // Unescape the few C-style escapes the string table is allowed to
        // use. Values are single-line in the .ini, so a multi-line string
        // (e.g. update_retry = "%s\nClick to retry.") can only be spelled
        // as a literal backslash-n and has to be decoded here -- otherwise
        // the UI renders the backslash. Anything not in this set is passed
        // through verbatim, backslash included, so an unknown escape can
        // never silently eat a character.
        std::string decoded;
        decoded.reserve(std::strlen(val));
        for (const char* c = val; *c; ++c) {
            if (*c != '\\' || c[1] == '\0') { decoded.push_back(*c); continue; }
            switch (c[1]) {
                case 'n':  decoded.push_back('\n'); ++c; break;
                case 't':  decoded.push_back('\t'); ++c; break;
                case '\\': decoded.push_back('\\'); ++c; break;
                default:   decoded.push_back(*c);         break;
            }
        }
        out[std::string(key)] = decoded;
        ++n;
    }
    std::fclose(f);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Locale: loaded %d entries from %s", n, path);
    return true;
}

std::string ExeDir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    char* slash = std::strrchr(buf, '\\');
    if (!slash) slash = std::strrchr(buf, '/');
    if (slash) *slash = '\0';
    return std::string(buf);
}

std::string SettingsIniPath() {
    const char* a = std::getenv("APPDATA");
    if (!a || !*a) return "";
    std::string dir = std::string(a) + "\\FM2K_Rollback";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\settings.ini";
}

// Tiny INI write/read for the language preference. Keep flat one-key-per-
// line so a textedit fallback works (same shape as audio.ini /
// dev_flags.ini).
std::string ReadSetting(const char* key) {
    const std::string path = SettingsIniPath();
    if (path.empty()) return "";
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return "";
    char line[256];
    std::string result;
    while (std::fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        // Trim trailing space from key.
        char* k = p;
        size_t klen = std::strlen(k);
        while (klen > 0 && (k[klen-1] == ' ' || k[klen-1] == '\t')) k[--klen] = '\0';
        if (std::strcmp(k, key) != 0) continue;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        size_t vlen = std::strlen(v);
        while (vlen > 0 && (v[vlen-1] == '\n' || v[vlen-1] == '\r' ||
                            v[vlen-1] == ' '  || v[vlen-1] == '\t')) v[--vlen] = '\0';
        result = v;
        break;
    }
    std::fclose(f);
    return result;
}

void WriteSetting(const char* key, const char* value) {
    const std::string path = SettingsIniPath();
    if (path.empty()) return;
    // Read all keys, replace ours, write back.
    std::vector<std::pair<std::string, std::string>> kv;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                  s.back() == ' '  || s.back() == '\t')) s.pop_back();
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            kv.emplace_back(s.substr(0, eq), s.substr(eq + 1));
        }
        std::fclose(f);
    }
    bool found = false;
    for (auto& p : kv) if (p.first == key) { p.second = value; found = true; }
    if (!found) kv.emplace_back(key, value);
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (const auto& p : kv) std::fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        std::fclose(f);
    }
}

Lang LangFromCode(const char* code) {
    if (!code) return Lang::En;
    if (std::strcmp(code, "ja") == 0) return Lang::Ja;
    if (std::strcmp(code, "es") == 0) return Lang::Es;
    return Lang::En;
}

Lang AutoDetectFromOs() {
    WCHAR locale[LOCALE_NAME_MAX_LENGTH] = {0};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
        return Lang::En;
    }
    // We only care about the language tag (first 2 chars).
    if (locale[0] == L'j' && locale[1] == L'a') return Lang::Ja;
    if (locale[0] == L'e' && locale[1] == L's') return Lang::Es;
    return Lang::En;
}

StringMap* MapForLang(Lang lang) {
    switch (lang) {
        case Lang::En: return &g_strings_en;
        case Lang::Ja: return &g_strings_ja;
        case Lang::Es: return &g_strings_es;
    }
    return &g_strings_en;
}

}  // namespace

namespace Locale {

void Init() {
    // Load all three INIs unconditionally so switching language at runtime
    // is just a pointer flip — no disk I/O on the toggle. Total memory cost
    // is trivial (under 50KB across all three for ~200 strings each).
    const std::string dir = ExeDir() + "\\locales";
    LoadIni((dir + "\\en.ini").c_str(), g_strings_en);
    LoadIni((dir + "\\ja.ini").c_str(), g_strings_ja);
    LoadIni((dir + "\\es.ini").c_str(), g_strings_es);

    // Persisted choice wins over OS detection. Empty/missing setting = first
    // run, fall back to OS locale.
    const std::string saved = ReadSetting("language");
    Lang chosen = saved.empty()
        ? AutoDetectFromOs()
        : LangFromCode(saved.c_str());
    Set(chosen);
}

void Set(Lang lang) {
    g_current_lang = lang;
    g_active = MapForLang(lang);
    WriteSetting("language", CodeForLang(lang));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Locale: active language = %s", CodeForLang(lang));
}

Lang Current() { return g_current_lang; }

const char* CodeForLang(Lang lang) {
    switch (lang) {
        case Lang::En: return "en";
        case Lang::Ja: return "ja";
        case Lang::Es: return "es";
    }
    return "en";
}

const char* DisplayNameForLang(Lang lang) {
    // Always native-script — users find their language by recognizing it,
    // not by reading the launcher's current label. So "日本語" stays
    // "日本語" even when the launcher is set to English.
    // Plain string literals — file is UTF-8 so the multibyte sequences
    // pass through untouched. Avoids C++20's char8_t / char incompatibility.
    switch (lang) {
        case Lang::En: return "English";
        case Lang::Ja: return "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";  // 日本語
        case Lang::Es: return "Espa\xc3\xb1ol";                          // Español
    }
    return "English";
}

const std::vector<Lang>& All() {
    static const std::vector<Lang> kAll = { Lang::En, Lang::Ja, Lang::Es };
    return kAll;
}

}  // namespace Locale

const char* T(const char* key) {
    if (!key) return "";
    // Active language first.
    auto it = g_active->find(key);
    if (it != g_active->end()) return it->second.c_str();
    // Fallback to en.
    if (g_active != &g_strings_en) {
        auto it2 = g_strings_en.find(key);
        if (it2 != g_strings_en.end()) return it2->second.c_str();
    }
    // Last-ditch: return the key itself so devs can spot missing entries
    // visually instead of seeing blanks. Stable because `key` is a literal.
    return key;
}

}  // namespace fm2k
