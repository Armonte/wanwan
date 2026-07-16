// model_test.cpp -- headless gate for the real StudioModel binding
// (studio/app/real_model.*). Loads a real Bewear.player and asserts the
// design-doc expectations end-to-end through the data layer -- rows,
// uses, format probe, RMS -- with no SDL and no ImGui linked (that
// separation is the point of this gate; see real_model.h).
//
// Oracle values: docs/dev/2dfm_studio_design.md + the xref fixture gate
// (studio/tests/xref/) over the same file.
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

#include "real_model.h"

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

void TestLoadFailures(studio::RealModel& m) {
    std::string err;

    // Unknown extension refused before any I/O.
    err.clear();
    CHECK(!m.Load("whatever.txt", &err));
    CHECK(err.find("extension") != std::string::npos);

    // Missing file surfaces a readable error.
    err.clear();
    CHECK(!m.Load("/nonexistent/dir/foo.player", &err));
    CHECK(!err.empty());

    CHECK(!m.Loaded());
    CHECK(m.Sounds().empty());
}

void TestBewear(studio::RealModel& m, const std::string& path) {
    std::string err;
    if (!m.Load(path, &err)) {
        std::fprintf(stderr, "FAIL: Load(%s): %s\n", path.c_str(), err.c_str());
        ++g_failures;
        return;
    }
    CHECK(m.Loaded());
    CHECK_EQ(m.Path(), path);
    CHECK(m.CountsLabel().find("sounds") != std::string::npos);

    const std::vector<studio::SoundRow>& sounds = m.Sounds();
    CHECK_EQ(sounds.size(), 37u);
    if (sounds.size() != 37u) return;

    // Row 7 = bewear_cry.wav, used by 'Start' @tick 163 sprite 288
    // (script name space-padding trimmed; the tick is a jump estimate,
    // shown as '~163' -- see xref_test.cpp for the raw SoundUse gate).
    CHECK_EQ(sounds[7].name, std::string("bewear_cry.wav"));
    {
        const std::vector<studio::UseRow>& uses = m.Uses(7);
        CHECK_EQ(int(uses.size()), sounds[7].use_count);
        bool found = false;
        for (const studio::UseRow& u : uses)
            if (u.action == "Start" && u.tick == 163 && u.sprite == 288)
                found = true;
        CHECK(found);
    }

    // Rows 3 and 21 are the unused pair (footstep1.wav / steps.wav).
    CHECK_EQ(sounds[3].use_count, 0);
    CHECK(m.Uses(3).empty());
    CHECK_EQ(sounds[21].use_count, 0);
    CHECK(m.Uses(21).empty());

    // Row 2 format probe (engine RIFF walk): 22050Hz 16-bit, clean tier.
    CHECK(sounds[2].fmt.find("22050") != std::string::npos);
    CHECK(sounds[2].fmt.find("16-bit") != std::string::npos);
    CHECK(sounds[2].validity == studio::Validity::Ok);
    CHECK(sounds[2].validity_msg.empty());
    CHECK(!sounds[2].modified);

    // RMS for row 7: finite, sane dBFS range.
    CHECK(std::isfinite(sounds[7].rms_dbfs));
    CHECK(sounds[7].rms_dbfs > -60.0);
    CHECK(sounds[7].rms_dbfs < 0.0);

    // Playback-facing caches: PCM + waveform + duration line up.
    const kgt::PcmAudio* pcm = m.Pcm(7);
    CHECK(pcm != nullptr);
    if (pcm) {
        CHECK(pcm->rate > 0);
        CHECK(pcm->bits == 8 || pcm->bits == 16);
        CHECK(pcm->channels == 1 || pcm->channels == 2);
    }
    CHECK(!m.Waveform(7).empty());
    CHECK(m.Seconds(7) > 0.0);

    // Out-of-range accessors stay safe.
    CHECK(m.Uses(-1).empty());
    CHECK(m.Uses(999).empty());
    CHECK(m.Waveform(999).empty());
    CHECK(m.Pcm(999) == nullptr);
    CHECK_EQ(m.Seconds(-1), 0.0);

    // A failed re-load must keep the loaded contents intact.
    std::string err2;
    CHECK(!m.Load("/nonexistent/dir/foo.player", &err2));
    CHECK_EQ(m.Sounds().size(), 37u);
    CHECK(m.Loaded());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path =
        argc > 1 ? argv[1] : "/mnt/d/games/fm2k/_NODEV/pkmncc/Bewear.player";

    studio::RealModel m;
    TestLoadFailures(m);
    TestBewear(m, path);

    if (g_failures) {
        std::fprintf(stderr, "model_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("model_test: all checks passed\n");
    return 0;
}
