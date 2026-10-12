// Replay provenance v2 (gameplay tag) -- host-native tests.
//
// The product rule under test: cosmetic mods (palettes, recolored or
// re-encoded sprites, extra skin files, sounds) must NEVER change the tag;
// gameplay data (script steps, character tail, system block) must.
//
// Fixtures are synthetic FM2K files built in memory and written to a temp dir
// under the build tree, so nothing large is committed. One extra case runs the
// same checks on a real .player from a local game install when one exists
// (FM2K_TEST_GAME_DIR, default /mnt/d/games/fm2k) and skips with a message
// otherwise.
#include "doctest.h"

#include "../FM2KHook/src/netplay/gameplay_tag_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fm2k::gameplay_tag;

namespace {

using Bytes = std::vector<uint8_t>;

void Put32(Bytes& b, int32_t v) {
    uint8_t t[4];
    std::memcpy(t, &v, 4);
    b.insert(b.end(), t, t + 4);
}
void PutN(Bytes& b, size_t n, uint8_t fill) { b.insert(b.end(), n, fill); }

struct Image {
    int32_t w = 8, h = 4, flags = 0, size = 0;
    Bytes data;   // exactly what the engine will read
};

// Byte offsets of interesting regions in a built file, for mutation.
struct Layout {
    size_t items_off = 0;
    size_t first_image_hdr = 0;
    size_t first_image_data = 0;
    size_t palettes_off = 0;
    size_t sounds_data_off = 0;
    size_t tail_off = 0;   // after the end-of-core DWORD
};

struct CoreSpec {
    int n_scripts = 3;
    int n_items = 12;
    std::vector<Image> images;
    Bytes sound = Bytes(64, 0x55);
};

Bytes BuildCore(const CoreSpec& spec, Layout* lay) {
    Bytes b;
    const char sig[16] = "2DKGT2K";
    b.insert(b.end(), sig, sig + 16);
    PutN(b, 256, 0);
    std::memcpy(b.data() + 16, "Fixture", 7);
    Put32(b, spec.n_scripts);
    for (int i = 0; i < spec.n_scripts; ++i) {
        Bytes s(39, 0);
        std::snprintf((char*)s.data(), 32, "skill%d", i);
        s[32] = (uint8_t)(i * 4);
        b.insert(b.end(), s.begin(), s.end());
    }
    Put32(b, spec.n_items);
    lay->items_off = b.size();
    for (int i = 0; i < spec.n_items; ++i) PutN(b, 16, (uint8_t)(0x10 + i));
    Put32(b, (int32_t)spec.images.size());
    for (size_t i = 0; i < spec.images.size(); ++i) {
        const Image& im = spec.images[i];
        if (i == 0) lay->first_image_hdr = b.size();
        Put32(b, 0); Put32(b, im.w); Put32(b, im.h); Put32(b, im.flags); Put32(b, im.size);
        if (i == 0) lay->first_image_data = b.size();
        b.insert(b.end(), im.data.begin(), im.data.end());
    }
    lay->palettes_off = b.size();
    for (int i = 0; i < 0x2100; ++i) b.push_back((uint8_t)(i * 7));
    Put32(b, 1);
    Bytes sh(42, 0);
    std::memcpy(sh.data() + 4, "hit.wav", 7);
    int32_t sz = (int32_t)spec.sound.size();
    std::memcpy(sh.data() + 36, &sz, 4);
    sh[40] = 1;
    b.insert(b.end(), sh.begin(), sh.end());
    lay->sounds_data_off = b.size();
    b.insert(b.end(), spec.sound.begin(), spec.sound.end());
    Put32(b, 0);   // end-of-core DWORD
    lay->tail_off = b.size();
    return b;
}

CoreSpec DefaultSpec() {
    CoreSpec s;
    Image raw;                       // raw 8x4 with a private palette
    raw.flags = 1;
    raw.data.assign(0x400 + 32, 0x22);
    Image rle;                       // "compressed": explicit size
    rle.w = 16; rle.h = 16; rle.size = 40;
    rle.data.assign(40, 0x33);
    s.images = {raw, rle};
    return s;
}

Bytes BuildPlayer(const CoreSpec& spec, Layout* lay) {
    Bytes b = BuildCore(spec, lay);
    for (int i = 0; i < 3; ++i) Put32(b, 0);   // three counted tables, empty
    for (int i = 0; i < 0x2b66 + 0x74b + 0x507c + 0x11ac; ++i)
        b.push_back((uint8_t)(i * 13));
    return b;
}

Bytes BuildStage(Layout* lay) {
    Bytes b = BuildCore(DefaultSpec(), lay);
    Put32(b, 3);           // bgm_sound_id
    PutN(b, 1029, 0);
    return b;
}

constexpr size_t kSysCommonImages = 58420;
constexpr size_t kSysHitstop      = 20005;   // block_hitstop_frames

Bytes BuildKgt(const std::vector<std::string>& chars,
               const std::vector<std::string>& stages, Layout* lay) {
    Bytes b = BuildCore(DefaultSpec(), lay);
    Bytes sys(0x1023C, 0);
    for (size_t i = 0; i < chars.size(); ++i)
        std::memcpy(sys.data() + i * 256, chars[i].data(), chars[i].size());
    for (size_t i = 0; i < stages.size(); ++i)
        std::memcpy(sys.data() + 20008 + i * 256, stages[i].data(), stages[i].size());
    sys[kSysHitstop] = 8;
    b.insert(b.end(), sys.begin(), sys.end());
    return b;
}

uint64_t Digest(FileKind k, const Bytes& b) {
    MemorySource src(b.data(), b.size());
    uint64_t d = 0;
    REQUIRE(FileDigest(k, src, &d));
    return d;
}

void WriteFile(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)b.data(), (std::streamsize)b.size());
}

Bytes ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// FileProvider over a plain directory (names are used as-is).
class TestDirProvider final : public FileProvider {
public:
    explicit TestDirProvider(fs::path d) : dir_(std::move(d)) {}
    bool Stat(const std::string& name, std::string* key, uint64_t* size,
              uint64_t* mtime) override {
        std::error_code ec;
        const fs::path p = dir_ / name;
        if (!fs::is_regular_file(p, ec)) return false;
        *key = p.string();
        *size = fs::file_size(p, ec);
        *mtime = (uint64_t)fs::last_write_time(p, ec).time_since_epoch().count();
        return true;
    }
    ByteSource* Open(const std::string& name) override {
        auto* h = new Held;
        h->bytes = ReadFile(dir_ / name);
        h->src = std::make_unique<MemorySource>(h->bytes.data(), h->bytes.size());
        held_.emplace_back(h);
        return h->src.get();
    }
    void Close(ByteSource*) override {}
private:
    struct Held { Bytes bytes; std::unique_ptr<MemorySource> src; };
    fs::path dir_;
    std::vector<std::unique_ptr<Held>> held_;
};

fs::path FreshDir(const char* name) {
    // Next to the test binary (tests/build/, gitignored), whatever the cwd.
    std::error_code ec0;
    fs::path base = fs::read_symlink("/proc/self/exe", ec0).parent_path();
    if (ec0 || base.empty()) base = fs::current_path();
    fs::path d = base / "gameplay_tag_fixtures" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// A whole synthetic game folder: kgt + exe + 2 characters + 1 stage.
struct Game {
    fs::path dir;
    Layout player_lay;
    explicit Game(const char* name) : dir(FreshDir(name)) {
        Layout l;
        WriteFile(dir / "game.kgt", BuildKgt({"alice", "bob", "carol"}, {"dojo"}, &l));
        WriteFile(dir / "game.exe", Bytes(4096, 0x90));
        WriteFile(dir / "alice.player", BuildPlayer(DefaultSpec(), &player_lay));
        WriteFile(dir / "bob.player", BuildPlayer(DefaultSpec(), &l));
        WriteFile(dir / "dojo.stage", BuildStage(&l));
    }
    uint32_t Tag(std::vector<uint8_t> chars = {0, 1}, std::vector<uint8_t> st = {0}) {
        TestDirProvider fp(dir);
        Participants p;
        p.chars = chars;
        p.stages = st;
        return ComputeTag(fp, "game.kgt", "game.exe", p);
    }
    void Patch(const char* file, size_t off, uint8_t v) {
        Bytes b = ReadFile(dir / file);
        REQUIRE(off < b.size());
        b[off] = v;
        WriteFile(dir / file, b);
    }
};

}  // namespace

TEST_CASE("gameplay tag: palette bytes are cosmetic") {
    Layout lay;
    const Bytes base = BuildPlayer(DefaultSpec(), &lay);
    Bytes mod = base;
    for (size_t i = 0; i < 0x2100; ++i) mod[lay.palettes_off + i] ^= 0xA5;
    CHECK(Digest(FileKind::Player, base) == Digest(FileKind::Player, mod));
}

TEST_CASE("gameplay tag: sprite pixels, private palette and RLE re-encode are cosmetic") {
    Layout lay;
    const CoreSpec spec = DefaultSpec();
    const Bytes base = BuildPlayer(spec, &lay);

    Bytes px = base;   // recolor: pixel + private-palette bytes
    for (size_t i = 0; i < spec.images[0].data.size(); ++i) px[lay.first_image_data + i] ^= 0xFF;
    CHECK(Digest(FileKind::Player, base) == Digest(FileKind::Player, px));

    CoreSpec re = spec;   // re-encoded to a different stored size
    re.images[1].size = 77;
    re.images[1].data.assign(77, 0x44);
    Layout l2;
    const Bytes reenc = BuildPlayer(re, &l2);
    CHECK(reenc.size() != base.size());
    CHECK(Digest(FileKind::Player, base) == Digest(FileKind::Player, reenc));

    CoreSpec dims = spec;   // redrawn at new dimensions, extra frame added
    dims.images[0].w = 10;
    dims.images[0].data.assign(0x400 + 40, 0x22);
    Image extra; extra.data.assign(32, 1);
    dims.images.push_back(extra);
    const Bytes redraw = BuildPlayer(dims, &l2);
    CHECK(Digest(FileKind::Player, base) == Digest(FileKind::Player, redraw));
}

TEST_CASE("gameplay tag: sounds are cosmetic") {
    Layout lay;
    const Bytes base = BuildPlayer(DefaultSpec(), &lay);
    CoreSpec s = DefaultSpec();
    s.sound.assign(300, 0x99);
    Layout l2;
    CHECK(Digest(FileKind::Player, base) == Digest(FileKind::Player, BuildPlayer(s, &l2)));
}

TEST_CASE("gameplay tag: script steps and the character tail are gameplay") {
    Layout lay;
    const Bytes base = BuildPlayer(DefaultSpec(), &lay);
    Bytes step = base;
    step[lay.items_off + 16 * 5 + 3] ^= 1;
    CHECK(Digest(FileKind::Player, base) != Digest(FileKind::Player, step));
    Bytes tail = base;
    tail[lay.tail_off + 100] ^= 1;
    CHECK(Digest(FileKind::Player, base) != Digest(FileKind::Player, tail));
}

TEST_CASE("gameplay tag: kgt system block -- hitstop and system skills count, editor image names do not") {
    Layout lay;
    const Bytes base = BuildKgt({"alice"}, {"dojo"}, &lay);
    Bytes names = base;
    names[lay.tail_off + kSysCommonImages + 10] = 'x';
    CHECK(Digest(FileKind::Kgt, base) == Digest(FileKind::Kgt, names));
    Bytes hs = base;
    hs[lay.tail_off + kSysHitstop] = 12;
    CHECK(Digest(FileKind::Kgt, base) != Digest(FileKind::Kgt, hs));
    Bytes sk = base;   // system skill table (round animation skill index)
    sk[lay.tail_off + 64820 + 0x1A] = 3;
    CHECK(Digest(FileKind::Kgt, base) != Digest(FileKind::Kgt, sk));

    MemorySource src(base.data(), base.size());
    Roster r;
    REQUIRE(ParseKgtRoster(src, &r));
    CHECK(r.chars[0] == "alice");
    CHECK(r.chars[1].empty());
    CHECK(r.stages[0] == "dojo");
}

TEST_CASE("gameplay tag: stage bgm is cosmetic") {
    Layout lay;
    const Bytes base = BuildStage(&lay);
    Bytes bgm = base;
    bgm[lay.tail_off] = 9;
    CHECK(Digest(FileKind::Stage, base) == Digest(FileKind::Stage, bgm));
}

TEST_CASE("gameplay tag: folder-level -- unrelated .player files never matter") {
    ClearCache();
    Game g("extra_player");
    const uint32_t t0 = g.Tag();
    REQUIRE(t0 != 0);

    Layout l;   // a skin / backup / unused roster character appears
    WriteFile(g.dir / "alice_skin.player", BuildPlayer(DefaultSpec(), &l));
    WriteFile(g.dir / "zzz_backup.kgt", Bytes(100, 1));
    WriteFile(g.dir / "carol.player", BuildPlayer(DefaultSpec(), &l));
    CHECK(g.Tag() == t0);

    // A palette edit of a PARTICIPATING character, which grows nothing.
    g.Patch("alice.player", g.player_lay.palettes_off + 40, 0x01);
    CHECK(g.Tag() == t0);

    // A script step edit of a participating character does change it...
    g.Patch("alice.player", g.player_lay.items_off + 7, 0xEE);
    const uint32_t t1 = g.Tag();
    CHECK(t1 != t0);
    // ...but only for matches that use her.
    Game h("extra_player_b");
    CHECK(h.Tag({1, 1}) == g.Tag({1, 1}));
}

TEST_CASE("gameplay tag: missing participant, unknown exe and order") {
    ClearCache();
    Game g("missing");
    const uint32_t t0 = g.Tag();
    CHECK(g.Tag({1, 0}) == t0);   // participants are a set
    fs::remove(g.dir / "bob.player");
    CHECK(g.Tag() != t0);
    TestDirProvider fp(g.dir);
    CHECK(ComputeTag(fp, "game.kgt", "nope.exe", Participants{}) == 0);
    CHECK(ComputeTag(fp, "nope.kgt", "game.exe", Participants{}) == 0);
}

TEST_CASE("gameplay tag: digest cache hits on rescan, misses after a change") {
    ClearCache();
    Game g("cache");
    g.Tag();
    const size_t miss0 = CacheMisses();
    CHECK(miss0 == 5);   // kgt, exe, 2 players, stage
    g.Tag();
    CHECK(CacheMisses() == miss0);
    CHECK(CacheHits() >= 5);
    Layout l;   // resized file -> new identity -> re-digested
    CoreSpec s = DefaultSpec();
    s.sound.assign(128, 1);
    WriteFile(g.dir / "bob.player", BuildPlayer(s, &l));
    g.Tag();
    CHECK(CacheMisses() == miss0 + 1);
}

TEST_CASE("gameplay tag: header block round-trip and overflow") {
    uint8_t hdr[256] = {};
    Participants p;
    p.chars = {7, 3, 7};
    p.stages = {2};
    EncodeHeader(hdr, 0xDEADBEEF, p);
    uint32_t tag = 0;
    Participants q;
    REQUIRE(DecodeHeader(hdr, &tag, &q));
    CHECK(tag == 0xDEADBEEF);
    CHECK(q.chars == std::vector<uint8_t>{3, 7});
    CHECK(q.stages == std::vector<uint8_t>{2});
    CHECK_FALSE(q.whole_roster);
    CHECK(hdr[kHdrOffTag - 1] == 0);   // nothing written below the block

    Participants big;
    for (int i = 0; i < 40; ++i) big.chars.push_back((uint8_t)i);
    EncodeHeader(hdr, 5, big);
    REQUIRE(DecodeHeader(hdr, &tag, &q));
    CHECK(q.whole_roster);
    CHECK(q.chars.empty());

    uint8_t zero[256] = {};
    CHECK_FALSE(DecodeHeader(zero, &tag, &q));   // pre-v2 file
}

TEST_CASE("gameplay tag: a real .player from a local game (skips if absent)") {
    const char* env = std::getenv("FM2K_TEST_GAME_DIR");
    const fs::path root = env ? env : "/mnt/d/games/fm2k";
    std::error_code ec;
    fs::path found;
    if (fs::is_directory(root, ec)) {
        for (auto it = fs::recursive_directory_iterator(root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
            if (it->path().extension() == ".player") { found = it->path(); break; }
        }
    }
    if (found.empty()) {
        MESSAGE("no local FM2K game under " << root.string()
                << " (set FM2K_TEST_GAME_DIR) -- real-file case skipped");
        return;
    }
    MESSAGE("real-file case: " << found.string());
    const Bytes base = ReadFile(found);
    // Locate the regions the same way the engine does.
    auto rd = [&](size_t o) { int32_t v; std::memcpy(&v, base.data() + o, 4); return v; };
    size_t o = 272;
    o += 4 + (size_t)rd(o) * 39;
    const size_t items = o + 4;
    const int32_t n_items = rd(o);
    o += 4 + (size_t)n_items * 16;
    const int32_t n_img = rd(o);
    o += 4;
    size_t first_px = 0, first_len = 0;
    for (int32_t i = 0; i < n_img; ++i) {
        const int32_t w = rd(o + 4), h = rd(o + 8), fl = rd(o + 12), sz = rd(o + 16);
        size_t n = (size_t)w * (size_t)h + ((fl & 1) ? 0x400 : 0);
        if (sz) n = (size_t)sz;
        o += 20;
        if (!first_len && n) { first_px = o; first_len = n; }
        o += n;
    }
    const size_t pal = o;

    const uint64_t d0 = Digest(FileKind::Player, base);
    Bytes m = base;
    for (size_t i = 0; i < 0x2100; ++i) m[pal + i] ^= 0x5A;
    for (size_t i = 0; i < first_len; ++i) m[first_px + i] ^= 0x3C;
    CHECK(Digest(FileKind::Player, m) == d0);
    if (n_items > 0) {
        Bytes s = base;
        s[items + 1] ^= 1;
        CHECK(Digest(FileKind::Player, s) != d0);
    }
}
