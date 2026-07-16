// real_model.h -- StudioModel over an EditSession's parsed kgt::KgtFile.
//
// DATA LAYER ONLY: no SDL, no ImGui. This header + real_model.cpp compile
// natively for the headless gates (studio/tests/model/, tests/write/);
// playback and file dialogs live in the app layer (audio_player.*,
// main.cpp).
//
// Everything the panels read -- rows, per-sound use lists, decoded PCM,
// waveform previews, RMS, validity tiers -- is computed in Load() and
// cached, index-aligned with the session's sounds. Mutations (replace/
// undo/redo/save) flow through the owned EditSession and rebuild only the
// touched slot's caches.
#pragma once

#include <string>
#include <vector>

#include "../core/audio/audio_convert.h"
#include "../core/kgt_file.h"
#include "app_state.h"
#include "edit_session.h"

namespace studio {

class RealModel : public StudioModel {
public:
    // Read + parse `path` (EditSession::Load: pre-edit round-trip gate
    // included), then rebuild every cached row/use/waveform. On failure
    // returns false with a user-facing *err and the previous contents
    // intact. Refuses FM95 files and unknown extensions.
    bool Load(const std::string& path, std::string* err);

    bool Loaded() const { return session_.Loaded(); }
    // False = pre-edit gate failed: browse-only mode (banner, editing off).
    bool Editable() const { return session_.Editable(); }
    const std::string& Path() const { return session_.Path(); }
    // "299 scripts, 566 sprites, 37 sounds" -- for the title bar.
    std::string CountsLabel() const;

    const std::vector<SoundRow>& Sounds() const override { return sounds_; }
    const std::vector<UseRow>& Uses(int sound) const override;
    const std::vector<float>& Waveform(int sound) const override;
    double Seconds(int sound) const override;
    const kgt::PcmAudio* Pcm(int sound) const override;

    // ── write path (wraps EditSession, keeps caches in sync) ──
    bool Dirty() const { return session_.Dirty(); }
    bool CanUndo() const { return session_.CanUndo(); }
    bool CanRedo() const { return session_.CanRedo(); }
    bool PlanReplace(int slot, const uint8_t* data, size_t len,
                     ReplacePlan* out, std::string* err) const {
        return session_.PlanReplace(slot, data, len, out, err);
    }
    bool ApplyReplace(const ReplacePlan& plan, std::string* err);
    int Undo();  // returns the slot restored (-1 = nothing to undo)
    int Redo();
    bool SaveCopy(const std::string& dest, std::string* err);
    bool SaveOverwrite(std::string* err);

    EditSession& Session() { return session_; }  // tests / advanced callers

private:
    // Recompute one row's derived caches (name/type/fmt/validity/PCM/
    // waveform/RMS/modified) from the session's current payload. Leaves
    // use_count/uses_ alone -- payload edits never change the xref.
    void RebuildSlot(int i);
    void RefreshModifiedFlags();

    EditSession session_;
    // Per-sound caches, index-aligned with session_.File().sounds.
    std::vector<SoundRow> sounds_;
    std::vector<std::vector<UseRow>> uses_;
    std::vector<kgt::PcmAudio> pcm_;  // decoded preview PCM (empty if none)
    std::vector<std::vector<float>> wave_;
};

}  // namespace studio
