// kgt_file.h -- kgtcore: byte-exact parser/serializer for FM2K
// .player/.stage/.demo/.kgt files (the "2dfm Studio" core, no UI deps).
//
// MODEL (v1, sound-editor scope): parse the common-resource PREFIX typed,
// keep everything after the sound table as an opaque tail. This matches
// tools/kgt/fpk.py's Container and is sufficient for sounds + xref +
// sprite thumbnails while making byte-exact round-trip trivial.
//
// On-disk layout (little-endian; oracle: tools/kgt/{kgt,fm2nd}.py):
//   16B  signature        ("2DKGT2K\0" / "2DKGT2G\0"; FM95 sigs
//                          "KGTGAME\0" / "2DKGT95\0" must be REFUSED)
//   256B project_name     (CP932, NUL-padded, preserve trailing garbage)
//   i32 script_n,      then script_n * 39B Script records
//   i32 script_item_n, then script_item_n * 16B ScriptItem records
//   i32 sprite_n,      then sprite_n * (20B header + content) SpriteFrame
//        content size: header.size != 0 -> that many bytes;
//        size == 0 -> w*h bytes (+1024 if has_private_palette) when w*h>0,
//        else 0 bytes  (see fpk_reader.cpp sprite walk -- proven in prod)
//   8 * 1056B Palette   (1024B of 256*BGRA + 32B file-only gap)
//   i32 sound_n,       then sound_n * (42B header + size bytes) Sound
//   tail: every remaining byte to EOF, verbatim
//
// Record layouts:
//   Script:     char name[32]; i16 script_index; u8 unknown_flag1; i32 special_flag
//   ScriptItem: u8 script_type; u8 payload[15]
//   SpriteFrame hdr: i32 unknown_flag1, width, height, has_private_palette, size
//   Sound hdr:  i32 unknown1(0 on disk); char name[32]; i32 size;
//               u8 sound_type; u8 sound_track
//     sound_type low nibble: 0=stop-all 1=WAV 2=MIDI 3=CD; bit 0x10 = loop
//
// CONTRACT: Serialize(Parse(bytes)) == bytes for every FM2K-family file.
// The corpus gate (studio/tests/) enforces this over every local game
// file (6676 known-good incl. all 105 .kgt as of 2026-07-15). Parse
// REFUSES rather than guesses: any structural surprise must fail loudly.
// Sound.size is redundant with data.size(); Serialize() must derive the
// field from data.size() -- a mismatch is unrepresentable by design.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace kgt {

enum class FileType { Kgt, Player, Stage, Demo };

struct Script {
    std::array<uint8_t, 32> name;   // CP932, preserve bytes verbatim
    int16_t script_index = 0;       // start index into items[]
    uint8_t unknown_flag1 = 0;
    int32_t special_flag = 0;
};

struct ScriptItem {
    uint8_t script_type = 0;        // opcode (0..37 with gaps)
    std::array<uint8_t, 15> payload{};
};

struct SpriteFrame {
    int32_t unknown_flag1 = 0;
    int32_t width = 0, height = 0;
    int32_t has_private_palette = 0;
    int32_t size = 0;               // preserve verbatim (0 has meaning!)
    std::vector<uint8_t> content;   // raw 8bpp indices (+1024B palette if private)
};

struct Palette {
    std::array<uint8_t, 1024> colors;  // 256 * BGRA (A byte constant 0x01)
    std::array<uint8_t, 32> gap;       // file-only padding, preserve
};

struct Sound {
    int32_t unknown1 = 0;           // runtime alloc ptr slot, 0 on disk
    std::array<uint8_t, 32> name;   // CP932
    uint8_t sound_type = 0;
    uint8_t sound_track = 0;
    std::vector<uint8_t> data;      // size field derived from this on write
};

struct KgtFile {
    FileType type = FileType::Player;
    std::array<uint8_t, 16> signature;
    std::array<uint8_t, 256> project_name;
    std::vector<Script> scripts;
    std::vector<ScriptItem> items;
    std::vector<SpriteFrame> sprites;
    std::array<Palette, 8> palettes;
    std::vector<Sound> sounds;
    std::vector<uint8_t> tail;      // everything after the sound table
};

// Decoded-name helpers (CP932 -> UTF-8, best-effort like _try_decode).
std::string DecodeName(const uint8_t* bytes, size_t len);
inline std::string ScriptName(const Script& s) { return DecodeName(s.name.data(), 32); }
inline std::string SoundName(const Sound& s) { return DecodeName(s.name.data(), 32); }

// FileType from a path's extension (.kgt/.player/.stage/.demo);
// returns false for anything else.
bool FileTypeFromPath(const std::string& path, FileType* out);

// True if the buffer starts with an FM95-family signature
// ("KGTGAME" / "2DKGT95") -- caller should refuse those gracefully.
bool IsFm95(const uint8_t* data, size_t len);

// Parse. On failure returns false and sets *err (never partially succeeds).
bool Parse(const uint8_t* data, size_t len, FileType type,
           KgtFile* out, std::string* err);

// Serialize back to the exact on-disk byte stream.
std::vector<uint8_t> Serialize(const KgtFile& f);

}  // namespace kgt
