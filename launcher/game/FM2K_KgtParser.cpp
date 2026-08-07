#include "FM2K_KgtParser.h"
#include "vendored/2dfm_format/2dfm_format.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <windows.h>

#include <cctype>
#include <cstring>
#include <vector>

namespace fm2k {
namespace {

// Fixed-window read with bounds check. Returns false if any byte requested
// would be past EOF (a malformed/truncated .kgt or our offset arithmetic
// drifted). All reads in this parser go through this so a bad file
// short-circuits cleanly instead of crashing.
bool kgt_read(SDL_IOStream* io, void* dst, size_t bytes) {
    return SDL_ReadIO(io, dst, bytes) == bytes;
}

bool kgt_seek_rel(SDL_IOStream* io, Sint64 delta) {
    return SDL_SeekIO(io, delta, SDL_IO_SEEK_CUR) >= 0;
}

// CP932 (Shift-JIS) → UTF-8 conversion via Win32. Trims any trailing NULs
// inside the 256-byte slot. Empty slot (first byte 0) returns "".
std::string cp932_to_utf8(const char* sjis, size_t cap) {
    if (!sjis || cap == 0 || sjis[0] == '\0') return {};
    int byte_len = (int)strnlen(sjis, cap);
    if (byte_len <= 0) return {};
    int wlen = MultiByteToWideChar(932, 0, sjis, byte_len, nullptr, 0);
    if (wlen <= 0) return {};
    std::vector<wchar_t> wide((size_t)wlen);
    if (MultiByteToWideChar(932, 0, sjis, byte_len,
                            wide.data(), wlen) <= 0) {
        return {};
    }
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen,
                                    nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return {};
    std::string out((size_t)u8len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen,
                        out.data(), u8len, nullptr, nullptr);
    return out;
}

// Skip the variable-size common-resource block. Mirrors the structure
// readCommonResourcePart() in 2dfmFileReader.cpp:295 walks, but doesn't
// allocate or decode any of it -- just seeks past.
//
// Layout (each preceded by a uint32 count):
//   scripts          (39 bytes each)
//   script items     (16 bytes each)
//   pictures         (20-byte header + variable payload sized by header)
//   8 shared palettes (each 1024 bytes + 32-byte trailer)
//   sounds           (42-byte header + variable payload sized by header)
bool skip_common_resource_part(SDL_IOStream* io) {
    int32_t count = 0;

    // Scripts
    if (!kgt_read(io, &count, 4)) return false;
    if (!kgt_seek_rel(io, (Sint64)count * _2dfm::SCRIPT_SIZE)) return false;

    // Script items
    if (!kgt_read(io, &count, 4)) return false;
    if (!kgt_seek_rel(io, (Sint64)count * _2dfm::SCRIPT_ITEM_SIZE)) return false;

    // Pictures: per-entry header tells us payload size.
    if (!kgt_read(io, &count, 4)) return false;
    for (int32_t i = 0; i < count; ++i) {
        _2dfm::PictureHeader hdr{};
        if (!kgt_read(io, &hdr, _2dfm::PICTURE_HEADER_SIZE)) return false;
        // Payload size: the explicit `size` field if non-zero, else the
        // computed pixel-area-plus-private-palette size. Mirrors
        // get2dfmPictureSize() in 2dfmFileReader.cpp.
        int payload = hdr.size;
        if (payload == 0) {
            payload = hdr.width * hdr.height
                    + (hdr.hasPrivatePalette ? 1024 : 0);
        }
        if (payload < 0) return false;  // negative = corrupt header
        if (!kgt_seek_rel(io, payload)) return false;
    }

    // 8 shared palettes -- each PALETTE_SIZE bytes + 32-byte trailer
    // (8 × sizeof(int) per the original reader).
    for (int p = 0; p < 8; ++p) {
        if (!kgt_seek_rel(io, _2dfm::PALETTE_SIZE + 8 * (Sint64)sizeof(int32_t))) {
            return false;
        }
    }

    // Sounds
    if (!kgt_read(io, &count, 4)) return false;
    for (int32_t i = 0; i < count; ++i) {
        _2dfm::SoundItemHeader hdr{};
        if (!kgt_read(io, &hdr, _2dfm::SOUND_ITEM_HEADER_SIZE)) return false;
        if (hdr.size > 0) {
            if (!kgt_seek_rel(io, hdr.size)) return false;
        }
    }
    return true;
}

} // namespace

bool ParseKgtSummary(const std::filesystem::path& kgt_path, KgtSummary& out) {
    out = {};

    std::string path_utf8;
    {
        // SDL_IOFromFile on Windows accepts UTF-8 -- convert from the
        // wide-char filesystem path so JP-named .kgt files work.
        const std::wstring& wide = kgt_path.wstring();
        int u8len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                                        nullptr, 0, nullptr, nullptr);
        if (u8len <= 0) return false;
        path_utf8.assign((size_t)u8len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                            path_utf8.data(), u8len, nullptr, nullptr);
    }

    SDL_IOStream* io = SDL_IOFromFile(path_utf8.c_str(), "rb");
    if (!io) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "ParseKgtSummary: open failed for '%s'", path_utf8.c_str());
        return false;
    }

    auto cleanup = [&](bool ok) -> bool {
        SDL_CloseIO(io);
        return ok;
    };

    // 1. KGT header (272 bytes: 16 sig + 256 NameInfo)
    _2dfm::KgtFileHeader hdr{};
    if (!kgt_read(io, &hdr, _2dfm::KGT_FILE_HEADER_SIZE)) {
        return cleanup(false);
    }
    out.project_name = cp932_to_utf8(hdr.name.name, sizeof(hdr.name.name));

    // Signature gate -- four known formats, two parser dispatches:
    //   "2DKGT2K\0"  FM2K (KGT 2nd)         -- WW, ReSHUFFLE, most modern
    //   "2DKGT2G\0"  FM2K (KGT 2 G-variant) -- AOB
    //   "KGTGAME\0"  FM95                    -- CPW (cmdline-loader path)
    //   "2DKGT95\0"  FM95                    -- verify_kgt_magic-checked variant
    //
    // FM95's .kgt layout is fundamentally different from FM2K's: the file
    // is a 0x78D48-byte fixed-size block, not a stream of variable-size
    // sections. Player/stage/demo arrays sit at known fixed offsets
    // (derived from FM95 IDA -- see FM95_Integration.h address mapping).
    const char* sig = (const char*)hdr.fileSignature;
    const bool is_fm2k = (std::memcmp(sig, "2DKGT2K", 7) == 0) ||
                         (std::memcmp(sig, "2DKGT2G", 7) == 0);
    const bool is_fm95 = (std::memcmp(sig, "KGTGAME", 7) == 0) ||
                         (std::memcmp(sig, "2DKGT95", 7) == 0);
    if (!is_fm2k && !is_fm95) {
        char sig_print[17] = {};
        std::memcpy(sig_print, sig, 16);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "ParseKgtSummary: unsupported .kgt signature '%s' (%s)",
            sig_print, path_utf8.c_str());
        return cleanup(false);
    }

    std::vector<_2dfm::NameInfo> player_slots(_2dfm::maxPlayerNum);
    std::vector<_2dfm::NameInfo> stage_slots(_2dfm::maxStageNum);
    std::vector<_2dfm::NameInfo> demo_slots(_2dfm::maxDemoNum);

    if (is_fm2k) {
        // FM2K layout -- variable-size common-resource block, then fixed.
        if (!skip_common_resource_part(io)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "ParseKgtSummary: common-resource skip failed for '%s'",
                path_utf8.c_str());
            return cleanup(false);
        }
        // +4 skip after common-resource (matches original parser behaviour).
        if (!kgt_seek_rel(io, 4)) return cleanup(false);
        if (!kgt_read(io, player_slots.data(),
                      sizeof(_2dfm::NameInfo) * _2dfm::maxPlayerNum)) {
            return cleanup(false);
        }
        // Reactions: 200 × 36 bytes.
        if (!kgt_seek_rel(io, sizeof(_2dfm::ReactionItem) * _2dfm::maxReactionNum)) {
            return cleanup(false);
        }
        // +4 skip + RecoverTimeConfig (4 bytes).
        if (!kgt_seek_rel(io, 4 + (Sint64)sizeof(_2dfm::RecoverTimeConfig))) {
            return cleanup(false);
        }
        if (!kgt_read(io, stage_slots.data(),
                      sizeof(_2dfm::NameInfo) * _2dfm::maxStageNum)) {
            return cleanup(false);
        }
        if (!kgt_read(io, demo_slots.data(),
                      sizeof(_2dfm::NameInfo) * _2dfm::maxDemoNum)) {
            return cleanup(false);
        }
    } else {
        // FM95 layout -- fixed offsets inside the 0x78D48-byte g_kgt_data
        // block. Derived from CPW IDA (LoadKgtFile @ 0x4072D0,
        // load_kgt_from_cmdline @ 0x406750) cross-referenced with
        // FM95_Integration.h address map:
        //   g_kgt_data              @ 0x463BE0   → file offset 0
        //   g_player_file_name_array@ 0x463CF0   → offset 0x110
        //   g_stage_file_name_array @ 0x467B88   → offset 0x3FA8
        //   g_demo_file_name_array  @ 0x46AD88   → offset 0x71A8
        //
        // Header (272 B) was already consumed above; we're at file offset
        // 0x110 -- exactly where the player array starts.
        constexpr Sint64 kHeaderEnd     = 0x110;   // consumed
        constexpr Sint64 kStageOff      = 0x3FA8;
        constexpr Sint64 kPlayerSize    = 256 * _2dfm::maxPlayerNum;  // 0x3200
        constexpr Sint64 kPostPlayerEnd = kHeaderEnd + kPlayerSize;   // 0x3310

        if (!kgt_read(io, player_slots.data(), kPlayerSize)) return cleanup(false);
        // Skip the misc-config gap between player and stage arrays.
        if (!kgt_seek_rel(io, kStageOff - kPostPlayerEnd)) return cleanup(false);
        if (!kgt_read(io, stage_slots.data(),
                      sizeof(_2dfm::NameInfo) * _2dfm::maxStageNum)) {
            return cleanup(false);
        }
        // Demos immediately follow stages with no gap on FM95.
        if (!kgt_read(io, demo_slots.data(),
                      sizeof(_2dfm::NameInfo) * _2dfm::maxDemoNum)) {
            return cleanup(false);
        }
    }

    // We don't need anything past this point for the dropdowns. Convert
    // and pack into the output struct.
    auto pack = [](const std::vector<_2dfm::NameInfo>& src,
                   std::vector<std::string>& dst) {
        dst.reserve(src.size());
        for (const auto& slot : src) {
            dst.push_back(cp932_to_utf8(slot.name, sizeof(slot.name)));
        }
    };
    pack(player_slots, out.player_names);
    pack(stage_slots,  out.stage_names);
    pack(demo_slots,   out.demo_names);

    // The KGT's player_names slots are FILENAMES (e.g. "c1", "瑞希君"),
    // not always display names. Some games (vanpri) use placeholder
    // filenames like c1/c2 in the KGT and put the actual character
    // display name inside each .player file's header. Enrich here by
    // peeking at each referenced .player file's header and using its
    // NameInfo when non-empty.
    //
    // Layout of a .player file (mirrors KGT header format):
    //   bytes  0..15  : 16-byte signature (we don't validate)
    //   bytes 16..271 : NameInfo (256-byte CP932 string -- display name)
    //
    // Failure modes (file missing, header read short, name empty) all
    // fall through to the existing filename, so this enrichment never
    // makes a row worse than it was. FM95 .player files share the same
    // layout per CPW IDA, so the same path applies for both engines.
    {
        const std::filesystem::path game_dir = kgt_path.parent_path();
        for (size_t i = 0; i < out.player_names.size(); ++i) {
            const std::string& fname = out.player_names[i];
            if (fname.empty()) continue;
            // Build "<game_dir>/<filename>.player". UTF-8 → wide for
            // path construction so JP-named .player files resolve.
            std::filesystem::path pf = game_dir /
                std::filesystem::u8path(fname + ".player");
            std::wstring pw = pf.wstring();
            int u8len = WideCharToMultiByte(CP_UTF8, 0, pw.c_str(), -1,
                                            nullptr, 0, nullptr, nullptr);
            if (u8len <= 0) continue;
            std::string pf_utf8((size_t)u8len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, pw.c_str(), -1,
                                pf_utf8.data(), u8len, nullptr, nullptr);
            SDL_IOStream* pio = SDL_IOFromFile(pf_utf8.c_str(), "rb");
            if (!pio) continue;
            _2dfm::KgtFileHeader phdr{};
            const bool ok = (SDL_ReadIO(pio, &phdr,
                                _2dfm::KGT_FILE_HEADER_SIZE)
                             == _2dfm::KGT_FILE_HEADER_SIZE);
            SDL_CloseIO(pio);
            if (!ok) continue;
            std::string display = cp932_to_utf8(phdr.name.name,
                                                sizeof(phdr.name.name));
            if (!display.empty() && display != fname) {
                // Skip the override when the embedded name STARTS WITH
                // the filename (case-insensitive). This is the
                // "filename is already a real name + embedded has junk
                // appended" case -- e.g. filename="Primeape" but
                // embedded="PrimeapeCThrow" (creator's internal
                // naming convention). Keeping the filename gives the
                // user the clean "Primeape" they expect. The
                // intended override case ("c1" filename →
                // 瑞希君 embedded) still fires because the embedded
                // name doesn't start with the filename. v0.2.40.
                auto starts_with_ci = [](const std::string& s,
                                         const std::string& prefix) {
                    if (s.size() < prefix.size()) return false;
                    for (size_t k = 0; k < prefix.size(); ++k) {
                        const unsigned char a = (unsigned char)s[k];
                        const unsigned char b = (unsigned char)prefix[k];
                        if (std::tolower(a) != std::tolower(b)) return false;
                    }
                    return true;
                };
                if (!starts_with_ci(display, fname)) {
                    out.player_names[i] = std::move(display);
                }
            }
        }
    }

    out.valid = true;

    int player_n = 0, stage_n = 0, demo_n = 0;
    for (const auto& s : out.player_names) if (!s.empty()) ++player_n;
    for (const auto& s : out.stage_names)  if (!s.empty()) ++stage_n;
    for (const auto& s : out.demo_names)   if (!s.empty()) ++demo_n;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "ParseKgtSummary '%s' [%s]: project='%s' players=%d/50 stages=%d/50 demos=%d/100",
        path_utf8.c_str(), is_fm95 ? "FM95" : "FM2K",
        out.project_name.c_str(), player_n, stage_n, demo_n);

    return cleanup(true);
}

// Format a char-id for UI display. "<name> (#id)" if we have a name in
// the parsed KGT, else "Char #id" so the caller never has to branch on
// kgt-availability. -1 / out-of-range ids stringify the raw id verbatim.
std::string FormatCharLabel(const KgtSummary* kgt, int id) {
    if (kgt && kgt->valid) {
        const std::string& n = kgt->PlayerName(id);
        if (!n.empty()) return n + " (#" + std::to_string(id) + ")";
    }
    return "Char #" + std::to_string(id);
}

std::string FormatStageLabel(const KgtSummary* kgt, int id) {
    if (kgt && kgt->valid) {
        const std::string& n = kgt->StageName(id);
        if (!n.empty()) return n + " (#" + std::to_string(id) + ")";
    }
    return "Stage #" + std::to_string(id);
}

} // namespace fm2k
