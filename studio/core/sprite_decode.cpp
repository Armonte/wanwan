#include "sprite_decode.h"

namespace kgt {

bool DecompressSprite(const uint8_t* buf, size_t n, size_t dest_size,
                      std::vector<uint8_t>& out) {
    out.assign(dest_size, 0);
    size_t rp = 0;  // write cursor
    size_t ip = 0;  // read cursor

    while (ip < n) {
        uint32_t cur = buf[ip];
        const uint32_t op = cur >> 6;
        cur &= 0x3F;

        // A zero length in the opcode byte escapes to a wider count: one
        // more byte (+0x3F), or if that is also zero, three more (+0x013F).
        if (cur == 0) {
            if (ip + 1 >= n) return false;
            ++ip;
            cur = buf[ip];
            if (cur == 0) {
                if (ip + 3 >= n) return false;
                ++ip;
                cur = (uint32_t)buf[ip] | ((uint32_t)buf[ip + 1] << 8);
                ip += 2;
                cur += ((uint32_t)buf[ip] << 16) + 0x013F;
            } else {
                cur += 0x3F;
            }
        }

        switch (op) {
            case 0:
                // SKIP. Emits nothing and leaves the zero-fill standing --
                // this is the opcode that makes index 0 the background
                // color as a matter of format, not convention.
                if (cur > dest_size - rp) return false;
                rp += cur;
                break;

            case 1:  // literal run
                if (cur > 0) {
                    if (cur > dest_size - rp) return false;
                    if (ip + cur >= n) return false;
                    for (uint32_t i = 0; i < cur; ++i)
                        out[rp + i] = buf[ip + 1 + i];
                    rp += cur;
                    ip += cur;
                }
                break;

            case 2: {  // run of one repeated byte
                if (ip + 1 >= n) return false;
                ++ip;
                const uint8_t b = buf[ip];
                if (cur > dest_size - rp) return false;
                for (uint32_t i = 0; i < cur; ++i) out[rp + i] = b;
                rp += cur;
                break;
            }

            default: {  // back-reference
                if (ip + 1 >= n) return false;
                ++ip;
                uint32_t back = buf[ip];
                if (back == 0) {
                    if (ip + 1 >= n) return false;
                    ++ip;
                    back = ((uint32_t)buf[ip] + 1) << 8;
                }
                if (back > rp) return false;          // would read before the start
                if (cur > dest_size - rp) return false;
                const size_t start = rp - back;
                // Byte-at-a-time and deliberately NOT memcpy: an overlapping
                // back-reference (back < cur) is legal here and is how the
                // codec expresses a repeating pattern, so each byte must be
                // able to see the ones this same run just wrote.
                for (uint32_t i = 0; i < cur; ++i) out[rp + i] = out[start + i];
                rp += cur;
                break;
            }
        }
        ++ip;
    }
    return true;
}

bool DecodeSprite(const KgtFile& f, int sprite_index, int shared_palette,
                  int transparent_index, DecodedSprite& out) {
    out = DecodedSprite{};
    if (sprite_index < 0 || sprite_index >= (int)f.sprites.size()) {
        out.error = "sprite index out of range";
        return false;
    }
    const SpriteFrame& s = f.sprites[(size_t)sprite_index];
    if (s.width <= 0 || s.height <= 0) {
        out.error = "empty sprite (zero width or height)";
        return false;
    }
    // Guard the multiply before it happens: width/height are signed values
    // straight out of the file and a hostile pair could overflow size_t.
    const uint64_t pixels = (uint64_t)s.width * (uint64_t)s.height;
    if (pixels > (uint64_t)64 * 1024 * 1024) {
        out.error = "implausible sprite dimensions";
        return false;
    }

    const size_t pal_bytes = s.has_private_palette ? 1024u : 0u;
    const size_t want = (size_t)pixels + pal_bytes;

    std::vector<uint8_t> plain;
    if (s.size != 0) {
        if (!DecompressSprite(s.content.data(), s.content.size(), want, plain)) {
            out.error = "compressed payload is truncated or malformed";
            return false;
        }
    } else {
        if (s.content.size() < want) {
            out.error = "uncompressed payload is shorter than width*height";
            return false;
        }
        plain.assign(s.content.begin(), s.content.begin() + (ptrdiff_t)want);
    }

    // Private palette is a PREFIX; indices follow it.
    const uint8_t* pal = nullptr;
    if (s.has_private_palette) {
        pal = plain.data();
    } else {
        const int pi = (shared_palette >= 0 && shared_palette < 8) ? shared_palette : 0;
        pal = f.palettes[(size_t)pi].colors.data();
    }
    const uint8_t* idx = plain.data() + pal_bytes;

    out.width = s.width;
    out.height = s.height;
    out.rgba.resize((size_t)pixels * 4);
    for (uint64_t i = 0; i < pixels; ++i) {
        const uint8_t v = idx[i];
        const uint8_t* c = pal + (size_t)v * 4;   // BGRA in file order
        uint8_t* d = out.rgba.data() + (size_t)i * 4;
        d[0] = c[2];  // R
        d[1] = c[1];  // G
        d[2] = c[0];  // B
        // The file's alpha byte is a constant 0x01 sentinel, so it is
        // discarded outright; opacity is decided by the transparent index.
        d[3] = (transparent_index >= 0 && v == (uint8_t)transparent_index) ? 0 : 255;
    }
    out.ok = true;
    return true;
}

}  // namespace kgt
