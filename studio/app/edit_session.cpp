// edit_session.cpp -- see edit_session.h. Ports the proven gates of
// tools/kgt/soundtool.py cmd_replace: pre-edit round-trip gate at Load,
// FATAL-tier probe on incoming WAVs, post-serialize self-verify (only
// touched sounds differ, output stable), .bak = oldest original, and the
// tmp + re-read + atomic-rename overwrite sequence.
#include "edit_session.h"

#include <cstdio>
#include <cstring>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#endif

namespace studio {
namespace {

#ifdef _WIN32
// SDL dialogs and our own strings are UTF-8; the C runtime's narrow fopen
// is ANSI-codepage. Convert so non-ASCII paths (JP mod folders, non-ASCII
// usernames) work.
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()),
                                nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}
#endif

FILE* OpenPath(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring wpath = Utf8ToWide(path);
    wchar_t wmode[8] = {};
    for (size_t i = 0; mode[i] && i < 7; ++i)
        wmode[i] = wchar_t(mode[i]);
    return wpath.empty() ? nullptr : _wfopen(wpath.c_str(), wmode);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool RemovePath(const std::string& path) {
#ifdef _WIN32
    std::wstring w = Utf8ToWide(path);
    return !w.empty() && DeleteFileW(w.c_str()) != 0;
#else
    return std::remove(path.c_str()) == 0;
#endif
}

// Atomic replace of dest by tmp (same directory, so the rename is atomic
// on both platforms). Decodes the game-has-the-file-open case explicitly.
bool AtomicReplace(const std::string& tmp, const std::string& dest,
                   std::string* err) {
#ifdef _WIN32
    std::wstring wtmp = Utf8ToWide(tmp), wdest = Utf8ToWide(dest);
    if (wtmp.empty() || wdest.empty()) {
        *err = "path conversion failed: " + dest;
        return false;
    }
    if (!MoveFileExW(wtmp.c_str(), wdest.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION ||
            e == ERROR_USER_MAPPED_FILE) {
            *err = dest + " is open in another program (the game?) -- "
                   "close the game first, then save again";
        } else {
            *err = "rename over " + dest + " failed (Win32 error " +
                   std::to_string(e) + ")";
        }
        return false;
    }
    return true;
#else
    if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
        *err = "rename over " + dest + " failed: " +
               std::string(std::strerror(errno));
        return false;
    }
    return true;
#endif
}

std::string PcmDesc(const kgt::PcmAudio& a) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "PCM %uHz %u-bit %s", unsigned(a.rate),
                  unsigned(a.bits), a.channels == 2 ? "stereo" : "mono");
    return buf;
}

std::string JoinLines(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& p : v) {
        if (!s.empty()) s += "; ";
        s += p;
    }
    return s;
}

}  // namespace

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>* out,
                   std::string* err) {
    FILE* f = OpenPath(path, "rb");
    if (!f) {
        if (err) *err = "cannot open file: " + path;
        return false;
    }
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    const long n = ok ? std::ftell(f) : -1;
    ok = ok && n >= 0 && std::fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        out->resize(size_t(n));
        ok = (n == 0) ||
             std::fread(out->data(), 1, size_t(n), f) == size_t(n);
    }
    std::fclose(f);
    if (!ok && err) *err = "read failed: " + path;
    return ok;
}

bool WriteFileBytes(const std::string& path,
                    const std::vector<uint8_t>& bytes, std::string* err) {
    FILE* f = OpenPath(path, "wb");
    if (!f) {
        if (err) *err = "cannot create file: " + path;
        return false;
    }
    const bool wrote =
        bytes.empty() ||
        std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (wrote && closed) return true;
    if (err) *err = "write failed: " + path;
    return false;
}

bool FileExists(const std::string& path) {
    FILE* f = OpenPath(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// ── load + pre-edit gate ─────────────────────────────────────────────────

bool EditSession::Load(const std::string& path, std::string* err) {
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
        if (!ReadFileBytes(path, &bytes, &e)) return bail(std::move(e));
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

    // Pre-edit gate (workflow item 2): a file that does not round-trip
    // byte-exact enters browse-only mode -- unknown variants are protected
    // from us, not the other way around.
    const bool editable = (kgt::Serialize(f) == bytes);

    // Commit (a failed load above leaves the previous session intact).
    file_ = std::move(f);
    original_bytes_ = std::move(bytes);
    path_ = path;
    pristine_.clear();
    pristine_.reserve(file_.sounds.size());
    for (const kgt::Sound& sd : file_.sounds) pristine_.push_back(sd.data);
    modified_.assign(file_.sounds.size(), false);
    undo_.clear();
    redo_.clear();
    editable_ = editable;
    loaded_ = true;
    return true;
}

bool EditSession::SlotModified(int slot) const {
    return slot >= 0 && slot < int(modified_.size()) && modified_[slot];
}

bool EditSession::Dirty() const {
    for (bool m : modified_)
        if (m) return true;
    return false;
}

bool EditSession::RefuseIfNotEditable(std::string* err) const {
    if (!loaded_) {
        if (err) *err = "no file loaded";
        return false;
    }
    if (!editable_) {
        if (err)
            *err = "browse-only: this file did not round-trip byte-exact "
                   "through the parser, so editing is disabled to protect "
                   "it -- please report the file so the parser can be fixed";
        return false;
    }
    return true;
}

// ── replace pipeline ─────────────────────────────────────────────────────

bool EditSession::PlanReplace(int slot, const uint8_t* data, size_t len,
                              ReplacePlan* out, std::string* err) const {
    auto bail = [&](std::string msg) {
        if (err) *err = std::move(msg);
        return false;
    };
    if (!RefuseIfNotEditable(err)) return false;
    if (slot < 0 || slot >= int(file_.sounds.size()))
        return bail("sound index out of range");
    if (!data || len == 0) return bail("replacement file is empty");

    const kgt::Sound& sd = file_.sounds[slot];
    const uint8_t nib = sd.sound_type & 0x0F;
    ReplacePlan plan;
    plan.slot = slot;

    if (nib == 2) {  // MIDI slot: MThd check, bytes verbatim (no convert)
        if (len < 4 || std::memcmp(data, "MThd", 4) != 0)
            return bail("slot is MIDI but the file has no MThd header");
        plan.is_midi = true;
        plan.payload.assign(data, data + len);
        plan.src_desc = "MIDI (MThd)";
        plan.dst_desc = "MIDI (MThd), stored verbatim";
        *out = std::move(plan);
        return true;
    }
    if (nib != 1) {
        const char* type = nib == 0   ? "stop-all"
                           : nib == 3 ? "CD audio"
                                      : "unknown";
        return bail("this slot is type " + std::string(type) +
                    "; only WAV and MIDI slots can be replaced");
    }

    // WAV slot. Gate 1 (cmd_replace): a RIFF input that the engine walk
    // marks FATAL is refused outright -- decoding it might work, but the
    // file is telling us its own header lies.
    if (len >= 4 && std::memcmp(data, "RIFF", 4) == 0) {
        const kgt::WavValidity v = kgt::ProbeWav(data, len);
        if (v.Fatal())
            return bail("replacement WAV would break in-engine: " +
                        JoinLines(v.fatal));
        plan.src_desc = kgt::ProbeDesc(v);
    }

    kgt::PcmAudio src;
    {
        std::string e;
        if (!kgt::DecodeAudio(data, len, &src, &e)) return bail(std::move(e));
    }
    if (plan.src_desc.empty()) {  // non-RIFF input: label by magic
        const char* kind = "MP3";
        if (len >= 4 && std::memcmp(data, "OggS", 4) == 0) kind = "OGG";
        else if (len >= 4 && std::memcmp(data, "fLaC", 4) == 0) kind = "FLAC";
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s %uHz %s", kind, unsigned(src.rate),
                      src.channels == 1 ? "mono"
                      : src.channels == 2 ? "stereo" : "multichannel");
        plan.src_desc = buf;
    }

    // Target: 16-bit PCM, source rate kept, channels clamped to the
    // engine envelope (mono/stereo).
    const uint16_t dst_ch = src.channels > 2 ? uint16_t(2) : src.channels;
    kgt::PcmAudio conv = kgt::Normalize(src, 0, dst_ch, 16);
    if (conv.pcm.empty()) return bail("conversion produced no audio");
    plan.payload = kgt::EncodeWav(conv);
    if (plan.payload.empty()) return bail("encoding the converted WAV failed");
    plan.dst_desc = PcmDesc(conv);
    plan.seconds = conv.Seconds();
    plan.rms_dbfs = kgt::RmsDbfs(conv);
    *out = std::move(plan);
    return true;
}

bool EditSession::ApplyReplace(const ReplacePlan& plan, std::string* err) {
    if (plan.payload.empty()) {
        if (err) *err = "empty replace plan";
        return false;
    }
    return ReplaceSlot(plan.slot, plan.payload, err);
}

bool EditSession::ReplaceSlot(int slot, std::vector<uint8_t> payload,
                              std::string* err) {
    if (!RefuseIfNotEditable(err)) return false;
    if (slot < 0 || slot >= int(file_.sounds.size())) {
        if (err) *err = "sound index out of range";
        return false;
    }
    undo_.push_back({slot, std::move(file_.sounds[slot].data)});
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    file_.sounds[slot].data = std::move(payload);
    MarkModified(slot);
    return true;
}

void EditSession::MarkModified(int slot) {
    modified_[slot] =
        (file_.sounds[size_t(slot)].data != pristine_[size_t(slot)]);
}

// ── undo/redo (per-slot payload snapshots) ───────────────────────────────

int EditSession::Undo() {
    if (undo_.empty()) return -1;
    UndoEntry e = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({e.slot, std::move(file_.sounds[size_t(e.slot)].data)});
    file_.sounds[size_t(e.slot)].data = std::move(e.payload);
    MarkModified(e.slot);
    return e.slot;
}

int EditSession::Redo() {
    if (redo_.empty()) return -1;
    UndoEntry e = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({e.slot, std::move(file_.sounds[size_t(e.slot)].data)});
    file_.sounds[size_t(e.slot)].data = std::move(e.payload);
    MarkModified(e.slot);
    return e.slot;
}

// ── self-verify + save ───────────────────────────────────────────────────

bool EditSession::VerifyBytes(const std::vector<uint8_t>& bytes,
                              std::string* err) const {
    auto bail = [&](std::string msg) {
        if (err) *err = std::move(msg);
        return false;
    };
    kgt::KgtFile re;
    {
        std::string e;
        if (!kgt::Parse(bytes.data(), bytes.size(), file_.type, &re, &e))
            return bail("self-verify failed: output does not re-parse: " + e);
    }
    if (re.sounds.size() != file_.sounds.size())
        return bail("self-verify failed: sound count changed; not writing");
    for (size_t j = 0; j < re.sounds.size(); ++j) {
        // Untouched slots must equal the pristine payload; touched slots
        // the in-memory one -- which is exactly what file_ holds.
        if (re.sounds[j].data != file_.sounds[j].data)
            return bail("self-verify FAILED at sound " + std::to_string(j) +
                        "; not writing");
    }
    if (kgt::Serialize(re) != bytes)
        return bail("self-verify FAILED (output not stable); not writing");
    return true;
}

bool EditSession::BuildVerifiedBytes(std::vector<uint8_t>* out,
                                     std::string* err) const {
    std::vector<uint8_t> bytes = kgt::Serialize(file_);

    // Untouched-region proof: only Sound.data is ever mutated in this
    // session, so restoring the pristine payloads must reproduce the
    // original file byte-for-byte. Anything else drifting means a bug in
    // us -- refuse to write.
    kgt::KgtFile check = file_;
    for (size_t j = 0; j < check.sounds.size(); ++j)
        check.sounds[j].data = pristine_[j];
    if (kgt::Serialize(check) != original_bytes_) {
        if (err)
            *err = "self-verify failed: non-sound regions differ from the "
                   "original; not writing (please report this)";
        return false;
    }

    if (!VerifyBytes(bytes, err)) return false;
    *out = std::move(bytes);
    return true;
}

bool EditSession::CommitTo(const std::string& dest,
                           const std::vector<uint8_t>& bytes,
                           std::string* err) {
    const std::string tmp = dest + ".tmp";
    if (!WriteFileBytes(tmp, bytes, err)) {
        RemovePath(tmp);
        return false;
    }
    // Re-read the tmp from DISK and re-verify (workflow item 4c): what got
    // to the platter must parse and match record-by-record, not just what
    // we handed to fwrite.
    std::vector<uint8_t> back;
    {
        std::string e;
        if (!ReadFileBytes(tmp, &back, &e)) {
            RemovePath(tmp);
            if (err) *err = "tmp re-read failed: " + e;
            return false;
        }
    }
    if (back != bytes) {
        RemovePath(tmp);
        if (err) *err = "tmp re-read differs from what was written: " + tmp;
        return false;
    }
    if (!VerifyBytes(back, err)) {
        RemovePath(tmp);
        return false;
    }
    if (!AtomicReplace(tmp, dest, err)) {
        RemovePath(tmp);
        return false;
    }
    return true;
}

void EditSession::Rebaseline(const std::string& path,
                             std::vector<uint8_t> bytes) {
    path_ = path;
    original_bytes_ = std::move(bytes);
    for (size_t j = 0; j < file_.sounds.size(); ++j)
        pristine_[j] = file_.sounds[j].data;
    modified_.assign(file_.sounds.size(), false);
    // Undo history intentionally survives: undoing past a save re-dirties
    // against the new baseline, which is exactly what MarkModified computes.
}

bool EditSession::SaveCopy(const std::string& dest, std::string* err) {
    if (!RefuseIfNotEditable(err)) return false;
    if (dest.empty()) {
        if (err) *err = "no destination path";
        return false;
    }
    if (dest == path_) return SaveOverwrite(err);  // keep the .bak discipline

    std::vector<uint8_t> bytes;
    if (!BuildVerifiedBytes(&bytes, err)) return false;
    if (!CommitTo(dest, bytes, err)) return false;
    Rebaseline(dest, std::move(bytes));
    return true;
}

bool EditSession::SaveOverwrite(std::string* err) {
    if (!RefuseIfNotEditable(err)) return false;

    std::vector<uint8_t> bytes;
    if (!BuildVerifiedBytes(&bytes, err)) return false;

    // (a) .bak = the oldest original: written once from the bytes read at
    // open, never rewritten later (soundtool semantics).
    const std::string bak = path_ + ".bak";
    if (!FileExists(bak)) {
        std::string e;
        if (!WriteFileBytes(bak, original_bytes_, &e)) {
            if (err) *err = "backup failed, not writing: " + e;
            return false;
        }
    }

    // (b)(c)(d) tmp + re-read verify + atomic rename; (e) is inside.
    if (!CommitTo(path_, bytes, err)) return false;
    Rebaseline(path_, std::move(bytes));
    return true;
}

}  // namespace studio
