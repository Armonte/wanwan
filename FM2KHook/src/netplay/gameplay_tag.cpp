// Gameplay tag, Win32 file access -- see gameplay_tag.h. Everything that
// decides WHICH bytes count is in gameplay_tag_core.cpp; this file only finds
// and reads files.
#include "gameplay_tag.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <vector>
#include <windows.h>

namespace fm2k::gameplay_tag {

namespace {

// Names handed to the provider are either raw CP932 roster bytes (what the
// engine sprintf's into CreateFileA) or, for the exe / .kgt the caller already
// holds as a wide string, UTF-8 behind this marker byte. A roster name can
// never start with 0x01.
constexpr char kUtf8Marker = '\x01';

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
    return s;
}

std::wstring MbToWide(UINT cp, const std::string& s, DWORD flags) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(cp, flags, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, flags, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring NameToWide(const std::string& name) {
    if (!name.empty() && name[0] == kUtf8Marker)
        return MbToWide(CP_UTF8, name.substr(1), 0);
    // CP932 first (every JP FM2K roster); the ANSI page if the bytes are not
    // valid CP932, which is what CreateFileA itself would have used.
    std::wstring w = MbToWide(932, name, MB_ERR_INVALID_CHARS);
    if (w.empty()) w = MbToWide(CP_ACP, name, 0);
    return w;
}

// HANDLE-backed source with a 64 KiB read window: sprite headers are 20-byte
// reads interleaved with skips, so most land in the window of the last read.
class FileSource final : public ByteSource {
public:
    explicit FileSource(HANDLE h) : h_(h) {
        LARGE_INTEGER sz;
        size_ = GetFileSizeEx(h_, &sz) ? (uint64_t)sz.QuadPart : 0;
    }
    ~FileSource() override { CloseHandle(h_); }
    uint64_t Size() const override { return size_; }
    bool ReadAt(uint64_t off, void* dst, size_t n) override {
        if (off > size_ || n > size_ - off) return false;
        uint8_t* out = static_cast<uint8_t*>(dst);
        while (n) {
            if (off >= win_off_ && off < win_off_ + win_.size()) {
                const size_t at = (size_t)(off - win_off_);
                const size_t take = std::min(n, win_.size() - at);
                std::memcpy(out, win_.data() + at, take);
                out += take; off += take; n -= take;
                continue;
            }
            if (n >= kWindow) {   // big region: read straight through
                return Raw(off, out, n);
            }
            win_.resize((size_t)std::min<uint64_t>(kWindow, size_ - off));
            if (!Raw(off, win_.data(), win_.size())) { win_.clear(); return false; }
            win_off_ = off;
        }
        return true;
    }
private:
    static constexpr size_t kWindow = 64 * 1024;
    bool Raw(uint64_t off, uint8_t* dst, size_t n) {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return false;
        while (n) {
            DWORD got = 0;
            const DWORD want = (DWORD)std::min<size_t>(n, 1u << 30);
            if (!ReadFile(h_, dst, want, &got, nullptr) || got == 0) return false;
            dst += got; n -= got;
        }
        return true;
    }
    HANDLE h_;
    uint64_t size_ = 0;
    std::vector<uint8_t> win_;
    uint64_t win_off_ = 0;
};

class DirProvider final : public FileProvider {
public:
    explicit DirProvider(std::filesystem::path dir) : dir_(std::move(dir)) {}
    bool Stat(const std::string& name, std::string* key, uint64_t* size,
              uint64_t* mtime) override {
        const std::wstring p = Path(name);
        if (p.empty()) return false;
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return false;
        if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
        std::wstring lower = p;
        for (auto& c : lower) c = (wchar_t)towlower(c);
        *key = WideToUtf8(lower);
        *size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        *mtime = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
                 fad.ftLastWriteTime.dwLowDateTime;
        return true;
    }
    ByteSource* Open(const std::string& name) override {
        const std::wstring p = Path(name);
        if (p.empty()) return nullptr;
        // FILE_SHARE_READ|WRITE: the game holds nothing open, but an editor
        // might; reading must never fail or block because of it.
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        return new FileSource(h);
    }
    void Close(ByteSource* s) override { delete s; }
private:
    std::wstring Path(const std::string& name) {
        const std::wstring w = NameToWide(name);
        if (w.empty()) return {};
        return (dir_ / w).wstring();
    }
    std::filesystem::path dir_;
};

std::wstring LowerExt(const std::filesystem::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

}  // namespace

std::wstring ResolveKgtName(const wchar_t* dir_w, const wchar_t* game_exe_name_w) {
    if (!dir_w || !dir_w[0] || !game_exe_name_w || !game_exe_name_w[0]) return {};
    const std::filesystem::path dir(dir_w);
    std::error_code ec;
    std::filesystem::path stem_kgt = std::filesystem::path(game_exe_name_w).stem();
    stem_kgt += L".kgt";
    if (std::filesystem::is_regular_file(dir / stem_kgt, ec)) return stem_kgt.wstring();
    std::wstring only;
    int count = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code ec2;
        if (!e.is_regular_file(ec2) || LowerExt(e.path()) != L".kgt") continue;
        if (++count > 1) return {};
        only = e.path().filename().wstring();
    }
    return only;
}

uint32_t ComputeForDir(const wchar_t* dir_w, const wchar_t* game_exe_name_w,
                       const Participants& parts) {
    const std::wstring kgt = ResolveKgtName(dir_w, game_exe_name_w);
    if (kgt.empty()) return 0;
    DirProvider fp{std::filesystem::path(dir_w)};
    return ComputeTag(fp, std::string(1, kUtf8Marker) + WideToUtf8(kgt),
                      std::string(1, kUtf8Marker) + WideToUtf8(game_exe_name_w),
                      parts);
}

uint32_t ComputeLocal(const Participants& parts) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    const std::filesystem::path exe(buf);
    return ComputeForDir(exe.parent_path().wstring().c_str(),
                         exe.filename().wstring().c_str(), parts);
}

}  // namespace fm2k::gameplay_tag
