// real_model.cpp -- see real_model.h. All derived data is computed here,
// once, at Load(): row projections (format probe via DecodeEngineWav,
// xref use counts, engine-validity, RMS), per-sound use lists (script
// names resolved + trimmed), and mono waveform previews.
#include "real_model.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

#include "../core/xref.h"

namespace studio {
namespace {

bool ReadAll(const std::string& path, std::vector<uint8_t>* out,
             std::string* err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        *err = "cannot open file: " + path;
        return false;
    }
    const std::streamoff n = in.tellg();
    if (n < 0) {
        *err = "cannot size file: " + path;
        return false;
    }
    out->resize(static_cast<size_t>(n));
    in.seekg(0);
    if (n > 0 && !in.read(reinterpret_cast<char*>(out->data()), n)) {
        *err = "read failed: " + path;
        return false;
    }
    return true;
}

// CP932 name fields are space-padded in the 2dfm editor (e.g. Bewear's
// "Start "); trim for display.
std::string TrimTrailingSpaces(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
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
    auto bail = [&](std::string msg) {
        if (err) *err = std::move(msg);
        return false;
    };

    kgt::FileType type;
    if (!kgt::FileTypeFromPath(path, &type))
        return bail("unsupported extension (want .player/.stage/.demo/.kgt)");

    std::vector<uint8_t> bytes;
    {
        std::string e;
        if (!ReadAll(path, &bytes, &e)) return bail(std::move(e));
    }
    if (kgt::IsFm95(bytes.data(), bytes.size()))
        return bail("FM95-family file (KGTGAME/2DKGT95 signature) -- "
                    "2dfm Studio only opens FM2K files");

    kgt::KgtFile f;
    {
        std::string e;
        if (!kgt::Parse(bytes.data(), bytes.size(), type, &f, &e))
            return bail("parse failed: " + e);
    }

    // Build every cache into locals first: a throw/early-out must not
    // leave the model half-swapped (Parse itself never partially succeeds).
    const kgt::SoundXref x = kgt::BuildSoundXref(f);
    const size_t n = f.sounds.size();
    std::vector<SoundRow> sounds;
    std::vector<std::vector<UseRow>> uses(n);
    std::vector<kgt::PcmAudio> pcm(n);
    std::vector<std::vector<float>> wave(n);
    sounds.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        const kgt::Sound& sd = f.sounds[i];
        SoundRow r;
        r.name = kgt::SoundName(sd);
        r.sound_type = sd.sound_type;
        r.size = sd.data.size();

        const uint8_t nib = sd.sound_type & 0x0F;
        if (nib == 1) {  // WAV: probe with the engine's own RIFF walk
            if (sd.data.empty()) {
                r.fmt = "(empty slot)";
            } else {
                std::string werr;
                if (kgt::DecodeEngineWav(sd.data.data(), sd.data.size(),
                                         &pcm[i], &werr)) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "PCM %uHz %u-bit %s",
                                  unsigned(pcm[i].rate), unsigned(pcm[i].bits),
                                  pcm[i].channels == 2 ? "stereo" : "mono");
                    r.fmt = buf;
                    r.rms_dbfs = kgt::RmsDbfs(pcm[i]);
                    wave[i] = BuildWaveform(pcm[i]);
                } else {
                    r.fmt = werr;  // e.g. "engine wav: RIFF size overstates..."
                    r.valid = false;
                }
            }
        } else if (nib == 0) {
            r.fmt = sd.data.empty() ? "stop-all slot (no data)"
                                    : "stop-all slot";
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

        auto it = x.uses.find(int(i));
        if (it != x.uses.end()) {
            uses[i].reserve(it->second.size());
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
                uses[i].push_back(std::move(ur));
            }
        }
        r.use_count = int(uses[i].size());
        sounds.push_back(std::move(r));
    }

    // Commit.
    file_ = std::move(f);
    sounds_ = std::move(sounds);
    uses_ = std::move(uses);
    pcm_ = std::move(pcm);
    wave_ = std::move(wave);
    path_ = path;
    loaded_ = true;
    return true;
}

std::string RealModel::CountsLabel() const {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%lu scripts, %lu sprites, %lu sounds",
                  static_cast<unsigned long>(file_.scripts.size()),
                  static_cast<unsigned long>(file_.sprites.size()),
                  static_cast<unsigned long>(file_.sounds.size()));
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
