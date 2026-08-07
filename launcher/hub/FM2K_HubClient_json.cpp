// FM2K Hub client -- WinHTTP WebSocket transport.
//
// One I/O thread does the WS handshake then spawns a sender thread.
// The I/O thread itself owns the receive loop. Both push events
// onto a thread-safe inbox; the launcher's UI thread drains via
// HubClient::Poll() once per frame.
//
// JSON encode/decode is deliberately minimal -- the message catalog
// in docs/FM2K_Matchmaking_Design.md §15.2 is small enough that
// hand-rolled extractors are simpler than vendoring a JSON lib.
// If that catalog grows, swap in nlohmann/json.

// WinHTTP WebSocket APIs (WinHttpWebSocketCompleteUpgrade etc.) are
// gated on _WIN32_WINNT >= 0x0602 (Windows 8). Project-wide setting
// is 0x0601 (Win7) for compatibility; bump only this TU.
#ifdef _WIN32_WINNT
#  undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0602
#ifdef WINVER
#  undef WINVER
#endif
#define WINVER 0x0602

#include "FM2K_HubClient.h"
#include "version_local.h"  // fm2k::kAppVersion
#include <winhttp.h>

#include <SDL3/SDL_log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

#include "FM2K_HubClient_internal.h"

namespace fm2k {

std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Find `"key":` and return the position right after the colon (skipping whitespace).
// Returns std::string::npos if not found at outermost scope. Naive -- doesn't
// guard against nested objects with the same key. Fine for our flat protocol.
size_t FindKey(const std::string& s, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t p = 0;
    while (true) {
        p = s.find(needle, p);
        if (p == std::string::npos) return std::string::npos;
        size_t after = p + needle.size();
        while (after < s.size() && (s[after] == ' ' || s[after] == '\t')) ++after;
        if (after < s.size() && s[after] == ':') {
            ++after;
            while (after < s.size() && (s[after] == ' ' || s[after] == '\t')) ++after;
            return after;
        }
        p = after;
    }
}

// Hex-nibble lookup for \uXXXX decode. Returns -1 on non-hex.
static int JsonHexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Append `cp` (any 21-bit code point) to `out` as UTF-8 bytes.
static void JsonAppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

std::string GetStr(const std::string& s, const std::string& key) {
    size_t p = FindKey(s, key);
    if (p == std::string::npos || p >= s.size() || s[p] != '"') return {};
    ++p;
    std::string out;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) {
            char n = s[p + 1];
            switch (n) {
                case 'n': out += '\n'; p += 2; break;
                case 'r': out += '\r'; p += 2; break;
                case 't': out += '\t'; p += 2; break;
                case 'b': out += '\b'; p += 2; break;
                case 'f': out += '\f'; p += 2; break;
                case '"': out += '"';  p += 2; break;
                case '\\':out += '\\'; p += 2; break;
                case '/': out += '/';  p += 2; break;
                case 'u': {
                    // \uXXXX → UTF-8. Without this, Discord/hub-supplied
                    // nicks containing non-ASCII (e.g. "é" → "é")
                    // displayed as the literal "u00e9" in the UI.
                    if (p + 5 >= s.size()) { out += n; p += 2; break; }
                    int h0 = JsonHexNibble(s[p+2]);
                    int h1 = JsonHexNibble(s[p+3]);
                    int h2 = JsonHexNibble(s[p+4]);
                    int h3 = JsonHexNibble(s[p+5]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                        out += n; p += 2; break;
                    }
                    uint32_t cu = (uint32_t)((h0<<12)|(h1<<8)|(h2<<4)|h3);
                    p += 6;  // consumed \uXXXX
                    // Surrogate pair: high \uD800..\uDBFF + low \uDC00..\uDFFF
                    // combine into one code point.
                    if (cu >= 0xD800u && cu <= 0xDBFFu &&
                        p + 5 < s.size() && s[p] == '\\' && s[p+1] == 'u')
                    {
                        int l0 = JsonHexNibble(s[p+2]);
                        int l1 = JsonHexNibble(s[p+3]);
                        int l2 = JsonHexNibble(s[p+4]);
                        int l3 = JsonHexNibble(s[p+5]);
                        if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                            uint32_t lo = (uint32_t)((l0<<12)|(l1<<8)|(l2<<4)|l3);
                            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                                uint32_t cp = 0x10000u +
                                    ((cu - 0xD800u) << 10) +
                                    (lo - 0xDC00u);
                                JsonAppendUtf8(out, cp);
                                p += 6;
                                break;
                            }
                        }
                    }
                    JsonAppendUtf8(out, cu);
                    break;
                }
                default:  out += n; p += 2; break;
            }
        } else {
            out += s[p++];
        }
    }
    return out;
}

int GetInt(const std::string& s, const std::string& key, int def) {
    size_t p = FindKey(s, key);
    if (p == std::string::npos) return def;
    // Treat a literal `null` as "absent" -- same as the field being
    // omitted. Without this, atoi("null") returns 0 and a server
    // sending null for an unknown char/stage_id would silently parse
    // as "char/stage 0", which then resolves to the slot-0 entry of
    // the local KGT and produces a phantom row in the lobby panel.
    if (p + 4 <= s.size() && s.compare(p, 4, "null") == 0) return def;
    return std::atoi(s.c_str() + p);
}

// Parse a JSON bool by KEY. A raw find("true") would false-match a later key
// or nick containing the substring -- FindKey anchors to the actual value.
// Mirrors GetInt: null/absent/unexpected -> default.
bool GetBool(const std::string& s, const std::string& key, bool def) {
    size_t p = FindKey(s, key);
    if (p == std::string::npos) return def;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p + 4 <= s.size() && s.compare(p, 4, "true")  == 0) return true;
    if (p + 5 <= s.size() && s.compare(p, 5, "false") == 0) return false;
    return def;
}

// Pull a substring containing the JSON value following `key`. Returns empty
// if not found. Handles only object/array values where braces match -- used
// for nested {peer:{...}}.
std::string GetSub(const std::string& s, const std::string& key) {
    size_t p = FindKey(s, key);
    if (p == std::string::npos) return {};
    char open = s[p];
    if (open != '{' && open != '[') return {};
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    size_t start = p;
    for (; p < s.size(); ++p) {
        char c = s[p];
        if (esc) { esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (c == open) ++depth;
        else if (c == close) {
            --depth;
            if (depth == 0) return s.substr(start, p - start + 1);
        }
    }
    return {};
}

// Split a JSON array of objects into per-object substrings. Doesn't
// allocate any structured tree; we just hand back the chunks for
// further GetStr/GetInt extraction.
std::vector<std::string> SplitObjectArray(const std::string& arr) {
    std::vector<std::string> out;
    if (arr.size() < 2 || arr.front() != '[') return out;
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    size_t start = std::string::npos;
    for (size_t i = 1; i + 1 < arr.size(); ++i) {
        char c = arr[i];
        if (esc) { esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) {
                out.push_back(arr.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return out;
}

HubUser ParseUser(const std::string& obj) {
    HubUser u;
    u.id          = GetStr(obj, "id");
    u.nick        = GetStr(obj, "nick");
    u.room_id     = GetStr(obj, "room_id");
    u.status      = GetStr(obj, "status");
    u.opponent_id = GetStr(obj, "opponent_id");
    u.rtt_ms      = GetInt(obj, "rtt_ms", 0);
    u.tier        = GetStr(obj, "tier");
    return u;
}

HubRoom ParseRoom(const std::string& obj) {
    HubRoom r;
    r.id         = GetStr(obj, "id");
    r.name       = GetStr(obj, "name");
    r.user_count = GetInt(obj, "user_count", 0);
    return r;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
}  // namespace fm2k
