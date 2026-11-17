// SPDX-License-Identifier: MIT
// kiln.assets: a game texture's pixels as RGBA8 (texture_rgba in kiln_assets.h). Pure: no game, no Windows; the host test
// (tests/test_texture_rgba.cpp) runs it on Linux. The decoders are bc7enc_rdo's (bc7decomp for BC7, rgbcx for BC1/BC3/BC4/
// BC5; native/third_party/bc7enc, MIT/Unlicense), already linked into kiln-assets.dll for the retexture encoders.
//
// The game's texture data (docs/re/resources.md section 4, "Item icons as pictures"): a texture resource names one content
// blob that holds the whole mip chain, the largest level first, tightly packed; level l is max(1, w >> l) x max(1, h >> l);
// a block format's level is ceil(w / 4) * ceil(h / 4) blocks. Checked on every one of the 4,268 item icons of client 26430625
// (BC7_unorm, 256x256 with 9 levels mostly, also 512x512, 200x200 and a few that are not square).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "bc7enc/bc7decomp.h"
#include "bc7enc/rgbcx.h"

namespace ka::tex {

constexpr uint32_t fnv1a(const char* s) {
    uint32_t h = 0x811c9dc5u;
    for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 0x01000193u;
    return h;
}
constexpr uint32_t kUiTexture = fnv1a("keen::UiTextureResource");          // 0xf9e86997
constexpr uint32_t kRenderTexture = fnv1a("keen::RenderTextureResource");
static_assert(kUiTexture == 0xf9e86997u);

// keen::PixelFormat values this decodes (the game's reflection; docs/re/resources.md section 4)
enum : uint32_t {
    R8_unorm = 9, R8_srgb = 15,
    R8G8B8A8_unorm = 37, R8G8B8A8_srgb = 43, B8G8R8A8_unorm = 44, B8G8R8A8_srgb = 50,
    BC1_RGB_unorm = 131, BC1_RGB_srgb = 132, BC1_RGBA_unorm = 133, BC1_RGBA_srgb = 134,
    BC3_unorm = 137, BC3_srgb = 138, BC4_unorm = 139, BC5_unorm = 141, BC7_unorm = 145, BC7_srgb = 146,
};

// The largest texture texture_rgba decodes (a side): bigger ones are refused, not cut.
constexpr uint32_t kMaxSide = 4096;

struct Desc {
    uint32_t w = 0, h = 0, format = 0, levels = 1;
    uint8_t hash[16] = {};  // keen::ContentHash of the pixel blob {size, h0, h1, h2}
    uint32_t blob_size() const {
        uint32_t s;
        std::memcpy(&s, hash, 4);
        return s;
    }
};

inline bool supported(uint32_t f) {
    switch (f) {
        case R8_unorm: case R8_srgb: case R8G8B8A8_unorm: case R8G8B8A8_srgb: case B8G8R8A8_unorm: case B8G8R8A8_srgb:
        case BC1_RGB_unorm: case BC1_RGB_srgb: case BC1_RGBA_unorm: case BC1_RGBA_srgb: case BC3_unorm: case BC3_srgb:
        case BC4_unorm: case BC5_unorm: case BC7_unorm: case BC7_srgb:
            return true;
        default:
            return false;
    }
}

// Bytes of one 4x4 block (0: not a block format), or of one pixel.
inline uint32_t block_bytes(uint32_t f) {
    switch (f) {
        case BC1_RGB_unorm: case BC1_RGB_srgb: case BC1_RGBA_unorm: case BC1_RGBA_srgb: case BC4_unorm: return 8;
        case BC3_unorm: case BC3_srgb: case BC5_unorm: case BC7_unorm: case BC7_srgb: return 16;
        default: return 0;
    }
}
inline uint32_t pixel_bytes(uint32_t f) { return f == R8_unorm || f == R8_srgb ? 1 : 4; }

inline size_t level_bytes(uint32_t f, uint32_t w, uint32_t h) {
    w = std::max(1u, w), h = std::max(1u, h);
    const uint32_t b = block_bytes(f);
    return b ? static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * b : static_cast<size_t>(w) * h * pixel_bytes(f);
}

// A texture resource as kiln.assets reads it: keen::UiTextureResource (56 bytes and its strings: data@0 {hash 16},
// size uint2@0x28, format u8@0x31, levelCount u8@0x32) or keen::RenderTextureResource (72 bytes: width u16@0, height u16@2,
// levelCount u8@8, format u8@0xa, content@0x10). The same offsets texture_override reads.
inline bool describe(uint32_t type, const uint8_t* res, size_t n, Desc* out) {
    if (!res || !out) return false;
    Desc d;
    if (type == kUiTexture && n >= 56) {
        std::memcpy(d.hash, res, 16);
        std::memcpy(&d.w, res + 0x28, 4);
        std::memcpy(&d.h, res + 0x2c, 4);
        d.format = res[0x31];
        d.levels = res[0x32] ? res[0x32] : 1;
    } else if (type == kRenderTexture && n >= 72) {
        uint16_t w16, h16;
        std::memcpy(&w16, res, 2);
        std::memcpy(&h16, res + 2, 2);
        d.w = w16, d.h = h16;
        d.levels = res[8] ? res[8] : 1;
        d.format = res[0xa];
        std::memcpy(d.hash, res + 0x10, 16);
    } else {
        return false;
    }
    if (!d.w || !d.h || d.w > kMaxSide || d.h > kMaxSide) return false;
    // never more levels than the chain can have
    uint32_t most = 1;
    for (uint32_t s = std::max(d.w, d.h); s > 1; s >>= 1) ++most;
    d.levels = std::min(d.levels, most);
    *out = d;
    return true;
}

// The level to decode: the smallest whose width and height are both at least min_side; level 0 when min_side is 0 or the
// texture is smaller than that.
inline uint32_t pick_level(const Desc& d, uint32_t min_side) {
    uint32_t l = 0;
    while (min_side && l + 1 < d.levels && std::max(1u, d.w >> (l + 1)) >= min_side && std::max(1u, d.h >> (l + 1)) >= min_side) ++l;
    return l;
}

// Where `level` starts in the blob, and its size in pixels.
inline size_t level_offset(const Desc& d, uint32_t level, uint32_t* lw, uint32_t* lh) {
    size_t off = 0;
    for (uint32_t l = 0; l < level; ++l) off += level_bytes(d.format, d.w >> l, d.h >> l);
    if (lw) *lw = std::max(1u, d.w >> level);
    if (lh) *lh = std::max(1u, d.h >> level);
    return off;
}

// Decodes one level (`data` starts at it, `n` bytes) into `rgba` (w*h*4 bytes, rows top down, R G B A, straight alpha; an
// sRGB format keeps its bytes). False when `data` is short or the format is not supported. A BC7 block in the reserved mode
// decodes as transparent black, as a GPU reads it.
inline bool decode(uint32_t f, uint32_t w, uint32_t h, const uint8_t* data, size_t n, uint8_t* rgba) {
    if (!supported(f) || !w || !h || !data || !rgba || n < level_bytes(f, w, h)) return false;
    const size_t px = static_cast<size_t>(w) * h;
    switch (f) {
        case R8G8B8A8_unorm: case R8G8B8A8_srgb:
            std::memcpy(rgba, data, px * 4);
            return true;
        case B8G8R8A8_unorm: case B8G8R8A8_srgb:
            for (size_t i = 0; i < px; ++i) {
                rgba[i * 4 + 0] = data[i * 4 + 2];
                rgba[i * 4 + 1] = data[i * 4 + 1];
                rgba[i * 4 + 2] = data[i * 4 + 0];
                rgba[i * 4 + 3] = data[i * 4 + 3];
            }
            return true;
        case R8_unorm: case R8_srgb:
            for (size_t i = 0; i < px; ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = data[i], rgba[i * 4 + 3] = 255;
            return true;
        default:
            break;
    }
    const uint32_t bb = block_bytes(f), bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint8_t block[16 * 4];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* src = data + (static_cast<size_t>(by) * bw + bx) * bb;
            switch (f) {
                case BC1_RGB_unorm: case BC1_RGB_srgb:
                    rgbcx::unpack_bc1(src, block, true);
                    for (int i = 0; i < 16; ++i) block[i * 4 + 3] = 255;  // no alpha in an RGB format
                    break;
                case BC1_RGBA_unorm: case BC1_RGBA_srgb:
                    rgbcx::unpack_bc1(src, block, true);
                    break;
                case BC3_unorm: case BC3_srgb:
                    rgbcx::unpack_bc3(src, block);
                    break;
                case BC4_unorm:
                    rgbcx::unpack_bc4(src, block, 4);
                    for (int i = 0; i < 16; ++i) block[i * 4 + 1] = block[i * 4 + 2] = block[i * 4], block[i * 4 + 3] = 255;
                    break;
                case BC5_unorm:
                    rgbcx::unpack_bc5(src, block, 0, 1, 4);
                    for (int i = 0; i < 16; ++i) block[i * 4 + 2] = 0, block[i * 4 + 3] = 255;
                    break;
                default:  // BC7
                    if (!bc7decomp::unpack_bc7(src, reinterpret_cast<bc7decomp::color_rgba*>(block))) std::memset(block, 0, sizeof block);
                    break;
            }
            for (uint32_t y = 0; y < 4 && by * 4 + y < h; ++y) {
                const uint32_t cols = std::min(4u, w - bx * 4);
                std::memcpy(rgba + ((static_cast<size_t>(by) * 4 + y) * w + bx * 4) * 4, block + y * 16, cols * 4);
            }
        }
    return true;
}

}  // namespace ka::tex
