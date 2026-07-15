// app_state.h -- 2dfm Studio UI shell: view-model seam.
//
// Panels bind to StudioModel ONLY. The production implementation is
// RealModel (real_model.h): kgt::KgtFile + kgt::BuildSoundXref +
// kgt::PcmAudio projected into the row shapes below. This header stays
// free of SDL/ImGui includes so the data layer compiles headlessly
// (studio/tests/model/).
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

struct SDL_Window;

namespace kgt {
struct PcmAudio;
}

namespace studio {

class AudioPlayer;

// One sound-table row. Mirrors kgt::Sound + what the real binding
// derives per slot (format probe, xref count, engine-validity, RMS).
struct SoundRow {
    std::string name;        // kgt::SoundName() (CP932 -> UTF-8)
    uint8_t sound_type = 1;  // low nibble 0=stop-all 1=WAV 2=MIDI 3=CD; 0x10=loop
    size_t size = 0;         // Sound.data.size()
    std::string fmt;         // "PCM 32728Hz 16-bit mono" (audio_convert probe)
    int use_count = 0;       // xref uses across all scripts
    bool valid = true;       // engine RIFF-walker verdict
    double rms_dbfs = 0.0;   // kgt::RmsDbfs
};

// One usage site. Mirrors kgt::SoundUse + the resolved script name.
struct UseRow {
    std::string action;      // script name
    int item = 0;            // items[] index of the S block
    int tick = 0;
    int sprite = -1;
    bool tick_estimated = false;  // SF/SG/SC preceded it -> shown as '~tick'
};

// The one seam between panels and data.
struct StudioModel {
    virtual ~StudioModel() = default;
    virtual const std::vector<SoundRow>& Sounds() const = 0;
    virtual const std::vector<UseRow>& Uses(int sound) const = 0;
    // Mono preview samples in [-1,1] for the waveform strip (empty = no PCM).
    virtual const std::vector<float>& Waveform(int sound) const = 0;
    virtual double Seconds(int sound) const = 0;
    // Decoded PCM for playback; nullptr = nothing playable in that slot.
    virtual const kgt::PcmAudio* Pcm(int /*sound*/) const { return nullptr; }
};

// Cross-panel state, owned by main().
struct AppState {
    StudioModel* model = nullptr;
    AudioPlayer* player = nullptr;  // owned by main(); null = audio unavailable
    SDL_Window* window = nullptr;   // dialog parent
    int selected = 0;
    bool playing = false;
    std::string opened_path;        // shown in the title bar
    std::string replace_path;       // last Replace pick (not applied yet)
    std::string load_error;         // nonempty -> "Open failed" modal
    bool quit = false;

    // SDL file-dialog results land here; the callback may run off-thread
    // (see SDL_ShowOpenFileDialog docs), main loop consumes under the lock.
    std::mutex dlg_mutex;
    std::string pending_open, pending_replace;
};

std::string TypeLabel(uint8_t sound_type);  // "WAV", "MIDI loop", ...
std::string SizeLabel(size_t bytes);        // "73,772 B"

void DrawSoundTable(AppState& st);
void DrawSoundDetail(AppState& st);
void DrawUsagePanel(AppState& st);

}  // namespace studio
