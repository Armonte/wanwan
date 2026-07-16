// write_test.cpp -- headless gate for the Phase 4 write path
// (studio/app/edit_session.* + the RealModel wrappers): replace pipeline,
// undo/redo, Save-As-Copy, overwrite + .bak discipline, validity tiers,
// preview fallback, and the pre-edit gate wiring. No SDL, no ImGui.
//
// Everything runs against COPIES of the corpus file in a fresh temp dir;
// the source file is never opened for writing.
//
// Oracle: docs/dev/2dfm_studio_design.md "File-safety workflow (Phase 4
// contract)" + tools/kgt/soundtool.py cmd_replace/walk_wav semantics.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "edit_session.h"
#include "real_model.h"

namespace {

int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

#define CHECK_MSG(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, \
                     #cond, (msg)); \
        ++g_failures; \
    } \
} while (0)

std::vector<uint8_t> MustRead(const std::string& path) {
    std::vector<uint8_t> b;
    std::string err;
    if (!studio::ReadFileBytes(path, &b, &err)) {
        std::fprintf(stderr, "FATAL: %s\n", err.c_str());
        std::exit(2);
    }
    return b;
}

void MustWrite(const std::string& path, const std::vector<uint8_t>& b) {
    std::string err;
    if (!studio::WriteFileBytes(path, b, &err)) {
        std::fprintf(stderr, "FATAL: %s\n", err.c_str());
        std::exit(2);
    }
}

kgt::KgtFile MustParse(const std::vector<uint8_t>& bytes) {
    kgt::KgtFile f;
    std::string err;
    if (!kgt::Parse(bytes.data(), bytes.size(), kgt::FileType::Player, &f,
                    &err)) {
        std::fprintf(stderr, "FATAL: parse: %s\n", err.c_str());
        std::exit(2);
    }
    return f;
}

// Canonical float32 (fmt tag 3, 32-bit) WAV -- the "DAW master" shape
// that slipped into shipped games (pkmncc #46, 16c 1PMenu.stage #1).
std::vector<uint8_t> SynthFloat32Wav(uint32_t rate, uint16_t ch,
                                     size_t frames) {
    const uint32_t block = uint32_t(ch) * 4;
    const uint32_t data_bytes = uint32_t(frames) * block;
    std::vector<uint8_t> w;
    w.reserve(44 + data_bytes);
    auto put = [&](const void* s, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(s);
        w.insert(w.end(), b, b + n);
    };
    auto put_u32 = [&](uint32_t v) { put(&v, 4); };
    auto put_u16 = [&](uint16_t v) { put(&v, 2); };
    put("RIFF", 4);
    put_u32(36 + data_bytes);
    put("WAVE", 4);
    put("fmt ", 4);
    put_u32(16);
    put_u16(3);  // IEEE float
    put_u16(ch);
    put_u32(rate);
    put_u32(rate * block);
    put_u16(uint16_t(block));
    put_u16(32);
    put("data", 4);
    put_u32(data_bytes);
    for (size_t f = 0; f < frames; ++f) {
        const float v =
            0.5f * std::sin(2.0 * M_PI * 440.0 * double(f) / rate);
        for (uint16_t k = 0; k < ch; ++k) put(&v, 4);
    }
    return w;
}

// ── probe unit checks (walk_wav mirror) ──────────────────────────────────

void TestProbe(const std::vector<uint8_t>& engine_wav) {
    // Garbage: not RIFF at all.
    {
        const char* junk = "this is not audio";
        const kgt::WavValidity v =
            kgt::ProbeWav(reinterpret_cast<const uint8_t*>(junk),
                          std::strlen(junk));
        CHECK(v.Fatal());
        CHECK(!v.fatal.empty() &&
              v.fatal[0].find("not a RIFF/WAVE") != std::string::npos);
    }
    // A known-good engine WAV probes clean.
    {
        const kgt::WavValidity v =
            kgt::ProbeWav(engine_wav.data(), engine_wav.size());
        CHECK_MSG(v.Ok(), v.Fatal() ? v.fatal[0].c_str()
                                    : (v.warnings.empty()
                                           ? "?"
                                           : v.warnings[0].c_str()));
    }
    // Truncation: RIFF size field now overstates the buffer -> FATAL.
    {
        std::vector<uint8_t> cut(engine_wav.begin(),
                                 engine_wav.begin() + engine_wav.size() / 2);
        const kgt::WavValidity v = kgt::ProbeWav(cut.data(), cut.size());
        CHECK(v.Fatal());
        bool overstate = false;
        for (const std::string& m : v.fatal)
            if (m.find("overstates") != std::string::npos) overstate = true;
        CHECK(overstate);
    }
    // RIFF/WAVE with fmt but no data chunk -> FATAL "no data chunk".
    {
        std::vector<uint8_t> w = SynthFloat32Wav(22050, 1, 4);
        w.resize(44 - 8);          // drop the data chunk header + body
        w[4] = uint8_t(w.size() - 8);  // keep the RIFF size truthful
        w[5] = w[6] = w[7] = 0;
        const kgt::WavValidity v = kgt::ProbeWav(w.data(), w.size());
        bool nodata = false;
        for (const std::string& m : v.fatal)
            if (m.find("no data chunk") != std::string::npos) nodata = true;
        CHECK(nodata);
    }
    // Float32: walk passes (WARN tier), engine decoder refuses, general
    // decoder handles it -- the preview-fallback contract.
    {
        const std::vector<uint8_t> w = SynthFloat32Wav(44100, 2, 512);
        const kgt::WavValidity v = kgt::ProbeWav(w.data(), w.size());
        CHECK(v.Warn());
        CHECK(!v.warnings.empty());
        kgt::PcmAudio pcm;
        std::string err;
        CHECK(!kgt::DecodeEngineWav(w.data(), w.size(), &pcm, &err));
        CHECK(kgt::DecodeAudio(w.data(), w.size(), &pcm, &err));
        CHECK(pcm.bits == 16 && pcm.channels == 2 && pcm.rate == 44100);
    }
}

// ── (1) replace + SaveCopy: only the touched slot differs ────────────────

void TestReplaceAndSaveCopy(const std::string& dir,
                            const std::vector<uint8_t>& src_bytes) {
    const std::string copy = dir + "/t1.player";
    MustWrite(copy, src_bytes);

    studio::EditSession s;
    std::string err;
    CHECK_MSG(s.Load(copy, &err), err.c_str());
    CHECK(s.Editable());
    CHECK(!s.Dirty());

    const std::vector<uint8_t> slot2 = s.File().sounds[2].data;
    const std::vector<uint8_t> old7 = s.File().sounds[7].data;

    studio::ReplacePlan plan;
    CHECK_MSG(s.PlanReplace(7, slot2.data(), slot2.size(), &plan, &err),
              err.c_str());
    CHECK(plan.slot == 7);
    CHECK(!plan.is_midi);
    CHECK(plan.dst_desc.find("16-bit") != std::string::npos);
    {   // the stored payload must itself pass the strict engine decoder
        kgt::PcmAudio pcm;
        CHECK(kgt::DecodeEngineWav(plan.payload.data(), plan.payload.size(),
                                   &pcm, &err));
        CHECK(kgt::ProbeWav(plan.payload.data(), plan.payload.size()).Ok());
    }
    CHECK_MSG(s.ApplyReplace(plan, &err), err.c_str());
    CHECK(s.SlotModified(7));
    CHECK(!s.SlotModified(2));
    CHECK(s.Dirty());

    const std::string dest = dir + "/t1_edited.player";
    CHECK_MSG(s.SaveCopy(dest, &err), err.c_str());
    CHECK(s.Path() == dest);   // session adopted the copy
    CHECK(!s.Dirty());
    CHECK(!studio::FileExists(dest + ".tmp"));  // no droppings

    const std::vector<uint8_t> out = MustRead(dest);
    CHECK(out.size() ==
          src_bytes.size() - old7.size() + plan.payload.size());

    kgt::KgtFile re = MustParse(out);
    const kgt::KgtFile orig = MustParse(src_bytes);
    CHECK(re.sounds.size() == orig.sounds.size());
    CHECK(re.sounds[7].data == plan.payload);
    for (size_t j = 0; j < re.sounds.size(); ++j)
        if (j != 7) CHECK_MSG(re.sounds[j].data == orig.sounds[j].data,
                              ("sound " + std::to_string(j)).c_str());
    // Byte-identity of every non-sound region: restoring slot 7's pristine
    // payload must reproduce the source file exactly.
    re.sounds[7].data = old7;
    CHECK(kgt::Serialize(re) == src_bytes);
}

// ── (2) overwrite: .bak = oldest original, never rewritten ───────────────

void TestOverwriteBak(const std::string& dir,
                      const std::vector<uint8_t>& src_bytes) {
    const std::string copy = dir + "/t2.player";
    const std::string bak = copy + ".bak";
    MustWrite(copy, src_bytes);

    studio::EditSession s;
    std::string err;
    CHECK_MSG(s.Load(copy, &err), err.c_str());
    const std::vector<uint8_t> slot2 = s.File().sounds[2].data;

    studio::ReplacePlan plan;
    CHECK_MSG(s.PlanReplace(7, slot2.data(), slot2.size(), &plan, &err),
              err.c_str());
    CHECK_MSG(s.ApplyReplace(plan, &err), err.c_str());
    CHECK(!studio::FileExists(bak));
    CHECK_MSG(s.SaveOverwrite(&err), err.c_str());
    CHECK(!s.Dirty());
    CHECK(studio::FileExists(bak));
    CHECK(MustRead(bak) == src_bytes);           // .bak == ORIGINAL bytes
    const std::vector<uint8_t> after_first = MustRead(copy);
    CHECK(after_first != src_bytes);

    // Second overwrite must NOT rewrite the .bak.
    studio::ReplacePlan plan2;
    CHECK_MSG(s.PlanReplace(3, slot2.data(), slot2.size(), &plan2, &err),
              err.c_str());
    CHECK_MSG(s.ApplyReplace(plan2, &err), err.c_str());
    CHECK_MSG(s.SaveOverwrite(&err), err.c_str());
    CHECK(MustRead(bak) == src_bytes);           // still the oldest original
    CHECK(MustRead(copy) != after_first);

    // SaveCopy onto the session's own path keeps the .bak discipline too.
    const std::string copy3 = dir + "/t2b.player";
    MustWrite(copy3, src_bytes);
    studio::EditSession s3;
    CHECK_MSG(s3.Load(copy3, &err), err.c_str());
    studio::ReplacePlan plan3;
    CHECK_MSG(s3.PlanReplace(7, slot2.data(), slot2.size(), &plan3, &err),
              err.c_str());
    CHECK_MSG(s3.ApplyReplace(plan3, &err), err.c_str());
    CHECK_MSG(s3.SaveCopy(copy3, &err), err.c_str());  // dest == path
    CHECK(studio::FileExists(copy3 + ".bak"));
    CHECK(MustRead(copy3 + ".bak") == src_bytes);
}

// ── (3) undo/redo ────────────────────────────────────────────────────────

void TestUndoRedo(const std::string& dir,
                  const std::vector<uint8_t>& src_bytes) {
    const std::string copy = dir + "/t3.player";
    MustWrite(copy, src_bytes);

    studio::EditSession s;
    std::string err;
    CHECK_MSG(s.Load(copy, &err), err.c_str());
    CHECK(!s.CanUndo() && !s.CanRedo());

    const std::vector<uint8_t> pristine7 = s.File().sounds[7].data;
    const std::vector<uint8_t> slot2 = s.File().sounds[2].data;
    studio::ReplacePlan plan;
    CHECK_MSG(s.PlanReplace(7, slot2.data(), slot2.size(), &plan, &err),
              err.c_str());
    CHECK_MSG(s.ApplyReplace(plan, &err), err.c_str());
    CHECK(s.File().sounds[7].data == plan.payload);
    CHECK(s.CanUndo() && !s.CanRedo());

    CHECK(s.Undo() == 7);
    CHECK(s.File().sounds[7].data == pristine7);
    CHECK(!s.SlotModified(7));
    CHECK(!s.Dirty());
    CHECK(!s.CanUndo() && s.CanRedo());

    CHECK(s.Redo() == 7);
    CHECK(s.File().sounds[7].data == plan.payload);
    CHECK(s.SlotModified(7));
    CHECK(s.CanUndo() && !s.CanRedo());

    // Bounded stack: 70 pushes cap at 64 entries (oldest dropped).
    std::vector<uint8_t> a = plan.payload, b = pristine7;
    for (int i = 0; i < 70; ++i)
        CHECK(s.ReplaceSlot(7, (i & 1) ? a : b, &err));
    int undos = 0;
    while (s.CanUndo()) {
        s.Undo();
        ++undos;
    }
    CHECK(undos == 64);

    // Reload clears both stacks.
    CHECK_MSG(s.Load(copy, &err), err.c_str());
    CHECK(!s.CanUndo() && !s.CanRedo());
}

// ── (4) pipeline refusals ────────────────────────────────────────────────

void TestRefusals(const std::string& dir,
                  const std::vector<uint8_t>& src_bytes) {
    const std::string copy = dir + "/t4.player";
    MustWrite(copy, src_bytes);

    studio::EditSession s;
    std::string err;
    CHECK_MSG(s.Load(copy, &err), err.c_str());

    studio::ReplacePlan plan;
    // Garbage bytes: refused (not decodable as any supported format).
    const char* junk = "definitely not an audio file of any kind";
    CHECK(!s.PlanReplace(7, reinterpret_cast<const uint8_t*>(junk),
                         std::strlen(junk), &plan, &err));

    // Truncated WAV: the walk_wav FATAL gate refuses before decode.
    const std::vector<uint8_t>& slot2 = s.File().sounds[2].data;
    std::vector<uint8_t> cut(slot2.begin(),
                             slot2.begin() + slot2.size() / 2);
    err.clear();
    CHECK(!s.PlanReplace(7, cut.data(), cut.size(), &plan, &err));
    CHECK_MSG(err.find("would break in-engine") != std::string::npos,
              err.c_str());

    // MIDI bytes into a WAV slot: refused (unrecognized audio).
    const uint8_t fake_mid[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6,
                                0,   0,   0,   1,   0, 96};
    CHECK(!s.PlanReplace(7, fake_mid, sizeof fake_mid, &plan, &err));

    // MIDI slot semantics (if this file has one): MThd accepted verbatim,
    // non-MThd refused.
    int midi_slot = -1;
    for (size_t j = 0; j < s.File().sounds.size(); ++j)
        if ((s.File().sounds[j].sound_type & 0x0F) == 2) {
            midi_slot = int(j);
            break;
        }
    if (midi_slot >= 0) {
        CHECK_MSG(s.PlanReplace(midi_slot, fake_mid, sizeof fake_mid, &plan,
                                &err),
                  err.c_str());
        CHECK(plan.is_midi);
        CHECK(plan.payload.size() == sizeof fake_mid);
        CHECK(!s.PlanReplace(midi_slot, slot2.data(), slot2.size(), &plan,
                             &err));
    } else {
        std::printf("NOTE: no MIDI slot in this file; MThd-acceptance "
                    "checked only for rejection paths\n");
    }
}

// ── (5) float32 fixture: WARN tier + preview fallback + convert ──────────

void TestWarnTierAndConvert(const std::string& dir,
                            const std::vector<uint8_t>& src_bytes) {
    const std::string copy = dir + "/t5.player";
    MustWrite(copy, src_bytes);

    const std::vector<uint8_t> f32 = SynthFloat32Wav(44100, 2, 4410);

    // Fabricate a file with a WARN slot (7) and a FATAL slot (3) via the
    // raw payload primitive -- exactly what a 2dfm-editor import of a DAW
    // master looks like on disk.
    studio::EditSession s;
    std::string err;
    CHECK_MSG(s.Load(copy, &err), err.c_str());
    std::vector<uint8_t> trunc(s.File().sounds[2].data);
    trunc.resize(trunc.size() / 2);
    CHECK(s.ReplaceSlot(7, f32, &err));
    CHECK(s.ReplaceSlot(3, trunc, &err));
    const std::string fixture = dir + "/t5_tiers.player";
    CHECK_MSG(s.SaveCopy(fixture, &err), err.c_str());

    // RealModel over the fixture: tiers + preview fallback.
    studio::RealModel m;
    CHECK_MSG(m.Load(fixture, &err), err.c_str());
    const auto& rows = m.Sounds();
    CHECK(rows[2].validity == studio::Validity::Ok);
    CHECK(rows[7].validity == studio::Validity::Warn);
    CHECK(!rows[7].validity_msg.empty());
    CHECK(rows[7].fmt.find("tag=3") != std::string::npos);
    CHECK(m.Pcm(7) != nullptr);        // WARN stays auditable (dr_wav path)
    CHECK(!m.Waveform(7).empty());
    CHECK(m.Seconds(7) > 0.05);
    CHECK(rows[3].validity == studio::Validity::Fatal);
    CHECK(!rows[3].validity_msg.empty());
    CHECK(m.Pcm(3) == nullptr);        // FATAL stays preview-less
    CHECK(m.Waveform(3).empty());

    // One-click "Convert to 16-bit PCM": same pipeline, source = the
    // slot's own bytes. Afterwards the slot is clean-tier and engine-safe.
    const kgt::Sound& warn_slot = m.Session().File().sounds[7];
    studio::ReplacePlan conv;
    CHECK_MSG(m.PlanReplace(7, warn_slot.data.data(), warn_slot.data.size(),
                            &conv, &err),
              err.c_str());
    CHECK(conv.dst_desc.find("16-bit") != std::string::npos);
    CHECK_MSG(m.ApplyReplace(conv, &err), err.c_str());
    CHECK(m.Sounds()[7].validity == studio::Validity::Ok);
    CHECK(m.Sounds()[7].fmt.find("16-bit") != std::string::npos);
    CHECK(m.Sounds()[7].modified);
    {
        kgt::PcmAudio pcm;
        CHECK(kgt::DecodeEngineWav(conv.payload.data(), conv.payload.size(),
                                   &pcm, &err));
        CHECK(pcm.rate == 44100 && pcm.channels == 2 && pcm.bits == 16);
    }
}

// ── (6) pre-edit gate wiring ─────────────────────────────────────────────
// A real parse-succeeds-but-round-trip-differs file cannot be fabricated:
// every parsed field is preserved verbatim or derived (Sound.size), so
// Serialize(Parse(x)) == x by construction for anything Parse accepts.
// The gate WIRING is therefore exercised through the forced-verdict hook.

void TestBrowseOnlyGate(const std::string& dir,
                        const std::vector<uint8_t>& src_bytes) {
    const std::string copy = dir + "/t6.player";
    MustWrite(copy, src_bytes);

    studio::EditSession s;
    std::string err;
    CHECK_MSG(s.Load(copy, &err), err.c_str());
    CHECK(s.Editable());  // corpus file round-trips, of course
    s.ForceEditableForTest(false);
    CHECK(s.Loaded() && !s.Editable());

    const std::vector<uint8_t>& slot2 = s.File().sounds[2].data;
    studio::ReplacePlan plan;
    err.clear();
    CHECK(!s.PlanReplace(7, slot2.data(), slot2.size(), &plan, &err));
    CHECK_MSG(err.find("browse-only") != std::string::npos, err.c_str());
    CHECK(!s.ReplaceSlot(7, slot2, &err));
    CHECK(!s.SaveOverwrite(&err));
    CHECK(!s.SaveCopy(dir + "/t6_nope.player", &err));
    CHECK(!studio::FileExists(dir + "/t6_nope.player"));
    CHECK(!studio::FileExists(copy + ".bak"));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string source =
        argc > 1 ? argv[1] : "/mnt/d/games/fm2k/_NODEV/pkmncc/Bewear.player";

    char tmpl[] = "/tmp/kgt_write_test_XXXXXX";
    const char* dirc = mkdtemp(tmpl);
    if (!dirc) {
        std::fprintf(stderr, "FATAL: mkdtemp failed\n");
        return 2;
    }
    const std::string dir = dirc;
    std::printf("write_test: temp dir %s (source: %s)\n", dir.c_str(),
                source.c_str());

    const std::vector<uint8_t> src_bytes = MustRead(source);

    {   // engine WAV for the probe checks = slot 2 of the corpus file
        kgt::KgtFile f = MustParse(src_bytes);
        TestProbe(f.sounds[2].data);
    }
    TestReplaceAndSaveCopy(dir, src_bytes);
    TestOverwriteBak(dir, src_bytes);
    TestUndoRedo(dir, src_bytes);
    TestRefusals(dir, src_bytes);
    TestWarnTierAndConvert(dir, src_bytes);
    TestBrowseOnlyGate(dir, src_bytes);

    if (g_failures) {
        std::fprintf(stderr, "write_test: %d FAILURE(S) (temp dir kept: %s)\n",
                     g_failures, dir.c_str());
        return 1;
    }
    std::printf("write_test: all checks passed\n");
    return 0;
}
