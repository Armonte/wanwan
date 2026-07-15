// real_model.h -- StudioModel over a parsed kgt::KgtFile.
//
// DATA LAYER ONLY: no SDL, no ImGui. This header + real_model.cpp compile
// natively for the headless gate (studio/tests/model/); playback and file
// dialogs live in the app layer (audio_player.*, main.cpp).
//
// Everything the panels read -- rows, per-sound use lists, decoded PCM,
// waveform previews, RMS -- is computed once in Load() and cached,
// index-aligned with file_.sounds.
#pragma once

#include <string>
#include <vector>

#include "../core/audio/audio_convert.h"
#include "../core/kgt_file.h"
#include "app_state.h"

namespace studio {

class RealModel : public StudioModel {
public:
    // Read + parse `path`, then rebuild every cached row/use/waveform.
    // On failure returns false with a user-facing *err and the previous
    // contents intact. Refuses FM95 files and unknown extensions.
    bool Load(const std::string& path, std::string* err);

    bool Loaded() const { return loaded_; }
    const std::string& Path() const { return path_; }
    // "299 scripts, 566 sprites, 37 sounds" -- for the title bar.
    std::string CountsLabel() const;

    const std::vector<SoundRow>& Sounds() const override { return sounds_; }
    const std::vector<UseRow>& Uses(int sound) const override;
    const std::vector<float>& Waveform(int sound) const override;
    double Seconds(int sound) const override;
    const kgt::PcmAudio* Pcm(int sound) const override;

private:
    bool loaded_ = false;
    std::string path_;
    kgt::KgtFile file_;
    // Per-sound caches, index-aligned with file_.sounds (built in Load).
    std::vector<SoundRow> sounds_;
    std::vector<std::vector<UseRow>> uses_;
    std::vector<kgt::PcmAudio> pcm_;  // decoded engine WAV (empty if none)
    std::vector<std::vector<float>> wave_;
};

}  // namespace studio
