// Minimal inline ZIP writer (STORED method). Ported from
// /mnt/c/dev/bbbr/revolve_input_sdl3/src/rollback/zip_writer.cpp.
//
// See header for rationale. The format is the bare-minimum subset of
// PKZIP needed to produce a valid archive that unzip / 7z / Windows
// Explorer all accept:
//   - one Local File Header + data per file
//   - one Central Directory Header per file
//   - one End-of-Central-Directory record
//   - method = 0 (STORED, no compression)
//   - CRC32 = zlib/PKZIP polynomial 0xEDB88320

#include "zip_writer.h"

#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace fm2k::util::zip {

namespace {

// ----- CRC32 (zlib/PKZIP polynomial 0xEDB88320) ----------------------------

uint32_t g_crc_table[256];
bool     g_crc_init = false;

void InitCrcTable() {
    if (g_crc_init) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
    g_crc_init = true;
}

uint32_t Crc32(const uint8_t* data, size_t len) {
    InitCrcTable();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = g_crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// ----- DOS date / time encoding --------------------------------------------

void PackDosTime(uint16_t& dos_time, uint16_t& dos_date) {
    time_t now = time(nullptr);
    struct tm lt = *localtime(&now);
    dos_time = static_cast<uint16_t>(
        ((lt.tm_hour & 0x1F) << 11) |
        ((lt.tm_min  & 0x3F) <<  5) |
        ((lt.tm_sec / 2)     & 0x1F));
    dos_date = static_cast<uint16_t>(
        (((lt.tm_year - 80) & 0x7F) << 9) |
        (((lt.tm_mon + 1)    & 0x0F) << 5) |
        ( (lt.tm_mday        & 0x1F)));
}

// ----- File slurp ----------------------------------------------------------
//
// FILE_SHARE_READ + FILE_SHARE_WRITE so any logger holding the source
// file open can keep writing while we snapshot.

std::vector<uint8_t> SlurpFile(const std::string& path) {
    HANDLE h = CreateFileA(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 256 * 1024 * 1024) {
        // Sanity cap at 256 MB -- desync bundles never approach this.
        CloseHandle(h);
        return {};
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size.QuadPart));
    DWORD read_now = 0;
    size_t total = 0;
    while (total < buf.size()) {
        DWORD want = static_cast<DWORD>(
            (buf.size() - total) > 0x10000 ? 0x10000 : (buf.size() - total));
        if (!ReadFile(h, buf.data() + total, want, &read_now, nullptr) ||
            read_now == 0) {
            break;
        }
        total += read_now;
    }
    CloseHandle(h);
    if (total != buf.size()) buf.resize(total);
    return buf;
}

// Strip the directory part for the stored filename -- keep just the
// basename, normalize backslashes to forward.
std::string EntryName(const std::string& path) {
    size_t last_slash = path.find_last_of("/\\");
    std::string out = (last_slash == std::string::npos)
        ? path : path.substr(last_slash + 1);
    for (char& c : out) if (c == '\\') c = '/';
    return out;
}

}  // namespace

int WriteZip(const std::string& output_path,
             const std::vector<std::string>& files) {
    FILE* zip = std::fopen(output_path.c_str(), "wb");
    if (!zip) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "zip_writer: cannot create %s", output_path.c_str());
        return 0;
    }

    uint16_t dos_time = 0, dos_date = 0;
    PackDosTime(dos_time, dos_date);

    struct CdEntry {
        std::string name;
        uint32_t    crc;
        uint32_t    size;
        uint32_t    header_offset;
    };
    std::vector<CdEntry> entries;
    entries.reserve(files.size());

    auto write_bytes = [&](const void* p, size_t n) {
        std::fwrite(p, 1, n, zip);
    };
    auto write_u16 = [&](uint16_t v) { write_bytes(&v, 2); };
    auto write_u32 = [&](uint32_t v) { write_bytes(&v, 4); };

    // ----- Per-file local headers + data ----------------------------------
    for (const auto& path : files) {
        std::vector<uint8_t> data = SlurpFile(path);
        if (data.empty()) continue;  // skip missing / unreadable

        std::string name = EntryName(path);
        uint32_t crc  = Crc32(data.data(), data.size());
        uint32_t size = static_cast<uint32_t>(data.size());
        long header_off = std::ftell(zip);

        // Local file header (signature 0x04034B50, 30 bytes + name).
        write_u32(0x04034B50u);                  // signature
        write_u16(20);                            // version-needed (2.0 for STORE)
        write_u16(0);                             // flags
        write_u16(0);                             // method = 0 (STORED)
        write_u16(dos_time);
        write_u16(dos_date);
        write_u32(crc);
        write_u32(size);                          // compressed size = uncompressed
        write_u32(size);
        write_u16(static_cast<uint16_t>(name.size()));
        write_u16(0);                             // extra-field length
        write_bytes(name.data(), name.size());
        write_bytes(data.data(), data.size());

        entries.push_back({std::move(name), crc, size,
                           static_cast<uint32_t>(header_off)});
    }

    if (entries.empty()) {
        std::fclose(zip);
        DeleteFileA(output_path.c_str());
        return 0;
    }

    // ----- Central directory ----------------------------------------------
    long cd_start = std::ftell(zip);
    for (const auto& e : entries) {
        write_u32(0x02014B50u);                   // central dir signature
        write_u16(20);                             // version-made-by
        write_u16(20);                             // version-needed
        write_u16(0);                              // flags
        write_u16(0);                              // method
        write_u16(dos_time);
        write_u16(dos_date);
        write_u32(e.crc);
        write_u32(e.size);
        write_u32(e.size);
        write_u16(static_cast<uint16_t>(e.name.size()));
        write_u16(0);                              // extra
        write_u16(0);                              // comment
        write_u16(0);                              // disk-number-start
        write_u16(0);                              // internal-attrs
        write_u32(0);                              // external-attrs
        write_u32(e.header_offset);
        write_bytes(e.name.data(), e.name.size());
    }
    long cd_end = std::ftell(zip);

    // ----- End-of-central-directory record --------------------------------
    write_u32(0x06054B50u);                       // signature
    write_u16(0);                                  // disk #
    write_u16(0);                                  // disk with CD
    write_u16(static_cast<uint16_t>(entries.size()));   // entries on this disk
    write_u16(static_cast<uint16_t>(entries.size()));   // total entries
    write_u32(static_cast<uint32_t>(cd_end - cd_start));
    write_u32(static_cast<uint32_t>(cd_start));
    write_u16(0);                                  // comment length

    std::fclose(zip);
    return static_cast<int>(entries.size());
}

}  // namespace fm2k::util::zip
