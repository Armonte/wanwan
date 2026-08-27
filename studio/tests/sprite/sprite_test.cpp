// sprite_test.cpp -- headless gate for the usage panel's DATA path:
// core/sprite_decode.cpp plus RealModel's lazy sprite cache.
//
// The panel itself is a few ImGui calls; what can actually be wrong is
// underneath it -- a decode that fails on a compressed payload, a cache
// that outlives the file it was decoded from, or a texture key that lets
// one file's artwork render against another's xrefs. All three are testable
// without a window, which is why they are tested here rather than by
// driving the GUI.
//
// Run: ./build_native.sh [FILE.kgt]
#include <cstdio>
#include <string>

#include "real_model.h"
#include "sprite_decode.h"

namespace {

int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__,  \
                         #cond);                                          \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

bool g_no_coverage = false;

void TestSprites(studio::RealModel& m, const std::string& path) {
    std::string err;
    if (!m.Load(path, &err)) {
        std::fprintf(stderr, "load failed (%s): %s\n", path.c_str(), err.c_str());
        ++g_failures;
        return;
    }

    const uint64_t gen0 = m.Generation();
    CHECK(gen0 > 0);

    // Out-of-range must be nullptr, not a crash and not a stale entry.
    CHECK(m.Sprite(-1) == nullptr);
    CHECK(m.Sprite(1 << 30) == nullptr);

    // Walk the real xrefs and decode every sprite a sound actually plays
    // on. This is exactly the set the usage panel asks for.
    int sites = 0, decoded = 0, failed = 0;
    for (size_t s = 0; s < m.Sounds().size(); ++s) {
        for (const studio::UseRow& u : m.Uses(int(s))) {
            if (u.sprite < 0) continue;
            ++sites;
            const kgt::DecodedSprite* d = m.Sprite(u.sprite);
            if (!d) { ++failed; continue; }
            ++decoded;
            CHECK(d->ok);
            CHECK(d->width > 0 && d->height > 0);
            CHECK(d->rgba.size() == size_t(d->width) * size_t(d->height) * 4);
            // Identity, not equality: a second ask must hand back the SAME
            // cached object, or the panel re-decodes and re-uploads a
            // texture every frame.
            CHECK(m.Sprite(u.sprite) == d);
        }
    }
    std::printf("  xref sites with a sprite: %d  decoded: %d  undecodable: %d\n",
                sites, decoded, failed);
    // NO COVERAGE IS NOT A FAILURE, and must not be scored as a pass either.
    // A file can legitimately have no sound that plays on a sprite-bearing
    // site -- WonderfulWorld_ver_0946.kgt is one: it holds 85 pictures, but
    // they are stage/UI backgrounds and none of its sound xrefs carry a
    // sprite. Asserting sites > 0 there reds a gate on a healthy file;
    // silently passing claims the decoder was exercised when nothing ran.
    if (sites == 0) {
        std::printf("  NO COVERAGE -- no sound in this file plays on a "
                    "sprite-bearing site, so the decoder was never called. "
                    "Not scored as a pass.\n");
        g_no_coverage = true;
        return;
    }
    CHECK(decoded > 0);

    // Changing the shared palette must invalidate, because sprites without
    // a private palette change appearance.
    const uint64_t gen1 = m.Generation();
    m.SetSharedPalette(3);
    CHECK(m.Generation() != gen1);
    CHECK(m.SharedPalette() == 3);
    // Out-of-range and no-op writes must NOT churn the generation, or the
    // panel throws its textures away for nothing.
    const uint64_t gen2 = m.Generation();
    m.SetSharedPalette(3);
    m.SetSharedPalette(-1);
    m.SetSharedPalette(99);
    CHECK(m.Generation() == gen2);
    m.SetSharedPalette(0);

    // Re-loading the same file must bump the generation. This is the guard
    // against the cross-file cache bug: indices repeat across files, so a
    // texture cache keyed on index alone would survive an Open.
    const uint64_t gen3 = m.Generation();
    CHECK(m.Load(path, &err));
    CHECK(m.Generation() != gen3);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: sprite_test FILE.kgt\n");
        return 2;
    }
    studio::RealModel m;
    std::printf("sprite_test: %s\n", argv[1]);
    TestSprites(m, argv[1]);
    if (g_failures) {
        std::fprintf(stderr, "sprite_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    if (g_no_coverage) {
        std::printf("sprite_test: NO COVERAGE (rc=3) -- nothing was tested\n");
        return 3;
    }
    std::printf("sprite_test: all checks passed\n");
    return 0;
}
