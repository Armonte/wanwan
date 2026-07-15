// sound_detail.cpp -- right-top panel: waveform strip (ImDrawList over
// the model's PCM preview), play/stop through AudioPlayer (SDL3 audio,
// playhead follows the actual device position), format + RMS text,
// Replace via SDL_ShowOpenFileDialog.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <SDL3/SDL.h>

#include "app_state.h"
#include "audio_player.h"

namespace studio {
namespace {

// May fire off-thread (SDL_dialog.h) -- stash under the lock.
void ReplaceCb(void* ud, const char* const* files, int /*filter*/) {
    auto* st = static_cast<AppState*>(ud);
    if (files && files[0]) {
        std::lock_guard<std::mutex> lk(st->dlg_mutex);
        st->pending_replace = files[0];
    }
}

constexpr SDL_DialogFileFilter kAudioFilters[] = {
    {"audio (wav/mp3/ogg/flac/mid)", "wav;mp3;ogg;flac;mid"},
    {"All files", "*"},
};

void WaveformStrip(AppState& st, const std::vector<float>& wf, double secs) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz(ImGui::GetContentRegionAvail().x, 96.f);
    if (sz.x < 40.f) sz.x = 40.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p0 + sz, IM_COL32(22, 24, 30, 255));
    const float cy = p0.y + sz.y * .5f;
    dl->AddLine(ImVec2(p0.x, cy), ImVec2(p0.x + sz.x, cy), IM_COL32(70, 74, 88, 255));
    if (wf.empty()) {
        dl->AddText(ImVec2(p0.x + 8.f, p0.y + 8.f), IM_COL32(140, 140, 150, 255),
                    "no PCM preview");
    } else {
        // min/max column render, classic waveform strip.
        const float half = sz.y * .48f;
        for (int x = 0; x < int(sz.x); ++x) {
            size_t a = size_t(double(x) / sz.x * double(wf.size()));
            size_t b = size_t(double(x + 1) / sz.x * double(wf.size()));
            if (b <= a) b = a + 1;
            float lo = 1.f, hi = -1.f;
            for (size_t i = a; i < b && i < wf.size(); ++i) {
                lo = wf[i] < lo ? wf[i] : lo;
                hi = wf[i] > hi ? wf[i] : hi;
            }
            dl->AddLine(ImVec2(p0.x + float(x), cy - hi * half),
                        ImVec2(p0.x + float(x), cy - lo * half + 1.f),
                        IM_COL32(120, 190, 240, 255));
        }
        if (st.playing && st.player && secs > 0.0) {
            // Playhead at the device's actual position.
            double t = st.player->PositionSeconds() / secs;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            float px = p0.x + float(t) * sz.x;
            dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p0.y + sz.y),
                        IM_COL32(255, 210, 90, 255), 2.f);
        }
    }
    ImGui::Dummy(sz);
}

}  // namespace

void DrawSoundDetail(AppState& st) {
    if (!ImGui::Begin("Sound")) {
        ImGui::End();
        return;
    }
    const auto& sounds = st.model->Sounds();
    if (st.selected < 0 || st.selected >= int(sounds.size())) {
        ImGui::TextDisabled("no sound selected");
        ImGui::End();
        return;
    }
    const SoundRow& s = sounds[st.selected];
    ImGui::Text("#%d  %s", st.selected, s.name.c_str());

    WaveformStrip(st, st.model->Waveform(st.selected), st.model->Seconds(st.selected));

    const kgt::PcmAudio* pcm = st.model->Pcm(st.selected);
    if (!st.playing) {
        ImGui::BeginDisabled(!pcm || !st.player);
        if (ImGui::Button("> play")) {
            if (pcm && st.player && st.player->Play(*pcm))
                st.playing = true;
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("[] stop")) {
            st.playing = false;
            if (st.player) st.player->Stop();
        } else if (st.player && !st.player->Playing()) {
            st.playing = false;  // clip drained: main loop closes the device
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("replace...")) {
        SDL_ShowOpenFileDialog(ReplaceCb, &st, st.window, kAudioFilters,
                               int(SDL_arraysize(kAudioFilters)), nullptr, false);
    }

    ImGui::Text("%s  %s  %s", TypeLabel(s.sound_type).c_str(), s.fmt.c_str(),
                SizeLabel(s.size).c_str());
    if (pcm) ImGui::Text("RMS %.1f dBFS", s.rms_dbfs);
    if (!s.valid)
        ImGui::TextColored(ImVec4(1.f, .35f, .3f, 1.f), "engine RIFF walk: INVALID");
    if (!st.replace_path.empty())
        ImGui::TextDisabled("pending replace: %s (not applied -- write milestone)",
                            st.replace_path.c_str());
    ImGui::End();
}

}  // namespace studio
