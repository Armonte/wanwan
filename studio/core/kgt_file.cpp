// kgt_file.cpp -- see kgt_file.h. Port of tools/kgt/{kgt,fm2nd}.py
// CommonHeader parse/pack; sprite content-size rules mirror the proven
// walk in FM2KHook/src/vfs/fpk_reader.cpp.

#include "kgt_file.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif __has_include(<iconv.h>)
#include <iconv.h>
#include <cerrno>
#define KGT_HAVE_ICONV 1
#endif

namespace kgt {
namespace {

// ── bounds-checked little-endian cursor (pattern: fpk_reader.cpp) ────────
struct Cursor {
    const uint8_t* p;
    size_t len;
    size_t off = 0;
    bool ok = true;
    std::string err;

    Cursor(const uint8_t* d, size_t n) : p(d), len(n) {}

    void fail(const std::string& msg) {
        if (ok) { err = msg; ok = false; }
    }

    size_t remaining() const { return len - off; }

    bool need(size_t n, const char* what) {
        if (!ok) return false;
        if (n > len - off) {
            fail("truncated: need " + std::to_string(n) + " bytes for " +
                 what + ", have " + std::to_string(len - off) +
                 " at offset " + std::to_string(off));
            return false;
        }
        return true;
    }

    // Returns a pointer to `n` bytes and advances; nullptr on under-run.
    // n == 0 returns a valid (never dereferenced) pointer.
    const uint8_t* bytes(size_t n, const char* what) {
        if (!need(n, what)) return nullptr;
        const uint8_t* r = p + off;
        off += n;
        return r;
    }

    int32_t i32(const char* what) {
        if (!need(4, what)) return 0;
        int32_t v;
        std::memcpy(&v, p + off, 4);
        off += 4;
        return v;
    }

    int16_t i16(const char* what) {
        if (!need(2, what)) return 0;
        int16_t v;
        std::memcpy(&v, p + off, 2);
        off += 2;
        return v;
    }

    uint8_t u8(const char* what) {
        if (!need(1, what)) return 0;
        return p[off++];
    }
};

void emit(std::vector<uint8_t>& out, const void* src, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(src);
    out.insert(out.end(), b, b + n);
}

void emit_i32(std::vector<uint8_t>& out, int32_t v) { emit(out, &v, 4); }
void emit_i16(std::vector<uint8_t>& out, int16_t v) { emit(out, &v, 2); }

// Content byte count for one sprite frame (fpk_reader.cpp sprite walk):
// size != 0 -> size bytes; size == 0 -> w*h (+1024 if private palette)
// when w*h > 0, else 0 bytes.
size_t SpriteContentSize(const SpriteFrame& sf) {
    if (sf.size != 0) return static_cast<size_t>(sf.size);
    long long t = static_cast<long long>(sf.width) *
                  static_cast<long long>(sf.height);
    return (t > 0) ? static_cast<size_t>(t) + (sf.has_private_palette ? 1024 : 0)
                   : 0;
}

}  // namespace

bool IsFm95(const uint8_t* data, size_t len) {
    return len >= 7 && (std::memcmp(data, "KGTGAME", 7) == 0 ||
                        std::memcmp(data, "2DKGT95", 7) == 0);
}

bool FileTypeFromPath(const std::string& path, FileType* out) {
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos ||
        (slash != std::string::npos && dot < slash))
        return false;
    std::string ext = path.substr(dot + 1);
    for (char& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == "kgt")         *out = FileType::Kgt;
    else if (ext == "player") *out = FileType::Player;
    else if (ext == "stage")  *out = FileType::Stage;
    else if (ext == "demo")   *out = FileType::Demo;
    else return false;
    return true;
}

bool Parse(const uint8_t* data, size_t len, FileType type,
           KgtFile* out, std::string* err) {
    auto bail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };

    if (IsFm95(data, len))
        return bail("FM95 signature (KGTGAME/2DKGT95): refused, FM2K only");

    KgtFile f;
    f.type = type;
    Cursor c(data, len);

    const uint8_t* sig = c.bytes(16, "signature");
    if (!sig) return bail(c.err);
    if (std::memcmp(sig, "2DKGT2K", 7) != 0 &&
        std::memcmp(sig, "2DKGT2G", 7) != 0)
        return bail("unknown signature (want 2DKGT2K or 2DKGT2G)");
    std::memcpy(f.signature.data(), sig, 16);

    const uint8_t* proj = c.bytes(256, "project_name");
    if (!proj) return bail(c.err);
    std::memcpy(f.project_name.data(), proj, 256);

    // Scripts (fixed 39B records: whole-table bounds check up front).
    int32_t n = c.i32("script count");
    if (!c.ok) return bail(c.err);
    if (n < 0) return bail("negative script count");
    if (static_cast<uint64_t>(n) * 39 > c.remaining())
        return bail("truncated: script table (" + std::to_string(n) +
                    " records) extends past EOF");
    f.scripts.resize(static_cast<size_t>(n));
    for (Script& s : f.scripts) {
        std::memcpy(s.name.data(), c.bytes(32, "script name"), 32);
        s.script_index = c.i16("script index");
        s.unknown_flag1 = c.u8("script flag");
        s.special_flag = c.i32("script special_flag");
    }

    // Script items (fixed 16B records).
    n = c.i32("script_item count");
    if (!c.ok) return bail(c.err);
    if (n < 0) return bail("negative script_item count");
    if (static_cast<uint64_t>(n) * 16 > c.remaining())
        return bail("truncated: script_item table (" + std::to_string(n) +
                    " records) extends past EOF");
    f.items.resize(static_cast<size_t>(n));
    for (ScriptItem& it : f.items) {
        it.script_type = c.u8("item type");
        std::memcpy(it.payload.data(), c.bytes(15, "item payload"), 15);
    }

    // Sprite frames (20B header + variable content).
    n = c.i32("sprite count");
    if (!c.ok) return bail(c.err);
    if (n < 0) return bail("negative sprite count");
    f.sprites.reserve(std::min(static_cast<size_t>(n), c.remaining() / 20));
    for (int32_t i = 0; i < n; ++i) {
        SpriteFrame sf;
        sf.unknown_flag1 = c.i32("sprite flag");
        sf.width = c.i32("sprite width");
        sf.height = c.i32("sprite height");
        sf.has_private_palette = c.i32("sprite palette flag");
        sf.size = c.i32("sprite size");
        if (!c.ok)
            return bail(c.err + " (sprite " + std::to_string(i) + ")");
        if (sf.size < 0)
            return bail("sprite " + std::to_string(i) + ": negative size (" +
                        std::to_string(sf.size) + ")");
        size_t body = SpriteContentSize(sf);
        const uint8_t* b = c.bytes(body, "sprite content");
        if (!c.ok)
            return bail(c.err + " (sprite " + std::to_string(i) + ")");
        sf.content.assign(b, b + body);
        f.sprites.push_back(std::move(sf));
    }

    // 8 palettes, 1056B each.
    for (Palette& pal : f.palettes) {
        const uint8_t* colors = c.bytes(1024, "palette colors");
        const uint8_t* gap = c.bytes(32, "palette gap");
        if (!gap) return bail(c.err);
        std::memcpy(pal.colors.data(), colors, 1024);
        std::memcpy(pal.gap.data(), gap, 32);
    }

    // Sounds (42B header + `size` bytes of data).
    n = c.i32("sound count");
    if (!c.ok) return bail(c.err);
    if (n < 0) return bail("negative sound count");
    f.sounds.reserve(std::min(static_cast<size_t>(n), c.remaining() / 42));
    for (int32_t i = 0; i < n; ++i) {
        Sound sd;
        sd.unknown1 = c.i32("sound unknown1");
        const uint8_t* name = c.bytes(32, "sound name");
        int32_t size = c.i32("sound size");
        sd.sound_type = c.u8("sound type");
        sd.sound_track = c.u8("sound track");
        if (!c.ok)
            return bail(c.err + " (sound " + std::to_string(i) + ")");
        if (size < 0)
            return bail("sound " + std::to_string(i) + ": negative size (" +
                        std::to_string(size) + ")");
        std::memcpy(sd.name.data(), name, 32);
        const uint8_t* b = c.bytes(static_cast<size_t>(size), "sound data");
        if (!c.ok)
            return bail(c.err + " (sound " + std::to_string(i) + ")");
        sd.data.assign(b, b + size);
        f.sounds.push_back(std::move(sd));
    }

    // Tail: every remaining byte to EOF, verbatim.
    f.tail.assign(data + c.off, data + len);

    *out = std::move(f);
    return true;
}

std::vector<uint8_t> Serialize(const KgtFile& f) {
    size_t est = 16 + 256 + 4 * 4 + f.scripts.size() * 39 +
                 f.items.size() * 16 + 8 * 1056 + f.tail.size();
    for (const SpriteFrame& sf : f.sprites) est += 20 + sf.content.size();
    for (const Sound& sd : f.sounds) est += 42 + sd.data.size();

    std::vector<uint8_t> out;
    out.reserve(est);

    emit(out, f.signature.data(), 16);
    emit(out, f.project_name.data(), 256);

    emit_i32(out, static_cast<int32_t>(f.scripts.size()));
    for (const Script& s : f.scripts) {
        emit(out, s.name.data(), 32);
        emit_i16(out, s.script_index);
        out.push_back(s.unknown_flag1);
        emit_i32(out, s.special_flag);
    }

    emit_i32(out, static_cast<int32_t>(f.items.size()));
    for (const ScriptItem& it : f.items) {
        out.push_back(it.script_type);
        emit(out, it.payload.data(), 15);
    }

    emit_i32(out, static_cast<int32_t>(f.sprites.size()));
    for (const SpriteFrame& sf : f.sprites) {
        emit_i32(out, sf.unknown_flag1);
        emit_i32(out, sf.width);
        emit_i32(out, sf.height);
        emit_i32(out, sf.has_private_palette);
        emit_i32(out, sf.size);
        emit(out, sf.content.data(), sf.content.size());
    }

    for (const Palette& pal : f.palettes) {
        emit(out, pal.colors.data(), 1024);
        emit(out, pal.gap.data(), 32);
    }

    emit_i32(out, static_cast<int32_t>(f.sounds.size()));
    for (const Sound& sd : f.sounds) {
        emit_i32(out, sd.unknown1);
        emit(out, sd.name.data(), 32);
        emit_i32(out, static_cast<int32_t>(sd.data.size()));  // derived, by design
        out.push_back(sd.sound_type);
        out.push_back(sd.sound_track);
        emit(out, sd.data.data(), sd.data.size());
    }

    emit(out, f.tail.data(), f.tail.size());
    return out;
}

// ── DecodeName: CP932 -> UTF-8, best-effort, isolated from round-trip ────

namespace {

// Last-resort decode: ASCII printable passthrough, '?' for everything else.
std::string AsciiFallback(const uint8_t* b, size_t n) {
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i)
        s.push_back((b[i] >= 0x20 && b[i] < 0x7F) ? static_cast<char>(b[i])
                                                  : '?');
    return s;
}

#ifdef _WIN32
bool Cp932ToUtf8(const uint8_t* b, size_t n, std::string* out) {
    int wn = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                                 reinterpret_cast<const char*>(b),
                                 static_cast<int>(n), nullptr, 0);
    if (wn <= 0) return false;
    std::wstring w(static_cast<size_t>(wn), L'\0');
    if (MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                            reinterpret_cast<const char*>(b),
                            static_cast<int>(n), &w[0], wn) != wn)
        return false;
    int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn,
                                 nullptr, 0, nullptr, nullptr);
    if (un <= 0) return false;
    out->resize(static_cast<size_t>(un));
    return WideCharToMultiByte(CP_UTF8, 0, w.data(), wn,
                               &(*out)[0], un, nullptr, nullptr) == un;
}
#elif defined(KGT_HAVE_ICONV)
bool Cp932ToUtf8(const uint8_t* b, size_t n, std::string* out) {
    iconv_t cd = iconv_open("UTF-8", "CP932");
    if (cd == reinterpret_cast<iconv_t>(-1)) return false;
    std::vector<char> in(b, b + n);   // iconv wants a mutable char*
    char* ip = in.data();
    size_t il = n;
    std::string res;
    bool good = true;
    while (il > 0) {
        char buf[256];
        char* op = buf;
        size_t ol = sizeof(buf);
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        res.append(buf, static_cast<size_t>(op - buf));
        if (r == static_cast<size_t>(-1)) {
            if (errno == E2BIG) continue;   // output chunk full, keep going
            good = false;                   // EILSEQ/EINVAL: bad CP932
            break;
        }
    }
    iconv_close(cd);
    if (!good) return false;
    *out = std::move(res);
    return true;
}
#endif

}  // namespace

std::string DecodeName(const uint8_t* bytes, size_t len) {
    size_t n = 0;                       // NUL-terminate like _try_decode
    while (n < len && bytes[n] != 0) ++n;
    if (n == 0) return std::string();
#if defined(_WIN32) || defined(KGT_HAVE_ICONV)
    std::string out;
    if (Cp932ToUtf8(bytes, n, &out)) return out;
#endif
    return AsciiFallback(bytes, n);
}

}  // namespace kgt
