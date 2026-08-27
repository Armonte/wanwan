// sprite_decode.h -- sprite payload -> RGBA8888, for the usage panel's
// "what's on screen" thumbnail.
//
// Kept out of kgt_file.h on purpose. kgt_file's whole contract is
// Serialize(Parse(x)) == x, so it preserves sprite payloads VERBATIM and
// never interprets them. Decoding is lossy in the direction that matters
// (compressed -> pixels is not reversed on save), so it lives here where it
// cannot be confused for part of the round-trip guarantee.
//
// TWO THINGS THE FIELD NAMES DO NOT TELL YOU, both verified against
// tools/dump_player_pics.py and the engine port it was derived from:
//
//   1. SpriteFrame::size != 0 means the payload is COMPRESSED, not that it
//      has a length. Uncompressed payloads carry size == 0 and are exactly
//      w*h bytes (+1024 when has_private_palette).
//   2. A private palette is a PREFIX of the decompressed payload, not a
//      suffix -- 1024 bytes of palette, THEN w*h bytes of indices. The
//      comment in kgt_file.h reads the other way round and is misleading.
//
// Palette entries are BGRA with the alpha byte pinned to 0x01 by the engine.
// That 0x01 is a sentinel, not an opacity: honoring it renders every sprite
// invisible. Alpha comes from the transparent index instead.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kgt_file.h"

namespace kgt {

// Palette index treated as transparent.
//
// THIS IS A PROPERTY OF THE COMPRESSION FORMAT, not a guess from looking at
// artwork. The RLE's opcode 0 is a pure SKIP: it advances the write pointer
// and emits nothing, leaving whatever the destination buffer was initialized
// to. A skip opcode is only meaningful if the value it leaves behind is the
// "nothing here" color, and the engine zero-fills. So index 0 is background
// by construction of the codec.
//
// Corpus statistics agree where they can: across Mikyaku Impact's 233
// sprites index 0 holds 80% of corners and 61% of edge rows. They look
// ambiguous on WonderfulWorld (index 2 leads corners at 40%) purely because
// its 85 pictures are mostly full-bleed backgrounds, which are opaque
// everywhere and so have no keyed pixels to count.
constexpr int kDefaultTransparentIndex = 0;

struct DecodedSprite {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // width*height*4, byte order R,G,B,A
    bool ok = false;
    std::string error;          // populated when ok == false
};

// The engine's LZ-ish RLE (a port of _2dfm::decompress, 2dfmCommon.cpp:17).
// dest_size is known from the sprite header, so the output length is not
// discovered from the stream.
//
// Bounds-checked in every direction, unlike the Python reference. This runs
// on whatever file a user drags in, so a truncated or hostile payload has to
// return false rather than read or write out of range.
bool DecompressSprite(const uint8_t* src, size_t n, size_t dest_size,
                      std::vector<uint8_t>& out);

// Decode one sprite to RGBA. shared_palette selects among KgtFile::palettes
// and is ignored when the sprite carries a private palette. Returns false
// (with out.error set) for an empty, malformed or truncated sprite.
bool DecodeSprite(const KgtFile& f, int sprite_index, int shared_palette,
                  int transparent_index, DecodedSprite& out);

}  // namespace kgt
