// edit_session.h -- the write path of 2dfm Studio, UI-free.
//
// Owns the parsed kgt::KgtFile being edited plus everything the
// file-safety workflow demands (docs/dev/2dfm_studio_design.md,
// "File-safety workflow (Phase 4 contract)"): the pristine original
// bytes, per-slot pristine payload snapshots, the undo/redo stacks, and
// the save pipeline (Save-As-Copy primary; overwrite = ensure .bak +
// serialize to .tmp + re-read/verify + atomic rename). No SDL, no ImGui:
// this TU compiles natively for studio/tests/write/.
//
// Invariant the verifier leans on: ONLY sound payloads are ever mutated
// (ReplaceSlot); every other byte of the parsed file stays untouched in
// memory, and every save re-proves it stays untouched on disk before the
// original is replaced (soundtool.py cmd_replace gate semantics).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../core/audio/audio_convert.h"
#include "../core/kgt_file.h"

namespace studio {

// Whole-file read/write helpers (UTF-8 paths, _wfopen on Windows so
// non-ASCII usernames/dirs work). Shared with main.cpp and the tests.
bool ReadFileBytes(const std::string& path, std::vector<uint8_t>* out,
                   std::string* err);
bool WriteFileBytes(const std::string& path,
                    const std::vector<uint8_t>& bytes, std::string* err);
bool FileExists(const std::string& path);

// One prepared replacement: built by PlanReplace (decode + convert, no
// mutation), shown in the confirm modal (old vs new format/duration/RMS),
// committed by ApplyReplace -- "apply from the dialog, not on file-pick".
struct ReplacePlan {
    int slot = -1;
    bool is_midi = false;            // MIDI slots: raw MThd bytes, no convert
    std::vector<uint8_t> payload;    // bytes to store: EncodeWav(...) / .mid
    std::string src_desc;            // incoming audio before conversion
    std::string dst_desc;            // payload as stored ("PCM ...16-bit...")
    double seconds = 0.0;            // duration of the stored payload
    double rms_dbfs = 0.0;           // loudness of the stored payload
};

class EditSession {
public:
    // Read + parse `path` (read, close -- no handle kept), then run the
    // pre-edit gate: Serialize(Parse(bytes)) must equal the original
    // bytes or the session enters browse-only mode (Editable() == false,
    // every mutating call refuses). Failure keeps the previous contents.
    bool Load(const std::string& path, std::string* err);

    bool Loaded() const { return loaded_; }
    bool Editable() const { return loaded_ && editable_; }
    const std::string& Path() const { return path_; }
    const kgt::KgtFile& File() const { return file_; }

    bool SlotModified(int slot) const;
    bool Dirty() const;

    // Replace pipeline, gate 1 of soundtool cmd_replace: validate `data`
    // against the slot's type. WAV slots: RIFF inputs are ProbeWav-gated
    // (FATAL refuses -- the engine would OOB), then DecodeAudio
    // (wav/mp3/ogg/flac) -> Normalize to 16-bit PCM at the source rate
    // with channels clamped to 2 -> EncodeWav. MIDI slots: MThd check,
    // bytes verbatim. Other slot types refuse.
    bool PlanReplace(int slot, const uint8_t* data, size_t len,
                     ReplacePlan* out, std::string* err) const;
    bool ApplyReplace(const ReplacePlan& plan, std::string* err);

    // Payload-swap primitive under ApplyReplace (public so tests can
    // fabricate WARN/FATAL fixtures; the GUI always goes through
    // PlanReplace so stored payloads are engine-safe by construction).
    bool ReplaceSlot(int slot, std::vector<uint8_t> payload, std::string* err);

    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    int Undo();  // returns the slot restored, -1 if nothing to undo
    int Redo();

    // Save As Copy (the primary save). dest == Path() falls through to
    // SaveOverwrite (so the .bak discipline still holds if the user picks
    // the original in the dialog). On success the session re-baselines
    // onto dest: Path() moves, dirty markers clear; the undo history
    // survives (walking it re-dirties against the new baseline).
    bool SaveCopy(const std::string& dest, std::string* err);

    // Overwrite, exactly in the contract's order: (a) if `<path>.bak`
    // does not exist, write the ORIGINAL bytes to it (the .bak is always
    // the oldest original, never rewritten); (b) serialize + self-verify
    // to `<path>.tmp`; (c) re-read the tmp, Parse, verify record-by-
    // record; (d) atomic rename over the target; (e) any failure deletes
    // the tmp and leaves the original untouched.
    bool SaveOverwrite(std::string* err);

    // Test hook: force the pre-edit gate verdict. A real file that parses
    // but does not round-trip cannot be fabricated (every field is either
    // preserved verbatim or derived, by design), so the gate WIRING is
    // tested through this instead -- see studio/tests/write/.
    void ForceEditableForTest(bool editable) { editable_ = editable; }

private:
    struct UndoEntry {
        int slot = -1;
        std::vector<uint8_t> payload;  // data to restore on pop
    };
    static constexpr size_t kMaxUndo = 64;

    bool RefuseIfNotEditable(std::string* err) const;
    void MarkModified(int slot);
    // Serialize the edited model and prove it safe: restoring the
    // pristine payloads must reproduce the original bytes (untouched-
    // region proof), and re-parsing the output must yield every sound
    // record-identical to the in-memory model (cmd_replace gate 2).
    bool BuildVerifiedBytes(std::vector<uint8_t>* out, std::string* err) const;
    bool VerifyBytes(const std::vector<uint8_t>& bytes, std::string* err) const;
    // Write bytes to `dest + ".tmp"`, re-read + verify, atomically rename
    // over dest. On failure removes the tmp and reports.
    bool CommitTo(const std::string& dest, const std::vector<uint8_t>& bytes,
                  std::string* err);
    void Rebaseline(const std::string& path, std::vector<uint8_t> bytes);

    bool loaded_ = false;
    bool editable_ = false;
    std::string path_;
    kgt::KgtFile file_;
    std::vector<uint8_t> original_bytes_;            // as read at open/save
    std::vector<std::vector<uint8_t>> pristine_;     // per-slot baselines
    std::vector<bool> modified_;                     // data != pristine
    std::vector<UndoEntry> undo_, redo_;
};

}  // namespace studio
