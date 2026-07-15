// xref_test.cpp -- fixture gate for studio/core/xref.cpp.
//
// Loads the JSON fixture emitted by gen_fixture.py (the oracle: the
// byte-exact tools/kgt parsers over a real Bewear.player), rebuilds a
// kgt::KgtFile in memory (header-only aggregates -- kgt_file.cpp is
// intentionally NOT linked), runs BuildSoundXref, and asserts the
// results recorded from the validated Python prototype in
// docs/dev/2dfm_studio_design.md. Also runs synthetic malformed-input
// checks (clamping, descending windows, settings-item skip).
//
// The JSON reader below is deliberately minimal and ad-hoc: it parses
// exactly the fixed fixture shape, no external deps.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "xref.h"

namespace {

int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    auto va_ = (a); auto vb_ = (b); \
    if (!(va_ == vb_)) { \
        std::ostringstream os_; os_ << va_ << " vs " << vb_; \
        std::fprintf(stderr, "FAIL %s:%d: %s == %s (%s)\n", \
                     __FILE__, __LINE__, #a, #b, os_.str().c_str()); \
        ++g_failures; \
    } \
} while (0)

// ── minimal ad-hoc JSON reader (fixture shape only) ─────────────────────
// Supports: objects, arrays, strings (with \" \\ \/ \b \f \n \r \t and
// \uXXXX incl. surrogate pairs -> UTF-8), integer numbers. That is all
// gen_fixture.py emits.

struct Json {
    enum Kind { kNull, kNumber, kString, kArray, kObject } kind = kNull;
    long long num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    const Json& operator[](const std::string& key) const {
        static const Json null;
        auto it = obj.find(key);
        return it == obj.end() ? null : it->second;
    }
};

struct JsonParser {
    const char* p;
    const char* end;
    bool ok = true;

    explicit JsonParser(const std::string& s)
        : p(s.data()), end(s.data() + s.size()) {}

    void fail(const char* msg) {
        if (ok) std::fprintf(stderr, "json parse error: %s\n", msg);
        ok = false;
    }
    void SkipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }
    bool Consume(char c) {
        SkipWs();
        if (p < end && *p == c) { ++p; return true; }
        return false;
    }
    void AppendUtf8(std::string* out, unsigned cp) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    unsigned Hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            if (p >= end) { fail("truncated \\u"); return 0; }
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else { fail("bad \\u digit"); return 0; }
        }
        return v;
    }
    std::string ParseString() {
        std::string out;
        if (!Consume('"')) { fail("expected string"); return out; }
        while (ok && p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { out.push_back(c); continue; }
            if (p >= end) { fail("truncated escape"); break; }
            char e = *p++;
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned cp = Hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < end &&
                    p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo = Hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(&out, cp);
                break;
            }
            default: fail("bad escape"); break;
            }
        }
        if (p < end && *p == '"') ++p; else fail("unterminated string");
        return out;
    }
    Json ParseValue() {
        Json v;
        SkipWs();
        if (p >= end) { fail("truncated"); return v; }
        char c = *p;
        if (c == '{') {
            ++p;
            v.kind = Json::kObject;
            SkipWs();
            if (Consume('}')) return v;
            do {
                std::string key = ParseString();
                if (!Consume(':')) fail("expected ':'");
                v.obj[key] = ParseValue();
            } while (ok && Consume(','));
            if (!Consume('}')) fail("expected '}'");
        } else if (c == '[') {
            ++p;
            v.kind = Json::kArray;
            SkipWs();
            if (Consume(']')) return v;
            do {
                v.arr.push_back(ParseValue());
            } while (ok && Consume(','));
            if (!Consume(']')) fail("expected ']'");
        } else if (c == '"') {
            v.kind = Json::kString;
            v.str = ParseString();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            v.kind = Json::kNumber;
            bool neg = (c == '-');
            if (neg) ++p;
            long long n = 0;
            while (p < end && *p >= '0' && *p <= '9')
                n = n * 10 + (*p++ - '0');
            v.num = neg ? -n : n;
        } else {
            fail("unexpected token");
        }
        return v;
    }
};

// ── fixture -> in-memory KgtFile ─────────────────────────────────────────

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string TrimTrailingSpaces(const std::string& s) {
    size_t n = s.size();
    while (n > 0 && s[n - 1] == ' ') --n;
    return s.substr(0, n);
}

struct Fixture {
    kgt::KgtFile file;
    std::vector<std::string> script_names;  // decoded, index-aligned
    std::vector<std::string> sound_names;
};

bool LoadFixture(const std::string& path, Fixture* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open fixture: %s\n", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    JsonParser jp(text);
    Json root = jp.ParseValue();
    if (!jp.ok || root.kind != Json::kObject) return false;

    for (const Json& js : root["scripts"].arr) {
        kgt::Script s;
        s.name.fill(0);
        const std::string& name = js["name"].str;
        for (size_t i = 0; i < name.size() && i < s.name.size(); ++i)
            s.name[i] = static_cast<uint8_t>(name[i]);
        s.script_index = static_cast<int16_t>(js["script_index"].num);
        out->file.scripts.push_back(s);
        out->script_names.push_back(name);
    }
    for (const Json& ji : root["items"].arr) {
        kgt::ScriptItem it;
        it.script_type = static_cast<uint8_t>(ji["t"].num);
        const std::string& hex = ji["p"].str;
        if (hex.size() != 30) {
            std::fprintf(stderr, "bad payload hex length %zu\n", hex.size());
            return false;
        }
        for (size_t i = 0; i < 15; ++i) {
            int hi = HexNibble(hex[i * 2]), lo = HexNibble(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) return false;
            it.payload[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        out->file.items.push_back(it);
    }
    for (const Json& js : root["sounds"].arr) {
        kgt::Sound s;
        s.name.fill(0);
        // The fixture stores size only; xref cares about data.size().
        s.data.resize(static_cast<size_t>(js["size"].num));
        out->file.sounds.push_back(s);
        out->sound_names.push_back(js["name"].str);
    }
    return true;
}

// Script index by decoded name (trailing CP932-field padding spaces
// trimmed on both sides of the comparison: e.g. Bewear's "Start ").
int FindScript(const Fixture& fx, const std::string& name) {
    for (size_t i = 0; i < fx.script_names.size(); ++i)
        if (TrimTrailingSpaces(fx.script_names[i]) == name)
            return static_cast<int>(i);
    return -1;
}

std::vector<kgt::SoundUse> UsesInScript(const kgt::SoundXref& x,
                                        int sound, int script) {
    std::vector<kgt::SoundUse> out;
    auto it = x.uses.find(sound);
    if (it == x.uses.end()) return out;
    for (const kgt::SoundUse& u : it->second)
        if (u.script == script) out.push_back(u);
    return out;
}

// ── Bewear fixture assertions (docs/dev/2dfm_studio_design.md) ──────────

void TestBewear(const std::string& fixture_path) {
    Fixture fx;
    if (!LoadFixture(fixture_path, &fx)) {
        std::fprintf(stderr, "FAIL: fixture load\n");
        ++g_failures;
        return;
    }
    CHECK_EQ(fx.file.scripts.size(), 299u);
    CHECK_EQ(fx.file.items.size(), 4494u);
    CHECK_EQ(fx.file.sounds.size(), 37u);

    kgt::SoundXref x = kgt::BuildSoundXref(fx.file);

    // bewear_cry.wav <- 'Start' @tick 163 sprite 288 (a jump precedes
    // this use in the script, so the tick is an estimate).
    int start = FindScript(fx, "Start");
    CHECK(start >= 0);
    {
        std::vector<kgt::SoundUse> v = UsesInScript(x, 7, start);
        CHECK_EQ(v.size(), 1u);
        if (v.size() == 1) {
            CHECK_EQ(v[0].tick, 163);
            CHECK_EQ(v[0].sprite, 288);
            CHECK(v[0].tick_estimated);
        }
    }

    // bell.wav <- 'Hug Grab' @tick 61 sprite 480 + @tick 186 sprite 485
    // (first use is before the script's first jump, second is after).
    int hug = FindScript(fx, "Hug Grab");
    CHECK(hug >= 0);
    {
        std::vector<kgt::SoundUse> v = UsesInScript(x, 6, hug);
        CHECK_EQ(v.size(), 2u);
        if (v.size() == 2) {
            CHECK_EQ(v[0].tick, 61);
            CHECK_EQ(v[0].sprite, 480);
            CHECK(!v[0].tick_estimated);
            CHECK_EQ(v[1].tick, 186);
            CHECK_EQ(v[1].sprite, 485);
            CHECK(v[1].tick_estimated);
        }
    }

    // sound 2 <- '426B Bear Buster' @tick 37 sprite 601
    int buster = FindScript(fx, "426B Bear Buster");
    CHECK(buster >= 0);
    {
        std::vector<kgt::SoundUse> v = UsesInScript(x, 2, buster);
        CHECK_EQ(v.size(), 1u);
        if (v.size() == 1) {
            CHECK_EQ(v[0].tick, 37);
            CHECK_EQ(v[0].sprite, 601);
        }
    }

    // Unused sounds (size>0, zero uses) == exactly {3, 21}.
    std::vector<int> unused = x.UnusedSounds(fx.file);
    CHECK_EQ(unused.size(), 2u);
    if (unused.size() == 2) {
        CHECK_EQ(unused[0], 3);
        CHECK_EQ(unused[1], 21);
    }

    // Total sounds with at least one use.
    CHECK_EQ(x.uses.size(), 34u);

    // Scripts containing SF/SG/SC (oracle run over the same fixture).
    CHECK_EQ(x.scripts_with_jumps.size(), 185u);
}

// ── synthetic malformed-input checks ─────────────────────────────────────

kgt::ScriptItem MakeItem(uint8_t op, std::vector<uint8_t> payload = {}) {
    kgt::ScriptItem it;
    it.script_type = op;
    it.payload.fill(0);
    for (size_t i = 0; i < payload.size() && i < 15; ++i)
        it.payload[i] = payload[i];
    return it;
}

kgt::Script MakeScript(int16_t index) {
    kgt::Script s;
    s.name.fill(0);
    s.script_index = index;
    return s;
}

void TestSynthetic() {
    // Timeline semantics + settings-item skip + jump estimation.
    {
        kgt::KgtFile f;
        f.scripts.push_back(MakeScript(0));
        // item 0: opcode 3 (S) in the settings slot -- must be IGNORED.
        f.items.push_back(MakeItem(3, {0x00, 0x05, 0x00}));
        // I: wait 10, sprite 7
        f.items.push_back(MakeItem(12, {0x0A, 0x00, 0x07, 0x00}));
        // S: sound 1, command 0x12 -> tick 10, sprite 7, exact
        f.items.push_back(MakeItem(3, {0x12, 0x01, 0x00}));
        // SG (goto) -- marks the script, later ticks estimated
        f.items.push_back(MakeItem(10));
        f.items.push_back(MakeItem(9));  // second jump: no duplicate entry
        // I: wait 5, sprite index with high bits: p[3]=0x21 -> (1<<8)|3=259
        // (bits 5..7 of p[3] are flip flags, masked out)
        f.items.push_back(MakeItem(12, {0x05, 0x00, 0x03, 0x21}));
        // S: sound 1 again -> tick 15 (estimated), sprite 259
        f.items.push_back(MakeItem(3, {0x00, 0x01, 0x00}));

        kgt::Sound used, fresh, empty;
        used.data.resize(4);
        fresh.data.resize(4);
        // sounds: 0=empty (never "unused"), 1=used, 2=unused
        f.sounds.push_back(empty);
        f.sounds.push_back(used);
        f.sounds.push_back(fresh);

        kgt::SoundXref x = kgt::BuildSoundXref(f);
        CHECK_EQ(x.uses.size(), 1u);          // settings-slot S not recorded
        const std::vector<kgt::SoundUse>& v = x.uses[1];
        CHECK_EQ(v.size(), 2u);
        if (v.size() == 2) {
            CHECK_EQ(v[0].tick, 10);
            CHECK_EQ(v[0].sprite, 7);
            CHECK_EQ(static_cast<int>(v[0].command), 0x12);
            CHECK(!v[0].tick_estimated);
            CHECK_EQ(v[1].item, 6);
            CHECK_EQ(v[1].tick, 15);
            CHECK_EQ(v[1].sprite, 259);
            CHECK(v[1].tick_estimated);
        }
        CHECK_EQ(x.scripts_with_jumps.size(), 1u);
        if (!x.scripts_with_jumps.empty())
            CHECK_EQ(x.scripts_with_jumps[0], 0);
        std::vector<int> unused = x.UnusedSounds(f);
        CHECK_EQ(unused.size(), 1u);
        if (unused.size() == 1) CHECK_EQ(unused[0], 2);
    }

    // Malformed windows: out-of-range, descending, negative -- no crash,
    // no phantom uses.
    {
        kgt::KgtFile f;
        f.scripts.push_back(MakeScript(100));   // beyond items.size()
        f.scripts.push_back(MakeScript(2));     // descending
        f.scripts.push_back(MakeScript(-5));    // negative
        f.scripts.push_back(MakeScript(0));     // overlapping (covers all)
        f.items.push_back(MakeItem(0));
        f.items.push_back(MakeItem(3, {0x00, 0x00, 0x00}));  // S sound 0
        f.items.push_back(MakeItem(3, {0x00, 0x00, 0x00}));  // S sound 0
        kgt::Sound s;
        s.data.resize(1);
        f.sounds.push_back(s);

        kgt::SoundXref x = kgt::BuildSoundXref(f);
        // Script 0: [100, 2) clamped -> empty. Script 1: [2, -5) -> empty
        // (end clamps up to begin). Script 2: [-5 -> 0, 0) -> empty.
        // Script 3: [0, 3): item 0 = settings, items 1+2 = uses.
        CHECK_EQ(x.uses.size(), 1u);
        CHECK_EQ(x.uses[0].size(), 2u);
        for (const kgt::SoundUse& u : x.uses[0]) CHECK_EQ(u.script, 3);
        CHECK(x.UnusedSounds(f).empty());
    }

    // Single-script window with begin == items.size() (settings skip must
    // not read past the end) and empty file.
    {
        kgt::KgtFile f;
        f.scripts.push_back(MakeScript(0));
        kgt::SoundXref x = kgt::BuildSoundXref(f);  // items empty
        CHECK(x.uses.empty());
        CHECK(x.scripts_with_jumps.empty());
        CHECK(x.UnusedSounds(f).empty());
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string fixture =
        argc > 1 ? argv[1] : "bewear_fixture.json";
    TestBewear(fixture);
    TestSynthetic();
    if (g_failures) {
        std::fprintf(stderr, "xref_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("xref_test: all checks passed\n");
    return 0;
}
