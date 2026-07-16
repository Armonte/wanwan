// real_model.cpp -- see real_model.h. All derived data is computed here:
// row projections (tiered validity via kgt::ProbeWav, format probe via
// DecodeEngineWav with a DecodeAudio fallback for WARN slots, xref use
// counts, RMS), per-sound use lists (script names resolved + trimmed),
// and mono waveform previews. File IO and the write path live in
// EditSession (edit_session.cpp).
#include "real_model.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "../core/xref.h"

namespace studio {
namespace {

// CP932 name fields are space-padded in the 2dfm editor (e.g. Bewear's
// "Start "); trim for display.
std::string TrimTrailingSpaces(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string JoinLines(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& p : v) {
        if (!s.empty()) s += "\n";
        s += p;
    }
    return s;
}

// Frame -> mono float in [-1,1] (u8 is unsigned around 0x80).
float MonoSample(const kgt::PcmAudio& a, size_t frame) {
    float acc = 0.f;
    if (a.bits == 8) {
        const uint8_t* s = a.pcm.data() + frame * a.channels;
        for (int k = 0; k < a.channels; ++k)
            acc += float(int(s[k]) - 128) / 128.f;
    } else {
        const uint8_t* s = a.pcm.data() + frame * a.channels * 2;
        for (int k = 0; k < a.channels; ++k) {
            int16_t v;
            std::memcpy(&v, s + k * 2, 2);
            acc += float(v) / 32768.f;
        }
    }
    return acc / float(a.channels);
}

// Mono preview for the waveform strip. Long sounds collapse to min/max
// pairs per bucket -- the strip re-min/maxes per pixel, so the envelope
// survives the decimation.
std::vector<float> BuildWaveform(const kgt::PcmAudio& a) {
    std::vector<float> w;
    const size_t frames = a.FrameCount();
    if (frames == 0) return w;
    constexpr size_t kMaxSamples = size_t(1) << 20;  // 4MB of floats, plenty
    if (frames <= kMaxSamples) {
        w.reserve(frames);
        for (size_t f = 0; f < frames; ++f) w.push_back(MonoSample(a, f));
        return w;
    }
    const size_t buckets = kMaxSamples / 2;
    w.reserve(buckets * 2);
    for (size_t b = 0; b < buckets; ++b) {
        const size_t beg = size_t(uint64_t(b) * frames / buckets);
        size_t end = size_t(uint64_t(b + 1) * frames / buckets);
        if (end <= beg) end = beg + 1;
        float lo = 1.f, hi = -1.f;
        for (size_t f = beg; f < end && f < frames; ++f) {
            const float s = MonoSample(a, f);
            lo = s < lo ? s : lo;
            hi = s > hi ? s : hi;
        }
        w.push_back(lo);
        w.push_back(hi);
    }
    return w;
}

}  // namespace

bool RealModel::Load(const std::string& path, std::string* err) {
    if (!session_.Load(path, err)) return false;

    // The session committed; rebuild every cache from its file. Rows are
    // reset first so RebuildSlot writes onto clean defaults.
    const kgt::KgtFile& f = session_.File();
    const size_t n = f.sounds.size();
    sounds_.assign(n, SoundRow{});
    uses_.assign(n, {});
    pcm_.assign(n, {});
    wave_.assign(n, {});

    const kgt::SoundXref x = kgt::BuildSoundXref(f);
    for (size_t i = 0; i < n; ++i) {
        RebuildSlot(int(i));
        auto it = x.uses.find(int(i));
        if (it != x.uses.end()) {
            uses_[i].reserve(it->second.size());
            for (const kgt::SoundUse& u : it->second) {
                UseRow ur;
                ur.action =
                    (u.script >= 0 && u.script < int(f.scripts.size()))
                        ? TrimTrailingSpaces(kgt::ScriptName(f.scripts[u.script]))
                        : std::string("?");
                ur.item = u.item;
                ur.tick = u.tick;
                ur.sprite = u.sprite;
                ur.tick_estimated = u.tick_estimated;
                uses_[i].push_back(std::move(ur));
            }
        }
        sounds_[i].use_count = int(uses_[i].size());
    }
    return true;
}

void RealModel::RebuildSlot(int i) {
    const kgt::Sound& sd = session_.File().sounds[size_t(i)];
    SoundRow& r = sounds_[size_t(i)];
    const int keep_uses = r.use_count;  // xref is payload-independent
    r = SoundRow{};
    r.use_count = keep_uses;
    r.name = kgt::SoundName(sd);
    r.sound_type = sd.sound_type;
    r.size = sd.data.size();
    r.modified = session_.SlotModified(i);
    pcm_[size_t(i)] = {};
    wave_[size_t(i)].clear();

    const uint8_t nib = sd.sound_type & 0x0F;
    if (nib == 1) {  // WAV: tiered verdict + preview
        if (sd.data.empty()) {
            r.fmt = "(empty slot)";
            return;
        }
        const kgt::WavValidity v = kgt::ProbeWav(sd.data.data(), sd.data.size());
        if (v.Fatal()) {
            r.validity = Validity::Fatal;
            std::vector<std::string> all = v.fatal;
            all.insert(all.end(), v.warnings.begin(), v.warnings.end());
            r.validity_msg = JoinLines(all);
        } else if (v.Warn()) {
            r.validity = Validity::Warn;
            r.validity_msg = JoinLines(v.warnings);
        }

        std::string werr;
        kgt::PcmAudio& pcm = pcm_[size_t(i)];
        if (kgt::DecodeEngineWav(sd.data.data(), sd.data.size(), &pcm, &werr)) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "PCM %uHz %u-bit %s",
                          unsigned(pcm.rate), unsigned(pcm.bits),
                          pcm.channels == 2 ? "stereo" : "mono");
            r.fmt = buf;
            r.rms_dbfs = kgt::RmsDbfs(pcm);
            wave_[size_t(i)] = BuildWaveform(pcm);
        } else if (r.validity != Validity::Fatal) {
            // WARN slots stay auditable: fall back to the general decoder
            // (dr_wav handles float32/24-bit) for waveform/play/RMS.
            r.fmt = kgt::ProbeDesc(v);
            std::string derr;
            if (kgt::DecodeAudio(sd.data.data(), sd.data.size(), &pcm, &derr)) {
                r.rms_dbfs = kgt::RmsDbfs(pcm);
                wave_[size_t(i)] = BuildWaveform(pcm);
            } else {
                pcm = {};  // undecodable even leniently: preview-less
            }
        } else {
            r.fmt = werr;  // Fatal: engine refusal text doubles as format
        }
    } else if (nib == 0) {
        r.fmt = sd.data.empty() ? "stop-all slot (no data)" : "stop-all slot";
    } else if (nib == 2) {
        if (sd.data.empty())
            r.fmt = "MIDI (empty slot)";
        else if (sd.data.size() >= 4 &&
                 std::memcmp(sd.data.data(), "MThd", 4) == 0)
            r.fmt = "MIDI (MThd)";
        else
            r.fmt = "MIDI (no MThd header)";
    } else if (nib == 3) {
        r.fmt = "CD audio";
    } else {
        char buf[32];
        std::snprintf(buf, sizeof buf, "unknown type 0x%02X",
                      unsigned(sd.sound_type));
        r.fmt = buf;
    }
}

void RealModel::RefreshModifiedFlags() {
    for (size_t i = 0; i < sounds_.size(); ++i)
        sounds_[i].modified = session_.SlotModified(int(i));
}

bool RealModel::ApplyReplace(const ReplacePlan& plan, std::string* err) {
    if (!session_.ApplyReplace(plan, err)) return false;
    RebuildSlot(plan.slot);
    return true;
}

int RealModel::Undo() {
    const int slot = session_.Undo();
    if (slot >= 0) RebuildSlot(slot);
    return slot;
}

int RealModel::Redo() {
    const int slot = session_.Redo();
    if (slot >= 0) RebuildSlot(slot);
    return slot;
}

bool RealModel::SaveCopy(const std::string& dest, std::string* err) {
    if (!session_.SaveCopy(dest, err)) return false;
    RefreshModifiedFlags();
    return true;
}

bool RealModel::SaveOverwrite(std::string* err) {
    if (!session_.SaveOverwrite(err)) return false;
    RefreshModifiedFlags();
    return true;
}

std::string RealModel::CountsLabel() const {
    const kgt::KgtFile& f = session_.File();
    char buf[96];
    std::snprintf(buf, sizeof buf, "%lu scripts, %lu sprites, %lu sounds",
                  static_cast<unsigned long>(f.scripts.size()),
                  static_cast<unsigned long>(f.sprites.size()),
                  static_cast<unsigned long>(f.sounds.size()));
    return buf;
}

const std::vector<UseRow>& RealModel::Uses(int i) const {
    static const std::vector<UseRow> none;
    return (i >= 0 && i < int(uses_.size())) ? uses_[i] : none;
}

const std::vector<float>& RealModel::Waveform(int i) const {
    static const std::vector<float> none;
    return (i >= 0 && i < int(wave_.size())) ? wave_[i] : none;
}

double RealModel::Seconds(int i) const {
    return (i >= 0 && i < int(pcm_.size())) ? pcm_[i].Seconds() : 0.0;
}

const kgt::PcmAudio* RealModel::Pcm(int i) const {
    if (i < 0 || i >= int(pcm_.size()) || pcm_[i].pcm.empty()) return nullptr;
    return &pcm_[i];
}

}  // namespace studio
