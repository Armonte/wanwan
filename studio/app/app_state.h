// app_state.h -- 2dfm Studio UI shell: view-model seam.
//
// Panels bind to StudioModel for reads. The production implementation is
// RealModel (real_model.h): kgt::KgtFile + kgt::BuildSoundXref +
// kgt::PcmAudio projected into the row shapes below. Write actions go
// through AppState::real (RealModel wraps EditSession, the UI-free write
// path). This header stays free of SDL/ImGui includes so the data layer
// compiles headlessly (studio/tests/model/, studio/tests/write/).
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "edit_session.h"  // ReplacePlan (UI-free)

struct SDL_Window;
struct SDL_Renderer;

namespace kgt {
struct PcmAudio;
struct DecodedSprite;
}

namespace studio {

class AudioPlayer;
class RealModel;

// Engine-walk verdict tier for a WAV slot (kgt::ProbeWav, mirroring
// soundtool.py walk_wav): Fatal = the loader genuinely breaks/OOB-reads;
// Warn = plays on modern Windows but crashes legacy DirectSound stacks
// and wastes size (float32/24-bit/multichannel -- offer convert).
enum class Validity : uint8_t { Ok, Warn, Fatal };

// One sound-table row. Mirrors kgt::Sound + what the real binding
// derives per slot (format probe, xref count, validity tier, RMS).
struct SoundRow {
    std::string name;        // kgt::SoundName() (CP932 -> UTF-8)
    uint8_t sound_type = 1;  // low nibble 0=stop-all 1=WAV 2=MIDI 3=CD; 0x10=loop
    size_t size = 0;         // Sound.data.size()
    std::string fmt;         // "PCM 32728Hz 16-bit mono" (audio_convert probe)
    int use_count = 0;       // xref uses across all scripts
    Validity validity = Validity::Ok;  // tiered engine RIFF-walker verdict
    std::string validity_msg;          // '!' column tooltip (probe messages)
    bool modified = false;   // payload differs from the loaded baseline
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

// The one seam between panels and data (reads).
struct StudioModel {
    virtual ~StudioModel() = default;
    virtual const std::vector<SoundRow>& Sounds() const = 0;
    virtual const std::vector<UseRow>& Uses(int sound) const = 0;
    // Mono preview samples in [-1,1] for the waveform strip (empty = no PCM).
    virtual const std::vector<float>& Waveform(int sound) const = 0;
    virtual double Seconds(int sound) const = 0;
    // Decoded PCM for playback; nullptr = nothing playable in that slot.
    virtual const kgt::PcmAudio* Pcm(int /*sound*/) const { return nullptr; }

    // Sprite thumbnail source for the usage panel. Decoded lazily and
    // cached by the implementation; nullptr = no such sprite, or a payload
    // that would not decode. Returns RGBA8888 with alpha already keyed.
    virtual const kgt::DecodedSprite* Sprite(int /*sprite*/) const { return nullptr; }
    // Bumped on every successful Load. The panel's GPU texture cache keys
    // on this: sprite INDICES are reused across files, so a cache keyed on
    // index alone would show the previous file's artwork after an open.
    virtual uint64_t Generation() const { return 0; }
};

// Cross-panel state, owned by main().
struct AppState {
    StudioModel* model = nullptr;
    RealModel* real = nullptr;      // same object as model; write actions
    AudioPlayer* player = nullptr;  // owned by main(); null = audio unavailable
    SDL_Window* window = nullptr;   // dialog parent
    SDL_Renderer* renderer = nullptr;  // usage-panel thumbnail textures
    int selected = 0;
    bool playing = false;
    std::string load_error;         // nonempty -> "Open failed" modal
    bool quit = false;

    // ── write path (Phase 4) ──
    // Replace flow: the picker targets the slot selected when it was
    // opened (selection may change while the async dialog is up).
    int replace_slot = -1;
    ReplacePlan replace_plan;       // valid while replace_modal_open
    bool replace_modal_open = false;
    std::string replace_error;      // nonempty -> "Replace failed" modal

    bool confirm_overwrite = false; // "Save (overwrite)" confirmation modal
    std::string save_error;         // nonempty -> "Save failed" modal

    // Unsaved-changes confirm (Open/Quit with a dirty file): what to do
    // once the user resolves it, and whether a Save-As-Copy is the
    // in-flight resolution (its completion continues the action).
    enum class AfterConfirm : uint8_t { None, OpenDialog, OpenPath, Quit };
    AfterConfirm after_confirm = AfterConfirm::None;
    std::string after_confirm_path; // for AfterConfirm::OpenPath (drag-drop)
    bool confirm_unsaved = false;   // modal visible
    bool save_as_continues = false; // Save Copy picked in the confirm

    // SDL file-dialog results land here; the callbacks may run off-thread
    // (see SDL_dialog.h docs), main loop consumes under the lock.
    std::mutex dlg_mutex;
    std::string pending_open, pending_replace, pending_save_as;
    bool pending_save_cancel = false;  // save dialog dismissed
};

std::string TypeLabel(uint8_t sound_type);  // "WAV", "MIDI loop", ...
std::string SizeLabel(size_t bytes);        // "73,772 B"

void DrawSoundTable(AppState& st);
void DrawSoundDetail(AppState& st);
void DrawUsagePanel(AppState& st);

// sound_detail.cpp: build a ReplacePlan for `slot` from raw file bytes and
// raise either the confirm modal or the error modal. Shared by the picker
// path, drag-drop, and the WARN row's "Convert to 16-bit PCM" action.
void StartReplace(AppState& st, int slot, const uint8_t* data, size_t len);
// sound_detail.cpp: the confirm + error modals for the replace flow.
// Called at frame scope (main.cpp) so a collapsed panel can't eat them.
void DrawReplaceModals(AppState& st);

}  // namespace studio
