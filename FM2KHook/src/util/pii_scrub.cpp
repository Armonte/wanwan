// fm2k::pii — see pii_scrub.h for the contract.

#include "pii_scrub.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <regex>
#include <string>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace fm2k::pii {

namespace {

// Captured at Init() time. The username is what's embedded in basically
// every OneDrive / Documents / Downloads path — replacing it with
// "<USER>" handles 90%+ of the leak surface in one sweep without
// having to enumerate every path-shape regex.
std::string g_username;
std::atomic<bool> g_initialized{false};

// Case-insensitive substring replace-all. Used for the username pass —
// path strings from the OS come through with whatever case the file
// system reports, which doesn't always match GetEnvironmentVariable.
void ReplaceAllICase(std::string& s, std::string_view needle,
                     std::string_view replacement) {
    if (needle.empty() || s.size() < needle.size()) return;
    std::string lower_s; lower_s.reserve(s.size());
    for (char c : s) lower_s.push_back((char)std::tolower((unsigned char)c));
    std::string lower_n; lower_n.reserve(needle.size());
    for (char c : needle) lower_n.push_back((char)std::tolower((unsigned char)c));

    size_t pos = 0;
    while ((pos = lower_s.find(lower_n, pos)) != std::string::npos) {
        s.replace(pos, needle.size(), replacement);
        lower_s.replace(pos, needle.size(), replacement);
        // Re-lowercase the chunk we just spliced so subsequent searches
        // stay in sync. Replacement is ASCII so tolower is a no-op
        // beyond a defensive cast.
        for (size_t i = pos; i < pos + replacement.size(); ++i) {
            lower_s[i] = (char)std::tolower((unsigned char)lower_s[i]);
        }
        pos += replacement.size();
    }
}

// True for IP ranges we KEEP (not redacted).
//   127.0.0.0/8      loopback
//   10.0.0.0/8       RFC1918
//   172.16.0.0/12    RFC1918
//   192.168.0.0/16   RFC1918
//   169.254.0.0/16   link-local
//   0.0.0.0          unspecified
//   169.254.x.x      already covered above
// Public addresses are the ones that identify a person's home, so
// those get the last two octets redacted.
bool IsPrivateIPv4(int a, int b, int /*c*/, int /*d*/) {
    if (a == 127) return true;
    if (a == 10)  return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 169 && b == 254) return true;
    if (a == 0)   return true;
    return false;
}

// Compile regexes once. They live for the process lifetime; no
// teardown needed. std::regex isn't the fastest engine on the planet
// but logging is not a per-frame hot path and the patterns are small.
const std::regex& EmailRe() {
    static const std::regex re(
        R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})");
    return re;
}
const std::regex& IPv4Re() {
    static const std::regex re(
        R"((\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}))");
    return re;
}
// Discord snowflakes embedded in our log lines. Always tagged with one
// of these prefixes so we don't accidentally redact match tokens or
// frame counters that happen to land in the 17-19 digit range.
const std::regex& DiscordIdRe() {
    static const std::regex re(
        R"(((?:user_id|user|id|dc_id|discord_user_id|by_id)\s*[=:]\s*\"?)(\d{17,19})(\"?))");
    return re;
}
// "OneDrive - Acme Corp\" — strip the org, keep the OneDrive marker.
const std::regex& OneDriveOrgRe() {
    static const std::regex re(
        R"(OneDrive - [^\\\/]{1,128})");
    return re;
}

}  // namespace

void Init() {
    if (g_initialized.exchange(true)) return;
#ifdef _WIN32
    char buf[256] = {};
    DWORD n = GetEnvironmentVariableA("USERNAME", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        g_username.assign(buf, n);
    }
    if (g_username.empty()) {
        // Fallback: derive from %USERPROFILE% basename.
        char up[MAX_PATH] = {};
        DWORD un = GetEnvironmentVariableA("USERPROFILE", up, sizeof(up));
        if (un > 0 && un < sizeof(up)) {
            std::string s(up, un);
            auto slash = s.find_last_of("\\/");
            if (slash != std::string::npos) g_username = s.substr(slash + 1);
        }
    }
#else
    if (const char* u = std::getenv("USER")) g_username = u;
#endif
}

std::string Scrub(std::string_view in) {
    if (!g_initialized.load(std::memory_order_relaxed)) Init();
    std::string s(in);

    // 1) USERNAME — most leaks are paths containing the OS username.
    // Done first because it's the cheapest and clears the bulk of
    // identifying data before the regex passes run.
    if (!g_username.empty() && g_username.size() >= 2) {
        ReplaceAllICase(s, g_username, "<USER>");
    }

    // 2) OneDrive - <Org> -> OneDrive (handles enterprise machines
    // where the username may have been overridden but the AD tenant
    // name still leaks through the OneDrive folder path).
    s = std::regex_replace(s, OneDriveOrgRe(), "OneDrive");

    // 3) Email addresses — total opaque mask, we don't need partial
    // info to debug.
    s = std::regex_replace(s, EmailRe(), "<email>");

    // 4) Discord snowflake IDs in tagged contexts. Manual sregex_iterator
    // loop because std::regex_replace doesn't take a per-match callback
    // in C++17 and we need first-4 / last-4 truncation to tell distinct
    // users apart in the log without doxxing either one.
    {
        std::string out;
        out.reserve(s.size());
        auto begin = std::sregex_iterator(s.begin(), s.end(), DiscordIdRe());
        auto end   = std::sregex_iterator();
        size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            out.append(s, last, (size_t)m.position() - last);
            const std::string id = m[2].str();
            out += m[1].str();
            if (id.size() >= 8) {
                out += id.substr(0, 4);
                out += "...";
                out += id.substr(id.size() - 4);
            } else {
                out += id;  // too short to be an actual snowflake
            }
            out += m[3].str();
            last = (size_t)m.position() + (size_t)m.length();
        }
        out.append(s, last, std::string::npos);
        s = std::move(out);
    }

    // 5) Public IPv4 addresses — FULLY masked. The original rule kept the
    // first two octets ("108.197.*.*") for coarse correlation, but that's
    // still identifying (ISP + city) and users reading shared logs
    // shouldn't see ANY of a peer's address (FlippySpatula, 2026-07-19).
    // We KEEP private/RFC1918/loopback intact so LAN testing diagnostics
    // ("punch -> 192.168.1.42") still read sensibly — those aren't PII.
    {
        std::string out;
        out.reserve(s.size());
        auto begin = std::sregex_iterator(s.begin(), s.end(), IPv4Re());
        auto end   = std::sregex_iterator();
        size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            int a = std::atoi(m[1].str().c_str());
            int b = std::atoi(m[2].str().c_str());
            int c = std::atoi(m[3].str().c_str());
            int d = std::atoi(m[4].str().c_str());
            out.append(s, last, (size_t)m.position() - last);
            if (a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
                c >= 0 && c <= 255 && d >= 0 && d <= 255 &&
                !IsPrivateIPv4(a, b, c, d)) {
                out += "<pub-ip>";
            } else {
                out += m[0].str();  // private/loopback/unspecified — keep raw
            }
            last = (size_t)m.position() + (size_t)m.length();
        }
        out.append(s, last, std::string::npos);
        s = std::move(out);
    }

    return s;
}

size_t ScrubInto(const char* src, char* dst, size_t dst_cap) {
    if (!dst || dst_cap == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }
    std::string out = Scrub(std::string_view(src));
    size_t n = out.size();
    if (n >= dst_cap) n = dst_cap - 1;
    std::memcpy(dst, out.data(), n);
    dst[n] = '\0';
    return n;
}

}  // namespace fm2k::pii
