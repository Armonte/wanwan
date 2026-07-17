// sound_detail.cpp -- right-top panel: waveform strip (ImDrawList over
// the model's PCM preview), play/stop through AudioPlayer (SDL3 audio,
// playhead follows the actual device position), format + RMS + validity
// tier text, and the replace pipeline UI: picker via
// SDL_ShowOpenFileDialog -> StartReplace (PlanReplace) -> old-vs-new
// confirm modal -> ApplyReplace. WARN slots get a one-click "Convert to
// 16-bit PCM" that runs the same pipeline over the slot's own bytes.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <SDL3/SDL.h>

#include "app_state.h"
#include "audio_player.h"
#include "real_model.h"

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

void StartReplace(AppState& st, int slot, const uint8_t* data, size_t len) {
    st.replace_error.clear();
    st.replace_modal_open = false;
    if (!st.real) return;
    ReplacePlan plan;
    std::string err;
    if (!st.real->PlanReplace(slot, data, len, &plan, &err)) {
        st.replace_error = err.empty() ? std::string("replace failed") : err;
        return;
    }
    // Apply happens from the modal, never on file-pick (workflow item 8).
    st.replace_plan = std::move(plan);
    st.replace_modal_open = true;
}

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
    const bool editable = st.real && st.real->Editable();
    const uint8_t nib = s.sound_type & 0x0F;

    ImGui::Text("#%d  %s", st.selected, s.name.c_str());
    if (s.modified) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(.55f, .85f, 1.f, 1.f), "* modified");
    }

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
    const bool replaceable = editable && (nib == 1 || nib == 2);
    ImGui::BeginDisabled(!replaceable);
    if (ImGui::Button("replace...")) {
        // Target the slot selected NOW: the dialog is async and the
        // selection may move before the pick lands.
        st.replace_slot = st.selected;
        SDL_ShowOpenFileDialog(ReplaceCb, &st, st.window, kAudioFilters,
                               int(SDL_arraysize(kAudioFilters)), nullptr, false);
    }
    ImGui::EndDisabled();
    if (!replaceable &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                             ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(editable
                              ? "only WAV and MIDI slots can be replaced"
                              : "browse-only: this file failed the round-trip "
                                "gate, editing is disabled");
    }

    ImGui::Text("%s  %s  %s", TypeLabel(s.sound_type).c_str(), s.fmt.c_str(),
                SizeLabel(s.size).c_str());
    if (pcm) ImGui::Text("RMS %.1f dBFS", s.rms_dbfs);

    if (s.validity == Validity::Fatal) {
        ImGui::TextColored(ImVec4(1.f, .35f, .3f, 1.f),
                           "engine RIFF walk: FATAL -- breaks in-game");
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImVec4(1.f, .55f, .5f, 1.f), "%s",
                           s.validity_msg.c_str());
        ImGui::PopTextWrapPos();
    } else if (s.validity == Validity::Warn) {
        ImGui::TextColored(ImVec4(1.f, .85f, .35f, 1.f),
                           "format note -- plays fine on modern Windows");
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImVec4(1.f, .9f, .6f, 1.f), "%s",
                           s.validity_msg.c_str());
        ImGui::PopTextWrapPos();
        if (editable && nib == 1) {
            if (ImGui::Button("Convert to 16-bit PCM (optional)")) {
                // Same pipeline, source = the slot's own bytes; the
                // confirm modal shows the conversion before it applies.
                const kgt::Sound& sd =
                    st.real->Session().File().sounds[size_t(st.selected)];
                st.replace_slot = st.selected;
                StartReplace(st, st.selected, sd.data.data(), sd.data.size());
            }
        }
    }
    ImGui::End();
}

// Replace modals live at frame scope (main.cpp calls this after the
// panels) so a collapsed detail panel cannot swallow an open modal.
void DrawReplaceModals(AppState& st) {
    // Error first: PlanReplace refusals (FATAL WAVs, wrong slot type, ...).
    if (!st.replace_error.empty()) {
        if (!ImGui::IsPopupOpen("Replace failed"))
            ImGui::OpenPopup("Replace failed");
        if (ImGui::BeginPopupModal("Replace failed", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(420.f);
            ImGui::TextWrapped("%s", st.replace_error.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120.f, 0.f))) {
                st.replace_error.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        return;
    }

    if (!st.replace_modal_open) return;
    if (!ImGui::IsPopupOpen("Replace sound"))
        ImGui::OpenPopup("Replace sound");
    if (!ImGui::BeginPopupModal("Replace sound", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const ReplacePlan& plan = st.replace_plan;
    const auto& sounds = st.model->Sounds();
    const bool slot_ok = plan.slot >= 0 && plan.slot < int(sounds.size());
    if (!slot_ok) {  // model reloaded under the modal: bail out safely
        st.replace_modal_open = false;
        st.replace_plan = ReplacePlan{};
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const SoundRow& s = sounds[plan.slot];
    ImGui::Text("Replace #%d  %s", plan.slot, s.name.c_str());
    ImGui::Separator();
    if (plan.is_midi) {
        ImGui::Text("old: %s  %s", s.fmt.c_str(), SizeLabel(s.size).c_str());
        ImGui::Text("new: %s  %s", plan.dst_desc.c_str(),
                    SizeLabel(plan.payload.size()).c_str());
    } else {
        ImGui::Text("old: %s  %s  %.2f s  RMS %.1f dBFS", s.fmt.c_str(),
                    SizeLabel(s.size).c_str(), st.model->Seconds(plan.slot),
                    s.rms_dbfs);
        ImGui::Text("new: %s", plan.src_desc.c_str());
        ImGui::Text("  -> stored as %s  %s  %.2f s  RMS %.1f dBFS",
                    plan.dst_desc.c_str(), SizeLabel(plan.payload.size()).c_str(),
                    plan.seconds, plan.rms_dbfs);
    }
    ImGui::Spacing();
    if (ImGui::Button("Apply", ImVec2(120.f, 0.f))) {
        std::string err;
        if (st.real && st.real->ApplyReplace(plan, &err)) {
            st.playing = false;  // slot audio changed: stop stale playback
            st.selected = plan.slot;
        } else {
            st.replace_error = err.empty() ? std::string("apply failed") : err;
        }
        st.replace_modal_open = false;
        st.replace_plan = ReplacePlan{};
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.f, 0.f))) {
        st.replace_modal_open = false;
        st.replace_plan = ReplacePlan{};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace studio
