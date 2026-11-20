// SPDX-License-Identifier: MIT
// Host test of texture_rgba.h (kiln.assets' texture_rgba): reading a texture resource, choosing the mip level, where a level
// starts in the blob, and decoding RGBA8 / BGRA8 / R8 / BC1 / BC3 / BC4 / BC5 / BC7 (blocks encoded here with bc7enc_rdo's
// own encoders, then decoded back). No game, no Wine.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "bc7enc/bc7enc.h"
#include "texture_rgba.h"

namespace {
int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

using namespace ka::tex;

// A keen::UiTextureResource as the game stores it (56 bytes): the item icon of Material_T1_Charcoal on 26430625.
std::vector<uint8_t> ui_texture(uint32_t w, uint32_t h, uint32_t format, uint32_t levels, uint32_t blob) {
    std::vector<uint8_t> r(56 + 32, 0);
    const uint32_t hash[4] = {blob, 1653094401u, 1826496261u, 1428081865u};
    std::memcpy(r.data(), hash, 16);
    std::memcpy(r.data() + 0x28, &w, 4);
    std::memcpy(r.data() + 0x2c, &h, 4);
    r[0x30] = 1;  // Texture2D
    r[0x31] = static_cast<uint8_t>(format);
    r[0x32] = static_cast<uint8_t>(levels);
    return r;
}

size_t chain_bytes(const Desc& d) {
    size_t n = 0;
    for (uint32_t l = 0; l < d.levels; ++l) n += level_bytes(d.format, d.w >> l, d.h >> l);
    return n;
}

void describes_and_picks() {
    Desc d;
    auto r = ui_texture(256, 256, BC7_unorm, 9, 87408);
    CHECK(describe(kUiTexture, r.data(), r.size(), &d));
    CHECK(d.w == 256 && d.h == 256 && d.format == BC7_unorm && d.levels == 9);
    CHECK(d.blob_size() == 87408);
    CHECK(chain_bytes(d) == 87408);  // the game's blob size for a 256x256 BC7 icon with 9 levels
    CHECK(pick_level(d, 0) == 0);
    CHECK(pick_level(d, 256) == 0);
    CHECK(pick_level(d, 300) == 0);  // smaller than asked: the largest level
    CHECK(pick_level(d, 129) == 0);
    CHECK(pick_level(d, 128) == 1);
    CHECK(pick_level(d, 100) == 1);
    CHECK(pick_level(d, 64) == 2);
    CHECK(pick_level(d, 1) == 8);
    uint32_t lw = 0, lh = 0;
    CHECK(level_offset(d, 2, &lw, &lh) == 65536 + 16384 && lw == 64 && lh == 64);
    CHECK(level_offset(d, 8, &lw, &lh) == 87408 - 16 && lw == 1 && lh == 1);
    // not square, not a power of two (8kLoreText_mapPiece_communityQuest_2025_01_mae_icon: 360x388, 9 levels, 187,280 bytes)
    r = ui_texture(360, 388, BC7_unorm, 9, 187280);
    CHECK(describe(kUiTexture, r.data(), r.size(), &d));
    CHECK(chain_bytes(d) == 187280);
    CHECK(pick_level(d, 64) == 2);  // 90x97
    CHECK(level_offset(d, 2, &lw, &lh) > 0 && lw == 90 && lh == 97);
    // 1299x1314 with 11 levels: 2,283,744 bytes in the game
    r = ui_texture(1299, 1314, BC7_unorm, 11, 2283744);
    CHECK(describe(kUiTexture, r.data(), r.size(), &d) && chain_bytes(d) == 2283744);
    // too many levels are cut to what the chain can have; refused: too short, wrong type, no size, too big
    r = ui_texture(4, 4, BC7_unorm, 12, 16);
    CHECK(describe(kUiTexture, r.data(), r.size(), &d) && d.levels == 3);
    CHECK(!describe(kUiTexture, r.data(), 40, &d));
    CHECK(!describe(0xb5ce8765u, r.data(), r.size(), &d));  // keen::ItemInfo
    r = ui_texture(0, 4, BC7_unorm, 1, 16);
    CHECK(!describe(kUiTexture, r.data(), r.size(), &d));
    r = ui_texture(8192, 8, R8G8B8A8_unorm, 1, 8192 * 8 * 4);
    CHECK(!describe(kUiTexture, r.data(), r.size(), &d));
    // keen::RenderTextureResource: width u16 @0, height u16 @2, levelCount @8, format @0xa, content @0x10
    std::vector<uint8_t> rt(72, 0);
    const uint16_t w16 = 64, h16 = 32;
    std::memcpy(rt.data(), &w16, 2);
    std::memcpy(rt.data() + 2, &h16, 2);
    rt[8] = 7;
    rt[0xa] = BC1_RGB_srgb;
    const uint32_t size = 1368;
    std::memcpy(rt.data() + 0x10, &size, 4);
    CHECK(describe(kRenderTexture, rt.data(), rt.size(), &d) && d.w == 64 && d.h == 32 && d.levels == 7 && d.format == BC1_RGB_srgb);
    CHECK(d.blob_size() == 1368);
    CHECK(!describe(kRenderTexture, rt.data(), 71, &d));
    // formats
    CHECK(supported(BC7_srgb) && supported(BC1_RGBA_unorm) && supported(B8G8R8A8_srgb) && supported(R8_unorm));
    CHECK(!supported(135) && !supported(143) && !supported(140) && !supported(142) && !supported(0));
    CHECK(level_bytes(BC1_RGB_unorm, 5, 3) == 2 * 1 * 8 && level_bytes(R8G8B8A8_unorm, 5, 3) == 60 && level_bytes(R8_unorm, 5, 3) == 15);
    CHECK(level_bytes(BC7_unorm, 0, 0) == 16);
}

// a picture of w x h whose every 4x4 block has a colour (and alpha) of its own: what any BCn format keeps almost exactly, so a
// decoded pixel far from it means a decoder or placement fault, not the compression's loss
std::vector<uint8_t> picture(uint32_t w, uint32_t h, bool alpha) {
    std::vector<uint8_t> p(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t bx = x / 4, by = y / 4;
            uint8_t* q = &p[(static_cast<size_t>(y) * w + x) * 4];
            q[0] = static_cast<uint8_t>(40 + 70 * bx);
            q[1] = static_cast<uint8_t>(200 - 60 * by);
            q[2] = static_cast<uint8_t>(90 + 50 * ((bx + by) % 3));
            q[3] = alpha ? static_cast<uint8_t>(255 - 50 * (bx + by)) : 255;
        }
    return p;
}

// Encodes `img` (w x h) block by block with fn(dst, 4x4 RGBA block).
template <class F>
std::vector<uint8_t> blocks(const std::vector<uint8_t>& img, uint32_t w, uint32_t h, uint32_t bb, F fn) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint8_t> out(static_cast<size_t>(bw) * bh * bb);
    uint8_t blk[64];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t sx = std::min(w - 1, bx * 4 + x), sy = std::min(h - 1, by * 4 + y);
                    std::memcpy(&blk[(y * 4 + x) * 4], &img[(static_cast<size_t>(sy) * w + sx) * 4], 4);
                }
            fn(&out[(static_cast<size_t>(by) * bw + bx) * bb], blk);
        }
    return out;
}

int max_error(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int channels) {
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (static_cast<int>(i % 4) < channels) worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
    return worst;
}

void decodes() {
    rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
    bc7enc_compress_block_init();
    // RGBA8 as it is, BGRA8 swapped, R8 grey
    {
        const auto img = picture(5, 3, true);
        std::vector<uint8_t> out(img.size());
        CHECK(decode(R8G8B8A8_srgb, 5, 3, img.data(), img.size(), out.data()) && out == img);
        std::vector<uint8_t> bgra(img);
        for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
        CHECK(decode(B8G8R8A8_unorm, 5, 3, bgra.data(), bgra.size(), out.data()) && out == img);
        const uint8_t grey[3] = {7, 128, 250};
        std::vector<uint8_t> g(12);
        CHECK(decode(R8_unorm, 3, 1, grey, 3, g.data()));
        CHECK(g[0] == 7 && g[1] == 7 && g[2] == 7 && g[3] == 255 && g[8] == 250 && g[11] == 255);
        CHECK(!decode(R8G8B8A8_unorm, 5, 3, img.data(), img.size() - 1, out.data()));  // short
        CHECK(!decode(143, 4, 4, img.data(), img.size(), out.data()));                 // BC6H
    }
    // BC7: 12x8 (whole blocks) and 6x5 (the last blocks cut), alpha kept
    for (auto [w, h] : {std::pair{12u, 8u}, std::pair{6u, 5u}}) {
        const auto img = picture(w, h, true);
        bc7enc_compress_block_params p;
        bc7enc_compress_block_params_init(&p);
        const auto enc = blocks(img, w, h, 16, [&](uint8_t* dst, const uint8_t* blk) { bc7enc_compress_block(dst, blk, &p); });
        std::vector<uint8_t> out(img.size());
        CHECK(decode(BC7_unorm, w, h, enc.data(), enc.size(), out.data()));
        CHECK(max_error(out, img, 4) <= 4);
        // each block lands where it belongs: the same as bc7decomp block by block
        bc7decomp::color_rgba c[16];
        const uint32_t bw = (w + 3) / 4;
        for (uint32_t by = 0; by < (h + 3) / 4; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                CHECK(bc7decomp::unpack_bc7(&enc[(by * bw + bx) * 16], c));
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x)
                        if (bx * 4 + x < w && by * 4 + y < h)
                            CHECK(std::memcmp(&out[((by * 4 + y) * w + bx * 4 + x) * 4], &c[y * 4 + x], 4) == 0);
            }
        CHECK(!decode(BC7_unorm, w, h, enc.data(), enc.size() - 1, out.data()));
    }
    // BC7's reserved mode (a first byte of 0): transparent black
    {
        const uint8_t zero[16] = {};
        std::vector<uint8_t> out(64, 0xAB);
        CHECK(decode(BC7_srgb, 4, 4, zero, 16, out.data()));
        CHECK(max_error(out, std::vector<uint8_t>(64, 0), 4) == 0);
    }
    // BC1: RGB (alpha 255 whatever the block says) and RGBA; BC3 with alpha
    {
        const auto img = picture(8, 8, false);
        const auto enc = blocks(img, 8, 8, 8, [](uint8_t* dst, const uint8_t* blk) { rgbcx::encode_bc1(10, dst, blk, false, false); });
        std::vector<uint8_t> out(img.size());
        CHECK(decode(BC1_RGB_srgb, 8, 8, enc.data(), enc.size(), out.data()));
        CHECK(max_error(out, img, 3) <= 8);
        bool opaque = true;
        for (size_t i = 3; i < out.size(); i += 4) opaque = opaque && out[i] == 255;
        CHECK(opaque);
        const auto alpha = picture(8, 8, true);
        const auto e3 = blocks(alpha, 8, 8, 16, [](uint8_t* dst, const uint8_t* blk) { rgbcx::encode_bc3(10, dst, blk); });
        CHECK(decode(BC3_unorm, 8, 8, e3.data(), e3.size(), out.data()));
        CHECK(max_error(out, alpha, 4) <= 8);
    }
    // BC4 (grey, opaque) and BC5 (red and green)
    {
        const auto img = picture(8, 4, false);
        const auto e4 = blocks(img, 8, 4, 8, [](uint8_t* dst, const uint8_t* blk) { rgbcx::encode_bc4(dst, blk, 4); });
        std::vector<uint8_t> out(img.size());
        CHECK(decode(BC4_unorm, 8, 4, e4.data(), e4.size(), out.data()));
        bool grey = true;
        for (size_t i = 0; i < out.size(); i += 4) {
            grey = grey && out[i] == out[i + 1] && out[i] == out[i + 2] && out[i + 3] == 255;
            grey = grey && std::abs(int(out[i]) - int(img[i])) <= 2;
        }
        CHECK(grey);
        const auto e5 = blocks(img, 8, 4, 16, [](uint8_t* dst, const uint8_t* blk) { rgbcx::encode_bc5(dst, blk, 0, 1, 4); });
        CHECK(decode(BC5_unorm, 8, 4, e5.data(), e5.size(), out.data()));
        CHECK(max_error(out, img, 2) <= 2);
        bool rest = true;
        for (size_t i = 0; i < out.size(); i += 4) rest = rest && out[i + 2] == 0 && out[i + 3] == 255;
        CHECK(rest);
    }
    // a level of a chain: decode level 1 of a 8x8 BC7 texture (2 levels) from the blob at level_offset
    {
        const auto big = picture(8, 8, true), small = picture(4, 4, true);
        bc7enc_compress_block_params p;
        bc7enc_compress_block_params_init(&p);
        auto blob = blocks(big, 8, 8, 16, [&](uint8_t* dst, const uint8_t* blk) { bc7enc_compress_block(dst, blk, &p); });
        const auto l1 = blocks(small, 4, 4, 16, [&](uint8_t* dst, const uint8_t* blk) { bc7enc_compress_block(dst, blk, &p); });
        blob.insert(blob.end(), l1.begin(), l1.end());
        auto r = ui_texture(8, 8, BC7_unorm, 2, static_cast<uint32_t>(blob.size()));
        Desc d;
        CHECK(describe(kUiTexture, r.data(), r.size(), &d));
        uint32_t lw = 0, lh = 0;
        const uint32_t level = pick_level(d, 4);
        const size_t off = level_offset(d, level, &lw, &lh);
        CHECK(level == 1 && off == 64 && lw == 4 && lh == 4);
        std::vector<uint8_t> out(4 * 4 * 4);
        CHECK(decode(d.format, lw, lh, blob.data() + off, blob.size() - off, out.data()));
        CHECK(max_error(out, small, 4) <= 4);
    }
}
}  // namespace

// A level no mip fits: a mod's PNG icon (one level, 512x512) asked at 64 is scaled down to 64x64; a level within twice
// min_side stays; shapes are kept; the box filter keeps solid colours and weights colour by alpha.
void fits_and_downscales() {
    uint32_t w = 0, h = 0;
    fit_size(512, 512, 64, &w, &h);
    CHECK(w == 64 && h == 64);
    fit_size(128, 128, 64, &w, &h);
    CHECK(w == 128 && h == 128);  // within twice: a mip, kept (the draw scales it)
    fit_size(129, 100, 64, &w, &h);
    CHECK(w == 64 && h == 50);
    fit_size(360, 388, 32, &w, &h);
    CHECK(w == 30 && h == 32);
    fit_size(512, 512, 0, &w, &h);
    CHECK(w == 512 && h == 512);
    fit_size(4096, 1, 64, &w, &h);
    CHECK(w == 64 && h == 1);
    // 8x8 of four solid quadrants down to 2x2: each output pixel is its quadrant
    std::vector<uint8_t> src(8 * 8 * 4), dst(2 * 2 * 4);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            uint8_t* p = &src[(y * 8 + x) * 4];
            p[0] = x < 4 ? 200 : 10, p[1] = y < 4 ? 20 : 220, p[2] = 99, p[3] = 255;
        }
    downscale_box(src.data(), 8, 8, dst.data(), 2, 2);
    CHECK(dst[0] == 200 && dst[1] == 20 && dst[4] == 10 && dst[8 + 1] == 220 && dst[15] == 255);
    // a transparent half does not darken the colour: half red opaque, half black transparent -> red at half alpha
    std::vector<uint8_t> half(2 * 1 * 4, 0), one(4);
    half[0] = 255, half[3] = 255;  // pixel 0: red, opaque; pixel 1: black, clear
    downscale_box(half.data(), 2, 1, one.data(), 1, 1);
    CHECK(one[0] == 255 && one[1] == 0 && one[3] == 128);
}

int main() {
    describes_and_picks();
    decodes();
    fits_and_downscales();
    if (g_fail) {
        std::fprintf(stderr, "test_texture_rgba: %d failed\n", g_fail);
        return 1;
    }
    std::printf("PASS assets.texture-rgba (resource layouts, mip choice, RGBA8/BGRA8/R8/BC1/BC3/BC4/BC5/BC7 decoding, downscaling)\n");
    return 0;
}
